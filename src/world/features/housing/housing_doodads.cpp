#include "world/features/housing/housing_doodads.hpp"

#include <fstream>
#include <limits>
#include <stdexcept>

namespace chronicles::housing {
namespace {

using Json = nlohmann::json;

constexpr int kSupportedFormatVersion = 1;
constexpr double kAlwaysPercent = 100.0;
// Garden phases last up to five days; the exporter bounds timers at 30 days.
constexpr std::int64_t kMaximumPhaseDurationMs = 30LL * 24 * 60 * 60 * 1000;
constexpr int kMaximumAnimationMs = 60'000;
constexpr int kMaximumQuantity = 1'000'000;

std::runtime_error profile_error(const std::string &problem) {
  return std::runtime_error("Housing doodad profile: " + problem);
}

int bounded_int(const Json &object, const char *field, int minimum, int maximum) {
  const auto found = object.find(field);
  if (found == object.end() || !found->is_number_integer()) {
    throw profile_error(std::string(field) + " is not an integer");
  }
  const auto value = found->get<std::int64_t>();
  if (value < minimum || value > maximum) {
    throw profile_error(std::string(field) + " is out of range");
  }
  return static_cast<int>(value);
}

double rate_percent(const Json &object, const char *field) {
  const auto found = object.find(field);
  if (found == object.end() || !found->is_number()) {
    throw profile_error(std::string(field) + " is not a number");
  }
  const auto value = found->get<double>();
  if (!(value > 0.0) || value > kAlwaysPercent) {
    throw profile_error(std::string(field) + " is not a percentage");
  }
  return value;
}

DoodadReward parse_reward(const Json &entry) {
  DoodadReward reward;
  reward.pack_rate_percent = rate_percent(entry, "pack_rate_percent");
  const auto reason = entry.value("reason", std::string{});
  if (reason != "looting" && reason != "system") {
    throw profile_error("reward reason must be looting or system");
  }
  reward.looting_reason = reason == "looting";

  for (const auto &group_entry : entry.at("groups")) {
    DoodadLootGroup group;
    group.rate_percent = rate_percent(group_entry, "rate_percent");
    for (const auto &item_entry : group_entry.at("items")) {
      DoodadLootItem item;
      item.item_cms_id =
          bounded_int(item_entry, "item_cms_id", 1, std::numeric_limits<int>::max());
      item.min_quantity = bounded_int(item_entry, "min_quantity", 1, kMaximumQuantity);
      item.max_quantity =
          bounded_int(item_entry, "max_quantity", item.min_quantity, kMaximumQuantity);
      item.weight = bounded_int(item_entry, "weight", 1, std::numeric_limits<int>::max());
      group.items.push_back(item);
    }
    if (group.items.empty()) {
      throw profile_error("a reward group has no items");
    }
    reward.groups.push_back(std::move(group));
  }
  if (reward.groups.empty()) {
    throw profile_error("a reward has no groups");
  }
  return reward;
}

std::vector<DoodadPhaseChoice> parse_next(const Json &entry) {
  std::vector<DoodadPhaseChoice> choices;
  for (const auto &choice : entry.at("next")) {
    if (!choice.is_array() || choice.size() != 2) {
      throw profile_error("a next phase is not a [phase, weight] pair");
    }
    DoodadPhaseChoice parsed;
    parsed.phase_id = choice.at(0).get<int>();
    parsed.weight = choice.at(1).get<int>();
    if (parsed.phase_id <= 0 || parsed.weight <= 0) {
      throw profile_error("a next phase or weight is not positive");
    }
    choices.push_back(parsed);
  }
  if (choices.empty()) {
    throw profile_error("a transition names no next phase");
  }
  return choices;
}

DoodadTransition parse_transition(const Json &entry) {
  DoodadTransition transition;
  transition.next = parse_next(entry);
  const auto trigger = entry.value("trigger", std::string{});
  if (trigger == "timer") {
    transition.trigger = DoodadTransition::Trigger::timer;
    const auto duration = entry.at("duration_ms").get<std::int64_t>();
    if (duration <= 0 || duration > kMaximumPhaseDurationMs) {
      throw profile_error("a timer duration is out of range");
    }
    transition.duration_ms = duration;
    return transition;
  }
  if (trigger != "interaction") {
    throw profile_error("unknown transition trigger " + trigger);
  }

  transition.trigger = DoodadTransition::Trigger::interaction;
  transition.interaction_id =
      bounded_int(entry, "interaction_id", 1, std::numeric_limits<int>::max());
  transition.animation_id =
      bounded_int(entry, "interaction_anim_id", 1, std::numeric_limits<int>::max());
  for (const auto &condition_entry : entry.at("conditions")) {
    DoodadCondition condition;
    const auto kind = condition_entry.value("kind", std::string{});
    if (kind == "consume") {
      condition.kind = DoodadCondition::Kind::consume;
    } else if (kind == "own") {
      condition.kind = DoodadCondition::Kind::own;
    } else {
      throw profile_error("unknown condition kind " + kind);
    }
    condition.item_cms_id =
        bounded_int(condition_entry, "item_cms_id", 1, std::numeric_limits<int>::max());
    condition.count = bounded_int(condition_entry, "count", 1, kMaximumQuantity);
    transition.conditions.push_back(condition);
  }
  if (const auto reward = entry.find("reward"); reward != entry.end()) {
    transition.reward = parse_reward(*reward);
  }
  return transition;
}

DoodadAnimation parse_animation(const Json &entry) {
  DoodadAnimation animation;
  animation.id = bounded_int(entry, "interaction_anim_id", 1, std::numeric_limits<int>::max());
  animation.sp_cost = bounded_int(entry, "sp_cost", 0, kMaximumQuantity);
  animation.tool_equipment_type = entry.value("tool_equipment_type", std::string{"None"});
  animation.execute_time_ms = bounded_int(entry, "execute_time_ms", 1, kMaximumAnimationMs);
  animation.duration_ms =
      bounded_int(entry, "duration_ms", animation.execute_time_ms, kMaximumAnimationMs);
  return animation;
}

} // namespace

std::shared_ptr<const HousingDoodadProfile>
HousingDoodadProfile::load(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw profile_error("cannot open " + path.string());
  }
  const auto document = Json::parse(input);
  return std::make_shared<HousingDoodadProfile>(document);
}

