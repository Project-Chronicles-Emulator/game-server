#include "world/features/shop/npc_shop.hpp"

#include "testing/memory_repositories.hpp"

#include "data/client_catalog.hpp"
#include "cms.generated.pb.h"
#include "protocol/protocol.hpp"
#include "data/shop_conditions.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <string>

namespace chronicles {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

// Catalog identifiers mirror the captured merchant evidence: shop 1001 (interaction 212) sold
// items 133/15010403/11200201 at Item.priceValue, shop 1002 (interaction 213) sold 70109003 at a
// 2000 per-mille multiplier, and refunds matched Item.refundValue.
constexpr int kGeneralShop = 1001;
constexpr int kMaterialShop = 1002;
constexpr int kGeneralInteraction = 212;
constexpr int kMaterialInteraction = 213;
constexpr int kPermitInteraction = 300;
constexpr int kBrokenConditionInteraction = 301;
constexpr int kRepairInteraction = 65;
constexpr int kVoucherInteraction = 30;
constexpr int kGeneralMerchantNpc = 1001007;
constexpr int kMaterialMerchantNpc = 1001001;
constexpr int kPermitMerchantNpc = 1001999;
constexpr int kBrokenConditionNpc = 1001998;
constexpr int kBlacksmithNpc = 1001096;
constexpr int kVoucherNpc = 1001425;
constexpr int kPlainNpc = 1001294;

constexpr int kStoneMaterial = 70109003;
constexpr int kHealingPotion = 15010403;
constexpr int kPickaxe = 11200201;
constexpr int kKnife = 11200301;
constexpr int kLeather = 72000016;
constexpr int kHousingChair = 1900000001;
constexpr int kTokenPotion = 90002;
constexpr int kTokenChair = 1900000002;
constexpr int kOddPricedPotion = 90003;
constexpr int kQuestToken = 90004;
constexpr int kDailyLimitedPotion = 90005;
constexpr int kConditionalPotion = 90006;
constexpr int kBuildingDrawing = 20000020;
constexpr int kHousingBuilding = 2000001002;
constexpr int kHousingPermit = 123;
constexpr int kHeavyCrate = 90007;
constexpr int kHelmet = 11401101;
constexpr int kLongbow = 13102102;
constexpr int kWildAxe = 11102302;

constexpr int kGoodWithoutCondition = 100;
constexpr int kGoodWithCondition = 101;

constexpr std::uint64_t kMerchantUnit = 0x1000000000001002ULL;
constexpr std::uint64_t kFarMerchantUnit = 0x1000000000002002ULL;
constexpr int kMap = 8000;
constexpr int kCharacterId = 7;
constexpr int kArmorHelmetSlot = 19;

Json item_row(int cms_id, const std::string &category, const std::string &price_type,
              const std::string &price, const std::string &refund_type, const std::string &refund,
              const std::string &durability = "", const std::string &weight = "") {
  return {
      {"id", std::to_string(cms_id)}, {"category", category},        {"grade", "Normal"},
      {"priceType", price_type},      {"priceValue", price},         {"refundType", refund_type},
      {"refundValue", refund},        {"maxDurability", durability}, {"weight", weight}};
}

Json repairable_row(int cms_id, const std::string &equipment_type, const std::string &grade,
                    const std::string &slot, const std::string &durability) {
  auto row = item_row(cms_id, "Equipment", "Gold", "300", "Gold", "100", durability);
  row["equipmentType"] = equipment_type;
  row["grade"] = grade;
  row["equipSlot"] = slot;
  row["npcRepairable"] = "True";
  return row;
}

Json repair_cost_row(const std::string &equipment_type, const std::string &grade,
                     const std::string &slot, const std::string &cost) {
  return {{"category", "Equipment"}, {"equipmentType", equipment_type},
          {"grade", grade},          {"equipType", slot},
          {"costType", "Gold"},      {"costValue", cost},
          {"maxDurability", "1000"}};
}

Json shop_row(int shop_id, int item_cms_id, const std::string &price_multiplier = "",
              const std::string &limit = "", const std::string &reset = "",
              const std::string &condition = "") {
  return {{"id", std::to_string(shop_id)},
          {"FK_itemId", std::to_string(item_cms_id)},
          {"priceMultiplierPerMille", price_multiplier},
          {"refundMultiplierPerMille", ""},
          {"limitAmount", limit},
          {"resetPeriodType", reset},
          {"fk_shopItemCondition", condition}};
}

Json interaction_row(int id, int group, const std::string &task_case, const std::string &task,
                     const std::string &condition = "") {
  return {{"id", std::to_string(id)},
          {"groupId", std::to_string(group)},
          {"playerTaskCase", task_case},
          {"playerTask", task},
          {"FK_conditionId", condition}};
}

Json npc_row(int id, int group) {
  return {{"id", std::to_string(id)}, {"interactionGroupId", std::to_string(group)}};
}

Json condition_row(const std::string &id, const std::string &group, const std::string &op,
                   const std::string &type_case, const std::string &type,
                   const std::string &negated = "") {
  return {{"conditionId", id},  {"group", group},        {"logicalOperator", op},
          {"negated", negated}, {"typeCase", type_case}, {"type", type}};
}

ShopCatalogTables test_tables() {
  ShopCatalogTables tables;
  tables.items = {
      item_row(kStoneMaterial, "CraftMaterial", "Gold", "5", "Gold", "2", "", "10"),
      item_row(kHealingPotion, "Consumable", "Gold", "200", "Gold", "40", "", "400"),
      repairable_row(kPickaxe, "Pick", "Normal", "Tool_3", "2000"),
      repairable_row(kKnife, "Knife", "Normal", "Tool_1", "2000"),
      repairable_row(kHelmet, "None", "Normal", "ArmorHelmet", "10000"),
      repairable_row(kLongbow, "LongBow", "Grand", "Weapon_1", "10000"),
      item_row(kWildAxe, "Equipment", "Gold", "300", "Gold", "100", "10000"),
      item_row(kLeather, "CraftMaterial", "Gold", "10", "Gold", "1"),
      item_row(kHousingChair, "HousingObject", "Gold", "100", "Gold", "20", "", "5"),
      item_row(kTokenChair, "HousingObject", "HersanderToken", "10", "Gold", "100"),
      item_row(kTokenPotion, "Consumable", "HersanderToken", "5", "", ""),
      item_row(kOddPricedPotion, "Consumable", "Gold", "3", "Gold", "1"),
      item_row(kQuestToken, "QuestItem", "Gold", "1", "", ""),
      item_row(kDailyLimitedPotion, "Consumable", "Gold", "1", "Gold", "1"),
      item_row(kConditionalPotion, "Consumable", "Gold", "1", "Gold", "1"),
      item_row(kBuildingDrawing, "HousingBuildingDrawing", "Gold", "3000", "Gold", "300"),
      item_row(kHousingBuilding, "HousingBuilding", "Gold", "5000", "Gold", "500"),
      item_row(kHousingPermit, "HousingPermit", "Gold", "500", "", ""),
      item_row(kHeavyCrate, "Consumable", "Gold", "1", "Gold", "1", "", "1000"),
  };
  tables.item_repair_costs = {
      repair_cost_row("Pick", "Normal", "Tool_3", "10"),
      repair_cost_row("None", "Normal", "ArmorHelmet", "75"),
      repair_cost_row("LongBow", "Grand", "Weapon_1", "500"),
  };
  tables.shops = {
      shop_row(kGeneralShop, kHealingPotion),
      shop_row(kGeneralShop, kPickaxe),
      shop_row(kGeneralShop, kStoneMaterial, "2000"),
      shop_row(kGeneralShop, kHousingChair),
      shop_row(kGeneralShop, kTokenChair),
      shop_row(kGeneralShop, kHousingBuilding),
      shop_row(kGeneralShop, kHousingPermit),
      shop_row(kGeneralShop, kTokenPotion),
      shop_row(kGeneralShop, kOddPricedPotion, "1500"),
      shop_row(kGeneralShop, kDailyLimitedPotion, "", "2", "Daily"),
      shop_row(kGeneralShop, kConditionalPotion, "", "", "", "13"),
      shop_row(kGeneralShop, kBuildingDrawing),
      shop_row(kGeneralShop, kHeavyCrate),
      shop_row(kMaterialShop, kStoneMaterial, "2000"),
  };
  tables.interactions = {
      interaction_row(kGeneralInteraction, kGeneralInteraction, "Shop", "\"shopId\":1001"),
      interaction_row(kMaterialInteraction, kMaterialInteraction, "Shop", "\"shopId\":1002"),
      interaction_row(kPermitInteraction, kPermitInteraction, "Shop", "\"shopId\":1001", "704"),
      interaction_row(kBrokenConditionInteraction, kBrokenConditionInteraction, "Shop",
                      "\"shopId\":1001", "999"),
      interaction_row(kRepairInteraction, 64, "Repair", ""),
      interaction_row(kVoucherInteraction, kVoucherInteraction, "VoucherGroup",
                      "\"voucherGroupId\":2"),
  };
  tables.npcs = {
      npc_row(kGeneralMerchantNpc, kGeneralInteraction),
      npc_row(kMaterialMerchantNpc, kMaterialInteraction),
      npc_row(kPermitMerchantNpc, kPermitInteraction),
      npc_row(kBrokenConditionNpc, kBrokenConditionInteraction),
      npc_row(kBlacksmithNpc, 64),
      npc_row(kVoucherNpc, kVoucherInteraction),
      npc_row(kPlainNpc, 0),
  };
  tables.interaction_conditions = {
      // Catalog condition 704: hasPermit false OR true, which every character satisfies.
      condition_row("704", "1", "OR", "PioneerPermit", "\"hasPermit\":false"),
      condition_row("704", "1", "OR", "PioneerPermit", "\"hasPermit\":true"),
      condition_row("999", "1", "AND", "SomeFutureKind", "\"level\":1"),
  };
  tables.shop_item_conditions = {
      condition_row("13", "1", "AND", "Company",
                    "\"companyId\":\"EX5CompanyType_BlueSalt\",\"ignorePermit\":true,\"level\":3"),
  };
  tables.voucher_groups = {
      {{"VoucherGroupId", "2"}, {"FK_tradeGoodId", "100"}, {"FK_VoucherConditionId", ""}},
      {{"VoucherGroupId", "2"}, {"FK_tradeGoodId", "101"}, {"FK_VoucherConditionId", "1004"}},
  };
  tables.voucher_conditions = {
      condition_row("1004", "1", "OR", "Company",
                    "\"companyId\":\"EX5CompanyType_BlueSalt\",\"ignorePermit\":false,"
                    "\"level\":4"),
  };
  tables.trade_goods = {
      {{"id", "100"}, {"basicValue", "100"}, {"validPeriod", "100"}, {"company", "YokedIron"}},
      {{"id", "101"}, {"basicValue", "200"}, {"validPeriod", "10"}, {"company", "YokedIron"}},
  };
  tables.game_variables = {
      {{"name", "ItemMaxStack"}, {"value", "1000"}},
      {{"name", "ResetBaseDateTime"}, {"value", "2023-01-02 05:00:00"}},
      {{"name", "BagMaxWeightMultiplier"}, {"value", "200"}},
      {{"name", "TradeRefundBasisPoints"}, {"value", "1000"}},
  };
  tables.money = {{{"type", "Gold"}, {"limitUserMax", "9000000000000000"}}};
  return tables;
}

const ShopCatalog &test_catalog() {
  static const ShopCatalog catalog(test_tables());
  return catalog;
}

std::int64_t epoch_ticks_at(std::chrono::year_month_day date, std::chrono::hours hour) {
  const auto instant = std::chrono::sys_days{date} + hour;
  return std::chrono::duration_cast<std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>>(
             instant.time_since_epoch())
      .count();
}

// A Tuesday, so daily, weekly and monthly boundaries all differ.
const std::int64_t kNow = epoch_ticks_at(std::chrono::year{2026} / 10 / 27, 12h);
constexpr std::int64_t kTicksPerHour = 36'000'000'000;

Json shop_message(const Json &shop, int interaction_id = kGeneralInteraction,
                  std::uint64_t unit_id = kMerchantUnit) {
  return {{"target_unit_id", std::to_string(unit_id)},
          {"param", {{"interaction_id", interaction_id}, {"shop", shop}}}};
}

Json repair_message(const std::vector<std::int64_t> &item_ids,
                    int interaction_id = kRepairInteraction) {
  Json ids = Json::array();
  for (const auto item_id : item_ids) {
    ids.push_back(std::to_string(item_id));
  }
  return {{"target_unit_id", std::to_string(kMerchantUnit)},
          {"param", {{"interaction_id", interaction_id}, {"repair", {{"item_ids", ids}}}}}};
}

Json voucher_message(int good_cms_id, int count) {
  return {{"target_unit_id", std::to_string(kMerchantUnit)},
          {"param",
           {{"interaction_id", kVoucherInteraction},
            {"voucher_group", {{"good_cms_id", good_cms_id}, {"count", count}}}}}};
}

Json buy(int item_cms_id, int count) {
  return {{"buy_list", Json::array({{{"item_cms_id", item_cms_id}, {"count", count}}})}};
}

Json sell_stack(int item_cms_id, int count) {
  return {{"sell_list",
           Json::array({{{"stackable", {{"item_cms_id", item_cms_id}, {"count", count}}}}})}};
}

Json sell_item(std::int64_t item_id) {
  return {{"sell_list", Json::array({{{"item_id", std::to_string(item_id)}}})}};
}

Json sell_voucher(std::int64_t voucher_id) {
  return {{"sell_list", Json::array({{{"trade_voucher_id", std::to_string(voucher_id)}}})}};
}

ShopRequest parsed(const Json &message) {
  const auto result = parse_shop_request(message);
  EXPECT_EQ(result.error, x5::ec::Success);
  return result.request;
}

Character test_character() {
  Character character;
  character.id = kCharacterId;
  character.items = {
      CharacterItem{101, kWildAxe, 0, 0, 1, 10000, 0},
      CharacterItem{102, kKnife, 1, std::nullopt, 1, 1500, 0},
      CharacterItem{103, kHelmet, 2, kArmorHelmetSlot, 1, 9088, 0},
      CharacterItem{104, kLongbow, 3, 1, 1, 9908, 0},
      CharacterItem{105, kPickaxe, 4, std::nullopt, 1, 2000, 0},
  };
  return character;
}

WorldProgress test_progress() {
  WorldProgress progress;
  progress.revision = 4;
  progress.gold = 1196;
  progress.hersander_token = 40;
  progress.stackables[kLeather] = 4;
  progress.stackables[kStoneMaterial] = 1;
  return progress;
}

MerchantTradeInput trade_input(const Character &character, const WorldProgress &progress,
                               std::int64_t now = kNow,
                               std::optional<WeightBudget> budget = std::nullopt) {
  return MerchantTradeInput{test_catalog(), character, progress, now, ConditionContext{}, budget};
}

// Plans a request against the general shop (1001).
ShopTradePlan plan(const Json &shop, const Character &character = test_character(),
                   const WorldProgress &progress = test_progress(),
                   std::optional<WeightBudget> budget = std::nullopt, std::int64_t now = kNow) {
  MerchantService service;
  service.kind = MerchantServiceKind::shop;
  service.target_id = kGeneralShop;
  return plan_merchant_trade(trade_input(character, progress, now, budget), service,
                             parsed(shop_message(shop)));
}

ShopTradePlan plan_repair(const std::vector<std::int64_t> &item_ids,
                          const Character &character = test_character(),
                          const WorldProgress &progress = test_progress()) {
  MerchantService service;
  service.kind = MerchantServiceKind::repair;
  return plan_merchant_trade(trade_input(character, progress), service,
                             parsed(repair_message(item_ids)));
}

ShopTradePlan plan_voucher(int good_cms_id, int count,
                           const WorldProgress &progress = test_progress()) {
  MerchantService service;
  service.kind = MerchantServiceKind::voucher_group;
  service.target_id = 2;
  return plan_merchant_trade(trade_input(test_character(), progress), service,
                             parsed(voucher_message(good_cms_id, count)));
}

MerchantTarget merchant_target(int npc_cms_id, std::uint64_t unit_id = kMerchantUnit,
                               int x_mm = 1000) {
  return MerchantTarget{unit_id, npc_cms_id, {kMap, x_mm, 2000, 3000, 0}};
}

MerchantAccess access_for(const Json &message, const MerchantTarget *target,
                          const std::optional<WorldPosition> &position = WorldPosition{
                              kMap, 1500, 2500, 3000, 0}) {
  return authorize_merchant_access(test_catalog(), target, parsed(message), position,
                                   ConditionContext{});
}

TEST(ShopConditions, EvaluatesTheKindsTheMerchantCatalogUses) {
  const ConditionSet conditions(test_tables().interaction_conditions);
  EXPECT_EQ(conditions.evaluate(704, ConditionContext{}), ConditionResult::met);
  ConditionContext permit_holder;
  permit_holder.has_pioneer_permit = true;
  EXPECT_EQ(conditions.evaluate(704, permit_holder), ConditionResult::met);
  EXPECT_EQ(conditions.evaluate(999, ConditionContext{}), ConditionResult::unsupported);
  EXPECT_EQ(conditions.evaluate(12345, ConditionContext{}), ConditionResult::unsupported);

  const ConditionSet company(test_tables().shop_item_conditions);
  EXPECT_EQ(company.evaluate(13, ConditionContext{}), ConditionResult::unmet);
  ConditionContext veteran;
  veteran.company_levels["EX5CompanyType_BlueSalt"] = 3;
  EXPECT_EQ(company.evaluate(13, veteran), ConditionResult::met);
  veteran.company_levels["EX5CompanyType_BlueSalt"] = 2;
  EXPECT_EQ(company.evaluate(13, veteran), ConditionResult::unmet);
}

TEST(ShopConditions, CompanyConditionsFollowTheClientCheckOrder) {
  const ConditionSet conditions({
      condition_row("13", "1", "AND", "Company",
                    "\"companyId\":\"EX5CompanyType_BlueSalt\",\"ignorePermit\":true,\"level\":3"),
      condition_row("1004", "1", "AND", "Company",
                    "\"companyId\":\"EX5CompanyType_BlueSalt\",\"ignorePermit\":false,"
                    "\"level\":4"),
      // InteractionCondition rows omit level and ignorePermit: protobuf defaults 0 and false.
      condition_row("20", "1", "AND", "Company", "\"companyId\":\"EX5CompanyType_BlueSalt\""),
      condition_row("21", "1", "AND", "Company",
                    "\"companyId\":\"EX5CompanyType_None\",\"ignorePermit\":true"),
  });

  // Another company never satisfies a Blue Salt condition, whatever its level.
  ConditionContext antiqus;
  antiqus.company_levels["EX5CompanyType_Antiqus"] = 6;
  antiqus.has_pioneer_permit = true;
  EXPECT_EQ(conditions.evaluate(13, antiqus), ConditionResult::unmet);
  EXPECT_EQ(conditions.evaluate(21, antiqus), ConditionResult::unmet);

  ConditionContext blue_salt;
  blue_salt.company_levels["EX5CompanyType_BlueSalt"] = 4;
  EXPECT_EQ(conditions.evaluate(13, blue_salt), ConditionResult::met);
  // Without ignorePermit a valid pioneer permit is required.
  EXPECT_EQ(conditions.evaluate(1004, blue_salt), ConditionResult::unmet);
  EXPECT_EQ(conditions.evaluate(20, blue_salt), ConditionResult::unmet);
  blue_salt.has_pioneer_permit = true;
  EXPECT_EQ(conditions.evaluate(1004, blue_salt), ConditionResult::met);
  EXPECT_EQ(conditions.evaluate(20, blue_salt), ConditionResult::met);

  // "None" means the character belongs to no company.
  EXPECT_EQ(conditions.evaluate(21, ConditionContext{}), ConditionResult::met);
  EXPECT_EQ(conditions.evaluate(21, blue_salt), ConditionResult::unmet);
}

TEST(ShopConditions, AppliesNegationGroupsAndSubjectCompany) {
  const ConditionSet conditions({
      condition_row("1", "1", "AND", "PioneerPermit", "\"hasPermit\":true", "True"),
      condition_row("2", "1", "AND", "CompanyLevelGreaterEqual", "\"level\":2"),
      condition_row("3", "1", "AND", "PioneerPermit", "\"hasPermit\":false"),
      condition_row("3", "2", "OR", "PioneerPermit", "\"hasPermit\":false"),
      condition_row("4", "1", "AND", "PioneerPermit", "\"hasPermit\":true"),
      condition_row("4", "1", "OR", "PioneerPermit", "\"hasPermit\":true"),
      condition_row("5", "1", "AND", "PioneerPermit", "\"hasPermit\":\"yes\""),
  });
  EXPECT_EQ(conditions.evaluate(1, ConditionContext{}), ConditionResult::met);

  ConditionContext holder;
  holder.has_pioneer_permit = true;
  EXPECT_EQ(conditions.evaluate(1, holder), ConditionResult::unmet);
  // Every group must hold: the permit-free group fails for a holder.
  EXPECT_EQ(conditions.evaluate(3, holder), ConditionResult::unmet);
  EXPECT_EQ(conditions.evaluate(3, ConditionContext{}), ConditionResult::met);
  // A group that mixes AND and OR rows, or carries a malformed parameter, fails closed.
  EXPECT_EQ(conditions.evaluate(4, holder), ConditionResult::unsupported);
  EXPECT_EQ(conditions.evaluate(5, holder), ConditionResult::unsupported);

  // CompanyLevelGreaterEqual needs the trade good's company.
  EXPECT_EQ(conditions.evaluate(2, ConditionContext{}), ConditionResult::unsupported);
  ConditionContext subject;
  subject.subject_company = "YokedIron";
  EXPECT_EQ(conditions.evaluate(2, subject), ConditionResult::unmet);
  subject.company_levels["EX5CompanyType_YokedIron"] = 2;
  EXPECT_EQ(conditions.evaluate(2, subject), ConditionResult::met);
}

TEST(ShopConditions, RejectsMalformedConditionRows) {
  EXPECT_THROW(ConditionSet({condition_row("x", "1", "AND", "Company", "\"level\":1")}),
               std::runtime_error);
  EXPECT_THROW(ConditionSet({condition_row("1", "1", "XOR", "Company", "\"level\":1")}),
               std::runtime_error);
  EXPECT_THROW(ConditionSet({condition_row("1", "1", "AND", "Company", "not json")}),
               std::runtime_error);
}

TEST(NpcShop, CatalogLinksNpcInteractionGroupsToServices) {
  const auto &catalog = test_catalog();
  const auto shop = catalog.service_for(kGeneralMerchantNpc, kGeneralInteraction);
  ASSERT_TRUE(shop);
  EXPECT_EQ(shop->kind, MerchantServiceKind::shop);
  EXPECT_EQ(shop->target_id, kGeneralShop);
  EXPECT_FALSE(shop->condition_id);

  EXPECT_EQ(catalog.service_for(kMaterialMerchantNpc, kMaterialInteraction)->target_id,
            kMaterialShop);
  EXPECT_FALSE(catalog.service_for(kGeneralMerchantNpc, kMaterialInteraction));

  const auto repair = catalog.service_for(kBlacksmithNpc, kRepairInteraction);
  ASSERT_TRUE(repair);
  EXPECT_EQ(repair->kind, MerchantServiceKind::repair);

  const auto voucher = catalog.service_for(kVoucherNpc, kVoucherInteraction);
  ASSERT_TRUE(voucher);
  EXPECT_EQ(voucher->kind, MerchantServiceKind::voucher_group);
  EXPECT_EQ(voucher->target_id, 2);

  EXPECT_EQ(catalog.service_for(kPermitMerchantNpc, kPermitInteraction)->condition_id, 704);
  EXPECT_FALSE(catalog.is_merchant(kPlainNpc));
  EXPECT_EQ(catalog.maximum_stack(), 1000);
  EXPECT_EQ(catalog.shop_count(), 2U);
}

TEST(NpcShop, CatalogDerivesStorageCurrencyRepairAndWeight) {
  const auto &catalog = test_catalog();
  const auto *chair = catalog.item(kHousingChair);
  ASSERT_TRUE(chair);
  EXPECT_EQ(chair->storage, ShopItemStorage::housing);
  EXPECT_EQ(chair->unit_weight, 5);

  const auto *token_potion = catalog.item(kTokenPotion);
  ASSERT_TRUE(token_potion && token_potion->price);
  EXPECT_EQ(token_potion->price->currency, MoneyType::HersanderToken);
  EXPECT_FALSE(token_potion->refund);

  EXPECT_EQ(catalog.item(kBuildingDrawing)->storage, ShopItemStorage::stackable);
  EXPECT_EQ(catalog.item(kStoneMaterial)->storage, ShopItemStorage::stackable);

  const auto *bow = catalog.item(kLongbow);
  ASSERT_TRUE(bow);
  EXPECT_TRUE(bow->npc_repairable);
  EXPECT_EQ(bow->full_repair_cost, 500);
  EXPECT_FALSE(catalog.item(kWildAxe)->npc_repairable);
  EXPECT_EQ(catalog.weight_ceiling_percent(), 200);
  EXPECT_EQ(catalog.trade_refund_basis_points(), 1000);
  EXPECT_EQ(catalog.trade_good(100)->basic_value, 100);
  EXPECT_EQ(catalog.voucher_group(2)->size(), 2U);
}

TEST(NpcShop, CatalogRejectsMalformedRows) {
  auto bad_price = test_tables();
  bad_price.items.push_back(item_row(91000, "Consumable", "Gold", "12x", "Gold", "1"));
  EXPECT_THROW(ShopCatalog{bad_price}, std::runtime_error);

  auto bad_currency = test_tables();
  bad_currency.items.push_back(item_row(91001, "Consumable", "BlueSalt", "1", "Gold", "1"));
  EXPECT_THROW(ShopCatalog{bad_currency}, std::runtime_error);

  auto duplicate_good = test_tables();
  duplicate_good.shops.push_back(shop_row(kGeneralShop, kHealingPotion));
  EXPECT_THROW(ShopCatalog{duplicate_good}, std::runtime_error);

  auto bad_reset = test_tables();
  bad_reset.shops.push_back(shop_row(kGeneralShop, kKnife, "", "1", "Hourly"));
  EXPECT_THROW(ShopCatalog{bad_reset}, std::runtime_error);

  auto missing_stack = test_tables();
  missing_stack.game_variables.erase(missing_stack.game_variables.begin());
  EXPECT_THROW(ShopCatalog{missing_stack}, std::runtime_error);

  auto conflicting_repair = test_tables();
  conflicting_repair.item_repair_costs.push_back(
      repair_cost_row("LongBow", "Grand", "Weapon_1", "501"));
  EXPECT_THROW(ShopCatalog{conflicting_repair}, std::runtime_error);

  auto bad_voucher = test_tables();
  bad_voucher.voucher_groups.push_back({{"VoucherGroupId", "x"}, {"FK_tradeGoodId", "100"}});
  EXPECT_THROW(ShopCatalog{bad_voucher}, std::runtime_error);
}

TEST(NpcShop, ResetBoundariesFollowTheCatalogBaseTime) {
  const auto &catalog = test_catalog();
  EXPECT_EQ(catalog.next_reset_epoch_ticks(ShopResetPeriod::never, kNow), 0);
  EXPECT_EQ(catalog.next_reset_epoch_ticks(ShopResetPeriod::daily, kNow),
            epoch_ticks_at(std::chrono::year{2026} / 10 / 28, 5h));
  EXPECT_EQ(catalog.next_reset_epoch_ticks(ShopResetPeriod::weekly, kNow),
            epoch_ticks_at(std::chrono::year{2026} / 11 / 2, 5h));
  EXPECT_EQ(catalog.next_reset_epoch_ticks(ShopResetPeriod::monthly, kNow),
            epoch_ticks_at(std::chrono::year{2026} / 11 / 1, 5h));

  const auto early_morning = epoch_ticks_at(std::chrono::year{2026} / 10 / 27, 3h);
  EXPECT_EQ(catalog.next_reset_epoch_ticks(ShopResetPeriod::daily, early_morning),
            epoch_ticks_at(std::chrono::year{2026} / 10 / 27, 5h));
}

TEST(NpcShop, ParsesCapturedAndSchemaRequestShapes) {
  const auto open = parse_shop_request(shop_message(Json::object()));
  ASSERT_EQ(open.error, x5::ec::Success);
  EXPECT_EQ(open.request.action, ShopAction::open);
  EXPECT_EQ(open.request.target_unit_id, kMerchantUnit);

  const auto purchase = parsed(shop_message(buy(kHealingPotion, 1)));
  ASSERT_EQ(purchase.purchases.size(), 1U);
  EXPECT_EQ(purchase.action, ShopAction::buy);

  const auto stack_sale = parsed(shop_message(sell_stack(kLeather, 4)));
  EXPECT_EQ(stack_sale.sales.front().count, 4);

  const auto item_sale = parsed(shop_message(sell_item(102)));
  EXPECT_EQ(item_sale.sales.front().item_id, 102);

  const auto voucher_sale = parsed(shop_message(sell_voucher(77)));
  EXPECT_EQ(voucher_sale.sales.front().trade_voucher_id, 77);

  const auto repair = parsed(repair_message({102, 103}));
  EXPECT_EQ(repair.action, ShopAction::repair);
  EXPECT_EQ(repair.repair_item_ids, (std::vector<std::int64_t>{102, 103}));

  const auto voucher = parsed(voucher_message(100, 2));
  EXPECT_EQ(voucher.action, ShopAction::voucher_purchase);
  EXPECT_EQ(voucher.voucher_good_cms_id, 100);
  EXPECT_EQ(voucher.voucher_count, 2);
}

TEST(NpcShop, RejectsMalformedRequests) {
  const auto error_for = [](const Json &message) { return parse_shop_request(message).error; };

  Json mixed = buy(kHealingPotion, 1);
  mixed["sell_list"] = sell_stack(kLeather, 1).at("sell_list");
  EXPECT_EQ(error_for(shop_message(mixed)), x5::ec::Interaction_InvalidParamType);
  EXPECT_EQ(error_for(shop_message(buy(kHealingPotion, 0))), x5::ec::ItemCountError);
  EXPECT_EQ(error_for(shop_message(buy(kHealingPotion, -2))), x5::ec::ItemCountError);
  EXPECT_EQ(error_for(shop_message(buy(0, 1))), x5::ec::ItemCmsIdInvalid);
  EXPECT_EQ(error_for(shop_message(sell_stack(kLeather, 0))), x5::ec::ItemCountError);
  EXPECT_EQ(error_for(shop_message({{"sell_list", Json::array({{{"item_id", "-5"}}})}})),
            x5::ec::ItemIdInvalid);
  EXPECT_EQ(error_for(shop_message(
                {{"sell_list", Json::array({{{"item_id", "5"}, {"trade_voucher_id", "9"}}})}})),
            x5::ec::BadRequest);
  EXPECT_EQ(error_for(shop_message({{"sell_list", Json::array({Json::object()})}})),
            x5::ec::BadRequest);

  Json duplicate_lines = buy(kHealingPotion, 1);
  duplicate_lines["buy_list"].push_back({{"item_cms_id", kHealingPotion}, {"count", 2}});
  EXPECT_EQ(error_for(shop_message(duplicate_lines)), x5::ec::BadRequest);

  Json too_many = {{"buy_list", Json::array()}};
  for (std::size_t index = 0; index <= kMaximumShopRequestLines; ++index) {
    too_many["buy_list"].push_back({{"item_cms_id", 100 + index}, {"count", 1}});
  }
  EXPECT_EQ(error_for(shop_message(too_many)), x5::ec::BadRequest);

  auto bad_target = shop_message(Json::object());
  bad_target["target_unit_id"] = "not-a-unit";
  EXPECT_EQ(error_for(bad_target), x5::ec::BadRequest);
  EXPECT_FALSE(is_merchant_request({{"param", {{"interaction_id", 1}}}}));

  EXPECT_EQ(error_for(repair_message({})), x5::ec::BadRequest);
  EXPECT_EQ(error_for(repair_message({5, 5})), x5::ec::BadRequest);
  auto bad_repair = repair_message({5});
  bad_repair["param"]["repair"]["item_ids"] = Json::array({"abc"});
  EXPECT_EQ(error_for(bad_repair), x5::ec::ItemIdInvalid);
  EXPECT_EQ(error_for(voucher_message(100, 0)), x5::ec::ItemCountError);
  EXPECT_EQ(error_for(voucher_message(100, kMaximumVouchersPerRequest + 1)),
            x5::ec::ItemCountError);
  EXPECT_EQ(error_for(voucher_message(0, 1)), x5::ec::ItemCmsIdInvalid);
}

TEST(NpcShop, AuthorizesOnlyAnnouncedMerchantsOfTheMatchingServiceInRange) {
  const auto general = merchant_target(kGeneralMerchantNpc);
  const auto granted = access_for(shop_message(Json::object()), &general);
  EXPECT_EQ(granted.error, x5::ec::Success);
  EXPECT_EQ(granted.service.target_id, kGeneralShop);

  EXPECT_EQ(access_for(shop_message(Json::object()), &general, std::nullopt).error,
            x5::ec::BadRequest);
  EXPECT_EQ(access_for(shop_message(Json::object()), nullptr).error,
            x5::ec::Interaction_TargetNotFound);
  const auto other_unit = merchant_target(kGeneralMerchantNpc, kFarMerchantUnit);
  EXPECT_EQ(access_for(shop_message(Json::object()), &other_unit).error,
            x5::ec::Interaction_TargetNotFound);
  EXPECT_EQ(access_for(shop_message(Json::object(), kMaterialInteraction), &general).error,
            x5::ec::Interaction_GroupIdNotMatch);

  const auto far_merchant = merchant_target(kGeneralMerchantNpc, kMerchantUnit, 100000);
  EXPECT_EQ(access_for(shop_message(Json::object()), &far_merchant).error, x5::ec::TooFar);

  // A repair request addressed to a shop interaction, and the reverse, is a parameter mismatch.
  const auto blacksmith = merchant_target(kBlacksmithNpc);
  EXPECT_EQ(access_for(shop_message(Json::object(), kRepairInteraction), &blacksmith).error,
            x5::ec::Interaction_InvalidParamType);
  EXPECT_EQ(access_for(repair_message({5}), &blacksmith).error, x5::ec::Success);
  EXPECT_EQ(access_for(repair_message({5}, kGeneralInteraction), &general).error,
            x5::ec::Interaction_InvalidParamType);

  const auto voucher_npc = merchant_target(kVoucherNpc);
  EXPECT_EQ(access_for(voucher_message(100, 1), &voucher_npc).error, x5::ec::Success);
}

TEST(NpcShop, EvaluatesInteractionConditionsInsteadOfRejectingThem) {
  // Interaction 300 carries catalog condition 704 (PioneerPermit false OR true): met.
  const auto permit_merchant = merchant_target(kPermitMerchantNpc);
  const auto permitted =
      access_for(shop_message(Json::object(), kPermitInteraction), &permit_merchant);
  EXPECT_EQ(permitted.error, x5::ec::Success);
  EXPECT_EQ(permitted.service.target_id, kGeneralShop);

  // Interaction 301 uses a condition kind the evaluator does not know: fail closed.
  const auto broken_merchant = merchant_target(kBrokenConditionNpc);
  EXPECT_EQ(
      access_for(shop_message(Json::object(), kBrokenConditionInteraction), &broken_merchant).error,
      x5::ec::Interaction_InvalidData);
}

TEST(NpcShop, BuysStackablesAtCapturedPrices) {
  // Shop 1002 sold item 70109003 (priceValue 5) for 10 gold: a 2000 per-mille multiplier.
  MerchantService material_shop;
  material_shop.target_id = kMaterialShop;
  const auto material =
      plan_merchant_trade(trade_input(test_character(), test_progress()), material_shop,
                          parsed(shop_message(buy(kStoneMaterial, 1), kMaterialInteraction)));
  ASSERT_EQ(material.error, x5::ec::Success);
  ASSERT_EQ(material.trade.money_changes.size(), 1U);
  EXPECT_EQ(material.trade.money_changes.front().change, -10);
  EXPECT_EQ(material.trade.next.gold, 1186);
  EXPECT_EQ(material.trade.next.stackables.at(kStoneMaterial), 2);
  EXPECT_EQ(material.trade.next.revision, test_progress().revision + 1);

  const auto potions = plan(buy(kHealingPotion, 3));
  ASSERT_EQ(potions.error, x5::ec::Success);
  EXPECT_EQ(potions.trade.money_changes.front().change, -600);
  EXPECT_EQ(potions.trade.next.stackables.at(kHealingPotion), 3);
  EXPECT_EQ(potions.trade.changed_stackables, std::vector<int>{kHealingPotion});
}

TEST(NpcShop, BuysEquipmentAsNewInstancesWithCreationTime) {
  const auto purchase = plan(buy(kPickaxe, 2));
  ASSERT_EQ(purchase.error, x5::ec::Success);
  EXPECT_EQ(purchase.trade.money_changes.front().change, -600);
  ASSERT_EQ(purchase.trade.granted_items.size(), 2U);

  // Slots 0 to 4 are occupied by the test character's items.
  const auto &first = purchase.trade.granted_items.at(0);
  EXPECT_EQ(first.id, 0);
  EXPECT_EQ(first.cms_id, kPickaxe);
  EXPECT_EQ(first.inventory_slot, 5);
  EXPECT_FALSE(first.equip_slot);
  EXPECT_EQ(first.durability, 2000);
  EXPECT_EQ(first.grade, static_cast<int>(x5::cms::EX5ItemGrade_Normal));
  EXPECT_EQ(first.created_epoch_ticks, kNow);
  EXPECT_EQ(purchase.trade.granted_items.at(1).inventory_slot, 6);
}

TEST(NpcShop, PaysInHersanderTokensWithItsOwnBalanceAndCap) {
  const auto potion = plan(buy(kTokenPotion, 2));
  ASSERT_EQ(potion.error, x5::ec::Success);
  ASSERT_EQ(potion.trade.money_changes.size(), 1U);
  EXPECT_EQ(potion.trade.money_changes.front().type, MoneyType::HersanderToken);
  EXPECT_EQ(potion.trade.money_changes.front().change, -10);
  EXPECT_EQ(potion.trade.next.hersander_token, 30);
  EXPECT_EQ(potion.trade.next.gold, test_progress().gold);

  // One request can charge both currencies; each balance is checked on its own.
  Json both = {{"buy_list", Json::array({{{"item_cms_id", kTokenPotion}, {"count", 1}},
                                         {{"item_cms_id", kHealingPotion}, {"count", 1}}})}};
  const auto mixed = plan(both);
  ASSERT_EQ(mixed.error, x5::ec::Success);
  EXPECT_EQ(mixed.trade.money_changes.size(), 2U);
  EXPECT_EQ(mixed.trade.next.gold, 996);
  EXPECT_EQ(mixed.trade.next.hersander_token, 35);

  auto poor = test_progress();
  poor.hersander_token = 4;
  EXPECT_EQ(plan(buy(kTokenPotion, 1), test_character(), poor).error, x5::ec::MoneyNotEnough);
  // Plenty of gold does not cover a token price.
  poor.gold = 9'000'000'000'000;
  EXPECT_EQ(plan(buy(kTokenPotion, 1), test_character(), poor).error, x5::ec::MoneyNotEnough);

  // A token balance at Money.limitUserMax does not block an unrelated Gold credit.
  auto at_cap = test_progress();
  at_cap.hersander_token = kMoneyLimit;
  EXPECT_EQ(plan(sell_stack(kLeather, 4), test_character(), at_cap).error, x5::ec::Success);
}

TEST(NpcShop, BuysAndSellsHousingObjectsAsHousingItems) {
  const auto purchase = plan(buy(kHousingChair, 3));
  ASSERT_EQ(purchase.error, x5::ec::Success);
  EXPECT_EQ(purchase.trade.next.housing_items.at(kHousingChair).quantity, 3);
  EXPECT_EQ(purchase.trade.next.housing_items.at(kHousingChair).installed_count, 0);
  EXPECT_EQ(purchase.trade.changed_housing_items, std::vector<int>{kHousingChair});
  EXPECT_TRUE(purchase.trade.next.stackables.find(kHousingChair) ==
              purchase.trade.next.stackables.end());

  auto owner = test_progress();
  owner.housing_items[kHousingChair] = {5, 3};
  const auto sale = plan(sell_stack(kHousingChair, 2), test_character(), owner);
  ASSERT_EQ(sale.error, x5::ec::Success);
  EXPECT_EQ(sale.trade.money_changes.front().change, 40);
  EXPECT_EQ(sale.trade.next.housing_items.at(kHousingChair).quantity, 3);
  // Installed objects cannot be sold: only 2 of the 5 chairs are free.
  EXPECT_EQ(plan(sell_stack(kHousingChair, 3), test_character(), owner).error,
            x5::ec::ItemCountError);

  auto saturated = test_progress();
  saturated.housing_items[kHousingChair] = {std::numeric_limits<int>::max(), 0};
  EXPECT_EQ(plan(buy(kHousingChair, 1), test_character(), saturated).error, x5::ec::ItemCountError);

  auto many_kinds = test_progress();
  for (int index = 0; many_kinds.housing_items.size() < kMaximumHousingItemKinds; ++index) {
    many_kinds.housing_items[1'000'000 + index] = {1, 0};
  }
  EXPECT_EQ(plan(buy(kHousingChair, 1), test_character(), many_kinds).error, x5::ec::ItemSlotLimit);
}

// Candidate (docs/housing.md): buildings share housing storage with objects, and the housing
// rent item is counted as a stack like other consumables.
TEST(NpcShop, BuildingsGoToHousingStorageAndPermitsStack) {
  auto rich = test_progress();
  rich.gold = 100'000;

  const auto building = plan(buy(kHousingBuilding, 1), test_character(), rich);
  ASSERT_EQ(building.error, x5::ec::Success);
  EXPECT_EQ(building.trade.next.housing_items.at(kHousingBuilding).quantity, 1);
  EXPECT_EQ(building.trade.changed_housing_items, std::vector<int>{kHousingBuilding});

  const auto permits = plan(buy(kHousingPermit, 4), test_character(), rich);
  ASSERT_EQ(permits.error, x5::ec::Success);
  EXPECT_EQ(permits.trade.next.stackables.at(kHousingPermit), 4);
  EXPECT_FALSE(permits.trade.next.housing_items.contains(kHousingPermit));
}

TEST(NpcShop, HousingBlueprintPurchasesUseStacksRatherThanInstalledHousingStorage) {
  auto progress = test_progress();
  progress.gold = 10000;

  const auto purchased = plan(buy(kBuildingDrawing, 2), test_character(), progress);
  ASSERT_EQ(purchased.error, x5::ec::Success);
  EXPECT_EQ(purchased.trade.next.stackables.at(kBuildingDrawing), 2);
  EXPECT_FALSE(purchased.trade.next.housing_items.contains(kBuildingDrawing));
  EXPECT_TRUE(purchased.trade.granted_items.empty());
  EXPECT_EQ(purchased.trade.next.gold, 4000);
  EXPECT_EQ(purchased.trade.changed_stackables, std::vector<int>{kBuildingDrawing});
}

TEST(NpcShop, EvaluatesShopItemConditionsInsteadOfRejectingThem) {
  EXPECT_EQ(plan(buy(kConditionalPotion, 1)).error, x5::ec::Interaction_ConditionsNotMatch);

  // A character with the required company standing passes the same condition.
  MerchantService service;
  service.target_id = kGeneralShop;
  ConditionContext veteran;
  veteran.company_levels["EX5CompanyType_BlueSalt"] = 3;
  const MerchantTradeInput input{test_catalog(), test_character(), test_progress(),
                                 kNow,           veteran,          std::nullopt};
  const auto allowed =
      plan_merchant_trade(input, service, parsed(shop_message(buy(kConditionalPotion, 1))));
  EXPECT_EQ(allowed.error, x5::ec::Success);
}

TEST(NpcShop, RejectsWhenCarriedWeightWouldPassTheCeiling) {
  // Ceiling 1000: a potion weighs 400, so two fit on top of 200 and three do not.
  const WeightBudget budget{200, 1000};
  EXPECT_EQ(plan(buy(kHealingPotion, 2), test_character(), test_progress(), budget).error,
            x5::ec::Success);
  EXPECT_EQ(plan(buy(kHealingPotion, 3), test_character(), test_progress(), budget).error,
            x5::ec::ItemWeightLimit);
  // The crate weighs exactly the remaining headroom, then one unit more.
  EXPECT_EQ(
      plan(buy(kHeavyCrate, 1), test_character(), test_progress(), WeightBudget{0, 1000}).error,
      x5::ec::Success);
  EXPECT_EQ(
      plan(buy(kHeavyCrate, 1), test_character(), test_progress(), WeightBudget{1, 1000}).error,
      x5::ec::ItemWeightLimit);
  // Without a budget (unknown multiplier or weights) purchases are not weight checked.
  EXPECT_EQ(plan(buy(kHeavyCrate, 5)).error, x5::ec::Success);
}

TEST(NpcShop, RejectsInvalidPurchases) {
  const auto error_for = [](const Json &shop, const WorldProgress &progress = test_progress()) {
    return plan(shop, test_character(), progress).error;
  };

  EXPECT_EQ(error_for(buy(kLeather, 1)), x5::ec::ItemCmsIdInvalid);
  EXPECT_EQ(error_for(buy(kHealingPotion, 1001)), x5::ec::ItemCountError);
  EXPECT_EQ(error_for(buy(kBuildingDrawing, 1)), x5::ec::MoneyNotEnough);
  EXPECT_EQ(error_for(buy(kOddPricedPotion, 1)), x5::ec::Interaction_InvalidData);
  EXPECT_EQ(error_for(buy(kPickaxe, kMaximumPurchasedInstances + 1)), x5::ec::ItemCountError);

  auto poor = test_progress();
  poor.gold = 299;
  EXPECT_EQ(error_for(buy(kPickaxe, 1), poor), x5::ec::MoneyNotEnough);

  auto overflowing_stack = test_progress();
  overflowing_stack.stackables[kHealingPotion] = std::numeric_limits<int>::max();
  EXPECT_EQ(error_for(buy(kHealingPotion, 1), overflowing_stack), x5::ec::ItemCountError);

  auto many_kinds = test_progress();
  for (int index = 0; many_kinds.stackables.size() < kMaximumStackableKinds; ++index) {
    many_kinds.stackables[500000 + index] = 1;
  }
  EXPECT_EQ(error_for(buy(kHealingPotion, 1), many_kinds), x5::ec::ItemSlotLimit);

  auto full_inventory = test_character();
  for (int slot = 5; slot <= kMaximumInventorySlot; ++slot) {
    full_inventory.items.push_back(CharacterItem{1000 + slot, kKnife, slot});
  }
  EXPECT_EQ(plan(buy(kPickaxe, 1), full_inventory).error, x5::ec::ItemSlotLimit);

  auto exhausted_revision = test_progress();
  exhausted_revision.revision = std::numeric_limits<std::int64_t>::max();
  EXPECT_EQ(error_for(buy(kHealingPotion, 1), exhausted_revision), x5::ec::BadRequest);

  auto dead = test_progress();
  dead.dead = true;
  EXPECT_EQ(error_for(buy(kHealingPotion, 1), dead), x5::ec::Interaction_Invalid);
  EXPECT_EQ(plan(Json::object()).error, x5::ec::BadRequest);
}

TEST(NpcShop, PurchaseLimitsCountWithinTheResetWindow) {
  const auto first = plan(buy(kDailyLimitedPotion, 2));
  ASSERT_EQ(first.error, x5::ec::Success);
  const ShopLimitKey key{kGeneralShop, kDailyLimitedPotion};
  const auto &record = first.trade.next.shop_purchases.at(key);
  EXPECT_EQ(record.purchased_count, 2);
  EXPECT_EQ(record.reset_epoch_ticks, epoch_ticks_at(std::chrono::year{2026} / 10 / 28, 5h));

  const auto exceeded = plan(buy(kDailyLimitedPotion, 1), test_character(), first.trade.next);
  EXPECT_EQ(exceeded.error, x5::ec::PurchaseLimitExceeded);
  EXPECT_EQ(plan(buy(kDailyLimitedPotion, 3)).error, x5::ec::PurchaseLimitExceeded);

  const auto next_day = epoch_ticks_at(std::chrono::year{2026} / 10 / 28, 6h);
  const auto renewed =
      plan(buy(kDailyLimitedPotion, 1), test_character(), first.trade.next, std::nullopt, next_day);
  ASSERT_EQ(renewed.error, x5::ec::Success);
  const auto &renewed_record = renewed.trade.next.shop_purchases.at(key);
  EXPECT_EQ(renewed_record.purchased_count, 1);
  EXPECT_EQ(renewed_record.reset_epoch_ticks,
            epoch_ticks_at(std::chrono::year{2026} / 10 / 29, 5h));
}

TEST(NpcShop, SellsStacksAndInstancesForCatalogRefunds) {
  // Captured sales credited 4 x 72000016 for 4 gold and one equipment item at its refund price.
  const auto stack_sale = plan(sell_stack(kLeather, 4));
  ASSERT_EQ(stack_sale.error, x5::ec::Success);
  EXPECT_EQ(stack_sale.trade.money_changes.front().change, 4);
  EXPECT_EQ(stack_sale.trade.next.gold, 1200);
  EXPECT_EQ(stack_sale.trade.next.stackables.at(kLeather), 0);

  const auto item_sale = plan(sell_item(102));
  ASSERT_EQ(item_sale.error, x5::ec::Success);
  EXPECT_EQ(item_sale.trade.money_changes.front().change, 100);
  ASSERT_EQ(item_sale.trade.removed_items.size(), 1U);
  EXPECT_EQ(item_sale.trade.removed_items.front().id, 102);
}

TEST(NpcShop, RejectsInvalidSales) {
  const auto error_for = [](const Json &shop, const WorldProgress &progress = test_progress()) {
    return plan(shop, test_character(), progress).error;
  };

  EXPECT_EQ(error_for(sell_item(999)), x5::ec::ItemIdInvalid);
  EXPECT_EQ(error_for(sell_item(101)), x5::ec::ItemEquipped);
  EXPECT_EQ(error_for(sell_stack(kLeather, 5)), x5::ec::ItemCountError);
  EXPECT_EQ(error_for(sell_stack(kHealingPotion, 1)), x5::ec::ItemCountError);

  auto quest_items = test_progress();
  quest_items.stackables[kQuestToken] = 1;
  quest_items.stackables[kTokenPotion] = 1;
  EXPECT_EQ(error_for(sell_stack(kQuestToken, 1), quest_items), x5::ec::ItemNonSellable);
  // The token potion has no refund price, so it cannot be sold either.
  EXPECT_EQ(error_for(sell_stack(kTokenPotion, 1), quest_items), x5::ec::ItemNonSellable);

  auto unknown_items = test_progress();
  unknown_items.stackables[123456] = 1;
  EXPECT_EQ(error_for(sell_stack(123456, 1), unknown_items), x5::ec::ItemCmsIdInvalid);

  auto quick_slotted = test_progress();
  quick_slotted.utility_slots[0] = {kLeather, 3};
  EXPECT_EQ(error_for(sell_stack(kLeather, 2), quick_slotted), x5::ec::ItemLocked);
  EXPECT_EQ(error_for(sell_stack(kLeather, 1), quick_slotted), x5::ec::Success);

  // CUSTOM: proceeds above the wallet limit fill it to the limit instead of refusing the sale.
  auto wealthy = test_progress();
  wealthy.gold = kMoneyLimit - 1;
  const auto wealthy_sale = plan(sell_stack(kLeather, 4), test_character(), wealthy);
  ASSERT_EQ(wealthy_sale.error, x5::ec::Success);
  EXPECT_EQ(wealthy_sale.trade.next.gold, kMoneyLimit);
  EXPECT_EQ(wealthy_sale.trade.money_changes.front().change, 1);

  auto full = test_progress();
  full.gold = kMoneyLimit;
  const auto full_sale = plan(sell_stack(kLeather, 4), test_character(), full);
  ASSERT_EQ(full_sale.error, x5::ec::Success);
  EXPECT_EQ(full_sale.trade.next.gold, kMoneyLimit);
  EXPECT_TRUE(full_sale.trade.money_changes.empty());
  EXPECT_EQ(full_sale.trade.next.stackables.at(kLeather), 0);
}

TEST(NpcShop, RepairsWearAtThePerItemCeilingOfTheCatalogRate) {
  // Captured: a Grand longbow at durability 9908 cost 5 gold (500 x 92 / 10000 = 4.6, rounded
  // up). The helmet at 9088 costs ceil(75 x 912 / 10000) = 7.
  const auto longbow = plan_repair({104});
  ASSERT_EQ(longbow.error, x5::ec::Success);
  EXPECT_EQ(longbow.trade.money_changes.front().change, -5);
  ASSERT_EQ(longbow.trade.durability_updates.size(), 1U);
  EXPECT_EQ(longbow.trade.durability_updates.front().expected_durability, 9908);
  EXPECT_EQ(longbow.trade.durability_updates.front().durability, 10000);
  EXPECT_EQ(longbow.trade.repaired_items.front().durability, 10000);
  EXPECT_EQ(longbow.trade.next.gold, 1191);
  EXPECT_EQ(longbow.trade.next.revision, test_progress().revision + 1);

  // Repair-all lists every item; undamaged items are skipped, and the totals add the per-item
  // results (7 + 5 = 12).
  const auto all = plan_repair({103, 104, 105});
  ASSERT_EQ(all.error, x5::ec::Success);
  EXPECT_EQ(all.trade.money_changes.front().change, -12);
  EXPECT_EQ(all.trade.durability_updates.size(), 2U);

  // The tool uses the same proportion of its own maximum: 10 x 500 / 2000 = 2.5, rounded up.
  auto worn_tool = test_character();
  worn_tool.items.at(4).durability = 1500;
  const auto tool = plan_repair({105}, worn_tool);
  ASSERT_EQ(tool.error, x5::ec::Success);
  EXPECT_EQ(tool.trade.money_changes.front().change, -3);
}

TEST(NpcShop, ReproducesTheCapturedNineItemRepairTotal) {
  // Sanitized durability and cost-rate pairs of the captured repair-all request whose server
  // reply charged 207 gold. Summing per-item ceilings is the only rounding that matches:
  // per-item floors give 198 and rounding the sum gives 204.
  const std::vector<std::pair<int, int>> worn = {{9088, 75},  {9080, 150}, {9088, 75},
                                                 {9092, 75},  {9092, 150}, {9096, 75},
                                                 {9076, 500}, {9088, 150}, {8216, 500}};
  std::int64_t total = 0;
  for (const auto &[durability, rate] : worn) {
    total += (static_cast<std::int64_t>(rate) * (10000 - durability) + 9999) / 10000;
  }
  EXPECT_EQ(total, 207);
}

TEST(NpcShop, RejectsInvalidRepairs) {
  EXPECT_EQ(plan_repair({999}).error, x5::ec::ItemIdInvalid);
  // The Wild axe has no repair cost row, so the NPC cannot repair it.
  EXPECT_EQ(plan_repair({101}).error, x5::ec::ItemNotNpcRepairable);
  // The pickaxe is repairable but undamaged: nothing to do.
  EXPECT_EQ(plan_repair({105}).error, x5::ec::ItemNotRepairable);

  auto poor = test_progress();
  poor.gold = 4;
  EXPECT_EQ(plan_repair({103, 104}, test_character(), poor).error, x5::ec::MoneyNotEnough);

  auto dead = test_progress();
  dead.dead = true;
  EXPECT_EQ(plan_repair({104}, test_character(), dead).error, x5::ec::Interaction_Invalid);
}

TEST(NpcShop, IssuesTradeVouchersForTheCatalogPrice) {
  const auto purchase = plan_voucher(kGoodWithoutCondition, 2);
  ASSERT_EQ(purchase.error, x5::ec::Success);
  EXPECT_EQ(purchase.trade.money_changes.front().change, -200);
  ASSERT_EQ(purchase.trade.issued_voucher_ids.size(), 2U);

  const auto first_id = purchase.trade.issued_voucher_ids.at(0);
  EXPECT_EQ(first_id, (static_cast<std::int64_t>(kCharacterId) << 20) + 1);
  const auto &voucher = purchase.trade.next.trade_vouchers.at(first_id);
  EXPECT_EQ(voucher.good_cms_id, kGoodWithoutCondition);
  EXPECT_EQ(voucher.created_epoch_ticks, kNow);
  EXPECT_EQ(voucher.expiration_epoch_ticks, kNow + 100 * kTicksPerHour);
  EXPECT_FALSE(voucher.redeemed);

  // Ids continue after existing vouchers (redeemed ones included), so they never repeat.
  const auto second = plan_voucher(kGoodWithoutCondition, 1, purchase.trade.next);
  ASSERT_EQ(second.error, x5::ec::Success);
  EXPECT_EQ(second.trade.issued_voucher_ids.front(), first_id + 2);
}

TEST(NpcShop, RejectsInvalidVoucherPurchases) {
  EXPECT_EQ(plan_voucher(999, 1).error, x5::ec::ItemCmsIdInvalid);
  // Voucher condition 1004 needs BlueSalt level 4, which every character lacks today.
  EXPECT_EQ(plan_voucher(kGoodWithCondition, 1).error, x5::ec::Interaction_ConditionsNotMatch);

  auto poor = test_progress();
  poor.gold = 99;
  EXPECT_EQ(plan_voucher(kGoodWithoutCondition, 1, poor).error, x5::ec::MoneyNotEnough);

  auto crowded = test_progress();
  for (std::int64_t id = 1; crowded.trade_vouchers.size() < kMaximumTradeVouchers; ++id) {
    crowded.trade_vouchers[id] = {100, 1, 2, true};
  }
  EXPECT_EQ(plan_voucher(kGoodWithoutCondition, 1, crowded).error, x5::ec::ItemSlotLimit);
}

TEST(NpcShop, SellsVouchersForTheTradeRefundRate) {
  auto holder = test_progress();
  holder.trade_vouchers[11] = {kGoodWithoutCondition, kNow - 1, kNow + kTicksPerHour, false};
  holder.trade_vouchers[12] = {kGoodWithoutCondition, kNow - 100, kNow - 1, false};
  holder.trade_vouchers[13] = {kGoodWithoutCondition, kNow - 100, kNow + 5, true};

  // 100 x 1000 / 10000 = 10 gold.
  const auto sale = plan(sell_voucher(11), test_character(), holder);
  ASSERT_EQ(sale.error, x5::ec::Success);
  EXPECT_EQ(sale.trade.money_changes.front().change, 10);
  EXPECT_TRUE(sale.trade.next.trade_vouchers.at(11).redeemed);
  EXPECT_EQ(sale.trade.redeemed_voucher_ids, std::vector<std::int64_t>{11});

  EXPECT_EQ(plan(sell_voucher(12), test_character(), holder).error, x5::ec::ItemHasExpired);
  EXPECT_EQ(plan(sell_voucher(13), test_character(), holder).error, x5::ec::ItemIdInvalid);
  EXPECT_EQ(plan(sell_voucher(99), test_character(), holder).error, x5::ec::ItemIdInvalid);
}

TEST(NpcShop, ResponsePacketsMatchCapturedOrderAndSchemas) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  const auto encodes = [&protocol](const GameplayPacket &packet) {
    const auto frame = protocol.encode(packet.opcode, packet.message);
    return protocol.decode(frame).at("message");
  };

