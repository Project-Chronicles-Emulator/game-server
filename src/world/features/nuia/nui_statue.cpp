#include "world/features/nuia/nui_statue.hpp"

#include "common/clock_ticks.hpp"
#include "world/core/world_request_opcodes.hpp"
#include "world/features/quests/quest_rules.hpp"
#include "world/features/inventory/utility_bag.hpp"

#include <algorithm>
#include <charconv>
#include <climits>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace chronicles {
namespace {

using Json = nlohmann::json;
using quest_rules::require;

constexpr std::uint16_t kDiscover = world_request::kDiscoverAdventureObjectRequest;
constexpr std::uint16_t kActivate = world_request::kActivateAdventureObjectRequest;
constexpr std::uint16_t kInteract = world_request::kInteractionUnitRequest;
constexpr std::uint16_t kStart = world_request::kInteractionStartRequest;
constexpr std::uint16_t kExecute = 20070;
constexpr std::uint16_t kEnd = 20071;
constexpr std::uint16_t kOffer = 20077;
constexpr std::int64_t kTicksPerSecond = 10'000'000;
constexpr std::int64_t kTicksPerMillisecond = 10'000;
constexpr std::int64_t kActionLifetime = 30 * kTicksPerSecond;
constexpr std::int64_t kTimingAllowance = 100 * kTicksPerMillisecond;
constexpr std::size_t kMaximumPlacements = 100'000;
constexpr int kTearItem = 2000;
constexpr int kActivated = 2;

int integer(const Json &value, int minimum, int maximum) {
  if (!value.is_number_integer() ||
      (value.is_number_unsigned() &&
       value.get<std::uint64_t>() > static_cast<std::uint64_t>(maximum))) {
    throw std::invalid_argument("Nui integer has an invalid type or range");
  }
  const auto number = value.get<std::int64_t>();
  if (number < minimum || number > maximum) {
    throw std::invalid_argument("Nui integer is outside its allowed range");
  }
  return static_cast<int>(number);
}

std::uint64_t unit_id(const Json &value) {
  if (!value.is_string()) {
    throw std::invalid_argument("Nui target must be a decimal unit ID");
  }
  const auto &text = value.get_ref<const std::string &>();
  std::uint64_t result{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
  if (error != std::errc{} || end != text.data() + text.size() || result == 0) {
    throw std::invalid_argument("Invalid Nui unit ID");
  }
  return result;
}

WorldPosition position(const Json &point, int map_id, float yaw) {
  if (!point.is_array() || point.size() != 3) {
    throw std::invalid_argument("Nui point must have three coordinates");
  }
  return {map_id, integer(point[0], INT_MIN, INT_MAX), integer(point[1], INT_MIN, INT_MAX),
          integer(point[2], INT_MIN, INT_MAX), yaw};
}

double distance_squared(const WorldPosition &first, const WorldPosition &second) {
  const double x = static_cast<double>(first.x_mm) - second.x_mm;
  const double y = static_cast<double>(first.y_mm) - second.y_mm;
  const double z = static_cast<double>(first.z_mm) - second.z_mm;
  return x * x + y * y + z * z;
}

Json object_state(int cms_id, int state) {
  return {{"cms_id", cms_id},
          {"state", state == kActivated ? "EX5AdventureObjectState_Activated"
                                        : "EX5AdventureObjectState_Discovered"}};
}

std::vector<GameplayPacket> offering_packets(const WorldProgress &progress,
                                             std::uint32_t sequence) {
  const auto tears = progress.stackables.find(kTearItem);
  const int quantity = tears == progress.stackables.end() ? 0 : tears->second;
  return {{169,
           {{"use", Json::object()},
            {"packet_seq", {{"seq", std::to_string(sequence)}}},
            {"stackables", {{{"item_cms_id", kTearItem}, {"quantity", quantity}}}}}},
          {74,
           {{"level", progress.nui_blessing_level},
            {"dedicated_nui_tears", progress.dedicated_nui_tears}}}};
}

GameplayPacket failure(std::uint16_t opcode, std::uint32_t sequence, std::uint64_t player_id,
                       const std::string &code) {
  const Json packet_sequence{{"seq", std::to_string(sequence)}};
  if (opcode == kStart) {
    return {165,
            {{"ec", code},
             {"packet_seq", packet_sequence},
             {"weapon_state_type", "EX5WeaponStateType_Sheathed"}}};
  }
  if (opcode == kExecute || opcode == kEnd) {
    return {167, {{"ec", code}, {"unit_id", std::to_string(player_id)}}};
  }
  if (opcode == kActivate) {
    return {91, {{"ec", code}}};
  }
  return {2, {{"ec", code}, {"packet_seq", packet_sequence}}};
}

} // namespace

std::shared_ptr<const NuiStatueData> NuiStatueData::load(const std::filesystem::path &path) {
  if (std::filesystem::file_size(path) > 16 * 1024 * 1024) {
    throw std::runtime_error("Nui shrine definitions exceed the size limit");
  }
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Cannot open Nui shrine definitions");
  }
  Json document;
  input >> document;
  return std::make_shared<const NuiStatueData>(document);
}

