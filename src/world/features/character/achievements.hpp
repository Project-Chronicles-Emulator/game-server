#pragma once

#include "data/client_catalog.hpp"
#include "ec.generated.pb.h"
#include "persistence/character_state_types.hpp"
#include "protocol/gameplay_packet.hpp"
#include "world/features/character/progression.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace chronicles::character {

inline constexpr std::uint16_t kUpdateAchievementRequest = 20061;
inline constexpr std::uint16_t kAchievementUpdateNotification = 121;
inline constexpr std::uint16_t kAchievementSnapshotNotification = 143;

enum class AchievementGoalKind {
  character_level,
  quest_count,
  adventure_target,
  adventure_count,
  logbook_percent,
  play_record,
  expertise_level,
  player_kills,
  zone_state,
};

struct AchievementGoal {
  AchievementGoalKind kind{};
  int subject_id{};
  std::string zone;
  int object_type{};
  std::int64_t required{};
};

// Values are read from server-owned facts. A missing source is unavailable, never zero
// progress supplied by the requesting client or permission to accept its claimed state.
using AchievementGoalValue = std::function<std::optional<std::int64_t>(const AchievementGoal &)>;

struct AchievementPlan {
  x5::ec::EErrorCode error{x5::ec::Success};
  WorldProgress after;
  std::vector<GameplayPacket> packets;
};

class AchievementCatalog {
public:
  static std::shared_ptr<const AchievementCatalog> load(ClientCatalog &catalog);
  AchievementCatalog(const std::vector<nlohmann::json> &achievements,
                     const std::vector<nlohmann::json> &goals,
                     const std::vector<nlohmann::json> &trophies);

  AchievementPlan plan(const WorldProgress &before, int cms_id, int requested_state,
                       std::int64_t completion_tick, const AchievementGoalValue &goal_value,
                       const Progression *progression) const;
  std::size_t size() const noexcept;

private:
  enum class RewardKind { gold, hersander_token, experience, trophy };
  struct Definition {
    int parent_id{};
    std::vector<int> children;
    std::vector<AchievementGoal> goals;
    RewardKind reward{};
    std::int64_t reward_value{};
  };
  std::map<int, Definition> definitions_;
};

nlohmann::json achievement_snapshot(const WorldProgress &progress);
std::optional<std::int64_t> stored_achievement_goal_value(const AchievementGoal &goal,
                                                          const WorldProgress &progress);

} // namespace chronicles::character
