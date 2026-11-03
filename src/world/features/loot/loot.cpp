#include "world/features/loot/loot.hpp"
#include "world/core/inventory_slots.hpp"

#include "cms.generated.pb.h"
#include "persistence/item_wire.hpp"
#include "common/profile_json.hpp"

#include <spdlog/spdlog.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace chronicles {
namespace {

using Json = nlohmann::json;

std::string corpse_fingerprint(std::int64_t owner, int npc_cms_id, bool first_kill,
                               const LootContents &contents) {
  Json payload{{"owner", owner}, {"npc", npc_cms_id}, {"first_kill", first_kill},
               {"gold", contents.gold}};
  auto stackables = Json::array();
  for (const auto &[cms_id, quantity] : contents.stackables) {
    stackables.push_back(Json::array({cms_id, quantity}));
  }
  payload["stackables"] = std::move(stackables);
  auto instances = Json::array();
  for (const auto &item : contents.instances) {
    instances.push_back(Json::array({item.item_cms_id, item.polish_step}));
  }
  payload["instances"] = std::move(instances);
  const auto bytes = payload.dump();
  std::array<unsigned char, 32> digest{};
  unsigned int digest_size{};
  if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &digest_size, EVP_sha256(), nullptr) !=
          1 ||
      digest_size != digest.size()) {
    throw std::runtime_error("Cannot fingerprint a loot corpse");
  }
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result;
  result.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    result.push_back(digits[byte >> 4]);
    result.push_back(digits[byte & 0x0f]);
  }
  return result;
}

constexpr int kFormatVersion = 2;
constexpr char kBuildId[] = "40367e24241c9481bfc07fbf9ef1919f25fb0eaa";
constexpr std::uintmax_t kMaximumProfileBytes = 16 * 1024 * 1024;
constexpr char kItemGradePrefix[] = "EX5ItemGrade_";
constexpr char kGoldEntryKind[] = "gold";
constexpr char kItemEntryKind[] = "item";
constexpr char kStackableStorage[] = "stackable";
constexpr char kInstanceStorage[] = "instance";

// The NPC rows of a profile are positional; this must match tools/build_loot_data.py.
constexpr std::array<const char *, 5> kNpcFields{"npc_cms_id", "loot_pack_id", "handling_pack_id",
                                                 "handling_rate_percent", "kill_gold"};

// One loot entry never exceeds this quantity, so summing several groups cannot overflow.
constexpr int kMaximumEntryQuantity = 100000000;
constexpr double kAlwaysPercent = 100.0;

// Native world ticks are 100 ns units (see NativeTicks in native_world.cpp).
constexpr std::int64_t kTicksPerMillisecond = 10000;

std::vector<int> parse_quest_list(const Json &entry) {
  std::vector<int> quests;
  for (const auto &quest : entry.at("required_quests")) {
    const auto quest_id = profile_integer(quest, "required_quests");
    if (quest_id <= 0) {
      throw std::runtime_error("Loot item requires an invalid quest");
    }
    quests.push_back(quest_id);
  }
  return quests;
}

LootItemDefinition parse_item_definition(const Json &entry) {
  LootItemDefinition definition;

  const auto &storage = entry.at("storage").get_ref<const std::string &>();
  if (storage == kInstanceStorage) {
    definition.storage = LootItemStorage::instance;
  } else if (storage == kStackableStorage) {
    definition.storage = LootItemStorage::stackable;
  } else {
    throw std::runtime_error("Unknown loot item storage: " + storage);
  }

  const auto &grade_name = entry.at("grade").get_ref<const std::string &>();
  x5::cms::EX5ItemGrade grade;
  if (!x5::cms::EX5ItemGrade_Parse(kItemGradePrefix + grade_name, &grade)) {
    throw std::runtime_error("Unknown loot item grade: " + grade_name);
  }
  definition.grade = static_cast<int>(grade);

  definition.max_durability = profile_integer(entry.at("max_durability"), "max_durability");
  const bool needs_durability = definition.storage == LootItemStorage::instance;
  if (definition.max_durability < 0 || (needs_durability && definition.max_durability == 0)) {
    throw std::runtime_error("Invalid loot item durability");
  }

  definition.required_quests = parse_quest_list(entry);
  return definition;
}

LootEntry parse_entry(const Json &entry, const std::unordered_map<int, LootItemDefinition> &items) {
  LootEntry loot_entry;
  loot_entry.min_quantity = profile_integer(entry.at("min_quantity"), "min_quantity");
  loot_entry.max_quantity = profile_integer(entry.at("max_quantity"), "max_quantity");
  loot_entry.weight = profile_integer(entry.at("weight"), "weight");
  if (loot_entry.min_quantity <= 0 || loot_entry.min_quantity > loot_entry.max_quantity ||
      loot_entry.max_quantity > kMaximumEntryQuantity || loot_entry.weight <= 0) {
    throw std::runtime_error("Invalid loot entry quantity or weight");
  }

  const auto &kind = entry.at("kind").get_ref<const std::string &>();
  if (kind == kGoldEntryKind) {
    loot_entry.is_gold = true;
    return loot_entry;
  }
  if (kind != kItemEntryKind) {
    throw std::runtime_error("Unknown loot entry kind: " + kind);
  }

  loot_entry.item_cms_id = profile_integer(entry.at("item_cms_id"), "item_cms_id");
  const auto item = items.find(loot_entry.item_cms_id);
  if (item == items.end()) {
    throw std::runtime_error("Loot entry references undefined item " +
                             std::to_string(loot_entry.item_cms_id));
  }

  // One corpse grants at most this many rows in one storage trade.
  const bool is_instance = item->second.storage == LootItemStorage::instance;
  if (is_instance && loot_entry.max_quantity > static_cast<int>(kMaximumInstancesPerLootEntry)) {
    throw std::runtime_error("Loot entry grants too many item instances");
  }

  if (entry.contains("polish_step")) {
    loot_entry.polish_step = profile_integer(entry.at("polish_step"), "polish_step");
    if (!is_instance || loot_entry.polish_step <= 0 ||
        loot_entry.polish_step > kMaxItemPolishStep) {
      throw std::runtime_error("Invalid loot entry polish step");
    }
  }
  return loot_entry;
}

// A parsed group and whether the active rules allow it.
struct ParsedGroup {
  LootGroup group;
  bool allowed{};
};

