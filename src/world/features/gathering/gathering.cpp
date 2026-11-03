#include "world/features/gathering/gathering.hpp"

#include "common/clock_ticks.hpp"
#include "persistence/item_wire.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr int kFormatVersion = 2;
constexpr char kBuildId[] = "40367e24241c9481bfc07fbf9ef1919f25fb0eaa";

// Native world ticks are 100 ns units (see NativeTicks in native_world.cpp); the client's
// monotonic wire clock counts nanoseconds.
constexpr std::int64_t kTicksPerMillisecond = 10000;
constexpr std::int64_t kTicksPerSecond = 1000 * kTicksPerMillisecond;
constexpr std::int64_t kWireNanosecondsPerMillisecond = 1000000;
constexpr std::int64_t kWireNanosecondsPerTick = 100;
// The client stamps its request with the server-synchronised clock. Official replies echo it;
// a value further than this from the server clock is replaced by the server's own tick.
constexpr std::int64_t kMaximumClientTickSkewNs = 5000 * kWireNanosecondsPerMillisecond;
// Polling/network allowance for the client's execute request, as for tutorial objects.
constexpr int kExecuteToleranceMs = 100;
// An unfinished action can be replaced after this long, as for tutorial objects.
constexpr std::int64_t kActionTimeoutTicks = 30 * kTicksPerSecond;
// Official starts were 0.4-1.9 m from placements and 0.4-1.4 m from doodads. The server keeps
// the tutorial interaction allowance rather than inventing a tighter client rule.
constexpr double kGatheringRangeMm = 7000;
constexpr double kRequestPositionToleranceMm = 5000;
constexpr unsigned kMaximumRequestsPerSecond = 32;
constexpr std::size_t kMaximumSectorNodes = 4096;
constexpr double kAlwaysPercent = 100.0;
constexpr int kMaximumDurabilityLoss = 100000;
constexpr int kMaximumPhaseDurationMs = 24 * 60 * 60 * 1000;

// InteractionCondition 1242 and the P2 capture tie this door to Rescue Plan and its key.
constexpr int kCliffsPrisonDoor = 49023;
constexpr int kCliffsPrisonKey = 120109;
constexpr int kRescuePlanQuest = 20001053;
constexpr int kFindPrisonKeySequence = 1;
constexpr int kCliffsMap = 30104;
constexpr int kCliffsCannon = 10152;
constexpr int kCliffsCannonQuest = 20001054;
constexpr int kCliffsCannonCutScene = 301006;
constexpr int kCliffsBarrierDoodad = 10151;
constexpr std::uint32_t kCliffsCannonSector = 171 | (290 << 14);
constexpr std::uint32_t kCliffsCannonLocalId = 1;
constexpr std::uint32_t kCliffsBarrierSector = 171 | (289 << 14);
constexpr std::uint32_t kCliffsBarrierLocalId = 2;
constexpr int kCliffsElite = 5001510;
constexpr int kCliffsCaptain = 5001104;
constexpr int kCliffsCaptainQuest = 10000146;
constexpr int kCliffsEliteHiddenQuest = 90100021;
constexpr int kCliffsEliteBarricade = 10003005;
constexpr int kCliffsEliteObject = 777;
constexpr int kCliffsEliteObjectCutScene = 1550;
constexpr int kCliffsRift = 10130601;
constexpr std::uint32_t kCliffsEliteSector = 170 | (289 << 14);
constexpr std::uint32_t kCliffsRiftSector = 170 | (290 << 14);
constexpr std::uint32_t kCliffsEliteBarricadeLocalId = 5;
constexpr std::uint32_t kCliffsEliteObjectLocalId = 8;
constexpr std::uint32_t kCliffsRiftLocalId = 2;
constexpr int kCliffsCannonObjectiveFirst = 2;
constexpr int kCliffsCannonObjectiveLast = 5;
constexpr int kCliffsEliteObjective = 6;
constexpr int kCliffsCaptainObjective = 1;

bool cliffs_initially_deactivated_doodad(const GatheringNodeKey &node, int doodad_id) {
  if (node.map_id != kCliffsMap || node.kind != GatheringNodeKind::Doodad) {
    return false;
  }

  // The authored placements start deactivated even though their usable phase is 1.
  // A durable phase-1 boss activation must therefore be included for later visitors.
  return (node.sector_id == kCliffsEliteSector && node.index == kCliffsEliteObjectLocalId &&
          doodad_id == kCliffsEliteObject) ||
         (node.sector_id == kCliffsRiftSector && node.index == kCliffsRiftLocalId &&
          doodad_id == kCliffsRift);
}

bool initially_deactivated_doodad(const GatheringData &data, const GatheringNodeKey &node,
                                  int doodad_id) {
  const auto *point = data.doodad_point(node.map_id, node.sector_id, node.index);
  if (point && point->doodad_id == doodad_id && point->initially_deactivated) {
    return true;
  }
  // Older generated profiles did not carry the placement flag. Preserve the two
  // already evidenced Cliffs activations until those profiles are regenerated.
  return cliffs_initially_deactivated_doodad(node, doodad_id);
}

// Doodad spot unit IDs: sector << 32 | local_id << 12 | 6 (recovered tutorial layout).
constexpr std::uint64_t kDoodadUnitTypeMask = 0xFFF;
constexpr std::uint64_t kDoodadUnitType = 6;
constexpr int kDoodadLocalIdShift = 12;
constexpr std::uint64_t kDoodadLocalIdMask = 0xFFFFF;

// x5.cms.EX5EquipSlotType Tool_1 .. Tool_4.
constexpr int kFirstToolSlot = 2;
constexpr int kLastToolSlot = 5;

constexpr std::uint16_t kCommonError = 2;
constexpr std::uint16_t kStartInteractionAnimResponse = 165;
constexpr std::uint16_t kStartInteractionAnimNotification = 166;
constexpr std::uint16_t kInteractionAnimResult = 167;
constexpr std::uint16_t kItemsUpdateNotification = 169;

constexpr char kSheathedWeaponState[] = "EX5WeaponStateType_Sheathed";

// Recovered x5.ec.EErrorCode names returned for rejected gathering actions.
constexpr char kBadRequest[] = "BadRequest";
constexpr char kTooFar[] = "TooFar";
constexpr char kUnknownGathering[] = "Gathering_InvalidCmsId";
constexpr char kDead[] = "InteractionAnim_Dead";
constexpr char kActionInProgress[] = "InteractionAnim_UsingSkill";
constexpr char kAnimationMismatch[] = "InteractionAnim_Mismatch";
constexpr char kMissingCondition[] = "InteractionAnim_InvalidCondition";
constexpr char kInvalidData[] = "InteractionAnim_InvalidData";
constexpr char kInvalidParameter[] = "InteractionAnim_InvalidAnimParamType";
constexpr char kTargetNotFound[] = "InteractionAnim_NotFoundTarget";
constexpr char kInvalidTargetType[] = "InteractionAnim_InvalidTargetUnitType";
constexpr char kDoodadNotProgressible[] = "InteractionAnim_DoodadNotProgressible";
constexpr char kWrongInteraction[] = "InteractionAnim_NotMatchInteractionGroup";
constexpr char kTargetDepleted[] = "InteractionAnim_TargetStateNotMatch";
constexpr char kTooEarly[] = "InteractionAnim_TimeNotMatch";
constexpr char kGatheringFailed[] = "InteractionAnim_GatheringFail";
constexpr char kNoHandlingLoot[] = "Npc_HasNoHandlingLoot";

struct GatheringError {
  const char *code;
  // Packets that must precede the error, such as the stop of an abandoned SP drain.
  std::vector<GameplayPacket> preceding;
};

void require(bool condition, const char *code) {
  if (!condition) {
    throw GatheringError{code, {}};
  }
}

bool rescue_door_allowed(const WorldProgress &progress, const Character &character) {
  const auto quest = progress.quests.find(kRescuePlanQuest);
  if (quest == progress.quests.end() || quest->second.abandoned || quest->second.complete) {
    return false;
  }

  const auto key_stack = progress.stackables.find(kCliffsPrisonKey);
  const bool has_stack = key_stack != progress.stackables.end() && key_stack->second > 0;
  const bool has_item = std::ranges::any_of(character.items, [](const CharacterItem &item) {
    return item.cms_id == kCliffsPrisonKey;
  });
  const auto found_key = quest->second.objectives.find(kFindPrisonKeySequence);
  const bool key_objective_complete =
      found_key != quest->second.objectives.end() && found_key->second > 0;
  return has_stack || has_item || key_objective_complete;
}

void consume_rescue_key(const Character &character, WorldProgress &progress, ItemTrade &trade) {
  const auto item = std::ranges::find_if(character.items, [](const CharacterItem &entry) {
    return entry.cms_id == kCliffsPrisonKey;
  });
  if (item != character.items.end()) {
    trade.removed_item_ids.push_back(item->id);
    return;
  }

  // Older server versions stored this quest item as a stack. Consume that copy too.
  const auto stack = progress.stackables.find(kCliffsPrisonKey);
  if (stack != progress.stackables.end() && stack->second > 0) {
    --stack->second;
  }
}

bool cliffs_cannon_allowed(const WorldProgress &progress) {
  const auto quest = progress.quests.find(kCliffsCannonQuest);
  if (quest == progress.quests.end() || quest->second.abandoned) {
    return false;
  }
  if (quest->second.complete) {
    return true;
  }
  const auto speak_to_kevin = quest->second.objectives.find(1);
  return speak_to_kevin != quest->second.objectives.end() && speak_to_kevin->second > 0;
}

bool cliffs_objective_reached(const WorldProgress &progress, int quest_id, int objective) {
  const auto quest = progress.quests.find(quest_id);
  if (quest == progress.quests.end() || quest->second.abandoned) {
    return false;
  }
  if (quest->second.complete) {
    return true;
  }
  const auto found = quest->second.objectives.find(objective);
  return found != quest->second.objectives.end() && found->second > 0;
}

const Json &cliffs_elite_object_conditions() {
  static const Json conditions = Json::array({
      {{"group", 1}, {"operator", "AND"}, {"kind", "Quest"},
       {"quest_id", kCliffsEliteHiddenQuest}, {"requirement", "Acceptable"}},
      {{"group", 1}, {"operator", "AND"}, {"kind", "Quest"},
       {"quest_id", kCliffsCannonQuest}, {"requirement", "Progressing"}, {"progress_group", 4}},
      {{"group", 2}, {"operator", "AND"}, {"kind", "Quest"},
       {"quest_id", kCliffsEliteHiddenQuest}, {"requirement", "Acceptable"}},
      {{"group", 2}, {"operator", "AND"}, {"kind", "Quest"},
       {"quest_id", kCliffsCannonQuest}, {"requirement", "QuestComplete"}},
  });
  return conditions;
}

std::uint64_t sector_key(std::uint32_t sector_id, std::uint32_t index) {
  return (static_cast<std::uint64_t>(sector_id) << 32) | index;
}

int checked_int(const Json &value, const char *field) {
  if (!value.is_number_integer()) {
    throw std::runtime_error(std::string("Gathering profile field is not an integer: ") + field);
  }
  const auto number = value.get<std::int64_t>();
  if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max()) {
    throw std::runtime_error(std::string("Gathering profile field is out of range: ") + field);
  }
  return static_cast<int>(number);
}

int checked_bounded_int(const Json &value, const char *field, int minimum, int maximum) {
  const int number = checked_int(value, field);
  if (number < minimum || number > maximum) {
    throw std::runtime_error(std::string("Gathering profile field is out of range: ") + field);
  }
  return number;
}

std::uint32_t checked_uint32(const Json &value, const char *field) {
  if (!value.is_number_integer()) {
    throw std::runtime_error(std::string("Gathering profile field is not an integer: ") + field);
  }
  const auto number = value.get<std::int64_t>();
  if (number < 0 || number > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error(std::string("Gathering profile field is out of range: ") + field);
  }
  return static_cast<std::uint32_t>(number);
}

double checked_percent(const Json &value, const char *field) {
  if (!value.is_number()) {
    throw std::runtime_error(std::string("Gathering profile rate is not a number: ") + field);
  }
  const auto rate = value.get<double>();
  if (!std::isfinite(rate) || rate <= 0 || rate > kAlwaysPercent) {
    throw std::runtime_error(std::string("Gathering profile rate is out of range: ") + field);
  }
  return rate;
}

GatheringAnimation parse_animation(const Json &entry) {
  GatheringAnimation animation;
  animation.id = checked_int(entry.at("interaction_anim_id"), "interaction_anim_id");
  animation.sp_cost = checked_int(entry.at("sp_cost"), "sp_cost");
  animation.tool_equipment_type = entry.at("tool_equipment_type").get<std::string>();
  animation.execute_time_ms = checked_int(entry.at("execute_time_ms"), "execute_time_ms");
  animation.duration_ms = checked_int(entry.at("duration_ms"), "duration_ms");

  if (animation.id <= 0 || animation.sp_cost < 0 || animation.execute_time_ms <= 0 ||
      animation.execute_time_ms >= animation.duration_ms) {
    throw std::runtime_error("Invalid gathering animation " + std::to_string(animation.id));
  }
  return animation;
}

GatheringLootGroup parse_loot_group(const Json &entry) {
  GatheringLootGroup group;
  group.rate_percent = checked_percent(entry.at("rate_percent"), "rate_percent");

  for (const auto &item_entry : entry.at("items")) {
    GatheringLootItem item;
    item.item_cms_id = checked_int(item_entry.at("item_cms_id"), "item_cms_id");
    item.min_quantity = checked_int(item_entry.at("min_quantity"), "min_quantity");
    item.max_quantity = checked_int(item_entry.at("max_quantity"), "max_quantity");
    item.weight = checked_int(item_entry.at("weight"), "weight");
    if (item.item_cms_id <= 0 || item.min_quantity <= 0 || item.min_quantity > item.max_quantity ||
        item.weight <= 0) {
      throw std::runtime_error("Invalid gathering loot item " + std::to_string(item.item_cms_id));
    }
    group.items.push_back(item);
  }

  if (group.items.empty()) {
    throw std::runtime_error("Gathering loot group has no items");
  }
  return group;
}

GatheringLoot parse_loot(const Json &entry) {
  GatheringLoot loot;
  loot.pack_rate_percent = checked_percent(entry.at("pack_rate_percent"), "pack_rate_percent");
  for (const auto &group : entry.at("groups")) {
    loot.groups.push_back(parse_loot_group(group));
  }
  if (loot.groups.empty()) {
    throw std::runtime_error("Gathering loot has no groups");
  }
  return loot;
}

GatheringDefinition parse_definition(const Json &entry) {
  GatheringDefinition definition;
  definition.id = checked_int(entry.at("gathering_id"), "gathering_id");
  definition.animation_id = checked_int(entry.at("interaction_anim_id"), "interaction_anim_id");
  definition.deactivate_delay_ms =
      checked_int(entry.at("deactivate_delay_ms"), "deactivate_delay_ms");
  definition.activate_delay_min_ms =
      checked_int(entry.at("activate_delay_min_ms"), "activate_delay_min_ms");
  definition.activate_delay_max_ms =
      checked_int(entry.at("activate_delay_max_ms"), "activate_delay_max_ms");
  definition.loot = parse_loot(entry);

  if (definition.id <= 0 || definition.deactivate_delay_ms < 0 ||
      definition.activate_delay_min_ms <= 0 ||
      definition.activate_delay_min_ms > definition.activate_delay_max_ms) {
    throw std::runtime_error("Invalid gathering definition " + std::to_string(definition.id));
  }
  return definition;
}

DoodadRewardReason parse_reason(const Json &value) {
  const auto text = value.get<std::string>();
  if (text == "looting") {
    return DoodadRewardReason::Looting;
  }
  if (text == "system") {
    return DoodadRewardReason::System;
  }
  throw std::runtime_error("Unsupported doodad reward reason " + text);
}