  auto progress = test_progress();
  progress.shop_purchases[{kGeneralShop, kDailyLimitedPotion}] = {1, kNow + 1};
  progress.shop_purchases[{kGeneralShop, kHealingPotion}] = {9, 0};
  const auto request = parsed(shop_message(Json::object()));
  const auto open = shop_open_packets(test_catalog(), request, kGeneralShop, progress, kNow);
  ASSERT_EQ(open.size(), 2U);
  EXPECT_EQ(open.at(0).opcode, kInteractUnitResponseOpcode);
  EXPECT_EQ(open.at(1).opcode, kShopItemLimitNotificationOpcode);
  const auto limits = encodes(open.at(1));
  EXPECT_EQ(limits.at("shop_id"), kGeneralShop);
  // Only limited goods with purchases in the current window are reported.
  ASSERT_EQ(limits.at("limits").size(), 1U);
  EXPECT_EQ(limits.at("limits").at(0).at("item_cms_id"), kDailyLimitedPotion);
  EXPECT_EQ(limits.at("limits").at(0).at("count"), 1);
  EXPECT_EQ(limits.at("limits").at(0).at("reset_epochtime"), std::to_string(kNow + 1));
  const auto opened = encodes(open.at(0));
  EXPECT_EQ(opened.at("target_unit_id"), std::to_string(kMerchantUnit));
  EXPECT_EQ(opened.at("interaction_id"), kGeneralInteraction);
  EXPECT_FALSE(opened.contains("ec"));

