#include "world/features/mail/mail_session.hpp"

#include "world/core/tests/economy_test_support.hpp"

#include <gtest/gtest.h>

#include <mutex>

namespace chronicles::mail {
namespace {

using namespace economy_testing;

class RecordingMail final : public MailRepository {
public:
  explicit RecordingMail(EconomyFixture &fixture)
      : delegate_(fixture.repositories.database), fixture_(fixture) {
  }

  std::optional<MailRecord> deliver_mail(MailRecord mail) override {
    if (throw_delivery) {
      throw std::runtime_error("Synthetic unknown delivery outcome");
    }
    return delegate_.deliver_mail(std::move(mail));
  }

  std::vector<MailRecord> mailbox(std::int64_t account_id, std::int64_t character_id, int world_id,
                                  std::int64_t now) override {
    fixture_.trace.push_back("mailbox");
    if (throw_mailbox) {
      throw std::runtime_error("Synthetic mailbox read failure");
    }
    return delegate_.mailbox(account_id, character_id, world_id, now);
  }

  bool change_mail(std::int64_t account_id, std::int64_t character_id, int world_id,
                   const std::vector<std::int64_t> &ids, bool remove, std::int64_t now) override {
    return delegate_.change_mail(account_id, character_id, world_id, ids, remove, now);
  }

  MailClaimStatus claim_mail(std::int64_t account_id, std::int64_t character_id, int world_id,
                             const MailRecord &mail, std::int64_t now, const WorldProgress &before,
                             const WorldProgress &after,
                             std::vector<CharacterItem> &items) override {
    fixture_.trace.push_back("claim");
    const auto status =
        delegate_.claim_mail(account_id, character_id, world_id, mail, now, before, after, items);
    if (throw_after_claim) {
      throw std::runtime_error("Synthetic unknown claim COMMIT outcome");
    }
    return status;
  }

  bool throw_delivery{};
  bool throw_mailbox{};
  bool throw_after_claim{};

private:
  MemoryMailRepository delegate_;
  EconomyFixture &fixture_;
};

class ProbeContext final : public MailSessionContext {
public:
  explicit ProbeContext(EconomyFixture &fixture)
      : owner(MailOwner{fixture.actor.account_id, fixture.actor.id, fixture.actor.world_id}),
        fixture_(fixture) {
  }

  bool mail_closed() const override {
    return closed;
  }

  bool mail_entered() const override {
    return entered;
  }

  std::optional<MailOwner> mail_owner() const override {
    return owner;
  }

  bool mail_has_tutorial() const override {
    return tutorial;
  }

  std::int64_t mail_epoch_ticks() const override {
    return kNow;
  }

  const Json *mail_player_stats() const override {
    return &fixture_.prepared_stats;
  }

  world_metrics::TimedMutex &mail_protocol_mutex() const override {
    return mutex_;
  }

  Frame mail_encode(const GameplayPacket &packet) override {
    fixture_.trace.push_back("encode:" + std::to_string(packet.opcode));
    if (throw_encoding) {
      throw std::runtime_error("Synthetic post-commit codec failure");
    }
    encoded.push_back(packet);
    return encoded_probe_frame(packet.opcode);
  }

  void mail_append_encumbrance(std::vector<GameplayPacket> &packets) override {
    fixture_.trace.push_back("encumbrance");
    packets.push_back({115, Json::object()});
  }

  void mail_post_frame(Frame frame) override {
    fixture_.trace.push_back("post:" + std::to_string(frame.opcode()));
    posted.push_back(std::move(frame));
  }

  void mail_disconnect_after_uncertain_commit() override {
    fixture_.trace.push_back("disconnect");
    disconnect_requested = true;
  }

  std::optional<MailOwner> owner;
  bool closed{};
  bool entered{true};
  bool tutorial{true};
  bool throw_encoding{};
  bool disconnect_requested{};
  std::vector<GameplayPacket> encoded;
  std::vector<Frame> posted;

private:
  EconomyFixture &fixture_;
  mutable world_metrics::TimedMutex mutex_;
};

MailRecord reward(const Character &actor, bool account_mail = false) {
  MailRecord mail;
  mail.account_id = actor.account_id;
  mail.character_id = account_mail ? 0 : actor.id;
  mail.world_id = actor.world_id;
  mail.delivery_key = account_mail ? "account-reward" : "character-reward";
  mail.title = "Probe delivery";
  mail.text = "Synthetic mail";
  mail.sent_epoch_ticks = kNow - 1000;
  mail.expiry_epoch_ticks = kNow + 1'000'000;
  mail.attachment_expiry_epoch_ticks = kNow + 500'000;
  mail.attachments = {{MailAttachmentKind::item, kEquipment, 1, {}},
                      {MailAttachmentKind::item, kMaterial, 3, {}},
                      {MailAttachmentKind::money, 0, 25, {}}};
  return mail;
}

TEST(MailSession, ClosedSessionRefusesBeforeAnyMailboxOrProgressRead) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  MailFeature feature(fixture.items);
  feature.set_repository(std::make_shared<RecordingMail>(fixture));
  auto session = feature.create_session(*fixture.progress, context, *fixture.events);
  context.closed = true;
  MailClaimStatus result{};