ParsedGroup parse_group(const Json &entry, const std::unordered_map<int, LootItemDefinition> &items,
                        const LootCustomRules &rules) {
  ParsedGroup parsed;
  parsed.group.rate_percent = profile_percent(entry.at("rate_percent"), "group rate_percent");
  for (const auto &loot_entry : entry.at("entries")) {
    parsed.group.entries.push_back(parse_entry(loot_entry, items));
  }
  if (parsed.group.entries.empty()) {
    throw std::runtime_error("Loot group has no entries");
  }

  parsed.allowed = true;
  for (const auto &name : entry.at("custom_rules")) {
    const auto rule = parse_loot_rule(name.get_ref<const std::string &>());
    parsed.group.first_kill_only =
        parsed.group.first_kill_only || rule == LootRule::first_kill_rewards;
    parsed.allowed = parsed.allowed && rules.enabled(rule);
  }
  return parsed;
}

// Parses every pack of a list into out, withholding groups the rules switch off. declared
// collects every pack ID of the profile, including packs left without any allowed group.
void parse_pack_list(const Json &list, const std::unordered_map<int, LootItemDefinition> &items,
                     const LootCustomRules &rules, std::unordered_map<int, LootPack> &out,
                     std::set<int> &declared, std::size_t &withheld) {
  for (const auto &entry : list) {
    LootPack pack;
    pack.id = profile_integer(entry.at("loot_pack_id"), "loot_pack_id");
    pack.rate_percent = profile_percent(entry.at("rate_percent"), "pack rate_percent");
    if (pack.id <= 0 || entry.at("groups").empty()) {
      throw std::runtime_error("Invalid loot pack " + std::to_string(pack.id));
    }
    if (!declared.insert(pack.id).second) {
      throw std::runtime_error("Duplicate loot pack " + std::to_string(pack.id));
    }

    for (const auto &group_entry : entry.at("groups")) {
      auto parsed = parse_group(group_entry, items, rules);
      if (!parsed.allowed) {
        ++withheld;
        continue;
      }
      pack.has_first_kill_group = pack.has_first_kill_group || parsed.group.first_kill_only;
      pack.groups.push_back(std::move(parsed.group));
    }
    if (!pack.groups.empty()) {
      out.emplace(pack.id, std::move(pack));
    }
  }
}

int parse_item_key(const std::string &key) {
  int cms_id{};
  const auto *end = key.data() + key.size();
  const auto result = std::from_chars(key.data(), end, cms_id);
  if (result.ec != std::errc{} || result.ptr != end || cms_id <= 0) {
    throw std::runtime_error("Invalid loot item key: " + key);
  }
  return cms_id;
}

std::int64_t positive_setting(const Json &document, const char *field) {
  const auto value = profile_integer64(document.at(field), field);
  if (value <= 0) {
    throw std::runtime_error(std::string("Loot setting must be positive: ") + field);
  }
  return value;
}

double squared_distance_mm(const WorldPosition &first, const WorldPosition &second) {
  const double delta_x = static_cast<double>(first.x_mm) - second.x_mm;
  const double delta_y = static_cast<double>(first.y_mm) - second.y_mm;
  const double delta_z = static_cast<double>(first.z_mm) - second.z_mm;
  return delta_x * delta_x + delta_y * delta_y + delta_z * delta_z;
}

// Accepts the protobuf JSON forms of a uint64: a decimal string or an unsigned number.
std::optional<std::uint64_t> parse_unit_id(const Json &request) {
  const auto field = request.find("unit_id");
  if (field == request.end()) {
    return std::nullopt;
  }

  if (field->is_number_unsigned()) {
    return field->get<std::uint64_t>();
  }
  if (!field->is_string()) {
    return std::nullopt;
  }

  const auto &text = field->get_ref<const std::string &>();
  std::uint64_t unit_id{};
  const auto *end = text.data() + text.size();
  const auto result = std::from_chars(text.data(), end, unit_id);
  if (text.empty() || result.ec != std::errc{} || result.ptr != end) {
    return std::nullopt;
  }
  return unit_id;
}

GameplayPacket item_response(x5::ec::EErrorCode code, std::optional<std::uint64_t> unit_id) {
  Json message{{"packet_seq", Json::object()}};
  if (code != x5::ec::Success) {
    message["ec"] = x5::ec::EErrorCode_Name(code);
  }
  if (unit_id) {
    message["unit_id"] = std::to_string(*unit_id);
  }
  return {kLootItemResponseOpcode, std::move(message)};
}

// Both flags false: the client clears the corpse's loot highlight.
GameplayPacket cleared_loot_flags(std::uint64_t unit_id) {
  return {kUnitLootItemInfoOpcode, {{"unit_id", std::to_string(unit_id)}}};
}

// has_chance_to_loot and possible_handling_loot are both captured. possible_handling_loot marks a
// corpse the killer may butcher with a knife (captured Alpha Wolf and Wolf corpses).
GameplayPacket loot_announcement(std::uint64_t unit_id, bool has_chance_to_loot,
                                 bool possible_handling_loot) {
  Json message{{"unit_id", std::to_string(unit_id)}};
  if (has_chance_to_loot) {
    message["has_chance_to_loot"] = true;
  }
  if (possible_handling_loot) {
    message["possible_handling_loot"] = true;
  }
  return {kUnitLootItemInfoOpcode, std::move(message)};
}

bool quest_active(const WorldProgress &progress, int quest_id) {
  const auto found = progress.quests.find(quest_id);
  return found != progress.quests.end() && !found->second.complete && !found->second.abandoned;
}

// CUSTOM (guessed, not official): an item with a quest gate is an alternative only while one of
// its quests is active.
bool entry_available(const LootData &data, const LootEntry &entry, const LootRollContext &context) {
  if (entry.is_gold) {
    return true;
  }

  const auto *item = data.item(entry.item_cms_id);
  if (item == nullptr) {
    throw std::logic_error("Loot entry references an item that LootData did not define");
  }
  if (item->required_quests.empty()) {
    return true;
  }
  if (context.progress == nullptr) {
    return false;
  }
  return std::ranges::any_of(item->required_quests, [&](int quest_id) {
    return quest_active(*context.progress, quest_id);
  });
}

void merge_contents(LootContents &into, LootContents &&from) {
  into.gold += from.gold;
  for (const auto &[item_cms_id, quantity] : from.stackables) {
    into.stackables[item_cms_id] += quantity;
  }
  for (const auto &instance : from.instances) {
    if (into.instances.size() >= kMaximumLootInstancesPerCorpse) {
      break;
    }
    into.instances.push_back(instance);
  }
}

} // namespace

std::shared_ptr<const LootData> LootData::load(const std::filesystem::path &path,
                                               const LootCustomRules &rules) {
  std::error_code size_error;
  const auto size = std::filesystem::file_size(path, size_error);
  if (size_error || size > kMaximumProfileBytes) {
    throw std::runtime_error("Cannot read loot profile within the size limit: " + path.string());
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("Cannot open loot profile " + path.string());
  }

  try {
    return std::make_shared<const LootData>(Json::parse(input), rules);
  } catch (const Json::exception &error) {
    throw std::runtime_error("Invalid loot profile " + path.string() + ": " + error.what());
  }
}

