#pragma once

#include "world/features/loot/loot.hpp"
#include "testing/memory_repositories.hpp"
#include "protocol/protocol.hpp"

#include <gtest/gtest.h>

#include <deque>
#include <functional>
#include <string>
#include <utility>

// Shared fixtures for loot_tests.cpp and loot_custom_tests.cpp.
namespace chronicles::loot_test {

using Json = nlohmann::json;

constexpr int kHerb = 100;
constexpr int kStone = 101;
constexpr int kQuestItem = 102;
constexpr int kSword = 200;
constexpr int kOre = 300;

constexpr int kFullPackNpc = 5001;
constexpr int kSinglePackNpc = 5002;
constexpr int kPartialPackNpc = 5003;
constexpr int kGoldOnlyNpc = 5004;
constexpr int kFirstKillNpc = 5005;
constexpr int kQuestNpc = 5006;
constexpr int kPolishNpc = 5007;
constexpr int kHandlingNpc = 5008;
constexpr int kHandlingOnlyNpc = 5009;
constexpr int kStoneNpc = 5010;
constexpr int kUnlootedNpc = 5099;

constexpr int kFullPack = 10;
constexpr int kSinglePack = 11;
constexpr int kPartialPack = 12;
constexpr int kFirstKillPack = 13;
constexpr int kQuestPack = 14;
constexpr int kPolishPack = 15;
constexpr int kStonePack = 16;
constexpr int kHandlingPack = 20;

constexpr int kQuestId = 555;
constexpr int kPolishStep = 3;
constexpr std::int64_t kKillGold = 7;
constexpr std::int64_t kHandlingPercent = 50;

constexpr int kMap = 8000;
constexpr std::int64_t kStartTicks = 100000000;
constexpr std::int64_t kTicksPerMs = 10000;
constexpr std::int64_t kGoldLimit = 1000;
constexpr std::int64_t kCreatedEpochTicks = 123456789;
constexpr std::uint64_t kCorpse = 4242;
constexpr int kSwordGrade = 1;

inline Json entry(const char *kind, int item, int minimum, int maximum, int weight = 1,
                  int polish_step = 0) {
  Json result{
      {"kind", kind}, {"min_quantity", minimum}, {"max_quantity", maximum}, {"weight", weight}};
  if (std::string(kind) == "item") {
    result["item_cms_id"] = item;
  }
  if (polish_step != 0) {
    result["polish_step"] = polish_step;
  }
  return result;
}

// Builds the arrays explicitly: copy-list-initializing a Json parameter from a single Json
// element would copy the element instead of wrapping it in an array.
inline Json group(double rate, std::initializer_list<Json> entries,
                  std::initializer_list<const char *> rules = {}) {
  Json entry_array = Json::array();
  for (const auto &candidate : entries) {
    entry_array.push_back(candidate);
  }
  Json rule_array = Json::array();
  for (const char *rule : rules) {
    rule_array.push_back(rule);
  }
  return {{"loot_group_id", 1},
          {"rate_percent", rate},
          {"custom_rules", std::move(rule_array)},
          {"entries", std::move(entry_array)}};
}

inline Json pack(int id, double rate, std::initializer_list<Json> groups) {
  Json group_array = Json::array();
  for (const auto &candidate : groups) {
    group_array.push_back(candidate);
  }
  return {{"loot_pack_id", id}, {"rate_percent", rate}, {"groups", std::move(group_array)}};
}

inline Json item_definition(const char *storage, const char *grade, int durability,
                            std::initializer_list<int> quests = {}) {
  Json quest_array = Json::array();
  for (const int quest : quests) {
    quest_array.push_back(quest);
  }
  return {{"storage", storage},
          {"grade", grade},
          {"max_durability", durability},
          {"required_quests", std::move(quest_array)}};
}

inline Json npc_row(int npc, int pack_id, int handling_pack_id = 0, double handling_rate = 0.0,
                    std::int64_t gold = 0) {
  return Json::array({npc, pack_id, handling_pack_id, handling_rate, gold});
}

// Pack 10 always grants 2-4 herbs, 5-9 gold and one sword; pack 11 grants one herb; pack 12 is
// a 50% pack with one 25% group choosing between herb (weight 3) and ore (weight 1). The other
// packs exercise the custom rules: 13 has a first-kill group, 14 a quest-gated item, 15 a
// polished sword, 16 a full moon stone, and handling pack 20 grants two herbs.
inline Json profile_fixture() {
  Json packs = Json::array();
  packs.push_back(
      pack(kFullPack, 100.0,
           {group(100.0, {entry("item", kHerb, 2, 4)}), group(100.0, {entry("gold", 0, 5, 9)}),
            group(100.0, {entry("item", kSword, 1, 1)})}));
  packs.push_back(pack(kSinglePack, 100.0, {group(100.0, {entry("item", kHerb, 1, 1)})}));
  packs.push_back(
      pack(kPartialPack, 50.0,
           {group(25.0, {entry("item", kHerb, 1, 1, 3), entry("item", kOre, 1, 1, 1)})}));
  packs.push_back(pack(kFirstKillPack, 100.0,
                       {group(100.0, {entry("item", kHerb, 1, 1)}),
                        group(100.0, {entry("item", kStone, 1, 1)}, {"first_kill_rewards"})}));
  packs.push_back(pack(kQuestPack, 100.0,
                       {group(100.0, {entry("item", kQuestItem, 1, 1), entry("item", kHerb, 1, 1)},
                              {"quest_gated_rows"})}));
  packs.push_back(
      pack(kPolishPack, 100.0,
           {group(100.0, {entry("item", kSword, 1, 1, 1, kPolishStep)}, {"polish_step_rows"})}));
  packs.push_back(
      pack(kStonePack, 100.0, {group(100.0, {entry("item", kStone, 1, 1)}, {"full_moon_stones"})}));

  Json handling_packs = Json::array();
  handling_packs.push_back(
      pack(kHandlingPack, 100.0, {group(100.0, {entry("item", kHerb, 2, 2)}, {"handling_loot"})}));

  Json npcs = Json::array();
  npcs.push_back(npc_row(kFullPackNpc, kFullPack));
  npcs.push_back(npc_row(kSinglePackNpc, kSinglePack));
  npcs.push_back(npc_row(kPartialPackNpc, kPartialPack));
  npcs.push_back(npc_row(kGoldOnlyNpc, 0, 0, 0.0, kKillGold));
  npcs.push_back(npc_row(kFirstKillNpc, kFirstKillPack));
  npcs.push_back(npc_row(kQuestNpc, kQuestPack));
  npcs.push_back(npc_row(kPolishNpc, kPolishPack));
  npcs.push_back(
      npc_row(kHandlingNpc, kSinglePack, kHandlingPack, static_cast<double>(kHandlingPercent)));
  npcs.push_back(npc_row(kHandlingOnlyNpc, 0, kHandlingPack, 100.0));
  npcs.push_back(npc_row(kStoneNpc, kStonePack));

  return {
      {"format_version", 2},
      {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
      {"looting_range_mm", 5000},
      {"corpse_lootable_ms", 90000},
      {"corpse_empty_ms", 10000},
      {"gold_limit", kGoldLimit},
      {"items",
       {{"100", item_definition("stackable", "Normal", 0)},
        {"101", item_definition("stackable", "Rare", 0)},
        {"102", item_definition("stackable", "Normal", 0, {kQuestId})},
        {"200", item_definition("instance", "Grand", 10000)},
        {"300", item_definition("stackable", "Rare", 0)}}},
      {"packs", std::move(packs)},
      {"handling_packs", std::move(handling_packs)},
      {"npc_fields",
       {"npc_cms_id", "loot_pack_id", "handling_pack_id", "handling_rate_percent", "kill_gold"}},
      {"npcs", std::move(npcs)},
  };
}

// Records every request and answers from queues, falling back to the first/minimum outcome.
class ScriptedLootRandom final : public LootRandom {
public:
  std::deque<bool> pass_results;
  std::deque<std::size_t> indexes;
  std::deque<int> quantities;
  std::vector<double> rates_seen;
  std::vector<std::vector<int>> weights_seen;
  std::vector<std::pair<int, int>> ranges_seen;

  bool passes(double rate_percent) override {
    rates_seen.push_back(rate_percent);
    return take(pass_results, true);
  }

  std::size_t pick_index(const std::vector<int> &weights) override {
    weights_seen.push_back(weights);
    return take(indexes, std::size_t{0});
  }

  int pick_quantity(int minimum, int maximum) override {
    ranges_seen.emplace_back(minimum, maximum);
    return take(quantities, minimum);
  }

  std::size_t draws() const {
    return rates_seen.size() + weights_seen.size() + ranges_seen.size();
  }

private:
  template <typename Value> static Value take(std::deque<Value> &queue, Value fallback) {
    if (queue.empty()) {
      return fallback;
    }
    const auto value = queue.front();
    queue.pop_front();
    return value;
  }
};

inline void expect_encodes(const std::vector<GameplayPacket> &packets) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  for (const auto &packet : packets) {
    EXPECT_NO_THROW(protocol.encode(packet.opcode, packet.message))
        << packet.opcode << ": " << packet.message;
  }
}

inline Json loot_request(std::uint64_t unit_id) {
  return {{"unit_id", std::to_string(unit_id)}};
}

inline std::string error_of(const std::vector<GameplayPacket> &packets) {
  EXPECT_EQ(packets.size(), 1U);
  if (packets.size() != 1U) {
    return "<unexpected packet count>";
  }
  EXPECT_EQ(packets.front().opcode, kLootItemResponseOpcode);
  return packets.front().message.value("ec", std::string());
}

// Records party queries and notifications instead of delivering them.
class FakeAudience final : public LootAudience {
public:
  struct Sent {
    std::int64_t character_id{};
    GameplayPacket packet;
  };