  session->claim(1, [&](MailClaimStatus status) {
    result = status;
  });
  EXPECT_EQ(result, MailClaimStatus::conflict);
  EXPECT_TRUE(fixture.trace.empty());
}

TEST(MailSession, DurableReceiptPrecedesCacheAndFramesAndCompletionRunsLast) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  MailFeature feature(fixture.items);
  auto repository = std::make_shared<RecordingMail>(fixture);
  feature.set_repository(repository);
  auto delivered = feature.deliver_record(reward(fixture.actor));
  ASSERT_TRUE(delivered);
  auto session = feature.create_session(*fixture.progress, context, *fixture.events);
  fixture.on_snapshot = [&](const PlayerInventorySnapshotChanged &event) {
    EXPECT_EQ(fixture.stored_progress().gold, 5025);
    ASSERT_EQ(event.character.items.size(), fixture.actor.items.size() + 1);
    const auto granted = std::ranges::find_if(event.character.items, [](const CharacterItem &item) {
      return item.cms_id == kEquipment;
    });
    ASSERT_NE(granted, event.character.items.end());
    EXPECT_GT(granted->id, 0);
    auto retained = event.character.items;
    std::erase_if(retained, [](const CharacterItem &item) {
      return item.cms_id == kEquipment;
    });
    EXPECT_EQ(retained, fixture.actor.items);
  };
  MailClaimStatus result{};
  unsigned completions{};

  session->claim(delivered->id, [&](MailClaimStatus status) {
    fixture.trace.push_back("complete");
    result = status;
    ++completions;
  });
  EXPECT_EQ(result, MailClaimStatus::claimed);
  EXPECT_EQ(completions, 1);
  EXPECT_LT(fixture.trace_index("claim"), fixture.trace_index("adopt"));
  EXPECT_LT(fixture.trace_index("adopt"), fixture.trace_index("snapshot"));
  EXPECT_LT(fixture.trace_index("snapshot"), fixture.trace_index("encumbrance"));
  EXPECT_LT(fixture.trace_index("post:115"), fixture.trace_index("complete"));
  ASSERT_EQ(context.posted.size(), 2);
  EXPECT_EQ(context.posted[0].opcode(), 169);
  EXPECT_EQ(context.posted[1].opcode(), 115);
  EXPECT_EQ(context.encoded[0].message.at("system").at("char_mail_id"),
            std::to_string(delivered->id));
  EXPECT_EQ(std::ranges::count(fixture.trace, "stats"), 0);
}

TEST(MailSession, AccountReceiptPreventsASecondOwnedCharacterFromReceivingAgain) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  MailFeature feature(fixture.items);
  feature.set_repository(std::make_shared<RecordingMail>(fixture));
  auto delivered = feature.deliver_record(reward(fixture.actor, true));
  ASSERT_TRUE(delivered);
  auto session = feature.create_session(*fixture.progress, context, *fixture.events);
  session->claim(delivered->id, [](MailClaimStatus status) {
    EXPECT_EQ(status, MailClaimStatus::claimed);
  });
  const auto sibling = create_owner(fixture.repositories, "sibling", fixture.actor.account_id, 1);
  RecordingProgress sibling_progress(fixture.repositories, sibling, fixture.trace);
  ProbeContext sibling_context(fixture);
  sibling_context.owner = MailOwner{sibling.account_id, sibling.id, sibling.world_id};
  auto other = feature.create_session(sibling_progress, sibling_context, *fixture.events);
  MailClaimStatus result{};

  other->claim(delivered->id, [&](MailClaimStatus status) {
    result = status;
  });
  EXPECT_EQ(result, MailClaimStatus::already_claimed);
  EXPECT_EQ(fixture.stored_progress().gold, 5025);
  EXPECT_EQ(sibling_progress.load().gold, 0);
  EXPECT_TRUE(sibling_context.posted.empty());
}

