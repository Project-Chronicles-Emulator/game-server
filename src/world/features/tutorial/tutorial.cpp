#include "world/features/tutorial/tutorial.hpp"
#include "cms.generated.pb.h"
#include "common/clock_ticks.hpp"
#include "common/currency.hpp"
#include "persistence/item_wire.hpp"
#include "world/core/world_request_opcodes.hpp"
#include "world/features/character/progression.hpp"
#include "world/features/inventory/utility_bag.hpp"
#include "world/features/quests/quest_rules.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace chronicles {
namespace {

using Json = nlohmann::json;
using ActionError = quest_rules::RuleError;
using quest_rules::distance_squared;
using quest_rules::grant_experience;
using quest_rules::grant_rewards;
using quest_rules::kInteractionRangeMm;
using quest_rules::kPositionToleranceMm;
using quest_rules::objective_ready;
using quest_rules::objectives_complete;
using quest_rules::position_within_trigger;
using quest_rules::quest_json;
using quest_rules::quest_progress_update_json;
using quest_rules::request_position_matches;
using quest_rules::require;

constexpr int kTutorialMap = 8000;
constexpr double kQuestKillCreditRangeMm = 100000;
constexpr std::int64_t kTicksPerSecond = 10000000;
constexpr std::int64_t kTicksPerMillisecond = kTicksPerSecond / 1000;
constexpr int kPotionTimingToleranceMs = 200;
constexpr std::int64_t kMinimumInteractionTicks = kTicksPerSecond / 10;
constexpr std::int64_t kInteractionTimeoutTicks = 30 * kTicksPerSecond;
constexpr int kInteractionTimingToleranceMs = 100;

enum PacketOpcode : std::uint16_t {
  kCommonError = 2,
  kUnitAdded = 10,
  kSkillStarted = 14,
  kSkillEnded = 15,
  kBossKilledBanner = 25,
  kQuestUpdate = 27,
  kQuestAccepted = 28,
  kCharacterBaseChanged = 36,
  kSkillCost = 53,
  kVisibleChannel = 67,
  kCutScenePlay = 70,
  kAdventureObjectsUpdated = 90,
  kAdventureObjectActivated = 91,
  kUtilityBagSet = 104,
  kSetNpcAiPhase = 114,
  kWorldMoney = 126,
  kWorldAdventureObjects = 127,
  kWorldItems = 130,
  kWorldQuests = 136,
  kWorldUtilityBag = 140,
  kUnitInteractionResult = 163,
  kDialogueStarted = 161,
  kDialogueEnded = 162,
  kUnitInteracted = 164,
  kStartInteractionAnimationResult = 165,
  kInteractionAnimationStarted = 166,
  kInteractionAnimationResult = 167,
  kItemsUpdated = 169,
  kTeleportRequest = 181,
  kSectorUpdate = 193,
  kDoodadPhaseChanged = 194,
  kAcceptQuest = world_request::kAcceptQuestRequest,
  kStartSkill = world_request::kStartSkillRequest,
  kExecuteSkill = world_request::kSkillEffectReportRequest,
  kClaimQuestReward = world_request::kClaimQuestRewardRequest,
  kProgressQuest = world_request::kProgressQuestRequest,
  kDiscoverObject = world_request::kDiscoverAdventureObjectRequest,
  kCutSceneEnd = 20055,
  kInteractUnit = world_request::kInteractionUnitRequest,
  kStartDialogue = 20065,
  kEndDialogue = 20066,
  kStartInteraction = world_request::kInteractionStartRequest,
  kExecuteInteraction = 20070,
  kEndInteraction = 20071,
  kSetUtilityBag = 20080,
  kLevelTrigger = 20084,
  kTeleportStart = 20088,
  kTeleportCompletion = 20089,
};

// Candidate: VisibleChannelNoti carries a channel mask. The official tutorial capture
// switches the player to 6 while an invisible cutscene plays and back to 3 afterward.
constexpr std::uint64_t kCutSceneVisibleChannel = 6;
constexpr std::uint64_t kDefaultVisibleChannel = 3;
constexpr std::size_t kMaximumLevelTriggers = 256;
constexpr std::size_t kMaximumCutScenes = 64;
constexpr std::size_t kMaximumBossDefeats = 16;

// Instance rewards (equipment) grant one item row per unit; this bounds the rows one adventure
// object adds in a single storage commit.
constexpr int kMaximumInstanceRewardQuantity = 16;
constexpr char kItemGradePrefix[] = "EX5ItemGrade_";
// Every captured tutorial interaction animation reports the sheathed weapon state.
constexpr char kSheathedWeaponState[] = "EX5WeaponStateType_Sheathed";

std::uint64_t parse_unit_id(const Json &value) {
  const auto text = value.get<std::string>();
  std::uint64_t result{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || result == 0 ||
      result > std::numeric_limits<std::int64_t>::max()) {
    throw std::invalid_argument("Invalid static unit identifier");
  }
  return result;
}

Json position_json(const WorldPosition &position) {
  return {{"millimeter_x", position.x_mm},
          {"millimeter_y", position.y_mm},
          {"millimeter_z", position.z_mm}};
}

bool matches_trigger(const Json &request, const Json &trigger, const WorldPosition &position,
                     const char *field) {
  if (!request.contains(field) || !request_position_matches(request, position)) {
    return false;
  }
  const auto &wire = request.at(field);
  if (wire.value("trigger_volume_x", "") != trigger.at("guid").at(0).get<std::string>() ||
      wire.value("trigger_volume_y", "") != trigger.at("guid").at(1).get<std::string>()) {
    return false;
  }
  if (!position_within_trigger(trigger, position, kPositionToleranceMm)) {
    return false;
  }
  // Enter and leave events share the GUID, but a leave must be near the
  // boundary. The old inside-only check accepted leaving from the center.
  return std::string_view(field) != "trigger_volume_leave" ||
         !position_within_trigger(trigger, position, -kPositionToleranceMm);
}

// Level-trigger rows use the shared grouped condition rules over this profile's quests.
bool level_trigger_conditions_pass(const TutorialData &data, const Json &conditions,
                                   const WorldProgress &state) {
  const quest_rules::QuestLookup lookup = [&data](int quest_id) {
    return data.quest(quest_id);
  };
  return quest_rules::grouped_conditions_pass(conditions, [&](const Json &condition) {
    return quest_rules::quest_requirement_met(lookup, condition, state);
  });
}

std::string wire_guid_key(const Json &guid) {
  return guid.at(0).get<std::string>() + ":" + guid.at(1).get<std::string>();
}

WorldPosition definition_position(int map_id, const Json &definition) {
  const auto &point = definition.at("position");
  return WorldPosition{map_id, point.at(0).get<int>(), point.at(1).get<int>(),
                       point.at(2).get<int>(), definition.at("yaw").get<float>()};
}

void validate_point(const Json &point) {
  if (!point.is_array() || point.size() != 3) {
    throw std::runtime_error("Tutorial position must have three coordinates");
  }
  for (const auto &coordinate : point) {
    if (!coordinate.is_number_integer() ||
        coordinate.get<std::int64_t>() < std::numeric_limits<int>::min() ||
        coordinate.get<std::int64_t>() > std::numeric_limits<int>::max()) {
      throw std::runtime_error("Tutorial coordinate is outside millimeter range");
    }
  }
}

// x5.cms.EX5ItemGrade value for a catalog grade name such as "Normal".
int parse_item_grade(const std::string &grade_name) {
  x5::cms::EX5ItemGrade grade;
  if (!x5::cms::EX5ItemGrade_Parse(kItemGradePrefix + grade_name, &grade)) {
    throw std::runtime_error("Unknown tutorial item grade: " + grade_name);
  }
  return static_cast<int>(grade);
}

// An equipment reward is granted as item instances rather than as a WorldProgress stack.
void validate_instance_reward(const Json &reward) {
  const auto &instance = reward.at("instance");
  const int quantity = reward.at("quantity");
  if (reward.at("item_cms_id").get<int>() <= 0 || quantity > kMaximumInstanceRewardQuantity ||
      instance.at("max_durability").get<int>() <= 0) {
    throw std::runtime_error("Invalid tutorial instance reward definition");
  }
  parse_item_grade(instance.at("grade").get<std::string>());
}

// Only adventure objects grant instances; quest rewards and doodad costs remain stacks.
void validate_rewards(const Json &rewards, bool allow_instances = false) {
  if (!rewards.is_array() || rewards.size() > 16) {
    throw std::runtime_error("Tutorial reward count exceeds limit");
  }
  for (const auto &reward : rewards) {
    if (reward.at("item_cms_id").get<int>() < 0 || reward.at("quantity").get<int>() <= 0 ||
        reward.at("quantity").get<int>() > 1000000) {
      throw std::runtime_error("Invalid tutorial reward definition");
    }
    if (!reward.contains("instance")) {
      continue;
    }
    if (!allow_instances) {
      throw std::runtime_error("Tutorial instance rewards are limited to adventure objects");
    }
    validate_instance_reward(reward);
  }
}

// Unequipped item rows for one instance reward. Storage assigns their slots and identifiers.
void append_instance_rewards(const Json &reward, std::vector<CharacterItem> &granted) {
  const auto &instance = reward.at("instance");
  const int quantity = reward.at("quantity");
  for (int unit = 0; unit < quantity; ++unit) {
    CharacterItem item;
    item.cms_id = reward.at("item_cms_id");
    item.durability = instance.at("max_durability");
    item.grade = parse_item_grade(instance.at("grade").get<std::string>());
    granted.push_back(item);
  }
}

// Fills the adventure-object ItemsUpdateNoti with the committed item rows. The official chest
// update lists the new instance with its storage id, grade, durability and creation time.
void attach_granted_items(std::vector<GameplayPacket> &packets,
                          const std::vector<CharacterItem> &stored) {
  const auto update = std::ranges::find_if(packets, [](const GameplayPacket &packet) {
    return packet.opcode == kItemsUpdated && packet.message.contains("adventure_object_activate");
  });
  if (update == packets.end()) {
    throw std::logic_error("Granted adventure-object items have no ItemsUpdateNoti");
  }

  Json items = Json::array();
  for (const auto &item : stored) {
    items.push_back(item_instance_json(item));
  }
  update->message["items"] = std::move(items);
}

// Reply to a rejected request. A rejected StartInteractionAnimReq must be answered with
// StartInteractionAnimRes (the captured rejection shape); the client waits for that response
// and stays locked in its pending animation when it receives CommonErrorRes instead.
std::vector<GameplayPacket> rejection_packets(std::uint16_t opcode, const std::string &code,
                                              std::uint32_t sequence) {
  const Json packet_seq{{"seq", std::to_string(sequence)}};
  if (opcode == kStartInteraction) {
    return {{kStartInteractionAnimationResult,
             {{"ec", code},
              {"weapon_state_type", kSheathedWeaponState},
              {"packet_seq", packet_seq}}}};
  }
  return {{kCommonError, {{"ec", code}, {"packet_seq", packet_seq}}}};
}

void validate_wire_guid(const Json &guid) {
  if (!guid.is_array() || guid.size() != 2) {
    throw std::runtime_error("Invalid tutorial GUID");
  }
  for (const auto &part : guid) {
    const auto text = part.get<std::string>();
    std::uint64_t number{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
      throw std::runtime_error("Invalid tutorial GUID component");
    }
  }
}

void validate_trigger(const Json &trigger) {
  validate_point(trigger.at("center"));
  validate_wire_guid(trigger.at("guid"));
  if (trigger.contains("radius")) {
    if (trigger.at("radius").get<int>() <= 0) {
      throw std::runtime_error("Invalid tutorial trigger radius");
    }
  } else {
    validate_point(trigger.at("extent"));
    for (const auto &extent : trigger.at("extent")) {
      if (extent.get<int>() <= 0) {
        throw std::runtime_error("Invalid tutorial trigger extent");
      }
    }
    if (!std::isfinite(trigger.at("yaw").get<double>())) {
      throw std::runtime_error("Invalid tutorial trigger rotation");
    }
  }
}

void validate_doodad_action(const TutorialData &data, const Json &doodad,
                            const WorldProgress &state) {
  if (doodad.contains("phase_prerequisite")) {
    const auto &prerequisite = doodad.at("phase_prerequisite");
    const auto prerequisite_quest = state.quests.find(prerequisite.at("quest_id"));
    require(prerequisite_quest != state.quests.end(), "Interaction_ConditionsNotMatch");
    const auto sequence = prerequisite.at("sequence").get<int>();
    require(prerequisite_quest->second.complete ||
                (prerequisite_quest->second.objectives.contains(sequence) &&
                 prerequisite_quest->second.objectives.at(sequence) > 0),
            "Interaction_ConditionsNotMatch");
  }
  const int quest_id = doodad.at("quest_id");
  const int sequence = doodad.at("quest_sequence");
  require(state.quests.contains(quest_id) && !state.quests.at(quest_id).complete,
          "Quest_IsNotInProgress");
  const auto &quest = state.quests.at(quest_id);
  const auto &definition = *data.quest(quest_id);
  const auto objective =
      std::ranges::find_if(definition.at("progresses"), [sequence](const auto &entry) {
        return entry.at("sequence") == sequence;
      });
  require(objective != definition.at("progresses").end() &&
              objective_ready(definition, quest, objective->at("group")),
          "Quest_IsNotProgress");
  for (const auto &item : doodad.at("consumes")) {
    const int cms_id = item.at("item_cms_id");
    require(state.stackables.contains(cms_id) &&
                state.stackables.at(cms_id) >= item.at("quantity").get<int>(),
            "Interaction_ConditionsNotMatch");
  }
}

const Json *find_definition(const auto &index, const auto &key) {
  const auto found = index.find(key);
  return found == index.end() ? nullptr : &found->second;
}

void validate_level_trigger_task(const TutorialData &data, const Json &task) {
  const auto kind = task.at("kind").get<std::string>();
  if (kind == "PlayCutScene") {
    if (task.at("cut_scene_id").get<int>() <= 0) {
      throw std::runtime_error("Invalid level trigger cutscene");
    }
    return;
  }
  if (kind == "ChangeDoodadPhase") {
    const auto *doodad = data.doodad(parse_unit_id(task.at("unit_id")));
    const int phase = task.at("phase");
    if (!doodad || !doodad->contains("phases") ||
        !std::ranges::any_of(doodad->at("phases"),
                             [phase](const auto &entry) { return entry == phase; })) {
      throw std::runtime_error("Invalid level trigger doodad phase");
    }
    return;
  }
  throw std::runtime_error("Unsupported level trigger task");
}

void validate_level_trigger_condition(const Json &condition) {
  static const std::unordered_set<std::string> requirements{
      "Acceptable", "Progressing", "QuestComplete", "QuestProgress"};
  const auto operation = condition.at("operator").get<std::string>();
  if (condition.at("group").get<int>() <= 0 || condition.at("quest_id").get<int>() <= 0 ||
      (operation != "AND" && operation != "OR") ||
      !requirements.contains(condition.at("requirement").get<std::string>()) ||
      (condition.contains("progress_group") && condition.at("progress_group").get<int>() <= 0)) {
    throw std::runtime_error("Invalid level trigger condition");
  }
}

void load_level_triggers(const Json &document, TutorialData &data) {
  if (!document.contains("level_triggers")) {
    return;
  }

  const auto &triggers = document.at("level_triggers");
  if (!triggers.is_array() || triggers.size() > kMaximumLevelTriggers) {
    throw std::runtime_error("Tutorial level trigger count exceeds limit");
  }
  for (const auto &trigger : triggers) {
    validate_trigger(trigger);
    const auto type = trigger.at("trigger_type").get<std::string>();
    if ((type != "Enter" && type != "Leave") || !trigger.at("rules").is_array() ||
        trigger.at("rules").empty() || trigger.at("rules").size() > 8) {
      throw std::runtime_error("Invalid tutorial level trigger");
    }
    for (const auto &rule : trigger.at("rules")) {
      if (!rule.at("supported").is_boolean() || rule.at("tasks").size() > 8 ||
          rule.at("conditions").size() > 16) {
        throw std::runtime_error("Invalid tutorial level trigger rule");
      }
      if (!rule.at("supported").get<bool>()) {
        continue;
      }
      for (const auto &task : rule.at("tasks")) {
        validate_level_trigger_task(data, task);
      }
      for (const auto &condition : rule.at("conditions")) {
        validate_level_trigger_condition(condition);
      }
    }
    if (!data.level_triggers.emplace(wire_guid_key(trigger.at("guid")), trigger).second) {
      throw std::runtime_error("Duplicate tutorial level trigger");
    }
  }
}

void load_cut_scenes(const Json &document, TutorialData &data) {
  if (!document.contains("cut_scenes")) {
    return;
  }

  const auto &cut_scenes = document.at("cut_scenes");
  if (!cut_scenes.is_array() || cut_scenes.size() > kMaximumCutScenes) {
    throw std::runtime_error("Tutorial cutscene count exceeds limit");
  }
  for (const auto &cut_scene : cut_scenes) {
    const int cut_scene_id = cut_scene.at("cut_scene_id");
    if (cut_scene_id <= 0 || !cut_scene.at("invisible").is_boolean() ||
        !cut_scene.at("supported").is_boolean() || cut_scene.at("npc_bindings").size() > 8) {
      throw std::runtime_error("Invalid tutorial cutscene");
    }
    for (const auto &binding : cut_scene.at("npc_bindings")) {
      if (!data.npcs.contains(parse_unit_id(binding.at("unit_id"))) ||
          binding.at("ai_phase").get<int>() <= 0) {
        throw std::runtime_error("Tutorial cutscene binds an unknown NPC");
      }
    }
    if (cut_scene.contains("end_teleport")) {
      validate_point(cut_scene.at("end_teleport").at("position"));
      if (!std::isfinite(cut_scene.at("end_teleport").at("yaw").get<double>())) {
        throw std::runtime_error("Invalid tutorial cutscene teleport");
      }
    }
    if (cut_scene.contains("end_map_move") && cut_scene.at("supported").get<bool>()) {
      const auto &move = cut_scene.at("end_map_move");
      validate_point(move.at("position"));
      if (move.at("map_id").get<int>() <= 0 || !std::isfinite(move.at("yaw").get<double>())) {
        throw std::runtime_error("Invalid tutorial cutscene map move");
      }
    }
    if (!data.cut_scenes.emplace(cut_scene_id, cut_scene).second) {
      throw std::runtime_error("Duplicate tutorial cutscene");
    }
  }

  // A supported rule may only play cutscenes whose follow-up this profile can finish.
  for (const auto &[key, trigger] : data.level_triggers) {
    for (const auto &rule : trigger.at("rules")) {
      if (!rule.at("supported").get<bool>()) {
        continue;
      }
      for (const auto &task : rule.at("tasks")) {
        if (task.at("kind") != "PlayCutScene") {
          continue;
        }
        const auto *cut_scene = data.cut_scene(task.at("cut_scene_id").get<int>());
        if (!cut_scene || !cut_scene->at("supported").get<bool>()) {
          throw std::runtime_error("Tutorial level trigger plays an unsupported cutscene");
        }
      }
    }
  }
}

void load_boss_defeats(const Json &document, TutorialData &data) {
  if (!document.contains("boss_defeats")) {
    return;
  }

  const auto &defeats = document.at("boss_defeats");
  if (!defeats.is_array() || defeats.size() > kMaximumBossDefeats) {
    throw std::runtime_error("Tutorial boss defeat count exceeds limit");
  }
  for (const auto &defeat : defeats) {
    const int npc_cms_id = defeat.at("npc_cms_id");
    const auto *cut_scene = data.cut_scene(defeat.at("cut_scene_id").get<int>());
    if (npc_cms_id <= 0 || !cut_scene || !cut_scene->at("supported").get<bool>()) {
      throw std::runtime_error("Invalid tutorial boss defeat script");
    }
    for (const auto &activation : defeat.at("doodad_activations")) {
      const auto *doodad = data.doodad(parse_unit_id(activation.at("unit_id")));
      if (!doodad || doodad->contains("quest_id")) {
        throw std::runtime_error("Tutorial boss defeat activates an unknown doodad");
      }
    }
    if (!data.boss_defeats.emplace(npc_cms_id, defeat).second) {
      throw std::runtime_error("Duplicate tutorial boss defeat script");
    }
  }
}

} // namespace

