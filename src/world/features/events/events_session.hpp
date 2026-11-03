#pragma once

#include "common/world_metrics.hpp"
#include "world/core/frame_context.hpp"
#include "world/features/events/event_gm_commands.hpp"
#include "world/features/events/events_feature.hpp"
#include "world/features/party/party_world.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace chronicles {

class MapChannels;

namespace events {

struct EventsCharacter {
  int world_id{};
  std::int64_t character_id{};
};

// Feature-local capabilities required by the existing implementation. The composition
// adapter selects tutorial/shared progress, live combat and map state when each call runs;
// this port never caches another feature's state or exposes WorldSession/WorldState.
class EventsSessionContext {
public:
  virtual ~EventsSessionContext() = default;

  virtual std::uint64_t events_connection_id() const = 0;
  virtual bool events_closed() const = 0;
  virtual std::optional<EventsCharacter> events_character() const = 0;
  virtual const party::MemberPresence *events_party_identity() const = 0;
  virtual std::int64_t events_monotonic_ticks() const = 0;
  virtual std::int64_t events_epoch_ticks() const = 0;

  // The same mutex used by handshake position/close. *_locked requires its caller to hold it.
  virtual world_metrics::TimedMutex &events_protocol_mutex() const = 0;
  virtual std::optional<WorldPosition> events_position_locked() const = 0;
  virtual std::optional<WorldPosition> events_pending_map_destination() const = 0;
  virtual bool events_has_tutorial() const = 0;
  virtual bool events_in_housing() const = 0;
  virtual bool events_other_action_busy() const = 0;
  virtual bool events_has_gm_units() const = 0;
  virtual bool events_player_dead() const = 0;
  virtual bool events_begin_map_move(const WorldPosition &destination,
                                     bool preserve_player_combat) = 0;
  // Original pending-housing reset, pending-exit assignment, then spawned-GM-unit clear.
  virtual void events_stage_map_destination(const WorldPosition &destination) = 0;
  virtual const MapChannels *events_map_channels() const = 0;
  virtual bool events_zone_channel_move(const WorldGameplayRequest &request,
                                        const std::optional<WorldPosition> &position) = 0;

  virtual WorldProgress events_world_progress() const = 0;
  virtual bool events_save_world_progress(const WorldProgress &before,
                                          const WorldProgress &after) = 0;
  virtual void events_adopt_committed_progress(const WorldProgress &after) = 0;
  virtual void events_deliver_observers(const std::vector<GameplayPacket> &packets) = 0;
  virtual void events_gm_reply(const std::string &message) = 0;
};

// One per connected player, owned by that player's composition. Every mutation runs on
// its existing worker strand, except protocol-locked position queries during I/O close.
class EventsSession final {
public:
  EventsSession(EventsFeature &feature, EventsSessionContext &context);

  bool handle(FrameContext &frame, raid::RaidUpdate &raid_update);
  std::vector<GameplayPacket> execute_gm_command(const event_gm::EventCommandParse &parse);
  std::vector<GameplayPacket> channel_event_info(const std::optional<WorldPosition> &position);

  // Invoked at the original lifecycle positions. These perform actual feature work;
  // no empty entry hook is introduced because events has no entry-time initialization.
  void on_map_load_complete();
  void on_raid_eviction(int raid_map_id);
  void on_map_departure(const WorldPosition &destination);
  void on_leave_world();

  bool authorize_server_map_move(const WorldPosition &destination) const;
  // Worker reads are serialized; an I/O close caller must already hold protocol_mutex.
  WorldPosition persisted_position(const WorldPosition &position) const;
  std::optional<WorldPosition>
  housing_return_origin(const std::optional<WorldPosition> &fallback) const;

private:
  struct EventRouting {
    bool handled{};
    bool moved{};
  };

  EventRouting receive_event_request(const WorldGameplayRequest &request,
                                     const std::optional<WorldPosition> &position,
                                     std::vector<GameplayPacket> &packets,
                                     raid::RaidUpdate &raid_update);
  GameplayPacket ranking_list(const WorldGameplayRequest &request);
  std::optional<raid::RaidApplicant> raid_applicant() const;
  raid::RaidUpdate receive_raid_request(const WorldGameplayRequest &request);
  bool receive_raid_move(const WorldGameplayRequest &request,
                         const std::optional<WorldPosition> &position,
                         std::vector<GameplayPacket> &packets);
  GameplayPacket tribute_count(const WorldGameplayRequest &request,
                               const std::optional<WorldPosition> &position) const;
  void receive_item_tribute(const WorldGameplayRequest &request,
                            const std::optional<WorldPosition> &position,
                            std::vector<GameplayPacket> &packets);
  bool receive_channel_trigger_entry(const WorldGameplayRequest &request,
                                     const std::optional<WorldPosition> &position,
                                     std::vector<GameplayPacket> &packets);
  std::vector<RankingMember> ranking_participants(const WorldPosition &position) const;
  std::vector<GameplayPacket> execute_ranking_gm_command(const event_gm::EventCommand &command,
                                                         const WorldPosition &position);
  std::vector<GameplayPacket> execute_raid_gm_command(const event_gm::EventCommand &command);
  std::vector<GameplayPacket>
  execute_channel_event_gm_command(const event_gm::EventCommand &command,
                                   const WorldPosition &position);
  std::string channel_event_summary(const WorldPosition &position) const;

  EventsFeature &feature_;
  EventsSessionContext &context_;
  // Same 32-per-second local policy as the original native session.
  std::int64_t ranking_window_start_ticks_{};
  unsigned ranking_requests_in_window_{};
  ranking::RankingRuns ranking_runs_;
  // Sole authoritative raid return point. Writes and close-time reads use protocol_mutex.
  std::optional<WorldPosition> raid_origin_;
};

} // namespace events
} // namespace chronicles