TEST(MailSession, UnknownClaimCommitRequestsReconnectAndReceiptStillGuardsRetry) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  MailFeature feature(fixture.items);
  auto repository = std::make_shared<RecordingMail>(fixture);
  feature.set_repository(repository);
  auto delivered = feature.deliver_record(reward(fixture.actor));
  ASSERT_TRUE(delivered);
  repository->throw_after_claim = true;
  auto session = feature.create_session(*fixture.progress, context, *fixture.events);
  MailClaimStatus result{};

  session->claim(delivered->id, [&](MailClaimStatus status) {
    result = status;
  });
  EXPECT_EQ(result, MailClaimStatus::conflict);
  EXPECT_TRUE(context.disconnect_requested);
  EXPECT_EQ(fixture.stored_progress().gold, 5025);
  EXPECT_FALSE(fixture.last_snapshot);
  EXPECT_TRUE(context.posted.empty());
  repository->throw_after_claim = false;
  session->claim(delivered->id, [&](MailClaimStatus status) {
    result = status;
  });
  EXPECT_EQ(result, MailClaimStatus::already_claimed);
  EXPECT_EQ(fixture.stored_progress().gold, 5025);
}

TEST(MailSession, ReadFailureBeforeClaimDoesNotRequestAnUncertainCommitDisconnect) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  MailFeature feature(fixture.items);
  auto repository = std::make_shared<RecordingMail>(fixture);
  repository->throw_mailbox = true;
  feature.set_repository(repository);
  auto session = feature.create_session(*fixture.progress, context, *fixture.events);

  session->claim(1, [](MailClaimStatus status) {
    EXPECT_EQ(status, MailClaimStatus::conflict);
  });
  EXPECT_FALSE(context.disconnect_requested);
  EXPECT_TRUE(context.posted.empty());
}

TEST(MailSession, CodecFailureAfterTheReceiptPreservesTheDisconnectAndConflictBoundary) {
  EconomyFixture fixture;
  ProbeContext context(fixture);
  MailFeature feature(fixture.items);
  feature.set_repository(std::make_shared<RecordingMail>(fixture));
  const auto delivered = feature.deliver_record(reward(fixture.actor));
  ASSERT_TRUE(delivered);
  auto session = feature.create_session(*fixture.progress, context, *fixture.events);
  context.throw_encoding = true;

  session->claim(delivered->id, [](MailClaimStatus status) {
    EXPECT_EQ(status, MailClaimStatus::conflict);
  });
  EXPECT_TRUE(context.disconnect_requested);
  EXPECT_EQ(fixture.stored_progress().gold, 5025);
  EXPECT_TRUE(context.posted.empty());
}

class WorldProbe final : public MailWorldContext {
public:
  int mail_world_id() const override {
    return 1;
  }

  world_metrics::TimedMutex &mail_protocol_mutex() const override {
    return mutex;
  }

  Frame mail_encode(const GameplayPacket &packet) override {
    trace.push_back("encode");
    if (throw_encoding) {
      throw std::runtime_error("Synthetic live-notification codec failure");
    }
    encoded = packet;
    return encoded_probe_frame(packet.opcode);
  }

  void mail_each_peer(const std::function<bool(MailPeer)> &visitor) override {
    for (std::size_t index = 0; index < peers.size(); ++index) {
      trace.push_back("visit:" + std::to_string(index));
      if (!visitor(peers[index])) {
        break;
      }
    }
  }

  mutable world_metrics::TimedMutex mutex;
  std::vector<MailPeer> peers;
  std::vector<std::string> trace;
  GameplayPacket encoded;
  bool throw_encoding{};
};

TEST(MailFeature, AccountFanoutFiltersOwnersBeforeWorldQueriesAndPostsAfterUnlocking) {
  EconomyFixture fixture;
  MailFeature feature(fixture.items);
  WorldProbe context;
  auto mail = reward(fixture.actor, true);
  std::vector<std::int64_t> recipients;
  const auto peer = [&](bool authenticated, std::int64_t account_id, std::int64_t character_id,
                        bool in_world) {
    return MailPeer{authenticated,
                    account_id,
                    character_id,
                    [&, character_id, in_world] {
                      context.trace.push_back("world:" + std::to_string(character_id));
                      return in_world;
                    },
                    [&, character_id](Frame frame) {
                      std::lock_guard lock(context.mutex);
                      EXPECT_EQ(frame.opcode(), 174);
                      recipients.push_back(character_id);
                    },
                    {}};
  };
  context.peers = {peer(false, mail.account_id, 90, true), peer(true, 99, 91, true),
                   peer(true, mail.account_id, 92, false), peer(true, mail.account_id, 93, true),
                   peer(true, mail.account_id, 94, true)};

  EXPECT_EQ(feature.notify_online(mail, context), 2);
  EXPECT_EQ(recipients, (std::vector<std::int64_t>{93, 94}));
  EXPECT_TRUE(context.encoded.message.contains("account_mail"));
  EXPECT_EQ(context.trace.front(), "encode");
  EXPECT_EQ(std::ranges::count(context.trace, "world:90"), 0);
  EXPECT_EQ(std::ranges::count(context.trace, "world:91"), 0);
  mail.character_id = 93;
  recipients.clear();
  EXPECT_EQ(feature.notify_online(mail, context), 1);
  EXPECT_EQ(recipients, (std::vector<std::int64_t>{93}));
  EXPECT_TRUE(context.encoded.message.contains("char_mail"));
}