LootData::LootData(const Json &document, const LootCustomRules &rules) : rules_(rules) {
  try {
    if (document.at("format_version") != kFormatVersion || document.at("build_id") != kBuildId) {
      throw std::runtime_error("Loot profile belongs to an unsupported format or client build");
    }
    if (document.at("npc_fields") != Json(kNpcFields)) {
      throw std::runtime_error("Loot profile NPC fields do not match this server");
    }

    looting_range_mm_ = static_cast<double>(positive_setting(document, "looting_range_mm"));
    corpse_lootable_ms_ =
        rules_.lootable_lifetime_ms.value_or(positive_setting(document, "corpse_lootable_ms"));
    corpse_empty_ms_ =
        rules_.emptied_lifetime_ms.value_or(positive_setting(document, "corpse_empty_ms"));
    gold_limit_ = positive_setting(document, "gold_limit");

    for (const auto &[key, entry] : document.at("items").items()) {
      if (!items_.emplace(parse_item_key(key), parse_item_definition(entry)).second) {
        throw std::runtime_error("Duplicate loot item " + key);
      }
    }

    std::set<int> declared_packs;
    std::set<int> declared_handling_packs;
    parse_pack_list(document.at("packs"), items_, rules_, packs_, declared_packs, withheld_groups_);
    parse_pack_list(document.at("handling_packs"), items_, rules_, handling_packs_,
                    declared_handling_packs, withheld_groups_);

    std::set<int> seen_npcs;
    for (const auto &row : document.at("npcs")) {
      if (!row.is_array() || row.size() != kNpcFields.size()) {
        throw std::runtime_error("Loot NPC row must have one value per NPC field");
      }

      const auto npc_cms_id = profile_integer(row.at(0), "npc_cms_id");
      const auto pack_id = profile_integer(row.at(1), "loot_pack_id");
      const auto handling_id = profile_integer(row.at(2), "handling_pack_id");
      const auto kill_gold = profile_integer64(row.at(4), "kill_gold");
      const bool bad_pack = pack_id != 0 && !declared_packs.contains(pack_id);
      const bool bad_handling = handling_id != 0 && !declared_handling_packs.contains(handling_id);
      if (npc_cms_id <= 0 || kill_gold < 0 || bad_pack || bad_handling ||
          !seen_npcs.insert(npc_cms_id).second) {
        throw std::runtime_error("Invalid loot NPC row " + std::to_string(npc_cms_id));
      }

      LootNpc npc;
      npc.pack_id = packs_.contains(pack_id) ? pack_id : 0;
      if (handling_id != 0) {
        const auto rate = profile_percent(row.at(3), "handling_rate_percent");
        if (handling_packs_.contains(handling_id)) {
          npc.handling_pack_id = handling_id;
          npc.handling_rate_percent = rate;
        }
      }
      npc.kill_gold = rules_.kill_gold ? kill_gold : 0;
      if (npc.pack_id != 0 || npc.handling_pack_id != 0 || npc.kill_gold > 0) {
        npcs_.emplace(npc_cms_id, npc);
      }
    }
  } catch (const Json::exception &error) {
    throw std::runtime_error(std::string("Malformed loot profile: ") + error.what());
  }

  if (items_.empty() && npcs_.empty() && packs_.empty()) {
    throw std::runtime_error("Loot profile has no usable packs or NPCs");
  }
}

const LootNpc *LootData::npc(int npc_cms_id) const {
  const auto found = npcs_.find(npc_cms_id);
  return found == npcs_.end() ? nullptr : &found->second;
}

const LootPack *LootData::pack(int pack_id) const {
  const auto found = packs_.find(pack_id);
  return found == packs_.end() ? nullptr : &found->second;
}

const LootPack *LootData::handling_pack(int pack_id) const {
  const auto found = handling_packs_.find(pack_id);
  return found == handling_packs_.end() ? nullptr : &found->second;
}

const LootItemDefinition *LootData::item(int cms_id) const {
  const auto found = items_.find(cms_id);
  return found == items_.end() ? nullptr : &found->second;
}

const LootCustomRules &LootData::rules() const {
  return rules_;
}

double LootData::looting_range_mm() const {
  return looting_range_mm_;
}

std::int64_t LootData::corpse_lootable_ms() const {
  return corpse_lootable_ms_;
}

std::int64_t LootData::corpse_empty_ms() const {
  return corpse_empty_ms_;
}

std::int64_t LootData::gold_limit() const {
  return gold_limit_;
}

std::size_t LootData::npc_count() const {
  return npcs_.size();
}

std::size_t LootData::pack_count() const {
  return packs_.size() + handling_packs_.size();
}

std::size_t LootData::withheld_group_count() const {
  return withheld_groups_;
}

SeededLootRandom::SeededLootRandom(std::uint32_t seed) : engine_(seed) {}

bool SeededLootRandom::passes(double rate_percent) {
  if (rate_percent >= kAlwaysPercent) {
    return true;
  }
  std::uniform_real_distribution<double> percent(0.0, kAlwaysPercent);
  return percent(engine_) < rate_percent;
}

std::size_t SeededLootRandom::pick_index(const std::vector<int> &weights) {
  std::discrete_distribution<std::size_t> choose(weights.begin(), weights.end());
  return choose(engine_);
}

int SeededLootRandom::pick_quantity(int minimum, int maximum) {
  std::uniform_int_distribution<int> quantity(minimum, maximum);
  return quantity(engine_);
}

bool LootContents::empty() const {
  return gold == 0 && stackables.empty() && instances.empty();
}

LootContents roll_loot(const LootData &data, const LootPack &pack, LootRandom &random,
                       const LootRollContext &context) {
  LootContents contents;
  if (!random.passes(pack.rate_percent)) {
    return contents;
  }

  for (const auto &group : pack.groups) {
    if (group.first_kill_only && !context.first_kill_available) {
      continue;
    }
    if (!random.passes(group.rate_percent)) {
      continue;
    }

    std::vector<const LootEntry *> alternatives;
    std::vector<int> weights;
    for (const auto &candidate : group.entries) {
      if (entry_available(data, candidate, context)) {
        alternatives.push_back(&candidate);
        weights.push_back(candidate.weight);
      }
    }
    if (alternatives.empty()) {
      continue;
    }

    const auto &entry = *alternatives.at(random.pick_index(weights));
    const auto quantity = random.pick_quantity(entry.min_quantity, entry.max_quantity);

    if (entry.is_gold) {
      contents.gold += quantity;
      continue;
    }

    const auto *item = data.item(entry.item_cms_id);
    if (item->storage == LootItemStorage::stackable) {
      contents.stackables[entry.item_cms_id] += quantity;
      continue;
    }
    for (int unit = 0;
         unit < quantity && contents.instances.size() < kMaximumLootInstancesPerCorpse; ++unit) {
      contents.instances.push_back({entry.item_cms_id, entry.polish_step});
    }
  }
  return contents;
}

