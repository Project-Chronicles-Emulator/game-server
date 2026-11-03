#pragma once

#include "common/world_metrics.hpp"
#include "protocol/protocol.hpp"
#include "world/core/progress_access.hpp"
#include "world/core/session_event_bus.hpp"
#include "world/features/mail/mail.hpp"
#include "world/features/mail/mail_repository.hpp"

#include <functional>
#include <memory>

namespace chronicles::mail {

using ClaimCompletion = std::function<void(MailClaimStatus)>;
using MailPost = std::function<void(Frame)>;
using MailClaim = std::function<void(std::int64_t, ClaimCompletion)>;

class MailCodecPort {
public:
  virtual ~MailCodecPort() = default;

  virtual world_metrics::TimedMutex &mail_protocol_mutex() const = 0;
  // Requires the protocol mutex. Encoding is deliberately separate from posting.
  virtual Frame mail_encode(const GameplayPacket &packet) = 0;
};

struct MailPeer {
  bool authenticated{};
  std::int64_t account_id{};
  std::int64_t character_id{};
  // Evaluated only after owner filters, preserving the former short-circuit lock queries.
  std::function<bool()> in_world;
  MailPost post;
  MailClaim claim;
};

class MailWorldContext : public MailCodecPort {
public:
  virtual int mail_world_id() const = 0;
  // Called under the protocol mutex in the existing registry order. False stops at
  // the selected claim owner; recipient callbacks retain the corresponding session.
  virtual void mail_each_peer(const std::function<bool(MailPeer)> &visitor) = 0;
};

struct MailOwner {
  std::int64_t account_id{};
  std::int64_t character_id{};
  int world_id{};
};

class MailSessionContext : public MailCodecPort {
public:
  virtual bool mail_closed() const = 0;
  virtual bool mail_entered() const = 0;
  virtual std::optional<MailOwner> mail_owner() const = 0;
  virtual bool mail_has_tutorial() const = 0;
  virtual std::int64_t mail_epoch_ticks() const = 0;
  virtual const nlohmann::json *mail_player_stats() const = 0;
  virtual void mail_append_encumbrance(std::vector<GameplayPacket> &packets) = 0;
  virtual void mail_post_frame(Frame frame) = 0;
  virtual void mail_disconnect_after_uncertain_commit() = 0;
};

class MailSession;

class MailFeature final {
public:
  explicit MailFeature(std::shared_ptr<const ShopCatalog> items);

  void set_repository(std::shared_ptr<MailRepository> repository);
  bool has_repository() const noexcept;
  std::optional<MailRecord> deliver_record(const MailRecord &mail);
  std::optional<MailRecord> deliver_tutorial_completion(std::int64_t account_id, int world_id,
                                                        std::int64_t character_id,
                                                        std::int64_t now_epoch_ticks);

  std::unique_ptr<MailSession> create_session(ProgressAccess &progress, MailSessionContext &context,
                                              SessionEventBus &events);

  bool claim_online(const Character &character, std::int64_t mail_id, ClaimCompletion completion,
                    MailWorldContext &context);
  std::size_t notify_online(const MailRecord &mail, MailWorldContext &context);

  // GM retains protected target resolution and its post-durable failure catch. This
  // shared adapter emits its existing character-mail variant to that explicit recipient.
  void notify_peer(const MailRecord &mail, MailCodecPort &codec, MailPost recipient);

private:
  friend class MailSession;
  const std::shared_ptr<const ShopCatalog> items_;
  std::shared_ptr<MailRepository> repository_;
};

GameplayPacket mail_delivery_notification(const MailRecord &mail);

// Called on the existing owning worker. The composition only schedules/retains it;
// this object owns receipt, cache, notification and completion ordering.
class MailSession final {
public:
  MailSession(MailFeature &feature, ProgressAccess &progress, MailSessionContext &context,
              SessionEventBus &events);

  void claim(std::int64_t mail_id, const ClaimCompletion &completion);

private:
  MailFeature &feature_;
  ProgressAccess &progress_;
  MailSessionContext &context_;
  SessionEventBus &events_;
};

} // namespace chronicles::mail
