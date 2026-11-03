#include "world/features/guides/contents_guides.hpp"

#include "cms.generated.pb.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace chronicles::guides {
namespace {

using Json = nlohmann::json;

constexpr std::uint16_t kContentsGuideNotification = 105;
constexpr std::uint16_t kInteractUnitResponse = 163;
constexpr std::uintmax_t kMaximumProfileBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaximumGuides = 4096;
constexpr std::size_t kMaximumMaps = 4096;
constexpr std::size_t kMaximumConditionsPerGuide = 1024;
constexpr std::size_t kMaximumVolumesPerMap = 10000;

void validate_conditions(const Json &conditions) {
  if (!conditions.is_array() || conditions.size() > kMaximumConditionsPerGuide) {
    throw std::runtime_error("Invalid contents guide conditions");
  }
  std::map<int, std::string> operators;
  for (const auto &condition : conditions) {
    const int group = condition.at("group");
    const auto operation = condition.at("operator").get<std::string>();
    const auto kind = condition.at("kind").get<std::string>();
    if (group <= 0 || (operation != "AND" && operation != "OR") ||
        !condition.at("negated").is_boolean()) {
      throw std::runtime_error("Invalid contents guide condition group");
    }
    const auto [found, inserted] = operators.emplace(group, operation);
    if (!inserted && found->second != operation) {
      throw std::runtime_error("Mixed contents guide operators in one group");
    }
    if (kind == "TriggerVolumeEnter" || kind == "TriggerVolumeLeave") {
      if (condition.at("volume_id").get<std::string>().empty()) {
        throw std::runtime_error("Empty contents guide volume id");
      }
    } else if (kind == "Quest") {
      const auto requirement = condition.at("requirement").get<std::string>();
      if (condition.at("quest_id").get<int>() <= 0 ||
          (requirement != "Acceptable" && requirement != "Progressing" &&
           requirement != "QuestComplete" && requirement != "QuestProgress")) {
        throw std::runtime_error("Invalid contents guide quest condition");
      }
      if (condition.contains("progress_group") &&
          condition.at("progress_group").get<int>() <= 0) {
        throw std::runtime_error("Invalid contents guide quest group");
      }
    } else if (kind == "OwnItem") {
      if (condition.at("item_id").get<int>() <= 0 || condition.at("count").get<int>() <= 0 ||
          !condition.at("grade").is_string()) {
        throw std::runtime_error("Invalid contents guide item condition");
      }
    } else if (kind == "Interaction") {
      if (condition.at("interaction_id").get<int>() <= 0) {
        throw std::runtime_error("Invalid contents guide interaction condition");
      }
    } else if (kind == "CutSceneEnd") {
      if (condition.at("cut_scene_id").get<int>() <= 0) {
        throw std::runtime_error("Invalid contents guide cutscene condition");
      }
    } else if (kind == "CurrentWeaponType") {
      if (condition.at("weapon_type").get<std::string>().empty()) {
        throw std::runtime_error("Invalid contents guide weapon condition");
      }
    } else if (kind != "Unsupported") {
      throw std::runtime_error("Unknown contents guide condition kind");
    }
  }
}

void validate_volume(const Json &volume) {
  if (volume.at("id").get<std::string>().empty() || !volume.at("center").is_array() ||
      volume.at("center").size() != 3) {
    throw std::runtime_error("Invalid contents guide volume");
  }
  for (const auto &axis : volume.at("center")) {
    if (!axis.is_number() || !std::isfinite(axis.get<double>())) {
      throw std::runtime_error("Invalid contents guide volume center");
    }
  }
  if (volume.contains("radius")) {
    if (!volume.at("radius").is_number() || volume.at("radius").get<double>() <= 0 ||
        !std::isfinite(volume.at("radius").get<double>())) {
      throw std::runtime_error("Invalid contents guide volume radius");
    }
    return;
  }
  if (!volume.at("extent").is_array() || volume.at("extent").size() != 3 ||
      !volume.at("yaw").is_number() || !std::isfinite(volume.at("yaw").get<double>())) {
    throw std::runtime_error("Invalid contents guide box");
  }
  for (const auto &axis : volume.at("extent")) {
    if (!axis.is_number() || axis.get<double>() <= 0 || !std::isfinite(axis.get<double>())) {
      throw std::runtime_error("Invalid contents guide box extent");
    }
  }
}

struct Evaluation {
  const WorldProgress &progress;
  const Character &character;
  const std::set<std::string> &inside;
  const std::set<std::string> &left;
  const std::set<int> &interactions;
  std::optional<int> completed_cut_scene;
  const quest_rules::QuestLookup &quests;
  const std::map<int, ItemDescription> &items;
};

bool owns_item(const Json &condition, const Evaluation &view) {
  const int item_id = condition.at("item_id");
  const auto required_grade = condition.value("grade", std::string());
  std::int64_t count = 0;
  const auto stack = view.progress.stackables.find(item_id);
  const auto description = view.items.find(item_id);
  if (stack != view.progress.stackables.end() && description != view.items.end() &&
      (required_grade.empty() || required_grade == "EX5ItemGrade_" + description->second.grade)) {
    count += stack->second;
  }
  for (const auto &item : view.character.items) {
    if (item.cms_id != item_id || !x5::cms::EX5ItemGrade_IsValid(item.grade)) {
      continue;
    }
    if (required_grade.empty() || required_grade == x5::cms::EX5ItemGrade_Name(item.grade)) {
      count += item.quantity;
    }
  }
  return count >= condition.value("count", 1);
}

bool active_weapon_matches(const Json &condition, const Evaluation &view) {
  const int slot = view.progress.build.active_weapon_slot;
  const auto item = std::ranges::find_if(view.character.items, [slot](const CharacterItem &entry) {
    return entry.equip_slot == slot && entry.durability > 0;
  });
  if (item == view.character.items.end()) {
    return false;
  }
  const auto description = view.items.find(item->cms_id);
  return description != view.items.end() &&
         description->second.weapon_type == condition.at("weapon_type").get<std::string>();
}

bool condition_matches(const Json &condition, const Evaluation &view) {
  const auto kind = condition.at("kind").get<std::string>();
  if (kind == "TriggerVolumeEnter") {
    return view.inside.contains(condition.at("volume_id").get<std::string>());
  }
  if (kind == "TriggerVolumeLeave") {
    return view.left.contains(condition.at("volume_id").get<std::string>());
  }
  if (kind == "Quest") {
    return quest_rules::quest_requirement_met(view.quests, condition, view.progress);
  }
  if (kind == "OwnItem") {
    return owns_item(condition, view);
  }
  if (kind == "CurrentWeaponType") {
    return active_weapon_matches(condition, view);
  }
  if (kind == "Interaction") {
    return view.interactions.contains(condition.at("interaction_id").get<int>());
  }
  if (kind == "CutSceneEnd") {
    return view.completed_cut_scene == condition.at("cut_scene_id").get<int>();
  }
  return false;
}

bool requirements_match(const Json &conditions, const Evaluation &view,
                        bool require_spatial = false) {
  if (conditions.empty()) {
    return false;
  }
  std::map<int, std::vector<const Json *>> groups;
  for (const auto &condition : conditions) {
    groups[condition.at("group").get<int>()].push_back(&condition);
  }
  for (const auto &[group, members] : groups) {
    static_cast<void>(group);
    const bool all = members.front()->at("operator") == "AND";
    bool matched = all;
    bool unknown = false;
    bool spatial = false;
    for (const auto *condition : members) {
      if (condition->at("kind") == "Unsupported") {
        unknown = true;
        continue;
      }
      const bool value = condition_matches(*condition, view) != condition->value("negated", false);
      spatial = spatial || (condition->at("kind") == "TriggerVolumeEnter" && value);
      matched = all ? matched && value : matched || value;
    }
    // A true OR branch is decisive even when another branch is unknown. An AND
    // group is true only when every condition is understood and true.
    const bool definitely_matched = matched && (!all || !unknown);
    if (definitely_matched && (!require_spatial || spatial)) {
      return true;
    }
  }
  return false;
}

std::set<std::string> current_volumes(const Catalog &catalog, const WorldPosition &position) {
  std::set<std::string> inside;
  const auto *volumes = catalog.volumes(position.map_id);
  if (!volumes) {
    return inside;
  }
  for (const auto &volume : *volumes) {
    if (quest_rules::position_within_trigger(volume, position, 0)) {
      inside.insert(volume.at("id").get<std::string>());
    }
  }
  return inside;
}

std::set<int> successful_interactions(const std::vector<GameplayPacket> &packets) {
  std::set<int> interactions;
  for (const auto &packet : packets) {
    if (packet.opcode != kInteractUnitResponse ||
        (packet.message.contains("ec") && packet.message.at("ec") != "Success") ||
        !packet.message.contains("interaction_id")) {
      continue;
    }
    interactions.insert(packet.message.at("interaction_id").get<int>());
  }
  return interactions;
}

GameplayPacket notification(int guide_id, bool start) {
  return {kContentsGuideNotification,
          {{"contents_guide_cms_id", guide_id}, {"start_trigger", start}}};
}

} // namespace

