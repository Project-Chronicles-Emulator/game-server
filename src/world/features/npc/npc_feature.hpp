#pragma once

#include "world/core/frame_context.hpp"
#include "world/features/npc/map_static_npcs.hpp"
#include "world/features/npc/npc_patrol.hpp"
#include "world/features/npc/npc_population.hpp"
#include "world/features/npc/npc_reactions.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace chronicles::npc {

class NpcFeature {
public:
  NpcFeature(std::shared_ptr<const NpcPopulation> population, std::int64_t epoch_tick,
             std::optional<NpcReactionCatalog> reactions = std::nullopt);

  // Called at the original optional-catalog startup position. An invalid present export
  // still throws; an absent export remains optional according to MapStaticNpcs::load.
  void load_static_npcs(const std::filesystem::path &catalog_root);

  // Loads every `npc/placements/map-<id>.json` beside the catalog so that each map has its
  // own server-owned civilians. The configured primary population keeps its map; another
  // file for the same map is ignored. An invalid present file still throws at startup.
  void load_map_populations(const std::filesystem::path &catalog_root);

  // The configured primary population: the default overworld map after the tutorial.
  const std::shared_ptr<const NpcPopulation> &population() const noexcept;
  // The shared population of one map, or nullptr when that map has none.
  std::shared_ptr<const NpcPopulation> population_for(int map_id) const;
  std::size_t population_map_count() const noexcept;
  const std::optional<MapStaticNpcs> &static_npcs() const noexcept;
  std::int64_t epoch_tick() const noexcept;
  const std::optional<NpcReactionCatalog> &reactions() const noexcept;
  NpcReactionTimeline &reaction_timeline() const noexcept;

private:
  const std::shared_ptr<const NpcPopulation> population_;
  std::map<int, std::shared_ptr<const NpcPopulation>> populations_;
  const std::int64_t epoch_tick_;
  std::optional<MapStaticNpcs> static_npcs_;
  std::optional<NpcReactionCatalog> reactions_;
  mutable NpcReactionTimeline reaction_timeline_;
};

// The registry is the existing bounded application registry shared with packet delivery.
// These views do not copy unit identity, map authority or player-ID conversion policy.
class NpcSessionContext {
public:
  virtual ~NpcSessionContext() = default;

  virtual UnitRegistry &npc_registry() = 0;
  virtual const UnitRegistry &npc_registry() const = 0;
  virtual int npc_session_map_id() const = 0;
  virtual std::string npc_instance_key() const = 0;
  virtual bool npc_has_tutorial() const = 0;
  virtual bool npc_authenticated() const = 0;
  virtual std::int64_t npc_monotonic_ticks() const = 0;
  virtual bool npc_player_alive() const = 0;
  virtual std::string npc_reaction_scope() const = 0;
  virtual void npc_deliver_observers(const std::vector<GameplayPacket> &packets) = 0;
};

class NpcPlayerSession {
public:
  NpcPlayerSession(const NpcFeature &feature, NpcSessionContext &context);

  const AnnouncedUnit *interaction_npc(std::uint64_t unit_id) const;
  std::optional<int> session_owned_population_map() const;
  void set_population_map(std::optional<int> map_id);
  void announce_population();
  std::int64_t population_elapsed_ticks() const;
  std::vector<GameplayPacket> refresh_npc_sight(const WorldPosition &position);
  void track_announced_unit(const GameplayPacket &packet);

  void observe_patrol_position(std::uint16_t request_opcode);
  // A true result tells generic map orchestration to rearm its sector window next.
  bool observe_shared_population(const FrameContext &frame);
  void observe_npc_sight(FrameContext &frame);
  void observe_start_playing_patrol(bool entering_world,
                                    std::vector<GameplayPacket> &session_packets);

  bool handle_threat(FrameContext &frame);
  bool has_patrol() const noexcept;
  std::optional<std::int64_t> next_action_tick() const;
  std::vector<GameplayPacket> advance_patrol(std::int64_t ticks, const WorldPosition &position);

private:
  ReactionTimeline timeline() const;
  // The shared population of the map this session announced, or nullptr.
  std::shared_ptr<const NpcPopulation> current_population() const;
  // True when the unit belongs to the shared population and walks an authored route, so its
  // reaction pause must be retained for its later route pose.
  bool follows_patrol_route(std::uint64_t unit_id) const;

  const NpcFeature &feature_;
  NpcSessionContext &context_;
  std::optional<int> entered_population_map_;
  bool population_announced_{};
  std::unique_ptr<NpcPatrolSession> npc_patrol_;
  NpcSightTracker npc_sight_;
};

} // namespace chronicles::npc
