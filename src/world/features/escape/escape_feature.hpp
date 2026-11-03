#pragma once

#include "world/core/frame_context.hpp"
#include "world/features/escape/player_escape.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace chronicles {

struct Frame;

namespace escape {

struct EscapeBlockers {
  bool dead{};
  bool busy{};
  bool riding{};
};

// Explicit session capabilities. Position/cache, revival and invincibility remain owned
// by their existing systems; no WorldState, WorldSession or other feature pointer crosses.
class EscapeSessionContext {
public:
  virtual ~EscapeSessionContext() = default;

  virtual bool escape_has_character() const = 0;
  virtual bool escape_closed() const = 0;
  virtual std::uint64_t escape_connection_id() const = 0;
  virtual std::uint64_t escape_player_unit_id() const = 0;
  virtual std::int64_t escape_now_ticks() const = 0;
  virtual EscapeBlockers escape_blockers() const = 0;

  virtual boost::asio::any_io_executor escape_io_executor() = 0;
  virtual boost::asio::any_io_executor escape_worker_executor() = 0;
  // The opaque token keeps the containing player session (and this borrowed context)
  // alive through queued work and timer cancellation, exactly like the former self capture.
  virtual std::shared_ptr<void> retain_escape_owner() = 0;

  // Called only on completed teleports, before position adoption. Retains the original
  // pending-shrine commit/revive/packet insertion; false is its caught commit refusal.
  virtual bool finish_escape_revival(std::vector<GameplayPacket> &packets) = 0;
  virtual void adopt_escape_position(const WorldPosition &position) = 0;
  virtual bool escape_god_mode() const = 0;
  virtual GameplayPacket escape_invincibility_packet(bool enabled) const = 0;

  // The adapter keeps the existing codec lock around the entire loop. A partial result
  // must survive an encoding exception so the old catch/rearm/delivery order is retained.
  virtual void append_encoded_escape_packets(const std::vector<GameplayPacket> &packets,
                                             std::vector<Frame> &frames) = 0;
  virtual void enqueue_escape_frame(Frame frame) = 0;
};

class EscapeSession;

class EscapeFeature final {
public:
  EscapeFeature() = default;

  explicit EscapeFeature(EscapeRules rules);

  void load_catalog(ClientCatalog &catalog);

  std::unique_ptr<EscapeSession> create_session(EscapeSessionContext &context,
                                                boost::asio::any_io_executor timer_executor);

private:
  friend class EscapeSession;

  EscapeRules rules_;
};

// PlayerEscape remains lazy: every surviving request checks ownership at the existing broad
// pipeline position. Its constructor may therefore still reject an unrelated request.
class EscapeSession final {
public:
  EscapeSession(EscapeFeature &feature, EscapeSessionContext &context,
                boost::asio::any_io_executor timer_executor);

  bool handle(FrameContext &frame);

  bool handles(const WorldGameplayRequest &request);

  bool teleport_pending() const;

  // Shrine preflight originally constructed PlayerEscape before checking its pending bit,
  // before reading progress or selecting a shrine. Keep that separate from the passive query.
  bool server_teleport_pending();

  EscapeOutcome request_server_teleport(const WorldPosition &destination, std::int64_t now);

  // Original I/O close location. Map changes intentionally do not reset pending teleport
  // or cooldown: the former WorldSession kept them across those transitions too.
  void on_leave_world();

private:
  PlayerEscape &player();

  std::vector<GameplayPacket> receive(const WorldGameplayRequest &request,
                                      const std::optional<WorldPosition> &position);

  void arm_expiry(std::optional<std::int64_t> deadline);

  void resolve_expiry(std::shared_ptr<void> owner);

  EscapeFeature &feature_;
  EscapeSessionContext &context_;
  boost::asio::steady_timer timer_;
  std::optional<std::int64_t> expiry_tick_;
  std::optional<PlayerEscape> player_;
};

} // namespace escape
} // namespace chronicles