  std::vector<std::int64_t> members;
  int queries{};
  double last_range_mm{};
  std::vector<Sent> sent;

  std::vector<std::int64_t> party_members_near(const MonsterDefeat &, double range_mm) override {
    ++queries;
    last_range_mm = range_mm;
    return members;
  }

  void notify(std::int64_t character_id, std::vector<GameplayPacket> packets) override {
    for (auto &packet : packets) {
      sent.push_back({character_id, std::move(packet)});
    }
  }

  // Packets sent to one character, in order.
  std::vector<GameplayPacket> sent_to(std::int64_t character_id) const {
    std::vector<GameplayPacket> result;
    for (const auto &item : sent) {
      if (item.character_id == character_id) {
        result.push_back(item.packet);
      }
    }
    return result;
  }
};

// Two characters in the memory store (the owner and a party member); commits go through the real
// revision-checked trade path.
class LootSessionTest : public testing::Test {
protected:
  void SetUp() override {
    character.account_id = 1;
    character.name = "LootTester";
    character = store.characters->create(character);
    member.account_id = 1;
    member.slot = 1;
    member.name = "LootMember";
    member = store.characters->create(member);
  }

  LootPersistence persistence_for(const Character &who) {
    LootPersistence result;
    const auto character_id = who.id;
    result.load_character = [this, character_id]() -> std::optional<Character> {
      if (forced_character) {
        return forced_character;
      }
      for (auto &candidate : store.characters->characters(1, 1)) {
        if (candidate.id == character_id) {
          return candidate;
        }
      }
      return std::nullopt;
    };
    result.load_progress = [this, character_id] {
      return store.character_state->load_world_progress(1, character_id, 1);
    };
    result.commit = [this, character_id](
                        const LootCommit &commit) -> std::optional<std::vector<CharacterItem>> {
      ++commit_attempts;
      if (before_commit) {
        before_commit();
      }
      return store.character_state->commit_item_trade(
          1, character_id, 1, commit.before, commit.after, ItemTrade{commit.granted_items, {}, {}});
    };
    result.epoch_ticks = [] { return kCreatedEpochTicks; };
    if (weight_policy) {
      result.within_weight_limit = weight_policy;
    }
    return result;
  }

