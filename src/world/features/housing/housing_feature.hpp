#pragma once

#include "world/core/frame_context.hpp"
#include "world/core/progress_access.hpp"
#include "world/features/housing/housing_world.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace chronicles::housing {

class HousingSession;
class HousingSessionContext;

// The listener supplies its existing shared lifetime, with no second self-owning chain.
// A queued expiry job may run after stop, exactly as before, while the listener still exists.
// Its owner must retain this feature through the same WorldState for the lease's lifetime.
class HousingExpiryOwner {
public:
  virtual ~HousingExpiryOwner() = default;
  virtual bool housing_listener_open() const = 0;
};

class HousingFeature final {
public:
  HousingFeature(boost::asio::any_io_executor io_executor,
                 boost::asio::any_io_executor worker_executor,
                 std::chrono::steady_clock::duration expiry_interval = std::chrono::minutes(1));

  // Installs the same Hub shared with the front adapter at the original startup position.
  void set_hub(std::shared_ptr<HousingHub> hub);
  bool enabled() const noexcept;
  // A seated player's pose for a viewer that receives its UnitAddNoti later.
  std::optional<WorldPacket> late_viewer_pose(const nlohmann::json &unit_add_message) const;
  std::unique_ptr<HousingSession> create_session(::chronicles::ProgressAccess &progress,
                                                 HousingSessionContext &context);
  void schedule_expiry(std::weak_ptr<HousingExpiryOwner> owner);
  void stop();
  // Sessions can retain shared data after the listener dies. Preserve the cancellation
  // formerly supplied by destruction of the listener's own steady_timer member.
  void on_listener_destroyed() noexcept;

private:
  friend class HousingSession;

  std::shared_ptr<HousingHub> hub_;
  std::optional<boost::asio::steady_timer> expiry_timer_;
  boost::asio::any_io_executor worker_executor_;
  std::chrono::steady_clock::duration expiry_interval_;
};

class HousingSessionContext {
public:
  virtual ~HousingSessionContext() = default;

  virtual bool housing_has_handoff() const = 0;
  virtual bool housing_closed() const = 0;
  virtual std::int64_t housing_account_id() const = 0;
  virtual std::int64_t housing_character_id() const = 0;
  virtual int housing_world_id() const = 0;
  virtual const std::string *housing_character_name() const = 0;
  virtual std::uint64_t housing_connection_id() const = 0;
  virtual std::uint64_t housing_player_unit_id(std::int64_t character_id) const = 0;
  virtual bool housing_has_tutorial() const = 0;
  virtual const WorldProgress *housing_tutorial_progress() const = 0;
  virtual void housing_publish_progress(const WorldProgress &progress) = 0;

  // The containing session retains all executor/codec work and its single shared lifetime.
  virtual std::shared_ptr<SessionPort> housing_session_port() = 0;
  virtual std::optional<WorldPosition> housing_current_position() const = 0;
  virtual bool housing_begin_map_move(const WorldPosition &destination) = 0;
  virtual std::optional<WorldPosition>
  housing_public_return_origin(const std::optional<WorldPosition> &origin) const = 0;
  virtual std::optional<int> housing_raid_parent_map(int map_id) const = 0;
  // Where leaving the area goes when no origin was recorded (for example after a server
  // restart, or a login inside the area). `current` is the character's position in the area.
  virtual WorldPosition
  housing_default_return_position(const std::optional<WorldPosition> &current) const = 0;
  virtual void housing_clear_pending_exit() = 0;
  virtual void housing_stage_pending_exit(const WorldPosition &destination) = 0;
  virtual void housing_gm_reply(const std::string &line) = 0;

  // The session's 100 ns monotonic clock, used to time garden interactions.
  virtual std::int64_t housing_monotonic_ticks() const;
  // SP checks and drains for garden interactions. The default runs them without vitals.
  virtual HousingVitals housing_vitals();
};

class HousingSession final {
public:
  HousingSession(HousingFeature &feature, ::chronicles::ProgressAccess &progress,
                 HousingSessionContext &context);

  std::optional<HousingDestination> entry_destination(const Character &character) const;
  void stage_entry(const HousingDestination &destination);
  bool has_pending_entry() const noexcept;
  void cancel_pending_entry();
  void forget_origin();
  bool in_housing() const noexcept;
  int presence_map() const noexcept;

  void run_task(const std::function<void(const ProgressAccess &)> &task);
  void travel(const HousingDestination &destination);
  // Enters the housing area at `arrival` after the character walked into its cells (zone
  // channel move). True when the map move started.
  bool walk_in(const WorldPosition &arrival);
  std::optional<std::vector<GameplayPacket>> complete_entry();
  void on_map_departure();
  void on_leave_world();
  std::vector<GameplayPacket> publish_presence(bool entered_world,
                                               const std::optional<WorldPosition> &position);

  bool handle_request(FrameContext &frame);
  // Door/window toggles, furniture poses and garden/decoration interactions
  // (StartInteractionAnimReq on the housing unit or a placed doodad, and the
  // ExecuteInteractionReq/EndInteractionAnimReq of an open action) in the current channel.
  bool handle_toggle(FrameContext &frame);
  // The character's new position after a movement; a seated character leaving its seat no
  // longer shows its pose to later viewers.
  void observe_movement(const WorldPosition &position);
  bool handle_departure(FrameContext &frame);
  std::optional<std::vector<GameplayPacket>> dispatch_gm(const WorldGameplayRequest &request);
  // An object on a lot of the channel this character stands in (see HousingHub::placed_doodad).
  std::optional<HousingPlacedDoodad> placed_doodad(std::uint64_t unit_id, int map_id) const;

private:
  std::optional<HousingActor>
  authenticated_actor(const std::optional<WorldPosition> &position) const;
  ProgressAccess progress_access();
  static std::vector<GameplayPacket> gameplay_packets(std::vector<WorldPacket> packets);
  std::vector<GameplayPacket> receive(const WorldGameplayRequest &request,
                                      const std::optional<WorldPosition> &position);
  static int channel_presence_map(const HousingLocation &location);
  std::optional<WorldPosition> reported_departure_point(const nlohmann::json &request) const;
  void leave(const std::optional<WorldPosition> &departure_point = std::nullopt);

  HousingFeature &feature_;
  ::chronicles::ProgressAccess &progress_;
  HousingSessionContext &context_;
  std::optional<HousingDestination> pending_entry_;
  std::optional<WorldPosition> origin_;
  std::optional<HousingLocation> location_;
  // Published by the worker; presence and close read these on the I/O executor.
  std::atomic<int> presence_map_{};
  std::atomic<bool> attached_{};
};

} // namespace chronicles::housing