  const auto sale = plan(sell_item(102));
  ASSERT_EQ(sale.error, x5::ec::Success);
  const auto sold = merchant_trade_packets(parsed(shop_message(sell_item(102))), sale.trade, {});
  ASSERT_EQ(sold.size(), 2U);
  EXPECT_EQ(sold.at(0).opcode, kItemsUpdateNotificationOpcode);
  EXPECT_EQ(sold.at(1).opcode, kInteractUnitResponseOpcode);
  const auto update = encodes(sold.at(0));
  EXPECT_TRUE(update.contains("shop"));
  EXPECT_EQ(update.at("money_changes").at(0).at("change"), "100");
  EXPECT_EQ(update.at("money_changes").at(0).at("result"), "1296");
  EXPECT_FALSE(update.at("money_changes").at(0).contains("money_type"));
  EXPECT_EQ(update.at("items").at(0).at("item_id"), "102");
  EXPECT_FALSE(update.at("items").at(0).contains("durability"));

  const auto purchase = plan(buy(kPickaxe, 1));
  ASSERT_EQ(purchase.error, x5::ec::Success);
  auto committed = purchase.trade.granted_items;
  committed.front().id = 555;
  const auto bought =
      merchant_trade_packets(parsed(shop_message(buy(kPickaxe, 1))), purchase.trade, committed);
  const auto bought_update = encodes(bought.at(0));
  EXPECT_EQ(bought_update.at("items").at(0).at("item_id"), "555");
  EXPECT_EQ(bought_update.at("items").at(0).at("durability"), 2000);
  EXPECT_EQ(bought_update.at("items").at(0).at("created_epochtime"), std::to_string(kNow));
  EXPECT_EQ(bought_update.at("money_changes").at(0).at("change"), "-300");

