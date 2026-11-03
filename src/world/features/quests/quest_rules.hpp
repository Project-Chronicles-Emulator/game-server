#pragma once

#include "common/currency.hpp"
#include "protocol/gameplay_packet.hpp"
#include "persistence/character_state_types.hpp"

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace chronicles {
class Progression;
}

// Quest and trigger rules shared by the private tutorial instance (tutorial.cpp) and the
// shared-world quest service (world_quests.cpp). Definitions use the generated quest shape:
// objective groups (`progresses` with sequence/group/kind/parameters), trigger geometry in
// millimeters, item rewards (`item_cms_id` 0 is gold) and catalog experience.
namespace chronicles::quest_rules {

// A request refused with a client-visible error code (CommonErrorNoti `ec`).
struct RuleError {
  std::string code;
};

void require(bool condition, const char *code);

// Reported request positions may lag the tracked position by this much.
inline constexpr double kPositionToleranceMm = 5000;
// Talking to or using a world object requires the tracked position within this range.
inline constexpr double kInteractionRangeMm = 7000;
// Automatic PC2S_DiscoverAdventureObjectReq radius. Observed original-client behavior
// (2026-11-01 live packet log): all 23 reports the server refused were within 100.0 m
// horizontally of their target, while their 3D distance reached 112 m on cliffs. The client
// therefore measures this radius on the ground plane, independent of height.
inline constexpr double kDiscoveryRangeMm = 100000;

inline constexpr std::uint16_t kCharacterBaseChangedOpcode = 36;
inline constexpr std::uint16_t kItemsUpdatedOpcode = 169;

double distance_squared(const WorldPosition &position, const nlohmann::json &point);
bool request_position_matches(const nlohmann::json &request, const WorldPosition &position);
// True when an automatic discovery report from `position` (the server-tracked position) is
// within the client's discovery radius of the target at (`target_x_mm`, `target_y_mm`).
// Height is ignored like the client does, and the radius is widened by kPositionToleranceMm
// because the tracked position lags the position the client measured from.
bool within_discovery_range(const WorldPosition &position, double target_x_mm, double target_y_mm);
// Same check for a target given as a generated [x, y, z] millimeter point.
bool within_discovery_range(const WorldPosition &position, const nlohmann::json &point);
// Sphere (`radius`) or yaw-rotated box (`extent`, `yaw`) volume, grown by `tolerance_mm`.
bool position_within_trigger(const nlohmann::json &trigger, const WorldPosition &position,
                             double tolerance_mm);

nlohmann::json quest_json(int cms_id, const QuestProgress &quest);
// Official QuestUpdateNoti for one progress request lists only the objective that changed.
nlohmann::json quest_progress_update_json(int cms_id, const QuestProgress &quest,
                                          int sequence_id);

int objective_required_count(const nlohmann::json &objective);
// Sequential quests require every earlier group before an objective of `group` can advance.
bool objective_ready(const nlohmann::json &definition, const QuestProgress &quest, int group);
// Alternatives within one group use OR; every group must be satisfied.
bool objectives_complete(const nlohmann::json &definition, const QuestProgress &quest);
bool objective_group_complete(const nlohmann::json &definition, const QuestProgress &quest,
                              int group);

using QuestLookup = std::function<const nlohmann::json *(int)>;

// Candidate semantics, recovered from the requirement names and the tutorial trace:
// Acceptable = not yet accepted, Progressing = accepted and incomplete (optionally with
// `progress_group` as the current unsatisfied group), QuestProgress = `progress_group`
// satisfied, QuestComplete = completed.
bool quest_requirement_met(const QuestLookup &lookup, const nlohmann::json &condition,
                           const WorldProgress &state);

// Rows sharing a `group` combine with that group's operator (AND/OR); separate groups are
// alternatives. `evaluate` returns one row's result before its `negated` flag is applied.
bool grouped_conditions_pass(const nlohmann::json &conditions,
                             const std::function<bool(const nlohmann::json &)> &evaluate);

// Credits item stacks and gold (item_cms_id 0) into `next`, appending one ItemsUpdateNoti.
// An item_cms_id 0 entry with `money_type` "HersanderToken" credits Hersander tokens instead.
// `next` is a working copy; a refused credit throws RuleError before anything is committed.
void grant_rewards(WorldProgress &next, const nlohmann::json &rewards,
                   std::vector<GameplayPacket> &packets, const char *reason);

// Applies a signed Gold or Hersander token change to `next` and appends its wire MoneyChange to
// `update["money_changes"]`. Throws RuleError{code} when the balance would become negative or
// exceed the money limit.
void change_money(WorldProgress &next, MoneyType type, std::int64_t delta,
                  nlohmann::json &update, const char *code);

// Adds operator-scaled catalog experience. With a Progression, the award levels the character
// up and grants LevelUpPoints; ChangeCharacterBaseNoti carries `exp` plus the new level and
// point balance on a level-up. Without one (fixtures), only the cumulative EXP changes.
void grant_experience(WorldProgress &next, std::int64_t catalog_experience,
                      std::vector<GameplayPacket> &packets,
                      const Progression *progression = nullptr);

} // namespace chronicles::quest_rules