std::shared_ptr<const Catalog> Catalog::load(const std::filesystem::path &path) {
  if (std::filesystem::file_size(path) > kMaximumProfileBytes) {
    throw std::runtime_error("Contents guide profile exceeds its size limit");
  }
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Cannot open contents guide profile");
  }
  return std::make_shared<const Catalog>(Json::parse(input));
}

Catalog::Catalog(Json document) : document_(std::move(document)) {
  if (document_.at("format_version") != 1 ||
      document_.at("build_id") != "40367e24241c9481bfc07fbf9ef1919f25fb0eaa" ||
      !document_.at("guides").is_array() || document_.at("guides").size() > kMaximumGuides ||
      !document_.at("maps").is_array() || document_.at("maps").size() > kMaximumMaps) {
    throw std::runtime_error("Invalid contents guide profile");
  }
  std::set<int> ids;
  for (const auto &guide : document_.at("guides")) {
    const int id = guide.at("id");
    const auto type = guide.at("type").get<std::string>();
    if (id <= 0 || !ids.insert(id).second || (type != "Text" && type != "ControlTip")) {
      throw std::runtime_error("Invalid or duplicate contents guide");
    }
    validate_conditions(guide.at("start"));
    validate_conditions(guide.at("end"));
  }
  for (const auto &map : document_.at("maps")) {
    const int id = map.at("map_id");
    if (id <= 0 || !map.at("volumes").is_array() ||
        map.at("volumes").size() > kMaximumVolumesPerMap ||
        !maps_.emplace(id, map.at("volumes")).second) {
      throw std::runtime_error("Invalid or duplicate contents guide map");
    }
    std::set<std::string> volume_ids;
    for (const auto &volume : map.at("volumes")) {
      validate_volume(volume);
      if (!volume_ids.insert(volume.at("id").get<std::string>()).second) {
        throw std::runtime_error("Duplicate contents guide volume");
      }
    }
  }
}