DoodadPhase parse_doodad_phase(const Json &entry) {
  DoodadPhase phase;
  phase.phase_id = checked_bounded_int(entry.at("phase_id"), "phase_id", 1,
                                       std::numeric_limits<int>::max());

  for (const auto &choice : entry.at("next")) {
    if (!choice.is_array() || choice.size() != 2) {
      throw std::runtime_error("Doodad phase choice must have two fields");
    }
    phase.next.push_back({checked_bounded_int(choice.at(0), "next", 0,
                                              std::numeric_limits<int>::max()),
                          checked_bounded_int(choice.at(1), "weight", 1, 1000000)});
  }
  if (phase.next.empty()) {
    throw std::runtime_error("Doodad phase has no next phase");
  }

  const auto trigger = entry.at("trigger").get<std::string>();
  if (trigger == "timer") {
    phase.duration_ms =
        checked_bounded_int(entry.at("duration_ms"), "duration_ms", 1, kMaximumPhaseDurationMs);
    return phase;
  }
  if (trigger != "interaction") {
    throw std::runtime_error("Unsupported doodad phase trigger " + trigger);
  }

  phase.interaction_id = checked_bounded_int(entry.at("interaction_id"), "interaction_id", 1,
                                             std::numeric_limits<int>::max());
  // An instant interaction (no animation) is used with PC2S_InteractUnitReq.
  phase.instant = entry.value("instant", false);
  if (!phase.instant) {
    phase.animation_id = checked_int(entry.at("interaction_anim_id"), "interaction_anim_id");
  }
  if (const auto destination = entry.find("map_move"); destination != entry.end()) {
    WorldPosition travel;
    travel.map_id = checked_bounded_int(destination->at("map_id"), "map_id", 1,
                                        std::numeric_limits<int>::max());
    travel.x_mm = checked_int(destination->at("x_mm"), "x_mm");
    travel.y_mm = checked_int(destination->at("y_mm"), "y_mm");
    travel.z_mm = checked_int(destination->at("z_mm"), "z_mm");
    phase.map_move = travel;
  }
  if (entry.contains("skill_effect_id")) {
    const int effect_id = checked_int(entry.at("skill_effect_id"), "skill_effect_id");
    if (effect_id != 11150 && effect_id != 11151 && effect_id != 11152) {
      throw std::runtime_error("Unsupported gathering skill effect");
    }
    phase.skill_effect_id = effect_id;
  }
  // Quest doodad interactions have no harvesting reward: an empty loot rolls nothing.
  if (!entry.contains("reward")) {
    return phase;
  }
  const auto &reward = entry.at("reward");
  phase.loot = parse_loot(reward);
  phase.reason = parse_reason(reward.at("reason"));
  return phase;
}

DoodadDefinition parse_doodad(const Json &entry) {
  DoodadDefinition definition;
  definition.id = checked_bounded_int(entry.at("doodad_id"), "doodad_id", 1,
                                      std::numeric_limits<int>::max());
  definition.initial_phase = checked_int(entry.at("initial_phase"), "initial_phase");
  definition.quest = entry.value("quest", false);
  for (const auto &phase : entry.at("phases")) {
    definition.phases.push_back(parse_doodad_phase(phase));
  }

  bool despawns = false;
  for (const auto &phase : definition.phases) {
    const auto duplicates = std::ranges::count_if(definition.phases, [&](const auto &other) {
      return other.phase_id == phase.phase_id;
    });
    if (duplicates != 1) {
      throw std::runtime_error("Duplicate phase in doodad " + std::to_string(definition.id));
    }
    for (const auto &choice : phase.next) {
      if (choice.phase_id == kDespawnedDoodadPhase) {
        despawns = true;
      } else if (definition.phase(choice.phase_id) == nullptr) {
        throw std::runtime_error("Unknown next phase in doodad " + std::to_string(definition.id));
      }
    }
  }
  if (definition.phase(definition.initial_phase) == nullptr) {
    throw std::runtime_error("Unknown initial phase in doodad " + std::to_string(definition.id));
  }

  if (despawns) {
    definition.respawn_delay_min_ms = checked_bounded_int(
        entry.at("respawn_delay_min_ms"), "respawn_delay_min_ms", 1, kMaximumPhaseDurationMs);
    definition.respawn_delay_max_ms = checked_bounded_int(
        entry.at("respawn_delay_max_ms"), "respawn_delay_max_ms",
        definition.respawn_delay_min_ms, kMaximumPhaseDurationMs);
  }
  return definition;
}

GatheringMapArea parse_map(const Json &entry) {
  GatheringMapArea area;
  area.map_id = checked_int(entry.at("map_id"), "map_id");
  area.world = entry.contains("world") ? checked_bounded_int(entry.at("world"), "world", 0, 255)
                                       : 0;
  for (std::size_t axis = 0; axis < area.min_mm.size(); ++axis) {
    area.min_mm[axis] = checked_int(entry.at("area_min_mm").at(axis), "area_min_mm");
    area.max_mm[axis] = checked_int(entry.at("area_max_mm").at(axis), "area_max_mm");
    if (area.min_mm[axis] > area.max_mm[axis]) {
      throw std::runtime_error("Inverted gathering map area " + std::to_string(area.map_id));
    }
  }
  return area;
}

void require_six_fields(const Json &entry, const char *kind) {
  constexpr std::size_t kPointFieldCount = 6;
  if (!entry.is_array() || entry.size() != kPointFieldCount) {
    throw std::runtime_error(std::string(kind) + " must have six fields");
  }
}

GatheringPoint parse_point(const Json &entry) {
  require_six_fields(entry, "Gathering point");

  GatheringPoint point;
  point.sector_id = checked_uint32(entry.at(0), "sector_id");
  point.index = checked_uint32(entry.at(1), "index");
  point.gathering_id = checked_int(entry.at(2), "gathering_id");
  point.x_mm = checked_int(entry.at(3), "x_mm");
  point.y_mm = checked_int(entry.at(4), "y_mm");
  point.z_mm = checked_int(entry.at(5), "z_mm");
  return point;
}

// Optional fields carry the world index and authored initial deactivation.
DoodadPoint parse_doodad_point(const Json &entry) {
  constexpr std::size_t kWorldField = 6;
  constexpr std::size_t kDeactivationField = 7;
  if (!entry.is_array() || entry.size() < kWorldField || entry.size() > kDeactivationField + 1) {
    throw std::runtime_error("Doodad point must have six, seven or eight fields");
  }

  DoodadPoint point;
  point.sector_id = checked_uint32(entry.at(0), "sector_id");
  point.local_id = checked_uint32(entry.at(1), "local_id");
  if (point.local_id == 0 || point.local_id > kDoodadLocalIdMask) {
    throw std::runtime_error("Doodad local ID is outside the recovered unit-ID layout");
  }
  point.doodad_id = checked_int(entry.at(2), "doodad_id");
  point.x_mm = checked_int(entry.at(3), "x_mm");
  point.y_mm = checked_int(entry.at(4), "y_mm");
  point.z_mm = checked_int(entry.at(5), "z_mm");
  if (entry.size() > kWorldField) {
    point.world = checked_bounded_int(entry.at(kWorldField), "world", 0, 255);
  }
  if (entry.size() > kDeactivationField) {
    if (!entry.at(kDeactivationField).is_boolean()) {
      throw std::runtime_error("Doodad initial deactivation must be a boolean");
    }
    point.initially_deactivated = entry.at(kDeactivationField).get<bool>();
  }
  return point;
}

double squared_distance_mm(const WorldPosition &position, const std::array<int, 3> &point) {
  const double dx = position.x_mm - point[0];
  const double dy = position.y_mm - point[1];
  const double dz = position.z_mm - point[2];
  return dx * dx + dy * dy + dz * dz;
}

bool within(const WorldPosition &position, const std::array<int, 3> &point, double range_mm) {
  return squared_distance_mm(position, point) <= range_mm * range_mm;
}

bool request_position_matches(const Json &request, const WorldPosition &position) {
  if (!request.contains("pos") || !request.at("pos").is_object()) {
    return false;
  }

  const auto &pos = request.at("pos");
  const std::array<int, 3> requested{pos.value("millimeter_x", 0), pos.value("millimeter_y", 0),
                                     pos.value("millimeter_z", 0)};
  return within(position, requested, kRequestPositionToleranceMm);
}

std::uint64_t parse_unit_id(const Json &value) {
  if (value.is_string()) {
    const auto &text = value.get_ref<const std::string &>();
    std::size_t parsed_characters{};
    const auto unit_id = std::stoull(text, &parsed_characters);
    require(parsed_characters == text.size(), kBadRequest);
    return unit_id;
  }
  require(value.is_number_unsigned(), kBadRequest);
  return value.get<std::uint64_t>();
}

// The client sends sector_id << 32 for gathering placements; the low half is unused.
std::uint32_t parse_target_sector(const Json &value) {
  const auto target = parse_unit_id(value);
  require((target & 0xFFFFFFFFULL) == 0, kTargetNotFound);
  return static_cast<std::uint32_t>(target >> 32);
}

std::uint32_t parse_single_index(const Json &request) {
  const auto &indices = request.at("param").at("gathering").at("indices");
  // Every captured gathering start named exactly one placement.
  require(indices.is_array() && indices.size() == 1, kInvalidParameter);
  require(indices.at(0).is_number_unsigned(), kInvalidParameter);

  const auto index = indices.at(0).get<std::uint64_t>();
  require(index <= std::numeric_limits<std::uint32_t>::max(), kInvalidParameter);
  return static_cast<std::uint32_t>(index);
}

std::optional<int> requested_interaction(const Json &request) {
  const auto param = request.find("param");
  if (param == request.end() || !param->is_object()) {
    return std::nullopt;
  }

  // InteractUnitReq has a direct interaction_id; StartInteractionAnimReq nests it.
  const auto direct_id = param->find("interaction_id");
  if (direct_id != param->end() && direct_id->is_number_integer()) {
    return direct_id->get<int>();
  }

  const auto interaction = param->find("interaction");
  if (interaction == param->end() || !interaction->is_object()) {
    return std::nullopt;
  }
  const auto id = interaction->find("interaction_id");
  if (id == interaction->end() || !id->is_number_integer()) {
    return std::nullopt;
  }
  return id->get<int>();
}

// Official StartInteractionAnimNoti and its SP flow echo the request tick. Absent, malformed
// or implausible ticks are replaced by the server clock in the same nanosecond unit.
std::int64_t request_wire_tick(const Json &request, std::int64_t server_ticks) {
  const std::int64_t server_ns = server_ticks * kWireNanosecondsPerTick;
  const auto tick = request.find("tick");
  if (tick == request.end()) {
    return server_ns;
  }

  std::int64_t client_ns{};
  try {
    if (tick->is_string()) {
      const auto &text = tick->get_ref<const std::string &>();
      std::size_t parsed_characters{};
      client_ns = std::stoll(text, &parsed_characters);
      if (parsed_characters != text.size()) {
        return server_ns;
      }
    } else if (tick->is_number_integer()) {
      client_ns = tick->get<std::int64_t>();
    } else {
      return server_ns;
    }
  } catch (const std::exception &) {
    return server_ns;
  }

  if (client_ns <= 0 || client_ns > server_ns + kMaximumClientTickSkewNs ||
      client_ns < server_ns - kMaximumClientTickSkewNs) {
    return server_ns;
  }
  return client_ns;
}

float request_yaw(const Json &request, const WorldPosition &position) {
  // The client turns toward the node before sending its request yaw.
  const auto yaw = request.find("yaw");
  if (yaw != request.end() && yaw->is_number() && std::isfinite(yaw->get<double>())) {
    return yaw->get<float>();
  }
  return position.yaw;
}

std::int64_t add_milliseconds(std::int64_t ticks, std::int64_t milliseconds) {
  return ticks + milliseconds * kTicksPerMillisecond;
}

} // namespace

const DoodadPhase *DoodadDefinition::phase(int phase_id) const {
  const auto found = std::ranges::find_if(
      phases, [phase_id](const DoodadPhase &phase) { return phase.phase_id == phase_id; });
  return found == phases.end() ? nullptr : &*found;
}

std::uint64_t doodad_unit_id(std::uint32_t sector_id, std::uint32_t local_id) {
  return (static_cast<std::uint64_t>(sector_id) << 32) |
         (static_cast<std::uint64_t>(local_id) << kDoodadLocalIdShift) | kDoodadUnitType;
}

std::optional<GatheringNodeKey> doodad_node_of_unit(int map_id, std::uint64_t unit_id) {
  constexpr std::uint64_t kUnitTypeMask = (std::uint64_t{1} << kDoodadLocalIdShift) - 1;
  constexpr std::uint64_t kLocalIdMask = (std::uint64_t{1} << (32 - kDoodadLocalIdShift)) - 1;
  if ((unit_id & kUnitTypeMask) != kDoodadUnitType) {
    return std::nullopt;
  }
  const auto sector_id = static_cast<std::uint32_t>(unit_id >> 32);
  const auto local_id = static_cast<std::uint32_t>((unit_id >> kDoodadLocalIdShift) & kLocalIdMask);
  return GatheringNodeKey{map_id, GatheringNodeKind::Doodad, sector_id, local_id};
}

std::shared_ptr<const GatheringData> GatheringData::load(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("Cannot open gathering profile " + path.string());
  }

  try {
    return std::make_shared<const GatheringData>(Json::parse(input));
  } catch (const Json::exception &error) {
    throw std::runtime_error("Invalid gathering profile " + path.string() + ": " + error.what());
  }
}

GatheringData::GatheringData(const Json &document) {
  if (document.at("format_version") != kFormatVersion || document.at("build_id") != kBuildId) {
    throw std::runtime_error("Gathering profile belongs to an unsupported format or client "
                             "build; rebuild it with tools/build_gathering_data.py");
  }

  for (const auto &entry : document.at("maps")) {
    maps_.push_back(parse_map(entry));
  }

  tool_durability_loss_ = checked_bounded_int(document.at("tool_durability_loss"),
                                              "tool_durability_loss", 0, kMaximumDurabilityLoss);
  sp_regen_delay_ms_ = checked_bounded_int(document.at("sp_regen_delay_ms"), "sp_regen_delay_ms",
                                           0, kMaximumPhaseDurationMs);

  for (const auto &entry : document.at("animations")) {
    auto animation = parse_animation(entry);
    const auto id = animation.id;
    if (!animations_.emplace(id, std::move(animation)).second) {
      throw std::runtime_error("Duplicate gathering animation " + std::to_string(id));
    }
  }

  for (const auto &entry : document.at("gatherings")) {
    auto definition = parse_definition(entry);
    const auto id = definition.id;
    if (!animations_.contains(definition.animation_id)) {
      throw std::runtime_error("Gathering definition has no animation " + std::to_string(id));
    }
    if (!definitions_.emplace(id, std::move(definition)).second) {
      throw std::runtime_error("Duplicate gathering definition " + std::to_string(id));
    }
  }

  for (const auto &entry : document.at("unsupported_gatherings")) {
    const auto id = checked_int(entry.at("gathering_id"), "gathering_id");
    if (definitions_.contains(id) || !unsupported_.insert(id).second) {
      throw std::runtime_error("Conflicting unsupported gathering " + std::to_string(id));
    }
  }

  for (const auto &[equipment_type, items] : document.at("tool_items").items()) {
    auto &known_items = tool_items_[equipment_type];
    for (const auto &item : items) {
      known_items.insert(checked_int(item, "tool_items"));
    }
  }

  for (const auto &entry : document.at("points")) {
    const auto point = parse_point(entry);
    if (!definitions_.contains(point.gathering_id) && !unsupported_.contains(point.gathering_id)) {
      throw std::runtime_error("Gathering point references unknown ID " +
                               std::to_string(point.gathering_id));
    }
    if (!points_.emplace(sector_key(point.sector_id, point.index), point).second) {
      throw std::runtime_error("Duplicate gathering point in sector " +
                               std::to_string(point.sector_id));
    }
  }

  for (const auto &entry : document.at("doodads")) {
    auto definition = parse_doodad(entry);
    for (const auto &phase : definition.phases) {
      if (phase.interaction_id) {
        if (!phase.instant && !animations_.contains(phase.animation_id)) {
          throw std::runtime_error("Doodad phase has no animation " +
                                   std::to_string(definition.id));
        }
        if (definition.quest) {
          quest_doodad_interactions_.insert(*phase.interaction_id);
        } else {
          doodad_interactions_.insert(*phase.interaction_id);
        }
      }
    }
    const auto id = definition.id;
    if (!doodads_.emplace(id, std::move(definition)).second) {
      throw std::runtime_error("Duplicate gathering doodad " + std::to_string(id));
    }
  }

  for (const auto &entry : document.at("doodad_points")) {
    const auto point = parse_doodad_point(entry);
    if (!doodads_.contains(point.doodad_id)) {
      throw std::runtime_error("Doodad point references unknown doodad " +
                               std::to_string(point.doodad_id));
    }
    if (static_cast<std::size_t>(point.world) >= doodad_points_.size()) {
      doodad_points_.resize(static_cast<std::size_t>(point.world) + 1);
    }
    auto &world_points = doodad_points_.at(static_cast<std::size_t>(point.world));
    if (!world_points.emplace(sector_key(point.sector_id, point.local_id), point).second) {
      throw std::runtime_error("Duplicate doodad point in sector " +
                               std::to_string(point.sector_id));
    }
  }

  // Older profiles have no butchering section; they simply offer no corpse butchering.
  if (const auto handling = document.find("handling"); handling != document.end()) {
    parse_handling(*handling);
  }

  if (maps_.empty() || points_.empty() || definitions_.empty()) {
    throw std::runtime_error("Gathering profile has no usable maps, points or definitions");
  }
}

void GatheringData::parse_handling(const Json &handling) {
  const auto &rewards = handling.at("rewards");
  if (rewards.empty()) {
    return;
  }

  handling_.animation_id = checked_int(handling.at("interaction_anim_id"), "interaction_anim_id");
  const auto animation = animations_.find(handling_.animation_id);
  if (animation == animations_.end() || animation->second.tool_equipment_type == "None") {
    throw std::runtime_error("Butchering needs a known tool animation");
  }

  for (const auto &entry : rewards) {
    const auto npc_cms_id = checked_bounded_int(entry.at("npc_cms_id"), "npc_cms_id", 1,
                                                std::numeric_limits<int>::max());
    if (!handling_.rewards.emplace(npc_cms_id, parse_loot(entry)).second) {
      throw std::runtime_error("Duplicate butchering reward for NPC " +
                               std::to_string(npc_cms_id));
    }
  }
}