TEST(MailFeature, WrongWorldDoesNotEncodeOrInspectPeers) {
  EconomyFixture fixture;
  MailFeature feature(fixture.items);
  WorldProbe context;
  auto mail = reward(fixture.actor);
  mail.world_id = 2;

  EXPECT_EQ(feature.notify_online(mail, context), 0);
  EXPECT_TRUE(context.trace.empty());
}

TEST(MailFeature, ClaimSelectsOnlyTheFirstOwnedWorldPeerAndDoesNotWaitForItsWorker) {
  EconomyFixture fixture;
  MailFeature feature(fixture.items);
  WorldProbe context;
  ClaimCompletion pending;
  unsigned selected{};
  for (unsigned index = 0; index < 2; ++index) {
    context.peers.push_back(MailPeer{true,
                                     fixture.actor.account_id,
                                     fixture.actor.id,
                                     [] {
                                       return true;
                                     },
                                     {},
                                     [&, index](std::int64_t mail_id, ClaimCompletion completion) {
                                       std::lock_guard lock(context.mutex);
                                       EXPECT_EQ(mail_id, 7);
                                       selected = index + 1;
                                       pending = std::move(completion);
                                     }});
  }
  bool completed{};

  EXPECT_TRUE(feature.claim_online(
      fixture.actor, 7,
      [&](MailClaimStatus) {
        completed = true;
      },
      context));
  EXPECT_EQ(selected, 1);
  EXPECT_FALSE(completed);
  ASSERT_TRUE(pending);
  pending(MailClaimStatus::claimed);
  EXPECT_TRUE(completed);
  EXPECT_EQ(std::ranges::count(context.trace, "visit:1"), 0);
}

TEST(MailFeature, RawDeliveryThrowsWhileTutorialServiceKeepsItsSafeFailurePolicy) {
  EconomyFixture fixture;
  MailFeature feature(fixture.items);
  auto repository = std::make_shared<RecordingMail>(fixture);
  feature.set_repository(repository);
  repository->throw_delivery = true;

  EXPECT_THROW(feature.deliver_record(reward(fixture.actor)), std::runtime_error);
  EXPECT_FALSE(feature.deliver_tutorial_completion(fixture.actor.account_id, fixture.actor.world_id,
                                                   fixture.actor.id, kNow));
}

TEST(MailFeature, ExplicitRecipientAdapterPostsOutsideTheLockAndPropagatesNotificationFailure) {
  EconomyFixture fixture;
  MailFeature feature(fixture.items);
  auto repository = std::make_shared<RecordingMail>(fixture);
  feature.set_repository(repository);
  const auto delivered = feature.deliver_record(reward(fixture.actor));
  ASSERT_TRUE(delivered);
  WorldProbe codec;
  unsigned posts{};
  const auto recipient = [&](Frame frame) {
    std::lock_guard lock(codec.mutex);
    EXPECT_EQ(frame.opcode(), 174);
    ++posts;
  };

  feature.notify_peer(*delivered, codec, recipient);
  EXPECT_EQ(posts, 1);
  EXPECT_TRUE(codec.encoded.message.contains("char_mail"));

  // The explicit GM adapter keeps its character-mail wire variant; facade fanout
  // independently chooses account/character scope from the durable record.
  auto account_mail = *delivered;
  account_mail.character_id = 0;
  feature.notify_peer(account_mail, codec, recipient);
  EXPECT_EQ(posts, 2);
  EXPECT_TRUE(codec.encoded.message.contains("char_mail"));

  codec.throw_encoding = true;
  EXPECT_THROW(feature.notify_peer(*delivered, codec, recipient), std::runtime_error);
  EXPECT_EQ(posts, 2);
  EXPECT_EQ(
      repository->mailbox(fixture.actor.account_id, fixture.actor.id, fixture.actor.world_id, kNow)
          .size(),
      1);
}

} // namespace
} // namespace chronicles::mail