std::shared_ptr<const TutorialData> TutorialData::load(const std::filesystem::path &path) {
  // Recovered action stages for all four classes share this startup profile.
  // CombatData separately bounds skill, layer, stage, and event counts.
  if (std::filesystem::file_size(path) > 8 * 1024 * 1024) {
    throw std::runtime_error("Tutorial definition file exceeds size limit");
  }
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Cannot open tutorial definitions");
  }
  Json document;
  input >> document;
  return std::make_shared<const TutorialData>(std::move(document));
}

TutorialData::TutorialData(Json source) : document(std::move(source)) {
  const bool shared = document.value("profile_scope", "tutorial") == "shared_adventure";
  if (shared && (document.at("map_id").get<int>() <= 0 || !document.at("quests").empty() ||
                 !document.at("doodads").empty() || !document.at("civilians").empty() ||
                 document.contains("npc_spawns") || document.contains("level_triggers") ||
                 document.contains("cut_scenes"))) {
    throw std::runtime_error("Shared adventure profiles must contain only placed objects");
  }
  if (document.at("format_version") != 1 ||
      (!shared && document.at("map_id") != kTutorialMap && document.at("map_id") != 10117) ||
      document.at("build_id") != "40367e24241c9481bfc07fbf9ef1919f25fb0eaa" ||
      document.at("objects").size() > (shared ? 4096u : 128u) ||
      document.at("quests").size() > 64 || document.at("doodads").size() > 128 ||
      document.at("sectors").size() > 128) {
    throw std::runtime_error("Unsupported tutorial definition version or size");
  }
  for (const auto &object : document.at("objects")) {
    const auto unit_id = parse_unit_id(object.at("unit_id"));
    validate_point(object.at("position"));
    validate_rewards(object.at("rewards"), true);
    if (object.at("position").size() != 3 || object.at("cms_id").get<int>() <= 0 ||
        object.at("interaction_id").get<int>() <= 0 || object.at("rewards").size() > 16 ||
        !objects.emplace(unit_id, object).second) {
      throw std::runtime_error("Invalid or duplicate tutorial object");
    }
    if (object.value("experience", std::int64_t{0}) < 0) {
      throw std::runtime_error("Invalid tutorial adventure experience");
    }
    if (object.contains("build_points")) {
      const auto &points = object.at("build_points");
      if (!points.is_array() || points.size() > 2) {
        throw std::runtime_error("Invalid adventure build-point rewards");
      }
      for (const auto &entry : points) {
        const int type = entry.at("type");
        const int value = entry.at("value");
        if ((type != kLevelUpPointType && type != kWeaponPointType) || value <= 0 || value > 1000) {
          throw std::runtime_error("Invalid adventure build-point reward");
        }
      }
    }
    if (object.contains("animation_id") &&
        (object.at("animation_id").get<int>() <= 0 ||
         object.at("execute_time_ms").get<int>() <= 0 ||
         object.at("duration_ms").get<int>() <= object.at("execute_time_ms").get<int>() ||
         object.at("duration_ms").get<int>() > 30000)) {
      throw std::runtime_error("Invalid tutorial adventure animation");
    }
  }
  for (const auto &doodad : document.at("doodads")) {
    const auto unit_id = parse_unit_id(doodad.at("unit_id"));
    validate_point(doodad.at("position"));
    validate_rewards(doodad.at("consumes"));
    if (doodad.at("position").size() != 3 || !doodads.emplace(unit_id, doodad).second ||
        objects.contains(unit_id)) {
      throw std::runtime_error("Invalid or duplicate tutorial doodad");
    }
  }
  for (const auto &quest : document.at("quests")) {
    validate_rewards(quest.at("rewards"));
    if (quest.contains("supplies")) {
      validate_rewards(quest.at("supplies"));
    }
    if (quest.at("cms_id").get<int>() <= 0 || quest.at("experience").get<std::int64_t>() < 0 ||
        !quest.at("auto_complete").is_boolean() || !quest.at("sequential").is_boolean() ||
        !quest.at("progresses").is_array() || quest.at("progresses").size() > 64 ||
        !quest.at("accept_triggers").is_array() || quest.at("accept_triggers").size() > 8) {
      throw std::runtime_error("Invalid tutorial quest definition");
    }
    if (quest.contains("accept_npcs")) {
      const auto &npc_ids = quest.at("accept_npcs");
      if (!npc_ids.is_array() || npc_ids.size() > 8) {
        throw std::runtime_error("Invalid tutorial quest NPC acceptance");
      }
      for (const auto &npc_id : npc_ids) {
        if (!npc_id.is_number_integer() || npc_id.get<int>() <= 0) {
          throw std::runtime_error("Invalid tutorial quest NPC identifier");
        }
      }
    }
    for (const auto &trigger : quest.at("accept_triggers")) {
      validate_trigger(trigger);
      if (trigger.contains("request_field") &&
          trigger.at("request_field") != "trigger_volume_enter" &&
          trigger.at("request_field") != "unexpect_trigger_volume_enter") {
        throw std::runtime_error("Invalid tutorial quest acceptance trigger kind");
      }
    }
    std::unordered_set<int> sequences;
    for (const auto &objective : quest.at("progresses")) {
      const int sequence = objective.at("sequence");
      if (sequence <= 0 || objective.at("group").get<int>() <= 0 ||
          objective.at("parameters").value("count", 1) <= 0 ||
          objective.at("parameters").value("count", 1) > 4096 ||
          !sequences.insert(sequence).second) {
        throw std::runtime_error("Invalid tutorial quest objective");
      }
      if (objective.contains("trigger")) {
        validate_trigger(objective.at("trigger"));
      }
    }
    if (quest.at("progresses").size() > 64 ||
        !quests.emplace(quest.at("cms_id").get<int>(), quest).second) {
      throw std::runtime_error("Invalid or duplicate tutorial quest");
    }
  }
  for (const auto &item : document.at("utility_items")) {
    utility_items.insert(item.get<int>());
  }
  if (document.contains("potions")) {
    if (!document.at("potions").is_array() || document.at("potions").size() > 32) {
      throw std::runtime_error("Tutorial potion count exceeds limit");
    }
    for (const auto &potion : document.at("potions")) {
      const int item_id = potion.at("item_cms_id");
      const int execute_ms = potion.at("execute_time_ms");
      const int duration_ms = potion.at("duration_ms");
      if (!utility_items.contains(item_id) || potion.at("skill_id").get<int>() <= 0 ||
          potion.at("effect_id").get<int>() <= 0 || execute_ms < kPotionTimingToleranceMs ||
          duration_ms <= execute_ms || duration_ms > 10000 ||
          !potions.emplace(item_id, potion).second) {
        throw std::runtime_error("Invalid tutorial potion definition");
      }
    }
  }
  if (!document.at("civilians").is_array() || document.at("civilians").size() > 32) {
    throw std::runtime_error("Tutorial civilian count exceeds limit");
  }
  for (const auto &npc : document.at("civilians")) {
    validate_point(npc.at("position"));
    if (npc.at("cms_id").get<int>() <= 0 ||
        !civilians.emplace(parse_unit_id(npc.at("unit_id")), npc).second) {
      throw std::runtime_error("Invalid tutorial civilian definition");
    }
  }
  if (document.contains("npc_spawns")) {
    const auto &spawns = document.at("npc_spawns");
    if (!spawns.is_array() || spawns.size() > 128) {
      throw std::runtime_error("Tutorial NPC spawn count exceeds limit");
    }
    for (const auto &npc : spawns) {
      const auto unit_id = parse_unit_id(npc.at("unit_id"));
      validate_point(npc.at("position"));
      validate_wire_guid(npc.at("spawner_guid"));
      const auto behavior = npc.at("behavior").get<std::string>();
      const auto faction = npc.at("faction").get<std::string>();
      const auto namespace_id = unit_id >> 60;
      if (npc.at("cms_id").get<int>() <= 0 || !std::isfinite(npc.at("yaw").get<double>()) ||
          npc.at("anim_stance_key").get<int>() < 0 ||
          (npc.contains("initially_active") && !npc.at("initially_active").is_boolean()) ||
          (behavior != "Monster" && behavior != "Civilian" && behavior != "Mannequin") ||
          (faction != "EX5Faction_Bandit" && faction != "EX5Faction_TownNpc") ||
          namespace_id != (behavior == "Monster" ? 2 : 1) || (unit_id & 0xfff) != 2 ||
          objects.contains(unit_id) || doodads.contains(unit_id) ||
          !npcs.emplace(unit_id, npc).second) {
        throw std::runtime_error("Invalid or duplicate tutorial NPC spawn");
      }
    }
    for (const auto &[unit_id, civilian] : civilians) {
      const auto found = npcs.find(unit_id);
      if (found == npcs.end() || found->second != civilian) {
        throw std::runtime_error("Tutorial civilian differs from its NPC spawn");
      }
    }
  } else {
    // Older interaction-only profiles have just the dialogue NPC placements.
    npcs = civilians;
  }
  for (const auto &[cms_id, definition] : quests) {
    if (definition.contains("next_quest") &&
        !quests.contains(definition.at("next_quest").get<int>())) {
      throw std::runtime_error("Tutorial quest chain refers to an unknown definition");
    }
    std::unordered_set<int> visited{cms_id};
    const Json *current = &definition;
    while (current->contains("next_quest")) {
      const int next_id = current->at("next_quest");
      if (!visited.insert(next_id).second) {
        throw std::runtime_error("Tutorial quest chain contains a cycle");
      }
      current = quest(next_id);
      if (!current) {
        throw std::runtime_error("Tutorial quest chain refers to an unknown definition");
      }
    }
  }
  for (const auto &[id, doodad] : doodads) {
    if (!doodad.contains("quest_id")) {
      if (doodad.contains("interaction_id") || doodad.at("initial_phase").get<int>() <= 0) {
        throw std::runtime_error("Passive tutorial doodad has an invalid interaction");
      }
      continue;
    }
    const auto *definition = quest(doodad.at("quest_id").get<int>());
    if (!definition || doodad.at("animation_id").get<int>() < 0 ||
        doodad.at("initial_phase").get<int>() <= 0 ||
        doodad.at("completed_phase").get<int>() <= 0 ||
        !std::ranges::any_of(definition->at("progresses"), [&](const auto &objective) {
          return objective.at("sequence") == doodad.at("quest_sequence") &&
                 objective.at("kind") == "InteractDoodad";
        })) {
      throw std::runtime_error("Tutorial doodad has an invalid quest binding");
    }
    if (doodad.contains("execute_time_ms")) {
      const int execute_ms = doodad.at("execute_time_ms");
      const int duration_ms = doodad.at("duration_ms");
      if (execute_ms <= 0 || duration_ms <= execute_ms || duration_ms > 30000) {
        throw std::runtime_error("Invalid tutorial interaction timing");
      }
    }
    if (doodad.contains("reset_time_ms") && (doodad.at("reset_time_ms").get<int>() <= 0 ||
                                             doodad.at("reset_time_ms").get<int>() > 30000)) {
      throw std::runtime_error("Invalid tutorial doodad reset timing");
    }
    if (doodad.contains("phase_prerequisite")) {
      const auto &prerequisite = doodad.at("phase_prerequisite");
      const auto *required_quest = quest(prerequisite.at("quest_id"));
      if (!required_quest ||
          !std::ranges::any_of(required_quest->at("progresses"), [&](const auto &objective) {
            return objective.at("sequence") == prerequisite.at("sequence");
          })) {
        throw std::runtime_error("Unknown tutorial doodad phase prerequisite");
      }
    }
    for (const auto &update : doodad.value("phase_updates", Json::array())) {
      const auto target_id = parse_unit_id(update.at("unit_id"));
      const auto *target = this->doodad(target_id);
      if (target_id == id || !target || target->contains("interaction_id") ||
          !target->contains("phases") ||
          !std::ranges::any_of(target->at("phases"),
                               [&](const auto &phase) { return phase == update.at("phase"); })) {
        throw std::runtime_error("Invalid tutorial linked doodad phase");
      }
    }
  }
  load_level_triggers(document, *this);
  load_cut_scenes(document, *this);
  load_boss_defeats(document, *this);
  if (document.contains("followup_profiles")) {
    const auto &profiles = document.at("followup_profiles");
    if (map_id() != kTutorialMap || !profiles.is_array() || profiles.size() > 1) {
      throw std::runtime_error("Invalid tutorial followup profiles");
    }
    for (const auto &profile : profiles) {
      if (profile.at("map_id") != 10117 || profile.contains("followup_profiles") ||
          !followup_profiles.emplace(10117, std::make_shared<const TutorialData>(profile)).second) {
        throw std::runtime_error("Invalid or duplicate tutorial followup profile");
      }
    }
  }
}