  const auto rejected = shop_error_packet(9, x5::ec::MoneyNotEnough);
  EXPECT_EQ(rejected.opcode, kCommonErrorResponseOpcode);
  const auto rejection = encodes(rejected);
  EXPECT_EQ(rejection.at("ec"), "MoneyNotEnough");
  EXPECT_EQ(rejection.at("packet_seq").at("seq"), "9");
}

TEST(NpcShop, ExtendedResponsePacketsEncodeAgainstTheRecoveredSchema) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  const auto encodes = [&protocol](const GameplayPacket &packet) {
    const auto frame = protocol.encode(packet.opcode, packet.message);
    return protocol.decode(frame).at("message");
  };

  const auto token_purchase = plan(buy(kTokenChair, 1));
  ASSERT_EQ(token_purchase.error, x5::ec::Success);
  const auto token_packets =
      merchant_trade_packets(parsed(shop_message(buy(kTokenChair, 1))), token_purchase.trade, {});
  const auto token_update = encodes(token_packets.at(0));
  EXPECT_EQ(token_update.at("money_changes").at(0).at("money_type"), "EX5MoneyType_HersanderToken");
  EXPECT_EQ(token_update.at("money_changes").at(0).at("change"), "-10");
  EXPECT_EQ(token_update.at("money_changes").at(0).at("result"), "30");
  EXPECT_EQ(token_update.at("housing_items").at(0).at("item_cms_id"), kTokenChair);
  EXPECT_EQ(token_update.at("housing_items").at(0).at("quantity"), 1);
  EXPECT_TRUE(token_update.contains("shop"));

  const auto repair = plan_repair({103, 104});
  ASSERT_EQ(repair.error, x5::ec::Success);
  const auto repair_packets =
      merchant_trade_packets(parsed(repair_message({103, 104})), repair.trade, {});
  const auto repair_update = encodes(repair_packets.at(0));
  ASSERT_EQ(repair_update.at("items").size(), 2U);
  EXPECT_EQ(repair_update.at("items").at(0).at("durability"), 10000);
  EXPECT_EQ(repair_update.at("items").at(0).at("equip_slot"), "EX5EquipSlotType_ArmorHelmet");
  EXPECT_EQ(repair_update.at("money_changes").at(0).at("change"), "-12");
  EXPECT_TRUE(repair_update.contains("shop"));

  const auto vouchers = plan_voucher(kGoodWithoutCondition, 1);
  ASSERT_EQ(vouchers.error, x5::ec::Success);
  const auto voucher_packets =
      merchant_trade_packets(parsed(voucher_message(kGoodWithoutCondition, 1)), vouchers.trade, {});
  const auto voucher_update = encodes(voucher_packets.at(0));
  EXPECT_FALSE(voucher_update.contains("shop"));
  const auto &issued = voucher_update.at("trade_voucher").at("vouchers").at(0);
  EXPECT_EQ(issued.at("good_cms_id"), kGoodWithoutCondition);
  EXPECT_EQ(issued.at("created_epochtime"), std::to_string(kNow));

  // A sold voucher is removed from the client's list: shop update, discard update with the id
  // only (no good_cms_id), then the interaction result.
  auto holder = test_progress();
  holder.trade_vouchers[11] = {kGoodWithoutCondition, kNow - 1, kNow + kTicksPerHour, false};
  const auto sale = plan(sell_voucher(11), test_character(), holder);
  ASSERT_EQ(sale.error, x5::ec::Success);
  const auto sale_packets =
      merchant_trade_packets(parsed(shop_message(sell_voucher(11))), sale.trade, {});
  ASSERT_EQ(sale_packets.size(), 3U);
  EXPECT_TRUE(encodes(sale_packets.at(0)).contains("shop"));
  const auto removal = encodes(sale_packets.at(1));
  EXPECT_EQ(removal.at("discard").at("vouchers").at(0).at("voucher_id"), "11");
  EXPECT_FALSE(removal.at("discard").at("vouchers").at(0).contains("good_cms_id"));
  EXPECT_EQ(sale_packets.at(2).opcode, kInteractUnitResponseOpcode);
}

