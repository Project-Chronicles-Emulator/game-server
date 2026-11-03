#include "world/features/npc/npc_interactions.hpp"

#include <gtest/gtest.h>

#include "data/client_catalog.hpp"
#include "world/core/unit_registry.hpp"

#include <cstdlib>
#include <optional>

#include <string>
#include <vector>

namespace chronicles {
namespace {

using Rows = std::vector<nlohmann::json>;

nlohmann::json npc(const char *id, const char *group) {
  return {{"id", id}, {"interactionGroupId", group}};
}

nlohmann::json interaction(const char *id, const char *group, const char *task_case,
                           const char *task) {
  return {{"id", id}, {"groupId", group}, {"playerTaskCase", task_case}, {"playerTask", task}};
}

nlohmann::json shop(const char *id) { return {{"id", id}, {"FK_itemId", "133"}}; }

// Mirrors real catalog ids: interaction 36 (group 32, shop 1006) is offered by Npc 1001425 and
// shares its group with a non-shop interaction; interaction 104 is offered by Npc 31;
// interactions 101 and 105 have no Npc.
struct Fixture {
  Rows npcs{npc("1001425", "32"), npc("1001673", "32"), npc("31", "104"), npc("9000001", "999")};
  Rows interactions{interaction("36", "32", "Shop", "\"shopId\":1006"),
                    interaction("40", "32", "VoucherGroup", ""),
                    interaction("104", "104", "Shop", "\"shopId\":101001"),
                    interaction("101", "101", "Shop", "\"shopId\":101001"),
                    interaction("105", "105", "Shop", "\"shopId\":111105")};
  Rows shops{shop("1006"), shop("1006"), shop("101001"), shop("111105")};

  NpcInteractionIndex index() const {
    return NpcInteractionIndex::build(npcs, interactions, shops);
  }
};

TEST(NpcInteractions, NpcOffersInteractionsAndShopsThroughItsGroup) {
  const auto index = Fixture{}.index();

  EXPECT_TRUE(index.npc_offers_interaction(1001425, 36));
  EXPECT_TRUE(index.npc_offers_interaction(1001425, 40));
  EXPECT_TRUE(index.npc_offers_interaction(1001673, 36));
  EXPECT_TRUE(index.npc_offers_shop(1001425, 1006));
  EXPECT_EQ(index.interactions_for_npc(1001425), (std::vector<std::int64_t>{36, 40}));

  EXPECT_TRUE(index.npc_offers_interaction(31, 104));
  EXPECT_TRUE(index.npc_offers_shop(31, 101001));
}

TEST(NpcInteractions, NpcDoesNotOfferAnotherNpcsInteractionOrShop) {
  const auto index = Fixture{}.index();

  EXPECT_FALSE(index.npc_offers_interaction(31, 36));
  EXPECT_FALSE(index.npc_offers_shop(31, 1006));
  EXPECT_FALSE(index.npc_offers_interaction(1001425, 104));
  EXPECT_FALSE(index.npc_offers_shop(1001425, 101001));
}

TEST(NpcInteractions, NonShopInteractionOffersNoShop) {
  const auto index = Fixture{}.index();

  EXPECT_TRUE(index.npc_offers_interaction(1001425, 40));
  EXPECT_EQ(index.shop_for_interaction(40), std::nullopt);
  EXPECT_EQ(index.shop_for_interaction(36), 1006);
}

TEST(NpcInteractions, InteractionsWithoutAnyNpcAreNotOfferedByAnyTemplate) {
  const auto index = Fixture{}.index();

  EXPECT_EQ(index.shop_for_interaction(101), 101001);
  EXPECT_EQ(index.shop_for_interaction(105), 111105);
  for (const std::int64_t npc_id : {1001425, 1001673, 31, 9000001}) {
    EXPECT_FALSE(index.npc_offers_interaction(npc_id, 101));
    EXPECT_FALSE(index.npc_offers_interaction(npc_id, 105));
    EXPECT_FALSE(index.npc_offers_shop(npc_id, 111105));
  }
}

TEST(NpcInteractions, UnknownIdsAndGrouplessNpcOfferNothing) {
  const auto index = Fixture{}.index();

  EXPECT_TRUE(index.interactions_for_npc(9000001).empty());
  EXPECT_FALSE(index.npc_offers_interaction(9000001, 36));
  EXPECT_TRUE(index.interactions_for_npc(424242).empty());
  EXPECT_FALSE(index.npc_offers_shop(424242, 1006));
  EXPECT_EQ(index.shop_for_interaction(424242), std::nullopt);
  EXPECT_EQ(index.npc_count(), 4);
  EXPECT_EQ(index.interaction_count(), 5);
}

void expect_rejected(const Fixture &fixture, const std::string &expected_context) {
  try {
    fixture.index();
    FAIL() << "expected rejection containing: " << expected_context;
  } catch (const std::invalid_argument &error) {
    EXPECT_NE(std::string(error.what()).find(expected_context), std::string::npos)
        << error.what();
  }
}

TEST(NpcInteractions, MalformedRowsAreRejectedWithTableRowAndField) {
  Fixture bad_npc_id;
  bad_npc_id.npcs[1]["id"] = "10x";
  expect_rejected(bad_npc_id, "Npc row 1: field 'id' is not a non-negative integer: '10x'");

  Fixture missing_group;
  missing_group.npcs[2].erase("interactionGroupId");
  expect_rejected(missing_group, "Npc row 2: field 'interactionGroupId' is missing");

  Fixture duplicate_npc;
  duplicate_npc.npcs.push_back(npc("31", "104"));
  expect_rejected(duplicate_npc, "Npc row 4: duplicate id 31");

  Fixture duplicate_interaction;
  duplicate_interaction.interactions.push_back(interaction("36", "32", "Use", ""));
  expect_rejected(duplicate_interaction, "Interaction row 5: duplicate id 36");

  Fixture not_an_object;
  not_an_object.interactions[0] = "36";
  expect_rejected(not_an_object, "Interaction row 0: row is not an object");

  Fixture bad_shop_task;
  bad_shop_task.interactions[0]["playerTask"] = "\"shop\":1006";
  expect_rejected(bad_shop_task, "Interaction row 0: Shop interaction playerTask has no valid");

  Fixture unknown_shop;
  unknown_shop.interactions[2]["playerTask"] = "\"shopId\":777";
  expect_rejected(unknown_shop, "Interaction row 2: shopId 777 is not a Shop.id");

  Fixture bad_shop_row;
  bad_shop_row.shops[0]["id"] = "";
  expect_rejected(bad_shop_row, "Shop row 0: field 'id' is not a non-negative integer");
}

// Two merchants announced by the server: unit 7001 (template 1001425, offers interaction 36 /
// shop 1006) and unit 7002 (template 31, offers interaction 104 / shop 101001), plus unit 7003
// (template 9000001, offers nothing). All on map 9012 in instance "a".
struct ShopSession {
  static constexpr int kMap = 9012;
  static constexpr const char *kInstance = "a";