RolledCorpse roll_corpse(const LootData &data, const LootNpc &npc, LootRandom &random,
                         const LootRollContext &context) {
  RolledCorpse rolled;
  rolled.contents.gold = npc.kill_gold;

  if (const auto *pack = data.pack(npc.pack_id)) {
    merge_contents(rolled.contents, roll_loot(data, *pack, random, context));
    rolled.consumes_first_kill = pack->has_first_kill_group && context.first_kill_available;
  }

  if (const auto *handling = data.handling_pack(npc.handling_pack_id)) {
    if (random.passes(npc.handling_rate_percent)) {
      rolled.handling_loot = true;
      merge_contents(rolled.contents, roll_loot(data, *handling, random, context));
      rolled.consumes_first_kill = rolled.consumes_first_kill ||
                                   (handling->has_first_kill_group && context.first_kill_available);
    }
  }
  return rolled;
}

bool SharedLootBoard::open(const SharedCorpseKey &key, SharedCorpse corpse) {
  std::lock_guard lock(mutex_);
  if (corpses_.size() >= kMaximumSharedCorpses || corpses_.contains(key)) {
    return false;
  }
  corpses_.emplace(key, std::move(corpse));
  return true;
}

SharedClaim SharedLootBoard::claim(const SharedCorpseKey &key, std::int64_t claimant,
                                   std::int64_t now_ticks) {
  std::lock_guard lock(mutex_);
  const auto found = corpses_.find(key);
  if (found == corpses_.end()) {
    return {ClaimStatus::unknown, std::nullopt};
  }

  auto &corpse = found->second;
  if (!corpse.published) {
    return {ClaimStatus::unknown, std::nullopt};
  }
  if (now_ticks >= corpse.expire_tick) {
    return {ClaimStatus::expired, std::nullopt};
  }
  if (!corpse.eligible.contains(claimant)) {
    return {ClaimStatus::not_eligible, std::nullopt};
  }
  if (corpse.taken) {
    return {ClaimStatus::already_taken, std::nullopt};
  }

  // Copy the granted contents before changing board state. An allocation failure must not
  // strand an untaken corpse as claimed.
  SharedClaim claim{ClaimStatus::claimed, corpse};
  corpse.taken = true;
  claim.corpse->taken = true;
  return claim;
}

void SharedLootBoard::release(const SharedCorpseKey &key) {
  std::lock_guard lock(mutex_);
  const auto found = corpses_.find(key);
  if (found != corpses_.end()) {
    found->second.taken = false;
  }
}

std::optional<SharedLootBoard::MemberView>
SharedLootBoard::find_for_member(std::uint64_t unit_id, std::int64_t member,
                                 std::int64_t now_ticks) const {
  std::lock_guard lock(mutex_);
  for (const auto &[key, corpse] : corpses_) {
    const bool is_other_owner = key.owner_character_id != member;
    if (corpse.published && key.unit_id == unit_id && is_other_owner && corpse.eligible.contains(member) &&
        now_ticks < corpse.expire_tick) {
      return MemberView{key, corpse.position};
    }
  }
  return std::nullopt;
}

std::set<std::int64_t> SharedLootBoard::eligible(const SharedCorpseKey &key) const {
  std::lock_guard lock(mutex_);
  const auto found = corpses_.find(key);
  return found == corpses_.end() ? std::set<std::int64_t>{} : found->second.eligible;
}

bool SharedLootBoard::publish(const SharedCorpseKey &key, std::int64_t expire_tick) {
  std::lock_guard lock(mutex_);
  const auto entry = corpses_.find(key);
  if (entry == corpses_.end()) {
    return false;
  }
  entry->second.expire_tick = std::max(entry->second.expire_tick, expire_tick);
  entry->second.published = true;
  return true;
}

void SharedLootBoard::extend_expiry(const SharedCorpseKey &key, std::int64_t expire_tick) {
  std::lock_guard lock(mutex_);
  const auto entry = corpses_.find(key);
  if (entry != corpses_.end()) {
    entry->second.expire_tick = std::max(entry->second.expire_tick, expire_tick);
  }
}

void SharedLootBoard::close(const SharedCorpseKey &key) {
  std::lock_guard lock(mutex_);
  corpses_.erase(key);
}

std::size_t SharedLootBoard::size() const {
  std::lock_guard lock(mutex_);
  return corpses_.size();
}

LootSession::LootSession(std::shared_ptr<const LootData> data, std::int64_t character_id,
                         LootPersistence persistence, std::unique_ptr<LootRandom> random,
                         std::shared_ptr<LootAudience> audience,
                         std::shared_ptr<SharedLootBoard> board)
    : data_(std::move(data)), character_id_(character_id), persistence_(std::move(persistence)),
      random_(std::move(random)), audience_(std::move(audience)), board_(std::move(board)) {
  if (!data_ || character_id_ <= 0 || !random_ || !persistence_.load_character ||
      !persistence_.load_progress || (!persistence_.commit && !persistence_.commit_once)) {
    throw std::invalid_argument("Loot session requires data, a character, randomness and storage");
  }
}

bool LootSession::handles(std::uint16_t opcode) const {
  return opcode == kLootItemRequestOpcode;
}

void LootSession::set_butchering(std::function<bool(int npc_cms_id)> butcherable) {
  butcherable_ = std::move(butcherable);
}