const Json *TutorialData::object(std::uint64_t unit_id) const {
  return find_definition(objects, unit_id);
}

const Json *TutorialData::doodad(std::uint64_t unit_id) const {
  return find_definition(doodads, unit_id);
}

const Json *TutorialData::quest(int cms_id) const {
  return find_definition(quests, cms_id);
}

const Json *TutorialData::level_trigger(const std::string &wire_guid) const {
  return find_definition(level_triggers, wire_guid);
}

const Json *TutorialData::cut_scene(int cut_scene_id) const {
  return find_definition(cut_scenes, cut_scene_id);
}

const Json *TutorialData::boss_defeat(int npc_cms_id) const {
  return find_definition(boss_defeats, npc_cms_id);
}

int TutorialData::map_id() const {
  return document.at("map_id").get<int>();
}

std::shared_ptr<const TutorialData> TutorialData::followup(int requested_map_id) const {
  const auto found = followup_profiles.find(requested_map_id);
  return found == followup_profiles.end() ? nullptr : found->second;
}

TutorialSession::TutorialSession(std::shared_ptr<const TutorialData> data,
                                 std::int64_t character_id, WorldProgress progress, Commit commit,
                                 std::shared_ptr<const Progression> progression)
    : data_(std::move(data)), character_id_(character_id), progress_(std::move(progress)),
      commit_(std::move(commit)), progression_(std::move(progression)) {}