bool GatheringData::has_map(int map_id) const {
  return std::ranges::any_of(maps_, [map_id](const auto &area) { return area.map_id == map_id; });
}

bool GatheringData::map_contains(int map_id, const std::array<int, 3> &position_mm) const {
  return std::ranges::any_of(maps_, [&](const auto &area) {
    if (area.map_id != map_id) {
      return false;
    }
    for (std::size_t axis = 0; axis < position_mm.size(); ++axis) {
      if (position_mm[axis] < area.min_mm[axis] || position_mm[axis] > area.max_mm[axis]) {
        return false;
      }
    }
    return true;
  });
}

const GatheringPoint *GatheringData::point(std::uint32_t sector_id, std::uint32_t index) const {
  const auto found = points_.find(sector_key(sector_id, index));
  return found == points_.end() ? nullptr : &found->second;
}

const GatheringDefinition *GatheringData::definition(int gathering_id) const {
  const auto found = definitions_.find(gathering_id);
  return found == definitions_.end() ? nullptr : &found->second;
}

bool GatheringData::is_unsupported(int gathering_id) const {
  return unsupported_.contains(gathering_id);
}

const GatheringAnimation *GatheringData::animation(int animation_id) const {
  const auto found = animations_.find(animation_id);
  return found == animations_.end() ? nullptr : &found->second;
}

bool GatheringData::is_tool(int item_cms_id, const std::string &equipment_type) const {
  const auto found = tool_items_.find(equipment_type);
  return found != tool_items_.end() && found->second.contains(item_cms_id);
}

const DoodadPoint *GatheringData::doodad_point(std::uint32_t sector_id,
                                               std::uint32_t local_id) const {
  if (doodad_points_.empty()) {
    return nullptr;
  }
  const auto &main_world = doodad_points_.front();
  const auto found = main_world.find(sector_key(sector_id, local_id));
  return found == main_world.end() ? nullptr : &found->second;
}

const DoodadPoint *GatheringData::doodad_point(int map_id, std::uint32_t sector_id,
                                               std::uint32_t local_id) const {
  const auto map = std::ranges::find_if(maps_, [map_id](const GatheringMapArea &area) {
    return area.map_id == map_id;
  });
  if (map == maps_.end() || static_cast<std::size_t>(map->world) >= doodad_points_.size()) {
    return nullptr;
  }
  const auto &world_points = doodad_points_.at(static_cast<std::size_t>(map->world));
  const auto found = world_points.find(sector_key(sector_id, local_id));
  return found == world_points.end() ? nullptr : &found->second;
}

const DoodadDefinition *GatheringData::doodad(int doodad_id) const {
  const auto found = doodads_.find(doodad_id);
  return found == doodads_.end() ? nullptr : &found->second;
}

namespace {

// Splits a spot doodad wire unit id into its sector and local id; nullopt for other units.
std::optional<std::pair<std::uint32_t, std::uint32_t>> doodad_spot(std::uint64_t unit_id) {
  if ((unit_id & kDoodadUnitTypeMask) != kDoodadUnitType) {
    return std::nullopt;
  }
  const auto sector_id = static_cast<std::uint32_t>(unit_id >> 32);
  const auto local_id =
      static_cast<std::uint32_t>((unit_id >> kDoodadLocalIdShift) & kDoodadLocalIdMask);
  return std::pair{sector_id, local_id};
}

} // namespace

const DoodadPoint *GatheringData::doodad_point_of_unit(int map_id, std::uint64_t unit_id) const {
  const auto spot = doodad_spot(unit_id);
  return spot ? doodad_point(map_id, spot->first, spot->second) : nullptr;
}

bool GatheringData::names_quest_doodad(std::uint64_t unit_id) const {
  const auto spot = doodad_spot(unit_id);
  if (!spot) {
    return false;
  }
  for (const auto &world_points : doodad_points_) {
    const auto found = world_points.find(sector_key(spot->first, spot->second));
    if (found == world_points.end()) {
      continue;
    }
    const auto *definition = doodad(found->second.doodad_id);
    if (definition != nullptr && definition->quest) {
      return true;
    }
  }
  return false;
}

bool GatheringData::is_quest_doodad_interaction(int interaction_id) const {
  return quest_doodad_interactions_.contains(interaction_id);
}

bool GatheringData::is_doodad_interaction(int interaction_id) const {
  return doodad_interactions_.contains(interaction_id);
}

const GatheringLoot *GatheringData::handling_reward(int npc_cms_id) const {
  const auto found = handling_.rewards.find(npc_cms_id);
  return found == handling_.rewards.end() ? nullptr : &found->second;
}

int GatheringData::handling_animation_id() const {
  return handling_.animation_id;
}

int GatheringData::tool_durability_loss() const {
  return tool_durability_loss_;
}

int GatheringData::sp_regen_delay_ms() const {
  return sp_regen_delay_ms_;
}

std::size_t GatheringData::point_count() const {
  return points_.size();
}

std::size_t GatheringData::definition_count() const {
  return definitions_.size();
}

std::size_t GatheringData::doodad_point_count() const {
  std::size_t count = 0;
  for (const auto &world_points : doodad_points_) {
    count += world_points.size();
  }
  return count;
}

GatheringNodes::GatheringNodes(std::shared_ptr<const GatheringData> data,
                               std::uint32_t random_seed)
    : data_(std::move(data)), random_(random_seed) {}

void GatheringNodes::replace_due_indexes_locked(
    std::initializer_list<DueIndexChange> changes) {
  std::set<DueKey> staged_shared;
  std::set<DueKey> staged_local;
  std::set<GatheringNodeKey> staged_nodes;
  for (const auto &change : changes) {
    if (!change.next_tick) {
      continue;
    }
    if (change.next_shared) {
      staged_shared.emplace(*change.next_tick, change.node);
      staged_nodes.insert(change.node);
    } else {
      staged_local.emplace(*change.next_tick, change.node);
    }
  }
  for (const auto &change : changes) {
    if (!change.previous_tick) {
      continue;
    }
    if (change.previously_shared) {
      shared_due_by_time_.erase({*change.previous_tick, change.node});
      shared_due_by_node_.erase(change.node);
    } else {
      local_due_by_time_.erase({*change.previous_tick, change.node});
    }
  }
  shared_due_by_time_.merge(staged_shared);
  local_due_by_time_.merge(staged_local);
  shared_due_by_node_.merge(staged_nodes);
}

std::uint64_t GatheringNodes::replace_state_locked(const GatheringNodeKey &node,
                                          std::optional<NodeState> replacement) {
  prepare_revision_locked(node);
  auto existing = states_.find(node);
  const auto previous_tick = existing == states_.end() ? std::nullopt : existing->second.due_tick;
  const bool shared = shared_sources_.contains(node);
  bool inserted = false;
  if (replacement && existing == states_.end()) {
    auto result = states_.emplace(node, *replacement);
    existing = result.first;
    inserted = result.second;
  }
  try {
    replace_due_indexes_locked({{node, previous_tick, shared,
                                 replacement ? replacement->due_tick : std::nullopt, shared}});
  } catch (...) {
    if (inserted) {
      states_.erase(existing);
    }
    throw;
  }
  if (replacement) {
    existing->second = *replacement;
  } else if (existing != states_.end()) {
    states_.erase(existing);
  }
  return publish_revision_locked(node);
}

void GatheringNodes::prepare_revision_locked(const GatheringNodeKey &node) {
  if (next_revision_ == 0) {
    throw std::overflow_error("Gathering source revisions are exhausted");
  }
  revisions_.try_emplace(node, 0);
}

std::uint64_t GatheringNodes::publish_revision_locked(const GatheringNodeKey &node) noexcept {
  const auto revision = next_revision_++;
  revisions_.find(node)->second = revision;
  return revision;
}

namespace {

bool same_shared_source(const StoredSharedGatheringSource &left,
                        const StoredSharedGatheringSource &right) {
  return left.generation == right.generation && left.active == right.active &&
         left.phase == right.phase &&
         left.next_transition_epoch_ticks == right.next_transition_epoch_ticks &&
         left.timed_successor_phase == right.timed_successor_phase &&
         left.pending_receipt_id == right.pending_receipt_id &&
         left.next_transition_delay_ticks == right.next_transition_delay_ticks;
}

} // namespace

std::optional<GatheringNodes::NodeState> GatheringNodes::prepare_shared_source(
    const GatheringNodeKey &node, const StoredSharedGatheringSource &source,
    std::int64_t now_epoch_ticks, std::int64_t now_monotonic_ticks) const {
  if (source.generation < 0 || now_epoch_ticks < 0 || now_monotonic_ticks < 0 ||
      (source.next_transition_epoch_ticks && *source.next_transition_epoch_ticks < 0) ||
      source.pending_receipt_id || source.next_transition_delay_ticks) {
    return std::nullopt;
  }
  NodeState replacement;
  if (node.kind == GatheringNodeKind::Placement) {
    if (data_) {
      const auto *point = data_->point(node.sector_id, node.index);
      if (!point || !data_->definition(point->gathering_id) ||
          !data_->map_contains(node.map_id, {point->x_mm, point->y_mm, point->z_mm})) {
        throw std::runtime_error("Persisted gathering placement is absent from the profile");
      }
    }
    if (source.phase || source.timed_successor_phase ||
        (!source.active && !source.next_transition_epoch_ticks) ||
        (source.active && source.next_transition_epoch_ticks)) {
      throw std::runtime_error("Invalid persisted gathering placement state");
    }
  } else if (node.kind == GatheringNodeKind::Doodad) {
    const bool cannon_barrier = node.map_id == kCliffsMap &&
                                node.sector_id == kCliffsBarrierSector &&
                                node.index == kCliffsBarrierLocalId;
    const bool elite_barricade = node.map_id == kCliffsMap &&
                                  node.sector_id == kCliffsEliteSector &&
                                  node.index == kCliffsEliteBarricadeLocalId;
    if (!source.phase) {
      if (!cannon_barrier || source.active || source.generation <= 0 ||
          source.next_transition_epoch_ticks || source.timed_successor_phase) {
        throw std::runtime_error("Invalid unknown gathering doodad baseline");
      }
      replacement.doodad_id = kCliffsBarrierDoodad;
      replacement.unknown_baseline = true;
      replacement.begin_tick = now_monotonic_ticks;
      return replacement;
    }
    if (source.active != (*source.phase != kDespawnedDoodadPhase)) {
      throw std::runtime_error("Invalid persisted gathering doodad state");
    }
    if (cannon_barrier) {
      if (*source.phase != kDespawnedDoodadPhase || source.next_transition_epoch_ticks ||
          source.timed_successor_phase) {
        throw std::runtime_error("Invalid persisted Cliffs cannon barrier state");
      }
      replacement.doodad_id = kCliffsBarrierDoodad;
    } else if (elite_barricade) {
      if (*source.phase != 2 || !source.active || source.next_transition_epoch_ticks ||
          source.timed_successor_phase) {
        throw std::runtime_error("Invalid persisted Cliffs elite barricade state");
      }
      replacement.doodad_id = kCliffsEliteBarricade;
    } else {
      const auto *point = data_ ? data_->doodad_point(node.map_id, node.sector_id, node.index)
                                : nullptr;
      const auto *definition = point ? data_->doodad(point->doodad_id) : nullptr;
      if (!definition || (*source.phase != kDespawnedDoodadPhase &&
                          !definition->phase(*source.phase))) {
        throw std::runtime_error("Persisted gathering doodad is absent from the profile");
      }
      replacement.doodad_id = definition->id;
    }
    replacement.phase = *source.phase;
  } else {
    throw std::runtime_error("Invalid persisted gathering source kind");
  }
  replacement.begin_tick = now_monotonic_ticks;
  if (source.next_transition_epoch_ticks) {
    const auto remaining = *source.next_transition_epoch_ticks <= now_epoch_ticks
                               ? 0 : *source.next_transition_epoch_ticks - now_epoch_ticks;
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    replacement.due_tick = now_monotonic_ticks > maximum - remaining
                               ? maximum : now_monotonic_ticks + remaining;
  }
  return replacement;
}

bool GatheringNodes::install_shared_source(
    const GatheringNodeKey &node, const StoredSharedGatheringSource &source,
    std::int64_t now_epoch_ticks, std::int64_t now_monotonic_ticks,
    std::optional<std::uint64_t> owner_token, bool retain_owner, std::uint64_t *revision) {
  const auto replacement = prepare_shared_source(node, source, now_epoch_ticks,
                                                  now_monotonic_ticks);
  if (!replacement) {
    return false;
  }
  std::lock_guard publication_lock(publication_mutex_);
  std::lock_guard lock(mutex_);
  const auto reservation = reservations_.find(node);
  if ((reservation != reservations_.end() &&
       (!owner_token || reservation->second.token != *owner_token)) ||
      (reservation == reservations_.end() && (owner_token || retain_owner))) {
    return false;
  }
  const auto old_source = shared_sources_.find(node);
  if (old_source != shared_sources_.end() &&
      (old_source->second.generation > source.generation ||
       (old_source->second.generation == source.generation &&
        !same_shared_source(old_source->second, source)))) {
    return false;
  }
  const auto prior = states_.find(node);
  if (replacement->unknown_baseline && prior != states_.end() &&
      !prior->second.unknown_baseline &&
      (reservation == reservations_.end() || reservation->second.previous_recorded)) {
    return false;
  }
  // Allocate both rows before publishing either snapshot; failed allocation cannot alter
  // an existing source or release the reservation.
  prepare_revision_locked(node);
  const bool previously_shared = old_source != shared_sources_.end();
  const auto old_state = states_.find(node);
  const auto previous_tick = old_state == states_.end() ? std::nullopt : old_state->second.due_tick;
  auto [stored, source_inserted] = shared_sources_.try_emplace(node, source);
  bool state_inserted = false;
  try {
    auto result = states_.try_emplace(node, *replacement);
    auto state = result.first;
    state_inserted = result.second;
    replace_due_indexes_locked({{node, previous_tick, previously_shared,
                                 replacement->due_tick, true}});
    stored->second = source;
    state->second = *replacement;
  } catch (...) {
    if (state_inserted) {
      states_.erase(node);
    }
    if (source_inserted) {
      shared_sources_.erase(stored);
    }
    throw;
  }
  const auto published_revision = publish_revision_locked(node);
  if (revision) {
    *revision = published_revision;
  }
  if (reservation != reservations_.end()) {
    if (retain_owner) {
      reservation->second.previous = *replacement;
      reservation->second.previous_recorded = true;
    } else {
      reservations_.erase(reservation);
    }
  }
  return true;
}

bool GatheringNodes::install_shared_source_pair(
    const GatheringNodeKey &first, const StoredSharedGatheringSource &first_source,
    std::uint64_t first_token, const GatheringNodeKey &second,
    const StoredSharedGatheringSource &second_source, std::uint64_t second_token,
    std::int64_t now_epoch_ticks, std::int64_t now_monotonic_ticks, bool retain_owners,
    std::uint64_t *first_revision, std::uint64_t *second_revision) {
  if (first == second) {
    throw std::invalid_argument("Linked gathering sources must be distinct");
  }
  const auto first_state = prepare_shared_source(first, first_source, now_epoch_ticks,
                                                 now_monotonic_ticks);
  const auto second_state = prepare_shared_source(second, second_source, now_epoch_ticks,
                                                  now_monotonic_ticks);
  if (!first_state || !second_state) {
    return false;
  }
  auto first_copy = first_source;
  auto second_copy = second_source;
  std::lock_guard publication_lock(publication_mutex_);
  std::lock_guard lock(mutex_);
  const auto first_owner = reservations_.find(first);
  const auto second_owner = reservations_.find(second);
  const auto first_row = states_.find(first);
  const auto second_row = states_.find(second);
  if (first_owner == reservations_.end() || first_owner->second.token != first_token ||
      second_owner == reservations_.end() || second_owner->second.token != second_token ||
      first_row == states_.end() || second_row == states_.end()) {
    return false;
  }
  const auto previous_first = shared_sources_.find(first);
  const auto previous_second = shared_sources_.find(second);
  if ((first_state->unknown_baseline && first_owner->second.previous_recorded &&
       !first_owner->second.previous.unknown_baseline) ||
      (second_state->unknown_baseline && second_owner->second.previous_recorded &&
       !second_owner->second.previous.unknown_baseline)) {
    return false;
  }
  if ((previous_first != shared_sources_.end() &&
       (previous_first->second.generation > first_source.generation ||
        (previous_first->second.generation == first_source.generation &&
         !same_shared_source(previous_first->second, first_source)))) ||
      (previous_second != shared_sources_.end() &&
       (previous_second->second.generation > second_source.generation ||
        (previous_second->second.generation == second_source.generation &&
         !same_shared_source(previous_second->second, second_source))))) {
    return false;
  }
  if (next_revision_ == 0 || next_revision_ == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error("Gathering source revisions are exhausted");
  }
  prepare_revision_locked(first);
  prepare_revision_locked(second);
  const bool first_was_shared = previous_first != shared_sources_.end();
  const bool second_was_shared = previous_second != shared_sources_.end();
  auto [stored_first, inserted_first] = shared_sources_.try_emplace(first, first_copy);
  bool inserted_second = false;
  try {
    auto second_result = shared_sources_.try_emplace(second, second_copy);
    auto stored_second = second_result.first;
    inserted_second = second_result.second;
    replace_due_indexes_locked({
        {first, first_row->second.due_tick, first_was_shared, first_state->due_tick, true},
        {second, second_row->second.due_tick, second_was_shared, second_state->due_tick, true}});
    static_assert(std::is_nothrow_swappable_v<StoredSharedGatheringSource>);
    using std::swap;
    swap(stored_first->second, first_copy);
    swap(stored_second->second, second_copy);
    first_row->second = *first_state;
    second_row->second = *second_state;
    const auto published_first = publish_revision_locked(first);
    const auto published_second = publish_revision_locked(second);
    if (first_revision) {
      *first_revision = published_first;
    }
    if (second_revision) {
      *second_revision = published_second;
    }
    if (retain_owners) {
      first_owner->second.previous = *first_state;
      first_owner->second.previous_recorded = true;
      second_owner->second.previous = *second_state;
      second_owner->second.previous_recorded = true;
    } else {
      reservations_.erase(first_owner);
      reservations_.erase(second_owner);
    }
  } catch (...) {
    if (inserted_second) {
      shared_sources_.erase(second);
    }
    if (inserted_first) {
      shared_sources_.erase(stored_first);
    }
    throw;
  }
  return true;
}