MonsterDefeatReaction LootSession::monster_defeated(const MonsterDefeat &defeat) {
  if (defeat.unit_id == 0 || defeat.ticks <= 0) {
    return {};
  }
  const auto existing = corpses_.find(defeat.unit_id);
  if (existing != corpses_.end()) {
    const bool matching_event = defeat.reward_event_id.empty()
                                    ? existing->second.reward_event_id.empty() &&
                                          existing->second.death_tick == defeat.ticks
                                    : existing->second.reward_event_id == defeat.reward_event_id;
    if (existing->second.registration_complete || !matching_event ||
        existing->second.npc_cms_id != defeat.npc_cms_id) {
      return {};
    }
    auto &corpse = existing->second;
    const auto lifetime_ms = corpse.announces_removal ? data_->corpse_lootable_ms()
                                                      : data_->corpse_empty_ms();
    const auto lifetime_ticks = lifetime_ms * kTicksPerMillisecond;
    if (defeat.ticks > std::numeric_limits<std::int64_t>::max() - lifetime_ticks) {
      throw std::invalid_argument("Defeat tick leaves no room for the corpse lifetime");
    }
    corpse.expire_tick = std::max(corpse.expire_tick, defeat.ticks + lifetime_ticks);
    if (corpse.shared) {
      board_->extend_expiry({character_id_, defeat.unit_id}, corpse.expire_tick);
    }
    return finish_corpse_registration(defeat, corpse);
  }
  if (corpses_.size() >= kMaximumTrackedCorpses) {
    spdlog::warn("Corpse loot limit reached; unit {} will not be lootable", defeat.unit_id);
    return {};
  }

  Corpse corpse;
  corpse.owner_character_id = defeat.killer_character_id;
  corpse.npc_cms_id = defeat.npc_cms_id;
  corpse.position = defeat.position;
  corpse.death_tick = defeat.ticks;
  corpse.reward_event_id = defeat.reward_event_id;
  corpse.receipt_id = defeat.reward_event_id.empty() ? random_hex(16) : defeat.reward_event_id;
  corpse.publication_pending = !defeat.reward_event_id.empty();

  const bool killed_here = defeat.killer_character_id == character_id_;
  corpse.butcherable = killed_here && butcherable_ && butcherable_(defeat.npc_cms_id);
  if (corpse.butcherable) {
    corpse.handling = std::make_shared<CorpseHandlingState>(corpse.receipt_id);
  }

  // Only the killer receives loot rights, and only for NPCs with loot facts that the server
  // models (see tools/build_loot_data.py).
  bool has_chance_to_loot = false;
  const auto *npc = data_->npc(defeat.npc_cms_id);
  if (npc != nullptr && killed_here) {
    // The tutorial persistence serves this from the session's cached progress. If it fails, the
    // roll proceeds without quest or first-kill state, so those custom drops fail closed.
    std::optional<WorldProgress> progress;
    try {
      progress = persistence_.load_progress();
    } catch (const std::exception &error) {
      spdlog::warn("Loot roll without character progress: {}", error.what());
    }

    LootRollContext context;
    context.progress = progress ? &*progress : nullptr;
    context.first_kill_available = progress &&
                                   !progress->loot_first_kills.contains(defeat.npc_cms_id) &&
                                   !first_kills_reserved_.contains(defeat.npc_cms_id);
    auto rolled = roll_corpse(*data_, *npc, *random_, context);

    // CUSTOM (guessed, not official): a rolled handling pack joins the ordinary corpse take.
    // The butchering flag (possible_handling_loot) belongs to the evidenced knife interaction.
    has_chance_to_loot = npc->pack_id != 0 || npc->kill_gold > 0 || rolled.handling_loot;
    corpse.lootable = has_chance_to_loot;
    corpse.records_first_kill = rolled.consumes_first_kill && corpse.lootable;
    corpse.contents = std::move(rolled.contents);
  }
  corpse.announces_removal = corpse.lootable || corpse.butcherable;
  corpse.corpse_fingerprint = corpse_fingerprint(corpse.owner_character_id, corpse.npc_cms_id,
                                                 corpse.records_first_kill, corpse.contents);

  // Captured: an unbutchered butcherable corpse stayed NpcCorpseLootingDestroyMillis (90 s).
  const bool keeps_corpse = corpse.lootable || corpse.butcherable;
  const auto lifetime_ms = keeps_corpse ? data_->corpse_lootable_ms() : data_->corpse_empty_ms();
  const auto lifetime_ticks = lifetime_ms * kTicksPerMillisecond;
  if (defeat.ticks > std::numeric_limits<std::int64_t>::max() - lifetime_ticks) {
    throw std::invalid_argument("Defeat tick leaves no room for the corpse lifetime");
  }
  corpse.expire_tick = defeat.ticks + lifetime_ticks;

  corpse.has_chance_to_loot = has_chance_to_loot;

  const bool reserved_first_kill = corpse.records_first_kill;
  if (reserved_first_kill) {
    first_kills_reserved_.insert(defeat.npc_cms_id);
  }
  try {
    const auto [entry, inserted] = corpses_.emplace(defeat.unit_id, std::move(corpse));
    if (!inserted) {
      throw std::logic_error("Corpse was registered concurrently");
    }
    return finish_corpse_registration(defeat, entry->second);
  } catch (...) {
    // Once the corpse exists its reservation and roll belong to the staged registration.
    if (reserved_first_kill && !corpses_.contains(defeat.unit_id)) {
      first_kills_reserved_.erase(defeat.npc_cms_id);
    }
    throw;
  }
}

MonsterDefeatReaction LootSession::finish_corpse_registration(const MonsterDefeat &defeat,
                                                               Corpse &corpse) {
  if (!corpse.sharing_decided) {
    if (!corpse.party_query_complete) {
      const auto &rules = data_->rules();
      if (corpse.lootable && rules.party_shared_loot && audience_ && board_) {
        corpse.pending_party_members = audience_->party_members_near(defeat, rules.party_range_mm);
      }
      corpse.party_query_complete = true;
    }
    if (!corpse.pending_party_members.empty()) {
      SharedCorpse shared;
      shared.eligible.insert(corpse.pending_party_members.begin(), corpse.pending_party_members.end());
      shared.eligible.insert(character_id_);
      shared.position = corpse.position;
      shared.contents = corpse.contents;
      shared.npc_cms_id = corpse.npc_cms_id;
      shared.records_first_kill = corpse.records_first_kill;
      shared.receipt_id = corpse.receipt_id;
      shared.corpse_fingerprint = corpse.corpse_fingerprint;
      shared.expire_tick = corpse.expire_tick;
      shared.published = !corpse.publication_pending;
      if (board_->open({character_id_, defeat.unit_id}, std::move(shared))) {
        corpse.shared = true;
        corpse.contents = {};
      }
    }
    corpse.sharing_decided = true;
  }

  MonsterDefeatReaction reaction;
  if (corpse.announces_removal) {
    reaction.packets.push_back(
        loot_announcement(defeat.unit_id, corpse.has_chance_to_loot, corpse.butcherable));
    reaction.manages_corpse_removal = true;
  }
  if (corpse.publication_pending) {
    const auto event_id = corpse.reward_event_id;
    reaction.on_published = [this, unit_id = defeat.unit_id, event_id](std::int64_t ticks) {
      publish_corpse(unit_id, event_id, ticks);
    };
  } else {
    notify_pending_party_members(defeat.unit_id, corpse);
  }
  corpse.registration_complete = true;
  return reaction;
}

void LootSession::notify_pending_party_members(std::uint64_t unit_id, Corpse &corpse) {
  // A callback may throw after queuing delivery. Advance first so an ambiguous delivery is
  // not repeated; the recipient can still find and claim the shared corpse after publication.
  while (corpse.shared && corpse.next_party_notification < corpse.pending_party_members.size()) {
    const auto member = corpse.pending_party_members[corpse.next_party_notification++];
    audience_->notify(member, {loot_announcement(unit_id, corpse.has_chance_to_loot, false)});
  }
}