const WorldProgress &TutorialSession::progress() const {
  return progress_;
}

std::int64_t TutorialSession::stack_quantity(int item_cms_id) const {
  const auto stack = progress_.stackables.find(item_cms_id);
  return stack == progress_.stackables.end() ? 0 : stack->second;
}

SkillItemUsePlan TutorialSession::prepare_skill_item_use(const std::string &nonce,
                                                          int item_cms_id) const {
  return chronicles::prepare_skill_item_use(nonce, item_cms_id, progress_);
}

std::vector<GameplayPacket> TutorialSession::consume_used_item(int item_cms_id) {
  auto next = progress_;
  if (!spend_utility_item(next, item_cms_id)) {
    throw std::runtime_error("Accepted item use lost its inventory item");
  }

  const auto remaining = next.stackables.at(item_cms_id);
  commit_progress(std::move(next));

  std::vector<GameplayPacket> packets;
  packets.push_back(
      {kItemsUpdated,
       {{"stackables", {{{"item_cms_id", item_cms_id}, {"quantity", remaining}}}},
        {"utility_bag", Json::object()}}});
  return packets;
}

const TutorialData &TutorialSession::data() const {
  return *data_;
}

std::optional<WorldPosition> TutorialSession::take_completed_teleport() {
  auto completed = std::move(completed_teleport_);
  completed_teleport_.reset();
  return completed;
}

std::optional<TutorialMapMove> TutorialSession::take_map_move() {
  auto move = std::move(pending_map_move_);
  pending_map_move_.reset();
  return move;
}

void TutorialSession::enter_map(std::shared_ptr<const TutorialData> data) {
  if (!data) {
    throw std::invalid_argument("A map move requires an instance profile");
  }

  data_ = std::move(data);
  pending_.reset();
  doodad_reset_ticks_.clear();
  dialogue_target_.reset();
  active_cut_scene_.reset();
  pending_teleport_.reset();
  completed_teleport_.reset();
  pending_map_move_.reset();
}

std::optional<int> TutorialSession::dialogue_npc_cms_id() const {
  if (!dialogue_target_) {
    return std::nullopt;
  }
  const auto npc = data_->npcs.find(*dialogue_target_);
  if (npc == data_->npcs.end()) {
    return std::nullopt;
  }
  return npc->second.at("cms_id").get<int>();
}

void TutorialSession::adopt_committed_progress(WorldProgress committed) {
  if (committed.revision != progress_.revision + 1) {
    throw std::logic_error("Adopted tutorial progress must follow the current revision");
  }
  progress_ = std::move(committed);
}

void TutorialSession::accept_combat_progress(WorldProgress committed) {
  if (progress_.revision == std::numeric_limits<std::int64_t>::max() ||
      committed.revision != progress_.revision + 1 ||
      committed.dead == progress_.dead) {
    throw std::logic_error("Unexpected combat progress transition");
  }

  progress_ = std::move(committed);
}

void TutorialSession::accept_equipment_revision(std::int64_t committed_revision) {
  if (progress_.revision == std::numeric_limits<std::int64_t>::max() ||
      committed_revision != progress_.revision + 1) {
    throw std::logic_error("Unexpected equipment progress revision");
  }

  progress_.revision = committed_revision;
}

std::vector<GameplayPacket> TutorialSession::snapshot() const {
  Json stacks = Json::array();
  for (const auto &[cms_id, quantity] : progress_.stackables) {
    stacks.push_back({{"item_cms_id", cms_id}, {"quantity", quantity}});
  }
  Json objects = Json::array();
  for (const auto &[cms_id, state] : progress_.adventure_objects) {
    objects.push_back({{"cms_id", cms_id},
                       {"state", state == 2 ? "EX5AdventureObjectState_Activated"
                                            : "EX5AdventureObjectState_Discovered"}});
  }
  Json quests = Json::array();
  for (const auto &[cms_id, quest] : progress_.quests) {
    quests.push_back(quest_json(cms_id, quest));
  }
  return {{kWorldItems, {{"update", true}, {"stackables", std::move(stacks)}}},
          {kWorldMoney,
           {{"money",
             {{"values", world_money_values_json(progress_.gold, progress_.hersander_token)}}}}},
          {kWorldAdventureObjects, {{"adventure_objects", std::move(objects)}}},
          {kWorldQuests, {{"quests", std::move(quests)}}},
          {kWorldUtilityBag, {{"bag", utility_bag_json(progress_)}}}};
}

std::vector<GameplayPacket> TutorialSession::sectors() const {
  std::vector<GameplayPacket> packets;
  auto restored = progress_;
  std::vector<GameplayPacket> restored_phases;
  // Project links for characters saved before linked gates were persisted.
  refresh_doodad_phases(restored, 0, restored_phases);
  for (const auto &sector : data_->document.at("sectors")) {
    Json message{{"sector_id", sector}};
    for (const auto &[unit_id, doodad] : data_->doodads) {
      if (doodad.at("sector_id") != sector) {
        continue;
      }
      const auto saved = restored.doodad_phases.find(unit_id);
      // Deactivated placements (spot "deactivation") stay absent until a server script
      // activates them; the official sector refresh omits them as well.
      if (doodad.value("initially_deactivated", false) && saved == restored.doodad_phases.end()) {
        continue;
      }
      const bool reset_on_reconnect =
          doodad.contains("reset_time_ms") && !doodad_reset_ticks_.contains(unit_id);
      message["doodads"].push_back(
          {{"id", doodad.at("local_id")},
           {"phase", saved == restored.doodad_phases.end() || reset_on_reconnect
                         ? doodad.at("initial_phase").get<int>()
                         : saved->second}});
    }
    packets.push_back({kSectorUpdate, std::move(message)});
  }
  for (const auto &[id, npc] : data_->npcs) {
    // Script-controlled spawners remain mapped without appearing before their
    // activation event. Placement does not establish the missing quest trigger.
    if (!npc.value("initially_active", true)) {
      continue;
    }
    const auto &point = npc.at("position");
    Json npc_nub{{"npc_cms_id", npc.at("cms_id")},
                 {"base_unit_id", std::to_string(id)},
                 {"state", "EX5UnitStateType_Idle"},
                 {"faction_type", npc.value("faction", std::string("EX5Faction_TownNpc"))}};
    if (npc.contains("spawner_guid")) {
      npc_nub["spawner_id_x"] = npc.at("spawner_guid").at(0);
      npc_nub["spawner_id_y"] = npc.at("spawner_guid").at(1);
      npc_nub["anim_stance_key"] = npc.at("anim_stance_key");
    }
    packets.push_back({kUnitAdded,
                       {{"unit_id", std::to_string(id)},
                        {"by_sight", true},
                        {"nub",
                         {{"pos",
                           {{"millimeter_x", point.at(0)},
                            {"millimeter_y", point.at(1)},
                            {"millimeter_z", point.at(2)}}},
                          {"yaw", npc.at("yaw")},
                          {"npc", std::move(npc_nub)}}}}});
  }
  return packets;
}

void TutorialSession::activate(WorldProgress &next, std::uint64_t unit_id, int interaction_id,
                               const WorldPosition &position, std::vector<GameplayPacket> &packets,
                               std::vector<CharacterItem> &granted_instances,
                               bool animated) const {
  const auto *object = data_->object(unit_id);
  require(object != nullptr, "Interaction_TargetNotFound");
  require(!object->contains("unavailable_reason"), "Interaction_ConditionsNotMatch");
  require(object->at("interaction_id") == interaction_id, "Interaction_GroupIdNotMatch");
  require(distance_squared(position, object->at("position")) <=
              kInteractionRangeMm * kInteractionRangeMm,
          "TooFar");
  const int cms_id = object->at("cms_id");
  const bool consumed =
      next.adventure_objects.contains(cms_id) && next.adventure_objects.at(cms_id) == 2;
  packets.push_back({kAdventureObjectActivated,
                     {{"adventure_object",
                       {{"cms_id", cms_id}, {"state", "EX5AdventureObjectState_Activated"}}}}});
  if (!consumed) {
    require(!object->contains("build_points") || progression_ != nullptr,
            "Interaction_ConditionsNotMatch");
    next.adventure_objects[cms_id] = 2;

    Json rewards = object->at("rewards");
    if (object->value("loot_pack_id", 0) != 0) {
      require(static_cast<bool>(object_rewards_), "Interaction_ConditionsNotMatch");
      rewards = object_rewards_(cms_id, next);
      validate_rewards(rewards, true);
    }
    Json stack_rewards = Json::array();
    for (const auto &reward : rewards) {
      if (reward.contains("instance")) {
        append_instance_rewards(reward, granted_instances);
      } else {
        stack_rewards.push_back(reward);
      }
    }
    grant_rewards(next, stack_rewards, packets, "adventure_object_activate");

    // The item rows are filled in after the storage commit assigns their identifiers.
    if (!granted_instances.empty() && stack_rewards.empty()) {
      packets.push_back({kItemsUpdated, {{"adventure_object_activate", Json::object()}}});
    }
    grant_experience(next, object->value("experience", std::int64_t{0}), packets, progression_.get());
    if (object->contains("build_points")) {
      grant_build_points(next, object->at("build_points"), packets);
    }
  }
  if (!animated) {
    packets.push_back(
        {kUnitInteractionResult,
         {{"interaction_id", interaction_id}, {"target_unit_id", std::to_string(unit_id)}}});
  }
}

