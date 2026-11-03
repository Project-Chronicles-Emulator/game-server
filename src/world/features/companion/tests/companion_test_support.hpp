#pragma once

#include "data/client_catalog.hpp"
#include "p_a2c.ws.pb.h"
#include "p_c2a.ws.pb.h"
#include "p_c2f.rest.pb.h"
#include "p_c2f.ws.pb.h"
#include "p_f2c.ws.pb.h"
#include "protocol/protocol.hpp"
#include "testing/memory_repositories.hpp"
#include "world/core/progress_access.hpp"
#include "world/features/companion/companion_session.hpp"
#include "world/features/npc/npc_population.hpp"
#include "world/features/trade/trade_session.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <map>
#include <set>

namespace chronicles::companion_test_support {

using Json = nlohmann::json;
using companion::CompanionUpdate;
using companion::OutboundPacket;

inline constexpr int kMap = 9012;
inline constexpr int kWorldId = 1;
inline constexpr std::int64_t kAccountId = 1;
inline constexpr int kSummonId = 2001;
inline constexpr int kSummonItem = 2001;
inline constexpr int kRepairKit = 140;
inline constexpr int kWagonId = 200;
inline constexpr int kMateId = 2;
inline constexpr int kOriginPost = 1100;
inline constexpr int kDestinationPost = 1000;
inline constexpr int kItemRewardGood = 1102;
inline constexpr int kGoldRewardGood = 1103;
inline constexpr int kRewardItem = 60007;
// 100 ns Unix-epoch ticks of 2026-10-26 00:00:00 UTC.
inline constexpr std::int64_t kNow = 17614368000000000;

inline Protocol &protocol() {
  static Protocol shared(CHRONICLES_PROTOCOL_DIR);
  return shared;
}

// Round-trips one payload through the codec and returns the decoded message.
inline Json wire(std::uint16_t opcode, const Json &message) {
  const auto decoded = protocol().decode(protocol().encode(opcode, message));
  EXPECT_EQ(decoded.at("status"), "decoded") << "opcode " << opcode;
  return decoded.at("message");
}

// Encodes a client request as the handshake decodes it for the gameplay layer.
inline Json request(std::uint16_t opcode, const Json &body) {
  const auto decoded = protocol().decode(protocol().encode(opcode, body, 0));
  EXPECT_EQ(decoded.at("status"), "decoded") << "opcode " << opcode;
  return decoded.at("message");
}

inline std::vector<Json> addressed(const CompanionUpdate &update, std::uint64_t connection,
                                   std::uint16_t opcode) {
  std::vector<Json> matches;
  for (const auto &packet : update.packets) {
    if (packet.connection_id == connection && packet.opcode == opcode) {
      matches.push_back(wire(packet.opcode, packet.message));
    }
  }
  return matches;
}

inline std::vector<Json> own(const std::vector<GameplayPacket> &packets, std::uint16_t opcode) {
  std::vector<Json> matches;
  for (const auto &packet : packets) {
    if (packet.opcode == opcode) {
      matches.push_back(wire(packet.opcode, packet.message));
    }
  }
  return matches;
}

inline companion::CompanionCatalogTables companion_tables() {
  companion::CompanionCatalogTables tables;
  tables.summon_npcs = {{{"id", "2001"}, {"ability", "Mount"}, {"FK_npcId", "2001"}}};
  tables.npcs = {{{"id", "2001"}}, {{"id", "35"}}};
  tables.items = {
      {{"id", "2001"}, {"FK_skillEffectList", "[2001]"}, {"FK_interactionAnimId", "1013"}},
      {{"id", "140"}}};
  tables.skill_effects = {
      {{"id", "2001"}, {"jsonCase", "registerSummonNpc"}, {"json", "\"cmsId\":2001"}}};
  tables.vehicles = {{{"id", "200"},
                      {"vehicleType", "TradeWagon"},
                      {"maxLoadCapacity", "7"},
                      {"priceType", "Gold"},
                      {"priceValue", "100"},
                      {"health", "5000"},
                      {"FK_interactionGroupId", "62"}}};
  tables.interactions = {{{"id", "62"},
                          {"groupId", "62"},
                          {"FK_conditionId", "62"},
                          {"playerTaskCase", "RepairWagon"},
                          {"playerTask", "\"addDurability\":1500"}}};
  tables.interaction_conditions = {
      {{"conditionId", "62"}, {"typeCase", "DurabilityBelowThreshold"}, {"type", ""}},
      {{"conditionId", "62"}, {"typeCase", "ConsumeItem"}, {"type", "\"itemId\":140,\"count\":1"}}};
  tables.mate_npcs = {{{"id", "2"}, {"FK_npcId", "35"}}};
  for (int level = 1; level <= 4; ++level) {
    tables.mate_intimate_levels.push_back({{"FK_mateNpc", "2"},
                                           {"level", std::to_string(level)},
                                           {"nextLevelIntimate", std::to_string(level * 100)}});
  }
  tables.doodad_mate_interactions = {{{"FK_doodad", "160021"},
                                      {"FK_mateNpcs", "(2)"},
                                      {"intimateLevel", "1"},
                                      {"probability", "100"}}};
  return tables;
}

inline std::shared_ptr<const companion::CompanionCatalog> companion_catalog() {
  static const auto catalog =
      std::make_shared<const companion::CompanionCatalog>(companion_tables());
  return catalog;
}

inline Json trading_post_layer() {
  // Centimeters, as in the client's tradingpost.json.
  const auto post = [](int id, double x, double y, double z) {
    return Json{{"cmsId", id},
                {"tradeWagonParkingPos", {{"x", x}, {"y", y}, {"z", z}, {"yaw", 10.0}}},
                {"tradeWagonDeparturePos", {{"x", x}, {"y", y}, {"z", z}, {"yaw", 10.0}}},
                {"tradeWagonAsyncSphere",
                 {{"center", {{"x", x}, {"y", y}, {"z", z}}}, {"radius", 1200.0}}}};
  };
  return {{"tradingPosts", Json::array({post(kDestinationPost, -84726.0, 370437.0, -62848.0),
                                        post(kOriginPost, -148012.0, 336311.0, -60174.0)})}};
}

inline trade::TradeCatalogTables trade_tables() {
  trade::TradeCatalogTables tables;
  const auto good = [](int id, int basic_value) {
    return Json{{"id", std::to_string(id)},
                {"group", "None"},
                {"FK_tradeGoodQualityId", ""},
                {"basicValue", std::to_string(basic_value)},
                {"validPeriod", "1440"}};
  };
  tables.trade_goods = {good(kItemRewardGood, 1000), good(kGoldRewardGood, 1200)};
  tables.trading_posts = {{{"id", "1000"}, {"group", "100"}}, {{"id", "1100"}, {"group", "100"}}};
  tables.trading_post_goods = {{{"FK_tradingPostId", "1100"}, {"FK_tradeGoodId", "1102"}},
                               {{"FK_tradingPostId", "1100"}, {"FK_tradeGoodId", "1103"}}};
  tables.trading_post_purchases = {{{"tradingPostId", "1000"},
                                    {"FK_tradeGoodId", "1102"},
                                    {"startTradingPostId", "1100"},
                                    {"FK_Item", "60007"},
                                    {"rewardItemAmount", "6"}},
                                   {{"tradingPostId", "1000"},
                                    {"FK_tradeGoodId", "1103"},
                                    {"startTradingPostId", "1100"},
                                    {"moneyType", "Gold"},
                                    {"rewardBasisPoints", "13000"}}};
  tables.make_trade_goods = {{{"FK_TradeGood", "1102"}, {"costType", "Gold"}, {"costValue", "80"}}};
  tables.dialogue_selections = {{{"id", "100097"}, {"playerTaskCase", "TradeManagement"}}};
  tables.trade_pattern_descs = {{{"id", "3000"}, {"icon", "1"}}, {{"id", "1000"}, {"icon", "1"}}};
  tables.game_variables = {{{"name", "TradeWagonRespawnCooldown"}, {"value", "30000"}}};
  tables.maps = {
      {{"id", "9012"},
       {"world", "/Game/Maps/ArcheWorld/ArcheWorld.ArcheWorld"},
       {"area",
        R"("min":{"x":-265200,"y":200000,"z":-100000},"max":{"x":0,"y":400000,"z":100000})"}}};
  tables.trading_post_layers = {trading_post_layer()};
  return tables;
}

inline std::shared_ptr<const trade::TradeCatalog> trade_catalog() {
  static const auto catalog = std::make_shared<const trade::TradeCatalog>(trade_tables());
  return catalog;
}

inline std::shared_ptr<const ShopCatalog> item_catalog() {
  ShopCatalogTables tables;
  tables.items = {{{"id", "60007"}, {"category", "CraftMaterial"}, {"grade", "Normal"}},
                  {{"id", "140"}, {"category", "Consumable"}, {"grade", "Normal"}},
                  {{"id", "2001"}, {"category", "Consumable"}, {"grade", "Normal"}}};
  tables.game_variables = {{{"name", "ItemMaxStack"}, {"value", "1000"}},
                           {{"name", "ResetBaseDateTime"}, {"value", "2023-01-02 05:00:00"}}};
  tables.money = {{{"type", "Gold"}, {"limitUserMax", "9000000000000000"}}};
  static const auto catalog = std::make_shared<const ShopCatalog>(tables);
  return catalog;
}

inline WorldPosition at(int x_mm, int y_mm, int z_mm = 0) {
  return {kMap, x_mm, y_mm, z_mm, 0.0F};
}

inline companion::Participant participant(std::uint64_t connection, std::int64_t character,
                                          const WorldPosition &position) {
  return {connection, character, 0, position};
}

class SessionTest : public testing::Test {
protected:
  void SetUp() override {
    world_ = std::make_shared<companion::CompanionWorld>(companion_catalog(),
                                                         companion::CompanionWorldConfig{});
    character_ = add_character("Rider", 0);
    other_ = add_character("Watcher", 1);
    companions_ = make_companions(character_.id, 100);
    watcher_ = make_companions(other_.id, 200);
  }