TEST(NpcShopStorage, MemoryTradeCommitsAtomicallyUnderRevision) {
  MemoryRepositories store;
  Character character;
  character.account_id = 1;
  character.name = "ShopStorage";
  character = store.characters->create(character);
  const auto sellable = store.character_state->grant_item(
      1, character.id, 1, CharacterItem{0, kKnife, 40, std::nullopt, 1, 2000, 0});
  const auto worn = store.character_state->grant_item(
      1, character.id, 1, CharacterItem{0, kHelmet, 42, std::nullopt, 1, 9000, 0});
  ASSERT_TRUE(sellable && worn);
  const auto equipped = std::ranges::find_if(
      character.items, [](const CharacterItem &item) { return item.equip_slot.has_value(); });
  ASSERT_NE(equipped, character.items.end());

  const auto before = store.character_state->load_world_progress(1, character.id, 1);
  auto after = before;
  after.revision = before.revision + 1;
  after.gold = 80;
  after.shop_purchases[{kGeneralShop, kDailyLimitedPotion}] = {1, 0};
  after.housing_items[kHousingChair] = {2, 0};
  after.trade_vouchers[5] = {100, 10, 20, false};

  ItemTrade trade;
  CharacterItem purchased{0, kPickaxe, 41, std::nullopt, 1, 2000};
  purchased.created_epoch_ticks = kNow;
  trade.granted_items = {purchased};
  trade.removed_item_ids = {sellable->id};
  trade.durability_updates = {{worn->id, 9000, 10000}};

  auto equipped_removal = trade;
  equipped_removal.removed_item_ids = {sellable->id, equipped->id};
  EXPECT_FALSE(store.character_state->commit_item_trade(1, character.id, 1, before, after,
                                                        equipped_removal));
  auto colliding = trade;
  colliding.granted_items.front().inventory_slot = 40;
  colliding.removed_item_ids.clear();
  EXPECT_FALSE(
      store.character_state->commit_item_trade(1, character.id, 1, before, after, colliding));
  auto stale_wear = trade;
  stale_wear.durability_updates = {{worn->id, 8999, 10000}};
  EXPECT_FALSE(
      store.character_state->commit_item_trade(1, character.id, 1, before, after, stale_wear));
  EXPECT_FALSE(store.character_state->commit_item_trade(2, character.id, 1, before, after, trade));
  EXPECT_EQ(store.character_state->load_world_progress(1, character.id, 1), before);

  const auto committed =
      store.character_state->commit_item_trade(1, character.id, 1, before, after, trade);
  ASSERT_TRUE(committed);
  ASSERT_EQ(committed->size(), 1U);
  EXPECT_GT(committed->front().id, 0);
  EXPECT_EQ(store.character_state->load_world_progress(1, character.id, 1), after);
  const auto items = store.characters->characters(1, 1).front().items;
  EXPECT_FALSE(std::ranges::any_of(
      items, [&](const CharacterItem &item) { return item.id == sellable->id; }));
  EXPECT_TRUE(std::ranges::any_of(items, [&](const CharacterItem &item) {
    return item.id == committed->front().id && item.inventory_slot == 41 &&
           item.created_epoch_ticks == kNow;
  }));
  EXPECT_TRUE(std::ranges::any_of(items, [&](const CharacterItem &item) {
    return item.id == worn->id && item.durability == 10000;
  }));

  // A retried request carries the old revision and cannot apply twice.
  EXPECT_FALSE(
      store.character_state->commit_item_trade(1, character.id, 1, before, after, ItemTrade{}));

  // Redeemed vouchers cannot become valid again, and durability never becomes negative.
  auto reuse = after;
  reuse.revision = after.revision + 1;
  reuse.trade_vouchers[5].redeemed = true;
  ASSERT_TRUE(store.character_state->save_world_progress(1, character.id, 1, after, reuse));
  auto revive = reuse;
  revive.revision = reuse.revision + 1;
  revive.trade_vouchers[5].redeemed = false;
  EXPECT_THROW(store.character_state->save_world_progress(1, character.id, 1, reuse, revive),
               std::invalid_argument);

  ItemTrade equipped_grant;
  equipped_grant.granted_items = {CharacterItem{0, kPickaxe, 43, 3, 1, 2000}};
  auto next = reuse;
  next.revision = reuse.revision + 1;
  EXPECT_THROW(
      store.character_state->commit_item_trade(1, character.id, 1, reuse, next, equipped_grant),
      std::invalid_argument);
  ItemTrade wearing;
  wearing.durability_updates = {{worn->id, 10000, 9000}};
  ItemTrade negative_wear;
  negative_wear.durability_updates = {{worn->id, 10000, -1}};
  EXPECT_THROW(
      store.character_state->commit_item_trade(1, character.id, 1, reuse, next, negative_wear),
      std::invalid_argument);
  // Tool interactions wear the tool through the same transaction (captured knife 1500 -> 1400).
  ASSERT_TRUE(store.character_state->commit_item_trade(1, character.id, 1, reuse, next, wearing));
  const auto worn_items = store.characters->characters(1, 1).front().items;
  EXPECT_TRUE(std::ranges::any_of(worn_items, [&](const CharacterItem &item) {
    return item.id == worn->id && item.durability == 9000;
  }));
}