void LootSession::publish_corpse(std::uint64_t unit_id, const std::string &event_id,
                                 std::int64_t ticks) {
  const auto found = corpses_.find(unit_id);
  if (found == corpses_.end() || found->second.reward_event_id != event_id) {
    throw std::runtime_error("Staged loot corpse no longer matches its defeat");
  }
  auto &corpse = found->second;
  if (!corpse.publication_pending) {
    return;
  }
  const auto lifetime_ms = corpse.announces_removal ? data_->corpse_lootable_ms()
                                                    : data_->corpse_empty_ms();
  const auto lifetime_ticks = lifetime_ms * kTicksPerMillisecond;
  if (ticks > std::numeric_limits<std::int64_t>::max() - lifetime_ticks) {
    throw std::invalid_argument("Publication tick leaves no room for the corpse lifetime");
  }
  const auto expiry = std::max(corpse.expire_tick, ticks + lifetime_ticks);
  notify_pending_party_members(unit_id, corpse);
  if (corpse.shared && !board_->publish({character_id_, unit_id}, expiry)) {
    throw std::runtime_error("Staged shared loot corpse is missing");
  }
  corpse.expire_tick = expiry;
  corpse.publication_pending = false;
}

std::optional<ButcherableCorpse> LootSession::butcherable_corpse(std::uint64_t unit_id,
                                                                 std::int64_t ticks) const {
  const auto found = corpses_.find(unit_id);
  if (found == corpses_.end()) {
    return std::nullopt;
  }

  const auto &corpse = found->second;
  if (corpse.publication_pending || !corpse.butcherable || !corpse.handling ||
      !corpse.handling->available() || corpse.owner_character_id != character_id_ ||
      ticks >= corpse.expire_tick) {
    return std::nullopt;
  }
  return ButcherableCorpse{corpse.npc_cms_id, corpse.position, corpse.handling};
}

std::vector<GameplayPacket> LootSession::corpse_butchered(std::uint64_t unit_id,
                                                          std::int64_t ticks) {
  const auto found = corpses_.find(unit_id);
  if (found == corpses_.end() || !found->second.butcherable) {
    return {};
  }

  auto &corpse = found->second;
  corpse.butcherable = false;
  if (!corpse.lootable) {
    // Captured: a butchered corpse disappeared NpcCorpseNoneDestroyMillis (10.1-10.8 s) later.
    shorten_to_emptied_lifetime(corpse, ticks);
  }
  return {loot_announcement(unit_id, corpse.lootable, false)};
}

x5::ec::EErrorCode LootSession::check_range(const WorldPosition &corpse_position,
                                            const WorldPosition &player_position) const {
  const double limit_mm = data_->looting_range_mm() + data_->rules().range_tolerance_mm;
  if (squared_distance_mm(player_position, corpse_position) > limit_mm * limit_mm) {
    return x5::ec::Action_InvalidLootingRange;
  }
  return x5::ec::Success;
}

x5::ec::EErrorCode LootSession::plan_take(const TakeSource &source, LootCommit &commit) const {
  const auto &contents = source.contents;
  const auto &rules = data_->rules();
  auto &after = commit.after;
  after = commit.before;
  if (after.revision == std::numeric_limits<std::int64_t>::max()) {
    return x5::ec::Npc_FailLootTransaction;
  }
  ++after.revision;

  // CUSTOM: gold above the wallet limit stays on the corpse while everything else is taken.
  // Graduated characters hold the full wallet, so refusing the whole take made every corpse
  // with kill gold, including quest drops, impossible to loot for them (official rule not
  // captured).
  const auto gold_room = std::max<std::int64_t>(0, data_->gold_limit() - after.gold);
  after.gold += std::min(contents.gold, gold_room);

  for (const auto &[item_cms_id, quantity] : contents.stackables) {
    const bool is_new_stack = !after.stackables.contains(item_cms_id);
    if (is_new_stack && after.stackables.size() >= kMaximumStackableKinds) {
      return rules.full_bag_error;
    }

    auto &owned = after.stackables[item_cms_id];
    if (quantity > std::numeric_limits<int>::max() - owned) {
      return rules.count_limit_error;
    }
    owned += static_cast<int>(quantity);
  }

  const auto free_slots = available_inventory_slots(commit.character, contents.instances.size());
  if (free_slots.size() < contents.instances.size()) {
    return rules.full_bag_error;
  }
  const auto created_epoch_ticks = persistence_.epoch_ticks ? persistence_.epoch_ticks() : 0;
  for (std::size_t index = 0; index < contents.instances.size(); ++index) {
    const auto &instance = contents.instances[index];
    const auto *definition = data_->item(instance.item_cms_id);
    if (definition == nullptr) {
      return x5::ec::Npc_FailLootTransaction;
    }

    CharacterItem item;
    item.cms_id = instance.item_cms_id;
    item.inventory_slot = free_slots[index];
    item.durability = definition->max_durability;
    item.grade = definition->grade;
    item.polish_step = instance.polish_step;
    item.created_epoch_ticks = created_epoch_ticks;
    commit.granted_items.push_back(item);
  }

  // A full record set only stops tracking more NPCs; it never blocks the take.
  if (source.records_first_kill && after.loot_first_kills.size() < kMaximumLootFirstKills) {
    after.loot_first_kills.insert(source.npc_cms_id);
  }

  if (persistence_.within_weight_limit && !persistence_.within_weight_limit(commit)) {
    return rules.over_weight_error;
  }
  return x5::ec::Success;
}

std::optional<GameplayPacket>
LootSession::items_update_packet(std::uint64_t unit_id, const LootContents &contents,
                                 const LootCommit &commit,
                                 const std::vector<CharacterItem> &stored) const {
  if (contents.empty()) {
    return std::nullopt;
  }

  // The empty packet_seq matches the captured looting updates.
  Json update{{"looting", {{"target_unit_id", std::to_string(unit_id)}}},
              {"packet_seq", Json::object()}};

  if (!stored.empty()) {
    Json items = Json::array();
    for (const auto &item : stored) {
      items.push_back(item_instance_json(item));
    }
    update["items"] = std::move(items);
  }

  if (!contents.stackables.empty()) {
    // Captured stack updates report the resulting total, not the dropped amount.
    Json stackables = Json::array();
    for (const auto &[item_cms_id, quantity] : contents.stackables) {
      stackables.push_back(
          {{"item_cms_id", item_cms_id}, {"quantity", commit.after.stackables.at(item_cms_id)}});
    }
    update["stackables"] = std::move(stackables);
  }

  // Report the gold actually credited, which the wallet limit can lower.
  const auto credited_gold = commit.after.gold - commit.before.gold;
  if (credited_gold > 0) {
    update["money_changes"] = Json::array({{{"change", std::to_string(credited_gold)},
                                            {"result", std::to_string(commit.after.gold)}}});
  }
  return GameplayPacket{kLootItemsUpdateOpcode, std::move(update)};
}