HousingDoodadProfile::HousingDoodadProfile(const Json &document) {
  if (document.value("format_version", 0) != kSupportedFormatVersion) {
    throw profile_error("unsupported format version");
  }
  sp_regen_delay_ms_ = bounded_int(document, "sp_regen_delay_ms", 0, kMaximumAnimationMs);

  for (const auto &entry : document.at("animations")) {
    auto animation = parse_animation(entry);
    const auto id = animation.id;
    if (!animations_.emplace(id, std::move(animation)).second) {
      throw profile_error("animation " + std::to_string(id) + " repeats");
    }
  }

  for (const auto &entry : document.at("doodads")) {
    HousingDoodadDefinition definition;
    definition.doodad_id = bounded_int(entry, "doodad_id", 1, std::numeric_limits<int>::max());
    for (const auto &phase : entry.at("phases")) {
      const int phase_id = bounded_int(phase, "phase_id", 1, std::numeric_limits<int>::max());
      auto &transitions = definition.phases[phase_id];
      int timers = 0;
      for (const auto &transition_entry : phase.at("transitions")) {
        auto transition = parse_transition(transition_entry);
        if (transition.trigger == DoodadTransition::Trigger::timer) {
          ++timers;
        } else if (!animations_.contains(transition.animation_id)) {
          throw profile_error("transition names unexported animation " +
                              std::to_string(transition.animation_id));
        }
        transitions.push_back(std::move(transition));
      }
      if (timers > 1) {
        throw profile_error("doodad " + std::to_string(definition.doodad_id) + " phase " +
                            std::to_string(phase_id) + " has several timers");
      }
    }
    const auto id = definition.doodad_id;
    if (!doodads_.emplace(id, std::move(definition)).second) {
      throw profile_error("doodad " + std::to_string(id) + " repeats");
    }
  }
}

const HousingDoodadDefinition *HousingDoodadProfile::doodad(int doodad_cms_id) const {
  const auto found = doodads_.find(doodad_cms_id);
  return found == doodads_.end() ? nullptr : &found->second;
}

const DoodadAnimation *HousingDoodadProfile::animation(int animation_id) const {
  const auto found = animations_.find(animation_id);
  return found == animations_.end() ? nullptr : &found->second;
}

const DoodadTransition *HousingDoodadProfile::interaction(int doodad_cms_id, int phase_id,
                                                          int interaction_id) const {
  const auto *definition = doodad(doodad_cms_id);
  if (!definition) {
    return nullptr;
  }
  const auto phase = definition->phases.find(phase_id);
  if (phase == definition->phases.end()) {
    return nullptr;
  }
  for (const auto &transition : phase->second) {
    if (transition.trigger == DoodadTransition::Trigger::interaction &&
        transition.interaction_id == interaction_id) {
      return &transition;
    }
  }
  return nullptr;
}

const DoodadTransition *HousingDoodadProfile::timer(int doodad_cms_id, int phase_id) const {
  const auto *definition = doodad(doodad_cms_id);
  if (!definition) {
    return nullptr;
  }
  const auto phase = definition->phases.find(phase_id);
  if (phase == definition->phases.end()) {
    return nullptr;
  }
  for (const auto &transition : phase->second) {
    if (transition.trigger == DoodadTransition::Trigger::timer) {
      return &transition;
    }
  }
  return nullptr;
}

int HousingDoodadProfile::sp_regen_delay_ms() const {
  return sp_regen_delay_ms_;
}

std::size_t HousingDoodadProfile::doodad_count() const {
  return doodads_.size();
}

std::size_t HousingDoodadProfile::animation_count() const {
  return animations_.size();
}

int choose_doodad_phase(const std::vector<DoodadPhaseChoice> &choices, std::mt19937 &random) {
  std::vector<int> weights;
  weights.reserve(choices.size());
  for (const auto &choice : choices) {
    weights.push_back(choice.weight);
  }
  std::discrete_distribution<std::size_t> choose(weights.begin(), weights.end());
  return choices.at(choose(random)).phase_id;
}

std::map<int, int> roll_doodad_reward(const DoodadReward &reward, std::mt19937 &random) {
  std::uniform_real_distribution<double> percent(0.0, kAlwaysPercent);
  const auto passes = [&](double rate) {
    return rate >= kAlwaysPercent || percent(random) < rate;
  };

  std::map<int, int> granted;
  if (!passes(reward.pack_rate_percent)) {
    return granted;
  }
  for (const auto &group : reward.groups) {
    if (!passes(group.rate_percent)) {
      continue;
    }
    std::vector<int> weights;
    weights.reserve(group.items.size());
    for (const auto &item : group.items) {
      weights.push_back(item.weight);
    }
    std::discrete_distribution<std::size_t> choose(weights.begin(), weights.end());
    const auto &item = group.items.at(choose(random));
    std::uniform_int_distribution<int> quantity(item.min_quantity, item.max_quantity);
    auto &total = granted[item.item_cms_id];
    const int rolled = quantity(random);
    if (total > std::numeric_limits<int>::max() - rolled) {
      throw std::overflow_error("Housing doodad reward overflows a stack");
    }
    total += rolled;
  }
  return granted;
}

} // namespace chronicles::housing
