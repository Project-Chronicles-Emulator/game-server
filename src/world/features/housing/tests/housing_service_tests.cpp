#include "world/features/housing/housing_service.hpp"
#include "testing/memory_repositories.hpp"
#include "testing/housing_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace chronicles::housing {
namespace {

using namespace testing_support;

constexpr std::int64_t kStartTicks = 638'000'000'000'000'000;
constexpr int kWorldId = 1;
constexpr std::int64_t kAccountId = 7;

// The regenerated data/generated/housing/doodads.json next to the catalog that
// CHRONICLES_CLIENT_CATALOG names, or nullopt when that export is unavailable.
std::optional<nlohmann::json> exported_doodad_profile_json() {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    return std::nullopt;
  }

  const auto path =
      std::filesystem::path(catalog_path).parent_path() / "housing" / "doodads.json";
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  return nlohmann::json::parse(input);
}

class FakeGuilds final : public GuildDirectory {
public:
  std::optional<GuildMembership> membership(CharacterId character_id) const override {
    const auto found = memberships.find(character_id);
    if (found == memberships.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  std::vector<GuildMember> members(GuildId guild_id) const override {
    std::vector<GuildMember> result;
    for (const auto &member : roster) {
      const auto found = memberships.find(member.character_id);
      if (found != memberships.end() && found->second.guild_id == guild_id) {
        result.push_back(member);
      }
    }
    return result;
  }

  std::optional<bool> guild_exists(GuildId guild_id) const override {
    if (!answers_guild_existence) {
      return std::nullopt;
    }
    return std::ranges::any_of(memberships, [&](const auto &entry) {
      return entry.second.guild_id == guild_id;
    });
  }

  std::map<CharacterId, GuildMembership> memberships;
  std::vector<GuildMember> roster;
  // Off by default: like a directory without a guild registry, existence is unknown.
  bool answers_guild_existence = false;
};

class HousingServiceTest : public testing::Test {
protected:
  void SetUp() override {
    housing_store =
        std::make_shared<MemoryHousingStore>(repositories.character_state);
    owner = create_character("Builder");
    visitor = create_character("Visitor");
    guilds = std::make_shared<FakeGuilds>();
    service = make_service();
  }

  std::unique_ptr<HousingService> make_service(HousingPolicy policy = {}) {
    return std::make_unique<HousingService>(
        housing_catalog(), housing_store, kWorldId, [this] { return now; }, policy, guilds);
  }

  HousingActor create_character(const std::string &name) {
    // One account per character keeps lobby slots free for any number of test characters.
    const auto account_id = kAccountId + next_account_offset++;
    Character created;
    created.account_id = account_id;
    created.name = name;
    const auto character = repositories.characters->create(created);
    return HousingActor{account_id, character.id, kWorldId, name,
                        WorldPosition{8000, 1000, 2000, 3000, 0.0F}};
  }

  WorldProgress progress(const HousingActor &actor) {
    return repositories.character_state->load_world_progress(actor.account_id, actor.character_id,
                                                             kWorldId);
  }

  void give(const HousingActor &actor, std::map<int, int> stackables,
            std::map<int, HousingItemRecord> housing_items = {}, std::int64_t tokens = 0) {
    const auto before = progress(actor);
    auto after = before;
    after.revision = before.revision + 1;
    for (const auto &[cms_id, count] : stackables) {
      after.stackables[cms_id] += count;
    }
    for (const auto &[cms_id, record] : housing_items) {
      after.housing_items[cms_id] = record;
    }
    after.hersander_token += tokens;
    ASSERT_TRUE(repositories.character_state->save_world_progress(
        actor.account_id, actor.character_id, kWorldId, before, after));
  }

  ContractResult rent_small(const HousingActor &actor) {
    return service->contract(actor, SiteType::small, kZoneKey, ContractType::monthly, nullptr);
  }

  void enter_edit_mode(const HousingActor &actor, int channel = 1) {
    ASSERT_EQ(service->edit_mode(actor, EditMode::build, false, kZoneKey, channel).ec,
              x5::ec::Success);
  }

  UpdateResult update(const HousingActor &actor, UpdateRequest request, int channel = 1) {
    if (request.lot_number == 0) {
      request.lot_number = kSmallLot;
    }
    const auto result = service->update(actor, request, kZoneKey, channel, nullptr);
    // Every accepted save starts the 5 s cooldown; step past it for the next save.
    now += 6 * kEpochTicksPerSecond;
    return result;
  }

  // Rents the owner's small lot with the garden profile installed and places a garden plot at
  // site level. Returns the time the plot was placed (its first phase start).
  std::int64_t rent_with_garden(nlohmann::json profile = garden_profile_json(),
                                int seeds = 7) {
    give(owner, {{kPermitItem, 60}, {kSeedItem, seeds}}, {{kFarmItem, {1, 0}}});
    EXPECT_EQ(rent_small(owner).ec, x5::ec::Success);
    service->set_doodad_profile(std::make_shared<const HousingDoodadProfile>(profile), 11);
    enter_edit_mode(owner);
    UpdateRequest place;
    auto plot = chair(kGardenDoodadId);
    plot.cms_id = kFarmDoodad;
    place.doodads = {plot};
    const auto placed_at = now;
    EXPECT_EQ(update(owner, place).ec, x5::ec::Success);
    return placed_at;
  }

  static constexpr std::uint32_t kGardenDoodadId = 30;
  static constexpr DoodadUnitKey kGardenPlot{kSmallLot, 0, kGardenDoodadId};

  static DoodadRecord chair(std::uint32_t doodad_id, LocalPosition position = {100, 200, 0}) {
    DoodadRecord doodad;
    doodad.doodad_id = doodad_id;
    doodad.cms_id = kChairDoodad;
    doodad.position = position;
    return doodad;
  }

  std::int64_t now{kStartTicks};
  std::int64_t next_account_offset{};
  MemoryRepositories repositories;
  std::shared_ptr<MemoryHousingStore> housing_store;
  std::shared_ptr<FakeGuilds> guilds;
  std::unique_ptr<HousingService> service;
  HousingActor owner;
  HousingActor visitor;
};

TEST_F(HousingServiceTest, ContractChargesPermitsAndAssignsTheFirstFreeLot) {
  give(owner, {{kPermitItem, 100}});

  const auto result = rent_small(owner);
  ASSERT_EQ(result.ec, x5::ec::Success);
  EXPECT_EQ(result.lot_number, kSmallLot);
  EXPECT_EQ(result.channel, 1);
  EXPECT_EQ(result.contract_type, ContractType::monthly);
  EXPECT_EQ(result.expiry_epoch_ticks, kStartTicks + 30 * kEpochTicksPerDay);
  EXPECT_EQ(progress(owner).stackables.at(kPermitItem), 40);

  ASSERT_EQ(result.outcome.sites_added.size(), 1U);
  ASSERT_EQ(result.outcome.progress_changes.size(), 1U);
  ASSERT_EQ(result.outcome.owner_sites_changed.size(), 1U);

  // The preset building and object come with the lot and are not storage items.
  const auto site = service->personal_site(owner.character_id);
  ASSERT_TRUE(site.has_value());
  ASSERT_EQ(site->contents.houses.size(), 1U);
  EXPECT_FALSE(site->contents.houses.at(1).from_storage);
  ASSERT_EQ(site->contents.doodads.size(), 1U);
  EXPECT_EQ(site->contents.doodads.begin()->second.cms_id, kPresetDoodad);
  EXPECT_EQ(site->owner_name, "Builder");
  ASSERT_TRUE(site->return_position.has_value());
  EXPECT_EQ(site->return_position->map_id, 8000);

  give(visitor, {{kPermitItem, 60}});
  const auto second = rent_small(visitor);
  ASSERT_EQ(second.ec, x5::ec::Success);
  EXPECT_EQ(second.lot_number, kSecondSmallLot);
}

TEST_F(HousingServiceTest, ContractRefusesInvalidRequestsWithoutCharging) {
  give(owner, {{kPermitItem, 59}});
  EXPECT_EQ(rent_small(owner).ec, x5::ec::Housing_RentItemNotEnough);
  EXPECT_EQ(progress(owner).stackables.at(kPermitItem), 59);
  EXPECT_FALSE(service->personal_site(owner.character_id).has_value());

  give(owner, {{kPermitItem, 100}});
  EXPECT_EQ(service->contract(owner, SiteType::small, kUnmappedZoneKey, ContractType::monthly,
                              nullptr)
                .ec,
            x5::ec::Housing_InvalidZoneKey);
  EXPECT_EQ(service->contract(owner, SiteType::small, kZoneKey, ContractType::close, nullptr).ec,
            x5::ec::Housing_InvalidContractType);
  EXPECT_EQ(service->contract(owner, SiteType::none, kZoneKey, ContractType::monthly, nullptr).ec,
            x5::ec::Housing_InvalidArea);

  ASSERT_EQ(rent_small(owner).ec, x5::ec::Success);
  EXPECT_EQ(rent_small(owner).ec, x5::ec::Housing_CanHaveOnlyOneHousingSite);
}

TEST_F(HousingServiceTest, PlayerContractsNeedTheCompanyPositionOfTheSiteSize) {
  give(owner, {{kPermitItem, 300}}, {}, 1000);
  const auto before = progress(owner);

  auto applicant = owner;
  applicant.company_position_level = 2;
  EXPECT_EQ(service->contract(applicant, SiteType::small, kZoneKey, ContractType::monthly, nullptr)
                .ec,
            x5::ec::Housing_InsufficientCompanyPosition);

  // GeneralHousingSiteMpositionLv defaults to 4, one above the small-lot level.
  applicant.company_position_level = 3;
  EXPECT_EQ(
      service->contract(applicant, SiteType::medium, kZoneKey, ContractType::monthly, nullptr).ec,
      x5::ec::Housing_InsufficientCompanyPosition);

  // Refusals charge nothing and assign no lot.
  EXPECT_EQ(progress(owner).revision, before.revision);
  EXPECT_EQ(progress(owner).stackables.at(kPermitItem), 300);
  EXPECT_EQ(progress(owner).hersander_token, 1000);
  EXPECT_FALSE(service->personal_site(owner.character_id).has_value());

  const auto rented =
      service->contract(applicant, SiteType::small, kZoneKey, ContractType::monthly, nullptr);
  ASSERT_EQ(rented.ec, x5::ec::Success);
  EXPECT_EQ(progress(owner).stackables.at(kPermitItem), 240);
}

TEST_F(HousingServiceTest, OperatorContractsWithoutCompanyStandingAreNotGated) {
  give(owner, {{kPermitItem, 60}});
  ASSERT_FALSE(owner.company_position_level.has_value());

  EXPECT_EQ(rent_small(owner).ec, x5::ec::Success);
}

TEST_F(HousingServiceTest, MediumSitesNeedAOneTimeHersanderTokenUnlock) {
  give(owner, {{kPermitItem, 500}});
  EXPECT_EQ(service->contract(owner, SiteType::medium, kZoneKey, ContractType::monthly, nullptr).ec,
            x5::ec::MoneyNotEnough);

  give(owner, {}, {}, 1500);
  ASSERT_EQ(
      service->contract(owner, SiteType::medium, kZoneKey, ContractType::monthly, nullptr).ec,
      x5::ec::Success);
  EXPECT_EQ(progress(owner).hersander_token, 500);
  EXPECT_EQ(progress(owner).stackables.at(kPermitItem), 260);

  const auto info = service->unlock_info(owner, kZoneKey);
  EXPECT_NE(std::ranges::find(info.unlocked_site_types, SiteType::medium),
            info.unlocked_site_types.end());
  EXPECT_TRUE(info.region_rent_locked);
  EXPECT_TRUE(info.rentable_site_types.empty());

  // A second medium contract after release does not charge the unlock again.
  ASSERT_EQ(service->release(owner, PackageReason::cheat, nullptr).ec, x5::ec::Success);
  ASSERT_EQ(
      service->contract(owner, SiteType::medium, kZoneKey, ContractType::monthly, nullptr).ec,
      x5::ec::Success);
  EXPECT_EQ(progress(owner).hersander_token, 500);
}

TEST_F(HousingServiceTest, FullChannelsOpenANewChannelUntilThePolicyLimit) {
  HousingPolicy policy;
  policy.personal_channel_open_limit = 2;
  service = make_service(policy);

  std::vector<HousingActor> renters;
  for (int index = 0; index < 5; ++index) {
    auto renter = create_character("Renter" + std::to_string(index));
    give(renter, {{kPermitItem, 60}});
    renters.push_back(renter);
  }

  ASSERT_EQ(rent_small(renters[0]).channel, 1);
  ASSERT_EQ(rent_small(renters[1]).channel, 1);
  const auto third = rent_small(renters[2]);
  ASSERT_EQ(third.ec, x5::ec::Success);
  EXPECT_EQ(third.channel, 2);
  EXPECT_EQ(third.lot_number, kSmallLot);
  ASSERT_EQ(rent_small(renters[3]).channel, 2);
  EXPECT_EQ(rent_small(renters[4]).ec, x5::ec::Housing_ChannelLimitReached);
  EXPECT_EQ(progress(renters[4]).stackables.at(kPermitItem), 60);
}

TEST_F(HousingServiceTest, ExtensionOnlyInsideTheImminentWindow) {
  give(owner, {{kPermitItem, 200}});
  ASSERT_EQ(rent_small(owner).ec, x5::ec::Success);

  EXPECT_EQ(service->extend(owner, ContractType::monthly, nullptr).ec,
            x5::ec::Housing_FailedContractExtension);

  now = kStartTicks + 25 * kEpochTicksPerDay;
  const auto extended = service->extend(owner, ContractType::periodic, nullptr);
  ASSERT_EQ(extended.ec, x5::ec::Success);
  EXPECT_EQ(extended.expiry_epoch_ticks, kStartTicks + 60 * kEpochTicksPerDay);
  EXPECT_EQ(extended.contract_type, ContractType::periodic);
  EXPECT_EQ(progress(owner).stackables.at(kPermitItem), 80);

  const auto closed = service->extend(owner, ContractType::close, nullptr);
  ASSERT_EQ(closed.ec, x5::ec::Success);
  EXPECT_EQ(closed.contract_type, ContractType::close);
  EXPECT_EQ(progress(owner).stackables.at(kPermitItem), 80);

  EXPECT_EQ(service->extend(visitor, ContractType::monthly, nullptr).ec,
            x5::ec::Housing_NotRegisteredHousingSite);
}

TEST_F(HousingServiceTest, UpdatePlacesMovesAndRemovesStorageObjects) {
  give(owner, {{kPermitItem, 60}}, {{kChairItem, {2, 0}}});
  ASSERT_EQ(rent_small(owner).ec, x5::ec::Success);

  UpdateRequest place;
  place.doodads = {chair(10)};
  EXPECT_EQ(update(owner, place).ec, x5::ec::Housing_InvalidDoodadUpdateReq);

  enter_edit_mode(owner);
  const auto placed = update(owner, place);
  ASSERT_EQ(placed.ec, x5::ec::Success);
  EXPECT_EQ(progress(owner).housing_items.at(kChairItem), (HousingItemRecord{2, 1}));
  ASSERT_EQ(placed.outcome.site_updates.size(), 1U);
  ASSERT_EQ(placed.outcome.site_updates.front().doodads.size(), 1U);
  EXPECT_TRUE(placed.outcome.site_updates.front().doodads.front().from_storage);

  // A move keeps the object installed and charges nothing.
  UpdateRequest move;
  move.doodads = {chair(10, {300, 300, 0})};
  ASSERT_EQ(update(owner, move).ec, x5::ec::Success);
  EXPECT_EQ(progress(owner).housing_items.at(kChairItem), (HousingItemRecord{2, 1}));

  UpdateRequest remove;
  remove.remove_doodad_unit_ids = {housing_doodad_unit_id(kSmallLot, 0, 10)};
  const auto removed = update(owner, remove);
  ASSERT_EQ(removed.ec, x5::ec::Success);
  EXPECT_EQ(progress(owner).housing_items.at(kChairItem), (HousingItemRecord{2, 0}));
  ASSERT_EQ(removed.outcome.site_updates.front().removed_doodad_unit_ids.size(), 1U);

  // Removing the preset object returns nothing to storage.
  UpdateRequest remove_preset;
  remove_preset.remove_doodad_unit_ids = {housing_doodad_unit_id(kSmallLot, 0, 1)};
  ASSERT_EQ(update(owner, remove_preset).ec, x5::ec::Success);
  EXPECT_FALSE(progress(owner).housing_items.contains(kPresetDoodad));
}

// The stock client leaves edit mode first and saves after HousingEditModeNoti{end} arrives.
TEST_F(HousingServiceTest, SaveAfterLeavingEditModeIsAcceptedOnceAndPersists) {
  give(owner, {{kPermitItem, 60}}, {{kChairItem, {3, 0}}});
  ASSERT_EQ(rent_small(owner).ec, x5::ec::Success);

  enter_edit_mode(owner);
  ASSERT_EQ(service->edit_mode(owner, EditMode::build, true, kZoneKey, 1).ec, x5::ec::Success);
  UpdateRequest place;
  place.doodads = {chair(10)};
  const auto saved = update(owner, place);
  ASSERT_EQ(saved.ec, x5::ec::Success);
  EXPECT_EQ(progress(owner).housing_items.at(kChairItem), (HousingItemRecord{3, 1}));

  // The trailing save closed the session.
  UpdateRequest again;
  again.doodads = {chair(11)};
  EXPECT_EQ(update(owner, again).ec, x5::ec::Housing_InvalidDoodadUpdateReq);

  // A service started from the same store (a server restart) sees the placed chair.
  const auto restarted = make_service();
  const auto site = restarted->personal_site(owner.character_id);
  ASSERT_TRUE(site.has_value());
  const auto chair_entry = site->contents.doodads.find(DoodadKey{0, 10});
  ASSERT_NE(chair_entry, site->contents.doodads.end());
  EXPECT_EQ(chair_entry->second.cms_id, kChairDoodad);
  EXPECT_TRUE(chair_entry->second.from_storage);
}

TEST_F(HousingServiceTest, TrailingSaveWindowExpiresAndRefusalsCloseTheSession) {
  give(owner, {{kPermitItem, 60}}, {{kChairItem, {3, 0}}});
  ASSERT_EQ(rent_small(owner).ec, x5::ec::Success);
  UpdateRequest place;
  place.doodads = {chair(10)};

  // Ending edit mode that was never entered opens nothing.
  ASSERT_EQ(service->edit_mode(owner, EditMode::build, true, kZoneKey, 1).ec, x5::ec::Success);
  EXPECT_EQ(update(owner, place).ec, x5::ec::Housing_InvalidDoodadUpdateReq);

  // A save that arrives after the window is refused.
  enter_edit_mode(owner);
  ASSERT_EQ(service->edit_mode(owner, EditMode::build, true, kZoneKey, 1).ec, x5::ec::Success);
  now += HousingPolicy{}.edit_save_window_ticks + 1;
  EXPECT_EQ(update(owner, place).ec, x5::ec::Housing_InvalidDoodadUpdateReq);

  // A refused trailing save also closes the session.
  enter_edit_mode(owner);
  ASSERT_EQ(service->edit_mode(owner, EditMode::build, true, kZoneKey, 1).ec, x5::ec::Success);
  UpdateRequest outside;
  outside.doodads = {chair(10, {20000, 0, 0})};
  EXPECT_EQ(update(owner, outside).ec, x5::ec::Housing_InvalidArea);
  EXPECT_EQ(update(owner, place).ec, x5::ec::Housing_InvalidDoodadUpdateReq);
  EXPECT_EQ(progress(owner).housing_items.at(kChairItem), (HousingItemRecord{3, 0}));
}

TEST_F(HousingServiceTest, CustomizationSavedAfterLeavingCustomizeMode) {
  give(owner, {{kPermitItem, 60}});
  ASSERT_EQ(rent_small(owner).ec, x5::ec::Success);
  ASSERT_EQ(service->edit_mode(owner, EditMode::customize, false, kZoneKey, 1).ec,
            x5::ec::Success);
  ASSERT_EQ(service->edit_mode(owner, EditMode::customize, true, kZoneKey, 1).ec,
            x5::ec::Success);

  UpdateRequest customize;
  customize.house_part_custom_ids = std::string("\x01\x02\x03", 3);
  const auto saved = update(owner, customize);
  ASSERT_EQ(saved.ec, x5::ec::Success);
  ASSERT_EQ(saved.outcome.site_updates.size(), 1U);
  EXPECT_TRUE(saved.outcome.site_updates.front().custom_ids_changed);
  EXPECT_EQ(make_service()->personal_site(owner.character_id)->contents.house_part_custom_ids,
            std::string("\x01\x02\x03", 3));
}

TEST_F(HousingServiceTest, UpdateRejectsInvalidObjects) {
  give(owner, {{kPermitItem, 60}}, {{kChairItem, {1, 0}}, {kFarmItem, {3, 0}}});
  ASSERT_EQ(rent_small(owner).ec, x5::ec::Success);
  enter_edit_mode(owner);

  UpdateRequest outside;
  outside.doodads = {chair(10, {20000, 0, 0})};
  EXPECT_EQ(update(owner, outside).ec, x5::ec::Housing_InvalidArea);

  UpdateRequest duplicate;
  duplicate.doodads = {chair(10), chair(10)};
  EXPECT_EQ(update(owner, duplicate).ec, x5::ec::Housing_DuplicateHousingDoodad);

  UpdateRequest out_of_bounds;
  out_of_bounds.doodads = {chair(kMaximumObjectId + 1)};
  EXPECT_EQ(update(owner, out_of_bounds).ec, x5::ec::Housing_OutOfBoundId);

  UpdateRequest too_many;
  too_many.doodads = {chair(10), chair(11)};
  EXPECT_EQ(update(owner, too_many).ec, x5::ec::ItemError);

  UpdateRequest farms;
  for (std::uint32_t id = 20; id < 22; ++id) {
    auto farm = chair(id);
    farm.cms_id = kFarmDoodad;
    farms.doodads.push_back(farm);
  }
  EXPECT_EQ(update(owner, farms).ec, x5::ec::Housing_InvalidEditModeSaveFarmDoodadCount);

  UpdateRequest empty;
  EXPECT_EQ(update(owner, empty).ec, x5::ec::Housing_NoneChange);

  UpdateRequest wrong_lot;
  wrong_lot.lot_number = kSecondSmallLot;
  wrong_lot.doodads = {chair(10)};
  EXPECT_EQ(update(owner, wrong_lot).ec, x5::ec::Housing_NotOwner);

  // Nothing above changed the stored site or storage.
  EXPECT_EQ(progress(owner).housing_items.at(kChairItem), (HousingItemRecord{1, 0}));
  EXPECT_EQ(service->personal_site(owner.character_id)->contents.doodads.size(), 1U);
}

TEST_F(HousingServiceTest, SavesRespectTheEditCooldownAndObjectLimit) {
  give(owner, {{kPermitItem, 60}}, {{kChairItem, {20, 0}}});
  ASSERT_EQ(rent_small(owner).ec, x5::ec::Success);
  enter_edit_mode(owner);

  UpdateRequest first;
  first.lot_number = kSmallLot;
  first.doodads = {chair(10)};
  ASSERT_EQ(service->update(owner, first, kZoneKey, 1, nullptr).ec, x5::ec::Success);
  UpdateRequest second;
  second.lot_number = kSmallLot;
  second.doodads = {chair(11)};
  EXPECT_EQ(service->update(owner, second, kZoneKey, 1, nullptr).ec,
            x5::ec::Housing_InvalidEditModeSaveCooldown);
  now += 6 * kEpochTicksPerSecond;

  // Two preset objects plus one placed chair; the test catalog allows eight per small site.
  UpdateRequest crowded;
  for (std::uint32_t id = 20; id < 26; ++id) {
    crowded.doodads.push_back(chair(id));
  }
  EXPECT_EQ(update(owner, crowded).ec, x5::ec::Housing_InvalidEditModeSaveObjectLimitCount);
}

TEST_F(HousingServiceTest, FacilityBuildingsCarryTheirCraftingStation) {
  give(owner, {{kPermitItem, 60}}, {{kFacilityItem, {1, 0}}});
  ASSERT_EQ(rent_small(owner).ec, x5::ec::Success);
  enter_edit_mode(owner);

  HouseRecord facility;
  facility.house_id = 2;
  facility.attach_index = 3;
  facility.housing_id = kFacility;
  facility.position = {1000, 1000, 0};
  UpdateRequest place;
  place.houses = {facility};
  const auto placed = update(owner, place);
  ASSERT_EQ(placed.ec, x5::ec::Success);
  ASSERT_EQ(placed.outcome.site_updates.front().doodads.size(), 1U);
  const auto &station = placed.outcome.site_updates.front().doodads.front();
  EXPECT_EQ(station.cms_id, kStationDoodad);
  EXPECT_EQ(station.house_id, 2U);
  EXPECT_FALSE(station.from_storage);
  EXPECT_EQ(progress(owner).housing_items.at(kFacilityItem), (HousingItemRecord{1, 1}));

  // A main building cannot take an attach index.
  HouseRecord attached_main;
  attached_main.house_id = 3;
  attached_main.attach_index = 1;
  attached_main.housing_id = kMainBuilding;
  UpdateRequest invalid;
  invalid.houses = {attached_main};
  EXPECT_EQ(update(owner, invalid).ec, x5::ec::Housing_InvalidAttachIndex);

  UpdateRequest remove;
  remove.remove_house_ids = {2};
  ASSERT_EQ(update(owner, remove).ec, x5::ec::Success);
  EXPECT_EQ(progress(owner).housing_items.at(kFacilityItem), (HousingItemRecord{1, 0}));
  EXPECT_FALSE(service->personal_site(owner.character_id)->contents.doodads.contains({2, 1}));
}

// The stock client places a crafting station from housing storage as the station item's
// doodad, attached to the lot's preset main building (live request: house 1, doodad 11,
// cms 85002). The save must install the building item and keep the doodad.
TEST_F(HousingServiceTest, CraftingStationsPlacedAsDoodadsInstallTheirBuildingItem) {
  give(owner, {{kPermitItem, 60}}, {{kFacilityItem, {1, 0}}});
  ASSERT_EQ(rent_small(owner).ec, x5::ec::Success);
  enter_edit_mode(owner);

  DoodadRecord station;
  station.house_id = 1;
  station.doodad_id = 11;
  station.cms_id = kStationDoodad;
  station.position = {300, 400, 0};
  UpdateRequest place;
  place.doodads = {station};
  const auto placed = update(owner, place);
  ASSERT_EQ(placed.ec, x5::ec::Success);
  ASSERT_EQ(placed.outcome.site_updates.front().doodads.size(), 1U);
  EXPECT_TRUE(placed.outcome.site_updates.front().doodads.front().from_storage);
  EXPECT_EQ(progress(owner).housing_items.at(kFacilityItem), (HousingItemRecord{1, 1}));
  ASSERT_TRUE(service->personal_site(owner.character_id)->contents.doodads.contains({1, 11}));

  // A second station needs a second stored copy.
  station.doodad_id = 12;
  UpdateRequest second;
  second.doodads = {station};
  EXPECT_EQ(update(owner, second).ec, x5::ec::ItemError);

  UpdateRequest remove;
  remove.remove_doodad_unit_ids = {housing_doodad_unit_id(kSmallLot, 1, 11)};
  ASSERT_EQ(update(owner, remove).ec, x5::ec::Success);
  EXPECT_EQ(progress(owner).housing_items.at(kFacilityItem), (HousingItemRecord{1, 0}));
  EXPECT_FALSE(service->personal_site(owner.character_id)->contents.doodads.contains({1, 11}));
}

TEST_F(HousingServiceTest, MovingThePresetMainBuildingKeepsIt) {
  give(owner, {{kPermitItem, 60}});
  ASSERT_EQ(rent_small(owner).ec, x5::ec::Success);
  enter_edit_mode(owner);

  auto moved = service->personal_site(owner.character_id)->contents.houses.at(1);
  moved.position = {1000, -500, 0};
  moved.yaw = 90.0F;
  UpdateRequest request;
  request.houses = {moved};
  ASSERT_EQ(update(owner, request).ec, x5::ec::Success);

  const auto &stored = service->personal_site(owner.character_id)->contents.houses.at(1);
  EXPECT_EQ(stored.position, (LocalPosition{1000, -500, 0}));
  EXPECT_FLOAT_EQ(stored.yaw, 90.0F);
  EXPECT_EQ(stored.attach_index, kUnattachedHouseIndex);
  EXPECT_FALSE(stored.from_storage);
}

TEST_F(HousingServiceTest, UpgradesConsumeMaterialsAndBecomeOwned) {
  give(owner, {{kPermitItem, 60}, {kUpgradeMaterial, 25}});
  ASSERT_EQ(rent_small(owner).ec, x5::ec::Success);
  ASSERT_EQ(service->edit_mode(owner, EditMode::upgrade, false, kZoneKey, 1).ec,
            x5::ec::Success);

  auto upgraded = service->personal_site(owner.character_id)->contents.houses.at(1);
  upgraded.housing_id = kUpgradedBuilding;
  UpdateRequest request;
  request.houses = {upgraded};
  const auto result = update(owner, request);
  ASSERT_EQ(result.ec, x5::ec::Success);
  EXPECT_EQ(progress(owner).stackables.at(kUpgradeMaterial), 5);
  EXPECT_EQ(progress(owner).housing_items.at(kUpgradedBuildingItem), (HousingItemRecord{1, 1}));
  EXPECT_TRUE(service->personal_site(owner.character_id)->contents.houses.at(1).from_storage);

  // No upgrade row leads back down.
  auto downgraded = upgraded;
  downgraded.housing_id = kMainBuilding;
  UpdateRequest down;
  down.houses = {downgraded};
  EXPECT_EQ(update(owner, down).ec, x5::ec::Housing_NotAvailableUpgradeBuilding);
}

TEST_F(HousingServiceTest, CustomizationAndObjectsAreAnnouncedSeparately) {
  give(owner, {{kPermitItem, 60}}, {{kChairItem, {1, 0}}});
  ASSERT_EQ(rent_small(owner).ec, x5::ec::Success);
  enter_edit_mode(owner);

  UpdateRequest request;
  request.house_part_custom_ids = std::string("\x01\x02\x03", 3);
  request.doodads = {chair(10)};
  const auto result = update(owner, request);
  ASSERT_EQ(result.ec, x5::ec::Success);
  ASSERT_EQ(result.outcome.site_updates.size(), 2U);
  EXPECT_TRUE(result.outcome.site_updates[0].custom_ids_changed);
  EXPECT_TRUE(result.outcome.site_updates[0].doodads.empty());
  EXPECT_FALSE(result.outcome.site_updates[1].custom_ids_changed);
  EXPECT_EQ(result.outcome.site_updates[1].doodads.size(), 1U);
}

TEST_F(HousingServiceTest, ExpiredMonthlySitesArePackedWithOwnedObjectsOnly) {
  give(owner, {{kPermitItem, 60}}, {{kChairItem, {1, 0}}});
  ASSERT_EQ(rent_small(owner).ec, x5::ec::Success);
  enter_edit_mode(owner);
  UpdateRequest place;
  place.doodads = {chair(10)};
  ASSERT_EQ(update(owner, place).ec, x5::ec::Success);

  now = kStartTicks + 31 * kEpochTicksPerDay;
  const auto due = service->due_sites();
  ASSERT_EQ(due.size(), 1U);
  const auto expired = service->expire_site(due.front().site, nullptr);
  ASSERT_EQ(expired.ec, x5::ec::Success);
  ASSERT_EQ(expired.outcome.packages_created.size(), 1U);
  EXPECT_EQ(expired.outcome.packages_created.front().reason, PackageReason::lease_expired);
  ASSERT_EQ(expired.outcome.sites_removed.size(), 1U);
  EXPECT_FALSE(service->personal_site(owner.character_id).has_value());
  // The packed chair left storage; recovering the package returns it.
  EXPECT_EQ(progress(owner).housing_items.at(kChairItem), (HousingItemRecord{0, 0}));

  const auto packages = service->packages(owner);
  ASSERT_EQ(packages.packages.size(), 1U);
  EXPECT_EQ(packages.packages.front().original_lot_number, kSmallLot);
  EXPECT_EQ(packages.packages.front().site_type, SiteType::small);

  const auto recovered = service->recover_to_storage(owner, {}, true, nullptr);
  ASSERT_EQ(recovered.ec, x5::ec::Success);
  EXPECT_EQ(progress(owner).housing_items.at(kChairItem), (HousingItemRecord{1, 0}));
  EXPECT_TRUE(service->packages(owner).packages.empty());
}

TEST_F(HousingServiceTest, PeriodicSitesRenewWhileRentItemsLast) {
  give(owner, {{kPermitItem, 130}});
  ASSERT_EQ(
      service->contract(owner, SiteType::small, kZoneKey, ContractType::periodic, nullptr).ec,
      x5::ec::Success);

  now = kStartTicks + 30 * kEpochTicksPerDay;
  ASSERT_EQ(service->expire_site(service->due_sites().front().site, nullptr).ec,
            x5::ec::Success);
  EXPECT_EQ(service->personal_site(owner.character_id)->expiry_epoch_ticks,
            kStartTicks + 60 * kEpochTicksPerDay);
  EXPECT_EQ(progress(owner).stackables.at(kPermitItem), 10);

  now = kStartTicks + 60 * kEpochTicksPerDay;
  ASSERT_EQ(service->expire_site(service->due_sites().front().site, nullptr).ec,
            x5::ec::Success);
  EXPECT_FALSE(service->personal_site(owner.character_id).has_value());
}

TEST_F(HousingServiceTest, PeriodicSiteRenewalConflictDoesNotPackTheSite) {
  give(owner, {{kPermitItem, 130}});
  ASSERT_EQ(service->contract(owner, SiteType::small, kZoneKey, ContractType::periodic, nullptr).ec,
            x5::ec::Success);
  const auto site = service->personal_site(owner.character_id);
  ASSERT_TRUE(site);
  const auto stale = progress(owner);
  auto newer = stale;
  ++newer.revision;
  ASSERT_TRUE(repositories.character_state->save_world_progress(
      owner.account_id, owner.character_id, kWorldId, stale, newer));
  ProgressAccess access;
  access.character_id = owner.character_id;
  access.load = [stale] { return stale; };
  access.adopt = [](const WorldProgress &) {};
  now = kStartTicks + 30 * kEpochTicksPerDay;

  const auto result = service->expire_site(site->key, &access);
  EXPECT_EQ(result.ec, x5::ec::Housing_RetryContract);
  ASSERT_TRUE(service->personal_site(owner.character_id));
  EXPECT_EQ(service->personal_site(owner.character_id)->version, site->version);
  EXPECT_EQ(progress(owner), newer);
  EXPECT_TRUE(result.outcome.sites_removed.empty());
  EXPECT_TRUE(result.outcome.packages_created.empty());
}

TEST_F(HousingServiceTest, PeriodicGuildRenewalConflictDoesNotPackTheArea) {
  const GuildId guild_id = 55;
  guilds->memberships[owner.character_id] = {guild_id, "Builders", GuildGrade::master, 1, 10};
  guilds->memberships[visitor.character_id] = {guild_id, "Builders", GuildGrade::member, 1, 10};
  guilds->roster = {{owner.character_id, owner.account_id, "Builder", GuildGrade::master, 3},
                    {visitor.character_id, visitor.account_id, "Visitor", GuildGrade::member, 3}};
  give(owner, {{kPermitItem, 6000}}, {}, 5000);
  const auto area = service->guild_contract(owner, guild_id, kZoneKey, ContractType::periodic,
                                             "Harbor", nullptr);
  ASSERT_EQ(area.ec, x5::ec::Success);
  GuildResident resident;
  resident.lot_number = kSmallLot;
  resident.character_id = visitor.character_id;
  ASSERT_EQ(service->guild_allow(owner, guild_id, {resident}, {}).ec, x5::ec::Success);
  const auto stale = progress(owner);
  auto newer = stale;
  ++newer.revision;
  ASSERT_TRUE(repositories.character_state->save_world_progress(
      owner.account_id, owner.character_id, kWorldId, stale, newer));
  ProgressAccess access;
  access.character_id = owner.character_id;
  access.load = [stale] { return stale; };
  access.adopt = [](const WorldProgress &) {};
  now = kStartTicks + 31 * kEpochTicksPerDay;

  const auto result = service->expire_area(kZoneKey, area.channel, &access);
  EXPECT_EQ(result.ec, x5::ec::Housing_RetryContract);
  ASSERT_TRUE(service->guild_area(guild_id));
  EXPECT_TRUE(service->guild_site_of(visitor.character_id));
  EXPECT_EQ(progress(owner), newer);
  EXPECT_TRUE(result.outcome.sites_removed.empty());
  EXPECT_TRUE(result.outcome.guild_areas.empty());
}

TEST_F(HousingServiceTest, UnpackRestoresAPackageOnANewSite) {
  give(owner, {{kPermitItem, 120}}, {{kChairItem, {1, 0}}});
  ASSERT_EQ(rent_small(owner).ec, x5::ec::Success);
  enter_edit_mode(owner);
  UpdateRequest place;
  place.doodads = {chair(10)};
  ASSERT_EQ(update(owner, place).ec, x5::ec::Success);
  ASSERT_EQ(service->release(owner, PackageReason::lease_expired, nullptr).ec, x5::ec::Success);
  const auto package_id = service->packages(owner).packages.front().package_id;

  EXPECT_EQ(service->unpack(owner, package_id, kSmallLot, nullptr).ec,
            x5::ec::Housing_MovePackageNoHousingSite);
  ASSERT_EQ(rent_small(owner).ec, x5::ec::Success);
  EXPECT_EQ(service->unpack(visitor, package_id, kSmallLot, nullptr).ec,
            x5::ec::Housing_MovePackageInvalidPackage);

  const auto unpacked = service->unpack(owner, package_id, kSmallLot, nullptr);
  ASSERT_EQ(unpacked.ec, x5::ec::Success);
  EXPECT_EQ(progress(owner).housing_items.at(kChairItem), (HousingItemRecord{1, 1}));
  const auto site = service->personal_site(owner.character_id);
  ASSERT_TRUE(site->contents.doodads.contains({0, 10}));
  // The package replaced the preset content.
  EXPECT_FALSE(site->contents.doodads.contains({0, 1}));
  EXPECT_TRUE(service->packages(owner).packages.empty());
}

TEST_F(HousingServiceTest, DiscardRemovesOnlyUninstalledDiscardableItems) {
  give(owner, {}, {{kFarmItem, {3, 1}}, {kChairItem, {1, 0}}});

  EXPECT_EQ(service->discard_item(owner, kChairItem, 1, nullptr).ec, x5::ec::ItemError);
  EXPECT_EQ(service->discard_item(owner, kFarmItem, 3, nullptr).ec, x5::ec::ItemError);
  const auto discarded = service->discard_item(owner, kFarmItem, 2, nullptr);
  ASSERT_EQ(discarded.ec, x5::ec::Success);
  EXPECT_EQ(progress(owner).housing_items.at(kFarmItem), (HousingItemRecord{1, 1}));
  ASSERT_EQ(discarded.outcome.progress_changes.size(), 1U);
}

TEST_F(HousingServiceTest, VisitsRecordADestinationAndTheOrigin) {
  give(owner, {{kPermitItem, 60}});
  ASSERT_EQ(rent_small(owner).ec, x5::ec::Success);

  EXPECT_EQ(service->visit(visitor, kZoneKey, 5).ec, x5::ec::Housing_ChannelNotFound);
  const auto visit = service->visit(visitor, kZoneKey, 1);
  ASSERT_EQ(visit.ec, x5::ec::Success);
  ASSERT_TRUE(visit.destination.has_value());
  EXPECT_EQ(visit.destination->map_id, kHousingMapId);
  EXPECT_EQ(visit.destination->spawn.map_id, kHousingMapId);
  ASSERT_TRUE(visit.destination->origin.has_value());
  EXPECT_EQ(visit.destination->origin->map_id, 8000);

  const auto taken = service->take_destination(visitor.character_id, kHousingMapId);
  ASSERT_TRUE(taken.has_value());
  EXPECT_EQ(taken->channel, 1);
  EXPECT_EQ(service->origin_of(visitor.character_id)->map_id, 8000);

  // Going home lands on the owner's own sign, then waits for the cooldown.
  const auto home = service->go_home(owner);
  ASSERT_EQ(home.ec, x5::ec::Success);
  const auto *lot = service->catalog().site(kSmallLot);
  EXPECT_EQ(home.destination->spawn.x_mm, static_cast<int>(std::lround(lot->sign_x_cm * 10)));
  EXPECT_EQ(service->go_home(owner).ec, x5::ec::BadRequest);
  EXPECT_EQ(service->go_home(visitor).ec, x5::ec::Housing_NotRegisteredHousingSite);
}

TEST_F(HousingServiceTest, BoardsListsAndSearchesDescribeChannels) {
  give(owner, {{kPermitItem, 60}});
  ASSERT_EQ(rent_small(owner).ec, x5::ec::Success);

  const auto board = service->board(visitor, kZoneKey, 1);
  ASSERT_EQ(board.ec, x5::ec::Success);
  ASSERT_EQ(board.residents.size(), 1U);
  EXPECT_EQ(board.residents.front().lot_number, kSmallLot);
  EXPECT_EQ(board.residents.front().name, "Builder");

  const auto list = service->channel_list(owner, kZoneKey, AuthorityType::individual);
  ASSERT_EQ(list.entries.size(), 1U);
  EXPECT_TRUE(list.entries.front().is_residence);
  ASSERT_EQ(list.entries.front().assignments.size(), 2U);
  EXPECT_EQ(list.entries.front().assignments[0].site_type, SiteType::small);
  EXPECT_EQ(list.entries.front().assignments[0].assigned, 1);
  EXPECT_EQ(list.entries.front().assignments[0].total, 2);

  const auto found = service->channel_search(visitor, kZoneKey, AuthorityType::individual,
                                             "builder");
  ASSERT_EQ(found.ec, x5::ec::Success);
  EXPECT_EQ(found.entry->channel, 1);
  EXPECT_EQ(
      service->channel_search(visitor, kZoneKey, AuthorityType::individual, "nobody").ec,
      x5::ec::Housing_ChannelNotFound);
}

TEST_F(HousingServiceTest, StateSurvivesAServiceRestart) {
  give(owner, {{kPermitItem, 60}}, {{kChairItem, {1, 0}}});
  ASSERT_EQ(rent_small(owner).ec, x5::ec::Success);
  enter_edit_mode(owner);
  UpdateRequest place;
  place.doodads = {chair(10)};
  ASSERT_EQ(update(owner, place).ec, x5::ec::Success);

  service = make_service();
  const auto site = service->personal_site(owner.character_id);
  ASSERT_TRUE(site.has_value());
  EXPECT_TRUE(site->contents.doodads.contains({0, 10}));
  EXPECT_EQ(site->version, 2);
}

TEST_F(HousingServiceTest, StaleProgressRetriesWithoutChangingState) {
  give(owner, {{kPermitItem, 60}});
  // The session's cached view is one revision behind storage.
  ProgressAccess access;
  access.character_id = owner.character_id;
  access.load = [&] {
    auto snapshot = progress(owner);
    snapshot.revision -= 1;
    return snapshot;
  };
  access.adopt = [](const WorldProgress &) {};

  const auto result =
      service->contract(owner, SiteType::small, kZoneKey, ContractType::monthly, &access);
  EXPECT_EQ(result.ec, x5::ec::Housing_RetryContract);
  EXPECT_FALSE(service->personal_site(owner.character_id).has_value());
  EXPECT_EQ(progress(owner).stackables.at(kPermitItem), 60);
}

TEST_F(HousingServiceTest, SessionAccessAdoptsTheCommittedProgress) {
  give(owner, {{kPermitItem, 60}});
  std::optional<WorldProgress> adopted;
  ProgressAccess access;
  access.character_id = owner.character_id;
  access.load = [&] { return progress(owner); };
  access.adopt = [&](const WorldProgress &committed) { adopted = committed; };

  ASSERT_EQ(
      service->contract(owner, SiteType::small, kZoneKey, ContractType::monthly, &access).ec,
      x5::ec::Success);
  ASSERT_TRUE(adopted.has_value());
  EXPECT_EQ(*adopted, progress(owner));
}

TEST_F(HousingServiceTest, GuildAreasAssignMembersAndExpireTogether) {
  const GuildId guild_id = 55;
  guilds->memberships[owner.character_id] = {guild_id, "Builders", GuildGrade::master, 1, 10};
  guilds->memberships[visitor.character_id] = {guild_id, "Builders", GuildGrade::member, 1, 10};
  // Both members hold company position 3, the level GuildHousingSiteSpositionLv requires.
  guilds->roster = {{owner.character_id, owner.account_id, "Builder", GuildGrade::master, 3},
                    {visitor.character_id, visitor.account_id, "Visitor", GuildGrade::member, 3}};

  EXPECT_EQ(service->guild_contract(visitor, guild_id, kZoneKey, ContractType::monthly, "Harbor",
                                    nullptr)
                .ec,
            x5::ec::Guild_NoPermission);
  give(owner, {{kPermitItem, 3000}}, {}, 5000);
  EXPECT_EQ(
      service->guild_contract(owner, guild_id, kZoneKey, ContractType::monthly, "H", nullptr).ec,
      x5::ec::Guild_ChannelNameLengthInvalid);
  const auto area =
      service->guild_contract(owner, guild_id, kZoneKey, ContractType::monthly, "Harbor", nullptr);
  ASSERT_EQ(area.ec, x5::ec::Success);
  EXPECT_EQ(progress(owner).hersander_token, 0);
  EXPECT_EQ(progress(owner).stackables.at(kPermitItem), 0);
  EXPECT_EQ(
      service->guild_contract(owner, guild_id, kZoneKey, ContractType::monthly, "Again", nullptr)
          .ec,
      x5::ec::Guild_HasHousingContract);

  GuildResident resident;
  resident.lot_number = kSmallLot;
  resident.character_id = visitor.character_id;
  const auto allowed = service->guild_allow(owner, guild_id, {resident}, {});
  ASSERT_EQ(allowed.ec, x5::ec::Success);
  ASSERT_EQ(allowed.residents.size(), 1U);
  const auto guild_site = service->guild_site_of(visitor.character_id);
  ASSERT_TRUE(guild_site.has_value());
  EXPECT_EQ(guild_site->authority, AuthorityType::guild);
  EXPECT_EQ(guild_site->key.channel, area.channel);
  EXPECT_EQ(guild_site->expiry_epoch_ticks, area.expiry_epoch_ticks);

  const auto assignment = service->guild_assignment(owner, guild_id);
  ASSERT_EQ(assignment.characters_with_sites.size(), 1U);
  EXPECT_EQ(assignment.characters_with_sites.front(), visitor.character_id);
  EXPECT_EQ(service->guild_board(visitor, guild_id).residents.size(), 1U);

  // A freed lot keeps its cooldown.
  ASSERT_EQ(service->guild_allow(owner, guild_id, {}, {resident}).ec, x5::ec::Success);
  EXPECT_FALSE(service->guild_site_of(visitor.character_id).has_value());
  EXPECT_EQ(service->guild_allow(owner, guild_id, {resident}, {}).ec,
            x5::ec::Housing_GuildSiteAssignCooldown);
  now += 61 * 60 * kEpochTicksPerSecond;
  ASSERT_EQ(service->guild_allow(owner, guild_id, {resident}, {}).ec, x5::ec::Success);

  now = kStartTicks + 31 * kEpochTicksPerDay;
  const auto due = service->due_areas();
  ASSERT_EQ(due.size(), 1U);
  EXPECT_EQ(due.front().master_character_id, owner.character_id);
  const auto expired = service->expire_area(due.front().zone_key, due.front().channel, nullptr);
  ASSERT_EQ(expired.ec, x5::ec::Success);
  EXPECT_FALSE(service->guild_area(guild_id).has_value());
  EXPECT_FALSE(service->guild_site_of(visitor.character_id).has_value());
}

TEST_F(HousingServiceTest, GuildDepartureReleasesTheLotAndPreservesItsCooldown) {
  const GuildId guild_id = 55;
  guilds->memberships[owner.character_id] = {guild_id, "Builders", GuildGrade::master, 1, 10};
  guilds->memberships[visitor.character_id] = {guild_id, "Builders", GuildGrade::member, 1, 10};
  guilds->roster = {{owner.character_id, owner.account_id, "Builder", GuildGrade::master, 3},
                    {visitor.character_id, visitor.account_id, "Visitor", GuildGrade::member, 3}};
  give(owner, {{kPermitItem, 3000}}, {}, 5000);
  ASSERT_EQ(service->guild_contract(owner, guild_id, kZoneKey, ContractType::monthly, "Harbor",
                                    nullptr).ec,
            x5::ec::Success);
  GuildResident resident;
  resident.lot_number = kSmallLot;
  resident.character_id = visitor.character_id;
  ASSERT_EQ(service->guild_allow(owner, guild_id, {resident}, {}).ec, x5::ec::Success);

  // The departure reaches housing after the guild change has committed.
  guilds->memberships.erase(visitor.character_id);
  const auto result = service->guild_member_left(guild_id, visitor.character_id, nullptr, true);
  ASSERT_EQ(result.ec, x5::ec::Success);
  ASSERT_EQ(result.outcome.guild_sites.size(), 1u);
  EXPECT_EQ(result.outcome.guild_sites.front().action, GuildHousingAction::remove);
  EXPECT_FALSE(service->guild_site_of(visitor.character_id).has_value());

  // The freed lot keeps its reassignment cooldown, even for a member who rejoined.
  guilds->memberships[visitor.character_id] = {guild_id, "Builders", GuildGrade::member, 1, 10};
  EXPECT_EQ(service->guild_allow(owner, guild_id, {resident}, {}).ec,
            x5::ec::Housing_GuildSiteAssignCooldown);
}

TEST_F(HousingServiceTest, LateGuildCleanupNeverReleasesACurrentMembersHousing) {
  const GuildId guild_id = 55;
  guilds->answers_guild_existence = true;
  guilds->memberships[owner.character_id] = {guild_id, "Builders", GuildGrade::master, 1, 10};
  guilds->memberships[visitor.character_id] = {guild_id, "Builders", GuildGrade::member, 1, 10};
  guilds->roster = {{owner.character_id, owner.account_id, "Builder", GuildGrade::master, 3},
                    {visitor.character_id, visitor.account_id, "Visitor", GuildGrade::member, 3}};
  give(owner, {{kPermitItem, 3000}}, {}, 5000);
  ASSERT_EQ(service->guild_contract(owner, guild_id, kZoneKey, ContractType::monthly, "Harbor",
                                    nullptr).ec,
            x5::ec::Success);
  GuildResident resident;
  resident.lot_number = kSmallLot;
  resident.character_id = visitor.character_id;
  ASSERT_EQ(service->guild_allow(owner, guild_id, {resident}, {}).ec, x5::ec::Success);

  // A departure delivered after the character rejoined, or a disband replayed for a guild
  // that exists, changes nothing.
  const auto left = service->guild_member_left(guild_id, visitor.character_id, nullptr);
  EXPECT_EQ(left.ec, x5::ec::Success);
  EXPECT_TRUE(left.outcome.empty());
  EXPECT_TRUE(service->guild_site_of(visitor.character_id).has_value());
  const auto disbanded = service->guild_disbanded(guild_id);
  EXPECT_EQ(disbanded.ec, x5::ec::Success);
  EXPECT_TRUE(disbanded.outcome.empty());
  EXPECT_TRUE(service->guild_area(guild_id).has_value());
  EXPECT_TRUE(service->stale_guild_housing().empty());
}

TEST_F(HousingServiceTest, StaleGuildHousingFollowsTheGuildRegistryAndFailsClosed) {
  const GuildId guild_id = 55;
  guilds->memberships[owner.character_id] = {guild_id, "Builders", GuildGrade::master, 1, 10};
  guilds->memberships[visitor.character_id] = {guild_id, "Builders", GuildGrade::member, 1, 10};
  guilds->roster = {{owner.character_id, owner.account_id, "Builder", GuildGrade::master, 3},
                    {visitor.character_id, visitor.account_id, "Visitor", GuildGrade::member, 3}};
  give(owner, {{kPermitItem, 3000}}, {}, 5000);
  ASSERT_EQ(service->guild_contract(owner, guild_id, kZoneKey, ContractType::monthly, "Harbor",
                                    nullptr).ec,
            x5::ec::Success);
  GuildResident resident;
  resident.lot_number = kSmallLot;
  resident.character_id = visitor.character_id;
  ASSERT_EQ(service->guild_allow(owner, guild_id, {resident}, {}).ec, x5::ec::Success);

  // The visitor left, but its cleanup was lost. A directory that cannot confirm the guild
  // releases nothing.
  guilds->memberships.erase(visitor.character_id);
  EXPECT_TRUE(service->stale_guild_housing().empty());

  guilds->answers_guild_existence = true;
  auto stale = service->stale_guild_housing();
  EXPECT_TRUE(stale.disbanded_guilds.empty());
  ASSERT_EQ(stale.departed_residents.size(), 1u);
  EXPECT_EQ(stale.departed_residents.front().guild_id, guild_id);
  EXPECT_EQ(stale.departed_residents.front().character_id, visitor.character_id);
  ASSERT_EQ(service->guild_member_left(guild_id, visitor.character_id, nullptr).ec,
            x5::ec::Success);
  EXPECT_FALSE(service->guild_site_of(visitor.character_id).has_value());
  EXPECT_TRUE(service->stale_guild_housing().empty());

  // The guild was disbanded and that cleanup was lost too.
  guilds->memberships.erase(owner.character_id);
  stale = service->stale_guild_housing();
  EXPECT_TRUE(stale.departed_residents.empty());
  ASSERT_EQ(stale.disbanded_guilds.size(), 1u);
  EXPECT_EQ(stale.disbanded_guilds.front(), guild_id);
  ASSERT_EQ(service->guild_disbanded(guild_id).ec, x5::ec::Success);
  EXPECT_FALSE(service->guild_area(guild_id).has_value());
  EXPECT_TRUE(service->stale_guild_housing().empty());
}

TEST_F(HousingServiceTest, GameMasterCommandsEditHousingState) {
  give(owner, {{kPermitItem, 60}}, {{kChairItem, {1, 0}}});
  ASSERT_EQ(service->gm_set_unlock(owner, SiteType::medium, false, true).ec, x5::ec::Success);
  const auto info = service->unlock_info(owner, kZoneKey);
  EXPECT_NE(std::ranges::find(info.unlocked_site_types, SiteType::medium),
            info.unlocked_site_types.end());

  ASSERT_EQ(rent_small(owner).ec, x5::ec::Success);
  ASSERT_EQ(service->gm_set_remaining_minutes(owner, 5, ContractType::periodic).ec,
            x5::ec::Success);
  EXPECT_EQ(service->personal_site(owner.character_id)->expiry_epoch_ticks,
            now + 5 * 60 * kEpochTicksPerSecond);

  enter_edit_mode(owner);
  UpdateRequest place;
  place.doodads = {chair(10)};
  ASSERT_EQ(update(owner, place).ec, x5::ec::Success);
  EXPECT_EQ(progress(owner).housing_items.at(kChairItem), (HousingItemRecord{1, 1}));

  // Reset removes the site and keeps the owned chair in storage.
  ASSERT_EQ(service->gm_reset_account(owner, nullptr).ec, x5::ec::Success);
  EXPECT_FALSE(service->personal_site(owner.character_id).has_value());
  EXPECT_EQ(progress(owner).housing_items.at(kChairItem), (HousingItemRecord{1, 0}));

  ASSERT_EQ(rent_small(visitor).ec, x5::ec::Housing_RentItemNotEnough);
  give(visitor, {{kPermitItem, 60}});
  const auto rented = rent_small(visitor);
  ASSERT_EQ(rented.ec, x5::ec::Success);
  const auto released = service->gm_release(rented.lot_number, rented.channel);
  ASSERT_EQ(released.ec, x5::ec::Success);
  EXPECT_FALSE(service->personal_site(visitor.character_id).has_value());
}

TEST_F(HousingServiceTest, HousePartsToggleIndividualBits) {
  give(owner, {{kPermitItem, 60}});
  ASSERT_EQ(rent_small(owner).ec, x5::ec::Success);

  const auto opened = service->set_house_part(visitor, kZoneKey, 1, kSmallLot, 1, 3, true);
  ASSERT_EQ(opened.ec, x5::ec::Success);
  ASSERT_EQ(opened.outcome.parts_updates.size(), 1U);
  EXPECT_EQ(opened.outcome.parts_updates.front().parts_state, std::uint64_t{1} << 3);
  EXPECT_EQ(service->set_house_part(visitor, kZoneKey, 1, kSmallLot, 9, 3, true).ec,
            x5::ec::Housing_HouseNotFound);
  const auto closed = service->set_house_part(visitor, kZoneKey, 1, kSmallLot, 1, 3, false);
  ASSERT_EQ(closed.ec, x5::ec::Success);
  EXPECT_EQ(closed.outcome.parts_updates.front().parts_state, 0U);
}

TEST_F(HousingServiceTest, GardenPlotIsPlantedGrowsAndIsHarvested) {
  const auto placed_at = rent_with_garden();

  // Only the owner gardens; the interaction must exist in the current phase.
  EXPECT_EQ(service->check_doodad_use(visitor, kZoneKey, 1, kGardenPlot, kPlantInteraction,
                                      kPlantAnimation, nullptr)
                .ec,
            x5::ec::Housing_NotOwner);
  EXPECT_EQ(service->check_doodad_use(owner, kZoneKey, 1, kGardenPlot, kHarvestInteraction,
                                      kHarvestAnimation, nullptr)
                .ec,
            x5::ec::Interaction_Invalid);
  const auto check = service->check_doodad_use(owner, kZoneKey, 1, kGardenPlot,
                                               kPlantInteraction, kPlantAnimation, nullptr);
  ASSERT_EQ(check.ec, x5::ec::Success);
  EXPECT_EQ(check.phase, kEmptyPlotPhase);
  ASSERT_NE(check.animation, nullptr);
  EXPECT_EQ(check.animation->execute_time_ms, kPlantExecuteMilliseconds);

  // Planting spends five seeds and starts growth now.
  const auto planted_at = now;
  const auto planted = service->complete_doodad_use(owner, kZoneKey, 1, kGardenPlot,
                                                    kPlantInteraction, kEmptyPlotPhase, nullptr);
  ASSERT_EQ(planted.ec, x5::ec::Success);
  EXPECT_GT(planted_at, placed_at);
  EXPECT_EQ(planted.next_phase, kSownPhase);
  EXPECT_TRUE(planted.rewards.empty());
  EXPECT_EQ(planted.changed_stacks, (std::map<int, int>{{kSeedItem, 2}}));
  EXPECT_EQ(progress(owner).stackables.at(kSeedItem), 2);
  ASSERT_EQ(planted.outcome.doodad_phases.size(), 1U);
  EXPECT_EQ(planted.outcome.doodad_phases.front().phase, kSownPhase);
  EXPECT_EQ(planted.outcome.doodad_phases.front().phase_begin_epoch_ticks, planted_at);

  // A sown plot cannot be planted again; growth is not due yet.
  EXPECT_EQ(service->check_doodad_use(owner, kZoneKey, 1, kGardenPlot, kPlantInteraction,
                                      kPlantAnimation, nullptr)
                .ec,
            x5::ec::Interaction_Invalid);
  EXPECT_TRUE(service->advance_due_doodads().empty());

  // Growth elapses on the authored schedule, counted from the planting rather than the sweep.
  now = planted_at + 3 * kEpochTicksPerSecond;
  const auto grown = service->advance_due_doodads();
  ASSERT_EQ(grown.doodad_phases.size(), 1U);
  EXPECT_EQ(grown.doodad_phases.front().phase, kRipePhase);
  EXPECT_EQ(grown.doodad_phases.front().phase_begin_epoch_ticks,
            planted_at + kGrowthMilliseconds * 10'000LL);

  const auto harvested = service->complete_doodad_use(
      owner, kZoneKey, 1, kGardenPlot, kHarvestInteraction, kRipePhase, nullptr);
  ASSERT_EQ(harvested.ec, x5::ec::Success);
  EXPECT_EQ(harvested.rewards, (std::map<int, int>{{kCropItem, 2}}));
  EXPECT_FALSE(harvested.looting_reason);
  EXPECT_EQ(harvested.next_phase, kRestingPhase);
  EXPECT_EQ(progress(owner).stackables.at(kCropItem), 2);

  // A restart keeps the resting plot, which becomes plantable when its rest ends.
  auto restarted = make_service();
  restarted->set_doodad_profile(
      std::make_shared<const HousingDoodadProfile>(garden_profile_json()), 3);
  const auto site = restarted->personal_site(owner.character_id);
  ASSERT_TRUE(site.has_value());
  EXPECT_EQ(site->contents.doodads.at(DoodadKey{0, kGardenDoodadId}).phase, kRestingPhase);
  now += 3 * kEpochTicksPerSecond;
  const auto rested = restarted->advance_due_doodads();
  ASSERT_EQ(rested.doodad_phases.size(), 1U);
  EXPECT_EQ(rested.doodad_phases.front().phase, kEmptyPlotPhase);
}

TEST_F(HousingServiceTest, GardenRefusesMissingSeedsAndStalePhases) {
  rent_with_garden(garden_profile_json(), 4);
  // Four seeds are fewer than the five a planting spends.
  EXPECT_EQ(service->check_doodad_use(owner, kZoneKey, 1, kGardenPlot, kPlantInteraction,
                                      kPlantAnimation, nullptr)
                .ec,
            x5::ec::Interaction_ConditionsNotMatch);
  EXPECT_EQ(service->complete_doodad_use(owner, kZoneKey, 1, kGardenPlot, kPlantInteraction,
                                         kEmptyPlotPhase, nullptr)
                .ec,
            x5::ec::Interaction_ConditionsNotMatch);
  give(owner, {{kSeedItem, 1}});

  // An execute that expected another phase grants and spends nothing.
  EXPECT_EQ(service->complete_doodad_use(owner, kZoneKey, 1, kGardenPlot, kHarvestInteraction,
                                         kRipePhase, nullptr)
                .ec,
            x5::ec::Interaction_Invalid);
  EXPECT_EQ(progress(owner).stackables.at(kSeedItem), 5);

  // Exactly five seeds plant the plot and leave an empty stack.
  ASSERT_EQ(service->complete_doodad_use(owner, kZoneKey, 1, kGardenPlot, kPlantInteraction,
                                         kEmptyPlotPhase, nullptr)
                .ec,
            x5::ec::Success);
  EXPECT_EQ(progress(owner).stackables.at(kSeedItem), 0);
}

// Every seedling an exported garden plot accepts is refused one seed short of its cost and
// planted with exactly that cost. The synthetic catalog places only plot 160066, so each
// exported plot graph is installed under that placeable doodad ID in turn.
TEST_F(HousingServiceTest, EveryExportedSeedlingPlantsEveryGardenPlot) {
  const auto exported = exported_doodad_profile_json();
  if (!exported) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG and run tools/build_housing_data.py";
  }

  // Plot 160066 takes all ten seedlings; the larger plots 160068 and 160069 take seven.
  const std::map<int, int> expected_plantings_per_plot{{160066, 10}, {160068, 7}, {160069, 7}};
  std::map<int, int> plantings_per_plot;
  std::set<int> planted_seedlings;

  for (const auto &definition : exported->at("doodads")) {
    const int plot_id = definition.at("doodad_id").get<int>();
    if (!expected_plantings_per_plot.contains(plot_id)) {
      continue;
    }

    auto plot = definition;
    plot["doodad_id"] = kFarmDoodad;
    auto profile = *exported;
    profile["doodads"] = nlohmann::json::array({plot});

    for (const auto &phase : definition.at("phases")) {
      if (phase.at("phase_id").get<int>() != kEmptyPlotPhase) {
        continue;
      }

      for (const auto &transition : phase.at("transitions")) {
        if (transition.at("trigger") != "interaction") {
          continue;
        }

        const auto &conditions = transition.at("conditions");
        ASSERT_EQ(conditions.size(), 1U);
        ASSERT_EQ(conditions.front().at("kind"), "consume");
        const int seedling = conditions.front().at("item_cms_id").get<int>();
        const int cost = conditions.front().at("count").get<int>();
        const int interaction = transition.at("interaction_id").get<int>();
        const int animation = transition.at("interaction_anim_id").get<int>();
        const int sown_phase = transition.at("next").at(0).at(0).get<int>();
        SCOPED_TRACE("plot " + std::to_string(plot_id) + " seedling " +
                     std::to_string(seedling));

        rent_with_garden(profile, 0);
        give(owner, {{seedling, cost - 1}});
        EXPECT_EQ(service->check_doodad_use(owner, kZoneKey, 1, kGardenPlot, interaction,
                                            animation, nullptr)
                      .ec,
                  x5::ec::Interaction_ConditionsNotMatch);

        give(owner, {{seedling, 1}});
        const auto check = service->check_doodad_use(owner, kZoneKey, 1, kGardenPlot,
                                                     interaction, animation, nullptr);
        ASSERT_EQ(check.ec, x5::ec::Success);
        ASSERT_NE(check.animation, nullptr);

        const auto planted = service->complete_doodad_use(owner, kZoneKey, 1, kGardenPlot,
                                                          interaction, kEmptyPlotPhase, nullptr);
        ASSERT_EQ(planted.ec, x5::ec::Success);
        EXPECT_EQ(planted.next_phase, sown_phase);
        EXPECT_EQ(progress(owner).stackables.at(seedling), 0);
        ++plantings_per_plot[plot_id];
        planted_seedlings.insert(seedling);

        // Releasing the lot lets the next case rent and place a fresh empty plot.
        ASSERT_EQ(service->gm_reset_account(owner, nullptr).ec, x5::ec::Success);
      }
    }
  }

  EXPECT_EQ(plantings_per_plot, expected_plantings_per_plot);
  EXPECT_EQ(planted_seedlings,
            (std::set<int>{50000, 50001, 50002, 50003, 50004, 50005, 50006, 50007, 50008,
                           50009}));
}

TEST_F(HousingServiceTest, GardenTimersThatElapsedBackToBackChainInOneSweep) {
  // The ripe crop decays after 500 ms into rest, so a long absence passes sown -> ripe ->
  // resting -> empty in one sweep, each phase starting when the previous one ended.
  auto profile = garden_profile_json();
  auto &ripe = profile["doodads"][0]["phases"][2]["transitions"];
  ripe.push_back({{"trigger", "timer"},
                  {"duration_ms", 500},
                  {"next", nlohmann::json::array({nlohmann::json::array({kRestingPhase, 1})})}});
  rent_with_garden(profile);
  const auto planted_at = now;
  ASSERT_EQ(service->complete_doodad_use(owner, kZoneKey, 1, kGardenPlot, kPlantInteraction,
                                         kEmptyPlotPhase, nullptr)
                .ec,
            x5::ec::Success);

  now = planted_at + 60 * kEpochTicksPerSecond;
  const auto swept = service->advance_due_doodads();
  ASSERT_EQ(swept.doodad_phases.size(), 1U);
  EXPECT_EQ(swept.doodad_phases.front().phase, kEmptyPlotPhase);
  const auto rest_ended =
      planted_at + (kGrowthMilliseconds + 500 + kRestMilliseconds) * 10'000LL;
  EXPECT_EQ(swept.doodad_phases.front().phase_begin_epoch_ticks, rest_ended);
  EXPECT_EQ(service->personal_site(owner.character_id)
                ->contents.doodads.at(DoodadKey{0, kGardenDoodadId})
                .phase_begin_tick,
            rest_ended);
}

TEST_F(HousingServiceTest, PhaseInteractionsNeedTheProfile) {
  give(owner, {{kPermitItem, 60}, {kSeedItem, 5}}, {{kFarmItem, {1, 0}}});
  ASSERT_EQ(rent_small(owner).ec, x5::ec::Success);
  enter_edit_mode(owner);
  UpdateRequest place;
  auto plot = chair(kGardenDoodadId);
  plot.cms_id = kFarmDoodad;
  place.doodads = {plot};
  ASSERT_EQ(update(owner, place).ec, x5::ec::Success);

  EXPECT_EQ(service->doodad_profile(), nullptr);
  EXPECT_EQ(service->check_doodad_use(owner, kZoneKey, 1, kGardenPlot, kPlantInteraction,
                                      kPlantAnimation, nullptr)
                .ec,
            x5::ec::Interaction_Invalid);
  EXPECT_TRUE(service->advance_due_doodads().empty());
}

} // namespace
} // namespace chronicles::housing