LootSession::TakeOutcome LootSession::commit_take(std::uint64_t unit_id, const TakeSource &source) {
  TakeOutcome outcome;
  bool durable_commit_started = false;
  try {
    if (persistence_.lookup_claim) {
      const auto existing = persistence_.lookup_claim(source.receipt_id, source.corpse_fingerprint);
      if (existing.status == LootClaimStatus::already_claimed_by_same_character) {
        if (persistence_.force_reconnect) {
          persistence_.force_reconnect();
        }
        return outcome;
      }
      if (existing.status == LootClaimStatus::claimed_by_other_character) {
        outcome.error = x5::ec::Npc_HasNoLoot;
        outcome.claimed_by_other = true;
        return outcome;
      }
      if (existing.status == LootClaimStatus::mismatch) {
        outcome.error = x5::ec::Npc_FailLootTransaction;
        outcome.receipt_mismatch = true;
        return outcome;
      }
      if (existing.status != LootClaimStatus::not_found) {
        throw std::runtime_error("Unexpected loot receipt lookup status");
      }
    }
    const auto character = persistence_.load_character();
    if (!character) {
      outcome.error = x5::ec::Npc_FailLootTransaction;
      return outcome;
    }

    LootCommit commit;
    commit.character = *character;
    commit.before = persistence_.load_progress();
    outcome.error = plan_take(source, commit);
    if (outcome.error != x5::ec::Success) {
      return outcome;
    }

    commit.receipt_id = source.receipt_id;
    commit.corpse_fingerprint = source.corpse_fingerprint;
    if (persistence_.commit_once) {
      durable_commit_started = true;
      auto result = persistence_.commit_once(commit);
      if (result.status == LootClaimStatus::already_claimed_by_same_character) {
        // The durable write already happened. The session adapter reloads current state;
        // never replay stale item rows or calculate a second progress update.
        if (persistence_.force_reconnect) {
          persistence_.force_reconnect();
        }
        return outcome;
      }
      if (result.status == LootClaimStatus::claimed_by_other_character) {
        outcome.error = x5::ec::Npc_HasNoLoot;
        outcome.claimed_by_other = true;
        return outcome;
      }
      if (result.status == LootClaimStatus::mismatch) {
        outcome.error = x5::ec::Npc_FailLootTransaction;
        outcome.receipt_mismatch = true;
        return outcome;
      }
      if (result.status != LootClaimStatus::committed) {
        outcome.error = x5::ec::Npc_FailLootTransaction;
        return outcome;
      }
      // The adapter has reconciled the authoritative snapshot. Use it for totals even if
      // another serialized progress event advanced the character after the grant committed.
      commit.after = persistence_.load_progress();
      outcome.items_update = items_update_packet(unit_id, source.contents, commit,
                                                 result.granted_items);
    } else {
      auto committed = persistence_.commit(commit);
      if (!committed) {
        outcome.error = x5::ec::Npc_FailLootTransaction;
        return outcome;
      }
      outcome.items_update = items_update_packet(unit_id, source.contents, commit, *committed);
    }
  } catch (const std::exception &error) {
    spdlog::error("Corpse loot commit failed for character {}: {}", character_id_, error.what());
    if (durable_commit_started && persistence_.force_reconnect) {
      try {
        persistence_.force_reconnect();
      } catch (const std::exception &) {
        spdlog::error("Unable to schedule a reconnect after an uncertain loot commit");
        throw;
      }
    }
    outcome.error = x5::ec::Npc_FailLootTransaction;
    outcome.items_update.reset();
  }
  return outcome;
}

std::optional<LootSession::Target> LootSession::find_target(std::uint64_t unit_id,
                                                            std::int64_t ticks) {
  const auto own = corpses_.find(unit_id);
  if (own != corpses_.end()) {
    if (own->second.publication_pending || ticks >= own->second.expire_tick) {
      return std::nullopt;
    }

    Target target;
    target.own = &own->second;
    target.position = own->second.position;
    if (own->second.shared) {
      target.shared_key = SharedCorpseKey{own->second.owner_character_id, unit_id};
    }
    return target;
  }

  if (!data_->rules().party_shared_loot || !board_) {
    return std::nullopt;
  }
  const auto view = board_->find_for_member(unit_id, character_id_, ticks);
  if (!view) {
    return std::nullopt;
  }

  Target target;
  target.shared_key = view->key;
  target.position = view->position;
  return target;
}

x5::ec::EErrorCode LootSession::acquire(const Target &target, std::int64_t ticks,
                                        TakeSource &source) {
  const auto &rules = data_->rules();
  if (!target.shared_key) {
    const auto &corpse = *target.own;
    if (corpse.owner_character_id != character_id_) {
      return x5::ec::Npc_ImpossibleLoot;
    }
    if (!corpse.lootable) {
      return rules.repeated_take_error;
    }

    source.contents = corpse.contents;
    source.npc_cms_id = corpse.npc_cms_id;
    source.records_first_kill = corpse.records_first_kill;
    source.receipt_id = corpse.receipt_id;
    source.corpse_fingerprint = corpse.corpse_fingerprint;
    return x5::ec::Success;
  }

  auto claim = board_->claim(*target.shared_key, character_id_, ticks);
  switch (claim.status) {
  case ClaimStatus::claimed:
    source.contents = std::move(claim.corpse->contents);
    source.npc_cms_id = claim.corpse->npc_cms_id;
    source.records_first_kill = claim.corpse->records_first_kill;
    source.receipt_id = std::move(claim.corpse->receipt_id);
    source.corpse_fingerprint = std::move(claim.corpse->corpse_fingerprint);
    return x5::ec::Success;
  case ClaimStatus::not_eligible:
    return x5::ec::Npc_ImpossibleLoot;
  case ClaimStatus::already_taken:
    return rules.repeated_take_error;
  case ClaimStatus::unknown:
  case ClaimStatus::expired:
    return x5::ec::Npc_InvalidId;
  }
  throw std::logic_error("Unhandled claim status");
}

void LootSession::mark_corpse_emptied(Corpse &corpse, std::int64_t ticks) {
  corpse.lootable = false;
  corpse.contents = {};
  if (corpse.records_first_kill) {
    first_kills_reserved_.erase(corpse.npc_cms_id);
    corpse.records_first_kill = false;
  }

  // A corpse that can still be butchered keeps its lifetime until the knife is used.
  if (!corpse.butcherable) {
    shorten_to_emptied_lifetime(corpse, ticks);
  }
}

void LootSession::shorten_to_emptied_lifetime(Corpse &corpse, std::int64_t ticks) {
  // The lifetime never grows: the corpse already had a deadline from its death.
  const auto &rules = data_->rules();
  const auto anchor =
      rules.emptied_lifetime_anchor == EmptiedLifetimeAnchor::take ? ticks : corpse.death_tick;
  const auto emptied_deadline = anchor + data_->corpse_empty_ms() * kTicksPerMillisecond;
  corpse.expire_tick = std::min(corpse.expire_tick, emptied_deadline);
}