  Fixture fixture;
  NpcInteractionIndex index = fixture.index();
  UnitRegistry registry{16};
  OpenedShopState state;

  ShopSession() {
    announce(7001, 1001425);
    announce(7002, 31);
    announce(7003, 9000001);
  }

  void announce(std::uint64_t unit_id, int template_id) {
    AnnouncedUnit unit;
    unit.unit_id = unit_id;
    unit.npc_template_id = template_id;
    unit.map_id = kMap;
    unit.instance_key = kInstance;
    ASSERT_EQ(registry.announce(std::move(unit)), UnitAnnounceResult::Added);
  }

  ShopOpenResult open(std::uint64_t unit_id, std::int64_t interaction_id,
                      int map_id = kMap, const char *instance = kInstance) {
    return state.open(index, registry, unit_id, interaction_id, map_id, instance);
  }
};

TEST(OpenedShopState, NothingIsPermittedUntilAShopIsOpened) {
  ShopSession session;

  EXPECT_FALSE(session.state.current().has_value());
  EXPECT_FALSE(session.state.permits(1006, 7001, ShopSession::kMap, ShopSession::kInstance));
}

TEST(OpenedShopState, AcceptedOpenPermitsExactlyThatShopUnitMapAndInstance) {
  ShopSession session;

  ASSERT_EQ(session.open(7001, 36), ShopOpenResult::Opened);
  ASSERT_TRUE(session.state.current().has_value());
  EXPECT_EQ(session.state.current()->shop_id, 1006);
  EXPECT_EQ(session.state.current()->interaction_id, 36);
  EXPECT_TRUE(session.state.permits(1006, 7001, ShopSession::kMap, ShopSession::kInstance));

  EXPECT_FALSE(session.state.permits(101001, 7001, ShopSession::kMap, ShopSession::kInstance));
  EXPECT_FALSE(session.state.permits(1006, 7002, ShopSession::kMap, ShopSession::kInstance));
  EXPECT_FALSE(session.state.permits(1006, 7001, 8000, ShopSession::kInstance));
  EXPECT_FALSE(session.state.permits(1006, 7001, ShopSession::kMap, "b"));
}

TEST(OpenedShopState, RefusedOpensRecordNothingAndKeepTheExistingState) {
  ShopSession session;

  EXPECT_EQ(session.open(9999, 36), ShopOpenResult::UnknownUnit);
  EXPECT_EQ(session.open(7001, 36, 8000), ShopOpenResult::UnknownUnit);
  EXPECT_EQ(session.open(7001, 36, ShopSession::kMap, "b"), ShopOpenResult::UnknownUnit);
  EXPECT_EQ(session.open(7001, 104), ShopOpenResult::InteractionNotOffered);
  EXPECT_EQ(session.open(7003, 36), ShopOpenResult::InteractionNotOffered);
  EXPECT_EQ(session.open(7001, 40), ShopOpenResult::NotAShopInteraction);
  EXPECT_FALSE(session.state.current().has_value());

  ASSERT_EQ(session.open(7001, 36), ShopOpenResult::Opened);
  EXPECT_EQ(session.open(7001, 104), ShopOpenResult::InteractionNotOffered);
  EXPECT_TRUE(session.state.permits(1006, 7001, ShopSession::kMap, ShopSession::kInstance));
}

TEST(OpenedShopState, OpeningAnotherShopReplacesThePreviousOne) {
  ShopSession session;

  ASSERT_EQ(session.open(7001, 36), ShopOpenResult::Opened);
  ASSERT_EQ(session.open(7002, 104), ShopOpenResult::Opened);

  EXPECT_TRUE(session.state.permits(101001, 7002, ShopSession::kMap, ShopSession::kInstance));
  EXPECT_FALSE(session.state.permits(1006, 7001, ShopSession::kMap, ShopSession::kInstance));
}

TEST(OpenedShopState, DespawnOfTheOpenedUnitClearsButAnotherUnitDoesNot) {
  ShopSession session;
  ASSERT_EQ(session.open(7001, 36), ShopOpenResult::Opened);

  session.state.on_unit_despawn(7002);
  EXPECT_TRUE(session.state.permits(1006, 7001, ShopSession::kMap, ShopSession::kInstance));

  session.state.on_unit_despawn(7001);
  EXPECT_FALSE(session.state.current().has_value());
  EXPECT_FALSE(session.state.permits(1006, 7001, ShopSession::kMap, ShopSession::kInstance));
}

TEST(OpenedShopState, MapOrInstanceChangeClearsButTheSameLocationDoesNot) {
  ShopSession session;
  ASSERT_EQ(session.open(7001, 36), ShopOpenResult::Opened);

  session.state.on_map_or_instance_change(ShopSession::kMap, ShopSession::kInstance);
  EXPECT_TRUE(session.state.permits(1006, 7001, ShopSession::kMap, ShopSession::kInstance));

  session.state.on_map_or_instance_change(ShopSession::kMap, "b");
  EXPECT_FALSE(session.state.current().has_value());

  ASSERT_EQ(session.open(7001, 36), ShopOpenResult::Opened);
  session.state.on_map_or_instance_change(8000, ShopSession::kInstance);
  EXPECT_FALSE(session.state.current().has_value());
}

// Mirrors what the session does on a map change: the registry is cleared (units of the previous
// map are forgotten) and the opened-shop state is reset on the same strand.
TEST(OpenedShopState, MapChangeClearsRegistryAndStateSoThePreviousMapsUnitCannotBeOpened) {
  ShopSession session;
  ASSERT_EQ(session.open(7001, 36), ShopOpenResult::Opened);

  session.registry.clear();
  session.state.on_map_or_instance_change(8000, ShopSession::kInstance);

  EXPECT_FALSE(session.state.current().has_value());
  EXPECT_FALSE(session.state.permits(1006, 7001, ShopSession::kMap, ShopSession::kInstance));
  EXPECT_EQ(session.open(7001, 36), ShopOpenResult::UnknownUnit);
  EXPECT_FALSE(session.state.current().has_value());
}

TEST(OpenedShopState, ANewSessionStartsEmptyAndSessionsDoNotShareState) {
  ShopSession first;
  ShopSession second;
  ASSERT_EQ(first.open(7001, 36), ShopOpenResult::Opened);

  EXPECT_FALSE(second.state.current().has_value());
  EXPECT_FALSE(second.state.permits(1006, 7001, ShopSession::kMap, ShopSession::kInstance));
  EXPECT_TRUE(first.state.permits(1006, 7001, ShopSession::kMap, ShopSession::kInstance));
}

TEST(OpenedShopState, ClearForDisconnectOrExplicitCloseRemovesEligibility) {
  ShopSession session;
  ASSERT_EQ(session.open(7001, 36), ShopOpenResult::Opened);

  session.state.clear();

  EXPECT_FALSE(session.state.permits(1006, 7001, ShopSession::kMap, ShopSession::kInstance));
}

// Loads the real exported catalog (the same files the server uses). Gated on the catalog path,
// like the other catalog-backed tests: set CHRONICLES_CLIENT_CATALOG to data/generated/catalog.
TEST(NpcInteractionIndexCatalog, LoadsRealCatalogAndResolvesTheStationaryMerchantChain) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the exported catalog directory";
  }