void TutorialSession::accept(WorldProgress &next, int quest_id,
                             std::vector<GameplayPacket> &packets) const {
  const auto *definition = data_->quest(quest_id);
  require(definition != nullptr, "Quest_InvalidCmsId");
  if (next.quests.contains(quest_id) && next.quests.at(quest_id).complete) {
    packets.push_back({kQuestUpdate, {{"quest", quest_json(quest_id, next.quests.at(quest_id))}}});
    return;
  }
  if (!next.quests.contains(quest_id)) {
    QuestProgress quest;
    for (const auto &objective : definition->at("progresses")) {
      quest.objectives.emplace(objective.at("sequence").get<int>(), 0);
    }
    next.quests.emplace(quest_id, std::move(quest));
    if (definition->contains("supplies")) {
      grant_rewards(next, definition->at("supplies"), packets, "quest");
    }
  }
  packets.push_back({kQuestAccepted, {{"quest", quest_json(quest_id, next.quests.at(quest_id))}}});
  if (definition->at("auto_complete").get<bool>() &&
      objectives_complete(*definition, next.quests.at(quest_id))) {
    reward(next, *definition, packets);
  }
}

void TutorialSession::reward(WorldProgress &next, const Json &definition,
                             std::vector<GameplayPacket> &packets) const {
  const int quest_id = definition.at("cms_id");
  auto &quest = next.quests.at(quest_id);
  if (quest.complete) {
    packets.push_back({kQuestUpdate, {{"quest", quest_json(quest_id, quest)}}});
    return;
  }
  require(objectives_complete(definition, quest), "Quest_IsNotComplete");
  quest.complete = true;
  quest.success_count = 1;
  grant_rewards(next, definition.at("rewards"), packets, "quest_reward");
  grant_experience(next, definition.at("experience").get<std::int64_t>(), packets, progression_.get());
  if (definition.contains("build_points")) {
    grant_build_points(next, definition.at("build_points"), packets);
  }
  packets.push_back({kQuestUpdate, {{"quest", quest_json(quest_id, quest)}}});
  if (definition.contains("next_quest")) {
    accept(next, definition.at("next_quest").get<int>(), packets);
  }
}

void TutorialSession::execute(WorldProgress &next, const WorldPosition &position,
                              std::int64_t ticks, std::vector<GameplayPacket> &packets,
                              std::vector<CharacterItem> &granted_instances) {
  require(pending_.has_value(), "InteractionAnim_Mismatch");
  const auto *definition = data_->doodad(pending_->unit_id);
  if (!definition) {
    definition = data_->object(pending_->unit_id);
  }
  require(definition != nullptr, "InteractionAnim_NotFoundTarget");
  const auto &doodad = *definition;
  const auto minimum_ticks =
      doodad.contains("execute_time_ms")
          ? std::max(1, doodad.at("execute_time_ms").get<int>() - kInteractionTimingToleranceMs) *
                kTicksPerMillisecond
          : kMinimumInteractionTicks;
  require(ticks >= pending_->started_ticks + minimum_ticks &&
              ticks <= pending_->started_ticks + kInteractionTimeoutTicks,
          "InteractionAnim_TimeNotMatch");
  require(distance_squared(position, doodad.at("position")) <=
              kInteractionRangeMm * kInteractionRangeMm,
          "TooFar");
  if (!pending_->executed) {
    if (data_->object(pending_->unit_id)) {
      activate(next, pending_->unit_id, doodad.at("interaction_id"), position, packets,
               granted_instances, true);
    } else {
      interact_doodad(next, pending_->unit_id, position, ticks, packets);
    }
  }
  packets.push_back({kInteractionAnimationResult, {{"unit_id", std::to_string(character_id_)}}});
}

void TutorialSession::interact_doodad(WorldProgress &next, std::uint64_t unit_id,
                                      const WorldPosition &position, std::int64_t ticks,
                                      std::vector<GameplayPacket> &packets) const {
  const auto &doodad = *data_->doodad(unit_id);
  const auto saved = next.doodad_phases.find(unit_id);
  const bool completed =
      saved != next.doodad_phases.end() && saved->second == doodad.at("completed_phase");
  const auto saved_quest = next.quests.find(doodad.at("quest_id"));
  const int sequence = doodad.at("quest_sequence");
  const bool objective_done = saved_quest != next.quests.end() &&
                              saved_quest->second.objectives.contains(sequence) &&
                              saved_quest->second.objectives.at(sequence) > 0;
  if (completed || objective_done) {
    return;
  }
  validate_doodad_action(*data_, doodad, next);
  const int quest_id = doodad.at("quest_id");
  auto &quest = next.quests.at(quest_id);
  Json consumed = Json::array();
  for (const auto &item : doodad.at("consumes")) {
    const int cms_id = item.at("item_cms_id");
    const int quantity = item.at("quantity");
    next.stackables[cms_id] -= quantity;
    consumed.push_back({{"item_cms_id", cms_id}, {"quantity", next.stackables.at(cms_id)}});
  }
  if (!consumed.empty()) {
    packets.push_back(
        {kItemsUpdated, {{"stackables", std::move(consumed)}, {"system", Json::object()}}});
  }
  const int phase = doodad.at("completed_phase");
  next.doodad_phases[unit_id] = phase;
  quest.objectives[sequence] = 1;
  packets.push_back({kDoodadPhaseChanged,
                     {{"unit_id", std::to_string(unit_id)},
                      {"phase_id", phase},
                      {"phase_begin_tick", wire_monotonic_tick(ticks)}}});
  if (doodad.contains("cut_scene_id")) {
    play_cut_scene(doodad.at("cut_scene_id").get<int>(), position, packets);
  }
  packets.push_back({kQuestUpdate, {{"quest", quest_json(quest_id, quest)}}});
}

void TutorialSession::play_cut_scene(int cut_scene_id, const WorldPosition &position,
                                     std::vector<GameplayPacket> &packets) const {
  const auto *definition = data_->cut_scene(cut_scene_id);
  if (definition && definition->at("invisible").get<bool>()) {
    packets.push_back({kVisibleChannel,
                       {{"unit_id", std::to_string(character_id_)},
                        {"visible_channel", std::to_string(kCutSceneVisibleChannel)}}});
  }
  packets.push_back({kCutScenePlay,
                     {{"cut_scene_id", cut_scene_id},
                      {"unit_id", std::to_string(character_id_)},
                      {"pos", position_json(position)},
                      {"yaw", position.yaw}}});
}

void TutorialSession::remember_started_cut_scene(const std::vector<GameplayPacket> &packets) {
  for (const auto &packet : packets) {
    if (packet.opcode == kCutScenePlay) {
      active_cut_scene_ = packet.message.at("cut_scene_id").get<int>();
    }
  }
}

void TutorialSession::fire_level_trigger(WorldProgress &next, const Json &request,
                                         const WorldPosition &position, std::int64_t ticks,
                                         std::vector<GameplayPacket> &packets) const {
  // The client reports every authored volume it enters. Unknown volumes, unmet
  // conditions and repeats during a cutscene are routine and produce no reply.
  const auto key = request.at("trigger_id_x").get<std::string>() + ":" +
                   request.at("trigger_id_y").get<std::string>();
  const auto *trigger = data_->level_trigger(key);
  if (!trigger || active_cut_scene_) {
    return;
  }
  const bool has_supported_rule =
      std::ranges::any_of(trigger->at("rules"), [](const auto &rule) {
        return rule.at("supported").template get<bool>();
      });
  if (!has_supported_rule) {
    return;
  }

  require(request_position_matches(request, position), "TooFar");
  const bool leave = trigger->at("trigger_type") == "Leave";
  const bool inside = position_within_trigger(*trigger, position, kPositionToleranceMm);
  const bool deep_inside = position_within_trigger(*trigger, position, -kPositionToleranceMm);
  require(inside && (!leave || !deep_inside), "TooFar");

  // Each supported rule whose conditions hold runs its tasks. Unsupported rules (unknown
  // condition kinds, unrecovered end points) are never executed.
  bool cut_scene_started = false;
  for (const auto &rule : trigger->at("rules")) {
    if (!rule.at("supported").get<bool>() ||
        !level_trigger_conditions_pass(*data_, rule.at("conditions"), next)) {
      continue;
    }

    for (const auto &task : rule.at("tasks")) {
      if (task.at("kind") == "PlayCutScene") {
        // The client watches one server cutscene at a time.
        if (!cut_scene_started) {
          play_cut_scene(task.at("cut_scene_id").get<int>(), position, packets);
          cut_scene_started = true;
        }
        continue;
      }

      const auto unit_id = parse_unit_id(task.at("unit_id"));
      const int phase = task.at("phase");
      const auto saved = next.doodad_phases.find(unit_id);
      if (saved != next.doodad_phases.end() && saved->second == phase) {
        continue;
      }
      next.doodad_phases[unit_id] = phase;
      packets.push_back({kDoodadPhaseChanged,
                         {{"unit_id", std::to_string(unit_id)},
                          {"phase_id", phase},
                          {"phase_begin_tick", wire_monotonic_tick(ticks)}}});
    }
  }
}