bool GatheringNodes::release_published_reservations(
    const std::vector<GatheringNodeKey> &nodes, const std::vector<std::uint64_t> &tokens) {
  if (nodes.empty() || nodes.size() > 2 || nodes.size() != tokens.size() ||
      (nodes.size() == 2 && nodes[0] == nodes[1])) {
    return false;
  }
  std::lock_guard publication_lock(publication_mutex_);
  std::lock_guard lock(mutex_);
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    const auto owner = reservations_.find(nodes[index]);
    if (owner == reservations_.end() || owner->second.token != tokens[index]) {
      return false;
    }
  }
  for (const auto &node : nodes) {
    reservations_.erase(node);
  }
  return true;
}

std::optional<std::vector<std::uint64_t>> GatheringNodes::reserve_boss_source_batch(
    const std::vector<BossSourceTarget> &targets, bool require_shared_initial) {
  if (targets.empty() || targets.size() > 2 ||
      (targets.size() == 2 && targets[0].node == targets[1].node)) {
    throw std::invalid_argument("Invalid boss source target batch");
  }
  std::vector<std::uint64_t> tokens;
  tokens.reserve(targets.size());
  std::lock_guard publication_lock(publication_mutex_);
  std::lock_guard lock(mutex_);
  const auto rollback = [&] {
    for (std::size_t index = 0; index < tokens.size(); ++index) {
      const auto owner = reservations_.find(targets[index].node);
      if (owner == reservations_.end() || owner->second.token != tokens[index]) {
        std::terminate();
      }
      if (!owner->second.previous_recorded) {
        states_.erase(targets[index].node);
      }
      reservations_.erase(owner);
    }
  };
  try {
    for (const auto &target : targets) {
      const auto existing = states_.find(target.node);
      if (require_shared_initial && !shared_sources_.contains(target.node) &&
          existing != states_.end() &&
          (existing->second.due_tick || existing->second.phase != target.initial_phase)) {
        rollback();
        return std::nullopt;
      }
      const auto token = reserve_locked(target.node, NodeState{}, std::nullopt);
      if (!token) {
        rollback();
        return std::nullopt;
      }
      tokens.push_back(*token);
    }
    return tokens;
  } catch (...) {
    rollback();
    throw;
  }
}

std::optional<std::vector<GatheringStateChange>> GatheringNodes::install_private_boss_sources(
    const std::vector<BossSourceTarget> &targets, const std::vector<std::uint64_t> &tokens,
    std::int64_t now_ticks) {
  if (now_ticks < 0 || targets.empty() || targets.size() > 2 || targets.size() != tokens.size() ||
      (targets.size() == 2 && targets[0].node == targets[1].node)) {
    return std::nullopt;
  }
  std::vector<GatheringStateChange> changes;
  changes.reserve(targets.size());
  std::vector<int> doodad_ids;
  doodad_ids.reserve(targets.size());
  std::lock_guard publication_lock(publication_mutex_);
  std::lock_guard lock(mutex_);
  if (next_revision_ == 0 || targets.size() >
          (std::numeric_limits<std::uint64_t>::max)() - next_revision_) {
    throw std::overflow_error("Gathering source revisions are exhausted");
  }

  for (std::size_t index = 0; index < targets.size(); ++index) {
    const auto &target = targets[index];
    const auto owner = reservations_.find(target.node);
    const auto state = states_.find(target.node);
    if (owner == reservations_.end() || owner->second.token != tokens[index] ||
        state == states_.end() || shared_sources_.contains(target.node)) {
      return std::nullopt;
    }
    int doodad_id = kCliffsEliteBarricade;
    if (target.node.map_id != kCliffsMap || target.node.sector_id != kCliffsEliteSector ||
        target.node.index != kCliffsEliteBarricadeLocalId) {
      const auto *point = data_ ? data_->doodad_point(target.node.map_id, target.node.sector_id,
                                                       target.node.index) : nullptr;
      if (!point || !data_->doodad(point->doodad_id)) {
        return std::nullopt;
      }
      doodad_id = point->doodad_id;
    }
    doodad_ids.push_back(doodad_id);
    const auto phase = owner->second.previous_recorded ? owner->second.previous.phase
                                                        : target.initial_phase;
    const auto begin_tick = owner->second.previous_recorded ? owner->second.previous.begin_tick
                                                             : now_ticks;
    changes.push_back({target.node, phase != kDespawnedDoodadPhase, phase, begin_tick});
    prepare_revision_locked(target.node);
  }

  for (std::size_t index = 0; index < targets.size(); ++index) {
    const auto &target = targets[index];
    auto &owner = reservations_.find(target.node)->second;
    auto &state = states_.find(target.node)->second;
    if (!owner.previous_recorded) {
      state.doodad_id = doodad_ids[index];
      state.phase = target.initial_phase;
      state.begin_tick = now_ticks;
    }
    owner.previous = state;
    owner.previous_recorded = true;
    changes[index].revision = publish_revision_locked(target.node);
  }
  return changes;
}