  ClientCatalog catalog(catalog_path);
  const auto index = NpcInteractionIndex::load(catalog);
  EXPECT_GT(index.npc_count(), 0u);
  EXPECT_GT(index.interaction_count(), 0u);

  // Documented chain (npc-registry-audit.md / npc-spawn-data-audit.md): Npc 1001121 offers
  // interaction 212, a Shop interaction for shop 1001.
  EXPECT_TRUE(index.npc_offers_interaction(1001121, 212));
  EXPECT_EQ(index.shop_for_interaction(212), std::optional<std::int64_t>(1001));
  EXPECT_TRUE(index.npc_offers_shop(1001121, 1001));

  // Interaction 36 (group 32) is shop 1006 and is offered by Npc 1001425.
  EXPECT_EQ(index.shop_for_interaction(36), std::optional<std::int64_t>(1006));
  EXPECT_TRUE(index.npc_offers_shop(1001425, 1006));

  // Another template's interaction or shop is not offered, and an unknown template offers nothing.
  EXPECT_FALSE(index.npc_offers_shop(1001121, 1006));
  EXPECT_FALSE(index.npc_offers_interaction(1001121, 36));
  EXPECT_FALSE(index.npc_offers_interaction(-1, 212));
  EXPECT_TRUE(index.interactions_for_npc(-1).empty());
}