void TutorialSession::end_cut_scene(int cut_scene_id, std::int64_t ticks,
                                    std::vector<GameplayPacket> &packets) {
  // Client-only cutscenes and repeated end requests have no server follow-up.
  if (!active_cut_scene_ || *active_cut_scene_ != cut_scene_id) {
    return;
  }
  active_cut_scene_.reset();

  const auto *definition = data_->cut_scene(cut_scene_id);
  if (!definition) {
    return;
  }

  // Captured order for cutscene 1523: TeleportReq, VisibleChannelNoti, SetNpcAiPhaseNoti.
  if (definition->contains("end_teleport")) {
    const auto destination = definition_position(data_->map_id(), definition->at("end_teleport"));
    packets.push_back({kTeleportRequest,
                       {{"unit_id", std::to_string(character_id_)},
                        {"pos", position_json(destination)},
                        {"body_yaw", destination.yaw},
                        {"tick", wire_monotonic_tick(ticks)}}});
    pending_teleport_ = destination;
  }
  if (definition->at("invisible").get<bool>() && !definition->contains("end_map_move")) {
    packets.push_back({kVisibleChannel,
                       {{"unit_id", std::to_string(character_id_)},
                        {"visible_channel", std::to_string(kDefaultVisibleChannel)}}});
  }
  for (const auto &binding : definition->at("npc_bindings")) {
    packets.push_back({kSetNpcAiPhase,
                       {{"unit_id", binding.at("unit_id")},
                        {"new_ai_phase_transition_id", binding.at("ai_phase")}}});
  }
  if (definition->contains("end_map_move")) {
    const auto &move = definition->at("end_map_move");
    const int map_id = move.at("map_id");
    pending_map_move_ = TutorialMapMove{map_id, definition_position(map_id, move)};
  }
}

void TutorialSession::apply_boss_defeat(WorldProgress &next, int npc_cms_id,
                                        const WorldPosition &position, std::int64_t ticks,
                                        std::vector<GameplayPacket> &packets) const {
  const auto *defeat = data_->boss_defeat(npc_cms_id);
  if (!defeat) {
    return;
  }

  packets.push_back({kBossKilledBanner,
                     {{"boss_cms_id", std::to_string(npc_cms_id)}, {"is_last_hit_user", true}}});
  for (const auto &activation : defeat->at("doodad_activations")) {
    const auto unit_id = parse_unit_id(activation.at("unit_id"));
    const int phase = activation.at("phase");
    next.doodad_phases[unit_id] = phase;
    packets.push_back({kDoodadPhaseChanged,
                       {{"unit_id", std::to_string(unit_id)},
                        {"phase_id", phase},
                        {"phase_begin_tick", wire_monotonic_tick(ticks)}}});
  }
  play_cut_scene(defeat->at("cut_scene_id").get<int>(), position, packets);
}

void TutorialSession::refresh_doodad_phases(WorldProgress &next, std::int64_t ticks,
                                            std::vector<GameplayPacket> &packets) const {
  const auto phase = [&](std::uint64_t unit_id, const Json &definition) {
    const auto saved = next.doodad_phases.find(unit_id);
    return saved == next.doodad_phases.end() ? definition.at("initial_phase").get<int>()
                                             : saved->second;
  };
  const auto change_phase = [&](std::uint64_t unit_id, int desired_phase) {
    const auto &definition = *data_->doodad(unit_id);
    if (phase(unit_id, definition) == desired_phase) {
      return;
    }
    next.doodad_phases[unit_id] = desired_phase;
    packets.push_back({kDoodadPhaseChanged,
                       {{"unit_id", std::to_string(unit_id)},
                        {"phase_id", desired_phase},
                        {"phase_begin_tick", wire_monotonic_tick(ticks)}}});
  };

  for (const auto &[unit_id, definition] : data_->doodads) {
    if (definition.contains("phase_prerequisite") &&
        phase(unit_id, definition) == definition.at("initial_phase")) {
      const auto &requirement = definition.at("phase_prerequisite");
      const auto quest = next.quests.find(requirement.at("quest_id"));
      if (quest != next.quests.end()) {
        const auto objective = quest->second.objectives.find(requirement.at("sequence"));
        if (quest->second.complete ||
            (objective != quest->second.objectives.end() && objective->second > 0)) {
          change_phase(unit_id, definition.at("interaction_phase"));
        }
      }
    }
    if (definition.contains("completed_phase") &&
        phase(unit_id, definition) == definition.at("completed_phase")) {
      for (const auto &update : definition.value("phase_updates", Json::array())) {
        change_phase(parse_unit_id(update.at("unit_id")), update.at("phase"));
      }
    }
  }
}

std::vector<GameplayPacket> TutorialSession::record_monster_defeat(std::uint64_t unit_id,
                                                                   const WorldPosition &position,
                                                                   std::int64_t server_ticks) {
  const auto found = data_->npcs.find(unit_id);
  if (position.map_id != data_->map_id() || found == data_->npcs.end() ||
      found->second.at("behavior") != "Monster" ||
      distance_squared(position, found->second.at("position")) >
          kQuestKillCreditRangeMm * kQuestKillCreditRangeMm) {
    throw std::invalid_argument("Quest kill must belong to a nearby monster in this instance");
  }

  auto next = progress_;
  std::vector<GameplayPacket> packets;
  bool objective_finished = false;
  for (const auto &[quest_id, previous] : progress_.quests) {
    auto &quest = next.quests.at(quest_id);
    const auto *definition = data_->quest(quest_id);
    if (!definition || quest.complete || quest.credited_kills.contains(unit_id)) {
      continue;
    }

    // Evaluate eligibility against the state before this event. One kill must
    // not unlock and satisfy a later sequential group in the same dispatch.
    bool changed = false;
    for (const auto &objective : definition->at("progresses")) {
      if (objective.at("kind") != "HuntMonster" ||
          objective.at("parameters").at("npcId") != found->second.at("cms_id") ||
          !objective_ready(*definition, previous, objective.at("group"))) {
        continue;
      }
      const int sequence = objective.at("sequence");
      const int required = objective.at("parameters").value("count", 1);
      auto &value = quest.objectives[sequence];
      if (value < required) {
        ++value;
        changed = true;
        objective_finished = objective_finished || value == required;
      }
    }
    if (changed) {
      if (quest.credited_kills.size() >= 4096) {
        throw std::runtime_error("Quest kill receipt limit exceeded");
      }
      quest.credited_kills.insert(unit_id);
      packets.push_back({kQuestUpdate, {{"quest", quest_json(quest_id, quest)}}});
      if (definition->at("auto_complete").get<bool>() && objectives_complete(*definition, quest)) {
        reward(next, *definition, packets);
      }
    }
  }

  // Combat established this death, so the kill EXP belongs to this character. The official
  // tutorial capture reports the kill's ChangeCharacterBaseNoti after its quest updates.
  if (progression_) {
    const int npc_cms_id = found->second.at("cms_id");
    grant_experience(next, progression_->monster_experience(npc_cms_id), packets, progression_.get());
  }

  // A boss script runs once, with the kill that finishes its quest objective. Later kills
  // of the same boss (or of a multi-count objective's earlier targets) do not replay it.
  if (objective_finished) {
    apply_boss_defeat(next, found->second.at("cms_id").get<int>(), position, server_ticks,
                      packets);
  }

  refresh_doodad_phases(next, server_ticks, packets);
  commit_progress(std::move(next));
  remember_started_cut_scene(packets);
  return packets;
}

void TutorialSession::start_potion(const Json &request, const WorldPosition &position,
                                   std::int64_t ticks, std::vector<GameplayPacket> &packets) {
  require(!pending_, "Skill_UsingSkill");
  const auto &item = request.at("use_item");
  const int item_id = item.at("item_cms_id");
  const auto definition = data_->potions.find(item_id);
  require(definition != data_->potions.end(), "Skill_UnusableItem");
  require(item.value("item_id", std::string("0")) == "0", "Skill_InvalidUseItem");
  require(progress_.stackables.contains(item_id) && progress_.stackables.at(item_id) > 0,
          "Skill_NotFoundUseItem");

  const auto &start = request.at("start_skill");
  const auto client_tick = start.at("tick").get<std::string>();
  const auto instance = start.at("instance_id").get<std::uint32_t>();
  require(instance != 0 && !potion_instances_.contains(instance) && potion_instances_.size() < 4096,
          "Skill_InvalidInstanceId");
  require(start.at("skill_id") == definition->second.at("skill_id") &&
              start.value("src_skill_id", 0) == 0 && start.value("layer_index", 0) == 0,
          "Skill_InvalidCmsId");
  require(request_position_matches({{"pos", start.at("pos")}}, position), "TooFar");
  const auto &rotation = start.at("rotation");
  for (const auto *axis : {"pitch", "roll", "yaw"}) {
    require(std::isfinite(rotation.value(axis, 0.0)), "BadRequest");
  }
  if (pending_potion_) {
    const auto &previous = data_->potions.at(pending_potion_->item_cms_id);
    require(ticks >= pending_potion_->started_ticks +
                         previous.at("duration_ms").get<int>() * kTicksPerMillisecond,
            "Skill_UsingSkill");
  }

  const int duration_ms = definition->second.at("duration_ms");
  require(ticks > 0 && ticks <= std::numeric_limits<std::int64_t>::max() -
                                    duration_ms * kTicksPerMillisecond,
          "BadRequest");
  // Client tick/instance identify its animation. Eligibility uses server time.
  packets.push_back({kSkillCost,
                     {{"skill_instance_id", instance},
                      {"tick", client_tick},
                      {"cooldowns", {{{"expire_tick", client_tick}}}}}});
  packets.push_back(
      {kSkillStarted, {{"unit_id", std::to_string(character_id_)}, {"start_skill", start}}});
  pending_potion_ = PendingPotion{item_id, instance, ticks};
  potion_instances_.insert(instance);
}

void TutorialSession::validate_potion_effect(const Json &request, const WorldPosition &position,
                                             std::int64_t ticks) const {
  require(pending_potion_ && request.at("skill_instance_id") == pending_potion_->instance_id,
          "Skill_InvalidInstanceId");
  const auto &definition = data_->potions.at(pending_potion_->item_cms_id);
  const auto &effect = request.at("skill_effect");
  require(effect.at("effect_id") == definition.at("effect_id") &&
              effect.value("unique_effect_id", 0) == 0,
          "Skill_InvalidCmsId");
  require(effect.at("targets").is_array() && effect.at("targets").size() == 1 &&
              parse_unit_id(effect.at("targets").at(0).at("unit_id")) ==
                  static_cast<std::uint64_t>(character_id_),
          "Skill_InvalidTarget");
  require(request_position_matches(request, position), "TooFar");
  const auto minimum_ticks =
      (definition.at("execute_time_ms").get<int>() - kPotionTimingToleranceMs) *
      kTicksPerMillisecond;
  const auto duration_ticks = definition.at("duration_ms").get<int>() * kTicksPerMillisecond;
  require(ticks >= pending_potion_->started_ticks + minimum_ticks &&
              ticks <= pending_potion_->started_ticks + duration_ticks,
          "Skill_ExpiredUsableTick");
}

bool TutorialSession::busy() const {
  return pending_.has_value() || (pending_potion_ && !pending_potion_->finished);
}