std::optional<StoredSharedGatheringSource>
GatheringNodes::shared_source(const GatheringNodeKey &node) const {
  std::lock_guard lock(mutex_);
  const auto found = shared_sources_.find(node);
  if (found == shared_sources_.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::recursive_mutex &GatheringNodes::publication_mutex() noexcept {
  return publication_mutex_;
}

bool GatheringNodes::is_current_revision(const GatheringNodeKey &node,
                                         std::uint64_t revision) const {
  if (revision == 0) {
    return false;
  }
  std::lock_guard lock(mutex_);
  const auto found = revisions_.find(node);
  return found != revisions_.end() && found->second == revision;
}

std::vector<SharedGatheringSourceRecord>
GatheringNodes::due_shared_sources(std::int64_t now_ticks, std::size_t limit,
                                   std::optional<GatheringNodeKey> after) const {
  std::vector<SharedGatheringSourceRecord> due;
  if (limit == 0) {
    return due;
  }
  std::lock_guard lock(mutex_);
  auto entry = after ? shared_due_by_node_.upper_bound(*after) : shared_due_by_node_.begin();
  for (; entry != shared_due_by_node_.end(); ++entry) {
    const auto &node = *entry;
    const auto state = states_.find(node);
    const auto source = shared_sources_.find(node);
    if (reservations_.contains(node) || state == states_.end() || source == shared_sources_.end() ||
        !state->second.due_tick || *state->second.due_tick > now_ticks) {
      continue;
    }
    due.push_back({{node.map_id, static_cast<int>(node.kind), node.sector_id, node.index},
                   source->second});
    if (due.size() == limit) {
      break;
    }
  }
  return due;
}

std::optional<std::uint64_t> GatheringNodes::reserve_locked(
    const GatheringNodeKey &node, NodeState committed,
    std::optional<std::int64_t> delay_ticks) {
  if (reservations_.contains(node)) {
    return std::nullopt;
  }
  if (next_reservation_token_ == 0) {
    throw std::overflow_error("Gathering reservation tokens are exhausted");
  }
  if (delay_ticks && *delay_ticks <= 0) {
    throw std::invalid_argument("Invalid gathering respawn delay");
  }
  const auto existing = states_.find(node);
  Reservation reservation;
  reservation.token = next_reservation_token_;
  reservation.previous_recorded = existing != states_.end();
  if (reservation.previous_recorded) {
    reservation.previous = existing->second;
  }
  reservation.committed = committed;
  reservation.delay_ticks = delay_ticks;
  // Preallocate the eventual node row before any persistent award begins. If reserving
  // the owner fails, remove the placeholder and leave the previous node unchanged.
  if (!reservation.previous_recorded) {
    states_.emplace(node, NodeState{});
  }
  try {
    reservations_.emplace(node, reservation);
  } catch (...) {
    if (!reservation.previous_recorded) {
      states_.erase(node);
    }
    throw;
  }
  ++next_reservation_token_;
  return reservation.token;
}

std::optional<std::uint64_t> GatheringNodes::reserve_placement(
    const GatheringNodeKey &node, std::int64_t now_ticks, std::int64_t respawn_delay_ticks) {
  std::lock_guard publication_lock(publication_mutex_);
  std::lock_guard lock(mutex_);
  const auto existing = states_.find(node);
  const auto shared = shared_sources_.find(node);
  if (node.kind != GatheringNodeKind::Placement || reservations_.contains(node) ||
      (shared != shared_sources_.end() && !shared->second.active) ||
      (shared == shared_sources_.end() && existing != states_.end() && existing->second.due_tick &&
       *existing->second.due_tick > now_ticks)) {
    return std::nullopt;
  }
  return reserve_locked(node, NodeState{}, respawn_delay_ticks);
}

std::optional<std::uint64_t> GatheringNodes::reserve_doodad(
    const GatheringNodeKey &node, const DoodadDefinition &definition, int expected_phase,
    int next_phase, std::optional<std::int64_t> next_delay_ticks) {
  std::lock_guard publication_lock(publication_mutex_);
  std::lock_guard lock(mutex_);
  const auto existing = states_.find(node);
  const int current = existing == states_.end() ? definition.initial_phase : existing->second.phase;
  if (node.kind != GatheringNodeKind::Doodad || reservations_.contains(node) ||
      current != expected_phase) {
    return std::nullopt;
  }
  NodeState committed;
  committed.doodad_id = definition.id;
  committed.phase = next_phase;
  return reserve_locked(node, committed, next_delay_ticks);
}

std::optional<std::uint64_t> GatheringNodes::reserve_cliffs_cannon_barrier(
    const GatheringNodeKey &node) {
  if (node.map_id != kCliffsMap || node.kind != GatheringNodeKind::Doodad ||
      node.sector_id != kCliffsBarrierSector || node.index != kCliffsBarrierLocalId) {
    throw std::invalid_argument("Unsupported scripted gathering barrier");
  }
  std::lock_guard publication_lock(publication_mutex_);
  std::lock_guard lock(mutex_);
  NodeState committed;
  committed.doodad_id = kCliffsBarrierDoodad;
  committed.phase = kDespawnedDoodadPhase;
  return reserve_locked(node, committed, std::nullopt);
}

bool GatheringNodes::finish_reservation(const GatheringNodeKey &node, std::uint64_t token,
                                         bool committed, std::int64_t now_ticks,
                                         std::optional<std::int64_t> remaining_ticks,
                                         std::uint64_t *revision) {
  std::lock_guard publication_lock(publication_mutex_);
  std::lock_guard lock(mutex_);
  const auto reservation = reservations_.find(node);
  if (reservation == reservations_.end() || reservation->second.token != token) {
    return false;
  }
  auto state = states_.find(node);
  if (state == states_.end()) {
    throw std::logic_error("Gathering reservation lost its staged node");
  }
  std::optional<NodeState> replacement;
  if (committed) {
    const auto due_delay = remaining_ticks ? remaining_ticks : reservation->second.delay_ticks;
    if (due_delay && *due_delay < 0) {
      throw std::invalid_argument("Invalid gathering remaining delay");
    }
    replacement = reservation->second.committed;
    replacement->begin_tick = now_ticks;
    if (due_delay) {
      const auto delay = *due_delay;
      const auto maximum = std::numeric_limits<std::int64_t>::max();
      replacement->due_tick = now_ticks > maximum - delay ? maximum : now_ticks + delay;
    }
  } else if (reservation->second.previous_recorded) {
    replacement = reservation->second.previous;
  }
  if (committed) {
    prepare_revision_locked(node);
  }
  const bool shared = shared_sources_.contains(node);
  replace_due_indexes_locked({{node, state->second.due_tick, shared,
                               replacement ? replacement->due_tick : std::nullopt, shared}});
  if (replacement) {
    state->second = *replacement;
  } else {
    states_.erase(state);
  }
  if (committed) {
    const auto published_revision = publish_revision_locked(node);
    if (revision) {
      *revision = published_revision;
    }
  }
  reservations_.erase(reservation);
  return true;
}

bool GatheringNodes::finish_reservation_pair(const GatheringNodeKey &first,
                                              std::uint64_t first_token,
                                              const GatheringNodeKey &second,
                                              std::uint64_t second_token, bool committed,
                                              std::int64_t now_ticks,
                                              std::uint64_t *first_revision,
                                              std::uint64_t *second_revision) {
  if (first == second) {
    throw std::invalid_argument("Linked gathering reservations must be distinct");
  }
  std::lock_guard publication_lock(publication_mutex_);
  std::lock_guard lock(mutex_);
  const auto first_owner = reservations_.find(first);
  const auto second_owner = reservations_.find(second);
  const auto first_row = states_.find(first);
  const auto second_row = states_.find(second);
  if (first_owner == reservations_.end() || first_owner->second.token != first_token ||
      second_owner == reservations_.end() || second_owner->second.token != second_token ||
      first_row == states_.end() || second_row == states_.end()) {
    return false;
  }
  const auto replacement_for = [&](const Reservation &owner) -> std::optional<NodeState> {
    if (!committed) {
      return owner.previous_recorded ? std::optional<NodeState>(owner.previous) : std::nullopt;
    }
    auto replacement = owner.committed;
    replacement.begin_tick = now_ticks;
    if (owner.delay_ticks) {
      const auto maximum = std::numeric_limits<std::int64_t>::max();
      const auto delay = *owner.delay_ticks;
      replacement.due_tick = now_ticks > maximum - delay ? maximum : now_ticks + delay;
    }
    return replacement;
  };
  const auto first_replacement = replacement_for(first_owner->second);
  const auto second_replacement = replacement_for(second_owner->second);
  if (committed) {
    if (next_revision_ == 0 || next_revision_ == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("Gathering source revisions are exhausted");
    }
    prepare_revision_locked(first);
    prepare_revision_locked(second);
  }
  const bool first_shared = shared_sources_.contains(first);
  const bool second_shared = shared_sources_.contains(second);
  replace_due_indexes_locked({
      {first, first_row->second.due_tick, first_shared,
       first_replacement ? first_replacement->due_tick : std::nullopt, first_shared},
      {second, second_row->second.due_tick, second_shared,
       second_replacement ? second_replacement->due_tick : std::nullopt, second_shared}});
  if (first_replacement) {
    first_row->second = *first_replacement;
  } else {
    states_.erase(first_row);
  }
  if (second_replacement) {
    second_row->second = *second_replacement;
  } else {
    states_.erase(second_row);
  }
  if (committed) {
    const auto published_first = publish_revision_locked(first);
    const auto published_second = publish_revision_locked(second);
    if (first_revision) {
      *first_revision = published_first;
    }
    if (second_revision) {
      *second_revision = published_second;
    }
  }
  reservations_.erase(first_owner);
  reservations_.erase(second_owner);
  return true;
}

bool GatheringNodes::try_deplete(const GatheringNodeKey &node, std::int64_t now_ticks,
                                 std::int64_t respawn_tick, std::uint64_t *revision) {
  std::lock_guard publication_lock(publication_mutex_);
  std::lock_guard lock(mutex_);
  const auto found = states_.find(node);
  if (reservations_.contains(node) || shared_sources_.contains(node) ||
      (found != states_.end() && found->second.due_tick && *found->second.due_tick > now_ticks)) {
    return false;
  }

  NodeState state;
  state.begin_tick = now_ticks;
  state.due_tick = respawn_tick;
  const auto published_revision = replace_state_locked(node, state);
  if (revision) {
    *revision = published_revision;
  }
  return true;
}

void GatheringNodes::restore(const GatheringNodeKey &node) {
  std::lock_guard publication_lock(publication_mutex_);
  std::lock_guard lock(mutex_);
  const auto found = states_.find(node);
  if (found == states_.end() || reservations_.contains(node) || shared_sources_.contains(node)) {
    return;
  }

  auto state = found->second;
  if (node.kind == GatheringNodeKind::Doodad && state.previous_recorded) {
    state.phase = state.previous_phase;
    state.begin_tick = state.previous_begin_tick;
    // Only interaction phases can be claimed, and those have no timer of their own.
    state.due_tick.reset();
    state.previous_recorded = false;
    replace_state_locked(node, state);
    return;
  }
  replace_state_locked(node, std::nullopt);
}

bool GatheringNodes::is_active(const GatheringNodeKey &node, std::int64_t now_ticks) const {
  std::lock_guard lock(mutex_);
  const auto found = states_.find(node);
  const auto shared = shared_sources_.find(node);
  if (reservations_.contains(node)) {
    return false;
  }
  if (shared != shared_sources_.end()) {
    return shared->second.active;
  }
  return found == states_.end() || !found->second.due_tick || *found->second.due_tick <= now_ticks;
}

int GatheringNodes::doodad_phase(const GatheringNodeKey &node,
                                 const DoodadDefinition &definition) const {
  std::lock_guard lock(mutex_);
  const auto found = states_.find(node);
  const auto pending = reservations_.find(node);
  if (found == states_.end() || found->second.unknown_baseline ||
      (pending != reservations_.end() && !pending->second.previous_recorded)) {
    return definition.initial_phase;
  }
  return found->second.phase;
}

bool GatheringNodes::try_advance_doodad(const GatheringNodeKey &node,
                                        const DoodadDefinition &definition, int expected_phase,
                                        int next_phase, std::int64_t now_ticks,
                                        std::optional<std::int64_t> next_tick,
                                        std::uint64_t *revision) {
  std::lock_guard publication_lock(publication_mutex_);
  std::lock_guard lock(mutex_);
  const auto found = states_.find(node);
  const bool recorded = found != states_.end();
  const int current = recorded ? found->second.phase : definition.initial_phase;
  if (current != expected_phase || reservations_.contains(node) ||
      shared_sources_.contains(node)) {
    return false;
  }

  NodeState state;
  state.doodad_id = definition.id;
  state.phase = next_phase;
  state.begin_tick = now_ticks;
  state.due_tick = next_tick;
  state.previous_recorded = recorded;
  if (recorded) {
    state.previous_phase = found->second.phase;
    state.previous_begin_tick = found->second.begin_tick;
  }
  const auto published_revision = replace_state_locked(node, state);
  if (revision) {
    *revision = published_revision;
  }
  return true;
}

void GatheringNodes::set_scripted_doodad_phase(const GatheringNodeKey &node, int doodad_id,
                                               int phase, std::int64_t now_ticks,
                                               std::uint64_t *revision) {
  std::lock_guard publication_lock(publication_mutex_);
  std::lock_guard lock(mutex_);
  if (reservations_.contains(node) || shared_sources_.contains(node)) {
    throw std::runtime_error("Scripted doodad phase requires durable source coordination");
  }
  NodeState state;
  state.doodad_id = doodad_id;
  state.phase = phase;
  state.begin_tick = now_ticks;
  const auto published_revision = replace_state_locked(node, state);
  if (revision) {
    *revision = published_revision;
  }
}

bool GatheringNodes::initialize_scripted_doodad_phase(const GatheringNodeKey &node, int doodad_id,
                                                        int phase, std::int64_t now_ticks,
                                                        std::uint64_t *revision) {
  std::lock_guard publication_lock(publication_mutex_);
  std::lock_guard lock(mutex_);
  if (states_.contains(node)) {
    return false;
  }
  NodeState state;
  state.doodad_id = doodad_id;
  state.phase = phase;
  state.begin_tick = now_ticks;
  const auto published_revision = replace_state_locked(node, state);
  if (revision) {
    *revision = published_revision;
  }
  return true;
}

bool GatheringNodes::record_volume_doodad_phase(const GatheringNodeKey &node, int phase,
                                                std::int64_t now_ticks,
                                                bool only_if_unrecorded,
                                                std::uint64_t *revision) {
  std::lock_guard publication_lock(publication_mutex_);
  std::lock_guard lock(mutex_);
  const auto found = states_.find(node);
  if (reservations_.contains(node) || shared_sources_.contains(node)) {
    throw std::runtime_error("Volume doodad phase requires durable source coordination");
  }
  if (found != states_.end() && (only_if_unrecorded || found->second.phase == phase)) {
    return false;
  }

  // Volume doodads need no definition: their phase holds until a script changes it again.
  NodeState state;
  state.phase = phase;
  state.begin_tick = now_ticks;
  const auto published_revision = replace_state_locked(node, state);
  if (revision) {
    *revision = published_revision;
  }
  return true;
}

std::vector<GatheringStateChange>
GatheringNodes::cliffs_monster_defeated(int map_id, int npc_cms_id, std::int64_t now_ticks) {
  if (map_id != kCliffsMap) {
    return {};
  }

  std::vector<GatheringStateChange> changes;
  const auto activate = [&](std::uint32_t sector, std::uint32_t index, int doodad, int phase) {
    const GatheringNodeKey node{kCliffsMap, GatheringNodeKind::Doodad, sector, index};
    std::uint64_t revision{};
    if (initialize_scripted_doodad_phase(node, doodad, phase, now_ticks, &revision)) {
      changes.push_back({node, true, phase, now_ticks, revision});
    }
  };

  if (npc_cms_id == kCliffsElite) {
    activate(kCliffsEliteSector, kCliffsEliteBarricadeLocalId, kCliffsEliteBarricade, 2);
    activate(kCliffsEliteSector, kCliffsEliteObjectLocalId, kCliffsEliteObject, 1);
  } else if (npc_cms_id == kCliffsCaptain) {
    // The official encounter enables the rift after a separate post-boss object interaction.
    // That script is not available yet; enabling the rift here keeps the quest reachable.
    activate(kCliffsRiftSector, kCliffsRiftLocalId, kCliffsRift, 1);
  }
  return changes;
}

std::vector<GatheringStateChange>
GatheringNodes::restore_cliffs_progress(int map_id, const WorldProgress &progress,
                                         std::int64_t now_ticks) {
  if (map_id != kCliffsMap) {
    return {};
  }
  std::vector<GatheringStateChange> changes;
  bool cannon_reached = cliffs_objective_reached(progress, kCliffsCannonQuest,
                                                  kCliffsEliteObjective);
  for (int objective = kCliffsCannonObjectiveFirst; objective <= kCliffsCannonObjectiveLast;
       ++objective) {
    cannon_reached = cannon_reached ||
                     cliffs_objective_reached(progress, kCliffsCannonQuest, objective);
  }
  const bool captain_reached = cliffs_objective_reached(progress, kCliffsCaptainQuest,
                                                        kCliffsCaptainObjective);
  if (cannon_reached || captain_reached) {
    const GatheringNodeKey cannon_barrier{kCliffsMap, GatheringNodeKind::Doodad,
                                          kCliffsBarrierSector, kCliffsBarrierLocalId};
    std::uint64_t revision{};
    if (initialize_scripted_doodad_phase(cannon_barrier, kCliffsBarrierDoodad,
                                         kDespawnedDoodadPhase, now_ticks, &revision)) {
      changes.push_back({cannon_barrier, false, kDespawnedDoodadPhase, now_ticks, revision});
    }
  }
  if (cliffs_objective_reached(progress, kCliffsCannonQuest, kCliffsEliteObjective) ||
      captain_reached) {
    auto elite = cliffs_monster_defeated(map_id, kCliffsElite, now_ticks);
    changes.insert(changes.end(), elite.begin(), elite.end());
  }
  if (captain_reached) {
    auto rift = cliffs_monster_defeated(map_id, kCliffsCaptain, now_ticks);
    changes.insert(changes.end(), rift.begin(), rift.end());
  }
  return changes;
}

std::optional<GatheringStateChange>
GatheringNodes::advance_due(const GatheringNodeKey &node, NodeState &state, bool &erase) {
  erase = false;
  const auto due = *state.due_tick;
  if (node.kind == GatheringNodeKind::Placement) {
    erase = true;
    return GatheringStateChange{node, true, std::nullopt, due};
  }

  const DoodadDefinition *definition = data_ ? data_->doodad(state.doodad_id) : nullptr;
  if (definition == nullptr) {
    // Without its definition the doodad can only return to the authored initial state.
    erase = true;
    return GatheringStateChange{node, true, std::nullopt, due};
  }

  if (state.phase == kDespawnedDoodadPhase) {
    erase = true;
    return GatheringStateChange{node, true, std::nullopt, due};
  }

  const auto *phase = definition->phase(state.phase);
  if (phase == nullptr || phase->interaction_id) {
    // Interaction phases wait for a player; a stale timer is dropped.
    state.due_tick.reset();
    return std::nullopt;
  }

  std::vector<int> weights;
  for (const auto &choice : phase->next) {
    weights.push_back(choice.weight);
  }
  std::discrete_distribution<std::size_t> choose(weights.begin(), weights.end());
  const int next_phase = phase->next.at(choose(random_)).phase_id;

  state.phase = next_phase;
  state.begin_tick = due;
  state.previous_recorded = false;
  state.due_tick.reset();
  if (next_phase == kDespawnedDoodadPhase) {
    std::uniform_int_distribution<int> delay(definition->respawn_delay_min_ms,
                                             definition->respawn_delay_max_ms);
    state.due_tick = add_milliseconds(due, delay(random_));
  } else if (const auto *next = definition->phase(next_phase); next && !next->interaction_id) {
    state.due_tick = add_milliseconds(due, next->duration_ms);
  }
  return GatheringStateChange{node, next_phase != kDespawnedDoodadPhase, next_phase, due};
}

std::vector<GatheringStateChange> GatheringNodes::collect_respawns(std::int64_t now_ticks) {
  // Bounds the transitions one node can replay after a long scheduler stall.
  constexpr int kMaximumTransitionsPerNode = 8;

  std::vector<GatheringStateChange> changes;
  std::lock_guard publication_lock(publication_mutex_);
  std::lock_guard lock(mutex_);
  std::vector<GatheringNodeKey> due_nodes;
  for (const auto &[tick, node] : local_due_by_time_) {
    if (tick > now_ticks) {
      break;
    }
    if (!reservations_.contains(node)) {
      due_nodes.push_back(node);
    }
  }
  for (const auto &node : due_nodes) {
    const auto entry = states_.find(node);
    if (entry == states_.end() || reservations_.contains(node) || shared_sources_.contains(node)) {
      continue;
    }
    auto replacement = entry->second;
    bool erase = false;
    std::vector<GatheringStateChange> node_changes;
    for (int step = 0; step < kMaximumTransitionsPerNode; ++step) {
      if (!replacement.due_tick || *replacement.due_tick > now_ticks) {
        break;
      }
      if (auto change = advance_due(node, replacement, erase)) {
        node_changes.push_back(*change);
      }
      if (erase) {
        break;
      }
    }
    changes.insert(changes.end(), node_changes.begin(), node_changes.end());
    const auto revision = replace_state_locked(
        node, erase ? std::nullopt : std::optional<NodeState>(replacement));
    if (!node_changes.empty()) {
      changes.back().revision = revision;
    }
  }
  return changes;
}

std::optional<std::int64_t> GatheringNodes::next_respawn_tick(bool exclude_shared) const {
  std::lock_guard lock(mutex_);
  const auto earliest_available = [&](const std::set<DueKey> &index)
      -> std::optional<std::int64_t> {
    for (const auto &[tick, node] : index) {
      if (!reservations_.contains(node)) {
        return tick;
      }
    }
    return std::nullopt;
  };
  const auto local = earliest_available(local_due_by_time_);
  const auto shared = exclude_shared ? std::nullopt : earliest_available(shared_due_by_time_);
  if (!local) {
    return shared;
  }
  if (!shared) {
    return local;
  }
  return std::min(*local, *shared);
}

std::optional<std::int64_t> GatheringNodes::next_shared_respawn_tick() const {
  std::lock_guard lock(mutex_);
  for (const auto &[tick, node] : shared_due_by_time_) {
    if (!reservations_.contains(node)) {
      return tick;
    }
  }
  return std::nullopt;
}

std::pair<std::vector<GatheringStateChange>, std::vector<GatheringStateChange>>
GatheringNodes::sector_state(int map_id, std::uint32_t sector_id,
                             const std::function<void()> &before_append) const {
  std::vector<GatheringStateChange> placements;
  std::vector<GatheringStateChange> doodads;
  std::size_t examined = 0;
  std::lock_guard lock(mutex_);
  for (const auto kind : {GatheringNodeKind::Placement, GatheringNodeKind::Doodad}) {
    auto entry = states_.lower_bound({map_id, kind, sector_id, 0});
    while (entry != states_.end() && entry->first.map_id == map_id &&
           entry->first.kind == kind && entry->first.sector_id == sector_id) {
      if (++examined > kMaximumSectorNodes) {
        throw std::runtime_error("Gathering sector reconstruction exceeds its node limit");
      }
      const auto &node = entry->first;
      const auto &state = entry->second;
      if (state.unknown_baseline) {
        ++entry;
        continue;
      }
      const auto pending = reservations_.find(node);
      if (pending != reservations_.end() && !pending->second.previous_recorded) {
        ++entry;
        continue;
      }
      const auto shared = shared_sources_.find(node);
      const auto revision = revisions_.find(node);
      const auto local_revision = revision == revisions_.end() ? 0 : revision->second;
      if (kind == GatheringNodeKind::Placement) {
        if (shared == shared_sources_.end() || !shared->second.active) {
          if (before_append) {
            before_append();
          }
          placements.push_back({node, false, std::nullopt, state.begin_tick, local_revision});
        }
      } else {
        bool initial = false;
        if (shared != shared_sources_.end() && !shared->second.next_transition_epoch_ticks && data_) {
          const auto *definition = data_->doodad(state.doodad_id);
          initial = definition && state.phase == definition->initial_phase &&
                    !initially_deactivated_doodad(*data_, node, state.doodad_id);
        }
        if (!initial) {
          if (before_append) {
            before_append();
          }
          doodads.push_back({node, state.phase != kDespawnedDoodadPhase, state.phase,
                             state.begin_tick, local_revision});
        }
      }
      ++entry;
    }
  }
  return {std::move(placements), std::move(doodads)};
}

std::vector<GatheringStateChange> GatheringNodes::depleted_on_map(
    int map_id, const std::function<void()> &before_append) const {
  std::vector<GatheringStateChange> changes;
  std::lock_guard lock(mutex_);
  for (const auto &[node, state] : states_) {
    if (state.unknown_baseline) {
      continue;
    }
    const auto pending = reservations_.find(node);
    if (pending != reservations_.end() && !pending->second.previous_recorded) {
      continue;
    }
    const auto shared = shared_sources_.find(node);
    if (shared != shared_sources_.end() && shared->second.active) {
      continue;
    }
    if (node.map_id == map_id && node.kind == GatheringNodeKind::Placement) {
      if (before_append) {
        before_append();
      }
      const auto revision = revisions_.find(node);
      changes.push_back({node, false, std::nullopt, state.begin_tick,
                         revision == revisions_.end() ? 0 : revision->second});
    }
  }
  return changes;
}

std::vector<GatheringStateChange> GatheringNodes::doodads_on_map(
    int map_id, const std::function<void()> &before_append) const {
  std::vector<GatheringStateChange> changes;
  std::lock_guard lock(mutex_);
  for (const auto &[node, state] : states_) {
    if (state.unknown_baseline) {
      continue;
    }
    const auto pending = reservations_.find(node);
    if (pending != reservations_.end() && !pending->second.previous_recorded) {
      continue;
    }
    const auto shared = shared_sources_.find(node);
    if (shared != shared_sources_.end() && !shared->second.next_transition_epoch_ticks &&
        data_) {
      const auto *definition = data_->doodad(state.doodad_id);
      if (definition && state.phase == definition->initial_phase &&
          !initially_deactivated_doodad(*data_, node, state.doodad_id)) {
        continue;
      }
    }
    if (node.map_id == map_id && node.kind == GatheringNodeKind::Doodad) {
      if (before_append) {
        before_append();
      }
      const auto revision = revisions_.find(node);
      changes.push_back({node, state.phase != kDespawnedDoodadPhase, state.phase, state.begin_tick,
                         revision == revisions_.end() ? 0 : revision->second});
    }
  }
  return changes;
}

GatheringSession::GatheringSession(std::shared_ptr<const GatheringData> data,
                                   std::shared_ptr<GatheringNodes> nodes,
                                   std::uint64_t player_unit_id, GatheringPersistence persistence,
                                   std::uint32_t random_seed, GatheringVitals vitals)
    : data_(std::move(data)), nodes_(std::move(nodes)), player_unit_id_(player_unit_id),
      persistence_(std::move(persistence)), vitals_(std::move(vitals)), random_(random_seed) {
  if (!data_ || !nodes_ || player_unit_id_ == 0 || !persistence_.load_character ||
      !persistence_.load_progress || !persistence_.commit) {
    throw std::invalid_argument("Gathering session requires data, nodes, unit and storage");
  }
}

WorldProgress GatheringSession::load_progress() const {
  return persistence_.load_progress();
}

bool GatheringSession::handles(std::uint16_t opcode, const Json &request) const {
  if (opcode == kGatheringStartInteractionRequest) {
    const auto param = request.find("param");
    if (param == request.end() || !param->is_object()) {
      return false;
    }
    if (param->contains("gathering")) {
      return true;
    }
    if (param->contains("handling_loot")) {
      return butchering_available();
    }
    // Doodad gathering interactions (20001-20003) are dedicated to harvesting, so every
    // request naming one is answered here, including ones with an unknown target.
    const auto interaction = requested_interaction(request);
    if (!interaction) {
      return false;
    }
    if (data_->is_doodad_interaction(*interaction)) {
      return true;
    }
    // Quest doodad interaction ids can be shared with other systems' targets, so only a request
    // naming a profiled quest doodad belongs here.
    return data_->is_quest_doodad_interaction(*interaction) && names_quest_doodad(request);
  }
  if (opcode == kGatheringInteractUnitRequest) {
    // Instant quest doodad interactions; every other unit interaction belongs elsewhere.
    const auto interaction = requested_interaction(request);
    return interaction && data_->is_quest_doodad_interaction(*interaction) &&
           names_quest_doodad(request);
  }
  if (opcode == kGatheringExecuteInteractionRequest || opcode == kGatheringEndInteractionRequest) {
    return pending_.has_value();
  }
  return false;
}

bool GatheringSession::names_quest_doodad(const Json &request) const {
  try {
    return data_->names_quest_doodad(parse_unit_id(request.at("target_unit_id")));
  } catch (const GatheringError &) {
    return false;
  } catch (const std::exception &) {
    // Malformed unit ids (std::stoull) and JSON shapes belong to no profiled doodad.
    return false;
  }
}

bool GatheringSession::corpse_request(std::uint16_t opcode, const Json &request) const {
  if (opcode == kGatheringStartInteractionRequest) {
    const auto param = request.find("param");
    return param != request.end() && param->is_object() && param->contains("handling_loot") &&
           butchering_available();
  }
  if (opcode == kGatheringExecuteInteractionRequest || opcode == kGatheringEndInteractionRequest) {
    return pending_.has_value() && pending_->corpse_unit_id.has_value();
  }
  return false;
}

bool GatheringSession::butchering_available() const {
  return data_->handling_animation_id() != 0 && static_cast<bool>(corpses_.find);
}

void GatheringSession::set_corpses(GatheringCorpses corpses) {
  corpses_ = std::move(corpses);
}

void GatheringSession::set_nodes(std::shared_ptr<GatheringNodes> nodes) {
  if (!nodes || nodes == nodes_) {
    return;
  }
  // A claim belongs to the nodes it was made on; the character left them with its map or copy.
  pending_.reset();
  nodes_ = std::move(nodes);
}

const std::shared_ptr<GatheringNodes> &GatheringSession::nodes() const noexcept {
  return nodes_;
}

QuestEvents GatheringSession::take_quest_events() {
  auto events = std::move(quest_events_);
  quest_events_.clear();
  return events;
}

bool GatheringSession::busy() const {
  return pending_.has_value() && !pending_->executed;
}

GatheringOutcome GatheringSession::receive(std::uint16_t opcode, const Json &request,
                                           std::uint32_t sequence, const WorldPosition &position,
                                           std::int64_t server_ticks, bool other_action_busy) {
  const auto rejection = [&](const GatheringError &error) {
    GatheringOutcome outcome;
    outcome.packets = error.preceding;
    if (opcode == kGatheringStartInteractionRequest) {
      // The captured rejected starts used StartInteractionAnimRes with an error code.
      outcome.packets.push_back({kStartInteractionAnimResponse,
                                 {{"ec", error.code},
                                  {"weapon_state_type", kSheathedWeaponState},
                                  {"packet_seq", {{"seq", std::to_string(sequence)}}}}});
    } else if (opcode == kGatheringInteractUnitRequest) {
      // InteractUnitRes carries its own error code (x5.pb.InteractUnitRes.ec).
      outcome.packets.push_back(
          {kInteractUnitResponse,
           {{"ec", error.code}, {"packet_seq", {{"seq", std::to_string(sequence)}}}}});
    } else {
      outcome.packets.push_back(
          {kCommonError, {{"ec", error.code}, {"packet_seq", {{"seq", std::to_string(sequence)}}}}});
    }
    return outcome;
  };

  try {
    require(server_ticks > 0 &&
                server_ticks <= std::numeric_limits<std::int64_t>::max() / kWireNanosecondsPerTick -
                                    kActionTimeoutTicks,
            kBadRequest);
    require(within_request_limit(server_ticks), kBadRequest);

    switch (opcode) {
    case kGatheringStartInteractionRequest:
      return start(request, sequence, position, server_ticks, other_action_busy);
    case kGatheringExecuteInteractionRequest:
      return execute(request, position, server_ticks);
    case kGatheringEndInteractionRequest:
      return end(request, server_ticks);
    case kGatheringInteractUnitRequest:
      return instant(request, sequence, position, server_ticks, other_action_busy);
    default:
      return {};
    }
  } catch (const GatheringError &error) {
    return rejection(error);
  } catch (const Json::exception &) {
    return rejection({kBadRequest, {}});
  } catch (const std::invalid_argument &) {
    return rejection({kBadRequest, {}});
  } catch (const std::out_of_range &) {
    return rejection({kBadRequest, {}});
  }
}

GatheringSession::ResolvedTarget
GatheringSession::resolve_placement(const Json &request, const WorldPosition &position) const {
  const auto index = parse_single_index(request);
  const auto sector_id = parse_target_sector(request.at("target_unit_id"));

  const auto *point = data_->point(sector_id, index);
  require(point != nullptr, kTargetNotFound);
  const std::array<int, 3> point_mm{point->x_mm, point->y_mm, point->z_mm};
  require(data_->map_contains(position.map_id, point_mm), kTargetNotFound);

  const auto *definition = data_->definition(point->gathering_id);
  require(definition != nullptr, kUnknownGathering);

  ResolvedTarget target;
  target.node = {position.map_id, GatheringNodeKind::Placement, sector_id, index};
  target.position_mm = point_mm;
  target.animation_id = definition->animation_id;
  target.placement = definition;
  return target;
}

GatheringSession::ResolvedTarget
GatheringSession::resolve_doodad(const Json &request, const WorldPosition &position) const {
  const auto interaction_id = requested_interaction(request);
  require(interaction_id.has_value(), kInvalidParameter);

  const auto unit_id = parse_unit_id(request.at("target_unit_id"));
  require((unit_id & kDoodadUnitTypeMask) == kDoodadUnitType, kInvalidTargetType);
  const auto sector_id = static_cast<std::uint32_t>(unit_id >> 32);
  const auto local_id =
      static_cast<std::uint32_t>((unit_id >> kDoodadLocalIdShift) & kDoodadLocalIdMask);

  const auto *point = data_->doodad_point(position.map_id, sector_id, local_id);
  require(point != nullptr, kTargetNotFound);
  const std::array<int, 3> point_mm{point->x_mm, point->y_mm, point->z_mm};
  require(data_->map_contains(position.map_id, point_mm), kTargetNotFound);

  const auto *definition = data_->doodad(point->doodad_id);
  require(definition != nullptr, kTargetNotFound);

  ResolvedTarget target;
  target.node = {position.map_id, GatheringNodeKind::Doodad, sector_id, local_id};
  target.position_mm = point_mm;
  target.doodad = definition;

  // A despawned doodad, or one in a timed phase, cannot be harvested right now.
  const auto *phase = definition->phase(nodes_->doodad_phase(target.node, *definition));
  require(phase != nullptr && phase->interaction_id.has_value(), kDoodadNotProgressible);
  require(*phase->interaction_id == *interaction_id, kWrongInteraction);
  target.doodad_phase = phase;
  target.animation_id = phase->animation_id;
  return target;
}

GatheringSession::ResolvedTarget GatheringSession::resolve_corpse(const Json &request,
                                                                  const WorldPosition &position,
                                                                  std::int64_t server_ticks) const {
  const auto unit_id = parse_unit_id(request.at("target_unit_id"));
  const auto corpse = corpses_.find(unit_id, server_ticks);
  // Unknown, expired, someone else's or already butchered corpses all have nothing to handle.
  if (!corpse || data_->handling_reward(corpse->npc_cms_id) == nullptr) {
    throw GatheringError{kNoHandlingLoot, {}};
  }
  require(corpse->map_id == position.map_id, kTargetNotFound);

  ResolvedTarget target;
  target.node = {position.map_id, GatheringNodeKind::Placement, 0, 0};
  target.position_mm = corpse->position_mm;
  target.animation_id = data_->handling_animation_id();
  target.corpse_unit_id = unit_id;
  return target;
}

GatheringOutcome GatheringSession::start(const Json &request, std::uint32_t sequence,
                                         const WorldPosition &position, std::int64_t server_ticks,
                                         bool other_action_busy) {
  const int animation_id = request.at("interaction_anim_id").get<int>();
  require(!pending_ || pending_expired(server_ticks), kActionInProgress);
  require(!other_action_busy, kActionInProgress);
  require(!vitals_.dead || !vitals_.dead(), kDead);

  const auto &param = request.at("param");
  const bool corpse = param.contains("handling_loot");
  const bool doodad = !corpse && !param.contains("gathering");
  ResolvedTarget target;
  if (corpse) {
    target = resolve_corpse(request, position, server_ticks);
  } else if (doodad) {
    target = resolve_doodad(request, position);
  } else {
    target = resolve_placement(request, position);
  }
  if (doodad && target.doodad->id == kCliffsPrisonDoor) {
    const auto character = persistence_.load_character();
    require(character && rescue_door_allowed(persistence_.load_progress(), *character),
            kMissingCondition);
  }
  if (doodad && position.map_id == kCliffsMap && target.doodad->id == kCliffsRift) {
    require(cliffs_objective_reached(persistence_.load_progress(), kCliffsCaptainQuest,
                                     kCliffsCaptainObjective), kMissingCondition);
  }
  require(target.animation_id == animation_id, kAnimationMismatch);

  if (target.doodad_phase && target.doodad_phase->skill_effect_id) {
    require(vitals_.effect_available && vitals_.apply_effect &&
                vitals_.effect_available(*target.doodad_phase->skill_effect_id),
            kMissingCondition);
  }

  const auto *animation = data_->animation(animation_id);
  require(animation != nullptr, kInvalidData);
  require(request_position_matches(request, position) &&
              within(position, target.position_mm, kGatheringRangeMm),
          kTooFar);

  if (!doodad) {
    if (!corpse) {
      require(nodes_->is_active(target.node, server_ticks), kTargetDepleted);
    }
    // Captured placement gathers and butchering wear the equipped tool (the knife lost 100
    // durability per corpse); doodad gathers never changed one.
    if (animation->tool_equipment_type != "None") {
      require(usable_tool(*animation).has_value(), kMissingCondition);
    }
  }

  GatheringOutcome outcome;
  // An expired, unfinished action is replaced; its SP drain is stopped first.
  outcome.packets = abandon_pending(server_ticks);

  const auto wire_tick = request_wire_tick(request, server_ticks);
  bool stamina_flow = false;
  if (vitals_.begin_stamina && animation->sp_cost > 0) {
    const GatheringStaminaFlow flow{animation->sp_cost, wire_tick, animation->execute_time_ms,
                                    animation->duration_ms + data_->sp_regen_delay_ms(),
                                    server_ticks};
    auto announcement = vitals_.begin_stamina(flow);
    if (!announcement) {
      throw GatheringError{kMissingCondition, std::move(outcome.packets)};
    }
    // Official order: the SP flow StatSyncNoti precedes StartInteractionAnimNoti.
    outcome.packets.insert(outcome.packets.end(), std::make_move_iterator(announcement->begin()),
                           std::make_move_iterator(announcement->end()));
    stamina_flow = true;
  }

  pending_ = PendingGathering{target.node,
                              animation_id,
                              server_ticks,
                              false,
                              target.doodad_phase ? target.doodad_phase->phase_id : 0,
                              stamina_flow,
                              target.corpse_unit_id};

  // Official StartInteractionAnimNoti echoes the request's param, position, yaw and tick, and
  // carries an empty packet_seq and no target_unit_id.
  GameplayPacket notification{kStartInteractionAnimNotification,
                              {{"unit_id", std::to_string(player_unit_id_)},
                               {"interaction_anim_id", animation_id},
                               {"param", request.at("param")},
                               {"pos", request.at("pos")},
                               {"yaw", request_yaw(request, position)},
                               {"tick", std::to_string(wire_tick)},
                               {"weapon_state_type", kSheathedWeaponState},
                               {"packet_seq", Json::object()}}};
  outcome.observers.push_back(notification);
  outcome.packets.push_back(std::move(notification));
  return outcome;
}

GatheringOutcome GatheringSession::execute(const Json &request, const WorldPosition &position,
                                           std::int64_t server_ticks) {
  require(pending_ && pending_->animation_id == request.at("interaction_anim_id").get<int>(),
          kAnimationMismatch);
  if (vitals_.dead && vitals_.dead()) {
    throw GatheringError{kDead, abandon_pending(server_ticks)};
  }
  if (pending_->executed) {
    // A retried execute never grants a second reward.
    return {{animation_result()}, {}, {}};
  }

  const auto &animation = *data_->animation(pending_->animation_id);
  const auto earliest_ticks = add_milliseconds(
      pending_->started_ticks, animation.execute_time_ms - kExecuteToleranceMs);
  require(server_ticks >= earliest_ticks, kTooEarly);
  if (pending_expired(server_ticks)) {
    throw GatheringError{kTooEarly, abandon_pending(server_ticks)};
  }

  const auto node = pending_->node;
  const bool corpse = pending_->corpse_unit_id.has_value();
  const bool doodad = !corpse && node.kind == GatheringNodeKind::Doodad;
  int target_map_id = node.map_id;
  std::array<int, 3> node_mm{};
  const GatheringLoot *loot{};
  const DoodadDefinition *doodad_definition{};
  const DoodadPhase *doodad_phase{};
  const GatheringDefinition *placement{};
  std::shared_ptr<CorpseHandlingState> corpse_handling;
  if (corpse) {
    // The corpse may have expired or been removed during the animation.
    const auto target = corpses_.find(*pending_->corpse_unit_id, server_ticks);
    loot = target ? data_->handling_reward(target->npc_cms_id) : nullptr;
    if (loot == nullptr) {
      throw GatheringError{kNoHandlingLoot, abandon_pending(server_ticks)};
    }
    target_map_id = target->map_id;
    node_mm = target->position_mm;
    corpse_handling = target->handling;
  } else if (doodad) {
    const auto &point = *data_->doodad_point(node.map_id, node.sector_id, node.index);
    node_mm = {point.x_mm, point.y_mm, point.z_mm};
    doodad_definition = data_->doodad(point.doodad_id);
    doodad_phase = doodad_definition->phase(pending_->doodad_phase);
    loot = &doodad_phase->loot;
  } else {
    const auto &point = *data_->point(node.sector_id, node.index);
    node_mm = {point.x_mm, point.y_mm, point.z_mm};
    placement = data_->definition(point.gathering_id);
    loot = &placement->loot;
  }
  if (doodad_phase && doodad_phase->skill_effect_id &&
      (!vitals_.effect_available || !vitals_.apply_effect ||
       !vitals_.effect_available(*doodad_phase->skill_effect_id))) {
    throw GatheringError{kMissingCondition, abandon_pending(server_ticks)};
  }
  if (position.map_id != target_map_id || !within(position, node_mm, kGatheringRangeMm)) {
    throw GatheringError{kTooFar, abandon_pending(server_ticks)};
  }

  std::optional<CharacterItem> worn_tool;
  if (!doodad) {
    worn_tool = usable_tool(animation);
    if (animation.tool_equipment_type != "None" && !worn_tool) {
      // The tool was unequipped or broke during the animation.
      throw GatheringError{kMissingCondition, abandon_pending(server_ticks)};
    }
  }

  const auto before = persistence_.load_progress();
  if (before.revision == std::numeric_limits<std::int64_t>::max()) {
    throw GatheringError{kGatheringFailed, abandon_pending(server_ticks)};
  }
  auto after = before;
  // Storage accepts only a transition that advances the progress revision exactly once.
  after.revision = before.revision + 1;
  const auto rewards = roll_loot(*loot);
  for (const auto &[item_cms_id, quantity] : rewards) {
    auto &stack = after.stackables[item_cms_id];
    if (stack > std::numeric_limits<int>::max() - quantity) {
      throw GatheringError{kGatheringFailed, abandon_pending(server_ticks)};
    }
    stack += quantity;
  }

  ItemTrade trade;
  if (doodad && doodad_definition->id == kCliffsPrisonDoor) {
    const auto character = persistence_.load_character();
    if (!character || !rescue_door_allowed(before, *character)) {
      throw GatheringError{kMissingCondition, abandon_pending(server_ticks)};
    }
    consume_rescue_key(*character, after, trade);
  }
  if (doodad && position.map_id == kCliffsMap && doodad_definition->id == kCliffsRift &&
      !cliffs_objective_reached(before, kCliffsCaptainQuest, kCliffsCaptainObjective)) {
    throw GatheringError{kMissingCondition, abandon_pending(server_ticks)};
  }
  if (worn_tool) {
    const int worn_durability =
        std::max(0, worn_tool->durability - data_->tool_durability_loss());
    trade.durability_updates.push_back({worn_tool->id, worn_tool->durability, worn_durability});
    worn_tool->durability = worn_durability;
  }

  // A production claim reserves its source before any write and retains it until the
  // combined receipt, quest intent and durable source state are all resolved.
  int next_phase = 0;
  std::uint64_t legacy_revision{};
  bool claimed = false;
  std::vector<GameplayPacket> durable_packets;
  std::vector<GatheringStateChange> durable_changes;
  if (persistence_.commit_durable) {
    std::optional<std::uint64_t> reservation;
    std::optional<std::uint64_t> corpse_reservation;
    std::optional<std::int64_t> delay_ticks;
    std::optional<int> timed_successor;
    if (corpse) {
      if (corpse_handling) {
        corpse_reservation = corpse_handling->reserve();
      }
      claimed = corpse_reservation.has_value();
    } else if (doodad) {
      next_phase = choose_next_phase(*doodad_phase);
      const auto next_tick = doodad_next_tick(*doodad_definition, next_phase, server_ticks);
      if (next_tick) {
        delay_ticks = *next_tick - server_ticks;
      }
      if (const auto *timed = doodad_definition->phase(next_phase);
          timed && !timed->interaction_id) {
        timed_successor = choose_next_phase(*timed);
      } else if (next_phase == kDespawnedDoodadPhase && next_tick) {
        timed_successor = doodad_definition->initial_phase;
      }
      reservation = nodes_->reserve_doodad(node, *doodad_definition, pending_->doodad_phase,
                                            next_phase, delay_ticks);
      claimed = reservation.has_value();
    } else {
      delay_ticks = respawn_tick(*placement, server_ticks) - server_ticks;
      reservation = nodes_->reserve_placement(node, server_ticks, *delay_ticks);
      claimed = reservation.has_value();
    }
    if (!claimed) {
      throw GatheringError{kTargetDepleted, abandon_pending(server_ticks)};
    }
    GatheringDurableResult result;
    bool handed_to_adapter = false;
    const auto cleanup_unregistered = [&] {
      if (!handed_to_adapter && reservation &&
          !nodes_->finish_reservation(node, *reservation, false, server_ticks)) {
        std::terminate();
      }
      if (!handed_to_adapter && corpse_reservation &&
          !corpse_handling->finish(*corpse_reservation, false)) {
        std::terminate();
      }
    };
    try {
      GatheringDurableRequest action;
      action.node = node;
      action.corpse_unit_id = pending_->corpse_unit_id;
      action.corpse_handling = corpse_handling;
      action.corpse_reservation = corpse_reservation;
      action.reservation = reservation;
      action.nodes = nodes_;
      action.expected_phase = pending_->doodad_phase;
      action.next_phase = next_phase;
      action.delay_ticks = delay_ticks;
      action.timed_successor_phase = timed_successor;
      if (doodad) {
        action.events.push_back(
            {QuestEventKind::DoodadInteracted, doodad_definition->id, pending_->doodad_phase, 1});
      }
      action.position = position;
      action.before = before;
      action.after = after;
      action.trade = trade;
      handed_to_adapter = true;
      result = persistence_.commit_durable(action);
    } catch (const std::exception &error) {
      cleanup_unregistered();
      spdlog::error("Gathering commit setup failed: {}", error.what());
      throw GatheringError{kGatheringFailed, abandon_pending(server_ticks)};
    } catch (...) {
      cleanup_unregistered();
      spdlog::error("Gathering commit setup failed with a nonstandard exception");
      throw GatheringError{kGatheringFailed, abandon_pending(server_ticks)};
    }
    if (!result.committed) {
      throw GatheringError{kGatheringFailed, abandon_pending(server_ticks)};
    }
    after = std::move(result.after);
    trade = std::move(result.trade);
    durable_packets = std::move(result.packets);
    durable_changes = std::move(result.source_changes);
  } else {
    // Standalone fixtures without the production adapter retain the legacy direct path.
    if (corpse) {
      claimed = true;
    } else if (doodad) {
      next_phase = choose_next_phase(*doodad_phase);
      claimed = nodes_->try_advance_doodad(
          node, *doodad_definition, pending_->doodad_phase, next_phase, server_ticks,
          doodad_next_tick(*doodad_definition, next_phase, server_ticks), &legacy_revision);
    } else {
      claimed = nodes_->try_deplete(node, server_ticks, respawn_tick(*placement, server_ticks),
                                    &legacy_revision);
    }
    if (!claimed) {
      throw GatheringError{kTargetDepleted, abandon_pending(server_ticks)};
    }
    bool committed{};
    try {
      committed = persistence_.commit(before, after, trade);
    } catch (const std::exception &error) {
      spdlog::error("Gathering reward commit failed: {}", error.what());
    }
    if (!committed) {
      if (!corpse) {
        nodes_->restore(node);
      }
      throw GatheringError{kGatheringFailed, abandon_pending(server_ticks)};
    }
  }

  pending_->executed = true;
  if (pending_->stamina_flow && vitals_.spend_stamina) {
    vitals_.spend_stamina(server_ticks);
  }

  GatheringOutcome outcome;
  outcome.packets = std::move(durable_packets);
  const auto old_key = before.stackables.find(kCliffsPrisonKey);
  const auto new_key = after.stackables.find(kCliffsPrisonKey);
  const bool consumed_key_stack = old_key != before.stackables.end() &&
                                  new_key != after.stackables.end() &&
                                  new_key->second < old_key->second;
  if (!rewards.empty() || worn_tool || !trade.removed_item_ids.empty() || consumed_key_stack) {
    Json stackables = Json::array();
    for (const auto &[item_cms_id, quantity] : rewards) {
      stackables.push_back(
          {{"item_cms_id", item_cms_id}, {"quantity", after.stackables.at(item_cms_id)}});
    }
    if (consumed_key_stack) {
      stackables.push_back({{"item_cms_id", kCliffsPrisonKey}, {"quantity", new_key->second}});
    }
    Json update{{"stackables", std::move(stackables)}, {"packet_seq", Json::object()}};
    if (worn_tool) {
      // Captured tool gathers report the worn tool row in the same ItemsUpdateNoti.
      update["items"] = Json::array({item_instance_json(*worn_tool)});
    }
    if (!trade.removed_item_ids.empty()) {
      update["items"] = Json::array({{{"item_id", std::to_string(trade.removed_item_ids.front())},
                                       {"item_cms_id", kCliffsPrisonKey}}});
    }
    if (doodad && doodad_phase->reason == DoodadRewardReason::Looting) {
      update["looting"] = {{"target_unit_id", std::to_string(doodad_unit_id(node.sector_id,
                                                                            node.index))}};
    } else {
      update["system"] = Json::object();
    }
    outcome.packets.push_back({kItemsUpdateNotification, std::move(update)});
  }

  if (worn_tool && vitals_.tool_worn) {
    auto follow_ups = vitals_.tool_worn(*worn_tool);
    outcome.packets.insert(outcome.packets.end(), std::make_move_iterator(follow_ups.begin()),
                           std::make_move_iterator(follow_ups.end()));
  }

  if (corpse) {
    // Captured order: ItemsUpdateNoti, the corpse's cleared UnitLootItemInfoNoti, then
    // InteractionAnimRes. The corpse is not a shared node, so nothing is broadcast.
    if (corpses_.butchered) {
      auto flags = corpses_.butchered(*pending_->corpse_unit_id, server_ticks);
      outcome.packets.insert(outcome.packets.end(), std::make_move_iterator(flags.begin()),
                             std::make_move_iterator(flags.end()));
    }
    outcome.packets.push_back(animation_result());
    outcome.observers.push_back(animation_result());
    return outcome;
  }

  if (doodad) {
    if (!persistence_.commit_durable) {
      quest_events_.push_back(
          {QuestEventKind::DoodadInteracted, doodad_definition->id, pending_->doodad_phase, 1});
    }
    if (doodad_phase->skill_effect_id && vitals_.apply_effect) {
      auto effect = vitals_.apply_effect(*doodad_phase->skill_effect_id, position, server_ticks);
      outcome.packets.insert(outcome.packets.end(), std::make_move_iterator(effect.begin()),
                             std::make_move_iterator(effect.end()));
    }
  }

  if (doodad) {
    // A portal moves the player once its interaction executed.
    outcome.travel = doodad_phase->map_move;
  }
  if (!persistence_.commit_durable) {
    GatheringStateChange change{node, false, std::nullopt, server_ticks, legacy_revision};
    if (doodad) {
      change.phase = next_phase;
      change.active = next_phase != kDespawnedDoodadPhase;
    }
    durable_changes.push_back(change);
  }
  auto state_packets = gathering_state_packets(durable_changes);
  outcome.packets.insert(outcome.packets.end(), state_packets.begin(), state_packets.end());
  outcome.packets.push_back(animation_result());
  outcome.observers.push_back(animation_result());
  outcome.broadcasts = std::move(durable_changes);
  return outcome;
}

GatheringOutcome GatheringSession::instant(const Json &request, std::uint32_t sequence,
                                           const WorldPosition &position,
                                           std::int64_t server_ticks, bool other_action_busy) {
  require(!pending_ || pending_expired(server_ticks), kActionInProgress);
  require(!other_action_busy, kActionInProgress);
  require(!vitals_.dead || !vitals_.dead(), kDead);

  const auto target = resolve_doodad(request, position);
  require(target.doodad->quest && target.doodad_phase->instant, kWrongInteraction);
  // InteractUnitReq does not carry a position. The server's tracked position remains
  // authoritative for the range check.
  require(within(position, target.position_mm, kGatheringRangeMm), kTooFar);

  const auto &phase = *target.doodad_phase;
  const auto rewards = roll_loot(phase.loot);
  const auto before = persistence_.load_progress();
  require(before.revision < std::numeric_limits<std::int64_t>::max(), kGatheringFailed);
  const bool cliffs_cannon = position.map_id == kCliffsMap &&
                             target.node.sector_id == kCliffsCannonSector &&
                             target.node.index == kCliffsCannonLocalId &&
                             target.doodad->id == kCliffsCannon;
  if (cliffs_cannon) {
    require(cliffs_cannon_allowed(before), kMissingCondition);
  }
  const bool cliffs_elite_object = position.map_id == kCliffsMap &&
                                   target.node.sector_id == kCliffsEliteSector &&
                                   target.node.index == kCliffsEliteObjectLocalId &&
                                   target.doodad->id == kCliffsEliteObject;
  if (cliffs_elite_object) {
    require(persistence_.conditions_met &&
                persistence_.conditions_met(cliffs_elite_object_conditions(), position),
            kMissingCondition);
  }
  auto after = before;
  after.revision = before.revision + 1;
  for (const auto &[item_cms_id, quantity] : rewards) {
    auto &stack = after.stackables[item_cms_id];
    require(stack <= std::numeric_limits<int>::max() - quantity, kGatheringFailed);
    stack += quantity;
  }

  const int next_phase = choose_next_phase(phase);
  std::uint64_t legacy_revision{};
  std::vector<GameplayPacket> durable_packets;
  std::vector<GatheringStateChange> durable_changes;
  if (persistence_.commit_durable) {
    const auto next_tick = doodad_next_tick(*target.doodad, next_phase, server_ticks);
    const auto delay = next_tick ? std::optional<std::int64_t>(*next_tick - server_ticks)
                                 : std::nullopt;
    std::optional<int> successor;
    if (const auto *timed = target.doodad->phase(next_phase);
        timed && !timed->interaction_id) {
      successor = choose_next_phase(*timed);
    } else if (next_phase == kDespawnedDoodadPhase && next_tick) {
      successor = target.doodad->initial_phase;
    }
    const auto reservation = nodes_->reserve_doodad(target.node, *target.doodad,
                                                     phase.phase_id, next_phase, delay);
    require(reservation.has_value(), kTargetDepleted);
    std::optional<GatheringNodeKey> linked_node;
    std::optional<std::uint64_t> linked_reservation;
    if (cliffs_cannon && phase.phase_id == 1) {
      linked_node = GatheringNodeKey{kCliffsMap, GatheringNodeKind::Doodad,
                                     kCliffsBarrierSector, kCliffsBarrierLocalId};
      try {
        linked_reservation = nodes_->reserve_cliffs_cannon_barrier(*linked_node);
      } catch (...) {
        if (!nodes_->finish_reservation(target.node, *reservation, false, server_ticks)) {
          std::terminate();
        }
        throw;
      }
      if (!linked_reservation) {
        if (!nodes_->finish_reservation(target.node, *reservation, false, server_ticks)) {
          std::terminate();
        }
        throw GatheringError{kTargetDepleted, {}};
      }
    }
    GatheringDurableResult result;
    bool handed_to_adapter = false;
    const auto cleanup_unregistered = [&] {
      if (handed_to_adapter) {
        return;
      }
      const bool released = linked_node
          ? nodes_->finish_reservation_pair(target.node, *reservation, *linked_node,
                                             *linked_reservation, false, server_ticks)
          : nodes_->finish_reservation(target.node, *reservation, false, server_ticks);
      if (!released) {
        std::terminate();
      }
    };
    try {
      GatheringDurableRequest action;
      action.node = target.node;
      action.reservation = reservation;
      action.linked_node = linked_node;
      action.linked_reservation = linked_reservation;
      action.nodes = nodes_;
      action.expected_phase = phase.phase_id;
      action.next_phase = next_phase;
      action.delay_ticks = delay;
      action.timed_successor_phase = successor;
      action.events.push_back({QuestEventKind::DoodadInteracted, target.doodad->id,
                               phase.phase_id, 1});
      action.position = position;
      action.before = before;
      action.after = after;
      handed_to_adapter = true;
      result = persistence_.commit_durable(action);
    } catch (const std::exception &error) {
      cleanup_unregistered();
      spdlog::error("Quest doodad commit setup failed: {}", error.what());
      throw GatheringError{kGatheringFailed, {}};
    } catch (...) {
      cleanup_unregistered();
      spdlog::error("Quest doodad commit setup failed with a nonstandard exception");
      throw GatheringError{kGatheringFailed, {}};
    }
    require(result.committed, kGatheringFailed);
    after = std::move(result.after);
    durable_packets = std::move(result.packets);
    durable_changes = std::move(result.source_changes);
  } else {
    const bool claimed = nodes_->try_advance_doodad(
        target.node, *target.doodad, phase.phase_id, next_phase, server_ticks,
        doodad_next_tick(*target.doodad, next_phase, server_ticks), &legacy_revision);
    require(claimed, kTargetDepleted);
    if (!rewards.empty()) {
      bool committed{};
      try {
        committed = persistence_.commit(before, after, ItemTrade{});
      } catch (const std::exception &error) {
        spdlog::error("Quest doodad reward commit failed: {}", error.what());
      }
      if (!committed) {
        nodes_->restore(target.node);
        throw GatheringError{kGatheringFailed, {}};
      }
    }
    quest_events_.push_back({QuestEventKind::DoodadInteracted, target.doodad->id, phase.phase_id, 1});
  }

  GatheringOutcome outcome;
  outcome.packets = std::move(durable_packets);
  const auto unit_id = doodad_unit_id(target.node.sector_id, target.node.index);
  if (!rewards.empty()) {
    Json stackables = Json::array();
    for (const auto &[item_cms_id, quantity] : rewards) {
      stackables.push_back(
          {{"item_cms_id", item_cms_id}, {"quantity", after.stackables.at(item_cms_id)}});
    }
    outcome.packets.push_back(
        {kItemsUpdateNotification,
         {{"stackables", std::move(stackables)},
          {"looting", {{"target_unit_id", std::to_string(unit_id)}}},
          {"packet_seq", Json::object()}}});
  }

  if (!persistence_.commit_durable) {
    durable_changes.push_back({target.node, next_phase != kDespawnedDoodadPhase, next_phase,
                               server_ticks, legacy_revision});
  }
  auto state_packets = gathering_state_packets({durable_changes.front()});
  outcome.packets.insert(outcome.packets.end(), state_packets.begin(), state_packets.end());
  outcome.packets.push_back({kInteractUnitResponse,
                             {{"target_unit_id", std::to_string(unit_id)},
                              {"interaction_id", *phase.interaction_id},
                              {"packet_seq", {{"seq", std::to_string(sequence)}}}}});
  outcome.broadcasts.push_back(durable_changes.front());
  outcome.travel = phase.map_move;
  if (cliffs_cannon && phase.phase_id == 1) {
    const GatheringNodeKey barrier{kCliffsMap, GatheringNodeKind::Doodad,
                                   kCliffsBarrierSector, kCliffsBarrierLocalId};
    std::uint64_t barrier_revision{};
    if (!persistence_.commit_durable) {
      nodes_->set_scripted_doodad_phase(barrier, kCliffsBarrierDoodad,
                                        kDespawnedDoodadPhase, server_ticks, &barrier_revision);
    }
    const GatheringStateChange opened = persistence_.commit_durable
        ? durable_changes.back()
        : GatheringStateChange{barrier, false, kDespawnedDoodadPhase, server_ticks,
                               barrier_revision};
    auto barrier_packets = gathering_state_packets({opened});
    outcome.packets.insert(outcome.packets.end(), barrier_packets.begin(), barrier_packets.end());
    outcome.broadcasts.push_back(opened);
    outcome.cut_scene_id = kCliffsCannonCutScene;
  }
  if (cliffs_elite_object) {
    outcome.cut_scene_id = kCliffsEliteObjectCutScene;
  }
  return outcome;
}

GatheringOutcome GatheringSession::end(const Json &request, std::int64_t server_ticks) {
  const auto &ended = request.at("end_interaction_anim");
  require(pending_ && pending_->animation_id == ended.at("interaction_anim_id").get<int>(),
          kAnimationMismatch);

  // Ending before execution cancels the action; nothing was claimed or granted.
  GatheringOutcome outcome;
  outcome.packets = abandon_pending(server_ticks);
  outcome.packets.push_back(animation_result());
  outcome.observers.push_back(animation_result());
  return outcome;
}

GatheringOutcome GatheringSession::interrupt(std::int64_t server_ticks) {
  GatheringOutcome outcome;
  if (!pending_) {
    return outcome;
  }
  outcome.packets = abandon_pending(server_ticks);
  outcome.packets.push_back(animation_result());
  outcome.observers.push_back(animation_result());
  return outcome;
}

std::vector<GameplayPacket> GatheringSession::abandon_pending(std::int64_t server_ticks) {
  std::vector<GameplayPacket> packets;
  if (pending_ && !pending_->executed && pending_->stamina_flow && vitals_.cancel_stamina) {
    if (auto stopped = vitals_.cancel_stamina(server_ticks)) {
      packets.push_back(std::move(*stopped));
    }
  }
  pending_.reset();
  return packets;
}

std::optional<CharacterItem>
GatheringSession::usable_tool(const GatheringAnimation &animation) const {
  if (animation.tool_equipment_type == "None") {
    return std::nullopt;
  }

  const auto character = persistence_.load_character();
  if (!character) {
    return std::nullopt;
  }

  const auto tool = std::ranges::find_if(character->items, [&](const auto &item) {
    return item.equip_slot && *item.equip_slot >= kFirstToolSlot &&
           *item.equip_slot <= kLastToolSlot && item.durability > 0 &&
           data_->is_tool(item.cms_id, animation.tool_equipment_type);
  });
  if (tool == character->items.end()) {
    return std::nullopt;
  }
  return *tool;
}

std::map<int, int> GatheringSession::roll_loot(const GatheringLoot &loot) {
  std::uniform_real_distribution<double> percent(0.0, kAlwaysPercent);
  const auto passes = [&](double rate_percent) {
    return rate_percent >= kAlwaysPercent || percent(random_) < rate_percent;
  };

  std::map<int, int> rewards;
  if (!passes(loot.pack_rate_percent)) {
    return rewards;
  }

  for (const auto &group : loot.groups) {
    if (!passes(group.rate_percent)) {
      continue;
    }

    std::vector<int> weights;
    for (const auto &item : group.items) {
      weights.push_back(item.weight);
    }
    std::discrete_distribution<std::size_t> choose(weights.begin(), weights.end());
    const auto &item = group.items.at(choose(random_));

    std::uniform_int_distribution<int> quantity(item.min_quantity, item.max_quantity);
    auto &total = rewards[item.item_cms_id];
    const auto rolled = quantity(random_);
    require(total <= std::numeric_limits<int>::max() - rolled, kGatheringFailed);
    total += rolled;
  }
  return rewards;
}

int GatheringSession::choose_next_phase(const DoodadPhase &phase) {
  std::vector<int> weights;
  for (const auto &choice : phase.next) {
    weights.push_back(choice.weight);
  }
  std::discrete_distribution<std::size_t> choose(weights.begin(), weights.end());
  return phase.next.at(choose(random_)).phase_id;
}

std::optional<std::int64_t> GatheringSession::doodad_next_tick(const DoodadDefinition &definition,
                                                               int next_phase,
                                                               std::int64_t server_ticks) {
  if (next_phase == kDespawnedDoodadPhase) {
    // Captured Roseberry Bush respawns took 60.1-60.7 s for its authored 60 s delay.
    std::uniform_int_distribution<int> delay(definition.respawn_delay_min_ms,
                                             definition.respawn_delay_max_ms);
    return add_milliseconds(server_ticks, delay(random_));
  }

  const auto *phase = definition.phase(next_phase);
  if (phase != nullptr && !phase->interaction_id) {
    return add_milliseconds(server_ticks, phase->duration_ms);
  }
  return std::nullopt;
}

std::int64_t GatheringSession::respawn_tick(const GatheringDefinition &definition,
                                            std::int64_t server_ticks) {
  // 116 captured respawns took 31.8-180.5 s, matching deactivate delay + 30-180 s.
  std::uniform_int_distribution<int> delay(definition.activate_delay_min_ms,
                                           definition.activate_delay_max_ms);
  const auto total_ms = static_cast<std::int64_t>(definition.deactivate_delay_ms) + delay(random_);
  return add_milliseconds(server_ticks, total_ms);
}

bool GatheringSession::pending_expired(std::int64_t server_ticks) const {
  return pending_ && server_ticks > pending_->started_ticks + kActionTimeoutTicks;
}

bool GatheringSession::within_request_limit(std::int64_t server_ticks) {
  if (server_ticks - request_window_ticks_ >= kTicksPerSecond) {
    request_window_ticks_ = server_ticks;
    requests_in_window_ = 0;
  }
  ++requests_in_window_;
  return requests_in_window_ <= kMaximumRequestsPerSecond;
}

GameplayPacket GatheringSession::animation_result() const {
  return {kInteractionAnimResult, {{"unit_id", std::to_string(player_unit_id_)}}};
}

namespace {

class OrderedGatheringDelivery final {
public:
  explicit OrderedGatheringDelivery(std::shared_ptr<GatheringNodes> nodes)
      : nodes_(std::move(nodes)), lock_(nodes_->publication_mutex()) {
  }

private:
  std::shared_ptr<GatheringNodes> nodes_;
  std::unique_lock<std::recursive_mutex> lock_;
};

std::shared_ptr<void> begin_gathering_delivery(const std::weak_ptr<GatheringNodes> &weak_nodes) {
  auto nodes = weak_nodes.lock();
  if (!nodes) {
    return {};
  }
  return std::make_shared<OrderedGatheringDelivery>(std::move(nodes));
}

class GatheringDeltaPlan final : public LocalPacketPlan {
public:
  GatheringDeltaPlan(std::weak_ptr<GatheringNodes> nodes, int map_id, std::uint64_t channel,
                     std::vector<GatheringStateChange> changes, bool doodad, bool active)
      : nodes_(std::move(nodes)), map_id_(map_id), channel_(channel),
        changes_(std::move(changes)), doodad_(doodad), active_(active) {
    if (changes_.empty() || changes_.size() > kMaximumSectorNodes) {
      throw std::runtime_error("Gathering delta plan exceeds its node limit");
    }
    const auto sector = changes_.front().node.sector_id;
    if (std::ranges::any_of(changes_, [&](const auto &change) {
          return change.node.map_id != map_id_ || change.node.sector_id != sector;
        })) {
      throw std::invalid_argument("Gathering delta plan crosses source sectors");
    }
  }

  std::size_t retained_bytes() const noexcept override {
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    if (changes_.capacity() > (maximum - sizeof(*this)) / sizeof(GatheringStateChange)) {
      return maximum;
    }
    return sizeof(*this) + changes_.capacity() * sizeof(GatheringStateChange);
  }

  std::shared_ptr<void> begin_ordered_delivery() const override {
    return begin_gathering_delivery(nodes_);
  }

  int expected_map() const noexcept override {
    return map_id_;
  }

  std::uint64_t expected_channel() const noexcept override {
    return channel_;
  }

  std::optional<std::uint32_t> source_sector() const noexcept override {
    return changes_.front().node.sector_id;
  }

  LocalPacketDecision prepare(nlohmann::json &message) const override {
    const auto nodes = nodes_.lock();
    if (!nodes) {
      return LocalPacketDecision::drop;
    }
    if (doodad_) {
      return nodes->is_current_revision(changes_.front().node, changes_.front().revision)
          ? LocalPacketDecision::keep : LocalPacketDecision::drop;
    }
    const char *field = active_ ? "activated_indices" : "deactivated_indices";
    if (!message.contains(field) || !message.at(field).is_array()) {
      throw std::runtime_error("Invalid gathering state payload during revision validation");
    }
    std::map<std::uint32_t, bool> owned;
    for (const auto &change : changes_) {
      owned[change.node.index] = nodes->is_current_revision(change.node, change.revision);
    }
    Json current = Json::array();
    bool changed = false;
    for (const auto &value : message.at(field)) {
      if (!value.is_number_integer()) {
        throw std::runtime_error("Invalid gathering index during revision validation");
      }
      const auto index = value.get<std::int64_t>();
      const auto found = index >= 0 && index <= std::numeric_limits<std::uint32_t>::max()
          ? owned.find(static_cast<std::uint32_t>(index)) : owned.end();
      if (found != owned.end() && !found->second) {
        changed = true;
      } else {
        current.push_back(value);
      }
    }
    if (!changed) {
      return LocalPacketDecision::keep;
    }
    if (current.empty()) {
      message.erase(field);
      return message.size() <= 1 ? LocalPacketDecision::drop : LocalPacketDecision::replace;
    }
    message[field] = std::move(current);
    return LocalPacketDecision::replace;
  }

private:
  std::weak_ptr<GatheringNodes> nodes_;
  int map_id_{};
  std::uint64_t channel_{};
  std::vector<GatheringStateChange> changes_;
  bool doodad_{};
  bool active_{};
};

class GatheringSectorPlan final : public LocalPacketPlan {
public:
  GatheringSectorPlan(std::weak_ptr<GatheringNodes> nodes, int map_id, std::uint64_t channel,
                      std::uint32_t sector_id, const std::vector<GatheringStateChange> &original)
      : nodes_(std::move(nodes)), map_id_(map_id), channel_(channel), sector_id_(sector_id) {
    if (original.size() > kMaximumSectorNodes) {
      throw std::runtime_error("Gathering sector plan exceeds its node limit");
    }
    for (const auto &change : original) {
      owned_doodad_ids_.insert(change.node.index);
    }
  }

  std::size_t retained_bytes() const noexcept override {
    // Allow ample space for each allocator-backed tree node, including its bookkeeping.
    constexpr std::size_t kBytesPerTreeNode = 128;
    return sizeof(*this) + owned_doodad_ids_.size() * kBytesPerTreeNode;
  }

  std::shared_ptr<void> begin_ordered_delivery() const override {
    return begin_gathering_delivery(nodes_);
  }

  int expected_map() const noexcept override {
    return map_id_;
  }

  std::uint64_t expected_channel() const noexcept override {
    return channel_;
  }

  std::optional<std::uint32_t> initial_sector() const noexcept override {
    return sector_id_;
  }

  std::optional<std::uint32_t> source_sector() const noexcept override {
    return sector_id_;
  }

  LocalPacketDecision prepare(nlohmann::json &message) const override {
    const auto nodes = nodes_.lock();
    if (!nodes) {
      return LocalPacketDecision::drop;
    }
    const auto [placements, doodads] = nodes->sector_state(map_id_, sector_id_);
    message.erase("gathering_deactivated_indices");
    if (!placements.empty()) {
      auto indices = Json::array();
      for (const auto &change : placements) {
        indices.push_back(change.node.index);
      }
      message["gathering_deactivated_indices"] = std::move(indices);
    }
    auto owned = owned_doodad_ids_;
    for (const auto &change : doodads) {
      if (!owned.contains(change.node.index) && owned.size() >= kMaximumSectorNodes) {
        throw std::runtime_error("Gathering sector doodad identity limit exceeded");
      }
      owned.insert(change.node.index);
    }
    auto merged = Json::array();
    if (message.contains("doodads")) {
      if (!message.at("doodads").is_array() || message.at("doodads").size() > kMaximumSectorNodes) {
        throw std::runtime_error("Invalid or oversized sector doodad payload during reconstruction");
      }
      for (const auto &nub : message.at("doodads")) {
        if (!nub.is_object() || !nub.contains("id") || !nub.at("id").is_number_integer()) {
          throw std::runtime_error("Invalid sector doodad identity during reconstruction");
        }
        const auto id = nub.at("id").get<std::int64_t>();
        if (id < 0 || id > std::numeric_limits<std::uint32_t>::max() ||
            !owned.contains(static_cast<std::uint32_t>(id))) {
          if (merged.size() >= kMaximumSectorNodes) {
            throw std::runtime_error("Reconstructed sector doodads exceed the node limit");
          }
          merged.push_back(nub);
        }
      }
    }
    const auto added_doodads = std::ranges::count_if(doodads, [](const auto &change) {
      return change.node.kind == GatheringNodeKind::Doodad && change.phase.has_value();
    });
    if (static_cast<std::size_t>(added_doodads) > kMaximumSectorNodes - merged.size()) {
      throw std::runtime_error("Reconstructed sector doodads exceed the node limit");
    }
    for (auto &nub : sector_doodad_nubs(doodads)) {
      merged.push_back(std::move(nub));
    }
    message.erase("doodads");
    if (!merged.empty()) {
      message["doodads"] = std::move(merged);
    }
    return LocalPacketDecision::replace;
  }

private:
  std::weak_ptr<GatheringNodes> nodes_;
  int map_id_{};
  std::uint64_t channel_{};
  std::uint32_t sector_id_{};
  std::set<std::uint32_t> owned_doodad_ids_;
};

} // namespace

std::vector<GameplayPacket> gathering_state_packets(
    const std::vector<GatheringStateChange> &changes, std::shared_ptr<GatheringNodes> nodes,
    std::uint64_t channel) {
  using SectorState = std::tuple<int, std::uint32_t, bool>;
  std::map<SectorState, std::vector<GatheringStateChange>> indices_by_sector;
  std::vector<GameplayPacket> doodad_packets;
  for (const auto &change : changes) {
    if (change.node.kind == GatheringNodeKind::Doodad) {
      const auto unit_id = doodad_unit_id(change.node.sector_id, change.node.index);
      Json message{{"unit_id", std::to_string(unit_id)},
                   {"phase_begin_tick", wire_monotonic_tick(change.tick)}};
      if (change.phase) {
        message["phase_id"] = *change.phase;
      }
      GameplayPacket packet{kDoodadPhaseChangedNotification, std::move(message)};
      if (nodes) {
        packet.local_plan = std::make_shared<GatheringDeltaPlan>(
            nodes, change.node.map_id, channel, std::vector<GatheringStateChange>{change}, true,
            change.active);
      }
      doodad_packets.push_back(std::move(packet));
      continue;
    }
    indices_by_sector[{change.node.map_id, change.node.sector_id, change.active}].push_back(change);
  }

  std::vector<GameplayPacket> packets;
  for (auto &[sector_state, entries] : indices_by_sector) {
    const auto &[map_id, sector_id, active] = sector_state;
    const char *field = active ? "activated_indices" : "deactivated_indices";
    Json indices = Json::array();
    for (const auto &change : entries) {
      indices.push_back(change.node.index);
    }
    GameplayPacket packet{kGatheringStateChangedNotification,
                          {{"sector_id", sector_id}, {field, std::move(indices)}}};
    if (nodes) {
      packet.local_plan = std::make_shared<GatheringDeltaPlan>(nodes, map_id, channel, entries,
                                                               false, active);
    }
    packets.push_back(std::move(packet));
  }
  packets.insert(packets.end(), std::make_move_iterator(doodad_packets.begin()),
                 std::make_move_iterator(doodad_packets.end()));
  return packets;
}

std::shared_ptr<const LocalPacketPlan> gathering_sector_plan(
    std::shared_ptr<GatheringNodes> nodes, int map_id, std::uint64_t channel,
    std::uint32_t sector_id, const std::vector<GatheringStateChange> &original_doodads) {
  return std::make_shared<GatheringSectorPlan>(std::move(nodes), map_id, channel, sector_id,
                                               original_doodads);
}

nlohmann::json sector_doodad_nubs(const std::vector<GatheringStateChange> &doodads) {
  Json nubs = Json::array();
  for (const auto &change : doodads) {
    if (change.node.kind != GatheringNodeKind::Doodad || !change.phase) {
      continue;
    }
    Json nub{{"id", change.node.index}, {"phase_begin_tick", wire_monotonic_tick(change.tick)}};
    if (*change.phase != kDespawnedDoodadPhase) {
      nub["phase"] = *change.phase;
    }
    nubs.push_back(std::move(nub));
  }
  return nubs;
}

} // namespace chronicles
