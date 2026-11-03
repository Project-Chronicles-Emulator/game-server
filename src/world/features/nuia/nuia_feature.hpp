#pragma once

#include "world/core/frame_context.hpp"
#include "world/core/progress_access.hpp"
#include "world/features/nuia/nui_statue.hpp"
#include "world/features/nuia/nuia_tears.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace chronicles::nuia {

class NuiaSession;
class NuiaSessionContext;

class NuiaFeature final {
public:
  NuiaFeature() = default;
  NuiaFeature(std::shared_ptr<const NuiStatueData> statues, std::optional<NuiaTears> tears);

  // Called at the original startup position after quests and before channel events.
  // Missing files disable only their own service; malformed present files remain fatal.
  void load(const std::filesystem::path &catalog_root);
  bool has_statues() const noexcept;
  bool has_tears() const noexcept;
  bool has_data() const noexcept;
  std::vector<GameplayPacket> apply_revival_supplies(int task_id, WorldProgress &progress,
                                                     const Character &character) const;
  std::unique_ptr<NuiaSession> create_session(ProgressAccess &progress,
                                              NuiaSessionContext &context);

private:
  friend class NuiaSession;

  std::optional<NuiaTears> tears_;
  std::shared_ptr<const NuiStatueData> statues_;
};

// Commands stay synchronous on the existing worker. These ports borrow the real current
// owners; the feature holds no session/state pointer or duplicate combat/progress cache.
class NuiaSessionContext {
public:
  virtual ~NuiaSessionContext() = default;

  virtual bool nuia_has_handoff() const = 0;
  virtual bool nuia_has_tutorial() const = 0;
  virtual std::uint64_t nuia_player_unit_id() const = 0;
  virtual std::int64_t nuia_monotonic_ticks() const = 0;
  virtual bool nuia_has_character_stats() const = 0;
  virtual nlohmann::json nuia_character_stats(const Character &character) const = 0;
  virtual bool nuia_has_combat() const = 0;
  virtual bool nuia_player_dead() const = 0;
  virtual bool nuia_other_action_busy() const = 0;
  virtual bool nuia_processing_busy() const = 0;
  virtual bool nuia_riding() const = 0;
  // These intentionally preserve the escape owner's constructive lazy checks.
  virtual bool nuia_teleport_pending() = 0;
  virtual bool nuia_server_teleport_pending() = 0;

  virtual bool nuia_save_progress(const WorldProgress &before, const WorldProgress &after) = 0;
  virtual void nuia_publish_progress(const WorldProgress &progress) = 0;
  // Copy into combat, then return its live current stats. Called only with combat present.
  virtual const nlohmann::json &nuia_adopt_blessing_stats(const nlohmann::json &stats) = 0;
  virtual void nuia_publish_inventory_snapshot_locked(const Character &character,
                                                      const nlohmann::json *prepared_stats) = 0;
  virtual GameplayPacket nuia_growth_notification(std::int64_t character_id,
                                                  const nlohmann::json &before,
                                                  const nlohmann::json &after,
                                                  const nlohmann::json &current) const = 0;

  virtual std::vector<GameplayPacket> nuia_legacy_revive() = 0;
  virtual std::optional<WorldPosition> nuia_revival_position() const = 0;
  virtual std::optional<std::vector<GameplayPacket>>
  nuia_request_server_teleport(const WorldPosition &destination, std::int64_t now) = 0;
  virtual std::vector<GameplayPacket> nuia_persist_revival(const WorldPosition &destination,
                                                           int task_id) = 0;
  virtual std::vector<GameplayPacket> nuia_revive_combat(std::int64_t now) = 0;
  virtual void nuia_publish_combat_snapshot() = 0;
  virtual void nuia_publish_player_life() = 0;
};

class NuiaSession final {
public:
  NuiaSession(NuiaFeature &feature, ProgressAccess &progress, NuiaSessionContext &context);

  bool handle_statue(FrameContext &frame);
  bool handle_statue_busy_skill(FrameContext &frame);
  bool handle_tears(FrameContext &frame);
  void observe_tear_snapshot(FrameContext &frame);
  std::vector<GameplayPacket> revive_at_shrine(const WorldGameplayRequest &request);
  bool finish_escape_revival(std::vector<GameplayPacket> &packets);

  // Passive queries do not construct the domain action session. has_statue_session
  // deliberately differs from busy so the existing generic timer's cold guard survives.
  bool has_statue_session() const noexcept;
  bool busy() const;
  void cancel_action();
  std::optional<std::int64_t> next_action_tick() const;
  std::vector<GameplayPacket> advance(std::int64_t now, const WorldPosition &position);

private:
  NuiStatueSession &statue_session();
  std::vector<GameplayPacket> receive_statue(const WorldGameplayRequest &request,
                                             const WorldPosition &position);
  std::vector<GameplayPacket> receive_nuia_tear(const WorldGameplayRequest &request,
                                                const WorldPosition &position);

  NuiaFeature &feature_;
  ProgressAccess &progress_;
  NuiaSessionContext &context_;
  std::unique_ptr<NuiStatueSession> statue_;
  std::optional<NuiShrinePlacement> pending_shrine_revival_;
  bool tear_snapshot_sent_{};
  std::int64_t tear_window_start_ticks_{};
  unsigned tear_requests_in_window_{};
};

} // namespace chronicles::nuia