// Loads the hash-verified client export and replays the sanitized captured facts: interaction
// 116 opened shop 111103, interaction 212 sold 15010403/11200201/133 for 200/400/500 gold,
// interaction 213 sold 70109003 for 10, sales credited 4 x 72000016 for 4, one 70109003 for 2
// and one 11200301 for 80, and a repair at durability 9908 of a Grand longbow cost 5.
TEST(NpcShop, ExportedCatalogReproducesCapturedPrices) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the exported data/generated/catalog";
  }

  ClientCatalog client_catalog(catalog_path);
  const auto catalog = ShopCatalog::load(client_catalog);
  EXPECT_EQ(catalog->service_for(1001203, 116)->target_id, 111103);
  EXPECT_EQ(catalog->service_for(1001007, 212)->target_id, 1001);
  EXPECT_EQ(catalog->service_for(1001001, 213)->target_id, 1002);
  EXPECT_EQ(catalog->service_for(1001096, 65)->kind, MerchantServiceKind::repair);
  EXPECT_EQ(catalog->service_for(1001425, 32)->kind, MerchantServiceKind::voucher_group);
  EXPECT_EQ(catalog->service_for(1001425, 32)->target_id, 100);
  EXPECT_EQ(catalog->service_for(1001639, 223)->condition_id, 704);
  // The pioneer-permit condition on seven shop interactions is satisfiable by anyone; the
  // BlueSalt company condition on goods is not, because no character has company standing.
  EXPECT_EQ(catalog->evaluate_interaction_condition(704, ConditionContext{}), ConditionResult::met);
  EXPECT_EQ(catalog->evaluate_good_condition(13, ConditionContext{}), ConditionResult::unmet);

  const auto purchase_cost = [&](int shop_id, int item_cms_id) -> std::int64_t {
    const auto *good = catalog->good(shop_id, item_cms_id);
    const auto *item = catalog->item(item_cms_id);
    if (!good || !item || !item->price) {
      ADD_FAILURE() << "Missing shop good " << shop_id << "/" << item_cms_id;
      return -1;
    }
    return item->price->amount * good->price_multiplier_per_mille / 1000;
  };
  EXPECT_EQ(purchase_cost(1001, 15010403), 200);
  EXPECT_EQ(purchase_cost(1001, 11200201), 400);
  EXPECT_EQ(purchase_cost(1001, 133), 500);
  EXPECT_EQ(purchase_cost(1002, 70109003), 10);
  EXPECT_EQ(catalog->item(11200201)->max_durability, 2000);
  EXPECT_EQ(catalog->item(11200201)->storage, ShopItemStorage::instance);
  EXPECT_EQ(catalog->item(70109003)->storage, ShopItemStorage::stackable);
  EXPECT_EQ(catalog->item(1900000001)->storage, ShopItemStorage::housing);
  EXPECT_EQ(catalog->item(1900000001)->price->currency, MoneyType::HersanderToken);

  EXPECT_EQ(catalog->item(72000016)->refund->amount, 1);
  EXPECT_EQ(catalog->item(70109003)->refund->amount, 2);
  EXPECT_EQ(catalog->item(11200301)->refund->amount, 80);

  EXPECT_EQ(catalog->item(13102102)->full_repair_cost, 500);
  EXPECT_EQ(catalog->item(11102302)->full_repair_cost, 150);
  EXPECT_TRUE(catalog->item(11401101)->npc_repairable);
}

} // namespace
} // namespace chronicles
