#include "world/features/inventory/utility_bag.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr int kPotion = 40101101;
constexpr int kBandage = 15010401;
constexpr int kLeather = 72000016;

const std::unordered_set<int> kEligibleItems{kPotion, kBandage};

WorldProgress owned_progress() {
  WorldProgress progress;
  progress.stackables[kPotion] = 5;
  progress.stackables[kBandage] = 2;
  progress.stackables[kLeather] = 9;
  return progress;
}

Json slot(int index, int item_cms_id, int count) {
  return {{"slot", index}, {"item_cms_id", item_cms_id}, {"count", count}};
}

// Builds {"slots": [...]} explicitly; a one-element brace list could otherwise become a copy of
// that element instead of an array.
Json bag_request(std::vector<Json> slots) {
  Json request = Json::object();
  request["slots"] = Json::array();
  for (auto &entry : slots) {
    request["slots"].push_back(std::move(entry));
  }
  return request;
}

TEST(UtilityBag, AcceptedRequestChangesOnlyTheNamedSlots) {
  auto progress = owned_progress();
  progress.utility_slots[3] = {kBandage, 1};

  const auto request = bag_request({slot(0, kPotion, 3), slot(1, kPotion, 2)});
  EXPECT_EQ(apply_utility_bag_request(request, kEligibleItems, progress), std::nullopt);

  EXPECT_EQ(progress.utility_slots[0], (UtilitySlot{kPotion, 3}));
  EXPECT_EQ(progress.utility_slots[1], (UtilitySlot{kPotion, 2}));
  EXPECT_EQ(progress.utility_slots[2], (UtilitySlot{}));
  EXPECT_EQ(progress.utility_slots[3], (UtilitySlot{kBandage, 1}));
}

TEST(UtilityBag, ProtobufJsonOmittedZeroFieldsClearSlotZero) {
  auto progress = owned_progress();
  progress.utility_slots[0] = {kPotion, 4};

  // An empty UtilityBagSlot for slot 0 serializes with no fields at all.
  const auto request = bag_request({Json::object()});
  EXPECT_EQ(apply_utility_bag_request(request, kEligibleItems, progress), std::nullopt);
  EXPECT_EQ(progress.utility_slots[0], (UtilitySlot{}));
}

TEST(UtilityBag, ReservationsAboveTheOwnedStackAreRefusedAcrossSlots) {
  auto progress = owned_progress();
  const auto before = progress;

  const auto split = bag_request({slot(0, kPotion, 3), slot(2, kPotion, 3)});
  EXPECT_EQ(apply_utility_bag_request(split, kEligibleItems, progress),
            "Interaction_ConditionsNotMatch");
  EXPECT_EQ(progress, before);

  progress.stackables.erase(kPotion);
  const auto without_potions = progress;
  const auto unowned = bag_request({slot(0, kPotion, 1)});
  EXPECT_EQ(apply_utility_bag_request(unowned, kEligibleItems, progress),
            "Interaction_ConditionsNotMatch");
  EXPECT_EQ(progress, without_potions);
}

TEST(UtilityBag, ReassigningAnItemAlreadyInItsSlotTakesTheLastEntry) {
  auto progress = owned_progress();
  progress.utility_slots[0] = {kPotion, 0};
  progress.utility_slots[1] = {kBandage, 1};

  // Client shape when the item is already in the target slot: the slot cleared, its current
  // contents, then the new assignment with the full fill count.
  const auto request =
      bag_request({Json::object(), slot(0, kPotion, 0), slot(0, kPotion, 5)});
  EXPECT_EQ(apply_utility_bag_request(request, kEligibleItems, progress), std::nullopt);

  EXPECT_EQ(progress.utility_slots[0], (UtilitySlot{kPotion, 5}));
  EXPECT_EQ(progress.utility_slots[1], (UtilitySlot{kBandage, 1}));
}

TEST(UtilityBag, RefillUsesOnlyOwnedItemsNotReservedInOtherSlots) {
  auto progress = owned_progress();
  progress.stackables[kPotion] = 3;
  progress.utility_slots[0] = {kPotion, 0};
  progress.utility_slots[1] = {kPotion, 1};

  const auto request =
      bag_request({Json::object(), slot(0, kPotion, 0), slot(0, kPotion, 5)});
  EXPECT_EQ(apply_utility_bag_request(request, kEligibleItems, progress), std::nullopt);
  EXPECT_EQ(progress.utility_slots[0], (UtilitySlot{kPotion, 2}));
  EXPECT_EQ(progress.utility_slots[1], (UtilitySlot{kPotion, 1}));
}

TEST(UtilityBag, MovingAnItemToAnOccupiedSlotClearsItsPreviousSlot) {
  auto progress = owned_progress();
  progress.utility_slots[0] = {kPotion, 2};
  progress.utility_slots[2] = {kBandage, 1};

  // Client shape when the item moves from slot 0 to the occupied slot 2: slot 0 cleared, then
  // slot 2 cleared, restored and assigned.
  const auto request = bag_request({slot(0, 0, 0), slot(2, 0, 0), slot(2, kBandage, 1),
                                    slot(2, kPotion, 5)});
  EXPECT_EQ(apply_utility_bag_request(request, kEligibleItems, progress), std::nullopt);

  EXPECT_EQ(progress.utility_slots[0], (UtilitySlot{}));
  EXPECT_EQ(progress.utility_slots[2], (UtilitySlot{kPotion, 5}));
}