const Json &Catalog::guides() const {
  return document_.at("guides");
}

const Json *Catalog::volumes(int map_id) const {
  const auto found = maps_.find(map_id);
  return found == maps_.end() ? nullptr : &found->second;
}

Session::Session(std::shared_ptr<const Catalog> catalog, quest_rules::QuestLookup quests,
                 std::shared_ptr<const std::map<int, ItemDescription>> items)
    : catalog_(std::move(catalog)), quests_(std::move(quests)), items_(std::move(items)) {
  if (!catalog_ || !quests_ || !items_) {
    throw std::invalid_argument("Contents guide session requires a catalog and quest lookup");
  }
}

std::vector<GameplayPacket> Session::observe(const WorldPosition &position,
                                             std::optional<WorldProgress> progress,
                                             std::optional<Character> character,
                                             const std::vector<GameplayPacket> &action_packets,
                                             std::optional<int> completed_cut_scene) {
  const bool first_observation = !previous_;
  if (first_observation) {
    if (!progress || !character) {
      return {};
    }
    previous_ = Snapshot{position, *progress, *character};
  }
  const bool map_changed = position.map_id != previous_->position.map_id;
  const auto interactions = successful_interactions(action_packets);
  const auto &prior_position = previous_->position;
  const bool same_place = !map_changed && position.x_mm == prior_position.x_mm &&
                          position.y_mm == prior_position.y_mm && position.z_mm == prior_position.z_mm;
  if (!first_observation && same_place && !progress && !character && interactions.empty() &&
      !completed_cut_scene) {
    return {};
  }
  const auto current_inside = current_volumes(*catalog_, position);
  if (!first_observation && !map_changed && current_inside == inside_ && !progress && !character &&
      interactions.empty() && !completed_cut_scene) {
    previous_->position = position;
    return {};
  }
  const std::set<std::string> empty_volumes;
  const std::set<int> empty_interactions;
  std::set<std::string> left;
  if (!map_changed) {
    std::ranges::set_difference(inside_, current_inside, std::inserter(left, left.end()));
  }
  const auto &next_progress = progress ? *progress : previous_->progress;
  const auto &next_character = character ? *character : previous_->character;
  const Evaluation before{previous_->progress, previous_->character,
                          map_changed ? empty_volumes : inside_, empty_volumes,
                          empty_interactions, {}, quests_, *items_};
  const Evaluation after{next_progress, next_character, current_inside, left,
                         interactions, completed_cut_scene, quests_, *items_};

  std::vector<GameplayPacket> packets;
  if (map_changed) {
    for (const int id : active_) {
      packets.push_back(notification(id, false));
    }
    active_.clear();
  }
  for (const auto &guide : catalog_->guides()) {
    const int id = guide.at("id");
    const auto &start = guide.at("start");
    const auto &end = guide.at("end");
    const bool start_now = requirements_match(start, after, first_observation || map_changed);
    const bool was_started = !map_changed && requirements_match(start, before);
    const bool end_now = requirements_match(end, after);
    if (active_.contains(id) && end_now) {
      packets.push_back(notification(id, false));
      active_.erase(id);
    }
    if (active_.contains(id) || shown_.contains(id) || end_now || !start_now || was_started) {
      continue;
    }
    packets.push_back(notification(id, true));
    if (end.empty()) {
      shown_.insert(id);
    } else {
      active_.insert(id);
    }
  }
  previous_->position = position;
  if (progress) {
    previous_->progress = std::move(*progress);
  }
  if (character) {
    previous_->character = std::move(*character);
  }
  inside_ = current_inside;
  return packets;
}

} // namespace chronicles::guides