  LootPersistence persistence() {
    return persistence_for(character);
  }

  // The owner's session. Sets `data` and `random`.
  LootSession make_session(const Json &profile = profile_fixture(),
                           const LootCustomRules &rules = LootCustomRules::all_enabled(),
                           std::shared_ptr<LootAudience> audience = nullptr,
                           std::shared_ptr<SharedLootBoard> board = nullptr) {
    data = std::make_shared<const LootData>(profile, rules);
    return make_session_for(character, data, random, std::move(audience), std::move(board));
  }

  // A session for any character that shares `loot_data`; its scripted random is returned.
  LootSession make_session_for(const Character &who, std::shared_ptr<const LootData> loot_data,
                               ScriptedLootRandom *&random_out,
                               std::shared_ptr<LootAudience> audience = nullptr,
                               std::shared_ptr<SharedLootBoard> board = nullptr) {
    auto scripted = std::make_unique<ScriptedLootRandom>();
    random_out = scripted.get();
    return LootSession(std::move(loot_data), who.id, persistence_for(who), std::move(scripted),
                       std::move(audience), std::move(board));
  }

  MonsterDefeat defeat(std::uint64_t unit_id, int npc_cms_id, std::int64_t ticks = kStartTicks) {
    MonsterDefeat result;
    result.unit_id = unit_id;
    result.npc_cms_id = npc_cms_id;
    result.killer_character_id = character.id;
    result.position = {kMap, 0, 0, 0, 0};
    result.ticks = ticks;
    return result;
  }

  WorldPosition player_at(int x_mm) const {
    return {kMap, x_mm, 0, 0, 0};
  }

  WorldProgress progress() {
    return store.character_state->load_world_progress(1, character.id, 1);
  }

  WorldProgress progress_of(const Character &who) {
    return store.character_state->load_world_progress(1, who.id, 1);
  }

  // Applies a progress change the way another gameplay system would.
  void update_progress_of(const Character &who,
                          const std::function<void(WorldProgress &)> &change) {
    const auto before = progress_of(who);
    auto after = before;
    change(after);
    after.revision = before.revision + 1;
    ASSERT_TRUE(store.character_state->save_world_progress(1, who.id, 1, before, after));
  }

  void update_progress(const std::function<void(WorldProgress &)> &change) {
    update_progress_of(character, change);
  }

  std::vector<CharacterItem> stored_items() {
    return persistence().load_character()->items;
  }

  MemoryRepositories store;
  Character character;
  Character member;
  std::shared_ptr<const LootData> data;
  ScriptedLootRandom *random{};
  std::optional<Character> forced_character;
  std::function<void()> before_commit;
  std::function<bool(const LootCommit &)> weight_policy;
  int commit_attempts{};
};

} // namespace chronicles::loot_test