TEST(UtilityBag, OwnershipIsCheckedAgainstTheFinalEntryOfARepeatedSlot) {
  auto progress = owned_progress();
  const auto before = progress;

  // An earlier entry above the owned stack is replaced and does not refuse the request.
  const auto replaced = bag_request({slot(1, kPotion, 9), slot(1, kPotion, 4)});
  EXPECT_EQ(apply_utility_bag_request(replaced, kEligibleItems, progress), std::nullopt);
  EXPECT_EQ(progress.utility_slots[1], (UtilitySlot{kPotion, 4}));

  progress = before;
  const auto final_too_large = bag_request({slot(1, kPotion, 4), slot(1, kPotion, 9)});
  EXPECT_EQ(apply_utility_bag_request(final_too_large, kEligibleItems, progress),
            "Interaction_ConditionsNotMatch");
  EXPECT_EQ(progress, before);
}

TEST(UtilityBag, MalformedOrIneligibleSlotsAreRefusedWithoutChanges) {
  auto progress = owned_progress();
  progress.utility_slots[1] = {kPotion, 1};
  const auto before = progress;

  Json object_slots = Json::object();
  object_slots["slots"] = Json::object();
  const std::vector<Json> refused{
      Json::object(),
      object_slots,
      bag_request({slot(0, kLeather, 1)}),
      bag_request({slot(4, kPotion, 1)}),
      bag_request({slot(-1, kPotion, 1)}),
      bag_request({slot(0, kLeather, 1), slot(0, kPotion, 1)}),
      bag_request({slot(0, kPotion, -1)}),
      bag_request({slot(0, 0, 1)}),
      bag_request({slot(0, -kPotion, 1)}),
      bag_request({Json{{"slot", "0"}, {"item_cms_id", kPotion}}}),
      bag_request({Json{{"slot", 0}, {"item_cms_id", 4294967296LL}}}),
      bag_request({Json(7)}),
      bag_request({slot(0, kPotion, 1), slot(1, kPotion, 1), slot(2, kPotion, 1),
                   slot(3, kPotion, 1), slot(0, kPotion, 1)})};
  for (const auto &request : refused) {
    SCOPED_TRACE(request.dump());
    EXPECT_EQ(apply_utility_bag_request(request, kEligibleItems, progress), "BadRequest");
    EXPECT_EQ(progress, before);
  }
}

TEST(UtilityBag, WireBagListsEverySlotInOrder) {
  auto progress = owned_progress();
  progress.utility_slots[0] = {kPotion, 3};
  progress.utility_slots[2] = {kBandage, 1};

  const auto bag = utility_bag_json(progress);
  EXPECT_EQ(bag.at("item_cms_id1"), kPotion);
  EXPECT_EQ(bag.at("count1"), 3);
  EXPECT_EQ(bag.at("item_cms_id2"), 0);
  EXPECT_EQ(bag.at("count2"), 0);
  EXPECT_EQ(bag.at("item_cms_id3"), kBandage);
  EXPECT_EQ(bag.at("count3"), 1);
  EXPECT_EQ(bag.at("item_cms_id4"), 0);
  EXPECT_EQ(bag.size(), 8u);
}

TEST(UtilityBag, ContentsIncludeAnAssignedSlotWithoutReservedUnits) {
  WorldProgress progress;
  EXPECT_FALSE(has_utility_bag_contents(progress));

  // A slot keeps its assigned item after its last reserved unit was spent.
  progress.utility_slots[3] = {kBandage, 0};
  EXPECT_TRUE(has_utility_bag_contents(progress));
}

TEST(UtilityBag, CatalogEligibilityFollowsTheFillCountReference) {
  const std::vector<Json> rows{
      Json{{"id", std::to_string(kPotion)}, {"FK_utilityBagFullFillCnt", "1000"}},
      Json{{"id", std::to_string(kLeather)}, {"FK_utilityBagFullFillCnt", ""}},
      Json{{"id", std::to_string(kBandage)}}};
  EXPECT_EQ(utility_bag_eligible_items(rows), (std::unordered_set<int>{kPotion}));

  const std::vector<Json> malformed_id{Json{{"id", "x1"}, {"FK_utilityBagFullFillCnt", "1"}}};
  EXPECT_THROW(utility_bag_eligible_items(malformed_id), std::invalid_argument);
  const std::vector<Json> numeric_reference{
      Json{{"id", std::to_string(kPotion)}, {"FK_utilityBagFullFillCnt", 1000}}};
  EXPECT_THROW(utility_bag_eligible_items(numeric_reference), std::invalid_argument);
}

TEST(UtilityBag, SpentUnitsReleaseOnlyUnbackedReservationsInSlotOrder) {
  auto progress = owned_progress();
  progress.utility_slots[0] = {kPotion, 3};
  progress.utility_slots[1] = {kBandage, 2};
  progress.utility_slots[2] = {kPotion, 2};

  // Five owned potions still cover both potion slots.
  EXPECT_FALSE(release_unbacked_utility_units(progress, kPotion));

  progress.stackables[kPotion] = 4;
  EXPECT_TRUE(release_unbacked_utility_units(progress, kPotion));
  EXPECT_EQ(progress.utility_slots[0], (UtilitySlot{kPotion, 3}));
  EXPECT_EQ(progress.utility_slots[2], (UtilitySlot{kPotion, 1}));

  progress.stackables[kPotion] = 0;
  EXPECT_TRUE(release_unbacked_utility_units(progress, kPotion));
  EXPECT_EQ(progress.utility_slots[0], (UtilitySlot{kPotion, 0}));
  EXPECT_EQ(progress.utility_slots[2], (UtilitySlot{kPotion, 0}));
  EXPECT_EQ(progress.utility_slots[1], (UtilitySlot{kBandage, 2}));

  EXPECT_FALSE(release_unbacked_utility_units(progress, 0));
}

} // namespace
} // namespace chronicles
