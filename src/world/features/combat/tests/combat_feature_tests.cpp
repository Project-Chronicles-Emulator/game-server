#include "world/features/combat/tests/combat_feature_test_support.hpp"

namespace chronicles::combat {
namespace {

using namespace testing_support;

class SharedCombatOwner : public ::testing::Test {
protected:
  SharedCombatOwner() {
    write_profile(fixture.root(), world_profile(Json::array({empty_population(9012),
                                                             empty_population(9013, "Instance"),
                                                             empty_population(9014)})));
    feature.load_encounters(nullptr, fixture.root() / "catalog");
  }

  chronicles::testing_support::TestCatalog fixture{catalog_tables()};
  std::shared_ptr<ClientCatalog> catalog = std::make_shared<ClientCatalog>(fixture.root());
  CombatFeature feature{catalog};
  Admission admission;
};

TEST_F(SharedCombatOwner, PublicMapReusesTheOneSharedPopulationAcrossCharacters) {
  const auto first = feature.population_for(9012, 19, admission);
  const auto second = feature.population_for(9012, 17, admission);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first, second);
  EXPECT_EQ(admission.owner_queries, 0);
}

TEST_F(SharedCombatOwner, SoloInstancesReuseOnlyTheSameCharacterPopulation) {
  const auto first = feature.population_for(9013, 19, admission);
  const auto same = feature.population_for(9013, 19, admission);
  const auto other = feature.population_for(9013, 17, admission);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first, same);
  EXPECT_NE(first, other);
}

TEST_F(SharedCombatOwner, ExpiredInstancePopulationIsNotKeptAliveByTheWeakCache) {
  std::weak_ptr<CombatSession::WorldMobs> released;
  {
    const auto first = feature.population_for(9013, 19, admission);
    released = first;
  }
  EXPECT_TRUE(released.expired());
  EXPECT_NE(feature.population_for(9013, 19, admission), nullptr);
}

TEST_F(SharedCombatOwner, RaidAdmissionRefusalDoesNotFallThroughToPublicPopulation) {
  EXPECT_EQ(feature.population_for(9014, 19, admission), nullptr);
  EXPECT_EQ(admission.owner_queries, 1);
  EXPECT_EQ(admission.last_character, 19);
  EXPECT_EQ(admission.last_map, 9014);
}

TEST_F(SharedCombatOwner, RaidPopulationUsesTheAdmittedChannelAndPreservesTheSoloKeySpace) {
  admission.owner = 7;
  const auto first = feature.population_for(9014, 19, admission);
  const auto same_channel = feature.population_for(9014, 17, admission);
  admission.owner = 8;
  const auto other_channel = feature.population_for(9014, 19, admission);
  EXPECT_EQ(first, same_channel);
  EXPECT_NE(first, other_channel);
  EXPECT_NE(first, feature.population_for(9013, 7, admission));
}

TEST_F(SharedCombatOwner, MissingMapReturnsBeforeAnyRaidAdmissionQuery) {
  EXPECT_EQ(feature.population_for(9900, 19, admission), nullptr);
  EXPECT_EQ(admission.map_queries, 0);
  EXPECT_EQ(admission.owner_queries, 0);
}

TEST_F(SharedCombatOwner, InstanceCopiesOwnTheirMonstersForEveryoneInside) {
  InstanceCopies copies({9013});
  auto party_copy = copies.enter(9013, 19, 5);
  const auto solo_copy = copies.enter(9013, 17, std::nullopt);

  admission.copy = party_copy;
  auto leader = feature.population_for(9013, 19, admission);
  auto member = feature.population_for(9013, 23, admission);
  admission.copy = solo_copy;
  const auto solo = feature.population_for(9013, 17, admission);

  ASSERT_NE(leader, nullptr);
  EXPECT_EQ(leader, member);
  EXPECT_NE(leader, solo);

  // The copy keeps its monsters while it lives, even when no session holds them, and releases
  // them with itself.
  std::weak_ptr<CombatSession::WorldMobs> kept = leader;
  admission.copy.reset();
  leader.reset();
  member.reset();
  EXPECT_FALSE(kept.expired());
  party_copy.reset();
  EXPECT_TRUE(kept.expired());
}

TEST_F(SharedCombatOwner, MovementAndFallRulesLoadOnlyAtTheirExplicitStartupStage) {
  auto tables = catalog_tables();
  tables["GameVariable"][0]["value"] = "1250";
  chronicles::testing_support::TestCatalog changed_fixture(tables);
  auto changed_catalog = std::make_shared<ClientCatalog>(changed_fixture.root());
  CombatFeature changed(changed_catalog);
  EXPECT_EQ(changed.fall_rules().safe_height_mm, 10000);
  changed.load_movement_rules(*changed_catalog);
  EXPECT_EQ(changed.fall_rules().safe_height_mm, 12500);
  EXPECT_EQ(changed.movement_rules().sprint_interval_ms, 600);
  EXPECT_EQ(changed.movement_rules().swim_interval_ms, 1000);
}

TEST(CombatStartupOwner, MissingProfileDisablesWhileWrongBuildIsRefusedBeforeCombatParsing) {
  chronicles::testing_support::TestCatalog fixture({});
  CombatFeature absent;
  absent.load_encounters(nullptr, {});
  EXPECT_FALSE(absent.data());
  Json invalid{{"format_version", 1}, {"build_id", "wrong"}, {"combat", Json::object()}};
  write_profile(fixture.root(), invalid);
  try {
    absent.load_encounters(nullptr, fixture.root() / "catalog");
    FAIL() << "A foreign profile build must fail before reading its CombatData";
  } catch (const std::runtime_error &error) {
    EXPECT_STREQ(error.what(), "World monster profile belongs to an unsupported build");
  }
}

TEST(CombatStartupOwner, DuplicateMapRemainsAFatalStartupError) {
  chronicles::testing_support::TestCatalog fixture({});
  write_profile(fixture.root(),
                world_profile(Json::array({empty_population(9012), empty_population(9012)})));
  CombatFeature feature;
  EXPECT_THROW(feature.load_encounters(nullptr, fixture.root() / "catalog"), std::runtime_error);
}

TEST(CombatStartupOwner, RevivalCatalogRequirementRemainsConditionalOnTutorial) {
  CombatFeature feature;
  EXPECT_NO_THROW(feature.load_revival_spawns(false, nullptr, {}));
  EXPECT_FALSE(feature.revival_spawns());
  EXPECT_THROW(feature.load_revival_spawns(true, nullptr, {}), std::runtime_error);
}

} // namespace
} // namespace chronicles::combat
