#pragma once

#include "protocol/gameplay_packet.hpp"
#include "persistence/character_repository.hpp"
#include "persistence/character_state_types.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include <nlohmann/json.hpp>

namespace chronicles {

struct NuiShrinePlacement {
  int cms_id{};
  std::uint64_t unit_id{};
  bool flame{};
  WorldPosition position;
  WorldPosition spawn;
  int restricted_combat_radius_mm{};
  int revive_task_id{};
};

// Immutable, hash-checked export of the researched build's statues and flames.
// It indexes authored points, not terrain, collision or server navigation data.
class NuiStatueData {
public:
  static std::shared_ptr<const NuiStatueData> load(const std::filesystem::path &path);
  explicit NuiStatueData(const nlohmann::json &document);

  const NuiShrinePlacement *find(int map_id, std::uint64_t unit_id) const;
  int execute_time_ms(int interaction_id) const;
  int utility_capacity(int item_cms_id, const Character *character = nullptr) const;
  int remaining_tears(const WorldProgress &progress) const;
  void dedicate(WorldProgress &progress, int count) const;

  // Conservative local economics: utility slots reserve owned stack units without spending
  // or creating them. Capacity and equipped-bag tiers are recovered client behavior.
  void supplement_utility_bag(WorldProgress &progress, const Character *character = nullptr) const;
  std::vector<GameplayPacket> apply_revival_supplies(int task_id, WorldProgress &progress,
                                                     const Character *character = nullptr) const;

  // Candidate policy: nearest activated statue, or nearest flame on flame-only maps or
  // when explicitly requested. No cross-map/type fallback and no client destination.
  const NuiShrinePlacement *revival_shrine(const WorldPosition &current,
                                           const WorldProgress &progress, bool to_flame) const;
  std::optional<WorldPosition> revival_destination(const WorldPosition &current,
                                                   const WorldProgress &progress,
                                                   bool to_flame) const;

private:
  struct RevivalTask {
    std::map<int, int> supplies;
    bool refill_utility_bag{};
  };

  std::map<std::pair<int, std::uint64_t>, NuiShrinePlacement> placements_;
  std::map<int, int> execute_times_ms_;
  std::map<int, int> utility_capacity_;
  std::map<int, int> utility_item_groups_;
  std::map<int, std::array<int, 6>> utility_group_tiers_;
  std::map<int, std::pair<int, std::set<int>>> bag_options_;
  std::map<int, RevivalTask> revival_tasks_;
  std::set<int> flame_only_maps_;
  std::vector<int> required_tears_;
};

// One session's animated activation/refill state. The caller serializes receive(), commits
// through the supplied revision guard, then adopts the updated progress before publishing.
class NuiStatueSession {
public:
  using Commit = std::function<bool(const WorldProgress &, const WorldProgress &)>;

  NuiStatueSession(std::shared_ptr<const NuiStatueData> data, std::uint64_t player_unit_id,
                   Commit commit);

  bool handles(std::uint16_t opcode, const nlohmann::json &request,
               const WorldPosition &position) const;
  bool busy() const;
  void cancel_action();
  std::optional<std::int64_t> next_action_tick() const;
  std::vector<GameplayPacket> advance(std::int64_t now, const WorldPosition &position);

  std::vector<GameplayPacket> receive(std::uint16_t opcode, const nlohmann::json &request,
                                      std::uint32_t sequence, const WorldPosition &position,
                                      std::int64_t now, WorldProgress &progress, bool other_busy,
                                      const Character *character = nullptr);

private:
  struct Action {
    int map_id{};
    std::uint64_t target_unit_id{};
    int interaction_id{};
    std::int64_t started_tick{};
    bool executed{};
  };

  void commit_progress(WorldProgress &current, WorldProgress next);
  std::vector<GameplayPacket> execute(const WorldPosition &position, std::int64_t now,
                                      WorldProgress &progress, const Character *character);

  std::shared_ptr<const NuiStatueData> data_;
  std::uint64_t player_unit_id_{};
  Commit commit_;
  std::optional<Action> action_;
  std::int64_t request_window_tick_{};
  unsigned requests_in_window_{};
  std::optional<std::uint32_t> last_offering_sequence_;
  nlohmann::json last_offering_request_;
};

} // namespace chronicles
