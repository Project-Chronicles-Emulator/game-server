#include "world/features/combat/combat.hpp"
#include "common/clock_ticks.hpp"

#include "cms.generated.pb.h"
#include "data/starter_gear.hpp"
#include "world/core/world_request_opcodes.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include <spdlog/spdlog.h>

namespace chronicles {
namespace {

using Json = nlohmann::json;
constexpr std::int64_t kTicksPerMillisecond = 10000;
constexpr std::int64_t kTicksPerSecond = 10000000;
constexpr std::uint16_t kChangeStatNotification = 47;
constexpr std::uint16_t kUpdateStatusEffectNotification = 50;
constexpr std::uint16_t kStatSyncNotification = 54;
constexpr std::uint16_t kAddStatRecoveryNotification = 80;
// The tutorial potion's consumption effect. Without item skills in the combat profile its
// ExecuteSkillEffectReq belongs to the tutorial session's fallback potion path.
constexpr int kTutorialPotionUseEffectId = 100000141;
constexpr std::size_t kMaxActiveRecoveries = 32;
constexpr std::size_t kMaxItemSkills = 4096;
// Every placed world monster type contributes its skills; mirrors MAX_COMBAT_SKILLS in
// tools/build_combat_data.py.
constexpr std::size_t kMaxCombatSkills = 4096;
constexpr std::size_t kMaxStatModifiersPerStatus = 16;
constexpr int kMaxStatModifierMagnitude = 1000000;
constexpr int kMaxRecoveryIntervalMs = 60000;
constexpr int kMaxRecoveryDurationMs = 24 * 60 * 60 * 1000;
constexpr double kMaxRecoveryAmount = 1000000.0;
// EX5StatType_PotionHealRate, per mille like character_stats.cpp's neutral rate: 1000 keeps a
// potion's catalog value. Official potion flows (22 of 22) carry exactly the catalog rate.
constexpr int kPotionHealRateStat = 91;
constexpr int kNeutralStatRate = 1000;
constexpr std::uint16_t kUnitAddNotification = 10;
constexpr std::uint16_t kUnitRemoveNotification = 11;
constexpr std::uint16_t kDespawnSkillObjectNotification = 45;
constexpr std::uint16_t kAbnormalGaugeNotification = 52;
// Skill-object units carry type nibble 3 (every captured area unit id ends in 3) and, like every
// captured area id (for example 4099 and 24579), fit in 32 bits. The client resolves
// unit-addressed notifications through the low 32 bits of the wire id (0x146092DB0, see
// docs/world-mobs.md), so the former bit-60 tag left the area's later DamageNoti caster,
// DespawnSkillObjectNoti and UnitRemoveNoti pointing at no actor. Area ids are
// kAreaUnitIdBase | (sequence << 4) | 3: the 0xD0000000 block lies above the world-mob ids
// (0x9xxxxxx2) and below the companion (0xE0000000) and GM-spawned NPC (0xF0000000) blocks, and
// nibble 3 cannot match a player (1), NPC (2) or vehicle (4) id.
constexpr std::uint64_t kAreaUnitTypeNibble = 3;
constexpr std::uint64_t kAreaUnitIdBase = 0xD0000000ULL;
constexpr std::uint64_t kAreaUnitSequenceCount = 1ULL << 24;
// Observed: the official lifetime despawn of an area (45) preceded its unit removal (11) by
// 1496 ms.
constexpr std::int64_t kAreaRemovalDelayMs = 1496;
// CANDIDATE: how long a player's targeted or aimed area event waits for the client's
// PC2S_ExecuteSkillEffectReq placement before it falls back. Official area spawns follow the
// client's effect report (created_tick equals the report tick), and the report and the server
// deadline are offset by the same one-way latency, so a short window covers their jitter.
constexpr int kPlacementHintWaitMs = 300;
// A reported target or aim point may lie this far beyond the skill's range, like the 5 m
// allowance for client-reported projectile launch points.
constexpr double kPlacementHintToleranceMm = 5000;
// Bound on areas alive at once in one session, player and monster areas together; an area
// counts until its unit removal. The largest single cast spawns 25 (Lightning Storm 2235/2296
// and 2254, captured as 25 areas per cast), a boss pattern 16, and casts can overlap.
constexpr std::size_t kMaxActiveAreas = 128;
constexpr std::size_t kMaxMonsterStatuses = 16;
constexpr std::size_t kMaxItemEffectLists = 4096;
constexpr std::size_t kMaxItemEffectsPerList = 16;
constexpr int kMaxAreaRadiusMm = 100000;
constexpr int kMaxAreaDurationMs = 24 * 60 * 60 * 1000;
constexpr std::size_t kMaxActiveStatuses = 32;
constexpr std::size_t kMaxStatusEffectDefinitions = 4096;
constexpr int kMaxStatusDurationMs = 24 * 60 * 60 * 1000;
constexpr std::uint16_t kAddStatusEffectNotification = 49;
constexpr std::uint16_t kRemoveStatusEffectNotification = 51;
// Stat 71 holds a unit's remaining shield. Official shield grants report it with
// ChangeStatNoti, absorbed hits lower it in DamageNoti, and death clears it.
constexpr int kShieldStat = 71;
constexpr int kDeathClearedStat = kShieldStat;
// Recovered opcodes (protocol/opcodes.json) of hit immunity, cooldown reset and slot swaps.
constexpr std::uint16_t kImmuneNotification = 20;
constexpr std::uint16_t kResetCooldownNotification = 22;
constexpr std::uint16_t kChangeWeaponSkillSlotNotification = 96;
constexpr int kMaxImmunityRows = 1024;
constexpr int kMaxShieldRows = 4096;
constexpr int kMaxShieldAmount = 10000000;
constexpr int kMaxPerMille = 100000;
constexpr int kHealth = 69;
constexpr int kStamina = 72;
constexpr int kMaxHealth = 18;
constexpr int kMaxStamina = 19;
constexpr double kPositionToleranceMm = 5000;
constexpr double kNpcEngagementRangeMm = 5000;
// Candidate transfer radius: nearby party members use the same 30 m range as shared loot.
constexpr double kMonsterTargetTransferRangeMm = 30000;
// Candidate NPC position frame cadence (builder-2 addendum 28: mode 0 snaps when the new position
// is within 50 cm of the client's object position, and after the blend window max(interval, 1 s)
// the pawn teleports to the last target). A continued Walk is sent after this much authoritative
// displacement (> the 500 mm snap radius) or after the maximum interval (< the 1 s blend window).
constexpr double kNpcMoveNotiMinStepMm = 600.0;
constexpr std::int64_t kNpcMoveNotiMaxIntervalMs = 900;
constexpr std::int64_t kNpcMoveNotiMinPredMilli = 16;
constexpr std::size_t kMaximumProjectiles = 256;
// Recovered opcodes of the projectile lifecycle (protocol/opcodes.json).
constexpr std::uint16_t kSpawnProjectileNotification = 40;
constexpr std::uint16_t kSpawnClientProjectileFailNotification = 41;
constexpr std::uint16_t kProjectileHitNotification = 42;
constexpr std::uint16_t kDespawnProjectileNotification = 44;
using world_request::kClientProjectileHitRequest;
using world_request::kDespawnClientProjectileRequest;
using world_request::kExecuteSpawnProjectileRequest;
using world_request::kUpdateProjectileRequest;
// Upper bound on the projectiles one spawn report may launch, whatever its catalog
// maxSpawnCount says; the largest catalog value is far below it.
constexpr int kMaximumClientProjectilesPerReport = 16;
// Catalog projectile speeds and spawn offsets are centimetres (per second).
constexpr double kMillimetresPerCatalogCentimetre = 10.0;
constexpr std::size_t kMaxNpcStats = 8192;
constexpr int kCommonTutorialCombatAiPhase = 1230;

constexpr std::uint16_t kEndSkillNotification = 15;
constexpr std::uint16_t kChangeSkillStageResponse = 16;
using world_request::kChangeSkillStageRequest;
using world_request::kSkillEffectReportRequest;
using world_request::kStartSkillRequest;

// A client reports a transition from its own action clock, which starts about
// one network trip before the server's copy. Window edges accept this skew.
constexpr std::int64_t kStageTransitionToleranceMs = 250;

// x5.pb.StageTransitionReason names. Decoded proto3 JSON omits the zero value,
// so a request without a reason reports the stage's default transition.
constexpr std::string_view kDefaultTransitionReason = "default";
constexpr std::string_view kInputConditionReason = "input_condition";
constexpr std::string_view kGroundLandingReason = "ground_landing";

// Client-detected outcomes that an extracted action_stage_condition notify can
// authorize. The server has no terrain or grab simulation; it validates the
// condition type, target stage and window. Only ground landing has dedicated
// recovered EErrorCode names, so grab refusals use the generic stage code.
struct ActionStageConditionReason {
  std::string_view reason;
  std::string_view condition_type;
  const char *unable_code;
  const char *invalid_stage_code;
  const char *invalid_tick_code;
};

constexpr std::array<ActionStageConditionReason, 3> kActionStageConditionReasons{{
    {kGroundLandingReason, "EX5ActionStageConditionType_Ground_Landing",
     "Skill_GroundLandingUnable", "Skill_GroundLandingInvalidStageIndex",
     "Skill_GroundLandingInvalidTick"},
    {"grab_fail", "EX5ActionStageConditionType_GrabFail", "Skill_InvalidStage",
     "Skill_InvalidStage", "Skill_InvalidStage"},
    {"grab_success", "EX5ActionStageConditionType_GrabSuccess", "Skill_InvalidStage",
     "Skill_InvalidStage", "Skill_InvalidStage"},
}};

enum class StageRequestOutcome {
  Apply,
  // An early report of the correct default transition. The server deadline
  // still performs that transition, so the report changes nothing.
  Defer,
  Reject,
};

struct StageRequestDecision {
  StageRequestOutcome outcome{};
  const char *error_code{};
};

struct CombatError {
  const char *code;
};

void require(bool condition, const char *code) {
  if (!condition) {
    throw CombatError{code};
  }
}

std::uint64_t unit_id(const Json &value) {
  const auto text = value.get<std::string>();
  std::uint64_t result{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || result == 0) {
    throw std::invalid_argument("Invalid combat unit identifier");
  }
  return result;
}

Json wire_position(const WorldPosition &position) {
  return {{"millimeter_x", position.x_mm},
          {"millimeter_y", position.y_mm},
          {"millimeter_z", position.z_mm}};
}

double distance_squared(const WorldPosition &left, const WorldPosition &right) {
  const double x = static_cast<double>(left.x_mm) - right.x_mm;
  const double y = static_cast<double>(left.y_mm) - right.y_mm;
  const double z = static_cast<double>(left.z_mm) - right.z_mm;
  return x * x + y * y + z * z;
}

// True when the next continued Walk is due: the monster moved far enough since the last Walk or
// the maximum interval passed.
bool npc_move_frame_due(const WorldPosition &current, const WorldPosition &last_announced,
                        std::int64_t ticks, std::int64_t last_tick) {
  const double dx = static_cast<double>(current.x_mm) - last_announced.x_mm;
  const double dy = static_cast<double>(current.y_mm) - last_announced.y_mm;
  const double dz = static_cast<double>(current.z_mm) - last_announced.z_mm;
  return std::sqrt(dx * dx + dy * dy + dz * dz) >= kNpcMoveNotiMinStepMm ||
         ticks - last_tick >= kNpcMoveNotiMaxIntervalMs * kTicksPerMillisecond;
}

// Reads a wire x5.pb.WorldPos on the caller's map. Absent axes are proto3 zeros.
WorldPosition wire_point(const Json &point, int map_id) {
  return {map_id, point.value("millimeter_x", 0), point.value("millimeter_y", 0),
          point.value("millimeter_z", 0)};
}

bool matches_position(const Json &message, const WorldPosition &position) {
  if (!message.contains("pos")) {
    return false;
  }
  const auto supplied = wire_point(message.at("pos"), position.map_id);
  return distance_squared(supplied, position) <= kPositionToleranceMm * kPositionToleranceMm;
}

// Player root-motion reach, measured against 1,407 official PC2S_StartX5RootMotionReq frames
// (captures of 2026-10-24..26). The client overshoots the authored reach by at most 196 mm
// whatever the motion's length, so a proportional allowance alone refuses short lunges.
// Adapted motions also play their whole authored path when adaptedSetupData.maxMoveDistance
// is shorter (skill 1100518: limit 1 m, observed 2.4-2.7 m), so the reach is the larger value.
constexpr double kPlayerMotionReachScale = 1.05;
constexpr double kPlayerMotionReachMarginMm = 300;

// Ground-following motions take their height from the terrain under the path: steps, slopes
// and ledges change it independently of the authored horizontal reach (up to 1.5 m in the
// captures). Movement heights are client-reported everywhere else too; this bound only refuses
// a vertical jump no larger than the start-position tolerance would already allow.
constexpr double kGroundMotionMaxHeightChangeMm = kPositionToleranceMm;

struct PlayerMotionReach {
  double horizontal_mm{};
  double height_change_mm{};
  double allowed_mm{};
  bool within{};
};

PlayerMotionReach player_motion_reach(const Json &authored, const WorldPosition &start,
                                      const WorldPosition &destination) {
  const double authored_reach_mm = std::max(authored.at("max_distance_mm").get<double>(),
                                            authored.at("path_length_mm").get<double>());
  const double dx = static_cast<double>(destination.x_mm) - start.x_mm;
  const double dy = static_cast<double>(destination.y_mm) - start.y_mm;
  const double dz = static_cast<double>(destination.z_mm) - start.z_mm;

  PlayerMotionReach reach;
  reach.horizontal_mm = std::sqrt(dx * dx + dy * dy);
  reach.height_change_mm = std::abs(dz);
  reach.allowed_mm = authored_reach_mm * kPlayerMotionReachScale + kPlayerMotionReachMarginMm;

  if (authored.at("ground_move").get<bool>()) {
    reach.within = reach.horizontal_mm <= reach.allowed_mm &&
                   reach.height_change_mm <= kGroundMotionMaxHeightChangeMm;
  } else {
    // Airborne motions carry their height in the authored path, so bound the full distance.
    reach.within = std::sqrt(dx * dx + dy * dy + dz * dz) <= reach.allowed_mm;
  }
  return reach;
}

// Parses a protobuf JSON 64-bit integer: a decimal string on the wire, or a number from
// synthetic callers. Returns nullopt for anything else, so refusal paths can use it safely.
std::optional<std::int64_t> wire_int64(const Json &value) {
  if (value.is_number_integer()) {
    return value.get<std::int64_t>();
  }
  if (!value.is_string()) {
    return std::nullopt;
  }

  const auto &text = value.get_ref<const std::string &>();
  std::int64_t result{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return result;
}

// A projectile whose catalog row says isServerDriven=false is flown by the caster's own client,
// which announces it with PC2S_ExecuteSpawnProjectileSkillEffectReq (every captured player
// flight: 58 tutorial, 42 + 183 + 10 open-world spawns, each echoing a client request). Every
// catalog Projectile row carries the flag; rows without it (area skill objects and synthetic
// fixtures) keep the server-driven path.
bool is_client_driven_projectile(const Json &definition) {
  const auto &parameters = definition.at("parameters");
  return parameters.contains("isServerDriven") && !parameters.at("isServerDriven").get<bool>();
}

// Milliseconds a projectile needs from start to destination at its catalog speed, or nullopt
// when it has no speed or cannot arrive within its catalog lifetime.
std::optional<int> projectile_travel_ms(const Json &definition, const WorldPosition &start,
                                        const WorldPosition &destination) {
  const double speed_mm_per_second =
      definition.at("parameters").value("speed", 0.0) * kMillimetresPerCatalogCentimetre;
  const int lifetime_ms = definition.at("duration_ms").get<int>();
  if (!(speed_mm_per_second > 0) || lifetime_ms <= 0) {
    return std::nullopt;
  }

  const double distance_mm = std::sqrt(distance_squared(start, destination));
  const double travel_ms = std::ceil(distance_mm / speed_mm_per_second * 1000);
  if (!std::isfinite(travel_ms) || travel_ms > lifetime_ms) {
    return std::nullopt;
  }
  return std::max(1, static_cast<int>(travel_ms));
}

bool is_linear_projectile(const Json &definition) {
  return definition.at("parameters").value("projectileType", std::string{}) ==
         "EX5ProjectileType_Linearity";
}

// A cylinder around the authored hit capsule lets an aimed flight cross any valid body height.
// Profiles without capsule data retain a point target expanded by the projectile's radius.
bool flight_crosses_unit(const Json &definition, const WorldPosition &launch,
                         const WorldPosition &destination, const WorldPosition &unit,
                         const UnitCollisionRadii &capsule) {
  const double dx = static_cast<double>(destination.x_mm) - launch.x_mm;
  const double dy = static_cast<double>(destination.y_mm) - launch.y_mm;
  const double dz = static_cast<double>(destination.z_mm) - launch.z_mm;
  const double length_squared = dx * dx + dy * dy + dz * dz;
  const double ux = static_cast<double>(unit.x_mm) - launch.x_mm;
  const double uy = static_cast<double>(unit.y_mm) - launch.y_mm;
  const double uz = static_cast<double>(unit.z_mm) - launch.z_mm;
  const double fraction = length_squared > 0
                              ? std::clamp((ux * dx + uy * dy + uz * dz) / length_squared, 0.0, 1.0)
                              : 0.0;
  const double radius =
      definition.at("parameters").value("radius", 0.0) * kMillimetresPerCatalogCentimetre +
      capsule.radius_mm;
  return std::hypot(ux - fraction * dx, uy - fraction * dy) <= radius + 1 &&
         std::abs(uz - fraction * dz) <= capsule.half_height_mm + radius + 1;
}

// Moves an anchor by a catalog offset in centimetres expressed in the caster's frame: X forward
// along the yaw, Y to the right (the yaw rotation sense), Z up. A non-finite offset or yaw
// leaves the anchor unchanged.
WorldPosition offset_along_yaw(const WorldPosition &anchor, float yaw_degrees,
                               double forward_cm, double right_cm, double up_cm) {
  const double forward_mm = forward_cm * kMillimetresPerCatalogCentimetre;
  const double right_mm = right_cm * kMillimetresPerCatalogCentimetre;
  const double up_mm = up_cm * kMillimetresPerCatalogCentimetre;
  if (!std::isfinite(forward_mm) || !std::isfinite(right_mm) || !std::isfinite(up_mm) ||
      !std::isfinite(yaw_degrees)) {
    return anchor;
  }

  const double yaw_radians = yaw_degrees * std::numbers::pi / 180.0;
  const double cosine = std::cos(yaw_radians);
  const double sine = std::sin(yaw_radians);
  WorldPosition moved = anchor;
  moved.x_mm =
      static_cast<int>(std::llround(anchor.x_mm + forward_mm * cosine - right_mm * sine));
  moved.y_mm =
      static_cast<int>(std::llround(anchor.y_mm + forward_mm * sine + right_mm * cosine));
  moved.z_mm = static_cast<int>(std::llround(anchor.z_mm + up_mm));
  return moved;
}

// CANDIDATE launch point of a server-driven flight: the caster position moved by the effect's
// catalog spawn offset (X forward, Y right, Z up) rotated by the caster yaw. Captured official
// NPC launches (projectile 140808) sit about 0.66 m ahead and 0.86 m above the caster for a
// catalog offset of 0.5 m and 0.96 m; the client's socket/animation adjustment is not modelled.
WorldPosition projectile_launch_position(const Json &parameters, const WorldPosition &caster,
                                         float yaw_degrees) {
  return offset_along_yaw(caster, yaw_degrees, parameters.value("spawnOffsetX", 0.0),
                          parameters.value("spawnOffsetY", 0.0),
                          parameters.value("spawnOffsetZ", 0.0));
}

// Where a spawnAreaOfEffect places its area. Seven official captures hold 70 own areas from
// effects 160257-160274 and 211804; each Caster-anchored area sits exactly at the caster's
// current position plus targetingPurpose.offsetX centimetres along the cast yaw, within 1 mm.
// offsetY is to the right (the yaw rotation sense): five captured Lightning Storm casts (25
// areas each) match the catalog fan within 0.4 mm, and the opposite sign misses by 1.26 m.
// stickToGround is not modelled: the server has no terrain height, so the area keeps the
// anchor's height.
WorldPosition area_spawn_position(const Json &targeting_purpose, const WorldPosition &anchor,
                                  float yaw_degrees) {
  return offset_along_yaw(anchor, yaw_degrees, targeting_purpose.value("offsetX", 0.0),
                          targeting_purpose.value("offsetY", 0.0), 0.0);
}

// Evaluates one catalog EX5ComparisonType ("value <op> reference"). An unknown operator is not
// satisfied, so a condition the server cannot read never selects its stronger branch.
bool catalog_comparison_holds(double value, double reference, const std::string &comparison) {
  if (comparison == "EX5ComparisonType_LessThanOrEqual") {
    return value <= reference;
  }
  if (comparison == "EX5ComparisonType_LessThan") {
    return value < reference;
  }
  if (comparison == "EX5ComparisonType_GreaterThanOrEqual") {
    return value >= reference;
  }
  if (comparison == "EX5ComparisonType_GreaterThan") {
    return value > reference;
  }
  if (comparison == "EX5ComparisonType_Equal") {
    return value == reference;
  }
  return false;
}

// The key a skill's cooldown is stored under: its shared cooldown group when it has one,
// otherwise its own negated id, so groups and ungrouped skills never collide.
int skill_cooldown_key(int skill_id, const Json &skill) {
  const int group = skill.at("cooldown_group").get<int>();
  return group > 0 ? group : -skill_id;
}

// The ImmuneNoti reaction of a hit whose stagger the target is immune to. The damage effect's
// catalog reactionType without its level suffix (Sway2 -> Sway) must be one of the target's
// immune names; the notification then names the category's first level (Sway1), as 804
// captured official notifications do. Unlevelled reactions (ParryingAttacker) keep their name.
std::optional<std::string> immune_reaction(const Json &parameters,
                                           const std::set<std::string> &immune_names) {
  constexpr std::string_view kReactionPrefix = "EX5ReactionType_";
  const auto reaction = parameters.value("reactionType", std::string{});
  if (!reaction.starts_with(kReactionPrefix)) {
    return std::nullopt;
  }

  std::string category = reaction.substr(kReactionPrefix.size());
  const auto last_letter = category.find_last_not_of("0123456789");
  if (last_letter == std::string::npos) {
    return std::nullopt;
  }
  const bool levelled = last_letter + 1 < category.size();
  category.erase(last_letter + 1);
  if (!immune_names.contains(category)) {
    return std::nullopt;
  }

  std::string notified = std::string(kReactionPrefix) + category;
  if (levelled) {
    notified += "1";
  }
  return notified;
}

// Answers a refused PC2S_ExecuteSpawnProjectileSkillEffectReq with one
// PS2C_SpawnClientProjectileFailNoti per client flight that it named. It runs while a refusal is
// being reported, so it never throws: malformed entries are skipped.
void append_client_projectile_refusals(const Json &request, std::vector<GameplayPacket> &packets) {
  if (!request.is_object() || !request.contains("skill_effect")) {
    return;
  }
  const auto &report = request.at("skill_effect");
  if (!report.is_object() || !report.contains("projectile_datas") ||
      !report.at("projectile_datas").is_array()) {
    return;
  }

  int refused = 0;
  for (const auto &data : report.at("projectile_datas")) {
    if (refused >= kMaximumClientProjectilesPerReport) {
      break;
    }
    if (!data.is_object() || !data.contains("instance_id")) {
      continue;
    }
    const auto instance = wire_int64(data.at("instance_id"));
    if (!instance || *instance <= 0) {
      continue;
    }
    packets.push_back(
        {kSpawnClientProjectileFailNotification, {{"instance_id", std::to_string(*instance)}}});
    ++refused;
  }
}

const Json *by_index(const Json &values, int index) {
  const auto found = std::ranges::find_if(
      values, [index](const auto &value) { return value.at("index") == index; });
  return found == values.end() ? nullptr : &*found;
}

// An NPC skill the server may use as an attack: an NPC skill of catalog type Normal (Dodge and
// Guard are never attacks) whose initial stage executes an effect.
bool is_offensive_effect(const CombatData &data, int effect_id, int depth = 0) {
  const auto effect = data.effects.find(std::to_string(effect_id));
  if (effect == data.effects.end() || depth >= 8) {
    return false;
  }
  const auto kind = effect->value("kind", std::string{});
  if (kind == "damage") {
    return true;
  }
  if (kind == "spawnProjectile") {
    const int id = static_cast<int>(effect->at("parameters").value("projectileId", 0.0));
    const auto projectile = data.projectiles.find(std::to_string(id));
    if (projectile != data.projectiles.end()) {
      for (const auto &hit : projectile->at("hit_effects")) {
        if (is_offensive_effect(data, hit.get<int>(), depth + 1)) {
          return true;
        }
      }
    }
  }
  if (kind == "spawnAreaOfEffect") {
    const int id = static_cast<int>(effect->at("parameters").value("areaOfEffectId", 0.0));
    const auto area = data.areas.find(id);
    if (area != data.areas.end()) {
      const bool offensive_spawn =
          area->second.spawn_effect_id > 0 &&
          is_offensive_effect(data, area->second.spawn_effect_id, depth + 1);
      const bool offensive_despawn =
          area->second.despawn_effect_id > 0 &&
          is_offensive_effect(data, area->second.despawn_effect_id, depth + 1);
      const bool offensive_interval =
          is_offensive_effect(data, area->second.interval_effect_id, depth + 1);
      if (offensive_spawn || offensive_despawn || offensive_interval) {
        return true;
      }
    }
  }
  const int chained = effect->value("chain", 0);
  return chained > 0 && is_offensive_effect(data, chained, depth + 1);
}

bool is_executable_npc_attack(const CombatData &data, const Json &skill) {
  if (!skill.at("npc").get<bool>() || skill.at("type") != "Normal") {
    return false;
  }
  const auto &layer = skill.at("layers").at(0);
  const auto *first_stage = by_index(layer.at("stages"), 0);
  return first_stage && std::ranges::any_of(first_stage->at("events"), [&](const Json &event) {
           return is_offensive_effect(data, event.at("effect_id").get<int>());
         });
}

bool has_ranged_npc_effect(const CombatData &data, const Json &skill) {
  if (skill.value("min_range_mm", 0) > 0) {
    return true;
  }
  for (const auto &stage : skill.at("layers").at(0).at("stages")) {
    for (const auto &event : stage.at("events")) {
      const auto &effect = data.effects.at(std::to_string(event.at("effect_id").get<int>()));
      if (effect.at("kind") == "spawnProjectile" ||
          effect.at("parameters").value("distanceType", std::string{}) ==
              "EX5SkillEffectDistanceType_Ranged") {
        return true;
      }
    }
  }
  return false;
}

std::int64_t npc_cast_range_mm(const NpcAttackOption &attack, const NpcAiDefinition &definition) {
  auto cast_range = attack.range_mm;
  if (!attack.ranged) {
    cast_range = std::min(cast_range, definition.combat_distance_max_mm);
  }
  if (attack.melee_reach_mm) {
    cast_range = std::min(cast_range, *attack.melee_reach_mm);
  }
  return cast_range;
}

// The precomputed attack option of one of the NPC's skills, or none when it is not one of its
// executable attacks.
const NpcAttackOption *find_npc_attack_option(const CombatData &data, int cms_id, int skill_id) {
  for (const auto &attack : data.npc_attack_options(cms_id)) {
    if (attack.skill_id == skill_id) {
      return &attack;
    }
  }
  return nullptr;
}

// First captured executable NPC attack; a monster without one may retaliate when hit but must
// not proactively chase a player it cannot attack.
const Json *usable_npc_attack(const CombatData &data, int cms_id) {
  const auto &attacks = data.npc_attack_options(cms_id);
  if (attacks.empty()) {
    return nullptr;
  }
  return &data.skills.at(attacks.front().skill_id);
}

// Catalog distance band of a skill: Skill.minDistance and Skill.maxDistance (cm x10). Profiles
// exported before min_range_mm existed have no minimum (0).
bool distance_in_skill_band(const NpcAttackOption &attack, double distance_mm) {
  const double minimum = static_cast<double>(attack.min_range_mm);
  const double maximum = static_cast<double>(attack.range_mm) + kNpcPursuitSettleToleranceMm;
  return distance_mm >= minimum && distance_mm <= maximum;
}

// Local selection policy, not a recovered official AI tree: use the least recently used
// offensive action whose range and cooldown permit it. Stable catalog order breaks ties.
template <typename Castable>
std::optional<int>
select_npc_skill(const CombatData &data, int cms_id, const std::map<int, std::int64_t> &ready_ticks,
                 double distance_mm, std::int64_t ticks, const Castable &castable,
                 const std::map<int, std::int64_t> &last_used_ticks = {}) {
  std::optional<int> selected;
  auto oldest_use = std::numeric_limits<std::int64_t>::max();
  for (const auto &attack : data.npc_attack_options(cms_id)) {
    if (!distance_in_skill_band(attack, distance_mm) || !castable(attack)) {
      continue;
    }
    const auto ready = ready_ticks.find(attack.skill_id);
    if (ready != ready_ticks.end() && ticks < ready->second) {
      continue;
    }
    const auto last_use = last_used_ticks.find(attack.skill_id);
    const auto used_at = last_use == last_used_ticks.end()
                             ? std::numeric_limits<std::int64_t>::min()
                             : last_use->second;
    if (!selected || used_at < oldest_use) {
      selected = attack.skill_id;
      oldest_use = used_at;
    }
  }
  return selected;
}

// Pursuit stops at, and the attack gate opens at, this one distance. The bounds are catalog
// data: combat_distance_max_mm is NpcTemplate.combatSuitableDistMax (cm x10), range_mm is
// Skill.maxDistance (x10) and the melee reach comes from the attack's own hit shapes. Taking the
// smallest keeps the monster inside what its attack can reach without a number of our own.
std::int64_t npc_engagement_range_mm(const CombatData &data, int npc_cms_id,
                                     const NpcAiDefinition &definition,
                                     const Json *attack_skill) {
  auto range = definition.combat_distance_max_mm;
  if (!attack_skill) {
    return range;
  }

  const auto skill_range = attack_skill->at("range_mm").get<std::int64_t>();
  if (skill_range > 0) {
    range = std::min(range, skill_range);
  }
  const auto *attack =
      find_npc_attack_option(data, npc_cms_id, attack_skill->at("id").get<int>());
  if (attack && attack->melee_reach_mm) {
    range = std::min(range, *attack->melee_reach_mm);
  }
  return range;
}

std::optional<int> captured_tutorial_combat_phase(int npc_cms_id) {
  // The verified full-session trace uses phase 1230 for these ordinary tutorial enemies.
  // Herald Knight has distinct boss phases; never assign it this one.
  constexpr std::array<int, 10> kCommonPhaseNpcIds{5001010, 5001015, 5001016, 5001082, 5001083,
                                                   5001084, 5001085, 5001528, 5001529, 5001530};
  if (std::ranges::find(kCommonPhaseNpcIds, npc_cms_id) == kCommonPhaseNpcIds.end()) {
    return std::nullopt;
  }
  return kCommonTutorialCombatAiPhase;
}

int stat_value(const Json &stats, int stat) {
  return stats.value(std::to_string(stat), 0);
}

int skill_type_flag(const std::string &type) {
  static const std::map<std::string, int> flags{{"Normal", 1},        {"Guard", 2},
                                                {"Dodge", 4},         {"DrawWeapon", 8},
                                                {"SheathWeapon", 16}, {"SwapWeapon", 32},
                                                {"Parry", 64},        {"Ambush", 128},
                                                {"Execution", 256},   {"Item", 512},
                                                {"Command", 1024},    {"LandingSkill", 8192},
                                                {"Assistant", 32768}, {"WeaponSpecial", 65536}};
  const auto found = flags.find(type);
  return found == flags.end() ? 0 : found->second;
}

// Only a verified cylinder sector is a usable per-event shape. Boxes, spheres and any shape the
// generator marked unverified keep the generic effect targeting.
bool is_verified_cylinder_sector(const Json *shape) {
  return shape && shape->is_object() && shape->value("kind", std::string{}) == "cylinder_sector" &&
         shape->value("verified", false);
}

void validate_event_shape(const Json &shape) {
  if (!shape.is_object() || !shape.contains("kind") || !shape.at("kind").is_string()) {
    throw std::runtime_error("Invalid combat event shape");
  }
  if (!is_verified_cylinder_sector(&shape)) {
    return;
  }
  const auto finite_number = [&shape](const char *key, double minimum, double maximum) {
    const auto found = shape.find(key);
    if (found == shape.end() || !found->is_number()) {
      return false;
    }
    const double value = found->get<double>();
    return std::isfinite(value) && value >= minimum && value <= maximum;
  };
  const auto &offset = shape.contains("offset_mm") ? shape.at("offset_mm") : Json();
  const auto offset_axis = [&offset](const char *key) {
    const auto found = offset.find(key);
    return offset.is_object() && found != offset.end() && found->is_number() &&
           std::isfinite(found->get<double>()) && std::abs(found->get<double>()) <= 1000000;
  };
  if (!finite_number("outer_radius_mm", 0, 1000000) ||
      !finite_number("inner_radius_mm", 0, 1000000) || !finite_number("half_height_mm", 0, 1000000) ||
      !finite_number("angle_degrees", 0, 360) || !finite_number("axis_degrees", -3600, 3600) ||
      shape.at("inner_radius_mm").get<double>() > shape.at("outer_radius_mm").get<double>() ||
      !offset_axis("x") || !offset_axis("y") || !offset_axis("z")) {
    throw std::runtime_error("Invalid combat event cylinder sector");
  }
}

// Extra bearing tolerance that a target capsule of radius_mm adds when seen from distance_mm:
// the sector only has to touch the capsule's edge, not contain its centre.
double capsule_bearing_slack(double radius_mm, double distance_mm) {
  if (radius_mm <= 0) {
    return 0;
  }
  if (distance_mm <= radius_mm) {
    return std::numbers::pi;
  }
  return std::asin(radius_mm / distance_mm);
}

// Verified per-event cylinder sector at the caster pose: the centre is the caster position plus
// offset_mm rotated by the caster yaw (x forward, y left), the axis is the caster yaw plus
// axis_degrees, and the target capsule (target_radius_mm around its centre point) must overlap
// the band between the inner and outer radius within angle_degrees / 2 of the axis. Planar only;
// the vertical test is the effect's existing one.
bool within_event_sector(const Json &event_shape, const WorldPosition &caster, float caster_yaw,
                         const WorldPosition &target, double target_radius_mm) {
  constexpr double kDegreesToRadians = std::numbers::pi / 180;
  const double yaw = caster_yaw * kDegreesToRadians;
  const auto &offset = event_shape.at("offset_mm");
  const double offset_x = offset.at("x").get<double>();
  const double offset_y = offset.at("y").get<double>();
  const double origin_x = caster.x_mm + std::cos(yaw) * offset_x - std::sin(yaw) * offset_y;
  const double origin_y = caster.y_mm + std::sin(yaw) * offset_x + std::cos(yaw) * offset_y;
  const double x = static_cast<double>(target.x_mm) - origin_x;
  const double y = static_cast<double>(target.y_mm) - origin_y;
  const double distance = std::sqrt(x * x + y * y);
  if (distance > event_shape.at("outer_radius_mm").get<double>() + target_radius_mm ||
      distance < event_shape.at("inner_radius_mm").get<double>() - target_radius_mm) {
    return false;
  }
  const double angle = event_shape.at("angle_degrees").get<double>();
  if (angle >= 360) {
    return true;
  }
  const double axis = (caster_yaw + event_shape.at("axis_degrees").get<double>()) * kDegreesToRadians;
  // atan2 of the cross and dot products is the signed bearing difference already in (-pi, pi].
  const double bearing =
      std::atan2(std::cos(axis) * y - std::sin(axis) * x, std::cos(axis) * x + std::sin(axis) * y);
  return std::abs(bearing) <=
         angle * kDegreesToRadians / 2 + capsule_bearing_slack(target_radius_mm, distance);
}

// Tests one effect shape at the caster pose against the target's hit capsule. The official
// collision overlaps the shape with the target's game-hit capsule, so the capsule radius widens
// every horizontal bound and its half height every vertical bound. A zero target_volume tests
// the target's centre point only (profiles without collision tables).
bool within_shape(const Json &effect, const WorldPosition &origin, float yaw,
                  const WorldPosition &target, double fallback_range,
                  const Json *event_shape = nullptr,
                  const UnitCollisionRadii &target_volume = {}) {
  const auto &shape = effect.at("targeting");
  const auto kind = effect.at("targeting_kind").get<std::string>();
  const double target_radius = target_volume.radius_mm;
  const double target_half_height = target_volume.half_height_mm;
  const double x = static_cast<double>(target.x_mm) - origin.x_mm;
  const double y = static_cast<double>(target.y_mm) - origin.y_mm;
  const double z = static_cast<double>(target.z_mm) - origin.z_mm;
  if (is_verified_cylinder_sector(event_shape)) {
    // Same vertical test as the generic cylinder sector, then the event's own planar sector.
    const double radius = shape.value("outerRadius", shape.value("radius", 0.0)) * 10;
    const double half_height = shape.value("halfHeight", radius / 10) * 10;
    return std::abs(z) <= half_height + target_half_height &&
           within_event_sector(*event_shape, origin, yaw, target, target_radius);
  }

  const double radians = (yaw + shape.value("collisionDegree", 0.0)) * std::numbers::pi / 180;
  const double forward = std::cos(radians) * x + std::sin(radians) * y;
  const double side = -std::sin(radians) * x + std::cos(radians) * y;
  if (kind == "shapeBox") {
    return std::abs(forward) <= shape.value("extentX", 0.0) * 10 + target_radius &&
           std::abs(side) <= shape.value("extentY", 0.0) * 10 + target_radius &&
           std::abs(z) <= shape.value("extentZ", 0.0) * 10 + target_half_height;
  }
  if (kind == "shapeSphere") {
    const double radius = shape.value("radius", shape.value("outerRadius", 0.0)) *
                          kMillimetresPerCatalogCentimetre;
    // Distance from the sphere centre to the capsule's inner segment, which lies
    // half_height - radius above and below the target's centre point.
    const double segment_half_length = std::max(0.0, target_half_height - target_radius);
    const double vertical_gap = std::max(0.0, std::abs(z) - segment_half_length);
    const double reach = radius + target_radius;
    return x * x + y * y + vertical_gap * vertical_gap <= reach * reach;
  }
  if (kind == "shapeCylinderSector") {
    const double radius = shape.value("outerRadius", shape.value("radius", 0.0)) * 10;
    const double inner = std::max(0.0, shape.value("innerRadius", 0.0) * 10 - target_radius);
    const double outer = radius + target_radius;
    const double planar = x * x + y * y;
    if (planar > outer * outer || planar < inner * inner ||
        std::abs(z) > shape.value("halfHeight", radius / 10) * 10 + target_half_height) {
      return false;
    }
    const double angle = shape.value("angle", 360.0);
    const double slack = capsule_bearing_slack(target_radius, std::sqrt(planar));
    return angle >= 360 ||
           std::abs(std::atan2(side, forward)) <= angle * std::numbers::pi / 360 + slack;
  }
  if (kind == "shapeCylinder") {
    // A full cylinder around the origin. It previously fell through to the skill-range fallback
    // below, so melee cylinders of 50 m skills hit players 50 m away.
    const double radius = shape.value("radius", shape.value("outerRadius", 0.0)) *
                          kMillimetresPerCatalogCentimetre;
    const double half_height = shape.value("halfHeight", radius / kMillimetresPerCatalogCentimetre) *
                               kMillimetresPerCatalogCentimetre;
    const double reach = radius + target_radius;
    return x * x + y * y <= reach * reach && std::abs(z) <= half_height + target_half_height;
  }
  return distance_squared(origin, target) <= fallback_range * fallback_range;
}

// ---- Attack root motion data (stored only; nothing here moves a unit yet). ----

constexpr std::size_t kMaxRootMotionSamples = 4096;
constexpr double kMaxRootMotionDistanceMm = 1000000.0;
constexpr double kMaxRootMotionCoordinateMm = 10000000.0;
constexpr int kMaxRootMotionTimeMs = 300000;

[[noreturn]] void reject_root_motion(const char *what, const char *field) {
  throw std::runtime_error(std::string("Invalid ") + what + ": " + field);
}

double bounded_number(const Json &object, const char *key, double minimum, double maximum,
                      const char *what) {
  const auto found = object.is_object() ? object.find(key) : object.end();
  if (!object.is_object() || found == object.end() || !found->is_number()) {
    reject_root_motion(what, key);
  }
  const double value = found->get<double>();
  if (!std::isfinite(value) || value < minimum || value > maximum) {
    reject_root_motion(what, key);
  }
  return value;
}

std::string required_string(const Json &object, const char *key, const char *what) {
  const auto found = object.find(key);
  if (found == object.end() || !found->is_string() || found->get<std::string>().empty()) {
    reject_root_motion(what, key);
  }
  return found->get<std::string>();
}

bool required_bool(const Json &object, const char *key, const char *what) {
  const auto found = object.find(key);
  if (found == object.end() || !found->is_boolean()) {
    reject_root_motion(what, key);
  }
  return found->get<bool>();
}

RootMotionWindow parse_root_motion(const Json &block) {
  constexpr const char *kWhat = "combat root motion";
  if (!block.is_object()) {
    reject_root_motion(kWhat, "block");
  }
  RootMotionWindow window;
  window.type = required_string(block, "type", kWhat);
  window.window_start_ms =
      static_cast<int>(bounded_number(block, "window_start_ms", 0, kMaxRootMotionTimeMs, kWhat));
  window.window_end_ms = static_cast<int>(
      bounded_number(block, "window_end_ms", window.window_start_ms, kMaxRootMotionTimeMs, kWhat));
  window.max_move_distance_mm =
      bounded_number(block, "max_move_distance_mm", 0, kMaxRootMotionDistanceMm, kWhat);
  window.offset_distance_mm =
      bounded_number(block, "offset_distance_mm", 0, kMaxRootMotionDistanceMm, kWhat);
  window.offset_basis_pos_type = required_string(block, "offset_basis_pos_type", kWhat);
  window.offset_basis_dir_type = required_string(block, "offset_basis_dir_type", kWhat);
  window.offset_direction_degrees =
      bounded_number(block, "offset_direction_degrees", -3600, 3600, kWhat);
  window.look_at_dir_type = required_string(block, "look_at_dir_type", kWhat);
  window.rotation_angular_speed_degrees =
      bounded_number(block, "rotation_angular_speed_degrees", 0, 1000000, kWhat);
  window.using_scale = required_bool(block, "using_scale", kWhat);
  window.ground_move = required_bool(block, "ground_move", kWhat);
  window.ignore_collision = required_bool(block, "ignore_collision", kWhat);
  window.end_ground_move = required_bool(block, "end_ground_move", kWhat);

  const auto position_data = block.find("position_data");
  if (position_data == block.end() || !position_data->is_object()) {
    reject_root_motion(kWhat, "position_data");
  }
  // Sample times are absolute montage milliseconds, not relative to the window start.
  window.time_base = required_string(*position_data, "time_base", kWhat);
  if (window.time_base != "montage_ms") {
    reject_root_motion(kWhat, "position_data.time_base");
  }
  const auto samples = position_data->find("samples");
  if (samples == position_data->end() || !samples->is_array() || samples->empty() ||
      samples->size() > kMaxRootMotionSamples) {
    reject_root_motion(kWhat, "position_data.samples");
  }
  if (position_data->contains("sample_count") &&
      position_data->at("sample_count") != static_cast<std::int64_t>(samples->size())) {
    reject_root_motion(kWhat, "position_data.sample_count");
  }
  for (const auto &sample : *samples) {
    RootMotionSample parsed;
    parsed.elapsed_ms = static_cast<std::int64_t>(
        bounded_number(sample, "elapsed_ms", 0, kMaxRootMotionTimeMs, kWhat));
    parsed.x_mm =
        bounded_number(sample, "x_mm", -kMaxRootMotionCoordinateMm, kMaxRootMotionCoordinateMm, kWhat);
    parsed.y_mm =
        bounded_number(sample, "y_mm", -kMaxRootMotionCoordinateMm, kMaxRootMotionCoordinateMm, kWhat);
    parsed.z_mm =
        bounded_number(sample, "z_mm", -kMaxRootMotionCoordinateMm, kMaxRootMotionCoordinateMm, kWhat);
    if (!window.samples.empty() && parsed.elapsed_ms <= window.samples.back().elapsed_ms) {
      reject_root_motion(kWhat, "position_data.samples order");
    }
    window.samples.push_back(parsed);
  }
  return window;
}

void validate_player_motion(const Json &stage, int duration) {
  const auto bounded_time = [duration](const Json &value) {
    return value.is_number_integer() && value.get<std::int64_t>() >= 0 &&
           value.get<std::int64_t>() <= duration;
  };
  if (stage.contains("player_motion")) {
    const auto &motion = stage.at("player_motion");
    if (!motion.is_object() || !bounded_time(motion.at("start_ms")) ||
        !bounded_time(motion.at("end_ms")) || motion.at("start_ms") > motion.at("end_ms") ||
        !motion.at("ground_move").is_boolean() || !motion.at("using_scale").is_boolean()) {
      throw std::runtime_error("Invalid player root-motion window");
    }
    for (const auto *field : {"max_distance_mm", "path_length_mm"}) {
      const auto value = motion.at(field).get<double>();
      if (!std::isfinite(value) || value < 0 || value > 1000000) {
        throw std::runtime_error("Invalid player root-motion distance");
      }
    }
  }
  if (stage.contains("adapted_rotations")) {
    const auto &windows = stage.at("adapted_rotations");
    if (!windows.is_array() || windows.size() > 256) {
      throw std::runtime_error("Invalid player rotation windows");
    }
    for (const auto &window : windows) {
      if (!bounded_time(window.at("startTime")) || !bounded_time(window.at("endTime")) ||
          window.at("startTime") > window.at("endTime")) {
        throw std::runtime_error("Invalid player rotation deadline");
      }
    }
  }
}

void parse_unit_collision_table(const Json &document, const char *table,
                                std::map<int, UnitCollisionRadii> &destination) {
  const auto found = document.find(table);
  if (found == document.end()) {
    return;
  }
  if (!found->is_object() || found->size() > 4096) {
    reject_root_motion("combat collision table", table);
  }
  for (const auto &[key, row] : found->items()) {
    int id = 0;
    try {
      std::size_t consumed = 0;
      id = std::stoi(key, &consumed);
      if (consumed != key.size() || id <= 0) {
        reject_root_motion("combat collision table", table);
      }
    } catch (const std::logic_error &) {
      reject_root_motion("combat collision table", table);
    }
    UnitCollisionRadii radii;
    radii.radius_mm = bounded_number(row, "game_hit_capsule_radius_mm", 1, 1000000, table);
    radii.half_height_mm = bounded_number(row, "game_hit_capsule_half_height_mm", 1, 1000000, table);
    if (!destination.emplace(id, radii).second) {
      reject_root_motion("combat collision table", table);
    }
  }
}

// Wraps an angle in degrees into (-180, 180].
double wrapped_degrees(double degrees) {
  double wrapped = std::fmod(degrees, 360.0);
  if (wrapped > 180.0) {
    wrapped -= 360.0;
  } else if (wrapped <= -180.0) {
    wrapped += 360.0;
  }
  return wrapped;
}

// Lunge scope: the exact AdaptedChaseTarget semantics read from the real 190100400 block
// (offset direction 180 = back off from the target, look at the target, ground move, collision
// honoured, ground-adjusted end) with a window that lies inside the stage and whose authored
// path is a forward-only line ending at the window end. Any other setting is left unmoved
// because the server does not implement its meaning.
//
// Delayed windows (start > 0) are a Candidate extension since 2026-10-27: 394 NPC attacks,
// for example the 100100310 humanoid swing and the 194300500 charge, dash only after a wind-up.
// Ignoring them left the server-side monster where it stood while the attack visibly reached
// the player, so every such hit missed. Their exact client start timing is not captured.
// Offset direction -180 names the same back-off direction as 180.
bool supported_lunge_window(const RootMotionWindow &window, int stage_duration_ms) {
  const auto &samples = window.samples;
  if (window.type != "EX5RootMotionType_AdaptedChaseTarget" ||
      window.offset_basis_pos_type != "EX5AdaptedMotionOffsetBasisPos_TargetPos" ||
      window.offset_basis_dir_type != "EX5AdaptedMotionOffsetBasisDir_CasterToTargetDir" ||
      wrapped_degrees(window.offset_direction_degrees) != 180.0 ||
      window.look_at_dir_type != "EX5AdaptedMotionLookAtDir_ToTarget" || !window.ground_move ||
      window.ignore_collision || !window.end_ground_move ||
      window.window_end_ms > stage_duration_ms ||
      !window.using_scale || window.window_start_ms < 0 ||
      window.window_end_ms <= window.window_start_ms ||
      samples.size() < 2 || samples.front().elapsed_ms != window.window_start_ms ||
      samples.back().elapsed_ms != window.window_end_ms || samples.front().x_mm != 0.0 ||
      samples.back().x_mm <= 0.0) {
    return false;
  }
  for (std::size_t index = 0; index < samples.size(); ++index) {
    if (samples[index].y_mm != 0.0 || samples[index].z_mm != 0.0 ||
        (index > 0 && samples[index].x_mm < samples[index - 1].x_mm)) {
      return false;
    }
  }
  return true;
}

// The server does not track the player's model, so the hit radius comes from the profile's
// player_collision table and is used only when every playable model agrees on it.
std::optional<double> common_player_radius(const CombatData &data) {
  if (data.player_collision.empty()) {
    return std::nullopt;
  }
  const double radius = data.player_collision.begin()->second.radius_mm;
  for (const auto &[model, row] : data.player_collision) {
    if (row.radius_mm != radius) {
      return std::nullopt;
    }
  }
  return radius;
}

// Hit capsule of the player for shape tests. The server does not track the character's model,
// so the smallest catalog capsule is used: it never widens a hit beyond what any playable model
// would receive. A profile without the table yields a zero volume (centre-point tests).
UnitCollisionRadii player_hit_volume(const CombatData &data) {
  if (data.player_collision.empty()) {
    return {};
  }

  UnitCollisionRadii volume = data.player_collision.begin()->second;
  for (const auto &[model, row] : data.player_collision) {
    volume.radius_mm = std::min(volume.radius_mm, row.radius_mm);
    volume.half_height_mm = std::min(volume.half_height_mm, row.half_height_mm);
  }
  return volume;
}

UnitCollisionRadii npc_hit_volume(const CombatData &data, int npc_cms_id) {
  const auto found = data.npc_collision.find(npc_cms_id);
  if (found == data.npc_collision.end()) {
    return {};
  }
  return found->second;
}

// Resolution of the forward-reach scan below. Ten millimetres is far finer than any authored
// shape (the smallest exported radii are tens of centimetres) and keeps the load-time scan cheap.
constexpr double kReachScanStepMm = 10.0;

// Outer bound of a shape's reach along the caster's facing, before the exact scan. None when the
// event's targeting is not a caster-centred shape (missing targeting or a reference target).
std::optional<double> damage_shape_reach_bound_mm(const Json &effect, const Json *event_shape,
                                                  const UnitCollisionRadii &target_volume) {
  // within_shape reads the effect's targeting even when an event shape replaces its planar test.
  const auto targeting = effect.find("targeting");
  const auto targeting_kind = effect.find("targeting_kind");
  if (targeting == effect.end() || !targeting->is_object() || targeting_kind == effect.end() ||
      !targeting_kind->is_string()) {
    return std::nullopt;
  }

  if (is_verified_cylinder_sector(event_shape)) {
    const auto &offset = event_shape->at("offset_mm");
    const double offset_mm =
        std::hypot(offset.at("x").get<double>(), offset.at("y").get<double>());
    return offset_mm + event_shape->at("outer_radius_mm").get<double>() +
           target_volume.radius_mm;
  }

  const auto &shape = *targeting;
  const auto &kind = targeting_kind->get_ref<const std::string &>();
  double extent_cm = 0;
  if (kind == "shapeBox") {
    // A rotated box can reach as far as its half diagonal.
    extent_cm = std::hypot(shape.value("extentX", 0.0), shape.value("extentY", 0.0));
  } else if (kind == "shapeCylinderSector") {
    extent_cm = shape.value("outerRadius", shape.value("radius", 0.0));
  } else if (kind == "shapeCylinder" || kind == "shapeSphere") {
    extent_cm = shape.value("radius", shape.value("outerRadius", 0.0));
  } else {
    return std::nullopt;
  }
  return extent_cm * kMillimetresPerCatalogCentimetre + target_volume.radius_mm;
}

// Furthest distance straight ahead of the caster at which one direct damage event still touches
// a target capsule standing on the ground plane of the caster. It reuses within_shape itself, so
// lateral offsets, rotated sectors and inner radii are judged exactly as a real hit is.
std::optional<double> damage_event_forward_reach_mm(const Json &effect, const Json *event_shape,
                                                    const UnitCollisionRadii &target_volume) {
  const auto bound = damage_shape_reach_bound_mm(effect, event_shape, target_volume);
  if (!bound) {
    return std::nullopt;
  }

  // The caster faces +x (yaw 0) from the origin, the way an NPC faces its target when it casts.
  const WorldPosition caster{};
  for (double distance_mm = std::ceil(*bound); distance_mm >= 0; distance_mm -= kReachScanStepMm) {
    WorldPosition target{};
    target.x_mm = static_cast<int>(distance_mm);
    if (within_shape(effect, caster, 0.0F, target, 0, event_shape, target_volume)) {
      return distance_mm;
    }
  }
  return 0.0;
}

// Furthest caster-to-player centre distance at which a skill delivered only by caster-centred
// damage shapes still touches a standing player's capsule. Includes the travel of a supported
// attack lunge, which closes distance before the hit. None for skills with a projectile, an
// area or a damage path that is not such a shape: their reach is not bounded here.
std::optional<double> npc_melee_reach_mm(const CombatData &data, const Json &skill,
                                         const UnitCollisionRadii &player_volume) {
  const auto &layer = skill.at("layers").at(0);
  std::optional<double> shape_reach;
  for (const auto &stage : layer.at("stages")) {
    for (const auto &event : stage.at("events")) {
      const auto &effect = data.effects.at(std::to_string(event.at("effect_id").get<int>()));
      const auto kind = effect.value("kind", std::string{});
      if (kind == "spawnProjectile" || kind == "spawnAreaOfEffect") {
        return std::nullopt;
      }

      const auto targeting = effect.find("targeting");
      const bool self_targeted =
          targeting != effect.end() && targeting->is_object() &&
          targeting->value("refType", std::string{}) == "EX5ReferenceTargetType_Self";
      if (kind != "damage" || self_targeted) {
        continue;
      }

      const Json *event_shape = event.contains("shape") ? &event.at("shape") : nullptr;
      const auto event_reach = damage_event_forward_reach_mm(effect, event_shape, player_volume);
      if (!event_reach) {
        return std::nullopt;
      }
      shape_reach = std::max(shape_reach.value_or(0.0), *event_reach);
    }
  }
  if (!shape_reach) {
    return std::nullopt;
  }

  double reach = *shape_reach;
  const auto *first_stage = by_index(layer.at("stages"), 0);
  const auto lunge = data.root_motion.find(
      std::make_tuple(skill.at("id").get<int>(), layer.at("index").get<int>(), 0));
  if (first_stage && lunge != data.root_motion.end() &&
      supported_lunge_window(lunge->second, first_stage->at("duration_ms").get<int>())) {
    reach += lunge->second.max_move_distance_mm;
  }
  return reach;
}

void append(std::vector<GameplayPacket> &packets, std::vector<GameplayPacket> more) {
  packets.insert(packets.end(), std::make_move_iterator(more.begin()),
                 std::make_move_iterator(more.end()));
}

// Stage request fields are read without throwing, so that a malformed request
// for the current action is still refused and ends that action. Instance 0 is
// never allocated; proto3 JSON omits a zero instance or stage index.
std::uint32_t stage_request_instance(const Json &request) {
  const auto found = request.find("skill_instance_id");
  if (found == request.end() || !found->is_number_integer()) {
    return 0;
  }

  const auto instance = found->get<std::int64_t>();
  if (instance <= 0 || instance > std::numeric_limits<std::uint32_t>::max()) {
    return 0;
  }
  return static_cast<std::uint32_t>(instance);
}

// Returns -1 for a malformed or out-of-range index, which is always refused.
int requested_stage_index(const Json &request) {
  const auto found = request.find("stage_index");
  if (found == request.end()) {
    return 0;
  }
  if (!found->is_number_integer()) {
    return -1;
  }

  const auto stage_index = found->get<std::int64_t>();
  if (stage_index < 0 || stage_index > std::numeric_limits<int>::max()) {
    return -1;
  }
  return static_cast<int>(stage_index);
}

// An unknown enum value decodes as a number and is treated as unsupported.
std::string stage_transition_reason(const Json &request) {
  const auto found = request.find("reason");
  if (found == request.end()) {
    return std::string(kDefaultTransitionReason);
  }
  if (!found->is_string()) {
    return {};
  }
  return found->get<std::string>();
}

const char *stage_refusal_end_reason(std::string_view reason) {
  if (reason == kGroundLandingReason) {
    return "EX5EndSkillReason_LandingSkillFail";
  }
  return "EX5EndSkillReason_Default";
}

bool within_transition_window(const Json &notify, std::int64_t elapsed_ms) {
  const auto start_ms = notify.at("startTime").get<std::int64_t>();
  const auto end_ms = notify.at("endTime").get<std::int64_t>();
  return elapsed_ms >= start_ms - kStageTransitionToleranceMs &&
         elapsed_ms <= end_ms + kStageTransitionToleranceMs;
}

// The default transition is server-scheduled at the end of the stage. A client
// report may only name that stage; an early report is deferred to the deadline.
StageRequestDecision decide_default_transition(const Json &stage, int requested_stage,
                                               std::int64_t elapsed_ms) {
  const int default_next_stage = stage.at("next_stage");
  if (default_next_stage < 0 || requested_stage != default_next_stage) {
    return {StageRequestOutcome::Reject, "Skill_InvalidStage"};
  }

  const auto duration_ms = stage.at("duration_ms").get<std::int64_t>();
  if (elapsed_ms + kStageTransitionToleranceMs < duration_ms) {
    return {StageRequestOutcome::Defer};
  }
  return {StageRequestOutcome::Apply};
}

StageRequestDecision decide_input_condition(const Json &stage, int requested_stage,
                                            std::int64_t elapsed_ms) {
  bool stage_is_named = false;
  for (const auto &notify : stage.at("notifies")) {
    if (notify.at("notifyType") != "input_condition" ||
        notify.at("inputCondition").at("nextStageIndex") != requested_stage) {
      continue;
    }

    stage_is_named = true;
    if (within_transition_window(notify, elapsed_ms)) {
      return {StageRequestOutcome::Apply};
    }
  }

  if (stage_is_named) {
    return {StageRequestOutcome::Reject, "Skill_InputConditionInvalidTick"};
  }
  return {StageRequestOutcome::Reject, "Skill_InvalidStage"};
}

StageRequestDecision decide_action_stage_condition(const Json &stage,
                                                   const ActionStageConditionReason &condition,
                                                   int requested_stage, std::int64_t elapsed_ms) {
  bool condition_exists = false;
  bool stage_is_named = false;
  for (const auto &notify : stage.at("notifies")) {
    if (notify.at("notifyType") != "action_stage_condition") {
      continue;
    }

    for (const auto &entry : notify.at("actionStageCondition").at("datas")) {
      if (entry.value("eventType", std::string()) != condition.condition_type) {
        continue;
      }

      condition_exists = true;
      if (entry.at("transitActionStageIndex") != requested_stage) {
        continue;
      }

      stage_is_named = true;
      if (within_transition_window(notify, elapsed_ms)) {
        return {StageRequestOutcome::Apply};
      }
    }
  }

  if (!condition_exists) {
    return {StageRequestOutcome::Reject, condition.unable_code};
  }
  if (!stage_is_named) {
    return {StageRequestOutcome::Reject, condition.invalid_stage_code};
  }
  return {StageRequestOutcome::Reject, condition.invalid_tick_code};
}

// Each reason may only use the notify type that authorizes it. For example, a
// landing skill's stage 0 has an action_stage_condition and no input_condition.
StageRequestDecision decide_stage_request(const Json &stage, std::string_view reason,
                                          int requested_stage, std::int64_t elapsed_ms) {
  if (reason == kDefaultTransitionReason) {
    return decide_default_transition(stage, requested_stage, elapsed_ms);
  }
  if (reason == kInputConditionReason) {
    return decide_input_condition(stage, requested_stage, elapsed_ms);
  }
  for (const auto &condition : kActionStageConditionReasons) {
    if (reason == condition.reason) {
      return decide_action_stage_condition(stage, condition, requested_stage, elapsed_ms);
    }
  }

  // skill_cost_condition, chase, air_drop and teleport have no extracted
  // transition data in the tutorial profile.
  return {StageRequestOutcome::Reject, "Skill_InvalidStage"};
}

} // namespace

std::optional<int> select_npc_attack_skill(const CombatData &data, int cms_id,
                                           const std::map<int, std::int64_t> &ready_ticks,
                                           double distance_mm, std::int64_t ticks,
                                           const std::map<int, std::int64_t> &last_used_ticks) {
  return select_npc_skill(
      data, cms_id, ready_ticks, distance_mm, ticks, [](const NpcAttackOption &) { return true; },
      last_used_ticks);
}

WorldPosition LungeMotion::pose_at(std::int64_t ticks) const {
  const auto &samples = window->samples;
  const double first_ms = static_cast<double>(samples.front().elapsed_ms);
  const double last_ms = static_cast<double>(samples.back().elapsed_ms);
  const double montage_ms = static_cast<double>(ticks - start_tick) / kTicksPerMillisecond +
                            static_cast<double>(window->window_start_ms);
  const double time = std::clamp(montage_ms, first_ms, last_ms);
  const auto after = std::ranges::upper_bound(
      samples, time, std::less<>{},
      [](const RootMotionSample &sample) { return static_cast<double>(sample.elapsed_ms); });
  double forward = samples.back().x_mm;
  if (after == samples.begin()) {
    forward = samples.front().x_mm;
  } else if (after != samples.end()) {
    const auto &before_sample = *(after - 1);
    const double span = static_cast<double>(after->elapsed_ms - before_sample.elapsed_ms);
    const double fraction = (time - static_cast<double>(before_sample.elapsed_ms)) / span;
    forward = before_sample.x_mm + (after->x_mm - before_sample.x_mm) * fraction;
  }
  const double progress = forward / samples.back().x_mm;
  WorldPosition pose = start;
  pose.x_mm = static_cast<int>(std::llround(start.x_mm + (destination.x_mm - start.x_mm) * progress));
  pose.y_mm = static_cast<int>(std::llround(start.y_mm + (destination.y_mm - start.y_mm) * progress));
  pose.z_mm = static_cast<int>(std::llround(start.z_mm + (destination.z_mm - start.z_mm) * progress));
  pose.yaw = destination.yaw;
  return pose;
}

namespace {

// Parses a positive decimal CMS id used as a JSON object key.
int parse_cms_id_key(const std::string &text, const char *table) {
  int value{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value <= 0) {
    throw std::runtime_error(std::string("Invalid CMS id in combat ") + table + ": " + text);
  }
  return value;
}

// Resolves a catalog MaintainStat.statType name (for example "MaxSP") to its EX5StatType value.
int resolve_stat_type(const std::string &name) {
  x5::cms::EX5StatType value{};
  if (!x5::cms::EX5StatType_Parse("EX5StatType_" + name, &value) ||
      value == x5::cms::EX5StatType_None) {
    throw std::runtime_error("Unknown status stat modifier type: " + name);
  }
  return static_cast<int>(value);
}

// Reads the optional `stat_modifiers` of one status effect row. Only the flat catalog statAdd
// is kept: the unit of statMul is not established, so multipliers are not applied.
// Reads the optional `stat_modifiers` of one status effect row: the catalog statAdd and the
// statMul multiplier (1/1000 percent, see combat_rules::stat_multiplier_delta).
std::vector<StatusStatModifier> parse_status_stat_modifiers(const std::string &status_id,
                                                            const Json &row) {
  const auto rows = row.value("stat_modifiers", Json::array());
  if (!rows.is_array() || rows.size() > kMaxStatModifiersPerStatus) {
    throw std::runtime_error("Invalid stat modifiers for status effect " + status_id);
  }

  const auto bounded = [&status_id](const Json &entry, const char *key) {
    const auto value = entry.value(key, Json(0));
    if (!value.is_number_integer()) {
      throw std::runtime_error("Malformed stat modifier for status effect " + status_id);
    }
    const auto number = value.get<std::int64_t>();
    if (number < -kMaxStatModifierMagnitude || number > kMaxStatModifierMagnitude) {
      throw std::runtime_error("Stat modifier out of range for status effect " + status_id);
    }
    return static_cast<int>(number);
  };

  std::vector<StatusStatModifier> modifiers;
  for (const auto &entry : rows) {
    if (!entry.is_object() || !entry.contains("stat") || !entry.at("stat").is_string() ||
        !entry.contains("add")) {
      throw std::runtime_error("Malformed stat modifier for status effect " + status_id);
    }
    const int add = bounded(entry, "add");
    const int mul = bounded(entry, "mul");
    const int stat_type = resolve_stat_type(entry.at("stat").get<std::string>());
    if (add != 0 || mul != 0) {
      modifiers.push_back({stat_type, add, mul});
    }
  }
  return modifiers;
}

// The abnormal types a status's Immune row names; other immune kinds are not simulated.
std::set<combat_rules::AbnormalType> parse_abnormal_immunities(const std::string &status_id,
                                                              const Json &row) {
  const auto names = row.value("immune_types", Json::array());
  if (!names.is_array()) {
    throw std::runtime_error("Invalid immune types for status effect " + status_id);
  }

  std::set<combat_rules::AbnormalType> immunities;
  for (const auto &name : names) {
    if (!name.is_string()) {
      throw std::runtime_error("Invalid immune type for status effect " + status_id);
    }
    const auto type = combat_rules::abnormal_type_from_name(name.get<std::string>());
    if (type) {
      immunities.insert(*type);
    }
  }
  return immunities;
}

// Optional profile key `immunities`: Immune row id -> its EX5ImmuneType names.
std::map<int, ImmunityDefinition> parse_immunities(const Json &table) {
  if (!table.is_object() || table.size() > kMaxImmunityRows) {
    throw std::runtime_error("Invalid combat immunity table");
  }

  std::map<int, ImmunityDefinition> immunities;
  for (const auto &[id, names] : table.items()) {
    if (!names.is_array() || names.size() > 64) {
      throw std::runtime_error("Invalid combat immunity row " + id);
    }

    ImmunityDefinition definition;
    for (const auto &name : names) {
      if (!name.is_string() || name.get<std::string>().empty()) {
        throw std::runtime_error("Invalid combat immunity name in row " + id);
      }
      definition.names.insert(name.get<std::string>());
    }
    definition.dodge = definition.names.contains("Dodge");
    definition.damage = definition.names.contains("Damage");
    immunities.emplace(parse_cms_id_key(id, "immunities"), std::move(definition));
  }
  return immunities;
}

// Optional profile key `shields`: Shield row id -> lifetime, amount and granted immunity.
std::map<int, ShieldDefinition> parse_shields(const Json &table,
                                              const std::map<int, ImmunityDefinition> &immunities) {
  if (!table.is_object() || table.size() > kMaxShieldRows) {
    throw std::runtime_error("Invalid combat shield table");
  }

  std::map<int, ShieldDefinition> shields;
  for (const auto &[id, row] : table.items()) {
    const auto integer = [&id, &row](const char *key, int maximum) {
      if (!row.is_object() || !row.contains(key) || !row.at(key).is_number_integer() ||
          row.at(key).get<std::int64_t>() < 0 || row.at(key).get<std::int64_t>() > maximum) {
        throw std::runtime_error(std::string("Invalid combat shield ") + id + " field " + key);
      }
      return row.at(key).get<int>();
    };

    ShieldDefinition definition;
    definition.duration_ms = integer("duration_ms", kMaxStatusDurationMs);
    definition.amount = integer("amount", kMaxShieldAmount);
    definition.hp_per_remain_milli = integer("hp_per_remain_milli", kMaxPerMille);
    definition.hp_per_total_milli = integer("hp_per_total_milli", kMaxPerMille);

    const auto immune_text = row.value("immune_id", std::string{});
    if (!immune_text.empty()) {
      definition.immune_id = parse_cms_id_key(immune_text, "shields");
      if (!immunities.contains(definition.immune_id)) {
        throw std::runtime_error("Combat shield " + id + " names a missing Immune row");
      }
    }
    shields.emplace(parse_cms_id_key(id, "shields"), definition);
  }
  return shields;
}

std::map<int, AreaDefinition> parse_areas(const Json &table, const Json &effects) {
  if (!table.is_object()) {
    throw std::runtime_error("Invalid combat area table");
  }

  std::map<int, AreaDefinition> areas;
  for (const auto &[id, row] : table.items()) {
    const auto integer = [&id, &row](const char *key, int minimum, int maximum) {
      if (!row.is_object() || !row.contains(key) || !row.at(key).is_number_integer() ||
          row.at(key).get<std::int64_t>() < minimum || row.at(key).get<std::int64_t>() > maximum) {
        throw std::runtime_error(std::string("Invalid combat area ") + id + " field " + key);
      }
      return row.at(key).get<int>();
    };

    AreaDefinition area;
    area.duration_ms = integer("duration_ms", 1, kMaxAreaDurationMs);
    area.interval_ms = integer("interval_ms", 1, kMaxAreaDurationMs);
    area.interval_effect_id = integer("interval_effect_id", 1, std::numeric_limits<int>::max());
    area.shape = row.value("shape", std::string{"cylinder"});
    if (area.shape != "cylinder" && area.shape != "box") {
      throw std::runtime_error("Unsupported combat area shape for " + id);
    }
    area.radius_mm = integer("radius_mm", area.shape == "box" ? 0 : 1, kMaxAreaRadiusMm);
    if (area.shape == "box") {
      area.extent_x_mm = integer("extent_x_mm", 1, kMaxAreaRadiusMm);
      area.extent_y_mm = integer("extent_y_mm", 1, kMaxAreaRadiusMm);
    }
    area.half_height_mm = integer("half_height_mm", 1, kMaxAreaRadiusMm);
    area.max_targets = integer("max_targets", 0, 64);
    if (!effects.contains(std::to_string(area.interval_effect_id))) {
      throw std::runtime_error("Combat area " + id + " names an unexported interval effect");
    }

    // Optional: only areas with an authored spawn hit carry it.
    if (row.contains("spawn_effect_id")) {
      area.spawn_effect_id = integer("spawn_effect_id", 1, std::numeric_limits<int>::max());
      if (!effects.contains(std::to_string(area.spawn_effect_id))) {
        throw std::runtime_error("Combat area " + id + " names an unexported spawn effect");
      }
    }
    // Optional: only areas with an authored lifetime-end hit carry it.
    if (row.contains("despawn_effect_id")) {
      area.despawn_effect_id = integer("despawn_effect_id", 1, std::numeric_limits<int>::max());
      if (!effects.contains(std::to_string(area.despawn_effect_id))) {
        throw std::runtime_error("Combat area " + id + " names an unexported despawn effect");
      }
    }
    areas.emplace(parse_cms_id_key(id, "areas"), area);
  }
  return areas;
}

std::map<int, std::vector<int>> parse_item_effect_lists(const Json &table, const Json &effects) {
  if (!table.is_object() || table.size() > kMaxItemEffectLists) {
    throw std::runtime_error("Invalid combat item effect lists");
  }

  std::map<int, std::vector<int>> lists;
  for (const auto &[item_text, effect_ids] : table.items()) {
    if (!effect_ids.is_array() || effect_ids.empty() ||
        effect_ids.size() > kMaxItemEffectsPerList) {
      throw std::runtime_error("Invalid effect list for item " + item_text);
    }
    std::vector<int> list;
    for (const auto &effect_id : effect_ids) {
      if (!effect_id.is_number_integer() ||
          !effects.contains(std::to_string(effect_id.get<int>()))) {
        throw std::runtime_error("Item " + item_text + " names an unexported effect");
      }
      list.push_back(effect_id.get<int>());
    }
    lists.emplace(parse_cms_id_key(item_text, "item_effect_lists"), std::move(list));
  }
  return lists;
}

// Reads the optional item -> item-skill table. Every entry must name a loaded Item-type skill.
std::map<int, int> parse_item_skills(const Json &table, const std::map<int, Json> &skills) {
  if (!table.is_object() || table.size() > kMaxItemSkills) {
    throw std::runtime_error("Invalid combat item skill table");
  }

  std::map<int, int> item_skills;
  for (const auto &[item_text, skill_value] : table.items()) {
    const int item_cms_id = parse_cms_id_key(item_text, "item_skills");
    if (!skill_value.is_number_integer()) {
      throw std::runtime_error("Item " + item_text + " has no integral skill id");
    }
    const auto skill = skills.find(skill_value.get<int>());
    if (skill == skills.end() || skill->second.at("type") != "Item") {
      throw std::runtime_error("Item " + item_text + " names no loaded Item skill");
    }
    item_skills.emplace(item_cms_id, skill->first);
  }
  return item_skills;
}

bool valid_recovery_definition(const Json &row) {
  if (!row.is_object() || !row.contains("stat") ||
      (row.at("stat") != kHealth && row.at("stat") != kStamina)) {
    return false;
  }
  if (!row.contains("interval_ms") || !row.at("interval_ms").is_number_integer() ||
      row.at("interval_ms").get<int>() <= 0 ||
      row.at("interval_ms").get<int>() > kMaxRecoveryIntervalMs) {
    return false;
  }
  if (!row.contains("duration_ms") || !row.at("duration_ms").is_number_integer() ||
      row.at("duration_ms").get<int>() < 0 ||
      row.at("duration_ms").get<int>() > kMaxRecoveryDurationMs) {
    return false;
  }
  if (!row.contains("amount") || !row.at("amount").is_number()) {
    return false;
  }
  const double amount = row.at("amount").get<double>();
  if (!std::isfinite(amount) || amount < 0 || amount > kMaxRecoveryAmount) {
    return false;
  }
  // Optional since item skills: profiles exported before them still load.
  return row.value("use_potion_heal_rate", Json(false)).is_boolean() &&
         row.value("cancel_type", Json("None")).is_string();
}

void validate_recovery_definitions(const Json &recoveries) {
  if (!recoveries.is_object()) {
    throw std::runtime_error("Invalid combat recovery table");
  }
  for (const auto &[id, row] : recoveries.items()) {
    parse_cms_id_key(id, "recoveries");
    if (!valid_recovery_definition(row)) {
      throw std::runtime_error("Invalid combat recovery definition " + id);
    }
  }
}

} // namespace

CombatData::CombatData(const Json &document)
    : effects(document.at("effects")), projectiles(document.at("projectiles")),
      npc_stats(document.at("npc_stats")), npc_skills(document.at("npc_skills")),
      weapon_items(document.at("weapon_items")), recoveries(document.at("recoveries")),
      status_effects(document.value("status_effects", nlohmann::json::object())) {
  if (document.at("format_version") != 1 || document.at("skills").size() > kMaxCombatSkills ||
      effects.size() > 8192 || projectiles.size() > 2048 || npc_stats.size() > kMaxNpcStats) {
    throw std::runtime_error("Combat profile exceeds its supported limits");
  }
  for (const auto &skill : document.at("skills")) {
    const int id = skill.at("id");
    if (id <= 0 || skill.at("layers").empty() || skill.at("layers").size() > 64 ||
        skill.at("cooldown_ms").get<int>() < 0 || skill.at("range_mm").get<int>() < 0 ||
        skill.value("min_range_mm", 0) < 0 ||
        skill.value("min_range_mm", 0) > skill.at("range_mm").get<int>() ||
        skill.at("range_mm").get<int>() > 1000000 || !skills.emplace(id, skill).second) {
      throw std::runtime_error("Invalid combat skill definition");
    }
    std::set<int> layer_indexes;
    for (const auto &layer : skill.at("layers")) {
      if (!layer_indexes.insert(layer.at("index").get<int>()).second ||
          layer.at("stages").empty() || layer.at("stages").size() > 64) {
        throw std::runtime_error("Invalid combat action layer");
      }
      std::set<int> stage_indexes;
      for (const auto &stage : layer.at("stages")) {
        const int duration = stage.at("duration_ms");
        if (!stage_indexes.insert(stage.at("index").get<int>()).second || duration <= 0 ||
            duration > 300000 || stage.at("events").size() > 128 ||
            stage.at("notifies").size() > 256) {
          throw std::runtime_error("Invalid combat action stage");
        }
        for (const auto &event : stage.at("events")) {
          const int time = event.at("time_ms");
          const auto &effect_value = event.at("effect_id");
          if (!effect_value.is_number_integer()) {
            throw std::runtime_error("Combat effect CMS reference must be an integer");
          }
          std::uint64_t effect_id = 0;
          if (effect_value.is_number_unsigned()) {
            effect_id = effect_value.get<std::uint64_t>();
          } else {
            const auto signed_id = effect_value.get<std::int64_t>();
            if (signed_id > 0) {
              effect_id = static_cast<std::uint64_t>(signed_id);
            }
          }
          if (time < 0 || time >= duration || effect_id == 0 ||
              effect_id > static_cast<std::uint64_t>((std::numeric_limits<int>::max)()) ||
              !effects.contains(std::to_string(effect_id))) {
            throw std::runtime_error("Invalid combat effect deadline or CMS reference");
          }
          if (event.contains("shape")) {
            validate_event_shape(event.at("shape"));
          }
        }
        if (stage.contains("root_motion")) {
          const auto key =
              std::make_tuple(id, layer.at("index").get<int>(), stage.at("index").get<int>());
          root_motion.emplace(key, parse_root_motion(stage.at("root_motion")));
        }
        validate_player_motion(stage, duration);
      }
    }
    for (const auto &[name, amount] : skill.at("costs").items()) {
      if ((name != "SP" && name != "HP") || !std::isfinite(amount.get<double>()) ||
          amount.get<double>() < 0 || amount.get<double>() > 1000000) {
        throw std::runtime_error("Unsupported combat resource cost");
      }
    }
  }
  if (!status_effects.is_object() || status_effects.size() > kMaxStatusEffectDefinitions) {
    throw std::runtime_error("Invalid combat status effect table");
  }
  for (const auto &[id, row] : status_effects.items()) {
    if (!row.is_object() || !row.contains("duration_ms") ||
        !row.at("duration_ms").is_number_integer() || row.at("duration_ms").get<int>() < 0 ||
        row.at("duration_ms").get<int>() > kMaxStatusDurationMs) {
      throw std::runtime_error("Invalid combat status effect duration for " + id);
    }
    auto modifiers = parse_status_stat_modifiers(id, row);
    if (!modifiers.empty()) {
      status_stat_modifiers.emplace(parse_cms_id_key(id, "status_effects"), std::move(modifiers));
    }
    auto immunities = parse_abnormal_immunities(id, row);
    if (!immunities.empty()) {
      status_abnormal_immunities.emplace(parse_cms_id_key(id, "status_effects"),
                                         std::move(immunities));
    }
  }
  item_skills = parse_item_skills(document.value("item_skills", Json::object()), skills);
  validate_recovery_definitions(recoveries);
  immunities = parse_immunities(document.value("immunities", Json::object()));
  shields = parse_shields(document.value("shields", Json::object()), immunities);
  areas = parse_areas(document.value("areas", Json::object()), effects);
  item_effect_lists =
      parse_item_effect_lists(document.value("item_effect_lists", Json::object()), effects);
  if (document.contains("abnormal_gauges")) {
    abnormal_gauges = combat_rules::AbnormalGaugeRules::parse(document.at("abnormal_gauges"));
  }
  const auto locomotion = document.value("npc_locomotion", Json::object());
  if (!locomotion.is_object() || locomotion.size() > kMaxNpcStats) {
    throw std::runtime_error("Invalid NPC locomotion table");
  }
  for (const auto &[cms, row] : locomotion.items()) {
    const auto definition = NpcAiDefinition::from_json(row);
    if (!definition) {
      throw std::runtime_error("NPC locomotion block has no speed for " + cms);
    }
    npc_locomotion.emplace(parse_cms_id_key(cms, "npc_locomotion"), *definition);
  }
  parse_unit_collision_table(document, "npc_collision", npc_collision);
  parse_unit_collision_table(document, "player_collision", player_collision);
  // Effects, projectiles and areas must be loaded before attacks are classified.
  build_npc_attack_options();
}

void CombatData::build_npc_attack_options() {
  if (!npc_skills.is_object() || npc_skills.size() > kMaxNpcStats) {
    throw std::runtime_error("Invalid NPC skill table");
  }
  // Requires player_collision: melee reach is measured to the edge of the player's capsule.
  const auto player_volume = player_hit_volume(*this);
  for (const auto &[cms, skill_ids] : npc_skills.items()) {
    if (!skill_ids.is_array()) {
      throw std::runtime_error("NPC skill list is not an array for " + cms);
    }
    std::vector<NpcAttackOption> attacks;
    for (const auto &skill_id : skill_ids) {
      const auto skill = skills.find(skill_id.get<int>());
      if (skill == skills.end() || !is_executable_npc_attack(*this, skill->second)) {
        continue;
      }
      NpcAttackOption attack;
      attack.skill_id = skill->first;
      attack.min_range_mm = skill->second.value("min_range_mm", std::int64_t{});
      attack.range_mm = skill->second.at("range_mm").get<std::int64_t>();
      attack.ranged = has_ranged_npc_effect(*this, skill->second);
      // Also for attacks counted as ranged only because of a catalog minimum distance: their
      // damage can still be a short caster-centred shape (a charge or leap). Projectile and
      // area attacks get no reach and keep their catalog range.
      const auto reach = npc_melee_reach_mm(*this, skill->second, player_volume);
      if (reach) {
        attack.melee_reach_mm =
            std::max<std::int64_t>(1, static_cast<std::int64_t>(std::floor(*reach)));
      }
      attacks.push_back(attack);
    }
    if (!attacks.empty()) {
      npc_attacks.emplace(parse_cms_id_key(cms, "npc_skills"), std::move(attacks));
    }
  }
}

const std::vector<NpcAttackOption> &CombatData::npc_attack_options(int npc_cms_id) const {
  static const std::vector<NpcAttackOption> kNoAttacks;
  const auto found = npc_attacks.find(npc_cms_id);
  return found == npc_attacks.end() ? kNoAttacks : found->second;
}

CombatSession::CombatSession(std::shared_ptr<const CombatData> data,
                             std::shared_ptr<const TutorialData> tutorial_data,
                             const Character &character, Json stats, const WorldProgress &progress)
    : data_(std::move(data)), tutorial_data_(std::move(tutorial_data)), player_id_(character.id),
      class_id_(character.class_id), stats_(std::move(stats)) {
  if (character.id <= 0 || stat_value(stats_, kMaxHealth) <= 0 ||
      stat_value(stats_, kMaxStamina) <= 0) {
    throw std::runtime_error("Combat requires verified character vitals");
  }
  stats_["69"] = progress.dead ? 0 : stat_value(stats_, kMaxHealth);
  stats_["72"] = progress.dead ? 0 : stat_value(stats_, kMaxStamina);
  update_player_weapon(character);
  if (weapon_.empty()) {
    for (const auto &item : starter_gear_for_class(class_id_)) {
      if (item.slot_index == 0) {
        weapon_ = data_->weapon_items.value(std::to_string(item.cms_id), std::string());
      }
    }
  }
  // Shared maps use this session for player vitals and environmental damage only.
  // Their NPC simulation and skill dispatch remain owned by the shared-world services.
  if (!tutorial_data_) {
    return;
  }
  for (const auto &[id, npc] : tutorial_data_->npcs) {
    const auto cms = std::to_string(npc.at("cms_id").get<int>());
    if (npc.at("behavior") != "Monster" || !data_->npc_stats.contains(cms)) {
      continue;
    }
    const auto &point = npc.at("position");
    const auto &values = data_->npc_stats.at(cms);
    if (stat_value(values, kMaxHealth) <= 0) {
      throw std::runtime_error("Combat NPC has no maximum HP");
    }
    const auto inserted = monsters().emplace(
        id, Monster{npc.at("cms_id"),
                    values,
                    {tutorial_data_->map_id(), point.at(0), point.at(1), point.at(2),
                     npc.at("yaw")},
                    npc.value("initially_active", true)});
    if (npc.contains("ai")) {
      // from_json returns no definition when both speed fields are absent; that monster keeps
      // the legacy path. A half-present or invalid pair throws and rejects the profile.
      // run_speed_mm_per_second is UnitGroupModel.runSpeed x10; move_speed_rate is the
      // generator's neutral constant 1000 and stays a Candidate with no catalog or capture
      // evidence. No default is applied here.
      if (auto definition = NpcAiDefinition::from_json(npc.at("ai"))) {
        auto &monster = inserted.first->second;
        monster.ai.emplace(std::move(*definition), monster.position);
        monster.hostile = npc.at("faction") == "EX5Faction_Bandit";
        monster.attack_skill = usable_npc_attack(*data_, monster.cms_id);
        monster.engagement_range_mm = npc_engagement_range_mm(
            *data_, monster.cms_id, monster.ai->definition(), monster.attack_skill);
      }
    }
  }
  for (const auto &[quest_id, quest] : progress.quests) {
    for (const auto id : quest.credited_kills) {
      const auto found = monsters().find(id);
      if (found != monsters().end()) {
        found->second.stats["69"] = 0;
      }
    }
  }
}

bool CombatSession::handles(std::uint16_t opcode, const Json &request) const {
  const auto world_lock = lock_world_mobs();
  if (opcode == kStartSkillRequest) {
    return !request.contains("use_item") || owns_item_use();
  }
  if (opcode == kSkillEffectReportRequest) {
    // Once combat owns item use, every effect report (item effects included) describes a
    // combat action; otherwise the tutorial potion's report stays with the tutorial session.
    if (owns_item_use()) {
      return true;
    }
    return !request.contains("skill_effect") ||
           request.at("skill_effect").value("effect_id", 0) != kTutorialPotionUseEffectId;
  }
  return opcode == kExecuteSpawnProjectileRequest || opcode == kChangeSkillStageRequest ||
         opcode == kClientProjectileHitRequest || opcode == kDespawnClientProjectileRequest ||
         opcode == kUpdateProjectileRequest;
}

void CombatSession::set_item_inventory(ItemInventoryAccess inventory) {
  item_inventory_ = std::move(inventory);
}

bool CombatSession::owns_item_use() const {
  return item_inventory_.quantity && item_inventory_.consume_one && !data_->item_skills.empty();
}

void CombatSession::set_player_map(std::optional<int> map_id) {
  const auto world_lock = lock_world_mobs();
  if (map_id && *map_id <= 0) {
    throw std::invalid_argument("Player combat map must be positive");
  }
  player_map_ = map_id;
}

int CombatSession::used_item(const Json &request, int skill_id, const Json &skill) const {
  const bool item_skill = skill.at("type") == "Item";
  if (!request.contains("use_item")) {
    // An item skill without an item would grant its effects for free.
    require(!item_skill, "Skill_InvalidUseItem");
    return 0;
  }

  require(item_skill && owns_item_use(), "Skill_UnusableItem");
  const auto &use = request.at("use_item");
  const int item_cms_id = use.at("item_cms_id");
  const auto mapped = data_->item_skills.find(item_cms_id);
  require(mapped != data_->item_skills.end() && mapped->second == skill_id, "Skill_UnusableItem");
  // Item skills spend stackable consumables, which have no item instance id.
  require(use.value("item_id", std::string("0")) == "0", "Skill_InvalidUseItem");
  require(item_inventory_.quantity(item_cms_id) > 0, "Skill_NotFoundUseItem");
  return item_cms_id;
}

bool CombatSession::busy() const {
  const auto world_lock = lock_world_mobs();
  return action_.has_value();
}

const Json &CombatSession::player_stats() const {
  const auto world_lock = lock_world_mobs();
  return stats_;
}

bool CombatSession::player_dead() const {
  const auto world_lock = lock_world_mobs();
  return stat_value(stats_, kHealth) == 0;
}

bool CombatSession::player_stunned(std::int64_t ticks) const {
  if (!data_->abnormal_gauges) {
    return false;
  }

  const auto &types = data_->abnormal_gauges->types;
  const auto stun = types.find(combat_rules::AbnormalType::stun);
  if (stun == types.end() || stun->second.status_effect_id <= 0) {
    return false;
  }

  const auto world_lock = lock_world_mobs();
  const int stun_status = stun->second.status_effect_id;
  return std::ranges::any_of(statuses_, [stun_status, ticks](const ActiveStatus &status) {
    return status.status_effect_id == stun_status && status.expire_tick > ticks;
  });
}

bool CombatSession::monster_stunned(const Monster &monster, std::int64_t ticks) const {
  if (!data_->abnormal_gauges) {
    return false;
  }
  const auto stun = data_->abnormal_gauges->types.find(combat_rules::AbnormalType::stun);
  if (stun == data_->abnormal_gauges->types.end() || stun->second.status_effect_id <= 0) {
    return false;
  }
  return std::ranges::any_of(monster.statuses, [&](const ActiveStatus &status) {
    return status.status_effect_id == stun->second.status_effect_id && status.expire_tick > ticks;
  });
}

bool CombatSession::set_monster_status_sequence_for_test(std::uint64_t unit_id,
                                                          std::uint64_t next_instance) {
  const auto world_lock = lock_world_mobs();
  const auto monster = monsters().find(unit_id);
  if (monster == monsters().end() || next_instance < monster->second.next_status_instance) {
    return false;
  }
  monster->second.next_status_instance = next_instance;
  return true;
}

void CombatSession::record_world_monster_engagement(std::uint64_t unit_id) {
  if (!world_mobs_) {
    return;
  }
  const auto found = monsters().find(unit_id);
  if (found == monsters().end() || !found->second.active) {
    return;
  }

  auto &monster = found->second;
  engaged_world_mobs_.insert(unit_id);
  if (!monster.aggro) {
    if (monster.ai) {
      monster.damaged = true;
    } else {
      monster.aggro = true;
    }
  }
  if (monster.simulation_owner == 0 && !monster.awaiting_target_transfer) {
    monster.simulation_owner = player_id_;
  }
}

void CombatSession::interrupt_monster_action(std::uint64_t unit_id, Monster &monster,
                                               std::int64_t ticks, std::int64_t resume_tick,
                                               std::vector<GameplayPacket> &packets) {
  sync_npc_lunge_pose(monster, ticks);
  if (monster.ai) {
    monster.ai->pause_until(ticks, resume_tick);
    monster.position = monster.ai->position();
  }
  if (monster.move_announced) {
    announce_npc_move(unit_id, monster, false, ticks, packets);
  }
  finish(monster.action, unit_id, monster.position, ticks, packets, "EX5EndSkillReason_Default");
  cancel_npc_projectiles(unit_id, monster.position, ticks, packets);
}

std::vector<GameplayPacket>
CombatSession::apply_fall_damage(int damage, const WorldPosition &position, std::int64_t ticks) {
  const auto world_lock = lock_world_mobs();
  return apply_environmental_damage(damage, position, ticks, {});
}

std::vector<GameplayPacket> CombatSession::apply_drowning_damage(int damage,
                                                                 const WorldPosition &position,
                                                                 std::int64_t ticks) {
  const auto world_lock = lock_world_mobs();
  return apply_environmental_damage(damage, position, ticks, "EX5SkillEffectOriginType_Drowned");
}

std::vector<GameplayPacket> CombatSession::apply_environmental_damage(int damage,
                                                                      const WorldPosition &position,
                                                                      std::int64_t ticks,
                                                                      std::string_view origin,
                                                                      GmProtectionRule protection) {
  const bool protected_by_gm = gm_invulnerable_ && protection == GmProtectionRule::respected;
  if (damage <= 0 || player_dead() || protected_by_gm) {
    return {};
  }

  const auto previous = stat_value(stats_, kHealth);
  const auto applied = std::min(previous, damage);
  const auto result = previous - applied;
  stats_[std::to_string(kHealth)] = result;
  regeneration_tick_ = ticks + kTicksPerSecond;

  const auto player = std::to_string(player_id_);
  std::vector<GameplayPacket> packets;
  if (result == 0) {
    packets.push_back({32,
                       {{"unit_id", player},
                        {"state", "EX5UnitStateType_Demise"},
                        {"tick", wire_monotonic_tick(ticks)}}});
  }
  // Environmental attribution uses the protobuf default caster/skill fields. No
  // official fall packet is captured; this wire presentation remains Candidate.
  const Json health_change{{"stat_type", kHealth}, {"delta", -applied}, {"result", result}};
  Json notification{{"target_unit_id", player},
                    {"base_damage_amount", damage},
                    {"final_damage_amount", damage},
                    {"tick", wire_monotonic_tick(ticks)},
                    {"stat_changes", Json::array({health_change})}};
  if (!origin.empty()) {
    notification["skill_effect_origin_type"] = origin;
  }
  packets.push_back({19, std::move(notification)});
  if (result == 0) {
    packets.push_back({23, {{"target_unit_id", player}, {"pos", wire_position(position)}}});
    finish_player_death(ticks, packets);
  }
  return packets;
}

void CombatSession::finish_player_death(std::int64_t ticks, std::vector<GameplayPacket> &packets) {
  // Observed in all four captured combat deaths: DeathNoti is followed by the regen
  // checkpoint and stat-71 clear. Preserve that order for environmental deaths too.
  const auto player = std::to_string(player_id_);
  packets.push_back({54,
                     {{"unit_id", player},
                      {"tick", wire_monotonic_tick(ticks)},
                      {"regens",
                       {{{"applied_regen_range_end_tick", "9223372036854775807"},
                         {"checkpoint", {{"source", "SCS_Regen"}, {"stat_type", kHealth}}}}}}}});
  packets.push_back({47,
                     {{"unit_id", player},
                      {"tick", wire_monotonic_tick(ticks)},
                      {"default_source", "SCS_StatusEffect"},
                      {"stat_changes", {{{"stat_type", kDeathClearedStat}}}}}});
  // The packet above already reports the cleared shield.
  player_shield_.reset();
  stats_[std::to_string(kShieldStat)] = 0;
  // Dead players cannot receive projectile hits, and revival discards their old flights.
  action_.reset();
  clear_statuses_on_death(ticks, packets);
}

void CombatSession::defeat_monster(std::uint64_t target, std::uint64_t caster, std::int64_t ticks,
                                   std::vector<GameplayPacket> &packets, bool outbox_committed) {
  const auto position = monsters().at(target).position;
  auto &life = monsters().at(target);
  if (world_mobs_ && life.private_owner == 0 && world_defeat_capture_ && !outbox_committed) {
    if (pending_world_defeats_.size() >= kMaximumPendingWorldDefeats) {
      if (!pending_world_defeat_capacity_reported_) {
        spdlog::error("World monster reward backlog is full; lethal hits are paused until "
                      "pending defeats settle");
        pending_world_defeat_capacity_reported_ = true;
      }
      throw std::runtime_error("World monster reward backlog is full");
    }
    if (packets.empty() || packets.back().opcode != 19) {
      throw std::logic_error("A deferred monster defeat requires its lethal damage packet");
    }
    const MonsterDefeat defeat{target, life.cms_id, static_cast<std::int64_t>(player_id_),
                               position, ticks, life.reward_credit_id, life.reward_event_id};
    auto capture = world_defeat_capture_(defeat);
    if (capture.recipients.empty() ||
        capture.recipients.size() != capture.quest_captures.size()) {
      throw std::runtime_error("A world monster defeat has no durable reward recipient");
    }

    const auto &changes = packets.back().message.at("stat_changes");
    const auto health = std::ranges::find_if(changes, [](const auto &change) {
      return change.value("stat_type", 0) == kHealth;
    });
    if (health == changes.end()) {
      throw std::logic_error("A deferred monster defeat has no health change");
    }
    const int previous_health = health->at("result").get<int>() - health->at("delta").get<int>();
    pending_world_defeats_.push_back({defeat, caster, std::move(capture), packets.back()});
    life.pending_defeat_visible_hp = previous_health;
    packets.pop_back();
    return;
  }

  life.pending_defeat_visible_hp.reset();
  world_monster_defeated(target, ticks);
  bool corpse_removal_managed_by_observer = false;
  if (defeat_observer_ && (!world_mobs_ || monsters().at(target).private_owner == 0)) {
    try {
      auto reaction = defeat_observer_({target, monsters().at(target).cms_id,
                                        static_cast<std::int64_t>(player_id_), position, ticks});
      corpse_removal_managed_by_observer = reaction.manages_corpse_removal;
      append(packets, std::move(reaction.packets));
    } catch (...) {
      // The caller keeps a committed-but-unpublished defeat for operator recovery. Replaying
      // an observer that may have partially registered a corpse could duplicate or change loot.
      throw;
    }
  }
  packets.push_back({32,
                     {{"unit_id", std::to_string(target)},
                      {"state", "EX5UnitStateType_Demise"},
                      {"tick", wire_monotonic_tick(ticks)}}});
  packets.push_back({23,
                     {{"base_caster_unit_id", std::to_string(caster)},
                      {"target_unit_id", std::to_string(target)},
                      {"pos", wire_position(position)}}});

  // The observer may have changed the container; look the monster up again.
  auto &monster = monsters().at(target);
  defeated_.push_back(target);
  // Death removes every status (removeConditionType1 Death); the corpse needs none.
  monster.statuses.clear();
  monster.gauges.clear();
  monster.shield.reset();
  monster.stats[std::to_string(kShieldStat)] = 0;
  if ((!world_mobs_ || monster.private_owner != 0) && !corpse_removal_managed_by_observer) {
    monster.corpse_deadline = ticks + kCorpseRemovalDelayMs * kTicksPerMillisecond;
  }
  monster.action.reset();
}

std::vector<GameplayPacket>
CombatSession::change_movement_stamina(int delta, std::string_view source, std::int64_t ticks,
                                        bool announce) {
  const auto world_lock = lock_world_mobs();
  if (delta > 0 || (source != "SCS_SprintFlow" && source != "SCS_SwimFlow")) {
    throw std::invalid_argument("Invalid movement stamina consumption");
  }
  if (delta == 0 || player_dead() || gm_infinite_stamina_) {
    return {};
  }

  std::vector<GameplayPacket> packets;
  if (announce) {
    change_stat(kStamina, delta, ticks, packets);
  } else {
    adjust_vital(kStamina, delta);
  }
  for (auto &packet : packets) {
    packet.message["default_source"] = source;
  }
  stamina_resume_tick_ = std::max(stamina_resume_tick_, ticks + kTicksPerSecond);
  regeneration_tick_ = ticks + kTicksPerSecond;
  return packets;
}

std::optional<std::pair<std::uint64_t, GameplayPacket>>
CombatSession::begin_swimming_stamina_flow(double rate_per_second, std::int64_t ticks) {
  const auto world_lock = lock_world_mobs();
  if (player_dead() || gm_infinite_stamina_ || rate_per_second >= 0 ||
      !std::isfinite(rate_per_second)) {
    return std::nullopt;
  }
  const auto instance = next_recovery_instance_++;
  const auto stamina = stat_value(stats_, kStamina);
  GameplayPacket packet{
      kStatSyncNotification,
      {{"unit_id", std::to_string(player_id_)}, {"tick", wire_monotonic_tick(ticks)},
       {"flows", {{{"stat_type", kStamina}, {"source", "SCS_SwimFlow"},
                   {"instance_id", std::to_string(instance)}, {"rate_per_sec", rate_per_second},
                   {"checkpoint", {{"stat_type", kStamina}, {"source", "SCS_SwimFlow"},
                                   {"result", stamina}}}}}},
       {"regens", {{{"applied_regen_range_end_tick", "9223372036854775807"},
                     {"checkpoint", {{"stat_type", kStamina}, {"source", "SCS_Regen"},
                                     {"result", stamina}}}}}}}};
  return std::pair{instance, std::move(packet)};
}

GameplayPacket CombatSession::end_swimming_stamina_flow(std::uint64_t instance,
                                                        std::int64_t ticks,
                                                        bool block_regeneration) {
  const auto world_lock = lock_world_mobs();
  const auto stamina = stat_value(stats_, kStamina);
  constexpr std::int64_t kRegenDelayTicks = 300 * kTicksPerMillisecond;
  const auto regeneration_end = block_regeneration
                                    ? std::string("9223372036854775807")
                                    : wire_monotonic_tick(ticks + kRegenDelayTicks);
  return {kStatSyncNotification,
          {{"unit_id", std::to_string(player_id_)}, {"tick", wire_monotonic_tick(ticks)},
           {"flows", {{{"stat_type", kStamina}, {"source", "SCS_SwimFlow"},
                       {"instance_id", std::to_string(instance)},
                       {"checkpoint", {{"stat_type", kStamina}, {"source", "SCS_SwimFlow"},
                                       {"result", stamina}}}}}},
           {"regens", {{{"applied_regen_range_end_tick", regeneration_end},
                         {"checkpoint", {{"stat_type", kStamina}, {"source", "SCS_Regen"},
                                         {"result", stamina}}}}}}}};
}

bool CombatSession::movement_stamina_unlimited() const {
  const auto world_lock = lock_world_mobs();
  return gm_infinite_stamina_;
}

bool CombatSession::can_apply_stat_recovery(int effect_id) const {
  const auto world_lock = lock_world_mobs();
  if (player_dead() || recoveries_.size() >= kMaxActiveRecoveries) {
    return false;
  }
  const auto effect = data_->effects.find(std::to_string(effect_id));
  if (effect == data_->effects.end() || effect->value("kind", std::string()) != "statRecovery") {
    return false;
  }
  const double raw_id = effect->at("parameters").value("statRecoveryCmsId", 0.0);
  if (!std::isfinite(raw_id) || raw_id < 1 || raw_id > std::numeric_limits<int>::max() ||
      raw_id != std::floor(raw_id)) {
    return false;
  }
  const auto recovery = data_->recoveries.find(std::to_string(static_cast<int>(raw_id)));
  return recovery != data_->recoveries.end() && recovery->value("duration_ms", 0) > 0 &&
         recovery->value("interval_ms", 0) > 0 && recovery->value("amount", 0.0) > 0;
}

void CombatSession::set_movement_recovery_blocked(bool stamina, bool health, std::int64_t ticks) {
  const auto world_lock = lock_world_mobs();
  const bool resumed = (movement_stamina_recovery_blocked_ && !stamina) ||
                       (movement_health_recovery_blocked_ && !health);
  movement_stamina_recovery_blocked_ = stamina;
  movement_health_recovery_blocked_ = health;
  if (resumed && !player_dead()) {
    // Never retroactively regenerate time spent sprinting or submerged.
    stamina_resume_tick_ = std::max(stamina_resume_tick_, ticks + kTicksPerSecond);
    regeneration_tick_ = ticks + kTicksPerSecond;
  }
}

std::optional<std::vector<GameplayPacket>>
CombatSession::begin_interaction_stamina(int cost, std::int64_t wire_start_ns, int execute_ms,
                                         int regen_resume_ms, std::int64_t ticks) {
  constexpr std::int64_t kWireNanosecondsPerMillisecond = 1000000;
  const auto world_lock = lock_world_mobs();
  if (cost <= 0 || execute_ms <= 0 || regen_resume_ms < execute_ms || wire_start_ns <= 0) {
    throw std::invalid_argument("Invalid interaction stamina flow");
  }

  const int stamina = stat_value(stats_, kStamina);
  if (player_dead() || (!gm_infinite_stamina_ && stamina < cost)) {
    return std::nullopt;
  }

  interaction_stamina_.reset();
  if (gm_infinite_stamina_) {
    // GM infinite stamina ignores the cost, so no drain is announced or deducted.
    return std::vector<GameplayPacket>{};
  }

  const auto instance = next_recovery_instance_++;
  const auto regen_resume_tick = ticks + regen_resume_ms * kTicksPerMillisecond;
  interaction_stamina_ = InteractionStamina{instance, cost, regen_resume_tick};
  stamina_resume_tick_ = std::max(stamina_resume_tick_, regen_resume_tick);

  // Observed for every official gathering start: one SCS_InteractionFlow flow draining the
  // cost over the execute time (rate -cost / execute seconds) and a regen range ending
  // SPStatRegenDelayMilli after the animation, both checkpointing the current stamina.
  const double rate_per_second = -static_cast<double>(cost) * 1000.0 / execute_ms;
  const auto end_ns = wire_start_ns + execute_ms * kWireNanosecondsPerMillisecond;
  const auto regen_end_ns = wire_start_ns + regen_resume_ms * kWireNanosecondsPerMillisecond;
  GameplayPacket announcement{
      kStatSyncNotification,
      {{"unit_id", std::to_string(player_id_)},
       {"tick", std::to_string(wire_start_ns)},
       {"flows",
        {{{"stat_type", kStamina},
          {"source", "SCS_InteractionFlow"},
          {"instance_id", std::to_string(instance)},
          {"rate_per_sec", rate_per_second},
          {"end_tick", std::to_string(end_ns)},
          {"checkpoint",
           {{"stat_type", kStamina}, {"source", "SCS_InteractionFlow"}, {"result", stamina}}}}}},
       {"regens",
        {{{"applied_regen_range_end_tick", std::to_string(regen_end_ns)},
          {"checkpoint",
           {{"stat_type", kStamina}, {"source", "SCS_Regen"}, {"result", stamina}}}}}}}};
  return std::vector<GameplayPacket>{std::move(announcement)};
}

void CombatSession::spend_interaction_stamina(std::int64_t ticks) {
  const auto world_lock = lock_world_mobs();
  if (!interaction_stamina_) {
    return;
  }

  const auto spent = *interaction_stamina_;
  interaction_stamina_.reset();
  if (player_dead()) {
    return;
  }

  // The client already drained the announced flow; deducting silently keeps both copies equal.
  adjust_vital(kStamina, -spent.cost);
  stamina_resume_tick_ = std::max(stamina_resume_tick_, spent.regen_resume_tick);
  const auto earliest_regeneration = std::max(ticks, spent.regen_resume_tick);
  if (!regeneration_tick_ || *regeneration_tick_ > earliest_regeneration) {
    regeneration_tick_ = earliest_regeneration;
  }
}

std::optional<GameplayPacket> CombatSession::cancel_interaction_stamina(std::int64_t ticks) {
  const auto world_lock = lock_world_mobs();
  if (!interaction_stamina_) {
    return std::nullopt;
  }

  const auto instance = interaction_stamina_->instance;
  interaction_stamina_.reset();
  stamina_resume_tick_ = std::max(stamina_resume_tick_, ticks + kTicksPerSecond);

  // Candidate: no official cancelled gathering was captured. The stop follows the observed end
  // of a sprint flow: the same flow instance again, without a rate, checkpointing the value.
  return GameplayPacket{
      kStatSyncNotification,
      {{"unit_id", std::to_string(player_id_)},
       {"tick", wire_monotonic_tick(ticks)},
       {"flows",
        {{{"stat_type", kStamina},
          {"source", "SCS_InteractionFlow"},
          {"instance_id", std::to_string(instance)},
          {"checkpoint",
           {{"stat_type", kStamina},
            {"source", "SCS_InteractionFlow"},
            {"result", stat_value(stats_, kStamina)}}}}}}}};
}

std::vector<GameplayPacket> CombatSession::revive(std::int64_t ticks) {
  if (!player_dead()) {
    throw std::logic_error("Cannot revive a living combatant");
  }

  action_.reset();
  projectiles_.clear();
  projectile_retry_not_before_.reset();
  client_projectiles_awaiting_despawn_.clear();
  recoveries_.clear();
  // Death already removed every status and reverted its stat modifiers.
  statuses_.clear();
  cooldowns_.clear();
  regeneration_tick_.reset();
  stamina_resume_tick_ = 0;
  movement_stamina_recovery_blocked_ = false;
  movement_health_recovery_blocked_ = false;
  interaction_stamina_.reset();

  // Candidate in-place revive. Order and fields follow the captured revival (generic-part-2 record
  // 15192 ff.): add 50030002 (no expiry), remove it, then one ChangeStat with source SCS_Revive
  // (stats 68/70/71/73/80 cleared, HP and SP at their maxima), then Idle. Deviations: the official
  // server first runs a teleport handshake to an unknown position, adds 50030001 for about 1.5 s,
  // and charges a gold penalty; none of that is implemented.
  constexpr int kRevivalInvincibilityStatusId = 50030002;
  const auto instance = next_status_instance_++;
  const auto unit_text = std::to_string(player_id_);
  std::vector<GameplayPacket> packets;
  packets.push_back({kAddStatusEffectNotification,
                     {{"unit_id", unit_text},
                      {"status_effect",
                       {{"status_effect_id", kRevivalInvincibilityStatusId},
                        {"instance_id", instance},
                        {"level", 1},
                        {"stack_count", 1},
                        {"caster_unit_id", unit_text},
                        {"start_tick", wire_monotonic_tick(ticks)},
                        {"expire_tick", "9223372036854775807"}}}}});
  packets.push_back({kRemoveStatusEffectNotification,
                     {{"unit_id", unit_text},
                      {"tick", wire_monotonic_tick(ticks)},
                      {"status_effects",
                       {{{"instance_id", instance},
                         {"status_effect_id", kRevivalInvincibilityStatusId}}}}}});
  const int health = stat_value(stats_, kMaxHealth);
  const int stamina = stat_value(stats_, kMaxStamina);
  stats_[std::to_string(kHealth)] = health;
  stats_[std::to_string(kStamina)] = stamina;
  Json stat_changes = Json::array();
  for (const int cleared : {68, 70, 71, 73, 80}) {
    if (stats_.contains(std::to_string(cleared))) {
      stats_[std::to_string(cleared)] = 0;
    }
    stat_changes.push_back({{"stat_type", cleared}});
    if (cleared == 68) {
      stat_changes.push_back({{"stat_type", kHealth}, {"result", health}});
    }
    if (cleared == 71) {
      stat_changes.push_back({{"stat_type", kStamina}, {"result", stamina}});
    }
  }
  packets.push_back({47,
                     {{"unit_id", unit_text},
                      {"tick", wire_monotonic_tick(ticks)},
                      {"default_source", "SCS_Revive"},
                      {"stat_changes", std::move(stat_changes)}}});
  packets.push_back({32,
                     {{"unit_id", unit_text},
                      {"tick", wire_monotonic_tick(ticks)},
                      {"state", "EX5UnitStateType_Idle"}}});
  return packets;
}

std::vector<GameplayPacket> CombatSession::revive_if_dead(std::int64_t ticks) {
  const auto world_lock = lock_world_mobs();
  if (!player_dead()) {
    return {};
  }
  return revive(ticks);
}

void CombatSession::update_player_stats(Json stats) {
  const auto world_lock = lock_world_mobs();
  const auto current_health = stat_value(stats_, kHealth);
  const auto current_stamina = stat_value(stats_, kStamina);

  // Active status modifiers stay on top of the refreshed equipment stats.
  for (const auto &status : statuses_) {
    for (const auto &modifier : status.modifiers) {
      const auto key = std::to_string(modifier.stat_type);
      stats[key] = stat_value(stats, modifier.stat_type) + modifier.add;
    }
  }

  const auto maximum_health = stat_value(stats, kMaxHealth);
  const auto maximum_stamina = stat_value(stats, kMaxStamina);
  if (maximum_health <= 0 || maximum_stamina <= 0) {
    throw std::invalid_argument("Equipment produced invalid combat vitals");
  }

  // Equipment changes affect the maxima, not damage or spent resources.
  stats[std::to_string(kHealth)] = std::clamp(current_health, 0, maximum_health);
  stats[std::to_string(kStamina)] = std::clamp(current_stamina, 0, maximum_stamina);
  // An active shield keeps its remaining amount; equipment does not grant shields.
  if (player_shield_) {
    stats[std::to_string(kShieldStat)] = stat_value(stats_, kShieldStat);
  }
  stats_ = std::move(stats);
}

void CombatSession::update_player_weapon(const Character &character) {
  const auto world_lock = lock_world_mobs();
  active_weapon_slot_ = character.build.active_weapon_slot;
  weapon_.clear();
  for (const auto &item : character.items) {
    if (item.equip_slot == active_weapon_slot_ && item.durability > 0) {
      weapon_ = data_->weapon_items.value(std::to_string(item.cms_id), std::string());
      break;
    }
  }
}

bool CombatSession::is_swap_weapon_skill(int skill_id) const {
  const auto found = data_->skills.find(skill_id);
  return found != data_->skills.end() && found->second.value("type", std::string()) == "SwapWeapon";
}

bool CombatSession::refill_player_vitals() {
  if (stat_value(stats_, kHealth) <= 0) {
    return false;
  }

  stats_[std::to_string(kHealth)] = stat_value(stats_, kMaxHealth);
  stats_[std::to_string(kStamina)] = stat_value(stats_, kMaxStamina);
  // Full vitals need no further regeneration ticks.
  regeneration_tick_.reset();
  return true;
}

void CombatSession::set_gm_protection(bool invulnerable, bool hidden, bool infinite_stamina) {
  const auto world_lock = lock_world_mobs();
  gm_invulnerable_ = invulnerable;
  gm_hidden_ = hidden;
  gm_infinite_stamina_ = infinite_stamina;
}

std::vector<GameplayPacket> CombatSession::gm_restore_vitals(std::int64_t ticks) {
  const auto world_lock = lock_world_mobs();
  std::vector<GameplayPacket> packets;
  if (player_dead()) {
    return packets;
  }

  change_stat(kHealth, stat_value(stats_, kMaxHealth) - stat_value(stats_, kHealth), ticks,
              packets);
  change_stat(kStamina, stat_value(stats_, kMaxStamina) - stat_value(stats_, kStamina), ticks,
              packets);
  regeneration_tick_.reset();
  return packets;
}

bool CombatSession::add_gm_monster(std::uint64_t unit_id, int npc_cms_id,
                                   const WorldPosition &position) {
  const auto world_lock = lock_world_mobs();
  const auto stats = data_->npc_stats.find(std::to_string(npc_cms_id));
  const auto locomotion = data_->npc_locomotion.find(npc_cms_id);
  const auto *attack = usable_npc_attack(*data_, npc_cms_id);
  const bool first_private_spawn =
      !tutorial_data_ && !private_encounter_map_ && position.map_id > 0;
  if (unit_id == 0 || unit_id == player_id_ ||
      (!owns_encounter_map(position.map_id) && !first_private_spawn) ||
      stats == data_->npc_stats.end() || locomotion == data_->npc_locomotion.end() || !attack ||
      monsters().contains(unit_id) || monsters().size() >= kMaxNpcStats ||
      stat_value(*stats, kMaxHealth) <= 0) {
    return false;
  }

  Monster monster{npc_cms_id, *stats, position, true};
  monster.private_owner = world_mobs_ ? player_id_ : 0;
  monster.stats[std::to_string(kHealth)] = stat_value(*stats, kMaxHealth);
  monster.hostile = true;
  monster.attack_skill = attack;
  monster.ai.emplace(locomotion->second, position);
  monster.engagement_range_mm =
      npc_engagement_range_mm(*data_, npc_cms_id, locomotion->second, attack);
  if (!tutorial_data_) {
    private_encounter_map_ = position.map_id;
  }
  monsters().emplace(unit_id, std::move(monster));
  return true;
}

void CombatSession::cancel_npc_projectiles(std::uint64_t unit_id,
                                           const WorldPosition &position,
                                           std::int64_t ticks,
                                           std::vector<GameplayPacket> &packets) {
  const auto monster = monsters().find(unit_id);
  if (monster != monsters().end()) {
    // The target's session owns the flight, while a different session can interrupt its caster.
    // Each flight retains its launch epoch so later interruptions cannot hide an earlier one.
    auto next_epoch = std::make_shared<ProjectileInterruption>();
    monster->second.projectile_interruption->tick = ticks;
    monster->second.projectile_interruption = std::move(next_epoch);
  }

  std::erase_if(projectiles_, [&](const auto &flight) {
    // A delayed session poll must still resolve a hit whose deadline preceded the interruption.
    if (flight.caster != unit_id || flight.deadline < ticks) {
      return false;
    }
    packets.push_back({kDespawnProjectileNotification,
                       {{"caster_unit_id", std::to_string(unit_id)},
                        {"despawn_projectile",
                         {{"instance_id", std::to_string(flight.instance)},
                          {"despawn_tick", wire_monotonic_tick(ticks)},
                          {"reason_type", "EX5SkillObjectDespawnReasonType_SkillStageCancel"},
                          {"pos", wire_position(position)}}}}});
    return true;
  });
}

std::vector<GameplayPacket> CombatSession::remove_gm_monster(std::uint64_t unit_id,
                                                            std::int64_t ticks) {
  const auto world_lock = lock_world_mobs();
  std::vector<GameplayPacket> packets;
  const auto found = monsters().find(unit_id);
  if (found == monsters().end()) {
    return packets;
  }
  auto &monster = found->second;
  if (monster.ai) {
    if (const auto position = monster.ai->route_position_at(ticks)) {
      monster.position = *position;
    }
  }
  sync_npc_lunge_pose(monster, ticks);
  if (monster.move_announced) {
    announce_npc_move(unit_id, monster, false, ticks, packets);
  }
  finish(monster.action, unit_id, monster.position, ticks, packets,
         "EX5EndSkillReason_Default");
  cancel_npc_projectiles(unit_id, monster.position, ticks, packets);
  std::erase_if(areas_, [&](const auto &area) {
    if (area.caster != unit_id) {
      return false;
    }
    if (!area.removal_tick) {
      packets.push_back({kDespawnSkillObjectNotification,
                         {{"unit_id", std::to_string(area.unit_id)},
                          {"reason_type", "EX5SkillObjectDespawnReasonType_SkillStageCancel"}}});
    }
    packets.push_back(
        {kUnitRemoveNotification, {{"unit_ids", Json::array({std::to_string(area.unit_id)})}}});
    return true;
  });
  monsters().erase(found);
  return packets;
}

std::vector<GameplayPacket> CombatSession::gm_kill_player(const WorldPosition &position,
                                                          std::int64_t ticks) {
  const int remaining_health = stat_value(stats_, kHealth);
  return apply_environmental_damage(remaining_health, position, ticks, {},
                                    GmProtectionRule::ignored);
}

std::optional<CombatSession::MonsterDebugView>
CombatSession::monster_debug_view(std::uint64_t unit_id, std::int64_t ticks) const {
  const auto world_lock = lock_world_mobs();
  const auto found = monsters().find(unit_id);
  if (found == monsters().end()) {
    return std::nullopt;
  }

  const auto &monster = found->second;
  MonsterDebugView view;
  view.position = monster.position;
  view.alive = stat_value(monster.stats, kHealth) > 0;
  if (monster.ai) {
    // The stored position only advances at AI steps; the route gives the current point.
    if (const auto route_position = monster.ai->route_position_at(ticks)) {
      view.position = *route_position;
    }
    view.ai_state = monster.ai->state();
    view.route_end = monster.ai->route_end();
  }
  return view;
}

std::optional<int> CombatSession::live_monster_health(std::uint64_t unit_id) const {
  const auto world_lock = lock_world_mobs();
  if (!is_live_monster(unit_id)) {
    return std::nullopt;
  }
  return monster_hp(unit_id);
}

std::vector<GameplayPacket> CombatSession::gm_damage_monster(std::uint64_t unit_id, int amount,
                                                             std::int64_t ticks) {
  const auto world_lock = lock_world_mobs();
  std::vector<GameplayPacket> packets;
  if (amount <= 0 || !is_live_monster(unit_id)) {
    return packets;
  }

  auto &monster = monsters().at(unit_id);
  if (monster.ai) {
    if (const auto position = monster.ai->route_position_at(ticks)) {
      monster.position = *position;
    }
  }
  sync_npc_lunge_pose(monster, ticks);
  const int previous = stat_value(monster.stats, kHealth);
  const int result = previous - std::min(previous, amount);
  monster.stats[std::to_string(kHealth)] = result;

  // A GM hit has no skill; like environmental damage it leaves the skill fields unset.
  packets.push_back(
      {19,
       {{"caster_unit_id", std::to_string(player_id_)},
        {"base_caster_unit_id", std::to_string(player_id_)},
        {"target_unit_id", std::to_string(unit_id)},
        {"base_damage_amount", amount},
        {"final_damage_amount", amount},
        {"tick", wire_monotonic_tick(ticks)},
        {"stat_changes",
         {{{"stat_type", kHealth}, {"delta", result - previous}, {"result", result}}}}}});
  if (result == 0) {
    try {
      defeat_monster(unit_id, player_id_, ticks, packets);
    } catch (...) {
      if (world_defeat_capture_ && world_mobs_ && monsters().contains(unit_id) &&
          monsters().at(unit_id).private_owner == 0) {
        monsters().at(unit_id).stats[std::to_string(kHealth)] = previous;
      }
      throw;
    }
  } else if (monster.ai) {
    monster.damaged = true;
  }
  publish_world_packets(packets);
  return packets;
}

std::vector<GameplayPacket> CombatSession::gm_add_status_effect(std::uint64_t unit_id,
                                                                int status_effect_id,
                                                                std::int64_t ticks) {
  const auto world_lock = lock_world_mobs();
  std::vector<GameplayPacket> packets;
  if (unit_id == player_id_ && player_dead()) {
    return packets;
  }
  apply_status(unit_id, player_id_, status_effect_id, ticks, packets);
  return packets;
}

std::vector<GameplayPacket> CombatSession::gm_remove_status_effect(std::uint64_t unit_id,
                                                                   int status_effect_id,
                                                                   std::int64_t ticks) {
  const auto world_lock = lock_world_mobs();
  std::vector<GameplayPacket> packets;
  remove_statuses_by_id(unit_id, status_effect_id, ticks, packets);
  return packets;
}

std::vector<GameplayPacket> CombatSession::gm_add_abnormal_gauge(std::uint64_t unit_id,
                                                                 combat_rules::AbnormalType type,
                                                                 int amount, std::int64_t ticks) {
  const auto world_lock = lock_world_mobs();
  std::vector<GameplayPacket> packets;
  change_abnormal_gauge(unit_id, type, amount, player_id_, ticks, packets);
  return packets;
}

std::vector<GameplayPacket> CombatSession::gm_cancel_skill(std::uint64_t unit_id,
                                                           const WorldPosition &position,
                                                           std::int64_t ticks) {
  const auto world_lock = lock_world_mobs();
  std::vector<GameplayPacket> packets;
  if (unit_id == player_id_) {
    finish(action_, player_id_, position, ticks, packets, "EX5EndSkillReason_Default");
    return packets;
  }

  const auto found = monsters().find(unit_id);
  if (found != monsters().end()) {
    auto &monster = found->second;
    finish(monster.action, unit_id, monster.position, ticks, packets, "EX5EndSkillReason_Default");
  }
  return packets;
}

std::size_t CombatSession::gm_reset_cooldowns() {
  const auto world_lock = lock_world_mobs();
  std::size_t cleared = cooldowns_.size();
  cooldowns_.clear();
  for (auto &[id, monster] : monsters()) {
    cleared += monster.skill_ready_ticks.size();
    monster.skill_ready_ticks.clear();
  }
  return cleared;
}

std::vector<GameplayPacket> CombatSession::gm_toggle_monster_ai(std::uint64_t unit_id, bool enabled,
                                                                std::int64_t ticks) {
  const auto world_lock = lock_world_mobs();
  const auto found = monsters().find(unit_id);
  if (found == monsters().end() || !found->second.ai || !is_live_monster(unit_id)) {
    return {};
  }
  auto &monster = found->second;
  std::vector<GameplayPacket> packets;
  if (const auto position = monster.ai->route_position_at(ticks)) {
    monster.position = *position;
  }
  sync_npc_lunge_pose(monster, ticks);
  finish(monster.action, unit_id, monster.position, ticks, packets, "EX5EndSkillReason_Default");
  cancel_npc_projectiles(unit_id, monster.position, ticks, packets);
  monster.ai->reset_at(monster.position);
  monster.gm_ai_paused = !enabled;
  monster.aggro = false;
  monster.damaged = false;
  announce_npc_move(unit_id, monster, false, ticks, packets);
  return packets;
}

std::vector<GameplayPacket> CombatSession::gm_set_player_stat(int stat_type, int value,
                                                              std::int64_t ticks) {
  std::vector<GameplayPacket> packets;
  // Health transitions must go through the durable death/revival paths. Also keep derived
  // maxima nonnegative so adjust_vital's clamp always has a valid range.
  if (!x5::cms::EX5StatType_IsValid(stat_type) || stat_type == 0 || value < 0 ||
      (stat_type == kMaxHealth && value == 0) ||
      (stat_type == kHealth && (value == 0 || player_dead()))) {
    return packets;
  }
  if (stat_type == kHealth || stat_type == kStamina) {
    // Vitals keep their normal bounds and regeneration bookkeeping.
    change_stat(stat_type, value - stat_value(stats_, stat_type), ticks, packets);
    return packets;
  }

  const int previous = stat_value(stats_, stat_type);
  if (previous == value) {
    return packets;
  }
  stats_[std::to_string(stat_type)] = value;
  Json changes = Json::array({{{"stat_type", stat_type}, {"result", value}}});
  // A lowered maximum never leaves the current pool above it.
  if (stat_type == kMaxHealth) {
    changes.push_back({{"stat_type", kHealth}, {"result", adjust_vital(kHealth, 0)}});
  }
  if (stat_type == kMaxStamina) {
    changes.push_back({{"stat_type", kStamina}, {"result", adjust_vital(kStamina, 0)}});
  }
  packets.push_back({kChangeStatNotification,
                     {{"unit_id", std::to_string(player_id_)},
                      {"tick", wire_monotonic_tick(ticks)},
                      {"stat_changes", std::move(changes)}}});
  return packets;
}

std::vector<GameplayPacket> CombatSession::gm_execute_self_effect(int effect_id,
                                                                  const WorldPosition &position,
                                                                  std::int64_t ticks) {
  const auto world_lock = lock_world_mobs();
  // Validate the entire chain first: a refused tail must not leave a partially applied head.
  std::set<int> visited;
  int current = effect_id;
  while (current > 0) {
    const auto definition = data_->effects.find(std::to_string(current));
    if (definition == data_->effects.end() || !visited.insert(current).second ||
        visited.size() > 8) {
      return {};
    }
    const auto kind = definition->value("kind", std::string());
    const bool supported = kind == "restoreHP" || kind == "heal" || kind == "addStatusEffect" ||
                           kind == "removeStatusEffect" || kind == "addAbnormalGauge" ||
                           kind == "statRecovery";
    if (!supported || definition->at("targeting").value("refType", std::string()) !=
                          "EX5ReferenceTargetType_Self") {
      return {};
    }
    current = definition->value("chain", 0);
  }
  if (visited.empty()) {
    return {};
  }
  return apply_item_effects({effect_id}, position, ticks);
}

std::vector<GameplayPacket> CombatSession::gm_place_monster(std::uint64_t unit_id,
                                                            const WorldPosition &destination,
                                                            std::int64_t ticks) {
  const auto world_lock = lock_world_mobs();
  std::vector<GameplayPacket> packets;
  if (!is_live_monster(unit_id)) {
    return packets;
  }

  auto &monster = monsters().at(unit_id);
  finish(monster.action, unit_id, monster.position, ticks, packets, "EX5EndSkillReason_Default");
  monster.position = destination;
  if (monster.ai) {
    monster.ai->reset_at(destination);
  }
  monster.aggro = false;
  monster.damaged = false;
  announce_npc_move(unit_id, monster, false, ticks, packets);
  return packets;
}

int CombatSession::monster_hp(std::uint64_t id) const {
  const auto world_lock = lock_world_mobs();
  return stat_value(monsters().at(id).stats, kHealth);
}

void CombatSession::set_defeat_observer(MonsterDefeatObserver observer) {
  const auto world_lock = lock_world_mobs();
  defeat_observer_ = std::move(observer);
}

QuestEvents CombatSession::take_quest_events() {
  auto events = std::move(quest_events_);
  quest_events_.clear();
  return events;
}

const QuestEvents &CombatSession::quest_events() const noexcept {
  return quest_events_;
}

bool CombatSession::decorate_spawn(GameplayPacket &packet) const {
  const auto world_lock = lock_world_mobs();
  if (packet.opcode != 10 || !packet.message.contains("nub") ||
      !packet.message.at("nub").contains("npc")) {
    return true;
  }
  const auto found = monsters().find(unit_id(packet.message.at("unit_id")));
  if (found != monsters().end()) {
    if (stat_value(found->second.stats, kHealth) <= 0) {
      return false;
    }
    packet.message["nub"]["npc"]["stats"] = found->second.stats;
  }
  return true;
}

const Json *CombatSession::find_stage(const Action &action, int stage_index) const {
  const auto *layer = by_index(data_->skills.at(action.skill_id).at("layers"), action.layer);
  if (!layer) {
    return nullptr;
  }
  return by_index(layer->at("stages"), stage_index);
}

const Json &CombatSession::stage(const Action &action) const {
  const auto *result = find_stage(action, action.stage);
  if (!result) {
    throw std::runtime_error("Combat action lost its definition layer or stage");
  }
  return *result;
}

void CombatSession::enter_stage(Action &action, int stage_index, std::int64_t ticks) {
  if (action_ && &action == &*action_) {
    retain_player_projectile_immunity(action, ticks);
  }

  action.stage = stage_index;
  action.started = ticks;
  action.executed_events.clear();
  action.player_motion_received = false;
  action.adapted_rotation_windows.clear();
}

CombatSession::PlayerMotionOutcome
CombatSession::receive_player_motion(std::uint16_t opcode, const Json &request,
                                     std::uint32_t sequence, const WorldPosition &position,
                                     std::int64_t ticks) {
  const auto world_lock = lock_world_mobs();
  PlayerMotionOutcome result;
  const bool rotating = opcode == world_request::kAdaptedRotationRequest;
  const auto &motion = rotating ? request : request.value("root_motion", Json::object());
  const auto instance = motion.value("skill_instance_id", std::uint32_t{});
  const auto fail = [&](const char *error) {
    if (rotating) {
      // The recovered failure field is int32 even though the request uses uint32.
      const int reply_instance =
          instance <= static_cast<std::uint32_t>(std::numeric_limits<int>::max())
              ? static_cast<int>(instance)
              : 0;
      result.own.push_back(
          {65, {{"ec", error}, {"skill_instance_id", reply_instance}, {"yaw", position.yaw}}});
    } else {
      result.own.push_back(
          {2, {{"ec", error}, {"packet_seq", {{"seq", std::to_string(sequence)}}}}});
    }
    return result;
  };
  if (!action_ || action_->instance != instance || player_dead()) {
    return fail("Skill_NotUsingSkill");
  }
  if (!owns_encounter_map(position.map_id) ||
      motion.value("stage_index", 0u) != static_cast<unsigned>(action_->stage) ||
      (rotating && motion.value("layer_index", 0u) != static_cast<unsigned>(action_->layer))) {
    return fail("Skill_InvalidStage");
  }

  const auto &definition = stage(*action_);
  const auto wire_end = wire_int64(motion.value("end_tick", Json("0")));
  const auto duration = definition.at("duration_ms").get<std::int64_t>();
  const auto stage_end = action_->started + duration * kTicksPerMillisecond;
  // Local network tolerance; deadlines never come from unbounded client timestamps.
  constexpr auto tolerance = 500 * kTicksPerMillisecond;
  if (!wire_end || *wire_end < 0 || *wire_end / 100 < ticks - tolerance ||
      *wire_end / 100 > stage_end + tolerance) {
    return fail("BadRequest");
  }

  if (rotating) {
    const double yaw = request.value("end_yaw", 0.0);
    const auto activation = wire_int64(request.value("activate_tick", Json("0")));
    if (!std::isfinite(yaw) || std::abs(yaw) > 3600 || !activation || *activation < 0 ||
        *activation > *wire_end || !definition.contains("adapted_rotations")) {
      return fail("BadRequest");
    }
    int selected = -1;
    const auto &windows = definition.at("adapted_rotations");
    for (std::size_t index = 0; index < windows.size(); ++index) {
      const auto &window = windows[index];
      const auto start =
          action_->started + window.at("startTime").get<int>() * kTicksPerMillisecond;
      const auto end = action_->started + window.at("endTime").get<int>() * kTicksPerMillisecond;
      if (*activation / 100 >= start - tolerance && *activation / 100 <= end + tolerance &&
          *wire_end / 100 <= end + tolerance) {
        selected = static_cast<int>(index);
        break;
      }
    }
    if (selected < 0) {
      return fail("Skill_InvalidStage");
    }
    if (!action_->adapted_rotation_windows.insert(selected).second) {
      return result;
    }
    action_->yaw = static_cast<float>(std::remainder(yaw, 360.0));
    result.observers.push_back(
        {66,
         {{"unit_id", std::to_string(player_id_)},
          {"skill_instance_id", instance},
          {"stage_index", action_->stage},
          {"layer_index", action_->layer},
          {"end_yaw", action_->yaw},
          {"end_tick", wire_monotonic_tick(std::min(*wire_end / 100, stage_end))}}});
    return result;
  }

  const auto authored = definition.find("player_motion");
  if (authored == definition.end() || !motion.contains("start_pos") ||
      !motion.contains("dest_pos")) {
    return fail("Skill_InvalidStage");
  }
  const auto start = wire_point(motion.at("start_pos"), position.map_id);
  const auto destination = wire_point(motion.at("dest_pos"), position.map_id);
  const auto displacement = std::sqrt(distance_squared(start, destination));
  const auto yaw = motion.value("start_yaw", 0.0);
  const auto rotation = motion.value("rotation", Json::object());
  const auto end_yaw = rotation.value("yaw", 0.0);
  const auto target = wire_int64(motion.value("target_unit_id", Json("0")));

  // Name the failed check in the log; the generic refusal line only records the error code.
  // Distances only: positions and unit ids stay out of the log.
  const auto refuse_report = [&](std::string_view reason) {
    spdlog::warn("Player root motion refused skill_id={} stage={} reason={}", action_->skill_id,
                 action_->stage, reason);
    return fail("BadRequest");
  };
  if (distance_squared(start, position) > kPositionToleranceMm * kPositionToleranceMm) {
    return refuse_report("start_far_from_player");
  }
  const auto reach = player_motion_reach(*authored, start, destination);
  if (!reach.within) {
    spdlog::warn("Player root motion refused skill_id={} stage={} reason=beyond_authored_reach "
                 "horizontal_mm={:.0f} height_change_mm={:.0f} allowed_mm={:.0f}",
                 action_->skill_id, action_->stage, reach.horizontal_mm, reach.height_change_mm,
                 reach.allowed_mm);
    return fail("BadRequest");
  }
  if (!std::isfinite(yaw) || !std::isfinite(end_yaw) || std::abs(yaw) > 3600 ||
      std::abs(end_yaw) > 3600) {
    return refuse_report("invalid_yaw");
  }
  if (!target || *target < 0 ||
      (*target != 0 && static_cast<std::uint64_t>(*target) != action_->target)) {
    return refuse_report("target_mismatch");
  }
  const auto window_start =
      action_->started + authored->at("start_ms").get<int>() * kTicksPerMillisecond;
  const auto window_end =
      action_->started + authored->at("end_ms").get<int>() * kTicksPerMillisecond;
  if (ticks < window_start - tolerance || ticks > window_end + tolerance ||
      *wire_end / 100 > window_end + tolerance) {
    return fail("Skill_InvalidStage");
  }
  if (action_->player_motion_received) {
    return result;
  }
  const auto length = authored->at("path_length_mm").get<double>();
  const double scale =
      authored->at("using_scale").get<bool>() && length > 0 ? displacement / length : 1.0;
  if (!std::isfinite(scale) || scale > 100) {
    return fail("BadRequest");
  }
  Json accepted{{"start_pos", wire_position(start)},
                {"dest_pos", wire_position(destination)},
                {"end_tick", wire_monotonic_tick(std::min(*wire_end / 100, window_end))},
                {"rotation", {{"yaw", std::remainder(end_yaw, 360.0)}}},
                {"start_yaw", std::remainder(yaw, 360.0)},
                {"scale", scale},
                {"ground_move", authored->at("ground_move")},
                {"skill_instance_id", instance},
                {"stage_index", action_->stage},
                {"target_unit_id", std::to_string(*target)},
                // All recovered player requests leave this optional duration unset.
                {"rotate_duration", "0"}};
  action_->player_motion_received = true;
  result.observers.push_back(
      {64, {{"unit_id", std::to_string(player_id_)}, {"root_motion", std::move(accepted)}}});
  return result;
}

std::vector<std::uint64_t> CombatSession::targets(const Json &effect, const Action &action,
                                                  std::uint64_t caster, const WorldPosition &origin,
                                                  const WorldPosition &player_position,
                                                  const Json *event_shape) const {
  if (effect.at("targeting").value("refType", "") == "EX5ReferenceTargetType_Self") {
    return {caster};
  }
  const double range = std::max(1000, data_->skills.at(action.skill_id).at("range_mm").get<int>());
  // Shapes are tested against the target's game-hit capsule, as the client's collision does.
  if (caster != player_id_) {
    const auto player_volume = player_hit_volume(*data_);
    if (stat_value(stats_, kHealth) > 0 &&
        within_shape(effect, origin, action.yaw, player_position, range, event_shape,
                     player_volume)) {
      return {player_id_};
    }
    return {};
  }
  // Select only server-owned, living attackable monsters in this encounter. Civilians,
  // another player's private test NPCs and arbitrary official unit IDs are excluded.
  std::vector<std::pair<double, std::uint64_t>> candidates;
  for (const auto &[id, monster] : monsters()) {
    if (!is_live_monster(id) ||
        !within_shape(effect, origin, action.yaw, monster.position, range, event_shape,
                      npc_hit_volume(*data_, monster.cms_id))) {
      continue;
    }
    const double distance = distance_squared(origin, monster.position);
    candidates.emplace_back(id == action.target ? -1 : distance, id);
  }
  std::ranges::sort(candidates);
  const int limit =
      std::clamp(static_cast<int>(effect.at("targeting").value("maxTargetCount", 1.0)), 1, 32);
  std::vector<std::uint64_t> result;
  for (const auto &[distance, id] : candidates) {
    if (result.size() >= static_cast<std::size_t>(limit)) {
      break;
    }
    result.push_back(id);
  }
  return result;
}

void CombatSession::finish(std::optional<Action> &action, std::uint64_t caster,
                           const WorldPosition &position, std::int64_t ticks,
                           std::vector<GameplayPacket> &packets, const char *reason) {
  if (!action) {
    return;
  }
  if (caster == player_id_) {
    retain_player_projectile_immunity(*action, ticks);
  }

  // An interrupted lunge retargets the client's in-flight path to where the unit really is.
  if (action->lunge && !action->lunge->end_sent) {
    emit_lunge_end(caster, *action, position, ticks, packets);
  }
  packets.push_back({kEndSkillNotification,
                     {{"unit_id", std::to_string(caster)},
                      {"end_skill",
                       {{"instance_id", action->instance},
                        {"pos", wire_position(position)},
                        {"yaw", position.yaw},
                        {"tick", wire_monotonic_tick(ticks)},
                        {"reason", reason}}}}});
  const auto finished_instance = action->instance;
  action.reset();
  if (caster == player_id_) {
    cancel_action_recoveries(finished_instance, ticks, packets);
  }
}

int CombatSession::adjust_vital(int stat, std::int64_t delta) {
  const int maximum =
      stat == kHealth ? stat_value(stats_, kMaxHealth) : stat_value(stats_, kMaxStamina);
  const int previous = stat_value(stats_, stat);
  const auto result =
      std::clamp<std::int64_t>(static_cast<std::int64_t>(previous) + delta, 0, maximum);
  stats_[std::to_string(stat)] = static_cast<int>(result);
  return static_cast<int>(result);
}

void CombatSession::change_stat(int stat, int delta, std::int64_t ticks,
                                std::vector<GameplayPacket> &packets) {
  const int previous = stat_value(stats_, stat);
  const int result = adjust_vital(stat, delta);
  if (result == previous) {
    return;
  }
  packets.push_back(
      {kChangeStatNotification,
       {{"unit_id", std::to_string(player_id_)},
        {"tick", wire_monotonic_tick(ticks)},
        {"stat_changes",
         {{{"stat_type", stat}, {"delta", result - previous}, {"result", result}}}}}});
}

void CombatSession::start(const Json &request, const WorldPosition &position, std::int64_t ticks,
                          std::vector<GameplayPacket> &packets) {
  const auto &wire = request.at("start_skill");
  const int skill_id = wire.at("skill_id");
  const auto instance = wire.at("instance_id").get<std::uint32_t>();
  const auto found = data_->skills.find(skill_id);
  require(found != data_->skills.end(), "Skill_InvalidCmsId");
  const auto &skill = found->second;
  require(std::ranges::any_of(skill.at("classes"),
                              [&](const Json &entry) { return entry.get<int>() == class_id_; }),
          "Skill_InvalidCmsId");
  require(skill.at("weapon") == "None" || skill.at("weapon") == weapon_, "Skill_InvalidCmsId");
  const int item_cms_id = used_item(request, skill_id, skill);
  require(stat_value(stats_, kHealth) > 0, "Skill_UsingSkill");
  require(instance > last_instance_, "Skill_UsingSkill");
  require(matches_position(wire, position), "TooFar");
  const float yaw = wire.value("rotation", Json::object()).value("yaw", position.yaw);
  require(std::isfinite(yaw), "BadRequest");
  const int layer_index = wire.value("layer_index", 0);
  const auto *layer = by_index(skill.at("layers"), layer_index);
  require(layer && by_index(layer->at("stages"), 0), "Skill_InvalidCmsId");
  const int cooldown_key = skill_cooldown_key(skill_id, skill);
  require(!cooldowns_.contains(cooldown_key) || cooldowns_.at(cooldown_key) <= ticks,
          "Skill_Cooldown");
  std::uint64_t target{};
  if (request.contains("lock_on_target_id") && request.at("lock_on_target_id") != "0") {
    target = unit_id(request.at("lock_on_target_id"));
  }
  const auto &target_ids = request.value("target_ids", Json::array());
  require(target_ids.is_array() && target_ids.size() <= 16, "Skill_InvalidTarget");
  if (!target_ids.empty()) {
    target = unit_id(target_ids.at(0));
  }
  if (target != 0) {
    const auto monster = monsters().find(target);
    const double range = skill.at("range_mm").get<int>();
    const double minimum = skill.value("min_range_mm", 0);
    require(
        monster != monsters().end() && is_live_monster(target) &&
            distance_squared(position, monster->second.position) >= minimum * minimum &&
            (range <= 0 || distance_squared(position, monster->second.position) <= range * range),
        "Skill_InvalidTarget");
  }
  require(!skill.at("require_target").get<bool>() || target != 0, "Skill_InvalidTarget");
  bool may_cancel = !action_;
  if (action_) {
    const auto elapsed_ms = (ticks - action_->started) / kTicksPerMillisecond;
    for (const auto &notify : stage(*action_).at("notifies")) {
      if (elapsed_ms < notify.at("startTime").get<int>() ||
          elapsed_ms > notify.at("endTime").get<int>()) {
        continue;
      }
      if (notify.at("notifyType") == "cancelable") {
        may_cancel = may_cancel || ((notify.at("cancelable").at("skillTypeFlags").get<int>() &
                                     skill_type_flag(skill.at("type"))) != 0);
      }
      if (notify.at("notifyType") == "combo") {
        for (const auto &combo : notify.at("combo").at("comboNotifyDatas")) {
          may_cancel = may_cancel || combo.at("nextSkillId") == skill_id;
        }
      }
    }
  }
  require(may_cancel, "Skill_UsingSkill");
  const int sp_cost = static_cast<int>(std::ceil(skill.at("costs").value("SP", 0.0)));
  const int hp_cost = static_cast<int>(std::ceil(skill.at("costs").value("HP", 0.0)));
  // Observed: official refusals for a skill whose SP cost exceeds current SP carry
  // Skill_Condition_NotEnoughCost (skill 1418, SP:10). Official HP-cost shortage was
  // not captured, so that branch keeps its previous code until evidence exists.
  require(stat_value(stats_, kStamina) >= sp_cost, "Skill_Condition_NotEnoughCost");
  require(stat_value(stats_, kHealth) > hp_cost, "Skill_Condition_NotEnoughSP");
  finish(action_, player_id_, position, ticks, packets, "EX5EndSkillReason_Default");
  Json changes = Json::array();
  for (const auto &[stat, cost] : {std::pair{kStamina, sp_cost}, std::pair{kHealth, hp_cost}}) {
    if (cost > 0) {
      const int value = stat_value(stats_, stat) - cost;
      stats_[std::to_string(stat)] = value;
      changes.push_back({{"stat_type", stat}, {"delta", -cost}, {"result", value}});
    }
  }
  const auto cooldown_end = ticks + skill.at("cooldown_ms").get<int>() * kTicksPerMillisecond;
  cooldowns_[cooldown_key] = cooldown_end;
  packets.push_back({53,
                     {{"skill_instance_id", instance},
                      {"tick", wire_monotonic_tick(ticks)},
                      {"stat_changes", std::move(changes)},
                      {"cooldowns",
                       {{{"group_id", skill.at("cooldown_group")},
                         {"expire_tick", wire_monotonic_tick(cooldown_end)}}}}}});
  Json start = wire;
  start["tick"] = wire_monotonic_tick(ticks);
  start["pos"] = wire_position(position);
  packets.push_back(
      {14,
       {{"unit_id", std::to_string(player_id_)},
        {"start_skill", std::move(start)},
        {"weapon_slot_type", active_weapon_slot_},
        {"weapon_state_type", skill.at("type") == "SheathWeapon" ? "EX5WeaponStateType_Sheathed"
                                                                 : "EX5WeaponStateType_Drawn"}}});
  action_ = Action{skill_id, instance, layer_index, 0, ticks, target, yaw};
  action_->item_cms_id = item_cms_id;
  last_instance_ = instance;
  stamina_resume_tick_ = ticks + kTicksPerSecond;
  regeneration_tick_ = ticks + kTicksPerSecond;
  // An accepted skill start counts toward CounterUseSkill objectives.
  quest_events_.push_back({QuestEventKind::SkillUsed, skill_id, 0, 1});
}

void CombatSession::refuse_stage_change(std::uint32_t instance, const char *error_code,
                                        const char *end_reason, const WorldPosition &position,
                                        std::int64_t ticks, std::vector<GameplayPacket> &packets) {
  // The captured official refusal carried only the error and the instance.
  packets.push_back(
      {kChangeSkillStageResponse, {{"ec", error_code}, {"skill_instance_id", instance}}});

  // A stale report must never end a newer action of the same caster.
  if (!action_ || action_->instance != instance) {
    return;
  }

  // Fail closed: the client already left the refused stage locally. Keeping the
  // server copy would hold the caster "using a skill" for the stage deadline,
  // which is 99 seconds for a landing skill waiting for its ground contact.
  finish(action_, player_id_, position, ticks, packets, end_reason);
}

void CombatSession::change_stage(const Json &request, const WorldPosition &position,
                                 std::int64_t ticks, std::vector<GameplayPacket> &packets) {
  const auto instance = stage_request_instance(request);
  const int requested_stage = requested_stage_index(request);
  const auto reason = stage_transition_reason(request);
  const char *end_reason = stage_refusal_end_reason(reason);

  // Captured official behavior: a report for an action that already ended is
  // answered with Skill_NotUsingSkill.
  if (!action_ || action_->instance != instance) {
    refuse_stage_change(instance, "Skill_NotUsingSkill", end_reason, position, ticks, packets);
    return;
  }

  // The server deadline can apply a default transition just before the
  // client's report of the same transition arrives. That report is redundant.
  if (requested_stage == action_->stage) {
    return;
  }

  if (requested_stage < 0 || !find_stage(*action_, requested_stage)) {
    refuse_stage_change(instance, "Skill_InvalidStage", end_reason, position, ticks, packets);
    return;
  }

  if (!matches_position(request, position)) {
    refuse_stage_change(instance, "TooFar", end_reason, position, ticks, packets);
    return;
  }

  const auto elapsed_ms = (ticks - action_->started) / kTicksPerMillisecond;
  const auto decision = decide_stage_request(stage(*action_), reason, requested_stage, elapsed_ms);
  if (decision.outcome == StageRequestOutcome::Defer) {
    return;
  }

  if (decision.outcome == StageRequestOutcome::Reject) {
    refuse_stage_change(instance, decision.error_code, end_reason, position, ticks, packets);
    return;
  }

  // Accepted transitions are silent to their owner. More than one hundred
  // captured official acceptances carried neither PS2C_ChangeSkillStageRes nor
  // an owner PS2C_ChangeSkillStageNoti; the client already entered the stage.
  enter_stage(*action_, requested_stage, ticks);
}

std::vector<GameplayPacket> CombatSession::receive_impl(std::uint16_t opcode, const Json &request,
                                                   const WorldPosition &position,
                                                   std::int64_t ticks) {
  std::vector<GameplayPacket> packets;
  // CombatPlayerSession routes reports through receive_projectile_report, which validates
  // ownership and delivers observer corrections. They never enter the skill-start budget or
  // award damage; server flight timers own every hit.
  if (opcode == kUpdateProjectileRequest || opcode == kClientProjectileHitRequest) {
    return packets;
  }

  try {
    require(owns_encounter_map(position.map_id), "BadRequest");
    if (ticks - request_window_ >= kTicksPerSecond) {
      request_window_ = ticks;
      requests_in_window_ = 0;
    }
    require(++requests_in_window_ <= 64, "BadRequest");
    if (opcode == kStartSkillRequest) {
      start(request, position, ticks, packets);
    } else if (opcode == kChangeSkillStageRequest) {
      change_stage(request, position, ticks, packets);
    } else if (opcode == kSkillEffectReportRequest || opcode == kExecuteSpawnProjectileRequest) {
      require(action_ && action_->instance == request.at("skill_instance_id"), "Skill_UsingSkill");
      require(matches_position(request, position), "TooFar");
      const auto &report = request.at("skill_effect");
      const bool known = std::ranges::any_of(stage(*action_).at("events"), [&](const auto &event) {
        return event.at("effect_id") == report.at("effect_id") &&
               event.at("unique_id") == report.value("unique_effect_id", 0);
      });
      require(known, "BadRequest");

      // Other effect reports are diagnostic hints only; the session timer executes each event.
      // A client-driven projectile, however, exists only once its client announces it, and a
      // targeted or aimed area takes its placement from the report.
      if (opcode == kExecuteSpawnProjectileRequest) {
        spawn_client_projectiles(request, position, ticks, packets);
      } else {
        record_placement_hint(request, position);
      }
    } else if (opcode == kDespawnClientProjectileRequest) {
      despawn_client_projectile(request, packets);
    }
  } catch (const CombatError &failure) {
    packets.clear();
    // Diagnostic for live sessions: which player requests combat refused, and why. The fields
    // are read without throwing because the refused request may itself be malformed.
    const auto number_field = [](const Json &object, const char *key) -> std::int64_t {
      if (!object.is_object() || !object.contains(key) || !object.at(key).is_number_integer()) {
        return 0;
      }
      return object.at(key).get<std::int64_t>();
    };
    const Json start_skill =
        request.is_object() ? request.value("start_skill", Json::object()) : Json::object();
    spdlog::info("Combat refused player request opcode={} skill_id={} skill_instance_id={} ec={}",
                 opcode, number_field(start_skill, "skill_id"),
                 std::max(number_field(start_skill, "instance_id"),
                          number_field(request, "skill_instance_id")),
                 failure.code);
    if (opcode == kStartSkillRequest) {
      packets.push_back({13,
                         {{"ec", failure.code},
                          {"skill_id", request.at("start_skill").value("skill_id", 0)},
                          {"instance_id", request.at("start_skill").value("instance_id", 0u)},
                          {"weapon_slot_type", active_weapon_slot_},
                          {"weapon_state_type", "EX5WeaponStateType_Drawn"}}});
    } else if (opcode == kChangeSkillStageRequest) {
      // A stage request refused before validation (map or rate limit) must not
      // leave its action in use until a long stage deadline.
      refuse_stage_change(stage_request_instance(request), failure.code,
                          stage_refusal_end_reason(stage_transition_reason(request)), position,
                          ticks, packets);
    } else if (opcode == kExecuteSpawnProjectileRequest) {
      // Observed: the official server answers a refused client flight with one
      // SpawnClientProjectileFailNoti per projectile 22-74 ms later (15 of 15 in P2), so the
      // client removes the flight it already launched.
      append_client_projectile_refusals(request, packets);
    }
  } catch (const Json::exception &) {
    packets.clear();
    packets.push_back({2, {{"ec", "BadRequest"}}});
  } catch (const std::invalid_argument &) {
    packets.clear();
    packets.push_back({2, {{"ec", "BadRequest"}}});
  }
  return packets;
}

void CombatSession::apply_effect(int effect_id, const Action &action, std::uint64_t caster,
                                 std::uint64_t explicit_target, const WorldPosition &origin,
                                 const WorldPosition &player_position, std::int64_t ticks,
                                 std::vector<GameplayPacket> &packets, int depth,
                                 const Json *event_shape,
                                 std::optional<std::uint64_t> projectile_instance,
                                 std::optional<std::uint64_t> area_unit,
                                 const HitImmunity *retired_player_immunity) {
  if (depth >= 8 || !data_->effects.contains(std::to_string(effect_id))) {
    return;
  }
  const auto &effect = data_->effects.at(std::to_string(effect_id));
  const auto kind = effect.at("kind").get<std::string>();
  const auto &parameters = effect.at("parameters");
  // Every executed effect of the player's own skills counts toward CounterUseSkillEffect.
  if (caster == player_id_ && depth == 0) {
    quest_events_.push_back({QuestEventKind::SkillEffectExecuted, effect_id, 0, 1});
  }
  // An area is placed once per effect, also when no enemy is in reach yet (a balm is spread at
  // the user's feet); its own interval then finds the targets.
  if (kind == "spawnAreaOfEffect") {
    // An area left by a projectile (its hit or lifetime end) belongs where the flight ended: on
    // the struck unit, or at the impact point when it struck none.
    const bool at_impact = projectile_instance.has_value();
    auto target = explicit_target;
    if (target == 0 && !at_impact) {
      target = action.target;
    }
    spawn_area(effect, parameters, action, caster, target, origin, player_position, ticks, packets,
               at_impact);
    return;
  }
  if (kind == "spawnProjectile") {
    const auto selected = targets(effect, action, caster, origin, player_position, event_shape);
    std::uint64_t target = explicit_target;
    if (target == 0 && !selected.empty()) {
      target = selected.front();
    }
    // A server-driven player flight aimed at the client's ground marker (Dragon's Roar) flies to
    // the reported aim point instead of homing on a unit.
    std::optional<WorldPosition> aimed_destination;
    const auto position_type = effect.value("targeting_purpose", Json::object())
                                   .value("targetingPosType", std::string{});
    if (caster == player_id_ && position_type == "EX5TargetingPosType_Aim") {
      aimed_destination = reported_aim_point(action, origin);
      // Diagnostic for live sessions: whether the client reported the aimed marker.
      const char *source = "forward";
      if (aimed_destination) {
        source = "reported_aim";
      } else if (target != 0) {
        source = "target";
      }
      spdlog::info("Combat aimed projectile skill_id={} effect_unique_id={} source={}",
                   action.skill_id, action.effect_unique_id, source);
    }
    // A spawn is one authored event, even when its effect can select several enemies or none.
    spawn_server_projectile(parameters, action, caster, target, origin, player_position, ticks,
                            packets, aimed_destination);
    const int chain = effect.at("chain");
    if (chain > 0) {
      apply_effect(chain, action, caster, target, origin, player_position, ticks, packets,
                   depth + 1, nullptr, projectile_instance, area_unit, retired_player_immunity);
    }
    return;
  }
  std::vector<std::uint64_t> selected;
  if (effect.at("targeting").value("refType", std::string{}) == "EX5ReferenceTargetType_Self") {
    selected = {caster};
  } else if (explicit_target != 0) {
    selected = {explicit_target};
  } else {
    selected = targets(effect, action, caster, origin, player_position, event_shape);
  }
  for (const auto target : selected) {
    if (target == caster && retired_projectile_caster(action, caster)) {
      // A self-targeted effect cannot affect a different monster that reused the old unit ID.
      continue;
    }
    if (target != player_id_ &&
        !is_live_monster(target)) {
      continue;
    }
    if (kind == "damage") {
      if ((caster == player_id_) == (target == player_id_)) {
        continue;
      }
      // A game master with god mode takes no hit at all (no damage packet, no Demise state).
      if (target == player_id_ && gm_invulnerable_) {
        continue;
      }
      // Official rule (84 of 84 captured dodge immunities): a hit that lands while the target's
      // running stage has an immune window granting Dodge is cancelled and answered with
      // ImmuneNoti{dodge}; Damage immunity likewise with ImmuneNoti{damage}. It is checked before
      // the overlap group, because a later event of the same group still hit after a dodge in
      // four captured casts.
      auto immunity = hit_immunity(target, ticks);
      if (target == player_id_ && projectile_instance && retired_player_immunity) {
        immunity.dodge = immunity.dodge || retired_player_immunity->dodge;
        immunity.damage = immunity.damage || retired_player_immunity->damage;
        immunity.names.insert(retired_player_immunity->names.begin(),
                              retired_player_immunity->names.end());
      }
      if (immunity.dodge || immunity.damage) {
        auto immune_notification =
            hit_header(effect_id, action, caster, target, ticks, projectile_instance, area_unit);
        immune_notification[immunity.dodge ? "dodge" : "damage"] = true;
        packets.push_back({kImmuneNotification, std::move(immune_notification)});
        continue;
      }
      // Official rule (capture-verified, 772 of 772 samples): a damage effect whose catalog row has
      // an overlapGroupId lands once per target per cast, however many stage events reference it.
      // Effects without a group land at every event. Only a target that was actually selected
      // here (inside the shape, alive, on the opposing side) consumes the group, and the group id
      // is shared by every effect id of the cast.
      const auto group = effect.value("overlap_group", std::string{});
      const auto overlap_key = std::make_tuple(caster, action.instance, area_unit.value_or(0),
                                               area_unit ? ticks : 0);
      if (!group.empty() &&
          !overlap_hits_[overlap_key].hits.emplace(group, target).second) {
        continue;
      }
      const auto packets_before_hit = packets.size();
      auto &target_stats = target == player_id_ ? stats_ : monsters().at(target).stats;
      const int damage = hit_damage(parameters, action, caster, target);
      const auto previous_monster_shield = target == player_id_ ? std::optional<ActiveShield>{}
                                                               : monsters().at(target).shield;
      const int previous_shield_value = stat_value(target_stats, kShieldStat);
      Json shield_changes = Json::array();
      const int absorbed = absorb_with_shield(target, damage, shield_changes);
      const int health_loss = damage - absorbed;
      const int previous = stat_value(target_stats, kHealth);
      const int result = std::max(0, previous - health_loss);
      target_stats["69"] = result;
      if (caster == player_id_ && target != player_id_ && damage > 0 && result > 0) {
        record_world_monster_engagement(target);
      }
      // Diagnostic: names the NPC on each damage exchange (who was hit, who hit back).
      if (caster == player_id_) {
        spdlog::info("Combat damage player_to_npc target_unit_id={} npc_cms_id={} skill_id={} "
                     "skill_effect_id={} damage={} npc_hp={}",
                     target, monsters().at(target).cms_id, action.skill_id, effect_id, damage, result);
      } else {
        const auto *retired_caster = retired_projectile_caster(action, caster);
        const int caster_cms_id = retired_caster ? retired_caster->cms_id
                                                  : monsters().at(caster).cms_id;
        spdlog::info("Combat damage npc_to_player caster_unit_id={} npc_cms_id={} skill_id={} "
                     "skill_effect_id={} damage={} player_hp={}",
                     caster, caster_cms_id, action.skill_id, effect_id, damage, result);
      }
      if (target == player_id_) {
        regeneration_tick_ = ticks + kTicksPerSecond;
      }
      // Official rule (804 captured reaction immunities, 767 ahead of their DamageNoti): a hit
      // on a target whose immunity covers the hit's reaction category still deals damage, and
      // ImmuneNoti names the category's first level (Sway2 -> Sway1).
      const auto suppressed_reaction = immune_reaction(parameters, immunity.names);
      if (suppressed_reaction) {
        auto immune_notification =
            hit_header(effect_id, action, caster, target, ticks, projectile_instance, area_unit);
        immune_notification["reaction"] = *suppressed_reaction;
        packets.push_back({kImmuneNotification, std::move(immune_notification)});
      }
      // Observed (3 of 3 official local-player deaths): the Demise state precedes the lethal
      // DamageNoti, which is followed by the DeathNoti, all at the same tick.
      if (target == player_id_ && result == 0 && previous > 0) {
        packets.push_back({32,
                           {{"unit_id", std::to_string(target)},
                            {"state", "EX5UnitStateType_Demise"},
                            {"tick", wire_monotonic_tick(ticks)}}});
      }
      auto damage_notification =
          hit_header(effect_id, action, caster, target, ticks, projectile_instance, area_unit);
      damage_notification["base_damage_amount"] = damage;
      damage_notification["final_damage_amount"] = health_loss;
      Json stat_changes = Json::array(
          {{{"stat_type", kHealth}, {"delta", result - previous}, {"result", result}}});
      for (auto &change : shield_changes) {
        stat_changes.push_back(std::move(change));
      }
      damage_notification["stat_changes"] = std::move(stat_changes);
      // Observed (59 official samples): a hit the shield absorbs completely is typed Shield.
      if (absorbed > 0 && health_loss == 0) {
        damage_notification["damage_type"] = "EX5DamageType_Shield";
      }
      // Observed: every captured player projectile hit (127 of 127 in P2) and most NPC ones name
      // the flight instance with the Projectile origin.
      if (projectile_instance) {
        damage_notification["skill_effect_origin_type"] = "EX5SkillEffectOriginType_Projectile";
      }
      // Observed (official poison-bottle cloud): an area hit names the area unit as caster and
      // the thrower as base caster.
      if (area_unit) {
        damage_notification["skill_effect_origin_type"] = "EX5SkillEffectOriginType_AreaOfEffect";
      }
      packets.push_back({19, std::move(damage_notification)});
      // A hit that leaves its target alive may also attach the effect's status and build the
      // target's ailment gauges (abnormalStatus*Attack).
      if (result > 0) {
        if (effect.contains("status_effect_id")) {
          apply_status(target, caster, effect.at("status_effect_id").get<int>(), ticks, packets);
        }
        for (const auto &[type, attack] : combat_rules::abnormal_attacks(parameters)) {
          const auto gain = static_cast<int>(
              std::lround(attack * combat_rules::kGaugeGainPerAttack));
          change_abnormal_gauge(target, type, gain, caster, ticks, packets);
        }
      }
      if (result == 0 && previous > 0) {
        if (target == player_id_) {
          packets.push_back({23,
                             {{"base_caster_unit_id", std::to_string(caster)},
                              {"target_unit_id", std::to_string(target)},
                              {"pos", wire_position(player_position)}}});
          finish_player_death(ticks, packets);
        } else {
          try {
            defeat_monster(target, caster, ticks, packets);
          } catch (...) {
            // If the in-memory eligibility snapshot cannot be captured, do not leave an
            // unpublished monster dead or consume its shield before the player can retry. A
            // projectile or area may already have hit other units, so cancel only this contact
            // rather than replaying the entire attack and damaging those units twice.
            if (!world_defeat_capture_ || !world_mobs_ || !monsters().contains(target) ||
                monsters().at(target).private_owner != 0) {
              throw;
            }
            auto &restored = monsters().at(target);
            restored.stats["69"] = previous;
            if (restored.stats.contains("71")) {
              restored.stats["71"] = previous_shield_value;
            }
            restored.shield = previous_monster_shield;
            if (!group.empty()) {
              const auto overlap = overlap_hits_.find(overlap_key);
              if (overlap != overlap_hits_.end()) {
                overlap->second.hits.erase({group, target});
              }
            }
            packets.resize(packets_before_hit);
            auto cancelled =
                hit_header(effect_id, action, caster, target, ticks, projectile_instance, area_unit);
            cancelled["damage"] = true;
            packets.push_back({kImmuneNotification, std::move(cancelled)});
            if (pending_world_defeats_.size() < kMaximumPendingWorldDefeats) {
              spdlog::error("World monster lethal hit cancelled because reward capture failed");
            }
            continue;
          }
        }
      } else if (target != player_id_ && monsters().at(target).ai) {
        // AI monsters acquire their attacker on the next AI step and announce it themselves.
        monsters().at(target).damaged = true;
        if (world_mobs_ && monsters().at(target).simulation_owner == 0 &&
            !monsters().at(target).awaiting_target_transfer) {
          monsters().at(target).simulation_owner = player_id_;
        }
      } else if (target != player_id_ && !monsters().at(target).aggro) {
        auto &monster = monsters().at(target);
        monster.aggro = true;
        if (world_mobs_ && monster.simulation_owner == 0) {
          monster.simulation_owner = player_id_;
        }
        monster.next_attack = ticks + kTicksPerSecond;
        packets.push_back({32,
                           {{"unit_id", std::to_string(target)},
                            {"state", "EX5UnitStateType_Combat"},
                            {"tick", wire_monotonic_tick(ticks)}}});
      }
    } else if ((kind == "restoreHP" || kind == "heal") && target == player_id_ &&
               caster == player_id_) {
      const double amount = parameters.value("fixedHeal", parameters.value("value", 0.0)) +
                            parameters.value("magicCoefficient", 0.0) * stat_value(stats_, 17);
      change_stat(kHealth, static_cast<int>(std::clamp(amount, 0.0, 1000000.0)), ticks, packets);
    } else if (kind == "useItem" && target == player_id_ && caster == player_id_) {
      if (!spend_action_item(action, player_position, ticks, packets)) {
        return;
      }
    } else if (kind == "addStatusEffect" &&
               effect.at("targeting").value("refType", "") == "EX5ReferenceTargetType_Self") {
      if (target == caster && target == player_id_) {
        add_status_effect(parameters, target, caster, ticks, packets);
      }
    } else if (kind == "addStatusEffect" && caster == player_id_ && target != player_id_) {
      // A debuff the player places on a monster it reaches.
      const auto before = packets.size();
      add_status_effect(parameters, target, caster, ticks, packets);
      if (packets.size() != before) {
        record_world_monster_engagement(target);
      }
    } else if (kind == "removeStatusEffect" && target == player_id_ && caster == player_id_ &&
               effect.at("targeting").value("refType", "") == "EX5ReferenceTargetType_Self") {
      remove_status_effect(parameters, target, ticks, packets);
    } else if (kind == "addAbnormalGauge") {
      // Cures lower the user's own gauges; hits such as the dagger's bleed raise the target's.
      for (const auto &[type, change] : combat_rules::abnormal_gauge_adds(parameters)) {
        const auto before = packets.size();
        change_abnormal_gauge(target, type, change, caster, ticks, packets);
        if (caster == player_id_ && target != player_id_ && change > 0 &&
            packets.size() != before) {
          record_world_monster_engagement(target);
        }
      }
    } else if (kind == "statRecovery" && target == player_id_ && caster == player_id_) {
      start_stat_recovery(effect_id, parameters, action, ticks, packets);
    } else if ((kind == "changeWeaponSkillSlot" || kind == "skillCooldownReset") &&
               target == player_id_ && caster == player_id_) {
      apply_skill_slot_effect(effect_id, effect, ticks, packets);
    } else if (kind == "addShield" && target == caster) {
      // Every catalog addShield and dispelShield row targets its own caster.
      add_shield(effect, target, caster, ticks, packets);
    } else if (kind == "dispelShield" && target == caster) {
      clear_shield(target, ticks, packets);
    }
    const int chain = effect.at("chain");
    if (chain > 0) {
      apply_effect(chain, action, caster, target, origin, player_position, ticks, packets,
                   depth + 1, nullptr, projectile_instance, area_unit, retired_player_immunity);
    }
  }
}

const CombatSession::NpcProjectileCaster *
CombatSession::retired_projectile_caster(const Action &action, std::uint64_t caster) const {
  if (!action.npc_projectile_caster) {
    return nullptr;
  }
  const auto monster = monsters().find(caster);
  if (monster != monsters().end() &&
      monster->second.life_identity == action.npc_projectile_caster->life) {
    return nullptr;
  }
  return action.npc_projectile_caster.get();
}

int CombatSession::hit_damage(const Json &parameters, const Action &action, std::uint64_t caster,
                              std::uint64_t target) const {
  const auto *retired_caster = retired_projectile_caster(action, caster);
  const Json *caster_stats = &stats_;
  if (retired_caster) {
    caster_stats = &retired_caster->stats;
  } else if (caster != player_id_) {
    caster_stats = &monsters().at(caster).stats;
  }
  const auto &target_stats = target == player_id_ ? stats_ : monsters().at(target).stats;
  // Profiles and fixtures without attack-power ratings keep the earlier local model, so their
  // tuned health values still behave as before.
  if (!combat_rules::has_attack_ratings(*caster_stats)) {
    return combat_rules::legacy_damage(parameters, *caster_stats);
  }
  if (caster == player_id_) {
    return combat_rules::player_damage(parameters, *caster_stats, target_stats);
  }
  return combat_rules::npc_damage(parameters, *caster_stats, target_stats);
}

Json CombatSession::hit_header(int effect_id, const Action &action, std::uint64_t caster,
                               std::uint64_t target, std::int64_t ticks,
                               std::optional<std::uint64_t> projectile_instance,
                               std::optional<std::uint64_t> area_unit) const {
  Json header{{"caster_unit_id", std::to_string(caster)},
              {"base_caster_unit_id", std::to_string(caster)},
              {"target_unit_id", std::to_string(target)},
              {"skill_effect_cms_id", effect_id},
              {"skill_cms_id", action.skill_id},
              {"skill_instance_id", action.instance},
              {"skill_effect_unique_id", action.effect_unique_id},
              {"tick", wire_monotonic_tick(ticks)}};
  if (projectile_instance) {
    header["instance_id"] = std::to_string(*projectile_instance);
  }
  if (area_unit) {
    header["caster_unit_id"] = std::to_string(*area_unit);
  }
  return header;
}

void CombatSession::grant_hit_immunity(int immunity_id, HitImmunity &immunity) const {
  const auto row = data_->immunities.find(immunity_id);
  if (row == data_->immunities.end()) {
    return;
  }

  immunity.dodge = immunity.dodge || row->second.dodge;
  immunity.damage = immunity.damage || row->second.damage;
  immunity.names.insert(row->second.names.begin(), row->second.names.end());
}

CombatSession::HitImmunity CombatSession::action_immunity(const Action &action,
                                                          std::int64_t ticks) const {
  HitImmunity immunity;
  if (ticks < action.started) {
    return immunity;
  }

  const auto elapsed_ms = (ticks - action.started) / kTicksPerMillisecond;
  for (const auto &notify : stage(action).at("notifies")) {
    if (notify.value("notifyType", std::string{}) != "immune" ||
        elapsed_ms < notify.value("startTime", 0) || elapsed_ms > notify.value("endTime", 0)) {
      continue;
    }
    grant_hit_immunity(notify.value("immune", Json::object()).value("immuneCmsId", 0), immunity);
  }
  return immunity;
}

void CombatSession::retain_player_projectile_immunity(const Action &action, std::int64_t end_tick) {
  const auto stage_end =
      action.started + stage(action).at("duration_ms").get<std::int64_t>() * kTicksPerMillisecond;
  const auto cutoff = std::min(end_tick, stage_end);
  for (auto &projectile : projectiles_) {
    if (projectile.target != player_id_ || projectile.deadline < action.started ||
        projectile.deadline >= cutoff) {
      continue;
    }

    // Retirement wins at the exact cutoff; only earlier impacts retain the old stage.
    // A later retry must not consult a replacement action for an earlier impact. Storage is
    // bounded by the existing flight limit and disappears when that flight is removed.
    const auto immunity = action_immunity(action, projectile.deadline);
    auto &retained = projectile.retired_player_immunity;
    retained.dodge = retained.dodge || immunity.dodge;
    retained.damage = retained.damage || immunity.damage;
    retained.names.insert(immunity.names.begin(), immunity.names.end());
  }
}

CombatSession::HitImmunity CombatSession::hit_immunity(std::uint64_t unit,
                                                       std::int64_t ticks) const {
  HitImmunity immunity;

  const std::optional<Action> *action = &action_;
  const std::optional<ActiveShield> *shield = &player_shield_;
  if (unit != player_id_) {
    const auto monster = monsters().find(unit);
    if (monster == monsters().end()) {
      return immunity;
    }
    action = &monster->second.action;
    shield = &monster->second.shield;
  }

  // Stage immune windows are relative to the stage start, like every other stage notify.
  if (*action) {
    immunity = action_immunity(**action, ticks);
  }

  if (*shield && (*shield)->expire_tick > ticks) {
    const auto definition = data_->shields.find((*shield)->shield_id);
    if (definition != data_->shields.end() && definition->second.immune_id > 0) {
      grant_hit_immunity(definition->second.immune_id, immunity);
    }
  }
  return immunity;
}

void CombatSession::add_shield(const Json &effect, std::uint64_t holder, std::uint64_t caster,
                               std::int64_t ticks, std::vector<GameplayPacket> &packets) {
  const double raw_id = effect.at("parameters").value("shieldId", 0.0);
  if (!std::isfinite(raw_id) || raw_id < 1 || raw_id > std::numeric_limits<int>::max()) {
    return;
  }
  const int shield_id = static_cast<int>(raw_id);
  const auto definition = data_->shields.find(shield_id);
  if (definition == data_->shields.end() || definition->second.duration_ms <= 0) {
    return;
  }

  auto &holder_stats = holder == player_id_ ? stats_ : monsters().at(holder).stats;
  const auto per_mille_of = [](int value, int per_mille) {
    return static_cast<std::int64_t>(value) * per_mille / 1000;
  };
  const std::int64_t amount =
      definition->second.amount +
      per_mille_of(stat_value(holder_stats, kMaxHealth), definition->second.hp_per_total_milli) +
      per_mille_of(stat_value(holder_stats, kHealth), definition->second.hp_per_remain_milli);
  const int shield_amount = static_cast<int>(std::clamp<std::int64_t>(amount, 0, kMaxShieldAmount));

  auto &shield = holder == player_id_ ? player_shield_ : monsters().at(holder).shield;
  shield = ActiveShield{shield_id, ticks + definition->second.duration_ms * kTicksPerMillisecond};
  holder_stats[std::to_string(kShieldStat)] = shield_amount;

  // Observed (149 official grants): ChangeStatNoti{SCS_StatusEffect, stat 71 = amount}, then
  // AddStatusEffectNoti of the effect's status. Monster stats are not reported to the client.
  if (holder == player_id_) {
    packets.push_back({kChangeStatNotification,
                       {{"unit_id", std::to_string(holder)},
                        {"tick", wire_monotonic_tick(ticks)},
                        {"default_source", "SCS_StatusEffect"},
                        {"stat_changes", {{{"stat_type", kShieldStat}, {"result", shield_amount}}}}}});
  }
  if (effect.contains("status_effect_id")) {
    apply_status(holder, caster, effect.at("status_effect_id").get<int>(), ticks, packets);
  }
}

void CombatSession::clear_shield(std::uint64_t holder, std::int64_t ticks,
                                 std::vector<GameplayPacket> &packets) {
  if (holder != player_id_ && !monsters().contains(holder)) {
    return;
  }

  auto &shield = holder == player_id_ ? player_shield_ : monsters().at(holder).shield;
  auto &holder_stats = holder == player_id_ ? stats_ : monsters().at(holder).stats;
  const bool had_amount = stat_value(holder_stats, kShieldStat) > 0;
  if (!shield && !had_amount) {
    return;
  }
  shield.reset();
  holder_stats[std::to_string(kShieldStat)] = 0;

  // Observed shape of the official stat-71 clear: the stat with no result (zero).
  if (holder == player_id_ && had_amount) {
    packets.push_back({kChangeStatNotification,
                       {{"unit_id", std::to_string(holder)},
                        {"tick", wire_monotonic_tick(ticks)},
                        {"default_source", "SCS_StatusEffect"},
                        {"stat_changes", {{{"stat_type", kShieldStat}}}}}});
  }
}

void CombatSession::expire_shields(std::int64_t ticks, std::vector<GameplayPacket> &packets) {
  if (player_shield_ && player_shield_->expire_tick <= ticks) {
    clear_shield(player_id_, player_shield_->expire_tick, packets);
  }
  for (auto &[id, monster] : monsters()) {
    if (monster.private_owner != 0 && monster.private_owner != player_id_) {
      continue;
    }
    if (monster.shield && monster.shield->expire_tick <= ticks) {
      clear_shield(id, monster.shield->expire_tick, packets);
    }
  }
}

int CombatSession::absorb_with_shield(std::uint64_t target, int damage, Json &stat_changes) {
  auto &target_stats = target == player_id_ ? stats_ : monsters().at(target).stats;
  const int shield_before = stat_value(target_stats, kShieldStat);
  const int absorbed = std::clamp(damage, 0, std::max(0, shield_before));
  if (absorbed == 0) {
    return 0;
  }

  const int shield_after = shield_before - absorbed;
  target_stats[std::to_string(kShieldStat)] = shield_after;
  if (shield_after == 0) {
    // The status that announced the shield runs out on its own timer.
    auto &shield = target == player_id_ ? player_shield_ : monsters().at(target).shield;
    shield.reset();
  }
  // Observed: DamageNoti lists the shield change after the health change.
  stat_changes.push_back(
      {{"stat_type", kShieldStat}, {"delta", -absorbed}, {"result", shield_after}});
  return absorbed;
}

void CombatSession::apply_skill_slot_effect(int effect_id, const Json &effect,
                                            std::int64_t ticks,
                                            std::vector<GameplayPacket> &packets) {
  if (effect.at("kind") == "changeWeaponSkillSlot") {
    // Observed (240 official notifications): expire_tick is the effect tick plus the catalog
    // SkillEffect.duration. The client swaps its skill slots; the server keeps no slot state.
    // Every catalog swap has a duration; a profile exported without it announces nothing rather
    // than a swap that ends at once.
    const int duration_ms = std::clamp(effect.value("duration_ms", 0), 0, kMaxStatusDurationMs);
    if (duration_ms == 0) {
      return;
    }
    packets.push_back(
        {kChangeWeaponSkillSlotNotification,
         {{"skill_effect_cms_id", effect_id},
          {"expire_tick", wire_monotonic_tick(ticks + duration_ms * kTicksPerMillisecond)}}});
    return;
  }

  // skillCooldownReset (observed once: the parry counter 1127 announced effect 110328, whose
  // FK_CooldownResetSkillIds readies pursuit 1000526). Each named skill's cooldown key is freed.
  for (const auto &reset_skill : effect.value("cooldown_reset_skill_ids", Json::array())) {
    if (!reset_skill.is_number_integer()) {
      continue;
    }
    const auto skill = data_->skills.find(reset_skill.get<int>());
    if (skill == data_->skills.end()) {
      continue;
    }
    cooldowns_.erase(skill_cooldown_key(skill->first, skill->second));
  }
  packets.push_back({kResetCooldownNotification, {{"skill_effect_cms_id", effect_id}}});
}

bool CombatSession::is_live_monster(std::uint64_t unit) const {
  const auto found = monsters().find(unit);
  return found != monsters().end() && found->second.active &&
         found->second.attackable &&
         (found->second.private_owner == 0 || found->second.private_owner == player_id_) &&
         stat_value(found->second.stats, kHealth) > 0;
}

void CombatSession::spawn_server_projectile(const Json &parameters, const Action &action,
                                            std::uint64_t caster, std::uint64_t target,
                                            const WorldPosition &origin,
                                            const WorldPosition &player_position,
                                            std::int64_t ticks,
                                            std::vector<GameplayPacket> &packets,
                                            std::optional<WorldPosition> aimed_destination) {
  const int projectile_id = static_cast<int>(parameters.at("projectileId").get<double>());
  const auto found = data_->projectiles.find(std::to_string(projectile_id));
  if (found == data_->projectiles.end() || projectiles_.size() >= kMaximumProjectiles) {
    return;
  }
  const auto &definition = *found;

  // The player's client launches a client-driven flight itself and announces it with
  // PC2S_ExecuteSpawnProjectileSkillEffectReq. A second, server-numbered copy announced here
  // made the client build a separate flight instead of confirming its own.
  if (caster == player_id_ && is_client_driven_projectile(definition)) {
    return;
  }

  const auto launch = projectile_launch_position(parameters, origin, action.yaw);
  const int lifetime_ms = definition.at("duration_ms").get<int>();
  const double speed_mm_per_second =
      definition.at("parameters").value("speed", 0.0) * kMillimetresPerCatalogCentimetre;
  const double reach_mm = speed_mm_per_second * lifetime_ms / 1000;
  WorldPosition destination = launch;
  std::optional<int> travel_ms;
  if (aimed_destination) {
    // CANDIDATE: an aimed flight homes on no unit. It lands on the marker, or where its lifetime
    // ends along the same line when the marker lies beyond speed x lifetime.
    target = 0;
    const double distance_mm = std::sqrt(distance_squared(launch, *aimed_destination));
    const double fraction = distance_mm > reach_mm && distance_mm > 0 ? reach_mm / distance_mm
                                                                      : 1.0;
    destination.x_mm += static_cast<int>(
        std::llround((aimed_destination->x_mm - launch.x_mm) * fraction));
    destination.y_mm += static_cast<int>(
        std::llround((aimed_destination->y_mm - launch.y_mm) * fraction));
    destination.z_mm += static_cast<int>(
        std::llround((aimed_destination->z_mm - launch.z_mm) * fraction));
    travel_ms = speed_mm_per_second > 0
                    ? std::max(1, static_cast<int>(std::ceil(
                                      std::min(distance_mm, reach_mm) / speed_mm_per_second *
                                      1000)))
                    : lifetime_ms;
  } else if (target != 0) {
    destination = target == player_id_ ? player_position : monsters().at(target).position;
    travel_ms = projectile_travel_ms(definition, launch, destination);
  } else {
    const double yaw = action.yaw * std::numbers::pi / 180;
    destination.x_mm += std::llround(std::cos(yaw) * reach_mm);
    destination.y_mm += std::llround(std::sin(yaw) * reach_mm);
    travel_ms = lifetime_ms;
  }
  if (!travel_ms) {
    return;
  }

  if (next_projectile_work_life_id_ == (std::numeric_limits<std::uint64_t>::max)()) {
    throw std::overflow_error("Projectile work identity exhausted");
  }
  const auto instance = next_projectile_++;
  auto projectile_action = action;
  const auto monster = monsters().find(caster);
  if (monster != monsters().end() && !retired_projectile_caster(action, caster)) {
    projectile_action.npc_projectile_caster = std::make_shared<NpcProjectileCaster>(
        NpcProjectileCaster{monster->second.cms_id, monster->second.stats,
                            monster->second.life_identity});
  }
  projectiles_.push_back({instance, projectile_id, caster, target, std::move(projectile_action),
                          ticks + *travel_ms * kTicksPerMillisecond, false, launch, destination,
                          is_linear_projectile(definition), ticks});
  projectiles_.back().work_life_id = next_projectile_work_life_id_++;
  if (monster != monsters().end() && !retired_projectile_caster(action, caster)) {
    projectiles_.back().npc_interruption = monster->second.projectile_interruption;
  }
  packets.push_back({kSpawnProjectileNotification,
                     {{"caster_unit_id", std::to_string(caster)},
                      {"spawn_projectile",
                       {{"projectile_cms_id", projectile_id},
                        {"skill_instance_id", action.instance},
                        {"effect_unique_id", action.effect_unique_id},
                        {"instance_id", std::to_string(instance)},
                        {"target_unit_id", std::to_string(target)},
                        {"start_pos", wire_position(launch)},
                        {"dest_pos", wire_position(destination)},
                        {"spawn_tick", wire_monotonic_tick(ticks)},
                        {"duration", std::to_string(definition.at("duration_ms").get<int>())},
                        {"start_rotation", {{"yaw", action.yaw}}}}}}});
}

std::uint64_t CombatSession::projectile_target(const Json &definition, const WorldPosition &launch,
                                               const WorldPosition &destination,
                                               std::uint64_t requested_target) const {
  const auto &skill = data_->skills.at(action_->skill_id);
  const double minimum = skill.value("min_range_mm", 0);
  const double maximum = skill.at("range_mm").get<double>();
  const auto reachable = [&](std::uint64_t target) {
    if (!is_live_monster(target)) {
      return false;
    }
    const auto &position = monsters().at(target).position;
    const double distance = distance_squared(launch, position);
    return distance >= minimum * minimum && (maximum <= 0 || distance <= maximum * maximum) &&
           projectile_travel_ms(definition, launch, position).has_value();
  };
  // A homing lock may follow its selected live enemy. Straight flights must actually cross
  // a capsule; an arbitrary target ID in the report cannot bend an arrow off its visible path.
  if (!is_linear_projectile(definition) && reachable(requested_target)) {
    return requested_target;
  }
  std::uint64_t closest{};
  double nearest_distance = std::numeric_limits<double>::max();
  for (const auto &[id, monster] : monsters()) {
    if (!reachable(id)) {
      continue;
    }
    const auto collision = data_->npc_collision.find(monster.cms_id);
    const auto capsule =
        collision == data_->npc_collision.end() ? UnitCollisionRadii{} : collision->second;
    const double distance = distance_squared(launch, monster.position);
    if (distance < nearest_distance &&
        flight_crosses_unit(definition, launch, destination, monster.position, capsule)) {
      nearest_distance = distance;
      closest = id;
    }
  }
  return closest;
}

void CombatSession::spawn_client_projectiles(const Json &request, const WorldPosition &position,
                                             std::int64_t ticks,
                                             std::vector<GameplayPacket> &packets) {
  const auto &report = request.at("skill_effect");
  const auto &effect = data_->effects.at(std::to_string(report.at("effect_id").get<int>()));
  require(effect.at("kind") == "spawnProjectile", "BadRequest");
  const auto &parameters = effect.at("parameters");
  const int projectile_id = static_cast<int>(parameters.at("projectileId").get<double>());
  const auto found = data_->projectiles.find(std::to_string(projectile_id));
  require(found != data_->projectiles.end(), "BadRequest");
  const auto &definition = *found;

  // A server-driven flight is launched by its stage event. Its report names no client instance
  // (observed for effect 140805) and the official server sent the caster nothing back.
  if (!is_client_driven_projectile(definition)) {
    return;
  }

  const int unique_id = report.value("unique_effect_id", 0);
  const auto event_key = std::make_pair(action_->stage, unique_id);
  require(!action_->client_projectile_events.contains(event_key), "BadRequest");
  const auto event = std::ranges::find_if(stage(*action_).at("events"), [&](const Json &entry) {
    return entry.at("unique_id") == unique_id && entry.at("effect_id") == report.at("effect_id");
  });
  require(event != stage(*action_).at("events").end(), "BadRequest");
  const auto launch_tick = std::max(
      ticks, action_->started + event->at("time_ms").get<std::int64_t>() * kTicksPerMillisecond);

  // The whole report is validated before anything is announced, so a refusal (one
  // SpawnClientProjectileFailNoti per flight) never follows a confirmed spawn.
  const auto &flights = report.value("projectile_datas", Json::array());
  const int spawn_limit = std::clamp(static_cast<int>(parameters.value("maxSpawnCount", 1.0)), 1,
                                     kMaximumClientProjectilesPerReport);
  require(flights.is_array() && !flights.empty() &&
              flights.size() <= static_cast<std::size_t>(spawn_limit),
          "BadRequest");
  require(report.contains("fire_pos"), "BadRequest");
  const auto fire_position = wire_point(report.at("fire_pos"), position.map_id);
  require(distance_squared(fire_position, position) <= kPositionToleranceMm * kPositionToleranceMm,
          "TooFar");
  require(projectiles_.size() + flights.size() <= kMaximumProjectiles, "BadRequest");

  // Observed: the echo carries the request tick verbatim as spawn_tick.
  std::string spawn_tick = wire_monotonic_tick(ticks);
  if (request.contains("tick")) {
    const auto client_tick = wire_int64(request.at("tick"));
    require(client_tick.has_value(), "BadRequest");
    spawn_tick = std::to_string(*client_tick);
  }

  struct ClientFlight {
    std::uint64_t instance{};
    WorldPosition destination;
    Json direction;
    std::uint64_t target{};
  };
  std::vector<ClientFlight> accepted;
  const auto instance_in_use = [&](std::uint64_t instance) {
    const bool in_flight = std::ranges::any_of(projectiles_, [&](const Projectile &projectile) {
      return projectile.client_spawned && projectile.instance == instance;
    });
    const bool awaiting_despawn =
        std::ranges::find(client_projectiles_awaiting_despawn_, instance) !=
        client_projectiles_awaiting_despawn_.end();
    const bool repeated = std::ranges::any_of(
        accepted, [&](const ClientFlight &flight) { return flight.instance == instance; });
    return in_flight || awaiting_despawn || repeated;
  };

  for (const auto &flight_report : flights) {
    // The client keeps only the low 32 bits of a flight instance as its table key.
    const auto instance = flight_report.contains("instance_id")
                              ? wire_int64(flight_report.at("instance_id"))
                              : std::nullopt;
    require(instance && *instance > 0 && *instance <= std::numeric_limits<std::int32_t>::max(),
            "BadRequest");
    require(!instance_in_use(static_cast<std::uint64_t>(*instance)), "BadRequest");
    require(flight_report.contains("dest_pos"), "BadRequest");

    const auto &direction = flight_report.value("direction", Json::object());
    const double pitch = direction.value("pitch", 0.0);
    const double yaw = direction.value("yaw", 0.0);
    require(std::isfinite(pitch) && std::isfinite(yaw), "BadRequest");

    // A flight aimed at anything but a live monster of this instance flies for show only.
    std::uint64_t target{};
    const auto &target_reference = flight_report.value("target", Json::object());
    if (target_reference.contains("unit_id")) {
      const auto unit = wire_int64(target_reference.at("unit_id"));
      if (unit && *unit > 0 && is_live_monster(static_cast<std::uint64_t>(*unit))) {
        target = static_cast<std::uint64_t>(*unit);
      }
    }

    const auto destination = wire_point(flight_report.at("dest_pos"), position.map_id);
    target = projectile_target(definition, fire_position, destination, target);
    accepted.push_back({static_cast<std::uint64_t>(*instance),
                        destination,
                        {{"pitch", pitch}, {"yaw", yaw}, {"roll", 0.0}},
                        target});
  }

  const auto unique_effect_id = report.value("unique_effect_id", 0u);
  const auto lifetime_ms = definition.at("duration_ms").get<int>();
  const auto remaining_identities =
      (std::numeric_limits<std::uint64_t>::max)() - next_projectile_work_life_id_;
  if (accepted.size() > remaining_identities) {
    throw std::overflow_error("Projectile work identity exhausted");
  }
  action_->client_projectile_events.insert(event_key);
  for (const auto &flight : accepted) {
    // Observed: the confirmation echoes the client's instance, fire position, destination and
    // direction, with the catalog lifetime. The client confirms its own flight only when the
    // skill instance and effect unique ID match it.
    Json spawn{{"projectile_cms_id", projectile_id},
               {"skill_instance_id", action_->instance},
               {"instance_id", std::to_string(flight.instance)},
               {"spawn_tick", spawn_tick},
               {"duration", std::to_string(lifetime_ms)},
               {"start_pos", wire_position(fire_position)},
               {"dest_pos", wire_position(flight.destination)},
               {"start_rotation", flight.direction}};
    if (unique_effect_id != 0) {
      spawn["effect_unique_id"] = unique_effect_id;
    }
    if (flight.target != 0) {
      spawn["target_unit_id"] = std::to_string(flight.target);
    }
    packets.push_back({kSpawnProjectileNotification,
                       {{"caster_unit_id", std::to_string(player_id_)},
                        {"spawn_projectile", std::move(spawn)}}});

    client_projectiles_awaiting_despawn_.push_back(flight.instance);
    if (client_projectiles_awaiting_despawn_.size() > kMaximumProjectiles) {
      client_projectiles_awaiting_despawn_.pop_front();
    }

    // The server times the hit from the fire position to the target's current position; the
    // client's later hit report cannot select a target or shorten this flight.
    const auto destination =
        flight.target == 0 ? flight.destination : monsters().at(flight.target).position;
    const auto travel_ms = flight.target == 0
                               ? std::optional<int>(lifetime_ms)
                               : projectile_travel_ms(definition, fire_position, destination);
    if (!travel_ms) {
      continue;
    }
    auto projectile_action = *action_;
    projectile_action.effect_unique_id = unique_id;
    projectiles_.push_back({flight.instance, projectile_id, player_id_, flight.target,
                            projectile_action, launch_tick + *travel_ms * kTicksPerMillisecond,
                            true, fire_position, destination, is_linear_projectile(definition),
                            launch_tick});
    projectiles_.back().work_life_id = next_projectile_work_life_id_++;
  }
}

std::vector<GameplayPacket> CombatSession::receive_projectile_report(std::uint16_t opcode,
                                                                     const Json &request,
                                                                     const WorldPosition &position,
                                                                     std::int64_t ticks) {
  const auto world_lock = lock_world_mobs();
  if (!owns_encounter_map(position.map_id) || ticks <= 0) {
    return {};
  }
  if (ticks - projectile_report_window_ >= kTicksPerSecond) {
    projectile_report_window_ = ticks;
    projectile_reports_in_window_ = 0;
  }
  // Several homing projectiles report at once. This budget is separate from skill starts.
  if (projectile_reports_in_window_ >= 256) {
    return {};
  }
  ++projectile_reports_in_window_;
  try {
    const bool update = opcode == kUpdateProjectileRequest;
    if (!update && opcode != kClientProjectileHitRequest) {
      return {};
    }
    const auto &report = update ? request.at("update_projectile") : request;
    const auto instance = wire_int64(report.at("instance_id"));
    if (!instance || *instance <= 0) {
      return {};
    }
    const auto found = std::ranges::find_if(projectiles_, [&](const Projectile &flight) {
      return flight.client_spawned && flight.caster == player_id_ &&
             flight.instance == static_cast<std::uint64_t>(*instance);
    });
    if (found == projectiles_.end()) {
      // A report can arrive after the authoritative hit removed its flight. No second hit.
      return {};
    }
    auto &flight = *found;
    constexpr auto tolerance = 500 * kTicksPerMillisecond;
    if (ticks < flight.launched || ticks > flight.deadline + tolerance) {
      return {};
    }

    if (!update) {
      const auto &hit = request.at("hit");
      const auto target = wire_int64(hit.at("hit_unit_id"));
      const auto elapsed = request.value("elapsed_time_milli", -1);
      if (!target || *target <= 0 || static_cast<std::uint64_t>(*target) != flight.target ||
          elapsed < 0 || elapsed > 300000 || flight.hit_reported ||
          ticks + tolerance < flight.deadline ||
          std::abs(elapsed * kTicksPerMillisecond - (ticks - flight.launched)) > tolerance ||
          distance_squared(wire_point(hit.at("hit_pos"), position.map_id), flight.destination) >
              kPositionToleranceMm * kPositionToleranceMm) {
        return {};
      }
      flight.hit_reported = true;
      // Damage and ProjectileHitNoti still come from the server deadline. A valid report is
      // only confirmation; it cannot select another victim or execute early.
      return {};
    }

    const auto current_tick = wire_int64(report.at("cur_tick"));
    const auto previous_tick = wire_int64(report.at("prev_tick"));
    if (!current_tick || !previous_tick || *previous_tick < 0 || *current_tick <= *previous_tick ||
        *previous_tick / 100 < flight.launched - tolerance ||
        *current_tick / 100 > ticks + tolerance ||
        *current_tick / 100 > flight.deadline + tolerance ||
        *current_tick / 100 <= flight.last_report_tick) {
      return {};
    }
    const auto current = wire_point(report.at("cur_pos"), position.map_id);
    const auto previous = wire_point(report.at("prev_pos"), position.map_id);
    const double path_length = std::sqrt(distance_squared(flight.launch, flight.destination));
    // Bounds visual corrections without claiming an authoritative client collision solver.
    // Neither point changes the player's position or awards a hit.
    const double envelope = path_length + kPositionToleranceMm;
    if (distance_squared(current, flight.launch) > envelope * envelope ||
        distance_squared(previous, flight.launch) > envelope * envelope) {
      return {};
    }
    for (const auto *key : {"cur_rotation", "prev_rotation"}) {
      const auto &rotation = report.at(key);
      for (const auto *component : {"pitch", "yaw", "roll"}) {
        const auto value = rotation.value(component, 0.0);
        if (!std::isfinite(value) || std::abs(value) > 3600) {
          return {};
        }
      }
    }
    const bool lost = flight.target != 0 && !is_live_monster(flight.target);
    const auto destination = !flight.linear && flight.target != 0 && !lost
                                 ? monsters().at(flight.target).position
                                 : flight.destination;
    Json accepted{{"instance_id", std::to_string(flight.instance)},
                  {"lost_target", lost},
                  {"cur_tick", std::to_string(*current_tick)},
                  {"prev_tick", std::to_string(*previous_tick)},
                  {"cur_pos", wire_position(current)},
                  {"prev_pos", wire_position(previous)},
                  {"dest_pos", wire_position(destination)},
                  {"cur_rotation", report.at("cur_rotation")},
                  {"prev_rotation", report.at("prev_rotation")}};
    flight.last_report_tick = *current_tick / 100;
    return {
        {46,
         {{"unit_id", std::to_string(player_id_)}, {"update_projectile", std::move(accepted)}}}};
  } catch (const Json::exception &) {
    // Incomplete optional report fields are refused without changing a flight or sending an
    // unsolicited response. Protobuf framing and size validation have already run.
    return {};
  }
}

void CombatSession::despawn_client_projectile(const Json &request,
                                              std::vector<GameplayPacket> &packets) {
  // x5.cms.EX5SkillObjectDespawnReasonType names; proto3 JSON omits the zero value.
  static constexpr std::array<std::string_view, 7> kDespawnReasons{
      "EX5SkillObjectDespawnReasonType_None",      "EX5SkillObjectDespawnReasonType_LifeTime",
      "EX5SkillObjectDespawnReasonType_HitTarget", "EX5SkillObjectDespawnReasonType_HitStatic",
      "EX5SkillObjectDespawnReasonType_UseCount",
      "EX5SkillObjectDespawnReasonType_SkillStageCancel",
      "EX5SkillObjectDespawnReasonType_Parried"};

  const auto &despawn = request.at("despawn_projectile");
  const auto instance =
      despawn.contains("instance_id") ? wire_int64(despawn.at("instance_id")) : std::nullopt;
  require(instance && *instance > 0, "BadRequest");
  const auto reason = despawn.value("reason_type", std::string{kDespawnReasons.front()});
  require(std::ranges::find(kDespawnReasons, reason) != kDespawnReasons.end(), "BadRequest");

  Json echoed{{"instance_id", std::to_string(*instance)}, {"reason_type", reason}};
  if (despawn.contains("despawn_tick")) {
    const auto despawn_tick = wire_int64(despawn.at("despawn_tick"));
    require(despawn_tick.has_value(), "BadRequest");
    echoed["despawn_tick"] = std::to_string(*despawn_tick);
  }
  if (despawn.contains("pos")) {
    echoed["pos"] = wire_position(wire_point(despawn.at("pos"), 0));
  }
  if (despawn.contains("hit_unit_id")) {
    const auto hit_unit = wire_int64(despawn.at("hit_unit_id"));
    require(hit_unit && *hit_unit > 0, "BadRequest");
    echoed["hit_unit_id"] = std::to_string(*hit_unit);
  }

  // Only a flight this client launched, and only once: a repeated or unknown report is ignored.
  const auto id = static_cast<std::uint64_t>(*instance);
  const auto awaiting = std::ranges::find(client_projectiles_awaiting_despawn_, id);
  if (awaiting == client_projectiles_awaiting_despawn_.end()) {
    return;
  }
  client_projectiles_awaiting_despawn_.erase(awaiting);

  // A reported miss withdraws the flight's pending hit. A client can only give up its own damage
  // this way; a reported target hit still lands at the server's deadline, never earlier.
  if (reason != "EX5SkillObjectDespawnReasonType_HitTarget") {
    std::erase_if(projectiles_, [&](const Projectile &projectile) {
      return projectile.client_spawned && projectile.instance == id;
    });
  }

  // Observed: the official server echoes the client's despawn to its caster about 25 ms later.
  packets.push_back({kDespawnProjectileNotification,
                     {{"caster_unit_id", std::to_string(player_id_)},
                      {"despawn_projectile", std::move(echoed)}}});
}

int CombatSession::conditional_effect_for(const Json &conditional, std::uint64_t target) const {
  const bool on_player = target == player_id_;
  if (!on_player && !monsters().contains(target)) {
    return conditional.value("default_effect_id", 0);
  }
  const auto &target_stats = on_player ? stats_ : monsters().at(target).stats;
  const auto &target_statuses = on_player ? statuses_ : monsters().at(target).statuses;

  const auto condition_holds = [&](const Json &condition) {
    if (condition.value("unitType", std::string{}) != "EX5SkillEffectConditionUnitType_Target") {
      return false;
    }
    const auto kind = condition.value("kind", std::string{});
    const auto comparison = condition.value("comparisonType", std::string{});
    if (kind == "maxHPRatio") {
      const int maximum = stat_value(target_stats, kMaxHealth);
      if (maximum <= 0) {
        return false;
      }
      const double percent = 100.0 * stat_value(target_stats, kHealth) / maximum;
      return catalog_comparison_holds(percent, condition.value("hpRatio", 0.0), comparison);
    }
    if (kind == "statusEffectStack") {
      // Stack counts are not simulated: an active status counts as one stack.
      const int status_id = condition.value("statusEffectId", 0);
      const bool active = std::ranges::any_of(target_statuses, [&](const ActiveStatus &status) {
        return status.status_effect_id == status_id;
      });
      return catalog_comparison_holds(active ? 1.0 : 0.0, condition.value("stackCount", 0.0),
                                      comparison);
    }
    return false;
  };

  for (const auto &branch : conditional.value("branches", Json::array())) {
    const auto &conditions = branch.value("conditions", Json::array());
    if (!conditions.empty() && std::ranges::all_of(conditions, condition_holds)) {
      return branch.value("effect_id", 0);
    }
  }
  return conditional.value("default_effect_id", 0);
}

std::vector<std::uint64_t> CombatSession::penetrated_units(const Json &definition,
                                                           const Projectile &projectile) const {
  std::vector<std::pair<double, std::uint64_t>> crossed;
  for (const auto &[id, monster] : monsters()) {
    if (!is_live_monster(id)) {
      continue;
    }
    UnitCollisionRadii capsule;
    const auto collision = data_->npc_collision.find(monster.cms_id);
    if (collision != data_->npc_collision.end()) {
      capsule = collision->second;
    }
    if (id == projectile.target ||
        flight_crosses_unit(definition, projectile.launch, projectile.destination,
                            monster.position, capsule)) {
      crossed.emplace_back(distance_squared(projectile.launch, monster.position), id);
    }
  }
  std::ranges::sort(crossed);

  const int limit = std::clamp(definition.value("max_target_count", 1), 1, 32);
  std::vector<std::uint64_t> units;
  for (const auto &[distance, id] : crossed) {
    if (units.size() >= static_cast<std::size_t>(limit)) {
      break;
    }
    units.push_back(id);
  }
  return units;
}

void CombatSession::resolve_projectile_hit(const Projectile &projectile,
                                           const WorldPosition &player_position,
                                           std::int64_t ticks,
                                           std::vector<GameplayPacket> &packets) {
  bool target_alive = projectile.target == player_id_ ? stat_value(stats_, kHealth) > 0
                                                      : is_live_monster(projectile.target);
  const auto &definition = data_->projectiles.at(std::to_string(projectile.cms_id));
  if (target_alive && projectile.linear) {
    UnitCollisionRadii capsule;
    if (projectile.target == player_id_) {
      if (!data_->player_collision.empty()) {
        capsule = data_->player_collision.begin()->second;
      }
    } else {
      const auto collision = data_->npc_collision.find(monsters().at(projectile.target).cms_id);
      if (collision != data_->npc_collision.end()) {
        capsule = collision->second;
      }
    }
    const auto &position = projectile.target == player_id_
                               ? player_position
                               : monsters().at(projectile.target).position;
    // A linear flight cannot home onto a target that has moved away before contact.
    target_alive = flight_crosses_unit(definition, projectile.destination, projectile.destination,
                                       position, capsule);
  }
  if (target_alive) {
    // A piercing player flight (SkillObject isPenetration, e.g. the stitching burn) strikes every
    // living enemy its path crosses up to its maxTargetCount; any other flight its own target.
    std::vector<std::uint64_t> struck{projectile.target};
    if (projectile.caster == player_id_ && projectile.linear &&
        definition.at("parameters").value("isPenetration", false)) {
      struck = penetrated_units(definition, projectile);
    }
    for (const auto unit : struck) {
      const auto &origin = unit == player_id_ ? player_position : monsters().at(unit).position;
      std::vector<int> hit_effects;
      for (const auto &effect : definition.at("hit_effects")) {
        hit_effects.push_back(effect.get<int>());
      }
      if (definition.contains("conditional_hit")) {
        const int chosen = conditional_effect_for(definition.at("conditional_hit"), unit);
        if (chosen > 0 && data_->effects.contains(std::to_string(chosen))) {
          hit_effects.push_back(chosen);
        }
      }
      for (const int effect : hit_effects) {
        const auto &hit = data_->effects.at(std::to_string(effect));
        // Splash effects select their own enemies around the impact. Reference effects retain
        // the struck unit; self reference effects must still apply to the original caster.
        const bool self =
            hit.at("targeting").value("refType", std::string{}) == "EX5ReferenceTargetType_Self";
        const bool shape = hit.at("targeting_kind").get<std::string>().starts_with("shape");
        auto target = unit;
        if (self) {
          target = projectile.caster;
        } else if (shape) {
          target = 0;
        }
        apply_effect(effect, projectile.action, projectile.caster, target, origin, player_position,
                     ticks, packets, 0, nullptr, projectile.instance, std::nullopt,
                     &projectile.retired_player_immunity);
      }
    }
    // SkillObject.FK_hitTargetDespawnSkillEffect runs where the flight ended, on the struck
    // unit (for example the poison bottle's cloud, spawned at its main target).
    const auto &impact = projectile.target == player_id_
                             ? player_position
                             : monsters().at(projectile.target).position;
    for (const auto &effect : definition.value("despawn_effects", Json::array())) {
      apply_effect(effect.get<int>(), projectile.action, projectile.caster, projectile.target,
                   impact, player_position, ticks, packets);
    }

    // Observed order for a server-timed hit: DamageNoti, ProjectileHitNoti, DespawnProjectileNoti.
    // Captured player hits also list the caster in target_datas, apparently for self-targeted
    // hit effects; only the struck units are listed here (Candidate).
    Json target_datas = Json::array();
    for (const auto unit : struck) {
      target_datas.push_back(
          {{"target", {{"unit_id", std::to_string(unit)}, {"sub_collision_index", 0}}}});
    }
    packets.push_back({kProjectileHitNotification,
                       {{"caster_unit_id", std::to_string(projectile.caster)},
                        {"instance_id", std::to_string(projectile.instance)},
                        {"target_datas", std::move(target_datas)}}});
  }

  // SkillObject.FK_lifeTimeDespawnSkillEffect: a server-driven flight that ends without striking
  // a unit applies it where it ended (Dragon's Roar's fire area on its aimed point, a monster's
  // fire bomb that missed). Client-spawned flights end through the client's own despawn report.
  if (!target_alive && !projectile.client_spawned) {
    for (const auto &effect : definition.value("lifetime_despawn_effects", Json::array())) {
      apply_effect(effect.get<int>(), projectile.action, projectile.caster, 0,
                   projectile.destination, player_position, ticks, packets, 0, nullptr,
                   projectile.instance);
    }
  }

  // The client of a client-spawned flight reports its despawn, which is echoed instead.
  if (projectile.client_spawned) {
    return;
  }

  Json despawn{{"instance_id", std::to_string(projectile.instance)},
               {"despawn_tick", wire_monotonic_tick(ticks)}};
  if (target_alive) {
    despawn["reason_type"] = "EX5SkillObjectDespawnReasonType_HitTarget";
    despawn["hit_unit_id"] = std::to_string(projectile.target);
  } else {
    despawn["reason_type"] = "EX5SkillObjectDespawnReasonType_LifeTime";
  }
  packets.push_back({kDespawnProjectileNotification,
                     {{"caster_unit_id", std::to_string(projectile.caster)},
                      {"despawn_projectile", std::move(despawn)}}});
}

namespace {

// The statusEffectId of an add/removeStatusEffect effect, or nothing for a malformed id or a
// rate below 100: every captured add has rate 100 and no capture exercises a chance roll.
std::optional<int> certain_status_id(const Json &parameters) {
  const double raw_id = parameters.value("statusEffectId", 0.0);
  const double rate = parameters.value("rate", 100.0);
  if (!std::isfinite(raw_id) || raw_id < 1 || raw_id > std::numeric_limits<int>::max() ||
      raw_id != std::floor(raw_id) || rate != 100.0) {
    return std::nullopt;
  }
  return static_cast<int>(raw_id);
}

Json status_effect_json(int status_effect_id, std::uint32_t instance, std::uint64_t caster,
                        std::int64_t start_tick, std::int64_t expire_tick) {
  return {{"status_effect_id", status_effect_id},
          {"instance_id", instance},
          {"level", 1},
          {"stack_count", 1},
          {"caster_unit_id", std::to_string(caster)},
          {"start_tick", wire_monotonic_tick(start_tick)},
          {"expire_tick", wire_monotonic_tick(expire_tick)}};
}

} // namespace

void CombatSession::add_status_effect(const Json &parameters, std::uint64_t target,
                                      std::uint64_t caster, std::int64_t ticks,
                                      std::vector<GameplayPacket> &packets) {
  const auto status_id = certain_status_id(parameters);
  if (status_id) {
    apply_status(target, caster, *status_id, ticks, packets);
  }
}

std::vector<StatusStatModifier> CombatSession::resolve_status_modifiers(int status_id,
                                                                        const Json &stats) const {
  std::vector<StatusStatModifier> resolved;
  const auto catalog = data_->status_stat_modifiers.find(status_id);
  if (catalog == data_->status_stat_modifiers.end()) {
    return resolved;
  }

  // Candidate order: the flat statAdd first, then the multiplier on the result.
  for (const auto &modifier : catalog->second) {
    const int base = stat_value(stats, modifier.stat_type) + modifier.add;
    const int change = modifier.add + combat_rules::stat_multiplier_delta(base, modifier.mul);
    if (change != 0) {
      resolved.push_back({modifier.stat_type, change, 0});
    }
  }
  return resolved;
}

void CombatSession::apply_status(std::uint64_t target, std::uint64_t caster, int status_id,
                                 std::int64_t ticks, std::vector<GameplayPacket> &packets) {
  const bool on_player = target == player_id_;
  if (!on_player && !is_live_monster(target)) {
    return;
  }
  const auto definition = data_->status_effects.find(std::to_string(status_id));
  if (definition == data_->status_effects.end()) {
    return;
  }
  // An empty catalog duration is not a proven "permanent" status, so duration 0 applies nothing.
  const int duration_ms = definition->at("duration_ms").get<int>();
  if (duration_ms <= 0) {
    return;
  }

  bool applies_stun = false;
  if (data_->abnormal_gauges) {
    const auto &types = data_->abnormal_gauges->types;
    const auto stun = types.find(combat_rules::AbnormalType::stun);
    applies_stun = stun != types.end() && stun->second.status_effect_id == status_id;
  }

  auto &holder_statuses = on_player ? statuses_ : monsters().at(target).statuses;
  const auto active = std::ranges::find_if(holder_statuses, [&](const ActiveStatus &status) {
    return status.status_effect_id == status_id;
  });
  if (active != holder_statuses.end()) {
    // Observed: re-adding status 2001005 kept its instance and sent UpdateStatusEffectNoti with
    // the new start and expiry. Candidate: a Stacking status is refreshed the same way, because
    // stack counts are not simulated.
    active->start_tick = ticks;
    active->expire_tick = ticks + duration_ms * kTicksPerMillisecond;
    active->caster = caster;
    packets.push_back({kUpdateStatusEffectNotification,
                       {{"unit_id", std::to_string(target)},
                        {"tick", wire_monotonic_tick(ticks)},
                        {"status_effects",
                         {status_effect_json(active->status_effect_id, active->instance, caster,
                                             active->start_tick, active->expire_tick)}}}});
    if (!on_player && applies_stun) {
      interrupt_monster_action(target, monsters().at(target), ticks, active->expire_tick, packets);
    }
    return;
  }
  const auto limit = on_player ? kMaxActiveStatuses : kMaxMonsterStatuses;
  if (holder_statuses.size() >= limit) {
    return;
  }

  std::uint32_t status_instance{};
  if (on_player) {
    status_instance = next_status_instance_++;
  } else {
    auto &next_instance = monsters().at(target).next_status_instance;
    if (next_instance > (std::numeric_limits<std::uint32_t>::max)()) {
      throw std::overflow_error("Monster status instance IDs are exhausted");
    }
    status_instance = static_cast<std::uint32_t>(next_instance);
    ++next_instance;
  }

  ActiveStatus status{status_id, status_instance, ticks,
                      ticks + duration_ms * kTicksPerMillisecond, caster};
  if (on_player) {
    status.modifiers = resolve_status_modifiers(status_id, stats_);
    // Observed (status 100597, 3 of 3 adds): the stat change precedes the add notification.
    apply_stat_modifiers(status.modifiers, 1, ticks, packets);
  } else {
    auto &monster = monsters().at(target);
    status.modifiers = resolve_status_modifiers(status_id, monster.stats);
    apply_monster_modifiers(monster, status.modifiers, 1);
  }
  holder_statuses.push_back(status);
  packets.push_back(
      {kAddStatusEffectNotification,
       {{"unit_id", std::to_string(target)},
        {"status_effect", status_effect_json(status.status_effect_id, status.instance, caster,
                                             status.start_tick, status.expire_tick)}}});
  if (!on_player && applies_stun) {
    interrupt_monster_action(target, monsters().at(target), ticks, status.expire_tick, packets);
  }
  if (on_player) {
    // A status newly applied to the player counts toward AddStatusEffect objectives.
    quest_events_.push_back({QuestEventKind::StatusEffectApplied, status_id, 0, 1});
  }
}

void CombatSession::remove_status_effect(const Json &parameters, std::uint64_t target,
                                         std::int64_t ticks, std::vector<GameplayPacket> &packets) {
  // Group removal (rows without statusEffectId) and partial stack removal are not simulated.
  const auto status_id = certain_status_id(parameters);
  if (status_id) {
    remove_statuses_by_id(target, *status_id, ticks, packets);
  }
}

void CombatSession::remove_statuses_by_id(std::uint64_t target, int status_effect_id,
                                          std::int64_t ticks,
                                          std::vector<GameplayPacket> &packets) {
  if (target != player_id_ && !monsters().contains(target)) {
    return;
  }

  auto &holder_statuses = target == player_id_ ? statuses_ : monsters().at(target).statuses;
  const bool was_stunned = target != player_id_ && monster_stunned(monsters().at(target), ticks);
  std::vector<ActiveStatus> removed;
  std::erase_if(holder_statuses, [&](const ActiveStatus &status) {
    if (status.status_effect_id != status_effect_id) {
      return false;
    }
    removed.push_back(status);
    return true;
  });
  for (const auto &status : removed) {
    remove_status(target, status, ticks, packets);
  }
  if (was_stunned && !monster_stunned(monsters().at(target), ticks)) {
    auto &monster = monsters().at(target);
    if (monster.ai) {
      monster.ai->resume_after_pause(ticks);
    }
  }
}

void CombatSession::apply_stat_modifiers(const std::vector<StatusStatModifier> &modifiers,
                                         int sign, std::int64_t ticks,
                                         std::vector<GameplayPacket> &packets) {
  if (modifiers.empty()) {
    return;
  }

  Json changes = Json::array();
  bool health_maximum_changed = false;
  bool stamina_maximum_changed = false;
  for (const auto &modifier : modifiers) {
    const int result = stat_value(stats_, modifier.stat_type) + sign * modifier.add;
    stats_[std::to_string(modifier.stat_type)] = result;
    changes.push_back({{"stat_type", modifier.stat_type}, {"result", result}});
    health_maximum_changed = health_maximum_changed || modifier.stat_type == kMaxHealth;
    stamina_maximum_changed = stamina_maximum_changed || modifier.stat_type == kMaxStamina;
  }

  // Observed (status 100597, MaxSP +200): each changed maximum is followed by its current pool,
  // clamped to the new maximum when the status is removed.
  if (health_maximum_changed) {
    changes.push_back({{"stat_type", kHealth}, {"result", adjust_vital(kHealth, 0)}});
  }
  if (stamina_maximum_changed) {
    changes.push_back({{"stat_type", kStamina}, {"result", adjust_vital(kStamina, 0)}});
  }
  packets.push_back({kChangeStatNotification,
                     {{"unit_id", std::to_string(player_id_)},
                      {"tick", wire_monotonic_tick(ticks)},
                      {"default_source", "SCS_StatusEffect"},
                      {"stat_changes", std::move(changes)}}});
}

void CombatSession::apply_monster_modifiers(Monster &monster,
                                            const std::vector<StatusStatModifier> &modifiers,
                                            int sign) {
  for (const auto &modifier : modifiers) {
    const auto key = std::to_string(modifier.stat_type);
    monster.stats[key] = stat_value(monster.stats, modifier.stat_type) + sign * modifier.add;
  }

  // A lowered maximum never leaves the monster above it.
  const int maximum = stat_value(monster.stats, kMaxHealth);
  if (maximum > 0 && stat_value(monster.stats, kHealth) > maximum) {
    monster.stats[std::to_string(kHealth)] = maximum;
  }
}

void CombatSession::remove_status(std::uint64_t holder, const ActiveStatus &status,
                                  std::int64_t tick, std::vector<GameplayPacket> &packets) {
  if (holder == player_id_) {
    // Observed (status 100597, 3 of 3 removals): the stat change precedes the removal.
    apply_stat_modifiers(status.modifiers, -1, tick, packets);
  } else {
    apply_monster_modifiers(monsters().at(holder), status.modifiers, -1);
  }
  append_status_removal(holder, status, tick, packets);
}

bool CombatSession::immune_to(std::uint64_t unit, combat_rules::AbnormalType type) const {
  const std::vector<ActiveStatus> *holder_statuses = nullptr;
  if (unit == player_id_) {
    holder_statuses = &statuses_;
  } else if (const auto monster = monsters().find(unit); monster != monsters().end()) {
    holder_statuses = &monster->second.statuses;
  }
  if (!holder_statuses) {
    return false;
  }

  return std::ranges::any_of(*holder_statuses, [&](const ActiveStatus &status) {
    const auto immunities = data_->status_abnormal_immunities.find(status.status_effect_id);
    return immunities != data_->status_abnormal_immunities.end() &&
           immunities->second.contains(type);
  });
}

const combat_rules::AbnormalGauges *CombatSession::gauges_of(std::uint64_t unit) const {
  if (unit == player_id_) {
    return &gauges_;
  }
  const auto monster = monsters().find(unit);
  return monster == monsters().end() ? nullptr : &monster->second.gauges;
}

void CombatSession::change_abnormal_gauge(std::uint64_t unit, combat_rules::AbnormalType type,
                                          int change, std::uint64_t caster, std::int64_t ticks,
                                          std::vector<GameplayPacket> &packets) {
  if (!data_->abnormal_gauges || change == 0) {
    return;
  }
  const auto &rules = *data_->abnormal_gauges;
  const auto rule = rules.types.find(type);
  const bool on_player = unit == player_id_;
  if (rule == rules.types.end() || (on_player && stat_value(stats_, kHealth) <= 0) ||
      (!on_player && !is_live_monster(unit))) {
    return;
  }
  // Immunity potions (Immune rows 27 to 32) block any gain while their status lasts.
  if (change > 0 && immune_to(unit, type)) {
    return;
  }

  auto &gauges = on_player ? gauges_ : monsters().at(unit).gauges;
  const int current = gauges.value(type, rules, ticks);
  const auto next = static_cast<int>(std::clamp<std::int64_t>(
      static_cast<std::int64_t>(current) + change, 0, rule->second.maximum));
  if (next == current && current == 0) {
    return;
  }

  // Observed: a gauge notification carries the value at its update tick (the client decays it
  // from there); a filled gauge is reported empty, without a value, before its ailment status.
  const bool filled = next >= rule->second.maximum;
  gauges.set(type, filled ? 0 : next, ticks);
  Json gauge{{"type", std::string(combat_rules::abnormal_type_wire_name(type))},
             {"update_tick", wire_monotonic_tick(ticks)}};
  if (!filled && next > 0) {
    gauge["value"] = next;
  }
  packets.push_back({kAbnormalGaugeNotification,
                     {{"unit_id", std::to_string(unit)}, {"gauges", Json::array({gauge})}}});
  if (filled) {
    apply_status(unit, caster, rule->second.status_effect_id, ticks, packets);
  }
}

WorldPosition CombatSession::targeted_area_position(const std::string &position_type,
                                                    const Action &action, std::uint64_t target,
                                                    const WorldPosition &origin,
                                                    const WorldPosition &player_position) const {
  const auto live_position = [&](std::uint64_t unit) -> std::optional<WorldPosition> {
    if (unit == player_id_) {
      return player_position;
    }
    if (unit != 0 && is_live_monster(unit)) {
      return monsters().at(unit).position;
    }
    return std::nullopt;
  };

  const auto skill = data_->skills.find(action.skill_id);
  const double range_mm =
      skill == data_->skills.end() ? 0.0 : skill->second.at("range_mm").get<double>();
  const double reach_mm = range_mm + kPlacementHintToleranceMm;
  const auto within_reach = [&](const WorldPosition &point) {
    return point.map_id == origin.map_id && distance_squared(point, origin) <= reach_mm * reach_mm;
  };

  const bool aimed = position_type == "EX5TargetingPosType_Aim";
  const auto hint = action.placement_hints.find(action.effect_unique_id);
  const Action::PlacementHint *reported =
      hint == action.placement_hints.end() ? nullptr : &hint->second;
  const auto placed = [&](const char *source, const WorldPosition &position) {
    // Diagnostic for live sessions: where untargeted and aimed player areas came from.
    spdlog::info("Combat area placement skill_id={} effect_unique_id={} source={} "
                 "distance_mm={:.0f}",
                 action.skill_id, action.effect_unique_id, source,
                 std::sqrt(distance_squared(position, origin)));
    return position;
  };

  // An aimed area goes where the client aimed; a targeted one on its cast target first.
  if (aimed) {
    if (const auto aim = reported_aim_point(action, origin)) {
      return placed("reported_aim", *aim);
    }
  }
  if (const auto cast_target = live_position(target)) {
    return placed("cast_target", *cast_target);
  }
  if (reported) {
    if (const auto reported_target = live_position(reported->target);
        reported_target && within_reach(*reported_target)) {
      return placed("reported_target", *reported_target);
    }
    if (const auto aim = reported_aim_point(action, origin)) {
      return placed("reported_aim", *aim);
    }
  }

  // CANDIDATE fallback (no official untargeted cast was captured): the point ahead of the caster
  // along the cast yaw at the skill's range, where an unobstructed crosshair aim would end.
  return placed("range_fallback",
                offset_along_yaw(origin, action.yaw,
                                 range_mm / kMillimetresPerCatalogCentimetre, 0.0, 0.0));
}

std::optional<WorldPosition> CombatSession::reported_aim_point(const Action &action,
                                                               const WorldPosition &origin) const {
  const auto hint = action.placement_hints.find(action.effect_unique_id);
  if (hint == action.placement_hints.end() || !hint->second.aim) {
    return std::nullopt;
  }
  const auto skill = data_->skills.find(action.skill_id);
  const double range_mm =
      skill == data_->skills.end() ? 0.0 : skill->second.at("range_mm").get<double>();
  const double reach_mm = range_mm + kPlacementHintToleranceMm;
  const auto &aim = *hint->second.aim;
  if (aim.map_id != origin.map_id || distance_squared(aim, origin) > reach_mm * reach_mm) {
    return std::nullopt;
  }
  return aim;
}

bool CombatSession::needs_placement_report(const Action &action, std::uint64_t caster,
                                           const Json &event) const {
  if (caster != player_id_) {
    return false;
  }
  const auto found = data_->effects.find(std::to_string(event.at("effect_id").get<int>()));
  if (found == data_->effects.end()) {
    return false;
  }
  const auto position_type = found->value("targeting_purpose", Json::object())
                                 .value("targetingPosType", std::string{});

  // A server-driven flight aimed at the client's ground marker (Dragon's Roar) needs the aim.
  // Client-driven flights are launched by the client's own report and need no wait here.
  if (found->at("kind") == "spawnProjectile") {
    if (position_type != "EX5TargetingPosType_Aim") {
      return false;
    }
    const double projectile_id = found->at("parameters").value("projectileId", 0.0);
    const auto projectile =
        data_->projectiles.find(std::to_string(static_cast<long long>(projectile_id)));
    return projectile != data_->projectiles.end() &&
           projectile->at("parameters").value("isServerDriven", false);
  }

  if (found->at("kind") != "spawnAreaOfEffect") {
    return false;
  }
  if (position_type == "EX5TargetingPosType_Aim") {
    return true;
  }
  return position_type == "EX5TargetingPosType_MainTarget" &&
         (action.target == 0 || !is_live_monster(action.target));
}

void CombatSession::record_placement_hint(const Json &report, const WorldPosition &position) {
  if (!action_) {
    return;
  }
  const auto &effect = report.at("skill_effect");
  Action::PlacementHint hint;
  const auto &targets = effect.value("targets", Json::array());
  if (targets.is_array() && !targets.empty() && targets.at(0).contains("unit_id")) {
    const auto &first = targets.at(0).at("unit_id");
    if (first.is_string() && first.get<std::string>() != "0") {
      hint.target = unit_id(first);
    }
  }
  if (effect.contains("aim_position") && effect.at("aim_position").is_object()) {
    hint.aim = wire_point(effect.at("aim_position"), position.map_id);
  }
  action_->placement_hints[effect.value("unique_effect_id", 0)] = hint;
}

void CombatSession::spawn_area(const Json &effect, const Json &parameters, const Action &action,
                               std::uint64_t caster, std::uint64_t target,
                               const WorldPosition &origin, const WorldPosition &player_position,
                               std::int64_t ticks, std::vector<GameplayPacket> &packets,
                               bool at_impact) {
  const double raw_id = parameters.value("areaOfEffectId", 0.0);
  if (!std::isfinite(raw_id) || raw_id < 1 || raw_id > std::numeric_limits<int>::max() ||
      raw_id != std::floor(raw_id)) {
    return;
  }
  const int area_id = static_cast<int>(raw_id);
  const auto definition = data_->areas.find(area_id);
  if (definition == data_->areas.end() || areas_.size() >= kMaxActiveAreas) {
    return;
  }

  // EX5TargetingPosType_Caster places the area at its user (balms), moved by the authored
  // forward/side offset (the lightning line 2266 strikes 2.5 m to 12.5 m ahead). MainTarget
  // places it on the struck unit (the poison bottle's cloud, the elemental bursts) and Aim at
  // the aimed point (rain of fire). A player cast without a usable target uses the client's
  // reported placement and otherwise the point ahead at the skill's range; it never lands on
  // the caster, which made untargeted bursts, fire walls and meteors strike the mage itself.
  const Json targeting_purpose = effect.value("targeting_purpose", Json::object());
  const auto position_type = targeting_purpose.value("targetingPosType", std::string{});
  const bool targeted_placement = position_type == "EX5TargetingPosType_MainTarget" ||
                                  position_type == "EX5TargetingPosType_Aim";
  WorldPosition position = origin;
  if (position_type == "EX5TargetingPosType_Caster") {
    position = area_spawn_position(targeting_purpose, origin, action.yaw);
  } else if (targeted_placement && caster == player_id_ && !at_impact) {
    position = targeted_area_position(position_type, action, target, origin, player_position);
  } else if (position_type == "EX5TargetingPosType_MainTarget" && target != 0) {
    if (target == player_id_) {
      position = player_position;
    } else if (monsters().contains(target)) {
      position = monsters().at(target).position;
    }
  }

  ActiveArea area;
  area.unit_id = allocate_area_unit_id();
  area.cms_id = area_id;
  area.definition = &definition->second;
  area.caster = caster;
  area.position = position;
  area.position.yaw = action.yaw;
  area.action = action;
  area.next_hit_tick = ticks;
  area.end_tick = ticks + definition->second.duration_ms * kTicksPerMillisecond;

  // Observed shape of an own area announcement (official poison-bottle cloud, skill object
  // 100302): UnitAddNoti with a skill_object nub of type AreaOfEffect and its lifetime.
  packets.push_back({kUnitAddNotification,
                     {{"unit_id", std::to_string(area.unit_id)},
                      {"nub",
                       {{"pos", wire_position(position)},
                        {"yaw", area.position.yaw},
                        {"skill_object",
                         {{"type", "AreaOfEffect"},
                          {"skillobject_cms_id", area_id},
                          {"caster_unit_id", std::to_string(caster)},
                          {"created_tick", wire_monotonic_tick(ticks)},
                          {"duration", definition->second.duration_ms},
                          {"areaofeffect", Json::object()}}}}}}});
  areas_.push_back(std::move(area));

  // Observed (113 official area spawns with an authored spawn effect): the spawn hit lands at
  // the created tick, right after the UnitAddNoti, with the area unit as caster; the elemental
  // bursts (2274/2275) deal their damage only this way. The area is registered first so the
  // kMaxActiveAreas cap also bounds a spawn effect that spawns further areas. A copy is used
  // because such a chained spawn may reallocate areas_.
  const int spawn_effect_id = definition->second.spawn_effect_id;
  if (spawn_effect_id > 0) {
    const ActiveArea spawned = areas_.back();
    apply_area_effect(spawned, spawn_effect_id, ticks, player_position, packets);
  }
}

void CombatSession::apply_area_effect(const ActiveArea &area, int effect_id, std::int64_t ticks,
                                      const WorldPosition &player_position,
                                      std::vector<GameplayPacket> &packets) {
  const auto &effect = data_->effects.at(std::to_string(effect_id));
  const int effect_limit = static_cast<int>(effect.at("targeting").value("maxTargetCount", 1.0));
  for (const auto target : area_targets(area, effect_limit, player_position)) {
    apply_effect(effect_id, area.action, area.caster, target, area.position, player_position,
                 ticks, packets, 0, nullptr, std::nullopt, area.unit_id);
  }
}

std::uint64_t CombatSession::allocate_area_unit_id() {
  // At most kMaxActiveAreas ids are live at once, so the scan ends within a few steps even
  // after the 24-bit sequence wraps.
  while (true) {
    const std::uint64_t sequence = next_area_serial_ % kAreaUnitSequenceCount;
    ++next_area_serial_;
    const std::uint64_t unit_id = kAreaUnitIdBase | (sequence << 4) | kAreaUnitTypeNibble;
    const bool in_use = std::ranges::any_of(
        areas_, [unit_id](const ActiveArea &area) { return area.unit_id == unit_id; });
    if (!in_use) {
      return unit_id;
    }
  }
}

std::vector<std::uint64_t> CombatSession::area_targets(const ActiveArea &area, int effect_limit,
                                                       const WorldPosition &player_position) const {
  const auto inside = [&area](const WorldPosition &position) {
    const double dx = static_cast<double>(position.x_mm) - area.position.x_mm;
    const double dy = static_cast<double>(position.y_mm) - area.position.y_mm;
    const double dz = static_cast<double>(position.z_mm) - area.position.z_mm;
    if (area.definition->shape == "box") {
      const double radians = area.position.yaw * std::numbers::pi / 180;
      const double forward = std::cos(radians) * dx + std::sin(radians) * dy;
      const double side = -std::sin(radians) * dx + std::cos(radians) * dy;
      return std::abs(forward) <= area.definition->extent_x_mm &&
             std::abs(side) <= area.definition->extent_y_mm &&
             std::abs(dz) <= area.definition->half_height_mm;
    }
    const double radius = area.definition->radius_mm;
    return dx * dx + dy * dy <= radius * radius && std::abs(dz) <= area.definition->half_height_mm;
  };

  if (area.caster != player_id_) {
    if (stat_value(stats_, kHealth) > 0 && inside(player_position)) {
      return {player_id_};
    }
    return {};
  }

  std::vector<std::pair<double, std::uint64_t>> candidates;
  for (const auto &[id, monster] : monsters()) {
    if (is_live_monster(id) && inside(monster.position)) {
      candidates.emplace_back(distance_squared(area.position, monster.position), id);
    }
  }
  std::ranges::sort(candidates);

  const int limit = area.definition->max_targets > 0 ? area.definition->max_targets
                                                     : std::clamp(effect_limit, 1, 32);
  std::vector<std::uint64_t> result;
  for (const auto &[distance, id] : candidates) {
    if (result.size() >= static_cast<std::size_t>(limit)) {
      break;
    }
    result.push_back(id);
  }
  return result;
}

void CombatSession::advance_areas(std::int64_t ticks, const WorldPosition &player_position,
                                  std::vector<GameplayPacket> &packets) {
  // Indexed access: an interval effect may spawn another area and grow areas_.
  for (std::size_t index = 0; index < areas_.size(); ++index) {
    const AreaDefinition &definition = *areas_[index].definition;
    while (!areas_[index].removal_tick && areas_[index].next_hit_tick < areas_[index].end_tick &&
           areas_[index].next_hit_tick <= ticks) {
      const ActiveArea area = areas_[index];
      apply_area_effect(area, definition.interval_effect_id, area.next_hit_tick, player_position,
                        packets);
      areas_[index].next_hit_tick += definition.interval_ms * kTicksPerMillisecond;
    }

    if (!areas_[index].removal_tick && ticks >= areas_[index].end_tick) {
      const ActiveArea expired = areas_[index];
      areas_[index].removal_tick = expired.end_tick + kAreaRemovalDelayMs * kTicksPerMillisecond;
      // Observed (11 of 11 official lightning-storm hits, effect 160212 of area 160501): the
      // lifetime-end hit lands at the end tick with the area unit as caster, ahead of the
      // LifeTime despawn. Its own shape around the area selects the targets. A copy is used
      // because the effect may spawn further areas and reallocate areas_.
      if (definition.despawn_effect_id > 0) {
        apply_effect(definition.despawn_effect_id, expired.action, expired.caster, 0,
                     expired.position, player_position, expired.end_tick, packets, 0, nullptr,
                     std::nullopt, expired.unit_id);
      }
      packets.push_back({kDespawnSkillObjectNotification,
                         {{"unit_id", std::to_string(expired.unit_id)},
                          {"reason_type", "EX5SkillObjectDespawnReasonType_LifeTime"}}});
    }
    const auto &area = areas_[index];
    if (area.removal_tick && ticks >= *area.removal_tick) {
      packets.push_back(
          {kUnitRemoveNotification, {{"unit_ids", Json::array({std::to_string(area.unit_id)})}}});
    }
  }
  std::erase_if(areas_, [ticks](const ActiveArea &area) {
    return area.removal_tick && ticks >= *area.removal_tick;
  });
}

const std::vector<int> *CombatSession::item_effect_list(int item_cms_id) const {
  const auto world_lock = lock_world_mobs();
  const auto found = data_->item_effect_lists.find(item_cms_id);
  return found == data_->item_effect_lists.end() ? nullptr : &found->second;
}

std::vector<GameplayPacket> CombatSession::apply_item_effects(const std::vector<int> &effect_ids,
                                                              const WorldPosition &position,
                                                              std::int64_t ticks) {
  const auto world_lock = lock_world_mobs();
  std::vector<GameplayPacket> packets;
  if (player_dead()) {
    return packets;
  }

  // A consumable used without a skill has no action of its own; its effects target the user.
  Action action;
  for (const int effect_id : effect_ids) {
    apply_effect(effect_id, action, player_id_, player_id_, position, position, ticks, packets);
  }
  return packets;
}

int CombatSession::abnormal_gauge(std::uint64_t unit_id, combat_rules::AbnormalType type,
                                  std::int64_t ticks) const {
  const auto world_lock = lock_world_mobs();
  const auto *gauges = gauges_of(unit_id);
  if (!gauges || !data_->abnormal_gauges) {
    return 0;
  }
  return gauges->value(type, *data_->abnormal_gauges, ticks);
}

int CombatSession::monster_stat(std::uint64_t unit_id, int stat_type) const {
  const auto world_lock = lock_world_mobs();
  return stat_value(monsters().at(unit_id).stats, stat_type);
}

bool CombatSession::spend_action_item(const Action &action, const WorldPosition &position,
                                      std::int64_t ticks, std::vector<GameplayPacket> &packets) {
  if (!owns_item_use() || !action_ || action_->instance != action.instance ||
      action_->item_cms_id == 0) {
    return false;
  }
  if (action_->item_spent) {
    return true;
  }

  // The item can leave the inventory during the animation (sold, discarded or moved). The use
  // then fails: the action ends before any of its effects applies.
  if (item_inventory_.quantity(action_->item_cms_id) <= 0) {
    finish(action_, player_id_, position, ticks, packets, "EX5EndSkillReason_Default");
    return false;
  }

  // Observed (official potion, dagger, ice wedge and stamina potion uses): the stack update is
  // sent at the consumption event, before the effects that the item grants.
  auto consumption = item_inventory_.consume_one(action_->item_cms_id);
  action_->item_spent = true;
  append(packets, std::move(consumption));
  // A spent item counts toward CounterUseItem and UseItemInTriggerVolume objectives.
  quest_events_.push_back({QuestEventKind::ItemUsed, action_->item_cms_id, 0, 1});
  return true;
}

void CombatSession::start_stat_recovery(int effect_id, const Json &parameters,
                                        const Action &action, std::int64_t ticks,
                                        std::vector<GameplayPacket> &packets) {
  const double raw_id = parameters.value("statRecoveryCmsId", 0.0);
  if (!std::isfinite(raw_id) || raw_id < 1 || raw_id > std::numeric_limits<int>::max() ||
      raw_id != std::floor(raw_id)) {
    return;
  }
  const int recovery_cms_id = static_cast<int>(raw_id);
  const auto key = std::to_string(recovery_cms_id);
  if (!data_->recoveries.contains(key) || recoveries_.size() >= kMaxActiveRecoveries) {
    return;
  }

  const auto &definition = data_->recoveries.at(key);
  const int interval_ms = definition.at("interval_ms");
  const int duration_ms = definition.at("duration_ms");
  auto amount = static_cast<std::int64_t>(definition.at("amount").get<double>());
  if (definition.value("use_potion_heal_rate", false)) {
    const std::int64_t heal_rate =
        stats_.value(std::to_string(kPotionHealRateStat), kNeutralStatRate);
    amount = amount * std::max<std::int64_t>(0, heal_rate) / kNeutralStatRate;
  }
  if (duration_ms <= 0 || amount <= 0) {
    return;
  }

  Recovery recovery;
  recovery.instance = next_recovery_instance_++;
  recovery.stat = definition.at("stat");
  recovery.amount = static_cast<int>(
      std::min(amount, static_cast<std::int64_t>(kMaxRecoveryAmount)));
  recovery.interval_ms = interval_ms;
  recovery.start_tick = ticks;
  recovery.end_tick = ticks + duration_ms * kTicksPerMillisecond;
  const bool player_action = action_ && action_->instance == action.instance;
  if (player_action && definition.value("cancel_type", std::string("None")) == "EndSkillStage") {
    recovery.cancelled_with_action = action.instance;
  }
  recoveries_.push_back(recovery);

  // Observed (22 of 22 official potion drinks): StatSyncNoti announces the flow with the current
  // value as its checkpoint, then AddStatRecoveryNoti names the effect and the catalog row. The
  // client simulates the flow; no per-tick stat change follows.
  const double rate_per_second = static_cast<double>(recovery.amount) * 1000.0 / interval_ms;
  const auto instance_text = std::to_string(recovery.instance);
  packets.push_back(
      {kStatSyncNotification,
       {{"unit_id", std::to_string(player_id_)},
        {"tick", wire_monotonic_tick(ticks)},
        {"flows",
         {{{"stat_type", recovery.stat},
           {"source", "SCS_StatRecoveryFlow"},
           {"instance_id", instance_text},
           {"rate_per_sec", rate_per_second},
           {"start_tick", wire_monotonic_tick(recovery.start_tick)},
           {"end_tick", wire_monotonic_tick(recovery.end_tick)},
           {"checkpoint",
            {{"stat_type", recovery.stat},
             {"source", "SCS_StatRecoveryFlow"},
             {"result", stat_value(stats_, recovery.stat)}}}}}}}});
  packets.push_back({kAddStatRecoveryNotification,
                     {{"unit_id", std::to_string(player_id_)},
                      {"skill_effect_id", effect_id},
                      {"stat_recovery",
                       {{"cms_id", recovery_cms_id},
                        {"instance_id", instance_text},
                        {"recovery_value_per_sec", rate_per_second},
                        {"start_tick", wire_monotonic_tick(recovery.start_tick)},
                        {"end_tick", wire_monotonic_tick(recovery.end_tick)}}}}});
}

void CombatSession::settle_recoveries(std::int64_t ticks) {
  for (auto &recovery : recoveries_) {
    // A dead player gains nothing; revival clears every recovery.
    if (stat_value(stats_, kHealth) <= 0) {
      break;
    }
    const auto until = std::min(ticks, recovery.end_tick);
    if (until <= recovery.start_tick) {
      continue;
    }
    const auto elapsed_ms = (until - recovery.start_tick) / kTicksPerMillisecond;
    const auto due = static_cast<std::int64_t>(recovery.amount) * elapsed_ms /
                     recovery.interval_ms;
    if (due > recovery.applied) {
      adjust_vital(recovery.stat, due - recovery.applied);
      recovery.applied = due;
    }
  }
  std::erase_if(recoveries_, [ticks](const Recovery &recovery) {
    return recovery.end_tick <= ticks;
  });
}

void CombatSession::cancel_action_recoveries(std::uint32_t action_instance, std::int64_t ticks,
                                             std::vector<GameplayPacket> &packets) {
  settle_recoveries(ticks);

  std::vector<Recovery> cancelled;
  std::erase_if(recoveries_, [&](const Recovery &recovery) {
    if (recovery.cancelled_with_action != action_instance) {
      return false;
    }
    cancelled.push_back(recovery);
    return true;
  });

  // Candidate: no official cancelled recovery was captured. The stop follows the observed end
  // of a sprint flow: the same flow instance again, without a rate, checkpointing the value.
  for (const auto &recovery : cancelled) {
    packets.push_back(
        {kStatSyncNotification,
         {{"unit_id", std::to_string(player_id_)},
          {"tick", wire_monotonic_tick(ticks)},
          {"flows",
           {{{"stat_type", recovery.stat},
             {"source", "SCS_StatRecoveryFlow"},
             {"instance_id", std::to_string(recovery.instance)},
             {"checkpoint",
              {{"stat_type", recovery.stat},
               {"source", "SCS_StatRecoveryFlow"},
               {"result", stat_value(stats_, recovery.stat)}}}}}}}});
  }
}

void CombatSession::append_status_removal(std::uint64_t holder, const ActiveStatus &status,
                                          std::int64_t tick,
                                          std::vector<GameplayPacket> &packets) const {
  packets.push_back({kRemoveStatusEffectNotification,
                     {{"unit_id", std::to_string(holder)},
                      {"tick", wire_monotonic_tick(tick)},
                      {"status_effects",
                       {{{"instance_id", status.instance},
                         {"status_effect_id", status.status_effect_id}}}}}});
}

void CombatSession::expire_statuses(std::int64_t ticks, std::vector<GameplayPacket> &packets) {
  const auto take_expired = [ticks](std::vector<ActiveStatus> &statuses) {
    std::vector<ActiveStatus> expired;
    std::erase_if(statuses, [&](const ActiveStatus &status) {
      if (status.expire_tick > ticks) {
        return false;
      }
      expired.push_back(status);
      return true;
    });
    return expired;
  };

  // Candidate: the official removal lags the expiry by tens of milliseconds; the expiry tick is
  // used.
  for (const auto &status : take_expired(statuses_)) {
    remove_status(player_id_, status, status.expire_tick, packets);
  }
  for (auto &[id, monster] : monsters()) {
    if (monster.private_owner != 0 && monster.private_owner != player_id_) {
      continue;
    }
    for (const auto &status : take_expired(monster.statuses)) {
      remove_status(id, status, status.expire_tick, packets);
    }
  }
}

void CombatSession::clear_statuses_on_death(std::int64_t ticks,
                                            std::vector<GameplayPacket> &packets) {
  // Observed: in the three captures a player's active timed status (4001, 9.9 s remaining) was
  // removed 98 ms after its death and another 1438 ms after, before any revive request. The delay
  // is Candidate (two samples), so the removal is reported at the death tick.
  const auto removed = std::move(statuses_);
  statuses_.clear();
  for (const auto &status : removed) {
    remove_status(player_id_, status, ticks, packets);
  }
  gauges_.clear();
}

void CombatSession::begin_npc_lunge(std::uint64_t id, Monster &monster,
                                    const WorldPosition &target, std::int64_t ticks,
                                    std::vector<GameplayPacket> &packets) {
  if (!monster.action || monster.action->stage != 0 || monster.action->lunge_window_checked) {
    return;
  }
  const auto found = data_->root_motion.find(
      std::make_tuple(monster.action->skill_id, monster.action->layer, monster.action->stage));
  const auto npc_collision = data_->npc_collision.find(monster.cms_id);
  const auto player_radius = common_player_radius(*data_);
  if (found == data_->root_motion.end() || npc_collision == data_->npc_collision.end() ||
      !player_radius ||
      !supported_lunge_window(found->second, stage(*monster.action).at("duration_ms").get<int>())) {
    monster.action->lunge_window_checked = true;
    return;
  }
  const auto &window = found->second;

  // A delayed window starts when the attack's wind-up ends; the target is sampled then.
  const auto window_start_tick =
      monster.action->started + window.window_start_ms * kTicksPerMillisecond;
  if (ticks < window_start_tick) {
    return;
  }
  monster.action->lunge_window_checked = true;
  if (window.window_start_ms > 0 &&
      (target.x_mm != monster.position.x_mm || target.y_mm != monster.position.y_mm)) {
    // LookAtDir_ToTarget: a delayed dash turns toward where the target is now, and the attack's
    // later hit shapes follow that facing.
    monster.action->yaw = static_cast<float>(
        std::atan2(static_cast<double>(target.y_mm) - monster.position.y_mm,
                   static_cast<double>(target.x_mm) - monster.position.x_mm) *
        180 / std::numbers::pi);
  }

  // Destination: the target backed off by the authored offset plus both game-hit radii, along
  // the caster-to-target direction, clamped to max_move_distance from the caster start (3D).
  const WorldPosition start = monster.position;
  const double to_x = static_cast<double>(target.x_mm) - start.x_mm;
  const double to_y = static_cast<double>(target.y_mm) - start.y_mm;
  const double to_z = static_cast<double>(target.z_mm) - start.z_mm;
  const double distance = std::sqrt(to_x * to_x + to_y * to_y + to_z * to_z);
  if (distance == 0.0) {
    return;
  }
  const double standoff = window.offset_distance_mm + npc_collision->second.radius_mm + *player_radius;
  double move_x = to_x - to_x / distance * standoff;
  double move_y = to_y - to_y / distance * standoff;
  double move_z = to_z - to_z / distance * standoff;
  double displacement = std::sqrt(move_x * move_x + move_y * move_y + move_z * move_z);
  if (displacement > window.max_move_distance_mm) {
    const double clamp = window.max_move_distance_mm / displacement;
    move_x *= clamp;
    move_y *= clamp;
    move_z *= clamp;
    displacement = window.max_move_distance_mm;
  }
  WorldPosition destination = start;
  destination.x_mm = static_cast<int>(std::llround(start.x_mm + move_x));
  destination.y_mm = static_cast<int>(std::llround(start.y_mm + move_y));
  destination.z_mm = static_cast<int>(std::llround(start.z_mm + move_z));
  destination.yaw = monster.action->yaw;
  if (displacement == 0.0 || (destination.x_mm == start.x_mm && destination.y_mm == start.y_mm &&
                              destination.z_mm == start.z_mm)) {
    return;
  }

  LungeMotion motion;
  motion.window = &window;
  motion.start = start;
  motion.start.yaw = monster.action->yaw;
  motion.destination = destination;
  motion.scale = displacement / window.samples.back().x_mm;
  motion.start_tick = monster.action->started + window.window_start_ms * kTicksPerMillisecond;
  motion.end_tick = monster.action->started + window.window_end_ms * kTicksPerMillisecond;

  Json root_motion{{"start_pos", wire_position(motion.start)},
                   {"dest_pos", wire_position(motion.destination)},
                   {"end_tick", wire_monotonic_tick(motion.end_tick)},
                   {"rotate_duration", window.window_end_ms - window.window_start_ms},
                   {"rotation", {{"yaw", motion.destination.yaw}}},
                   {"start_yaw", motion.start.yaw},
                   {"ground_move", window.ground_move},
                   {"scale", motion.scale},
                   {"skill_instance_id", monster.action->instance},
                   {"target_unit_id", std::to_string(player_id_)}};
  packets.push_back({64, {{"unit_id", std::to_string(id)}, {"root_motion", std::move(root_motion)}}});
  spdlog::info("Combat npc_lunge unit_id={} skill_id={} instance_id={} displacement_mm={:.0f}", id,
               monster.action->skill_id, monster.action->instance, displacement);
  monster.action->lunge = motion;
}

void CombatSession::sync_npc_lunge_pose(Monster &monster, std::int64_t ticks) {
  if (!monster.action || !monster.action->lunge || monster.action->stage != 0) {
    return;
  }
  const auto pose = monster.action->lunge->pose_at(ticks);
  monster.position = pose;
  if (monster.ai) {
    monster.ai->place(pose);
  }
}

void CombatSession::emit_lunge_end(std::uint64_t caster, Action &action, const WorldPosition &pose,
                                   std::int64_t end_tick, std::vector<GameplayPacket> &packets) {
  if (!action.lunge || action.lunge->end_sent) {
    return;
  }
  packets.push_back({106,
                     {{"unit_id", std::to_string(caster)},
                      {"skill_instance_id", action.instance},
                      {"end_tick", wire_monotonic_tick(end_tick)},
                      {"end_yaw", pose.yaw},
                      {"dest_pos", wire_position(pose)}}});
  action.lunge->end_sent = true;
}

void CombatSession::advance_action(std::optional<Action> &action, std::uint64_t caster,
                                   const WorldPosition &origin,
                                   const WorldPosition &player_position, std::int64_t ticks,
                                   std::vector<GameplayPacket> &packets, int transitions) {
  if (!action || transitions >= 64) {
    return;
  }
  const auto &definition = stage(*action);
  const auto elapsed = (ticks - action->started) / kTicksPerMillisecond;
  int index = 0;
  for (const auto &event : definition.at("events")) {
    if (elapsed >= event.at("time_ms").get<int>() && !action->executed_events.contains(index)) {
      // A targeted or aimed area waits briefly for the client's placement report; once it
      // arrives (or the wait ends) the area appears at that moment, as official spawns follow
      // the client's report.
      const bool needs_report = needs_placement_report(*action, caster, event);
      const bool reported = action->placement_hints.contains(event.at("unique_id").get<int>());
      const int wait_end_ms = std::min(event.at("time_ms").get<int>() + kPlacementHintWaitMs,
                                       definition.at("duration_ms").get<int>());
      if (needs_report && !reported && elapsed < wait_end_ms) {
        ++index;
        continue;
      }
      if (action_copy_hook_for_test_) {
        action_copy_hook_for_test_();
      }
      auto copy = *action;
      copy.executed_events.insert(index);
      copy.effect_unique_id = event.at("unique_id").get<int>();
      const int effect_id = event.at("effect_id").get<int>();
      auto event_tick =
          copy.started + event.at("time_ms").get<std::int64_t>() * kTicksPerMillisecond;
      if (needs_report) {
        event_tick = std::max(event_tick, ticks);
      }
      // The event's own collision shape (verified cylinder sector only) replaces the generic
      // effect targeting for this event, for NPC casters only: the shape data was verified
      // against NPC casts and player attacks keep the generic effect path even when the profile
      // carries event.shape. Chained effects keep their own shape.
      const Json *event_shape =
          caster != player_id_ && event.contains("shape") ? &event.at("shape") : nullptr;
      // A hit is evaluated from the lunge pose at the event's own time, never from the pose at
      // the (possibly much later) poll that resolves it.
      WorldPosition event_origin = origin;
      if (copy.lunge && copy.stage == 0) {
        event_origin = copy.lunge->pose_at(
            copy.started + static_cast<std::int64_t>(event.at("time_ms").get<int>()) *
                               kTicksPerMillisecond);
      }
      // Copy and event setup may allocate; the live action must stay retryable until they finish.
      action->executed_events.insert(index);
      apply_effect(effect_id, copy, caster, 0, event_origin, player_position,
                   event_tick, packets, 0, event_shape);
      if (!action) {
        return;
      }
    }
    ++index;
  }
  if (action->lunge && !action->lunge->end_sent && ticks >= action->lunge->end_tick) {
    emit_lunge_end(caster, *action, action->lunge->destination, action->lunge->end_tick, packets);
  }
  if (elapsed >= definition.at("duration_ms").get<int>()) {
    const int next = definition.at("next_stage");
    // A missing default stage ends the action instead of losing its definition.
    if (next >= 0 && next != action->stage && find_stage(*action, next)) {
      const auto stage_end =
          action->started + definition.at("duration_ms").get<std::int64_t>() * kTicksPerMillisecond;
      enter_stage(*action, next, stage_end);
      advance_action(action, caster, origin, player_position, ticks, packets, transitions + 1);
    } else {
      finish(action, caster, origin, ticks, packets, "EX5EndSkillReason_Default");
    }
  }
}

// Sends one PS2C_MoveNoti for the monster at its current authoritative position. The wire tick is
// the existing monotonic nanosecond clock, forced strictly increasing per monster (the client
// derives speed from |dpos| / dtick). Pursuit uses Run because its trajectory uses run speed.
// A moving frame also
// carries pred_milli (the expected time until the next frame at the route speed, at least 16 ms
// and at most kNpcMoveNotiMaxIntervalMs) and the goal pred_pos: the authoritative route position
// pred_milli ahead (x, y, z), clamped to the route end. velocity is never set (the client ignores
// it and pred_pos alone selects the moving branch). A Stop carries neither.
void CombatSession::announce_npc_move(std::uint64_t id, Monster &monster, bool moving,
                                      std::int64_t ticks, std::vector<GameplayPacket> &packets) {
  const auto move_tick = std::max(ticks, monster.last_move_tick + 1);
  Json message{{"unit_id", std::to_string(id)},
               {"tick", wire_monotonic_tick(move_tick)},
               {"mode", "MoveMode_Normal"},
               {"detail", moving ? "MoveDetail_Run" : "MoveDetail_Stop"},
               {"pos", wire_position(monster.position)},
               {"body_yaw", monster.position.yaw}};
  if (moving) {
    const auto &definition = monster.ai->definition();
    const double speed_mm_per_second =
        definition.run_speed_mm_per_second * definition.move_speed_rate / 1000.0;
    const double step_milliseconds = speed_mm_per_second > 0
                                         ? kNpcMoveNotiMinStepMm / speed_mm_per_second * 1000.0
                                         : static_cast<double>(kNpcMoveNotiMaxIntervalMs);
    const auto pred_milli = std::clamp<std::int64_t>(std::llround(step_milliseconds),
                                                     kNpcMoveNotiMinPredMilli,
                                                     kNpcMoveNotiMaxIntervalMs);
    const auto goal = monster.ai->route_position_at(move_tick + pred_milli * kTicksPerMillisecond);
    message["pred_milli"] = pred_milli;
    message["pred_pos"] = wire_position(goal.value_or(monster.position));
    monster.last_move_position = monster.position;
  }
  packets.push_back({177, std::move(message)});
  monster.last_move_tick = move_tick;
  monster.move_announced = moving;
}

namespace {
std::atomic<bool> idle_early_out_enabled{true};
} // namespace

void CombatSession::set_idle_early_out_enabled_for_test(bool enabled) {
  idle_early_out_enabled.store(enabled);
}

bool CombatSession::idle_mob_not_due(std::uint64_t id, const Monster &monster, int player_map_id,
                                     std::int64_t ticks) const {
  // Cheap tests first; monster_hp does a keyed stat lookup, so it is last. An attack-less mob has
  // attack_skill == nullptr for good, hence attack_resolved.
  return idle_early_out_enabled.load(std::memory_order_relaxed) && monster.ai && !monster.action &&
         monster.active && monster.hostile && (monster.attack_skill || monster.attack_resolved) &&
         monster.ai->idle_and_not_due(ticks, player_map_id) && monster_hp(id) > 0;
}

std::string CombatSession::monster_state_fingerprint_for_test() const {
  const auto world_lock = lock_world_mobs();
  std::string text;
  for (const auto &[id, monster] : monsters()) {
    text += std::to_string(id) + ":" + std::to_string(monster.position.map_id) + "," +
            std::to_string(monster.position.x_mm) + "," + std::to_string(monster.position.y_mm) +
            "," + std::to_string(monster.position.z_mm) + " aggro=" +
            std::to_string(monster.aggro) + " dmg=" + std::to_string(monster.damaged) +
            " own=" + std::to_string(monster.simulation_owner) +
            " act=" + std::to_string(monster.active) + " hp=" + std::to_string(monster_hp(id)) +
            " resolved=" + std::to_string(monster.attack_resolved) +
            " skill=" + std::to_string(monster.attack_skill != nullptr) +
            " eng=" + std::to_string(monster.engagement_range_mm) +
            " action=" + std::to_string(monster.action.has_value()) +
            " nextatk=" + std::to_string(monster.next_attack);
    if (monster.ai) {
      text += " ai=" + std::to_string(static_cast<int>(monster.ai->state())) +
              " due=" + (monster.ai->next_action_tick()
                             ? std::to_string(*monster.ai->next_action_tick())
                             : std::string("none")) +
              " moving=" + std::to_string(monster.ai->moving());
    }
    text += " ready=";
    for (const auto &[skill, tick] : monster.skill_ready_ticks) {
      text += std::to_string(skill) + "@" + std::to_string(tick) + ";";
    }
    text += " used=";
    for (const auto &[skill, tick] : monster.skill_last_used_ticks) {
      text += std::to_string(skill) + "@" + std::to_string(tick) + ";";
    }
    text += "\n";
  }
  return text;
}

void CombatSession::advance_npc_ai(std::uint64_t id, Monster &monster,
                                   const WorldPosition &player_position, std::int64_t ticks,
                                   std::vector<GameplayPacket> &packets) {
  if (world_mobs_) {
    if (idle_mob_not_due(id, monster, player_position.map_id, ticks)) {
      // Exactly what the full path below ends with for such a mob: NpcAi::advance returns an
      // empty update, so only these two assignments take effect (see idle_mob_not_due).
      monster.aggro = false;
      monster.position = monster.ai->position();
      world_metrics::add(world_metrics::CounterId::MobsSkippedIdleNotDue);
      return;
    }
    world_metrics::add(world_metrics::CounterId::MobsEvaluated);
  }
  if (world_mobs_ && !monster.attack_skill) {
    monster.attack_skill = usable_npc_attack(*data_, monster.cms_id);
    monster.engagement_range_mm = npc_engagement_range_mm(
        *data_, monster.cms_id, monster.ai->definition(), monster.attack_skill);
    monster.attack_resolved = true;
  }
  // Put a lunging monster on its pose for THIS tick before the AI decides anything, so a cancel,
  // leash exit or return route starts from where the unit really is and the interruption 106
  // reports that pose, not the previous poll's. A dead or inactive monster is never moved again.
  if (monster.active && monster_hp(id) > 0) {
    sync_npc_lunge_pose(monster, ticks);
  }
  NpcAiInput input;
  input.player_position = player_position;
  input.ticks = ticks;
  input.player_alive = stat_value(stats_, kHealth) > 0;
  input.monster_active = monster.active;
  input.monster_alive = monster_hp(id) > 0;
  input.monster_hostile = monster.hostile;
  input.action_active = monster.action.has_value();
  input.damaged = monster.damaged;
  input.perception_enabled = monster.attack_skill != nullptr && monster.proactive && !gm_hidden_;
  input.engagement_range_mm = monster.engagement_range_mm;
  if (!monster.action && monster.attack_skill) {
    const double distance_mm = std::sqrt(distance_squared(monster.position, player_position));
    const auto castable = [&](const NpcAttackOption &attack) {
      return distance_mm <=
             npc_cast_range_mm(attack, monster.ai->definition()) + kNpcPursuitSettleToleranceMm;
    };
    const auto ready =
        select_npc_skill(*data_, monster.cms_id, monster.skill_ready_ticks, distance_mm, ticks,
                         castable, monster.skill_last_used_ticks);
    if (ready) {
      // Stop the route before announcing the cast. Long-range attacks with a zero minimum
      // distance are just as eligible as attacks with an explicit ranged band.
      input.action_active = true;
    } else {
      double nearest_band = std::numeric_limits<double>::max();
      bool chosen_ready = false;
      for (const auto &attack : data_->npc_attack_options(monster.cms_id)) {
        const auto cooldown = monster.skill_ready_ticks.find(attack.skill_id);
        const bool candidate_ready =
            cooldown == monster.skill_ready_ticks.end() || cooldown->second <= ticks;
        const auto minimum = attack.min_range_mm;
        const auto cast_range = npc_cast_range_mm(attack, monster.ai->definition());
        if (cast_range < minimum) {
          // Its hit shape cannot reach as far as its catalog minimum distance, so no distance
          // lets it land; standing in that band would leave the monster idle forever.
          continue;
        }
        const double gap =
            std::max({static_cast<double>(minimum) - distance_mm, distance_mm - cast_range, 0.0});
        if ((candidate_ready && !chosen_ready) ||
            (candidate_ready == chosen_ready && gap < nearest_band)) {
          nearest_band = gap;
          chosen_ready = candidate_ready;
          input.minimum_engagement_range_mm = minimum;
          input.engagement_range_mm = std::max<std::int64_t>(1, cast_range);
        }
      }
    }
  }
  const auto update = monster.ai->advance(input);
  if (update.acquired || monster.ai->state() != NpcAiState::Idle) {
    monster.damaged = false;
  }

  // Every other consumer (targeting, ranges, spawn and death packets) reads monster.position.
  monster.position = monster.ai->position();

  if (world_mobs_ && update.acquired && monster.move_announced && !monster.ai->moving()) {
    announce_npc_move(id, monster, false, ticks, packets);
  }

  if (update.cancel_action) {
    finish(monster.action, id, monster.position, ticks, packets, "EX5EndSkillReason_Default");
    cancel_npc_projectiles(id, monster.position, ticks, packets);
  }

  if (update.acquired) {
    monster.next_attack = ticks + kTicksPerSecond;
    packets.push_back(
        {98, {{"unit_id", std::to_string(id)}, {"new_target_unit_id", std::to_string(player_id_)}}});
    packets.push_back({32,
                       {{"unit_id", std::to_string(id)},
                        {"state", "EX5UnitStateType_Combat"},
                        {"tick", wire_monotonic_tick(ticks)}}});
    if (const auto phase = captured_tutorial_combat_phase(monster.cms_id)) {
      packets.push_back(
          {114, {{"unit_id", std::to_string(id)}, {"new_ai_phase_transition_id", *phase}}});
    }
  }

  // Captured death clears are 98 then 114 without target/phase fields. The Return state on
  // leash is a local choice: no leash was captured.
  if (update.aggro_released) {
    packets.push_back({98, {{"unit_id", std::to_string(id)}}});
    if (update.target_cleared) {
      packets.push_back({32,
                         {{"unit_id", std::to_string(id)},
                          {"state", "EX5UnitStateType_Return"},
                          {"tick", wire_monotonic_tick(ticks)}}});
    }
    if (captured_tutorial_combat_phase(monster.cms_id)) {
      packets.push_back({114, {{"unit_id", std::to_string(id)}}});
    }
  }

  // Ordinary pursuit has no active skill instance. The client ignores 64/106 without
  // that lifecycle; reserve them for real attack lunges (begin_npc_lunge).
  // PS2C_MoveNoti for the pursuit (candidate contract, builder-2 addenda 25-28): a Run at the
  // route start, then a Run whenever the monster has moved at least kNpcMoveNotiMinStepMm since
  // the last moving frame (the client snaps updates within 500 mm of its own position) or
  // kNpcMoveNotiMaxIntervalMs has passed (the client's blend window is about 1 s); a Stop at the
  // final position when the route ends (arrival, leash/reset, lost target, death) and no Walk
  // after a Stop until a new route starts. Moving frames carry the route goal.
  if (update.motion_start) {
    // A re-plan toward a moving target while the client is already walking is not a new walk:
    // it follows the same displacement/interval cadence as continued frames.
    if (!monster.move_announced ||
        npc_move_frame_due(monster.position, monster.last_move_position, ticks,
                           monster.last_move_tick)) {
      announce_npc_move(id, monster, true, ticks, packets);
    }
  } else if (update.motion_complete_tick && monster.move_announced) {
    announce_npc_move(id, monster, false, ticks, packets);
  } else if (monster.move_announced && monster.ai->moving() && update.moved &&
             npc_move_frame_due(monster.position, monster.last_move_position, ticks,
                                monster.last_move_tick)) {
    announce_npc_move(id, monster, true, ticks, packets);
  }

  if (update.reset_health) {
    const int previous = monster_hp(id);
    const int maximum = stat_value(monster.stats, kMaxHealth);
    monster.stats[std::to_string(kHealth)] = maximum;
    if (previous != maximum) {
      packets.push_back(
          {47,
           {{"unit_id", std::to_string(id)},
            {"tick", wire_monotonic_tick(ticks)},
            {"stat_changes",
             {{{"stat_type", kHealth}, {"delta", maximum - previous}, {"result", maximum}}}}}});
    }
    packets.push_back({32,
                       {{"unit_id", std::to_string(id)},
                        {"state", "EX5UnitStateType_Idle"},
                        {"tick", wire_monotonic_tick(ticks)}}});
  }

  // The attack loop in advance() runs for any monster whose aggro flag is set; for an AI monster
  // it additionally waits until the pursuit is over and the player is within engagement range.
  monster.aggro = monster.ai->state() == NpcAiState::Engaged;
}

bool CombatSession::owns_encounter_map(int map_id) const {
  const auto world_lock = lock_world_mobs();
  if (world_mobs_) {
    return world_map_id() == map_id;
  }
  if (tutorial_data_) {
    return map_id == tutorial_data_->map_id();
  }
  const auto active_map = private_encounter_map_ ? private_encounter_map_ : player_map_;
  return active_map && map_id == *active_map;
}

bool CombatSession::private_encounter_active() const {
  const auto world_lock = lock_world_mobs();
  return world_mobs_ || private_encounter_map_.has_value();
}

std::optional<CombatSession::ProjectileWork> CombatSession::earliest_projectile_work() const {
  const auto world_lock = lock_world_mobs();
  const auto earliest = std::ranges::min_element(projectiles_, {}, &Projectile::deadline);
  if (earliest == projectiles_.end()) {
    return std::nullopt;
  }
  if (earliest->work_life_id == 0) {
    throw std::logic_error("Projectile work has no life identity");
  }
  return ProjectileWork{earliest->work_life_id, earliest->caster, earliest->instance,
                        earliest->deadline};
}

bool CombatSession::player_in_combat() const {
  const auto world_lock = lock_world_mobs();
  if (player_dead()) {
    return false;
  }
  return std::ranges::any_of(monsters(), [&](const auto &entry) {
    const auto &[unit_id, monster] = entry;
    const bool owns_simulation = monster.simulation_owner == player_id_;
    const bool participated = engaged_world_mobs_.contains(unit_id) &&
                              visible_world_mobs_.contains(unit_id);
    return monster.active && (monster.aggro || monster.damaged) && monster_hp(unit_id) > 0 &&
           (monster.private_owner == 0 || monster.private_owner == player_id_) &&
           (!world_mobs_ || owns_simulation || participated);
  });
}

std::vector<GameplayPacket> CombatSession::advance(std::int64_t ticks,
                                                   const WorldPosition &position,
                                                   TutorialSession &tutorial) {
  const auto world_lock = lock_world_mobs();
  return advance_encounter(ticks, position, &tutorial);
}

std::vector<GameplayPacket>
CombatSession::advance_private_encounter(std::int64_t ticks, const WorldPosition &position) {
  static_assert(std::is_nothrow_move_constructible_v<GameplayPacket>);
  const auto world_lock = lock_world_mobs();
  if (world_mobs_) {
    auto packets = refresh_world_mobs(position, ticks);
    std::erase_if(engaged_world_mobs_, [&](std::uint64_t unit_id) {
      const auto monster = monsters().find(unit_id);
      return monster == monsters().end() || !visible_world_mobs_.contains(unit_id) ||
             !monster->second.active || stat_value(monster->second.stats, kHealth) <= 0 ||
             (!monster->second.aggro && !monster->second.damaged);
    });
    auto resolved = advance_encounter(ticks, position, nullptr);
    if (resolved.size() > packets.max_size() - packets.size()) {
      throw std::length_error("World combat result exceeds packet capacity");
    }
    // Allocate the owner's combined batch before publishing to the shared journal.
    // Once publication succeeds, moving these packets into the reserved vector cannot throw.
    packets.reserve(packets.size() + resolved.size());
    publish_world_packets(resolved);
    packets.insert(packets.end(), std::make_move_iterator(resolved.begin()),
                   std::make_move_iterator(resolved.end()));
    return packets;
  }
  if (!private_encounter_map_ && !player_map_) {
    return advance_player_vitals(ticks);
  }
  return advance_encounter(ticks, position, nullptr);
}

std::optional<CombatSession::WorldMilestoneAdvance>
CombatSession::advance_next_world_milestone(const combat::CombatTimeline::Token &token,
                                             std::int64_t now_tick,
                                             const WorldPosition &position) {
  static_assert(std::is_nothrow_move_assignable_v<std::vector<GameplayPacket>>);
  const auto world_lock = lock_world_mobs();
  if (!world_mobs_ || position.map_id != world_map_id() || !world_milestone_owner_ ||
      token.owner != *world_milestone_owner_) {
    return std::nullopt;
  }
  const auto timeline = world_combat_timeline();
  if (!timeline || !timeline->is_claimed(token) || executed_world_milestone_ == token ||
      retained_world_milestone_) {
    return std::nullopt;
  }
  const auto scheduled_tick = next_action_tick();
  if (!scheduled_tick || *scheduled_tick != token.tick || *scheduled_tick > now_tick) {
    return std::nullopt;
  }
  auto enclosing_owner = world_milestone_enclosing_owner_;
  if (!enclosing_owner) {
    return std::nullopt;
  }
  auto retained = std::make_shared<RetainedWorldMilestone>();
  retained->token = token;
  retained->result.tick = *scheduled_tick;
  retained->enclosing_owner = std::move(enclosing_owner);
  if (!timeline->begin_milestone(token)) {
    return std::nullopt;
  }

  // Keep the owner before executing; once effects start, replay is forbidden even on failure.
  retained_world_milestone_ = retained;
  executed_world_milestone_ = token;
  try {
    retained->result.packets = advance_private_encounter(*scheduled_tick, position);
    retained->state = WorldMilestoneOutcomeState::packets_retained;
    if (milestone_retention_hook_for_test_) {
      milestone_retention_hook_for_test_();
    }
    retained->result.next_tick = next_action_tick();
    retained->state = WorldMilestoneOutcomeState::ready;
    return retained->result;
  } catch (...) {
    if (retained->state == WorldMilestoneOutcomeState::executing) {
      retained->state = WorldMilestoneOutcomeState::uncertain;
    }
    throw;
  }
}

std::shared_ptr<const CombatSession::RetainedWorldMilestone>
CombatSession::retained_world_milestone(const combat::CombatTimeline::Token &token) const {
  const auto world_lock = lock_world_mobs();
  if (!retained_world_milestone_ || retained_world_milestone_->token != token) {
    return nullptr;
  }
  return retained_world_milestone_;
}

bool CombatSession::finish_retained_world_milestone(const combat::CombatTimeline::Token &token) {
  const auto world_lock = lock_world_mobs();
  const auto timeline = world_combat_timeline();
  if (!timeline || !timeline->is_claimed(token) || !retained_world_milestone_ ||
      retained_world_milestone_->token != token ||
      retained_world_milestone_->state != WorldMilestoneOutcomeState::packets_retained) {
    return false;
  }
  retained_world_milestone_->result.next_tick = next_action_tick();
  retained_world_milestone_->state = WorldMilestoneOutcomeState::ready;
  return true;
}

bool CombatSession::acknowledge_retained_world_milestone(
    const combat::CombatTimeline::Token &token,
    const std::shared_ptr<const RetainedWorldMilestone> &outcome) {
  const auto world_lock = lock_world_mobs();
  const auto timeline = world_combat_timeline();
  if (!timeline || timeline->is_claimed(token) || !retained_world_milestone_ ||
      retained_world_milestone_ != outcome || retained_world_milestone_->token != token ||
      retained_world_milestone_->state != WorldMilestoneOutcomeState::ready) {
    return false;
  }
  retained_world_milestone_.reset();
  return true;
}

void CombatSession::set_milestone_retention_hook_for_test(std::function<void()> hook) {
  const auto world_lock = lock_world_mobs();
  milestone_retention_hook_for_test_ = std::move(hook);
}

void CombatSession::set_action_copy_hook_for_test(std::function<void()> hook) {
  const auto world_lock = lock_world_mobs();
  action_copy_hook_for_test_ = std::move(hook);
}

CombatSession::ProjectileBatchResult CombatSession::advance_simple_npc_projectiles(
    std::int64_t ticks, const WorldPosition &position, std::vector<GameplayPacket> &packets) {
  static_assert(std::is_nothrow_move_constructible_v<GameplayPacket>);
  static_assert(noexcept(std::declval<std::vector<Projectile> &>().swap(
      std::declval<std::vector<Projectile> &>())));
  static_assert(noexcept(std::declval<Json &>().swap(std::declval<Json &>())));

  if (projectile_preflight_hook_for_test_) {
    projectile_preflight_hook_for_test_();
  }
  if (player_shield_ || stat_value(stats_, kShieldStat) != 0) {
    return ProjectileBatchResult::unsupported;
  }

  struct DueFlight {
    std::uint64_t life_id;
    std::int64_t deadline;
  };
  std::vector<DueFlight> due;
  due.reserve(projectiles_.size());
  for (const auto &projectile : projectiles_) {
    if (projectile.npc_interruption) {
      const auto interruption = projectile.npc_interruption->tick;
      const bool retired = retired_projectile_caster(projectile.action, projectile.caster);
      if ((retired && !interruption) || (interruption && *interruption <= projectile.deadline)) {
        return ProjectileBatchResult::unsupported;
      }
    }
    if (projectile.deadline > ticks) {
      continue;
    }
    if (projectile.client_spawned || projectile.caster == player_id_ ||
        projectile.target != player_id_) {
      return ProjectileBatchResult::unsupported;
    }

    const auto &definition = data_->projectiles.at(std::to_string(projectile.cms_id));
    const auto &hit_effects = definition.at("hit_effects");
    if (hit_effects.size() != 1 || definition.contains("conditional_hit") ||
        !definition.value("despawn_effects", Json::array()).empty() ||
        !definition.value("lifetime_despawn_effects", Json::array()).empty()) {
      return ProjectileBatchResult::unsupported;
    }
    const auto &effect = data_->effects.at(std::to_string(hit_effects.at(0).get<int>()));
    if (effect.at("kind") != "damage" || effect.at("chain") != 0 ||
        effect.contains("status_effect_id") ||
        !effect.value("overlap_group", std::string{}).empty() ||
        effect.at("targeting").value("refType", std::string{}) ==
            "EX5ReferenceTargetType_Self" ||
        !combat_rules::abnormal_attacks(effect.at("parameters")).empty()) {
      return ProjectileBatchResult::unsupported;
    }
    if (projectile.work_life_id == 0 ||
        std::ranges::count_if(projectiles_, [&](const Projectile &candidate) {
          return candidate.work_life_id == projectile.work_life_id;
        }) != 1) {
      return ProjectileBatchResult::unsupported;
    }
    due.push_back({projectile.work_life_id, projectile.deadline});
  }
  if (due.empty()) {
    return ProjectileBatchResult::unsupported;
  }

  std::stable_sort(due.begin(), due.end(), [](const DueFlight &left, const DueFlight &right) {
    return left.deadline < right.deadline;
  });

  for (const auto &flight : due) {
    std::optional<Json> previous_stats;
    const auto previous_regeneration = regeneration_tick_;
    try {
      const auto found = std::ranges::find_if(projectiles_, [&](const Projectile &projectile) {
        return projectile.work_life_id == flight.life_id;
      });
      if (found == projectiles_.end()) {
        throw std::logic_error("NPC projectile work identity disappeared");
      }
      const auto &projectile = *found;
      const auto &definition = data_->projectiles.at(std::to_string(projectile.cms_id));
      const auto &effect = data_->effects.at(
          std::to_string(definition.at("hit_effects").at(0).get<int>()));
      const int damage = hit_damage(effect.at("parameters"), projectile.action,
                                    projectile.caster, projectile.target);
      if (stat_value(stats_, kHealth) <= damage) {
        return ProjectileBatchResult::unsupported;
      }
      // Mutate only a private replacement before impact. Projectile moves may throw;
      // neither a failed copy nor compaction can affect the live flight list.
      auto remaining = projectiles_;
      const auto removed = std::erase_if(remaining, [&](const Projectile &candidate) {
        return candidate.work_life_id == flight.life_id;
      });
      if (removed != 1) {
        throw std::logic_error("NPC projectile work identity is ambiguous");
      }
      previous_stats.emplace(stats_);
      std::vector<GameplayPacket> staged;
      resolve_projectile_hit(projectile, position, projectile.deadline, staged);
      if (staged.size() > packets.max_size() - packets.size()) {
        throw std::length_error("Projectile result exceeds its packet capacity");
      }
      packets.reserve(packets.size() + staged.size());
      projectiles_.swap(remaining);
      for (auto &packet : staged) {
        packets.push_back(std::move(packet));
      }
    } catch (...) {
      if (previous_stats) {
        stats_.swap(*previous_stats);
      }
      regeneration_tick_ = previous_regeneration;
      defer_projectile_retry(ticks);
      return ProjectileBatchResult::deferred;
    }
  }
  return ProjectileBatchResult::completed;
}

void CombatSession::set_projectile_preflight_hook_for_test(std::function<void()> hook) {
  projectile_preflight_hook_for_test_ = std::move(hook);
}

void CombatSession::defer_projectile_retry(std::int64_t ticks) noexcept {
  constexpr std::int64_t kProjectileRetryDelay = 100 * kTicksPerMillisecond;
  if (ticks > (std::numeric_limits<std::int64_t>::max)() - kProjectileRetryDelay) {
    projectile_retry_not_before_ = (std::numeric_limits<std::int64_t>::max)();
  } else {
    projectile_retry_not_before_ = ticks + kProjectileRetryDelay;
  }
  try {
    spdlog::error("NPC projectile result could not be staged; retrying the unchanged flight");
  } catch (...) {
    // Logging must not prevent the retained flight from being retried.
  }
}

std::vector<GameplayPacket> CombatSession::advance_encounter(std::int64_t ticks,
                                                             const WorldPosition &position,
                                                             TutorialSession *tutorial) {
  if (!owns_encounter_map(position.map_id)) {
    return {};
  }
  bool hold_due_projectiles = projectile_retry_not_before_ && ticks < *projectile_retry_not_before_;
  if (!hold_due_projectiles) {
    projectile_retry_not_before_.reset();
  }
  std::vector<GameplayPacket> packets;
  if (tutorial) {
    const auto quest = tutorial->progress().quests.find(10000043);
    if (quest != tutorial->progress().quests.end()) {
      for (auto &[id, monster] : monsters()) {
        if (tutorial_data_->npcs.contains(id) && !monster.active && monster.cms_id >= 5001528 &&
            monster.cms_id <= 5001530) {
          const auto &definition = tutorial_data_->npcs.at(id);
          monster.active = true;
          if (quest->second.credited_kills.contains(id)) {
            monster.stats["69"] = 0;
            continue;
          }
          const auto &guid = definition.at("spawner_guid");
          packets.push_back({10,
                             {{"unit_id", std::to_string(id)},
                              {"by_sight", true},
                              {"nub",
                               {{"pos", wire_position(monster.position)},
                                {"yaw", monster.position.yaw},
                                {"npc",
                                 {{"npc_cms_id", monster.cms_id},
                                  {"base_unit_id", std::to_string(id)},
                                  {"stats", monster.stats},
                                  {"state", "EX5UnitStateType_Idle"},
                                  {"faction_type", definition.at("faction")},
                                  {"spawner_id_x", guid.at(0)},
                                  {"spawner_id_y", guid.at(1)},
                                  {"anim_stance_key", definition.at("anim_stance_key")}}}}}}});
        }
      }
    }
  }
  // Accrue recoveries first, so every vital read during this step sees the current value.
  settle_recoveries(ticks);
  advance_action(action_, player_id_, position, position, ticks, packets);
  if (!hold_due_projectiles) {
    try {
      hold_due_projectiles =
          advance_simple_npc_projectiles(ticks, position, packets) == ProjectileBatchResult::deferred;
    } catch (...) {
      // Classification and sorting can allocate, but run before any flight is applied.
      defer_projectile_retry(ticks);
      hold_due_projectiles = true;
    }
  }
  // Extract due flights before applying effects: an effect may spawn another
  // projectile, and must not invalidate the vector currently being traversed.
  std::vector<Projectile> due;
  const auto interrupted_npc_projectile = [&](const Projectile &projectile) {
    if (projectile.npc_interruption) {
      const auto caster = monsters().find(projectile.caster);
      const auto interruption = projectile.npc_interruption->tick;
      const bool retired = retired_projectile_caster(projectile.action, projectile.caster);
      if ((retired && !interruption) || (interruption && *interruption <= projectile.deadline)) {
        const auto &despawn_position = retired ? projectile.launch : caster->second.position;
        packets.push_back({kDespawnProjectileNotification,
                           {{"caster_unit_id", std::to_string(projectile.caster)},
                            {"despawn_projectile",
                             {{"instance_id", std::to_string(projectile.instance)},
                              {"despawn_tick", wire_monotonic_tick(ticks)},
                              {"reason_type", "EX5SkillObjectDespawnReasonType_SkillStageCancel"},
                              {"pos", wire_position(despawn_position)}}}}});
        return true;
      }
    }
    return false;
  };
  std::erase_if(projectiles_, [&](const auto &projectile) {
    if (interrupted_npc_projectile(projectile)) {
      return true;
    }
    if (!hold_due_projectiles && projectile.deadline <= ticks) {
      due.push_back(projectile);
      return true;
    }
    return false;
  });
  if (projectiles_.empty()) {
    projectile_retry_not_before_.reset();
  }
  // Preserve launch order for simultaneous impacts: no capture establishes a different
  // priority. Earlier impacts must be applied first even when a late poll finds both due.
  std::stable_sort(due.begin(), due.end(), [](const Projectile &left, const Projectile &right) {
    return left.deadline < right.deadline;
  });
  for (const auto &projectile : due) {
    // An earlier due flight can itself stun the caster of a later due flight.
    if (!interrupted_npc_projectile(projectile)) {
      resolve_projectile_hit(projectile, position, projectile.deadline, packets);
    }
  }
  advance_areas(ticks, position, packets);
  for (auto &[id, monster] : monsters()) {
    if (monster.private_owner != 0 && monster.private_owner != player_id_) {
      continue;
    }
    // The kill credit was recorded when the monster died (defeated_), so removing the unit later
    // cannot affect quest progress. World respawns allocate a new life instead of reusing this ID.
    if (monster.corpse_deadline && ticks >= *monster.corpse_deadline) {
      packets.push_back({11, {{"unit_ids", Json::array({std::to_string(id)})}}});
      monster.corpse_deadline.reset();
    }
    if (world_mobs_ && monster.simulation_owner != 0 &&
        monster.simulation_owner != player_id_) {
      continue;
    }
    if (monster.gm_ai_paused) {
      continue;
    }
    if (monster_stunned(monster, ticks)) {
      continue;
    }
    if (world_mobs_ && monster.awaiting_target_transfer) {
      const double transfer_range = monster.ai ? kMonsterTargetTransferRangeMm
                                               : kNpcEngagementRangeMm;
      if (stat_value(stats_, kHealth) <= 0 || position.map_id != monster.position.map_id ||
          distance_squared(position, monster.position) > transfer_range * transfer_range ||
          (monster.ai && !monster.ai->can_retarget(position))) {
        continue;
      }
      monster.awaiting_target_transfer = false;
    }
    if (monster.ai) {
      advance_npc_ai(id, monster, position, ticks, packets);
      if (world_mobs_) {
        monster.simulation_owner = monster.aggro ? player_id_ : 0;
      }
    } else if (world_mobs_ && monster.aggro && monster.simulation_owner == 0 &&
               monster.active && monster_hp(id) > 0 && stat_value(stats_, kHealth) > 0 &&
               distance_squared(position, monster.position) <=
                   kNpcEngagementRangeMm * kNpcEngagementRangeMm) {
      monster.simulation_owner = player_id_;
      packets.push_back({98, {{"unit_id", std::to_string(id)},
                              {"new_target_unit_id", std::to_string(player_id_)}}});
    }
    if (world_mobs_ && monster.aggro && monster.simulation_owner != player_id_) {
      continue;
    }
    if (!monster.active || monster_hp(id) <= 0 || !monster.aggro ||
        stat_value(stats_, kHealth) <= 0) {
      continue;
    }
    sync_npc_lunge_pose(monster, ticks);
    // A delayed lunge window opens during the action; start its motion before this poll's
    // events so a hit after the window start is judged from the moving pose.
    begin_npc_lunge(id, monster, position, ticks, packets);
    advance_action(monster.action, id, monster.position, position, ticks, packets);
    if (monster.action || ticks < monster.next_attack) {
      continue;
    }
    const auto cms = std::to_string(monster.cms_id);
    Json candidates = data_->npc_skills.value(cms, Json::array());
    if (monster.ai) {
      const auto selected = select_npc_skill(
          *data_, monster.cms_id, monster.skill_ready_ticks,
          std::sqrt(distance_squared(position, monster.position)), ticks,
          [&](const NpcAttackOption &attack) {
            const double range =
                npc_cast_range_mm(attack, monster.ai->definition()) + kNpcPursuitSettleToleranceMm;
            return !monster.ai->moving() &&
                   distance_squared(position, monster.position) <= range * range;
          },
          monster.skill_last_used_ticks);
      if (!selected) {
        continue;
      }
      candidates = Json::array({*selected});
      const auto cooldown_ms = data_->skills.at(*selected).at("cooldown_ms").get<int>();
      monster.skill_ready_ticks[*selected] = ticks + cooldown_ms * kTicksPerMillisecond;
      monster.skill_last_used_ticks[*selected] = ticks;
      const int group = data_->skills.at(*selected).value("cooldown_group", 0);
      if (group > 0) {
        for (const auto &id : data_->npc_skills.value(cms, Json::array())) {
          const auto skill = data_->skills.find(id.get<int>());
          if (skill != data_->skills.end() && skill->second.value("cooldown_group", 0) == group) {
            monster.skill_ready_ticks[id.get<int>()] = ticks + cooldown_ms * kTicksPerMillisecond;
          }
        }
      }
    }
    // The action's authored duration and per-skill cooldown own pacing. A universal three
    // second delay prevented the captured short combinations from ever happening.
    monster.next_attack = monster.ai ? ticks : ticks + 3 * kTicksPerSecond;
    if (!monster.ai && distance_squared(position, monster.position) >
                           kNpcEngagementRangeMm * kNpcEngagementRangeMm) {
      continue;
    }
    // Legacy (non-AI) monsters keep "first listed executable skill"; the only difference from
    // before is that skills whose catalog type is not Normal (Dodge, Guard, ...) are skipped:
    // they are movement or defence actions, never ordinary attacks (the AI path above has
    // already picked one eligible skill, which passes the same test).
    for (const auto &skill_id : candidates) {
      const auto skill = data_->skills.find(skill_id.get<int>());
      if (skill == data_->skills.end() || !is_executable_npc_attack(*data_, skill->second)) {
        continue;
      }
      const auto &layer = skill->second.at("layers").at(0);
      const auto instance = next_npc_instance_++;
      const auto yaw = static_cast<float>(
          std::atan2(static_cast<double>(position.y_mm) - monster.position.y_mm,
                     static_cast<double>(position.x_mm) - monster.position.x_mm) *
          180 / std::numbers::pi);
      monster.action =
          Action{skill_id.get<int>(), instance, layer.at("index"), 0, ticks, player_id_, yaw};
      spdlog::info("Combat npc_start_skill unit_id={} npc_cms_id={} skill_id={} instance_id={}", id,
                   monster.cms_id, skill_id.get<int>(), instance);
      // The client moves a non-local unit only from 177 frames, so a monster that was walking
      // is stopped where it stands before its own action begins.
      if (monster.move_announced) {
        announce_npc_move(id, monster, false, ticks, packets);
      }
      packets.push_back({14,
                         {{"unit_id", std::to_string(id)},
                          {"weapon_state_type", "EX5WeaponStateType_Drawn"},
                          {"start_skill",
                           {{"skill_id", skill_id},
                            {"instance_id", instance},
                            {"tick", wire_monotonic_tick(ticks)},
                            {"pos", wire_position(monster.position)},
                            {"rotation", {{"yaw", yaw}}}}}}});
      begin_npc_lunge(id, monster, position, ticks, packets);
      break;
    }
  }
  append(packets, advance_player_vitals(ticks));
  // If persistence fails, the world connection closes before any death or quest
  // success packet is queued. Retries cannot turn a client report into kill credit.
  for (const auto id : defeated_) {
    if (tutorial && tutorial_data_->npcs.contains(id)) {
      append(packets, tutorial->record_monster_defeat(id, position, ticks));
    }
  }
  defeated_.clear();
  release_finished_overlap_hits();
  return packets;
}

// Called at the end of advance(): the only places that add entries (action events, projectile hit
// effects, chains) run inside advance(), so the map is bounded by the live casts plus in-flight
// projectiles.
void CombatSession::release_finished_overlap_hits() {
  std::erase_if(overlap_hits_, [this](const auto &entry) {
    const auto [caster, instance, area, pulse] = entry.first;
    if (area != 0) {
      return true;
    }
    const std::optional<Action> *action = &action_;
    if (caster != player_id_) {
      const auto monster = monsters().find(caster);
      if (monster == monsters().end()) {
        return true;
      }
      action = &monster->second.action;
    }
    if (*action && (*action)->instance == instance) {
      return false;
    }
    return !std::ranges::any_of(projectiles_, [&](const Projectile &projectile) {
      return projectile.caster == caster && projectile.action.instance == instance;
    });
  });
}

std::vector<GameplayPacket> CombatSession::advance_player_vitals(std::int64_t ticks) {
  const auto world_lock = lock_world_mobs();
  std::vector<GameplayPacket> packets;
  if (!tutorial_data_) {
    settle_recoveries(ticks);
  }
  if (regeneration_tick_ && ticks >= *regeneration_tick_) {
    if (stat_value(stats_, kHealth) > 0) {
      const auto seconds =
          std::min<std::int64_t>(60, 1 + (ticks - *regeneration_tick_) / kTicksPerSecond);
      if (!movement_health_recovery_blocked_) {
        change_stat(
            kHealth,
            static_cast<int>(std::min<std::int64_t>(1000000, seconds * stat_value(stats_, 15))),
            ticks, packets);
      }
      if (!movement_stamina_recovery_blocked_ && ticks >= stamina_resume_tick_) {
        change_stat(
            kStamina,
            static_cast<int>(std::min<std::int64_t>(1000000, seconds * stat_value(stats_, 25))),
            ticks, packets);
      }
    }
    regeneration_tick_ = ticks + kTicksPerSecond;
    if (stat_value(stats_, kHealth) <= 0 ||
        (stat_value(stats_, kHealth) == stat_value(stats_, kMaxHealth) &&
         stat_value(stats_, kStamina) == stat_value(stats_, kMaxStamina))) {
      regeneration_tick_.reset();
    }
  }
  expire_statuses(ticks, packets);
  expire_shields(ticks, packets);
  return packets;
}

std::optional<std::int64_t> CombatSession::next_action_tick() const {
  const auto world_lock = lock_world_mobs();
  std::optional<std::int64_t> result;
  if (world_mobs_) {
    result = world_refresh_tick_;
  }
  const auto include = [&](std::int64_t tick) {
    if (!result || tick < *result) {
      result = tick;
    }
  };
  const auto include_action = [&](const std::optional<Action> &action, std::uint64_t caster) {
    if (!action) {
      return;
    }
    const auto &definition = stage(*action);
    const int stage_ms = definition.at("duration_ms").get<int>();
    include(action->started + stage_ms * kTicksPerMillisecond);
    int index = 0;
    for (const auto &event : definition.at("events")) {
      if (!action->executed_events.contains(index)) {
        // An area waiting for the client's placement report is due when its wait ends.
        int due_ms = event.at("time_ms").get<int>();
        const bool waiting =
            needs_placement_report(*action, caster, event) &&
            !action->placement_hints.contains(event.at("unique_id").get<int>());
        if (waiting) {
          due_ms = std::min(due_ms + kPlacementHintWaitMs, stage_ms);
        }
        include(action->started + due_ms * kTicksPerMillisecond);
      }
      ++index;
    }
  };
  include_action(action_, player_id_);
  for (const auto &[id, monster] : monsters()) {
    if (monster.private_owner != 0 && monster.private_owner != player_id_) {
      continue;
    }
    if (world_mobs_ && monster.simulation_owner != 0 &&
        monster.simulation_owner != player_id_) {
      continue;
    }
    if (monster.corpse_deadline) {
      include(*monster.corpse_deadline);
    }
    // A GM-paused monster runs no AI or attack, so its stale deadlines must not wake the loop.
    if (monster.gm_ai_paused) {
      continue;
    }
    // Status expiry is scheduled separately below; do not wake on stale action or AI deadlines.
    if (monster_stunned(monster, 0)) {
      continue;
    }
    if (monster.ai && monster.ai->next_action_tick()) {
      include(*monster.ai->next_action_tick());
    }
    if (monster.active && monster.aggro && monster_hp(id) > 0 && stat_value(stats_, kHealth) > 0) {
      include_action(monster.action, id);
      // AI monsters are re-evaluated on the AI's own deadlines, so a gated attack timer that is
      // already in the past cannot make the scheduler spin.
      if (!monster.action && !monster.ai) {
        include(monster.next_attack);
      }
    }
  }
  for (const auto &projectile : projectiles_) {
    include(projectile_retry_not_before_
                ? std::max(projectile.deadline, *projectile_retry_not_before_)
                : projectile.deadline);
  }
  for (const auto &recovery : recoveries_) {
    // Accrual is proportional to elapsed time, so only the end must be scheduled.
    include(recovery.end_tick);
  }
  for (const auto &status : statuses_) {
    include(status.expire_tick);
  }
  if (player_shield_) {
    include(player_shield_->expire_tick);
  }
  for (const auto &[id, monster] : monsters()) {
    if (monster.private_owner != 0 && monster.private_owner != player_id_) {
      continue;
    }
    if (monster.shield) {
      include(monster.shield->expire_tick);
    }
  }
  for (const auto &[id, monster] : monsters()) {
    for (const auto &status : monster.statuses) {
      if (monster.private_owner != 0 && monster.private_owner != player_id_) {
        continue;
      }
      include(status.expire_tick);
    }
  }
  for (const auto &area : areas_) {
    if (area.removal_tick) {
      include(*area.removal_tick);
    } else {
      include(std::min(area.next_hit_tick, area.end_tick));
    }
  }
  if (regeneration_tick_) {
    include(*regeneration_tick_);
  }
  return result;
}

} // namespace chronicles