// Opens the real stationary merchant through the server-owned registry only. Catalog rows used
// (data/generated/catalog/Tables): Npc.jsonl id 1001121 (name Beren) interactionGroupId 212;
// Interaction.jsonl id 212, groupId 212, playerTaskCase Shop, playerTask "shopId":1001;
// Shop.jsonl id 1001 (14 rows). Npc 1001425 offers interaction 36 / shop 1006 instead.
// The native buy/sell refusal (kMerchantTargetAuthorizationAvailable = false) lives in
// native_world.cpp and is covered by the NativeWorld.Shop* refusal tests; it is not reachable here.
TEST(NpcInteractionIndexCatalog, OpensTheRealMerchantOnlyForTheAnnouncedUnitOnItsOwnMapAndInstance) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the exported catalog directory";
  }

  ClientCatalog catalog(catalog_path);
  const auto index = NpcInteractionIndex::load(catalog);

  constexpr std::uint64_t kMerchantUnit = 9100001;
  constexpr std::uint64_t kOtherNpcUnit = 9100002;
  UnitRegistry registry{16};
  const auto announce = [&](std::uint64_t unit_id, int template_id) {
    AnnouncedUnit unit;
    unit.unit_id = unit_id;
    unit.npc_template_id = template_id;
    unit.map_id = 9012;
    unit.instance_key = "42";
    ASSERT_EQ(registry.announce(std::move(unit)), UnitAnnounceResult::Added);
  };
  announce(kMerchantUnit, 1001121);
  announce(kOtherNpcUnit, 1001425);

  OpenedShopState state;
  ASSERT_EQ(state.open(index, registry, kMerchantUnit, 212, 9012, "42"), ShopOpenResult::Opened);
  ASSERT_TRUE(state.current().has_value());
  EXPECT_EQ(state.current()->shop_id, 1001);
  EXPECT_EQ(state.current()->interaction_id, 212);
  EXPECT_TRUE(state.permits(1001, kMerchantUnit, 9012, "42"));

  // Eligibility is bound to that exact unit, shop, map and instance.
  EXPECT_FALSE(state.permits(1006, kMerchantUnit, 9012, "42"));
  EXPECT_FALSE(state.permits(1001, kOtherNpcUnit, 9012, "42"));
  EXPECT_FALSE(state.permits(1001, kMerchantUnit, 8000, "42"));
  EXPECT_FALSE(state.permits(1001, kMerchantUnit, 9012, "43"));

  // A target announced on another map or instance cannot be opened from this one.
  OpenedShopState fresh;
  EXPECT_EQ(fresh.open(index, registry, kMerchantUnit, 212, 8000, "42"),
            ShopOpenResult::UnknownUnit);
  EXPECT_EQ(fresh.open(index, registry, kMerchantUnit, 212, 9012, "43"),
            ShopOpenResult::UnknownUnit);

  // Another NPC's interaction is not offered by the merchant, and the merchant's is not offered
  // by the other NPC.
  EXPECT_EQ(fresh.open(index, registry, kMerchantUnit, 36, 9012, "42"),
            ShopOpenResult::InteractionNotOffered);
  EXPECT_EQ(fresh.open(index, registry, kOtherNpcUnit, 212, 9012, "42"),
            ShopOpenResult::InteractionNotOffered);
  EXPECT_FALSE(fresh.current().has_value());
}

} // namespace
} // namespace chronicles