bool TutorialSession::has_cut_scene(int cut_scene_id) const noexcept {
  return active_cut_scene_ && *active_cut_scene_ == cut_scene_id;
}

std::optional<std::int64_t> TutorialSession::next_action_tick() const {
  std::optional<std::int64_t> deadline;
  const auto include = [&](std::int64_t tick) {
    if (!deadline || tick < *deadline) {
      deadline = tick;
    }
  };
  if (pending_potion_ && !pending_potion_->finished) {
    const auto &definition = data_->potions.at(pending_potion_->item_cms_id);
    const char *time_field = pending_potion_->consumed ? "duration_ms" : "execute_time_ms";
    include(pending_potion_->started_ticks +
            definition.at(time_field).get<int>() * kTicksPerMillisecond);
  }
  if (pending_) {
    include(pending_->started_ticks + kInteractionTimeoutTicks);
  }
  for (const auto &[unit_id, tick] : doodad_reset_ticks_) {
    include(tick);
  }
  return deadline;
}

void TutorialSession::commit_progress(WorldProgress next) {
  if (next == progress_) {
    return;
  }
  if (next.revision == std::numeric_limits<std::int64_t>::max()) {
    throw std::runtime_error("World progress revision exhausted");
  }
  ++next.revision;
  if (!commit_(progress_, next)) {
    throw std::runtime_error("World progress revision conflict; reconnect to reload state");
  }
  progress_ = std::move(next);
}

void TutorialSession::set_instance_commit(InstanceCommit commit) {
  instance_commit_ = std::move(commit);
}

void TutorialSession::set_object_rewards(ObjectRewards resolver) {
  object_rewards_ = std::move(resolver);
}

void TutorialSession::refresh_shared_progress(WorldProgress committed) {
  if (data_->document.value("profile_scope", "tutorial") != "shared_adventure" ||
      committed.revision < progress_.revision) {
    throw std::logic_error("Shared adventure progress cannot replace a newer stored revision");
  }
  progress_ = std::move(committed);
}

std::vector<CharacterItem>
TutorialSession::commit_progress_with_items(WorldProgress next,
                                            const std::vector<CharacterItem> &granted) {
  // Without item storage the reward cannot be delivered, so nothing is committed and the
  // interaction stays unexecuted for a later retry.
  require(static_cast<bool>(instance_commit_), "Interaction_ConditionsNotMatch");
  if (next.revision == std::numeric_limits<std::int64_t>::max()) {
    throw std::runtime_error("World progress revision exhausted");
  }
  ++next.revision;

  auto result = instance_commit_(progress_, next, granted);
  if (result.status == InstanceCommitStatus::inventory_full) {
    throw ActionError{"ItemSlotLimit"};
  }
  if (result.status != InstanceCommitStatus::committed || result.stored.size() != granted.size()) {
    throw std::runtime_error("World progress revision conflict; reconnect to reload state");
  }

  progress_ = std::move(next);
  return std::move(result.stored);
}

void TutorialSession::grant_build_points(WorldProgress &next, const Json &entries,
                                         std::vector<GameplayPacket> &packets) const {
  if (!progression_) {
    return;
  }

  for (const auto &entry : entries) {
    const int point_type = entry.at("type");
    const int amount = entry.at("value");
    require(amount > 0, "BadRequest");
    require(progression_->grant_build_points(next, point_type, amount) == x5::ec::Success,
            "BadRequest");
    if (point_type == kWeaponPointType) {
      packets.push_back({kCharacterBaseChanged, weapon_points_message(next)});
    } else {
      packets.push_back({kCharacterBaseChanged, level_up_points_message(next)});
    }
  }
}

std::vector<GameplayPacket> TutorialSession::advance(std::int64_t ticks,
                                                     const WorldPosition &position) {
  const auto deadline = next_action_tick();
  if (!deadline || ticks < *deadline) {
    return {};
  }
  std::vector<GameplayPacket> packets;
  auto next = progress_;
  std::vector<std::uint64_t> completed_resets;
  for (const auto &[unit_id, deadline_tick] : doodad_reset_ticks_) {
    if (ticks < deadline_tick) {
      continue;
    }
    const int initial_phase = data_->doodad(unit_id)->at("initial_phase");
    next.doodad_phases[unit_id] = initial_phase;
    packets.push_back({kDoodadPhaseChanged,
                       {{"unit_id", std::to_string(unit_id)},
                        {"phase_id", initial_phase},
                        {"phase_begin_tick", wire_monotonic_tick(ticks)}}});
    completed_resets.push_back(unit_id);
  }
  commit_progress(std::move(next));
  for (const auto unit_id : completed_resets) {
    doodad_reset_ticks_.erase(unit_id);
  }
  if (pending_ && ticks >= pending_->started_ticks + kInteractionTimeoutTicks) {
    pending_.reset();
    packets.push_back({kInteractionAnimationResult, {{"unit_id", std::to_string(character_id_)}}});
  }
  if (!pending_potion_ || pending_potion_->finished) {
    return packets;
  }
  const auto &potion = data_->potions.at(pending_potion_->item_cms_id);
  const char *potion_time = pending_potion_->consumed ? "duration_ms" : "execute_time_ms";
  if (ticks <
      pending_potion_->started_ticks + potion.at(potion_time).get<int>() * kTicksPerMillisecond) {
    return packets;
  }
  const int item_id = pending_potion_->item_cms_id;
  if (!pending_potion_->consumed) {
    auto consumed = consume_used_item(item_id);
    packets.insert(packets.end(), std::make_move_iterator(consumed.begin()),
                   std::make_move_iterator(consumed.end()));
    // Kept for this fallback path only; official item uses send no UtilityBagSetRes.
    packets.push_back({kUtilityBagSet, {{"bag", utility_bag_json(progress_)}}});
    pending_potion_->consumed = true;
  }
  const auto end_tick =
      pending_potion_->started_ticks +
      data_->potions.at(item_id).at("duration_ms").get<int>() * kTicksPerMillisecond;
  if (ticks >= end_tick) {
    packets.push_back({kSkillEnded,
                       {{"unit_id", std::to_string(character_id_)},
                        {"end_skill",
                         {{"instance_id", pending_potion_->instance_id},
                          {"tick", wire_monotonic_tick(ticks)},
                          {"pos", position_json(position)},
                          {"yaw", position.yaw},
                          {"reason", "EX5EndSkillReason_Default"}}}}});
    pending_potion_->finished = true;
  }
  return packets;
}

