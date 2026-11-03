#include "world/features/mail/mail_session.hpp"

#include "world/features/mail/tutorial_completion_mail.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <mutex>
#include <utility>

namespace chronicles::mail {
namespace {

constexpr std::uint16_t kMailDeliveryNotification = 174;

GameplayPacket delivery_notification(const MailRecord &mail, const char *scope) {
  return {kMailDeliveryNotification, {{scope, mail_content_json(mail)}}};
}

// The dispatch is prepared under the codec/registry lock, then posts after releasing it.
// Retained callbacks preserve the former vector<shared_ptr<WorldSession>> lifetime.
struct MailDispatch {
  Frame frame;
  std::vector<MailPost> recipients;

  std::size_t send() const {
    for (const auto &recipient : recipients) {
      recipient(frame);
    }
    return recipients.size();
  }
};

} // namespace

GameplayPacket mail_delivery_notification(const MailRecord &mail) {
  const auto key = mail.character_id == 0 ? "account_mail" : "char_mail";
  return delivery_notification(mail, key);
}

MailFeature::MailFeature(std::shared_ptr<const ShopCatalog> items) : items_(std::move(items)) {
}

void MailFeature::set_repository(std::shared_ptr<MailRepository> repository) {
  repository_ = std::move(repository);
}

bool MailFeature::has_repository() const noexcept {
  return repository_ != nullptr;
}

std::optional<MailRecord> MailFeature::deliver_record(const MailRecord &mail) {
  if (!repository_) {
    return std::nullopt;
  }
  return repository_->deliver_mail(mail);
}

std::optional<MailRecord> MailFeature::deliver_tutorial_completion(std::int64_t account_id,
                                                                   int world_id,
                                                                   std::int64_t character_id,
                                                                   std::int64_t now_epoch_ticks) {
  if (!repository_ || !items_) {
    return std::nullopt;
  }
  try {
    return deliver_tutorial_completion_mail(*repository_, *items_, account_id, world_id,
                                            now_epoch_ticks);
  } catch (const std::exception &error) {
    spdlog::warn("Tutorial completion mail was not delivered for character {}: {}", character_id,
                 error.what());
    return std::nullopt;
  }
}

std::unique_ptr<MailSession> MailFeature::create_session(ProgressAccess &progress,
                                                         MailSessionContext &context,
                                                         SessionEventBus &events) {
  return std::make_unique<MailSession>(*this, progress, context, events);
}

bool MailFeature::claim_online(const Character &character, std::int64_t mail_id,
                               ClaimCompletion completion, MailWorldContext &context) {
  if (!completion || character.world_id != context.mail_world_id()) {
    return false;
  }
  MailClaim selected;
  {
    std::lock_guard lock(context.mail_protocol_mutex());
    context.mail_each_peer([&](MailPeer peer) {
      if (peer.authenticated && peer.character_id == character.id &&
          peer.account_id == character.account_id && peer.in_world()) {
        selected = std::move(peer.claim);
        return false;
      }
      return true;
    });
  }
  if (!selected) {
    return false;
  }
  selected(mail_id, std::move(completion));
  return true;
}

std::size_t MailFeature::notify_online(const MailRecord &mail, MailWorldContext &context) {
  if (mail.world_id != context.mail_world_id()) {
    return 0;
  }
  const auto packet = mail_delivery_notification(mail);
  MailDispatch dispatch;
  {
    std::lock_guard lock(context.mail_protocol_mutex());
    dispatch.frame = context.mail_encode(packet);
    context.mail_each_peer([&](MailPeer peer) {
      if (peer.authenticated && peer.account_id == mail.account_id &&
          (mail.character_id == 0 || peer.character_id == mail.character_id) && peer.in_world()) {
        dispatch.recipients.push_back(std::move(peer.post));
      }
      return true;
    });
  }
  return dispatch.send();
}

void MailFeature::notify_peer(const MailRecord &mail, MailCodecPort &codec, MailPost recipient) {
  MailDispatch dispatch;
  {
    std::lock_guard lock(codec.mail_protocol_mutex());
    // The existing GM recipient capability always emits the character-mail variant.
    dispatch.frame = codec.mail_encode(delivery_notification(mail, "char_mail"));
  }
  dispatch.recipients.push_back(std::move(recipient));
  dispatch.send();
}

MailSession::MailSession(MailFeature &feature, ProgressAccess &progress,
                         MailSessionContext &context, SessionEventBus &events)
    : feature_(feature), progress_(progress), context_(context), events_(events) {
}

void MailSession::claim(std::int64_t mail_id, const ClaimCompletion &completion) {
  bool storage_attempted{};
  try {
    if (context_.mail_closed() || !context_.mail_entered()) {
      completion(MailClaimStatus::conflict);
      return;
    }
    const auto owner = context_.mail_owner();
    if (!owner || !feature_.repository_ || !feature_.items_) {
      completion(MailClaimStatus::conflict);
      return;
    }
    const auto now = context_.mail_epoch_ticks();
    const auto mails =
        feature_.repository_->mailbox(owner->account_id, owner->character_id, owner->world_id, now);
    const auto mail = std::ranges::find_if(mails, [mail_id](const MailRecord &candidate) {
      return candidate.id == mail_id;
    });
    if (mail == mails.end()) {
      completion(MailClaimStatus::not_found);
      return;
    }
    if (mail->claimed_epoch_ticks != 0) {
      completion(MailClaimStatus::already_claimed);
      return;
    }
    if (now >= mail->attachment_expiry_epoch_ticks) {
      completion(MailClaimStatus::expired);
      return;
    }
    const auto character = progress_.character();
    if (!character) {
      completion(MailClaimStatus::not_found);
      return;
    }
    const auto before = progress_.load();
    auto plan = plan_mail_claim(*mail, *character, before, *feature_.items_, now);
    storage_attempted = true;
    const auto result =
        feature_.repository_->claim_mail(owner->account_id, owner->character_id, owner->world_id,
                                         *mail, now, plan.before, plan.after, plan.granted_items);
    if (result != MailClaimStatus::claimed) {
      completion(result);
      return;
    }

    if (context_.mail_has_tutorial()) {
      events_.publish(PlayerProgressCommitted{plan.after});
    }
    Character live = *character;
    live.items.insert(live.items.end(), plan.granted_items.begin(), plan.granted_items.end());
    {
      std::lock_guard lock(context_.mail_protocol_mutex());
      events_.publish(PlayerInventorySnapshotChanged{live, context_.mail_player_stats()});
    }
    auto packets = mail_claim_packets(*mail, plan);
    context_.mail_append_encumbrance(packets);
    std::vector<Frame> frames;
    {
      std::lock_guard lock(context_.mail_protocol_mutex());
      for (const auto &packet : packets) {
        frames.push_back(context_.mail_encode(packet));
      }
    }
    for (auto &frame : frames) {
      context_.mail_post_frame(std::move(frame));
    }
    completion(MailClaimStatus::claimed);
  } catch (const std::exception &) {
    if (storage_attempted) {
      // An unknown COMMIT outcome, cache failure or notification failure must reload the
      // durable receipt/inventory through reconnect, exactly as the former worker did.
      context_.mail_disconnect_after_uncertain_commit();
    }
    completion(MailClaimStatus::conflict);
  }
}

} // namespace chronicles::mail