  Character add_character(const std::string &name, int slot) {
    Character created;
    created.account_id = kAccountId;
    created.slot = slot;
    created.name = name;
    return store_.characters->create(created);
  }

  std::unique_ptr<companion::CompanionSession> make_companions(std::int64_t character,
                                                               std::uint64_t connection) {
    return std::make_unique<companion::CompanionSession>(
        companion::CompanionServices{world_, companion_catalog(), item_catalog()},
        std::make_unique<RepositoryProgressAccess>(*store_.characters, *store_.character_state,
                                                   kAccountId, character, kWorldId),
        connection, character);
  }

  void change_progress(const std::function<void(WorldProgress &)> &change) {
    const auto before =
        store_.character_state->load_world_progress(kAccountId, character_.id, kWorldId);
    auto after = before;
    after.revision = before.revision + 1;
    change(after);
    ASSERT_TRUE(store_.character_state->save_world_progress(kAccountId, character_.id, kWorldId,
                                                            before, after));
  }

  WorldProgress stored() {
    return store_.character_state->load_world_progress(kAccountId, character_.id, kWorldId);
  }

  MemoryRepositories store_;
  std::shared_ptr<companion::CompanionWorld> world_;
  Character character_;
  Character other_;
  std::unique_ptr<companion::CompanionSession> companions_;
  std::unique_ptr<companion::CompanionSession> watcher_;
};

class TradeTest : public SessionTest {
protected:
  void SetUp() override {
    SessionTest::SetUp();
    trade_ = std::make_unique<trade::TradeSession>(
        trade::TradeServices{world_, companion_catalog(), trade_catalog(), item_catalog(),
                             std::make_shared<const std::set<int>>(std::set<int>{1001316})},
        std::make_unique<RepositoryProgressAccess>(*store_.characters, *store_.character_state,
                                                   kAccountId, character_.id, kWorldId),
        100, character_.id);
    change_progress([](WorldProgress &progress) {
      progress.gold = 10000;
    });
    origin_ = trade_catalog()->post(kOriginPost)->departure;
    destination_ = trade_catalog()->post(kDestinationPost)->departure;
    companions_->enter_world(participant(100, character_.id, here(origin_)));
  }

  static WorldPosition here(const trade::WorldPoint &point, int offset_mm = 0) {
    return {kMap, point.x_mm + offset_mm, point.y_mm, point.z_mm, 0.0F};
  }

  companion::SessionResult send(std::uint16_t opcode, const Json &body,
                                const WorldPosition &position, std::int64_t now = kNow) {
    return trade_->receive(opcode, request(opcode, body), ++sequence_, position, now);
  }

  // Opens the trade UI at the post the NPC stands at.
  void open_trade_ui(const trade::WorldPoint &post) {
    const auto npc = here(post, 1000);
    trade_->start_dialogue(77, npc, here(post, 2000));
    const auto ended =
        send(trade::kDialogueFunctionRequest,
             {{"dialogue_cms_id", 100097}, {"sequence_order", 1}, {"select_order", 1}},
             here(post, 2000));
    ASSERT_EQ(own(ended.own, trade::kDialogueEndNotification).size(), 1u);
  }

  std::unique_ptr<trade::TradeSession> trade_;
  trade::WorldPoint origin_;
  trade::WorldPoint destination_;
  std::uint32_t sequence_{};
};

} // namespace chronicles::companion_test_support