NuiStatueData::NuiStatueData(const Json &document) {
  try {
    if (document.at("format_version") != 1 ||
        document.at("build_id") != "40367e24241c9481bfc07fbf9ef1919f25fb0eaa" ||
        document.at("source_executable_dump_sha256") !=
            "e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6" ||
        !document.at("placements").is_array() ||
        document.at("placements").size() > kMaximumPlacements) {
      throw std::invalid_argument("Unsupported Nui shrine definitions");
    }
    for (int interaction_id : {10, 16}) {
      const auto &timing = document.at("interactions").at(std::to_string(interaction_id));
      const int execute_ms = integer(timing.at("execute_time_ms"), 1, 30'000);
      const int duration_ms = integer(timing.at("duration_ms"), 1, 30'000);
      if (execute_ms >= duration_ms) {
        throw std::invalid_argument("Nui execute event must precede montage end");
      }
      execute_times_ms_.emplace(interaction_id, execute_ms);
    }
    for (const auto &map_id : document.at("flame_only_maps")) {
      flame_only_maps_.insert(integer(map_id, 1, INT_MAX));
    }
    for (const auto &[key, capacity] : document.at("utility_capacity").items()) {
      int item_id{};
      const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), item_id);
      if (error != std::errc{} || end != key.data() + key.size() || item_id <= 0) {
        throw std::invalid_argument("Invalid Nui refill item ID");
      }
      utility_capacity_.emplace(item_id, integer(capacity, 1, 1000));
    }
    if (document.contains("utility_groups")) {
      for (const auto &[key, tiers] : document.at("utility_groups").items()) {
        const int group_id = integer(Json::parse(key), 1, INT_MAX);
        if (!tiers.is_array() || tiers.size() != 6) {
          throw std::invalid_argument("Nui refill group must have six capacity tiers");
        }
        std::array<int, 6> counts{};
        for (std::size_t index = 0; index < counts.size(); ++index) {
          counts[index] = integer(tiers[index], 1, 1000);
        }
        utility_group_tiers_.emplace(group_id, counts);
      }
      for (const auto &[key, group] : document.at("utility_items").items()) {
        const int item_id = integer(Json::parse(key), 1, INT_MAX);
        const int group_id = integer(group, 1, INT_MAX);
        if (!utility_group_tiers_.contains(group_id)) {
          throw std::invalid_argument("Nui item references an unknown refill group");
        }
        utility_item_groups_.emplace(item_id, group_id);
      }
      for (const auto &[key, options] : document.at("bag_options").items()) {
        const int item_id = integer(Json::parse(key), 1, INT_MAX);
        const int level = integer(options.at("level"), 1, 1000);
        std::set<int> groups;
        for (const auto &value : options.at("groups")) {
          const int group = integer(value, 1, INT_MAX);
          if (!utility_group_tiers_.contains(group)) {
            throw std::invalid_argument("Nui bag references an unknown refill group");
          }
          groups.insert(group);
        }
        bag_options_.emplace(item_id, std::pair{level, std::move(groups)});
      }
    }
    if (document.contains("revive_tasks")) {
      for (const auto &[key, task] : document.at("revive_tasks").items()) {
        const int task_id = integer(Json::parse(key), 1, INT_MAX);
        RevivalTask parsed;
        parsed.refill_utility_bag = task.value("refillUtilityBag", false);
        if (!task.at("supplies").is_array() || task.at("supplies").size() > 64) {
          throw std::invalid_argument("Too many or invalid Nui revival supplies");
        }
        for (const auto &supply : task.at("supplies")) {
          const int item_id = integer(supply.at("itemId"), 1, INT_MAX);
          const int maximum = integer(supply.at("maxCount"), 1, 1000);
          if (!parsed.supplies.emplace(item_id, maximum).second) {
            throw std::invalid_argument("Duplicate Nui revival supply");
          }
        }
        revival_tasks_.emplace(task_id, std::move(parsed));
      }
    }
    if (document.contains("blessing_levels")) {
      for (const auto &level : document.at("blessing_levels")) {
        if (integer(level.at("level"), 1, 30) != required_tears_.size() + 1) {
          throw std::invalid_argument("Nui blessing levels must be consecutive");
        }
        required_tears_.push_back(integer(level.at("required_tears"), 1, 100'000));
      }
    }
    for (const auto &entry : document.at("placements")) {
      const int map_id = integer(entry.at("map_id"), 1, INT_MAX);
      const auto kind = entry.at("kind").get<std::string>();
      if (kind != "statue" && kind != "flame") {
        throw std::invalid_argument("Unknown Nui shrine kind");
      }
      const double yaw = entry.at("spawn_yaw").get<double>();
      if (!std::isfinite(yaw) || std::abs(yaw) > std::numeric_limits<float>::max()) {
        throw std::invalid_argument("Invalid Nui shrine yaw");
      }
      NuiShrinePlacement placement;
      placement.flame = kind == "flame";
      placement.unit_id = unit_id(entry.at("unit_id"));
      placement.cms_id = integer(entry.at("cms_id"), placement.flame ? 0 : 1, INT_MAX);
      placement.position = position(entry.at("position"), map_id, 0);
      placement.spawn = position(entry.at("spawn"), map_id, static_cast<float>(yaw));
      placement.restricted_combat_radius_mm =
          integer(entry.at("restricted_combat_radius_mm"), 0, 1'000'000);
      placement.revive_task_id = integer(entry.at("revive_task_id"), 0, INT_MAX);
      if (!placements_.emplace(std::pair{map_id, placement.unit_id}, placement).second) {
        throw std::invalid_argument("Duplicate Nui shrine placement");
      }
    }
  } catch (const std::exception &error) {
    throw std::runtime_error(std::string("Invalid Nui shrine definitions: ") + error.what());
  }
}

const NuiShrinePlacement *NuiStatueData::find(int map_id, std::uint64_t id) const {
  const auto found = placements_.find({map_id, id});
  return found == placements_.end() ? nullptr : &found->second;
}

int NuiStatueData::execute_time_ms(int interaction_id) const {
  const auto found = execute_times_ms_.find(interaction_id);
  return found == execute_times_ms_.end() ? 0 : found->second;
}

int NuiStatueData::utility_capacity(int item_cms_id, const Character *character) const {
  const auto found = utility_capacity_.find(item_cms_id);
  if (found == utility_capacity_.end()) {
    return 0;
  }
  const auto group = utility_item_groups_.find(item_cms_id);
  if (!character || group == utility_item_groups_.end()) {
    return found->second;
  }
  constexpr int kBagEquipSlot = 24;
  for (const auto &item : character->items) {
    if (item.equip_slot != kBagEquipSlot) {
      continue;
    }
    const auto bag = bag_options_.find(item.cms_id);
    if (bag == bag_options_.end() || !bag->second.second.contains(group->second)) {
      return found->second;
    }
    const int tier = bag->second.first - 1;
    const auto &tiers = utility_group_tiers_.at(group->second);
    // The recovered effective getter falls back to tier zero for invalid indices;
    // its tooltip's display-only clamp to five is not the runtime rule.
    return tier >= 0 && tier < static_cast<int>(tiers.size()) ? tiers[tier] : tiers[0];
  }
  return found->second;
}

void NuiStatueData::supplement_utility_bag(WorldProgress &progress,
                                           const Character *character) const {
  std::map<int, int> available;
  for (const auto &slot : progress.utility_slots) {
    const auto owned = progress.stackables.find(slot.item_cms_id);
    if (owned != progress.stackables.end()) {
      available[slot.item_cms_id] = owned->second;
    }
  }
  for (const auto &slot : progress.utility_slots) {
    require(slot.count >= 0 && available[slot.item_cms_id] >= slot.count, "BadRequest");
    available[slot.item_cms_id] -= slot.count;
  }
  for (auto &slot : progress.utility_slots) {
    const int capacity = utility_capacity(slot.item_cms_id, character);
    const int added = std::min(std::max(0, capacity - slot.count), available[slot.item_cms_id]);
    slot.count += added;
    available[slot.item_cms_id] -= added;
  }
}

std::vector<GameplayPacket>
NuiStatueData::apply_revival_supplies(int task_id, WorldProgress &progress,
                                      const Character *character) const {
  if (task_id == 0) {
    return {};
  }
  const auto found = revival_tasks_.find(task_id);
  require(found != revival_tasks_.end(), "BadRequest");
  Json stackables = Json::array();
  for (const auto &[item_id, maximum] : found->second.supplies) {
    require(progress.stackables.contains(item_id) ||
                progress.stackables.size() < kMaximumStackableKinds,
            "BadRequest");
    auto &owned = progress.stackables[item_id];
    if (owned < maximum) {
      owned = maximum;
      stackables.push_back({{"item_cms_id", item_id}, {"quantity", owned}});
    }
  }
  std::vector<GameplayPacket> packets;
  if (!stackables.empty()) {
    packets.push_back({169, {{"system", Json::object()}, {"stackables", std::move(stackables)}}});
  }
  if (found->second.refill_utility_bag) {
    supplement_utility_bag(progress, character);
    packets.push_back({104, {{"bag", utility_bag_json(progress)}}});
  }
  return packets;
}

int NuiStatueData::remaining_tears(const WorldProgress &progress) const {
  require(progress.nui_blessing_level >= 0 &&
              static_cast<std::size_t>(progress.nui_blessing_level) <= required_tears_.size() &&
              progress.dedicated_nui_tears >= 0,
          "BadRequest");
  int remaining = 0;
  for (std::size_t index = progress.nui_blessing_level; index < required_tears_.size(); ++index) {
    remaining += required_tears_[index];
  }
  require(progress.dedicated_nui_tears <= remaining, "BadRequest");
  return remaining - progress.dedicated_nui_tears;
}

void NuiStatueData::dedicate(WorldProgress &progress, int count) const {
  require(count > 0 && count <= remaining_tears(progress), "BadRequest");
  int dedicated = progress.dedicated_nui_tears + count;
  while (static_cast<std::size_t>(progress.nui_blessing_level) < required_tears_.size()) {
    const int needed = required_tears_.at(progress.nui_blessing_level);
    if (dedicated < needed) {
      break;
    }
    dedicated -= needed;
    ++progress.nui_blessing_level;
  }
  progress.dedicated_nui_tears = dedicated;
}

const NuiShrinePlacement *NuiStatueData::revival_shrine(const WorldPosition &current,
                                                        const WorldProgress &progress,
                                                        bool to_flame) const {
  const bool flame = to_flame || flame_only_maps_.contains(current.map_id);
  const NuiShrinePlacement *nearest = nullptr;
  double best_distance = std::numeric_limits<double>::infinity();
  for (const auto &[identity, placement] : placements_) {
    if (identity.first != current.map_id || placement.flame != flame) {
      continue;
    }
    const auto activated = progress.adventure_objects.find(placement.cms_id);
    if (!flame &&
        (activated == progress.adventure_objects.end() || activated->second != kActivated)) {
      continue;
    }
    const double distance = distance_squared(current, placement.spawn);
    if (distance < best_distance) {
      nearest = &placement;
      best_distance = distance;
    }
  }
  return nearest;
}

std::optional<WorldPosition> NuiStatueData::revival_destination(const WorldPosition &current,
                                                                const WorldProgress &progress,
                                                                bool to_flame) const {
  const auto *shrine = revival_shrine(current, progress, to_flame);
  return shrine ? std::optional{shrine->spawn} : std::nullopt;
}

NuiStatueSession::NuiStatueSession(std::shared_ptr<const NuiStatueData> data,
                                   std::uint64_t player_unit_id, Commit commit)
    : data_(std::move(data)), player_unit_id_(player_unit_id), commit_(std::move(commit)) {
  if (!data_ || !commit_ || player_unit_id_ == 0) {
    throw std::invalid_argument("Nui Statue session requires data, player and commit");
  }
}

bool NuiStatueSession::handles(std::uint16_t opcode, const Json &request,
                               const WorldPosition &current) const {
  try {
    if (opcode == kExecute || opcode == kEnd) {
      return action_.has_value();
    }
    if (opcode == kStart) {
      const int animation = request.value("interaction_anim_id", 0);
      if (animation == 10 || animation == 16) {
        return true;
      }
    }
    if (opcode == kOffer && request.value("item_cms_id", 0) == kTearItem) {
      return true;
    }
    if (opcode != kDiscover && opcode != kActivate && opcode != kInteract) {
      return false;
    }
    const char *field = opcode == kInteract ? "target_unit_id" : "spot_object_id";
    const auto *placement = data_->find(current.map_id, unit_id(request.at(field)));
    return placement && !placement->flame;
  } catch (const std::exception &) {
    return false;
  }
}

bool NuiStatueSession::busy() const {
  return action_.has_value();
}

void NuiStatueSession::cancel_action() {
  action_.reset();
}

std::optional<std::int64_t> NuiStatueSession::next_action_tick() const {
  return action_ ? std::optional{action_->started_tick + kActionLifetime} : std::nullopt;
}

std::vector<GameplayPacket> NuiStatueSession::advance(std::int64_t now,
                                                      const WorldPosition &current) {
  if (!action_ ||
      (current.map_id == action_->map_id && now < action_->started_tick + kActionLifetime)) {
    return {};
  }
  action_.reset();
  return {failure(kEnd, 0, player_unit_id_, "InteractionAnim_Mismatch")};
}

void NuiStatueSession::commit_progress(WorldProgress &current, WorldProgress next) {
  if (current == next) {
    return;
  }
  require(current.revision < std::numeric_limits<std::int64_t>::max(), "BadRequest");
  next.revision = current.revision + 1;
  require(commit_(current, next), "BadRequest");
  current = std::move(next);
}

std::vector<GameplayPacket> NuiStatueSession::execute(const WorldPosition &current,
                                                      std::int64_t now, WorldProgress &progress,
                                                      const Character *character) {
  require(action_ && action_->map_id == current.map_id, "InteractionAnim_Mismatch");
  const auto *placement = data_->find(current.map_id, action_->target_unit_id);
  require(placement && !placement->flame, "Interaction_TargetNotFound");
  constexpr double range = quest_rules::kInteractionRangeMm;
  require(distance_squared(current, placement->position) <= range * range, "TooFar");
  const auto execute_tick = action_->started_tick +
                            data_->execute_time_ms(action_->interaction_id) * kTicksPerMillisecond;
  require(now + kTimingAllowance >= execute_tick, "InteractionAnim_Mismatch");
  require(now < action_->started_tick + kActionLifetime, "InteractionAnim_Mismatch");
  if (action_->executed) {
    return {{167, {{"unit_id", std::to_string(player_unit_id_)}}}};
  }

  auto next = progress;
  std::vector<GameplayPacket> packets;
  if (action_->interaction_id == 10) {
    require(next.adventure_objects.contains(placement->cms_id) ||
                next.adventure_objects.size() < 4096,
            "BadRequest");
    next.adventure_objects[placement->cms_id] = kActivated;
    packets.push_back({91, {{"adventure_object", object_state(placement->cms_id, kActivated)}}});
  } else {
    // Utility slots reference owned stacks; supplementing never manufactures or spends
    // inventory. Capacity and equipped-bag group tiers are catalog-derived.
    data_->supplement_utility_bag(next, character);
    packets.push_back({104, {{"bag", utility_bag_json(next)}}});
  }
  commit_progress(progress, std::move(next));
  action_->executed = true;
  packets.push_back({167, {{"unit_id", std::to_string(player_unit_id_)}}});
  return packets;
}

std::vector<GameplayPacket> NuiStatueSession::receive(std::uint16_t opcode, const Json &request,
                                                      std::uint32_t sequence,
                                                      const WorldPosition &current,
                                                      std::int64_t now, WorldProgress &progress,
                                                      bool other_busy, const Character *character) {
  try {
    require(now > 0 && now <= std::numeric_limits<std::int64_t>::max() - kActionLifetime,
            "BadRequest");
    if (now - request_window_tick_ >= kTicksPerSecond) {
      request_window_tick_ = now;
      requests_in_window_ = 0;
    }
    require(++requests_in_window_ <= 32, "BadRequest");
    if (action_ &&
        (action_->map_id != current.map_id || now >= action_->started_tick + kActionLifetime)) {
      action_.reset();
    }
    // Automatic proximity discovery must not cover the restored death/revival UI.
    // Explicit activation, offerings and interaction actions remain unavailable.
    if (opcode == kDiscover && progress.dead) {
      return {};
    }
    require(!progress.dead, "BadRequest");
    if (opcode == kExecute) {
      require(action_ && request.value("interaction_anim_id", 0) == action_->interaction_id,
              "InteractionAnim_Mismatch");
      return execute(current, now, progress, character);
    }
    if (opcode == kEnd) {
      require(action_ && request.at("end_interaction_anim").value("interaction_anim_id", 0) ==
                             action_->interaction_id,
              "InteractionAnim_Mismatch");
      action_.reset();
      return {{167, {{"unit_id", std::to_string(player_unit_id_)}}}};
    }

    const char *target_field =
        (opcode == kDiscover || opcode == kActivate) ? "spot_object_id" : "target_unit_id";
    const auto target = unit_id(request.at(target_field));
    const auto *placement = data_->find(current.map_id, target);
    require(placement && !placement->flame, "Interaction_TargetNotFound");
    if (opcode == kDiscover) {
      require(quest_rules::within_discovery_range(current, placement->position.x_mm,
                                                  placement->position.y_mm),
              "TooFar");
    } else {
      constexpr double range = quest_rules::kInteractionRangeMm;
      require(distance_squared(current, placement->position) <= range * range, "TooFar");
    }
    if (opcode != kOffer) {
      require(quest_rules::request_position_matches(request, current), "BadRequest");
    }
    if (opcode == kDiscover) {
      auto next = progress;
      require(next.adventure_objects.contains(placement->cms_id) ||
                  next.adventure_objects.size() < 4096,
              "BadRequest");
      next.adventure_objects.try_emplace(placement->cms_id, 1);
      commit_progress(progress, std::move(next));
      return {{90,
               {{"adventure_objects",
                 {object_state(placement->cms_id,
                               progress.adventure_objects.at(placement->cms_id))}}}}};
    }
    require(!other_busy, "InteractionAnim_Mismatch");
    if (opcode == kOffer) {
      require(!action_, "InteractionAnim_Mismatch");
      require(request.value("item_cms_id", 0) == kTearItem, "BadRequest");
      require(!request.value("use_quick_slot", false), "BadRequest");
      require(!request.contains("item_id") || request.at("item_id") == "0", "ItemIdInvalid");
      const int count = integer(request.at("count"), 1, INT_MAX);
      if (last_offering_sequence_ && sequence == *last_offering_sequence_) {
        require(request == last_offering_request_, "BadRequest");
        return offering_packets(progress, sequence);
      }
      require(!last_offering_sequence_ || sequence > *last_offering_sequence_, "BadRequest");
      const auto owned = progress.stackables.find(kTearItem);
      require(owned != progress.stackables.end() && owned->second >= count,
              "Interaction_ConditionsNotMatch");
      auto next = progress;
      data_->dedicate(next, count);
      next.stackables.at(kTearItem) -= count;
      commit_progress(progress, std::move(next));
      last_offering_sequence_ = sequence;
      last_offering_request_ = request;
      return offering_packets(progress, sequence);
    }
    if (opcode == kInteract) {
      require(!action_ && request.at("param").value("interaction_id", 0) == 28,
              "Interaction_ConditionsNotMatch");
      return {{163, {{"target_unit_id", std::to_string(target)}, {"interaction_id", 28}}}};
    }
    require(opcode == kStart, "Interaction_ConditionsNotMatch");
    const int animation = integer(request.at("interaction_anim_id"), 1, INT_MAX);
    require(animation == 10 || animation == 16, "InteractionAnim_Mismatch");
    require(request.at("param").at("interaction").value("interaction_id", 0) == animation,
            "InteractionAnim_Mismatch");
    require(!action_ || (action_->map_id == current.map_id && action_->target_unit_id == target &&
                         action_->interaction_id == animation),
            "InteractionAnim_Mismatch");
    const float yaw = request.value("yaw", 0.0F);
    require(std::isfinite(yaw), "BadRequest");
    if (!action_) {
      action_ = Action{current.map_id, target, animation, now, false};
    }
    return {{166,
             {{"unit_id", std::to_string(player_unit_id_)},
              {"interaction_anim_id", animation},
              {"param", request.at("param")},
              {"pos", request.at("pos")},
              {"yaw", yaw},
              {"tick", request.value("tick", wire_monotonic_tick(now))},
              {"weapon_state_type", "EX5WeaponStateType_Sheathed"}}}};
  } catch (const quest_rules::RuleError &error) {
    return {failure(opcode, sequence, player_unit_id_, error.code)};
  } catch (const Json::exception &) {
    return {failure(opcode, sequence, player_unit_id_, "BadRequest")};
  } catch (const std::invalid_argument &) {
    return {failure(opcode, sequence, player_unit_id_, "BadRequest")};
  }
}

} // namespace chronicles