std::vector<GameplayPacket> TutorialSession::receive(std::uint16_t opcode, const Json &request,
                                                     std::uint32_t sequence,
                                                     const WorldPosition &position,
                                                     std::int64_t server_ticks) {
  if (opcode == kStartSkill && !request.contains("use_item")) {
    return {};
  }
  if (opcode != kStartSkill && opcode != kExecuteSkill && opcode != kDiscoverObject &&
      opcode != kInteractUnit && opcode != kAcceptQuest && opcode != kProgressQuest &&
      opcode != kClaimQuestReward && opcode != kStartInteraction && opcode != kExecuteInteraction &&
      opcode != kEndInteraction && opcode != kSetUtilityBag && opcode != kStartDialogue &&
      opcode != kEndDialogue && opcode != kLevelTrigger && opcode != kCutSceneEnd &&
      opcode != kTeleportStart && opcode != kTeleportCompletion) {
    return {};
  }
  std::vector<GameplayPacket> packets;
  // Item instances an adventure object grants; they commit together with the progress below.
  std::vector<CharacterItem> granted_instances;
  try {
    require(server_ticks > 0 &&
                server_ticks <= std::numeric_limits<std::int64_t>::max() - kInteractionTimeoutTicks,
            "BadRequest");
    require(position.map_id == data_->map_id(), "BadRequest");
    if (server_ticks - request_window_ticks_ >= kTicksPerSecond) {
      request_window_ticks_ = server_ticks;
      requests_in_window_ = 0;
    }
    require(++requests_in_window_ <= 32, "BadRequest");
    auto next = progress_;
    switch (opcode) {
    case kStartSkill:
      start_potion(request, position, server_ticks, packets);
      break;
    case kExecuteSkill:
      validate_potion_effect(request, position, server_ticks);
      break;
    case kDiscoverObject: {
      const auto unit_id = parse_unit_id(request.at("spot_object_id"));
      const auto *object = data_->object(unit_id);
      require(object != nullptr, "Interaction_TargetNotFound");
      require(quest_rules::within_discovery_range(position, object->at("position")), "TooFar");
      const int cms_id = object->at("cms_id");
      if (!next.adventure_objects.contains(cms_id)) {
        next.adventure_objects[cms_id] = 1;
      }
      packets.push_back({kAdventureObjectsUpdated,
                         {{"adventure_objects",
                           {{{"cms_id", cms_id},
                             {"state", next.adventure_objects.at(cms_id) == 2
                                           ? "EX5AdventureObjectState_Activated"
                                           : "EX5AdventureObjectState_Discovered"}}}}}});
      break;
    }
    case kInteractUnit: {
      const auto unit_id = parse_unit_id(request.at("target_unit_id"));
      const int interaction_id = request.at("param").at("interaction_id");
      const auto *doodad = data_->doodad(unit_id);
      if (doodad) {
        require(doodad->contains("interaction_id") &&
                    doodad->at("interaction_id") == interaction_id &&
                    doodad->at("animation_id") == 0,
                "Interaction_GroupIdNotMatch");
        require(distance_squared(position, doodad->at("position")) <=
                    kInteractionRangeMm * kInteractionRangeMm,
                "TooFar");
        require(!pending_ && (!pending_potion_ || pending_potion_->finished),
                "InteractionAnim_Mismatch");
        interact_doodad(next, unit_id, position, server_ticks, packets);
        packets.push_back(
            {kUnitInteractionResult,
             {{"interaction_id", interaction_id}, {"target_unit_id", std::to_string(unit_id)}}});
      } else {
        const auto *object = data_->object(unit_id);
        require(object && !object->contains("animation_id"), "InteractionAnim_Mismatch");
        activate(next, unit_id, interaction_id, position, packets, granted_instances);
      }
      break;
    }
    case kAcceptQuest: {
      const int quest_id = request.at("quest_cms_id");
      const auto *definition = data_->quest(quest_id);
      require(definition != nullptr, "Quest_InvalidCmsId");
      bool allowed =
          std::ranges::any_of(definition->at("accept_triggers"), [&](const auto &trigger) {
            if (trigger.contains("request_field")) {
              return matches_trigger(request, trigger, position,
                                     trigger.at("request_field").get<std::string>().c_str());
            }
            return matches_trigger(request, trigger, position, "trigger_volume_enter") ||
                   matches_trigger(request, trigger, position, "unexpect_trigger_volume_enter");
          });
      if (request.contains("unit") && definition->contains("accept_npcs")) {
        const auto unit_id = parse_unit_id(request.at("unit").at("unit_id"));
        const auto npc = data_->npcs.find(unit_id);
        if (npc != data_->npcs.end() && npc->second.at("behavior") != "Monster") {
          allowed = request_position_matches(request, position) &&
                    distance_squared(position, npc->second.at("position")) <=
                        kInteractionRangeMm * kInteractionRangeMm &&
                    std::ranges::any_of(definition->at("accept_npcs"), [&](const auto &cms_id) {
                      return cms_id == npc->second.at("cms_id");
                    });
        }
      }
      require(allowed, "Quest_IsNotAcceptable");
      accept(next, quest_id, packets);
      break;
    }
    case kProgressQuest: {
      const int quest_id = request.at("quest_cms_id");
      const int sequence_id = request.at("quest_progress_sequence_id");
      const auto *definition = data_->quest(quest_id);
      require(definition != nullptr && next.quests.contains(quest_id), "Quest_IsNotInProgress");
      auto &quest = next.quests.at(quest_id);
      if (quest.complete) {
        packets.push_back({kQuestUpdate, {{"quest", quest_json(quest_id, quest)}}});
        break;
      }
      const auto objective =
          std::ranges::find_if(definition->at("progresses"), [sequence_id](const auto &entry) {
            return entry.at("sequence") == sequence_id;
          });
      require(objective != definition->at("progresses").end(), "Quest_IsNotProgress");
      require(objective_ready(*definition, quest, objective->at("group")), "Quest_IsNotProgress");
      if (objective->contains("trigger")) {
        const char *field = objective->at("kind") == "TriggerVolumeLeave" ? "trigger_volume_leave"
                                                                          : "trigger_volume_enter";
        require(matches_trigger(request, objective->at("trigger"), position, field),
                "Quest_IsNotProgress");
      } else if (objective->at("kind") == "DialogueSelection") {
        require(dialogue_target_.has_value(), "Quest_IsNotProgress");
        const auto &npc = data_->npcs.at(*dialogue_target_);
        const auto &selection = request.at("dialogue_selection");
        const auto &parameters = objective->at("parameters");
        require(parse_unit_id(selection.at("unit_id")) == *dialogue_target_ &&
                    npc.at("cms_id") == parameters.at("targetId") &&
                    selection.at("dialogue_cms_id") == parameters.at("dialogueId") &&
                    selection.at("sequence_order") == parameters.at("sequenceOrder") &&
                    selection.at("select_order") == parameters.at("selectOrder") &&
                    request_position_matches(request, position) &&
                    distance_squared(position, npc.at("position")) <=
                        kInteractionRangeMm * kInteractionRangeMm,
                "Quest_IsNotProgress");
      } else {
        // Kill and doodad objectives are generated by server actions only.
        throw ActionError{"Quest_IsNotProgress"};
      }
      quest.objectives[sequence_id] = 1;
      packets.push_back(
          {kQuestUpdate, {{"quest", quest_progress_update_json(quest_id, quest, sequence_id)}}});
      if (definition->at("auto_complete").get<bool>() && objectives_complete(*definition, quest)) {
        reward(next, *definition, packets);
      }
      break;
    }
    case kClaimQuestReward: {
      const int quest_id = request.at("quest_cms_id");
      const auto *definition = data_->quest(quest_id);
      require(definition != nullptr && next.quests.contains(quest_id), "Quest_IsNotInProgress");
      reward(next, *definition, packets);
      break;
    }
    case kStartInteraction: {
      const auto unit_id = parse_unit_id(request.at("target_unit_id"));
      const auto *doodad = data_->doodad(unit_id);
      const auto *object = data_->object(unit_id);
      if (!doodad) {
        doodad = object;
      }
      require(doodad != nullptr, "InteractionAnim_NotFoundTarget");
      require(!doodad->contains("unavailable_reason"), "Interaction_ConditionsNotMatch");
      require(doodad->contains("animation_id"), "InteractionAnim_NotFoundTarget");
      require(doodad->at("animation_id").get<int>() > 0 &&
                  doodad->at("animation_id") == request.at("interaction_anim_id") &&
                  doodad->at("interaction_id") ==
                      request.at("param").at("interaction").at("interaction_id"),
              "InteractionAnim_Mismatch");
      require(request_position_matches(request, position) &&
                  distance_squared(position, doodad->at("position")) <=
                      kInteractionRangeMm * kInteractionRangeMm,
              "TooFar");
      const int cms_id = doodad->at("cms_id");
      const bool executed =
          object ? next.adventure_objects.contains(cms_id) && next.adventure_objects.at(cms_id) == 2
                 : next.doodad_phases.contains(unit_id) &&
                       next.doodad_phases.at(unit_id) == doodad->at("completed_phase");
      if (!executed && !object) {
        validate_doodad_action(*data_, *doodad, next);
      }
      require(!pending_potion_ || pending_potion_->finished, "InteractionAnim_Mismatch");
      require(!pending_ || pending_->unit_id == unit_id ||
                  server_ticks > pending_->started_ticks + kInteractionTimeoutTicks,
              "InteractionAnim_Mismatch");
      if (!pending_ || pending_->unit_id != unit_id ||
          server_ticks > pending_->started_ticks + kInteractionTimeoutTicks) {
        pending_ = PendingInteraction{unit_id, doodad->at("animation_id"), server_ticks, executed};
      }
      Json started{{"interaction_anim_id", pending_->animation_id},
                   {"param", request.at("param")},
                   {"pos", position_json(position)},
                   {"tick", wire_monotonic_tick(pending_->started_ticks)},
                   {"unit_id", std::to_string(character_id_)},
                   {"weapon_state_type", kSheathedWeaponState}};
      if (object) {
        // Captured adventure-object starts (chest 100, note 305) echo the player's request yaw
        // and carry no target_unit_id; doodad starts name the doodad and use its placed yaw.
        started["yaw"] = request.contains("yaw") ? request.at("yaw").get<double>() : position.yaw;
      } else {
        started["target_unit_id"] = std::to_string(unit_id);
        started["yaw"] = doodad->at("yaw").get<double>();
      }
      packets.push_back({kInteractionAnimationStarted, std::move(started)});
      break;
    }
    case kStartDialogue: {
      const auto unit_id = parse_unit_id(request.at("target_unit_id"));
      const auto npc = data_->npcs.find(unit_id);
      require(npc != data_->npcs.end() && npc->second.value("behavior", "Civilian") != "Monster",
              "Interaction_TargetNotFound");
      require(request_position_matches(request, position) &&
                  distance_squared(position, npc->second.at("position")) <=
                      kInteractionRangeMm * kInteractionRangeMm,
              "TooFar");
      dialogue_target_ = unit_id;
      const auto &point = npc->second.at("position");
      const Json npc_position{{"millimeter_x", point.at(0)},
                              {"millimeter_y", point.at(1)},
                              {"millimeter_z", point.at(2)}};
      packets.push_back({kUnitInteracted,
                         {{"interacter_unit_id", std::to_string(character_id_)},
                          {"unit_id", std::to_string(unit_id)},
                          {"pos", npc_position},
                          {"yaw", npc->second.at("yaw")}}});
      packets.push_back(
          {kDialogueStarted,
           {{"target_unit_id", std::to_string(unit_id)}, {"target_unit_pos", npc_position}}});
      break;
    }
    case kEndDialogue:
      dialogue_target_.reset();
      packets.push_back({kDialogueEnded, {{"player_unit_id", character_id_}}});
      break;
    case kExecuteInteraction:
      require(pending_ && pending_->animation_id == request.at("interaction_anim_id"),
              "InteractionAnim_Mismatch");
      execute(next, position, server_ticks, packets, granted_instances);
      break;
    case kEndInteraction:
      require(pending_ && pending_->animation_id ==
                              request.at("end_interaction_anim").at("interaction_anim_id"),
              "InteractionAnim_Mismatch");
      pending_.reset();
      packets.push_back(
          {kInteractionAnimationResult, {{"unit_id", std::to_string(character_id_)}}});
      break;
    case kSetUtilityBag: {
      // Shared with the world session's handler; a refusal leaves `next` unchanged.
      const auto refusal = apply_utility_bag_request(request, data_->utility_items, next);
      if (refusal) {
        throw ActionError{*refusal};
      }
      packets.push_back({kUtilityBagSet, {{"bag", utility_bag_json(next)}}});
      break;
    }
    case kLevelTrigger:
      fire_level_trigger(next, request, position, server_ticks, packets);
      break;
    case kCutSceneEnd:
      end_cut_scene(request.value("cut_scene_id", 0), server_ticks, packets);
      break;
    case kTeleportStart:
      // The client acknowledges the server's TeleportReq; the capture shows no reply.
      require(pending_teleport_.has_value(), "BadRequest");
      break;
    case kTeleportCompletion:
      require(pending_teleport_.has_value(), "BadRequest");
      completed_teleport_ = pending_teleport_;
      pending_teleport_.reset();
      break;
    default:
      break;
    }
    refresh_doodad_phases(next, server_ticks, packets);
    if (granted_instances.empty()) {
      commit_progress(std::move(next));
    } else {
      const auto stored = commit_progress_with_items(std::move(next), granted_instances);
      attach_granted_items(packets, stored);
    }
    remember_started_cut_scene(packets);
    if (opcode == kExecuteInteraction && pending_) {
      pending_->executed = true;
    }
    if (opcode == kInteractUnit && std::ranges::any_of(packets, [](const auto &packet) {
          return packet.opcode == kCutScenePlay;
        })) {
      const auto unit_id = parse_unit_id(request.at("target_unit_id"));
      const auto &doodad = *data_->doodad(unit_id);
      if (doodad.contains("reset_time_ms")) {
        doodad_reset_ticks_[unit_id] =
            server_ticks + doodad.at("reset_time_ms").get<int>() * kTicksPerMillisecond;
      }
    }
    for (auto &packet : packets) {
      if (packet.opcode == kQuestAccepted || packet.opcode == kUnitInteractionResult ||
          packet.opcode == kInteractionAnimationStarted || packet.opcode == kItemsUpdated) {
        packet.message["packet_seq"] = {{"seq", std::to_string(sequence)}};
      }
    }
    return packets;
  } catch (const ActionError &error) {
    return rejection_packets(opcode, error.code, sequence);
  } catch (const Json::exception &) {
    return rejection_packets(opcode, "BadRequest", sequence);
  } catch (const std::invalid_argument &) {
    return rejection_packets(opcode, "BadRequest", sequence);
  } catch (const std::out_of_range &) {
    return rejection_packets(opcode, "BadRequest", sequence);
  }
}

} // namespace chronicles