void LootSession::tell_party_taken(const SharedCorpseKey &key, std::uint64_t unit_id) {
  if (!audience_ || !board_) {
    return;
  }
  for (const auto member : board_->eligible(key)) {
    if (member != character_id_) {
      audience_->notify(member, {cleared_loot_flags(unit_id)});
    }
  }
}

std::vector<GameplayPacket> LootSession::receive(const Json &request,
                                                 const WorldPosition &player_position,
                                                 std::int64_t ticks) {
  const auto unit_id = parse_unit_id(request);
  if (!unit_id) {
    return {item_response(x5::ec::BadRequest, std::nullopt)};
  }

  auto target = find_target(*unit_id, ticks);
  if (!target) {
    return {item_response(x5::ec::Npc_InvalidId, unit_id)};
  }

  const auto range = check_range(target->position, player_position);
  if (range != x5::ec::Success) {
    return {item_response(range, unit_id)};
  }

  TakeSource source;
  const auto access = acquire(*target, ticks, source);
  if (access != x5::ec::Success) {
    return {item_response(access, unit_id)};
  }

  const bool needs_commit = !source.contents.empty() || source.records_first_kill;
  TakeOutcome outcome;
  if (needs_commit) {
    outcome = commit_take(*unit_id, source);
    if (outcome.error != x5::ec::Success) {
      // With durable receipts, an unknown acknowledgement is safe to retry: PostgreSQL
      // serializes every claimant on the same immutable corpse ID. A confirmed other claimant
      // or mismatched receipt keeps the board closed to further attempts.
      if (target->shared_key && !outcome.claimed_by_other && !outcome.receipt_mismatch) {
        board_->release(*target->shared_key);
      }
      if (target->own && outcome.claimed_by_other) {
        mark_corpse_emptied(*target->own, ticks);
      }
      return {item_response(outcome.error, unit_id)};
    }
  }

  // The take succeeded exactly once; later requests find an empty corpse.
  bool still_butcherable = false;
  if (target->own != nullptr) {
    mark_corpse_emptied(*target->own, ticks);
    still_butcherable = target->own->butcherable;
  }
  if (target->shared_key) {
    tell_party_taken(*target->shared_key, *unit_id);
  }

  // CUSTOM (guessed, not official): the reply when the roll left the corpse empty.
  if (!needs_commit && data_->rules().empty_roll_reply == EmptyRollReply::no_loot_error) {
    return {item_response(x5::ec::Npc_HasNoLoot, unit_id)};
  }

  std::vector<GameplayPacket> packets;
  if (outcome.items_update) {
    packets.push_back(std::move(*outcome.items_update));
  }
  // The knife interaction stays offered on a taken corpse that is not butchered yet.
  packets.push_back(loot_announcement(*unit_id, false, still_butcherable));
  packets.push_back(item_response(x5::ec::Success, unit_id));
  return packets;
}

std::vector<GameplayPacket> LootSession::advance(std::int64_t ticks) {
  Json removed_units = Json::array();
  for (auto corpse = corpses_.begin(); corpse != corpses_.end();) {
    if (!corpse->second.registration_complete || corpse->second.publication_pending ||
        ticks < corpse->second.expire_tick) {
      ++corpse;
      continue;
    }
    // Combat removes the other corpses from the client; this session only forgets them.
    if (corpse->second.announces_removal) {
      removed_units.push_back(std::to_string(corpse->first));
    }
    if (corpse->second.records_first_kill) {
      first_kills_reserved_.erase(corpse->second.npc_cms_id);
    }
    if (corpse->second.shared && board_) {
      board_->close({corpse->second.owner_character_id, corpse->first});
    }
    corpse = corpses_.erase(corpse);
  }

  if (removed_units.empty()) {
    return {};
  }
  return {{kLootUnitRemoveOpcode, {{"unit_ids", std::move(removed_units)}}}};
}

std::optional<std::int64_t> LootSession::next_action_tick() const {
  std::optional<std::int64_t> earliest;
  for (const auto &[unit_id, corpse] : corpses_) {
    if (!corpse.registration_complete || corpse.publication_pending) {
      continue;
    }
    if (!earliest || corpse.expire_tick < *earliest) {
      earliest = corpse.expire_tick;
    }
  }
  return earliest;
}

std::size_t LootSession::corpse_count() const {
  return corpses_.size();
}

LootPersistence tutorial_loot_persistence(AuthService &auth, const Handoff &handoff,
                                          TutorialSession &tutorial,
                                          std::function<void(const Character &)> items_changed) {
  LootPersistence persistence;
  persistence.load_character = [&auth, handoff] { return auth.world_character(handoff); };
  persistence.load_progress = [&tutorial] { return tutorial.progress(); };
  auto synchronize_replay = [&auth, &tutorial, handoff, items_changed](
                                const LootClaimResult &result) {
    const auto authoritative = auth.world_progress(handoff);
    if (authoritative.revision != tutorial.progress().revision) {
      if (tutorial.progress().revision == std::numeric_limits<std::int64_t>::max() ||
          authoritative.revision != tutorial.progress().revision + 1) {
        throw std::runtime_error("Tutorial loot progress needs reconciliation");
      }
      tutorial.adopt_committed_progress(authoritative);
    } else if (authoritative != tutorial.progress()) {
      throw std::runtime_error("Tutorial loot progress disagrees with storage");
    }
    if (items_changed && !result.granted_items.empty()) {
      const auto character = auth.world_character(handoff);
      if (!character) {
        throw std::runtime_error("Tutorial loot character disappeared");
      }
      items_changed(*character);
    }
  };
  persistence.lookup_claim = [&auth, handoff, synchronize_replay](
                                 const std::string &receipt_id,
                                 const std::string &corpse_fingerprint) {
    auto result = auth.lookup_world_loot_claim(handoff, receipt_id, corpse_fingerprint);
    if (result.status == LootClaimStatus::already_claimed_by_same_character) {
      synchronize_replay(result);
    }
    return result;
  };
  persistence.commit_once = [&auth, handoff, synchronize_replay](const LootCommit &commit) {
    ItemTrade trade;
    trade.granted_items = commit.granted_items;
    auto result = auth.commit_world_loot_claim(handoff, commit.receipt_id,
                                               commit.corpse_fingerprint, commit.before,
                                               commit.after, trade);
    if (result.status == LootClaimStatus::committed ||
        result.status == LootClaimStatus::already_claimed_by_same_character) {
      synchronize_replay(result);
    }
    return result;
  };
  return persistence;
}

} // namespace chronicles
