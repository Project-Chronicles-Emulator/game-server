#pragma once

#include "world/core/instance_copies.hpp"
#include "world/features/combat/combat.hpp"
#include "world/features/combat/fall_damage.hpp"
#include "world/features/combat/movement_vitals.hpp"
#include "world/features/combat/player_pvp.hpp"
#include "world/features/combat/revival_spawns.hpp"

#include <filesystem>
#include <memory>
#include <mutex>

namespace chronicles {

class ClientCatalog;

namespace combat {

class CombatPlayerSession;
class CombatPlayerContext;

// Result-bearing raid admission at the original point, before the population-cache lock.
// Neither the shared Combat owner nor an encounter retains this composition adapter.
class CombatEncounterAdmission {
public:
  virtual ~CombatEncounterAdmission() = default;

  virtual bool combat_is_raid_map(int map_id) const = 0;
  virtual std::optional<std::uint64_t> combat_raid_instance_owner(std::int64_t character_id,
                                                                  int map_id) const = 0;

  // The instance or dungeon copy the character occupies on `map_id` (InstanceCopies); null on
  // shared maps or when the composition allocates no copies.
  virtual std::shared_ptr<InstanceCopy> combat_instance_copy(int map_id) const {
    static_cast<void>(map_id);
    return {};
  }
};

class CombatFeature final {
public:
  explicit CombatFeature(std::shared_ptr<ClientCatalog> catalog = {});

  // Separate real startup stages keep their old positions relative to other owners.
  void load_encounters(const TutorialData *tutorial, const std::filesystem::path &catalog_root);
  void load_movement_rules(ClientCatalog &catalog);
  void load_revival_spawns(bool tutorial_enabled, ClientCatalog *catalog,
                           const std::filesystem::path &catalog_root);

  const std::shared_ptr<const CombatData> &data() const noexcept;
  const FallDamageRules &fall_rules() const noexcept;
  const MovementVitalsRules &movement_rules() const noexcept;
  const std::optional<RevivalSpawns> &revival_spawns() const noexcept;
  bool has_catalog() const noexcept;
  std::optional<nlohmann::json> equipment_definition(int item_cms_id) const;
  PlayerPvpRegistry &player_pvp() noexcept;
  const PlayerPvpRegistry &player_pvp() const noexcept;

  std::shared_ptr<CombatSession::WorldMobs>
  population_for(int map_id, std::int64_t character_id, const CombatEncounterAdmission &admission);

  // Delay instance monster announcements until StartPlaying, as in the captured entry.
  bool announce_population_after_start(int map_id) const;
  std::unique_ptr<CombatPlayerSession> create_session(CombatPlayerContext &context);

private:
  const std::shared_ptr<ClientCatalog> catalog_;
  std::shared_ptr<const CombatData> data_;
  std::map<int, std::shared_ptr<CombatSession::WorldMobs>> populations_;
  std::map<int, nlohmann::json> definitions_;
  std::mutex instance_mutex_;
  std::map<std::pair<int, std::uint64_t>, std::weak_ptr<CombatSession::WorldMobs>> instances_;
  FallDamageRules fall_rules_;
  MovementVitalsRules movement_rules_;
  std::optional<RevivalSpawns> revival_spawns_;
  PlayerPvpRegistry player_pvp_;
};

} // namespace combat
} // namespace chronicles
