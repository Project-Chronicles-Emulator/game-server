#include "world/features/adventure/adventure.hpp"

#include "cms.generated.pb.h"
#include "world/core/world_request_opcodes.hpp"
#include "world/features/quests/quest_rules.hpp"

#include <charconv>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

namespace chronicles::adventure {
namespace {

using Json = nlohmann::json;
constexpr std::uint16_t kExecute = 20070;
constexpr std::uint16_t kEnd = 20071;

std::optional<std::uint64_t> target_id(const WorldGameplayRequest &request) {
  const auto key = request.opcode == world_request::kDiscoverAdventureObjectRequest
                       ? "spot_object_id"
                       : "target_unit_id";
  const auto found = request.message.find(key);
  if (found == request.message.end() || !found->is_string()) {
    return std::nullopt;
  }
  const auto text = found->get<std::string>();
  std::uint64_t result{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
  if (error != std::errc{} || end != text.data() + text.size() || result == 0) {
    return std::nullopt;
  }
  return result;
}

GameplayPacket refusal(const WorldGameplayRequest &request, const char *error) {
  if (request.opcode == world_request::kInteractionStartRequest) {
    return {165,
            {{"ec", error}, {"packet_seq", request.message.value("packet_seq", Json::object())}}};
  }
  return {2, {{"ec", error}, {"packet_seq", {{"seq", std::to_string(request.sequence)}}}}};
}

constexpr std::size_t kMaximumInteractionConditions = 16;
constexpr int kActivatedState = 2;
constexpr std::string_view kItemGradePrefix = "EX5ItemGrade_";

// Tutorial instance rewards carry catalog grade names such as "Normal"; the tutorial reward
// commit adds the enum prefix itself. Passing the full enum name ("EX5ItemGrade_Normal") made
// every equipment reward from a world object, such as First Hunt's hunting knife, fail.
std::string instance_reward_grade_name(int grade) {
  const auto &enum_name = x5::cms::EX5ItemGrade_Name(static_cast<x5::cms::EX5ItemGrade>(grade));
  if (!enum_name.starts_with(kItemGradePrefix)) {
    throw std::runtime_error("Adventure item has an unnamed grade " + std::to_string(grade));
  }
  return enum_name.substr(kItemGradePrefix.size());
}

// The exporter only emits rows the quest condition evaluator understands; anything else in
// a profile is a generation error, so loading fails instead of guessing at runtime.
void validate_interaction_conditions(const Json &conditions) {
  if (!conditions.is_array() || conditions.empty() ||
      conditions.size() > kMaximumInteractionConditions) {
    throw std::runtime_error("Invalid adventure interaction condition list");
  }

  for (const auto &condition : conditions) {
    const auto kind = condition.at("kind").get<std::string>();
    const auto group_operator = condition.at("operator").get<std::string>();
    const bool valid_row = condition.at("group").get<int>() > 0 &&
                           (group_operator == "AND" || group_operator == "OR") &&
                           condition.at("negated").is_boolean();
    bool valid_kind = false;
    if (kind == "Quest") {
      const auto requirement = condition.at("requirement").get<std::string>();
      valid_kind = condition.at("quest_id").get<int>() > 0 &&
                   (requirement == "Acceptable" || requirement == "Progressing" ||
                    requirement == "QuestProgress" || requirement == "QuestComplete") &&
                   condition.value("progress_group", 1) > 0;
    } else if (kind == "OwnItem") {
      valid_kind = condition.at("item_id").get<int>() > 0 &&
                   condition.at("count").get<std::int64_t>() > 0;
    } else if (kind == "PlayRecord") {
      const auto record_kind = condition.at("record").at("kind").get<std::string>();
      const auto comparison = condition.at("comparison").get<std::string>();
      valid_kind = condition.at("play_record_id").get<int>() > 0 &&
                   condition.at("count").get<std::int64_t>() >= 0 &&
                   (record_kind == "HuntMonster" || record_kind == "GetItem") &&
                   (comparison == "GreaterThanOrEqual" || comparison == "GreaterThan" ||
                    comparison == "Equal" || comparison == "NotEqual" || comparison == "LessThan" ||
                    comparison == "LessThanOrEqual");
    }
    if (!valid_row || !valid_kind) {
      throw std::runtime_error("Unsupported adventure interaction condition");
    }
  }
}

} // namespace

std::shared_ptr<const Catalog> Catalog::load(const std::filesystem::path &path) {
  if (std::filesystem::file_size(path) > 16 * 1024 * 1024) {
    throw std::runtime_error("Shared adventure profile exceeds size limit");
  }
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Cannot open shared adventure definitions");
  }
  Json document;
  input >> document;
  return std::make_shared<Catalog>(document);
}

Catalog::Catalog(const Json &document) : loot_(document.at("loot")) {
  if (document.at("format_version") != 1 ||
      document.at("source_executable_dump_sha256") !=
          "e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6" ||
      !document.at("maps").is_array() || document.at("maps").size() > 512) {
    throw std::runtime_error("Unsupported shared adventure profile");
  }
  for (const auto &definition : document.at("maps")) {
    if (definition.value("profile_scope", "") != "shared_adventure") {
      throw std::runtime_error("Expected a shared adventure map");
    }
    auto profile = std::make_shared<TutorialData>(definition);
    for (const auto &[unit, object] : profile->objects) {
      const auto pack_id = object.value("loot_pack_id", 0);
      if (pack_id != 0 && !loot_.pack(pack_id)) {
        throw std::runtime_error("Adventure object references an unavailable loot pack");
      }
      if (object.contains("interaction_conditions")) {
        validate_interaction_conditions(object.at("interaction_conditions"));
      }
    }
    if (!maps_.emplace(profile->map_id(), std::move(profile)).second) {
      throw std::runtime_error("Duplicate shared adventure map");
    }
  }
}

std::shared_ptr<const TutorialData> Catalog::map(int map_id) const {
  const auto found = maps_.find(map_id);
  return found == maps_.end() ? nullptr : found->second;
}

std::size_t Catalog::map_count() const noexcept {
  return maps_.size();
}

std::vector<int> Catalog::map_ids() const {
  std::vector<int> ids;
  ids.reserve(maps_.size());
  for (const auto &[map_id, profile] : maps_) {
    ids.push_back(map_id);
  }
  return ids;
}

Json Catalog::roll_rewards(int pack_id, LootRandom &random, const WorldProgress &progress) const {
  const auto *pack = loot_.pack(pack_id);
  if (!pack) {
    throw std::logic_error("Adventure reward lost its validated pack");
  }
  const auto contents = roll_loot(loot_, *pack, random, {&progress, true});
  Json rewards = Json::array();
  if (contents.gold != 0) {
    rewards.push_back({{"item_cms_id", 0}, {"quantity", contents.gold}});
  }
  for (const auto &[cms_id, quantity] : contents.stackables) {
    rewards.push_back({{"item_cms_id", cms_id}, {"quantity", quantity}});
  }
  for (const auto &instance : contents.instances) {
    const auto *item = loot_.item(instance.item_cms_id);
    if (!item || instance.polish_step != 0) {
      throw std::runtime_error("Unsupported adventure item instance modifier");
    }
    rewards.push_back({{"item_cms_id", instance.item_cms_id},
                       {"quantity", 1},
                       {"instance",
                        {{"grade", instance_reward_grade_name(item->grade)},
                         {"max_durability", item->max_durability}}}});
  }
  return rewards;
}

Session::Session(std::shared_ptr<const Catalog> catalog, std::int64_t wire_unit_id,
                 ProgressAccess &progress, TutorialSession::Commit commit,
                 tutorial::TutorialRewardPersistence items,
                 std::shared_ptr<const Progression> progression, std::unique_ptr<LootRandom> random)
    : catalog_(std::move(catalog)), wire_unit_id_(wire_unit_id), progress_(progress),
      commit_(std::move(commit)), items_(std::move(items)), progression_(std::move(progression)),
      random_(std::move(random)) {
  if (!random_) {
    random_ = std::make_unique<SeededLootRandom>(std::random_device{}());
  }
}

void Session::set_requirement_check(RequirementCheck check) {
  requirement_check_ = std::move(check);
}

bool Session::handles(const WorldGameplayRequest &request, const WorldPosition &position) const {
  if (request.opcode == kExecute || request.opcode == kEnd) {
    return busy() && interaction_->data().map_id() == position.map_id;
  }
  if (request.opcode != world_request::kInteractionStartRequest &&
      request.opcode != world_request::kInteractionUnitRequest &&
      request.opcode != world_request::kDiscoverAdventureObjectRequest) {
    return false;
  }
  const auto profile = catalog_->map(position.map_id);
  const auto target = target_id(request);
  return profile && target && profile->object(*target);
}

bool Session::busy() const {
  return interaction_ && interaction_->busy();
}

std::optional<std::int64_t> Session::next_action_tick() const {
  return interaction_ ? interaction_->next_action_tick() : std::nullopt;
}

void Session::enter_map(int map_id) {
  if (interaction_ && interaction_->data().map_id() == map_id) {
    return;
  }
  interaction_ = std::make_unique<TutorialSession>(catalog_->map(map_id), wire_unit_id_,
                                                   progress_.load(), commit_, progression_);
  interaction_->set_instance_commit(
      [this](const auto &before, const auto &after, const auto &items) {
        return tutorial::commit_tutorial_instances(items_, before, after, items);
      });
  interaction_->set_object_rewards(
      [this](int cms_id, const WorldProgress &progress) { return rewards(cms_id, progress); });
  origin_.reset();
}

Json Session::rewards(int cms_id, const WorldProgress &progress) {
  const auto saved = rolled_rewards_.find(cms_id);
  if (saved != rolled_rewards_.end()) {
    return saved->second;
  }
  for (const auto &[unit, object] : interaction_->data().objects) {
    if (object.at("cms_id") == cms_id) {
      auto result = catalog_->roll_rewards(object.at("loot_pack_id"), *random_, progress);
      rolled_rewards_.emplace(cms_id, result);
      return result;
    }
  }
  throw std::logic_error("Adventure reward has no current map object");
}

std::vector<GameplayPacket> Session::cancel() {
  const bool was_busy = busy();
  interaction_.reset();
  origin_.reset();
  if (!was_busy) {
    return {};
  }
  return {{167, {{"unit_id", std::to_string(wire_unit_id_)}}}};
}

bool Session::interaction_conditions_met(const WorldGameplayRequest &request,
                                         const WorldPosition &position,
                                         const WorldProgress &current) const {
  if (request.opcode != world_request::kInteractionStartRequest &&
      request.opcode != world_request::kInteractionUnitRequest) {
    return true;
  }

  // handles() already established the map profile, the target and its object.
  const auto *object = catalog_->map(position.map_id)->object(*target_id(request));
  const auto conditions = object->find("interaction_conditions");
  if (conditions == object->end()) {
    return true;
  }

  const auto saved = current.adventure_objects.find(object->at("cms_id").get<int>());
  if (saved != current.adventure_objects.end() && saved->second == kActivatedState) {
    return true;
  }
  return requirement_check_ && requirement_check_(*conditions, position);
}

std::vector<GameplayPacket> Session::receive(const WorldGameplayRequest &request,
                                             const WorldPosition &position, std::int64_t now,
                                             bool blocked) {
  if (!handles(request, position)) {
    return {refusal(request, "Interaction_TargetNotFound")};
  }
  enter_map(position.map_id);
  const auto current = progress_.load();
  interaction_->refresh_shared_progress(current);
  if (blocked || current.dead) {
    auto packets = cancel();
    if (request.opcode != world_request::kDiscoverAdventureObjectRequest) {
      packets.push_back(refusal(request, "InteractionAnim_UsingSkill"));
    }
    return packets;
  }
  if (!interaction_conditions_met(request, position, current)) {
    return {refusal(request, "Interaction_ConditionsNotMatch")};
  }
  auto packets =
      interaction_->receive(request.opcode, request.message, request.sequence, position, now);
  if (busy() && !origin_) {
    origin_ = position;
  }
  if (!busy()) {
    origin_.reset();
  }
  return packets;
}

std::vector<GameplayPacket>
Session::advance(std::int64_t now, const std::optional<WorldPosition> &position, bool blocked) {
  if (!busy()) {
    return {};
  }
  if (!position || position->map_id != interaction_->data().map_id() || blocked ||
      (origin_ && quest_rules::distance_squared(
                      *position, Json::array({origin_->x_mm, origin_->y_mm, origin_->z_mm})) >
                      1000.0 * 1000.0)) {
    return cancel();
  }
  interaction_->refresh_shared_progress(progress_.load());
  if (interaction_->progress().dead) {
    return cancel();
  }
  return interaction_->advance(now, *position);
}

std::vector<GameplayPacket> Session::abort(const WorldGameplayRequest &request) {
  auto packets = cancel();
  // Rolled rewards stay cached so a retry after the failure grants the same result.
  if (request.opcode != world_request::kDiscoverAdventureObjectRequest) {
    packets.push_back(refusal(request, "BadRequest"));
  }
  return packets;
}

} // namespace chronicles::adventure
