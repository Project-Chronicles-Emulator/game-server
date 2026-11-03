#include "world/features/processing/processing.hpp"
#include "testing/memory_repositories.hpp"
#include "protocol/protocol.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr std::int64_t kStartTicks = 100000000;
constexpr std::int64_t kTicksPerMs = 10000;
constexpr std::int64_t kAccountId = 1;
constexpr int kWorldId = 1;
constexpr int kSharedMap = 9011;
constexpr std::uint32_t kSector = 4767898;

constexpr int kCampfireDoodad = 20251;
constexpr int kSmelterDoodad = 81001;
constexpr int kCampfireInteraction = 10220;
constexpr int kSmeltingInteraction = 602;
constexpr int kCookingAnimation = 60001;
constexpr int kSmeltingAnimation = 60006;
constexpr int kCookingExecuteMs = 2933;

// Captured recipe: 9 x herb and 125 Gold become 5 x item 40101101.
constexpr int kCookingRecipe = 60190101;
constexpr int kHerb = 70102001;
constexpr int kCookedFood = 40101101;
// Synthetic equipment recipe at the smelter, used to check instance grants.
constexpr int kEquipmentRecipe = 60601001;
constexpr int kOre = 70300001;
constexpr int kEquipment = 11102302;
// Listed by the campfire pack but deliberately absent from this minimal recipe profile.
constexpr int kLearnedRecipe = 60203031;

std::uint64_t spot_unit(std::uint32_t local_id) {
  return (static_cast<std::uint64_t>(kSector) << 32) | (static_cast<std::uint64_t>(local_id) << 12) |
         6;
}

const std::uint64_t kCampfire = spot_unit(1);
const std::uint64_t kSmelter = spot_unit(2);
const std::uint64_t kOutsideCampfire = spot_unit(3);

Json processing_profile() {
  Json profile{
      {"format_version", 1},
      {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
      {"maximum_craft_amount", 999},
      {"maps", Json::array({{{"map_id", kSharedMap},
                             {"area_min_mm", {-100000, -100000, -100000}},
                             {"area_max_mm", {100000, 100000, 100000}}}})},
      {"animations", Json::array({{{"interaction_anim_id", kCookingAnimation},
                                   {"execute_time_ms", kCookingExecuteMs},
                                   {"duration_ms", 3000}},
                                  {{"interaction_anim_id", kSmeltingAnimation},
                                   {"execute_time_ms", 2933},
                                   {"duration_ms", 3000}}})},
      {"interactions", Json::array({{{"interaction_id", kSmeltingInteraction},
                                     {"group_id", kSmeltingInteraction},
                                     {"expertise_type", "Smelting"},
                                     {"tier_limit", 4},
                                     {"interaction_anim_id", kSmeltingAnimation}},
                                    {{"interaction_id", kCampfireInteraction},
                                     {"group_id", kCampfireInteraction},
                                     {"expertise_type", "Jewealry"},
                                     {"tier_limit", 4},
                                     {"interaction_anim_id", kCookingAnimation}}})},
      {"stations", Json::array({{{"doodad_cms_id", kCampfireDoodad},
                                 {"interaction_group_id", kCampfireInteraction},
                                 {"craft_ids", {kCookingRecipe, kLearnedRecipe}}},
                                {{"doodad_cms_id", kSmelterDoodad},
                                 {"interaction_group_id", kSmeltingInteraction},
                                 {"craft_ids", {kEquipmentRecipe}}}})},
      {"crafts", Json::array({{{"item_craft_id", kCookingRecipe},
                               {"product_item_cms_id", kCookedFood},
                               {"product_count", 5},
                               {"product_storage", "stackable"},
                               {"expertise_type", "Jewealry"},
                               {"proper_expertise_level", "Lv1"},
                               {"craft_group", "Food"},
                               {"gold_cost", 125},
                               {"materials", Json::array({{{"item_cms_id", kHerb}, {"count", 9}}})}},
                              {{"item_craft_id", kEquipmentRecipe},
                               {"product_item_cms_id", kEquipment},
                               {"product_count", 1},
                               {"product_storage", "instance"},
                               {"product_max_durability", 10000},
                               {"product_grade", "Normal"},
                               {"expertise_type", "Smelting"},
                               {"proper_expertise_level", "Lv1"},
                               {"craft_group", "Metal"},
                               {"gold_cost", 0},
                               {"materials", Json::array({{{"item_cms_id", kOre}, {"count", 2}}})}}})},
      {"unsupported_crafts",
       Json::array({{{"item_craft_id", kLearnedRecipe}, {"reason", "learned"}}})},
      {"placements", Json::array({{std::to_string(kCampfire), kCampfireDoodad, 1000, 0, 0},
                                  {std::to_string(kSmelter), kSmelterDoodad, -1000, 0, 0},
                                  {std::to_string(kOutsideCampfire), kCampfireDoodad, 500000,
                                   0, 0}})}};
  return profile;
}

Json position_json(int x_mm) {
  return {{"millimeter_x", x_mm}, {"millimeter_y", 0}, {"millimeter_z", 0}};
}

Json dialogue_request(std::uint64_t target = kCampfire, int x_mm = 0) {
  return {{"target_unit_id", std::to_string(target)}, {"pos", position_json(x_mm)}, {"yaw", 150.5}};
}

Json craft_request(int recipe = kCookingRecipe, int count = 1, std::uint64_t target = kCampfire,
                   int interaction = kCampfireInteraction, int animation = kCookingAnimation) {
  return {{"interaction_anim_id", animation},
          {"target_unit_id", std::to_string(target)},
          {"tick", "293072785741800"},
          {"pos", position_json(0)},
          {"yaw", 150.48},
          {"param",
           {{"interaction",
             {{"interaction_id", interaction},
              {"execute_craft", {{"item_craft_id", recipe}, {"count", count}}}}}}}};
}

Json execute_request(int animation = kCookingAnimation) {
  return {{"interaction_anim_id", animation}, {"tick", "293075722741800"}};
}

Json end_request(int animation = kCookingAnimation) {
  return {{"end_interaction_anim", {{"interaction_anim_id", animation}, {"pos", position_json(0)}}},
          {"tick", "293075787741800"}};
}

std::string error_code(const std::vector<GameplayPacket> &packets) {
  if (packets.size() != 1 || !packets.front().message.contains("ec")) {
    return "";
  }
  return packets.front().message.at("ec").get<std::string>();
}

void expect_wire(const std::vector<GameplayPacket> &packets) {
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  for (const auto &packet : packets) {
    const auto decoded = protocol.decode(protocol.encode(packet.opcode, packet.message));
    EXPECT_EQ(decoded.at("status"), "decoded") << "opcode " << packet.opcode;
  }
}

class ProcessingTest : public testing::Test {
protected:
  void SetUp() override {
    Character created;
    created.account_id = kAccountId;
    created.name = "Processor";
    character = store.characters->create(created);
    set_progress(2000, {{kHerb, 17}, {kOre, 4}});
  }

  void set_progress(std::int64_t gold, std::map<int, int> stackables) {
    const auto before =
        store.character_state->load_world_progress(kAccountId, character.id, kWorldId);
    auto after = before;
    after.revision = before.revision + 1;
    after.gold = gold;
    for (const auto &[item_cms_id, quantity] : stackables) {
      after.stackables[item_cms_id] = quantity;
    }
    ASSERT_TRUE(store.character_state->save_world_progress(kAccountId, character.id, kWorldId,
                                                           before, after));
  }

  WorldProgress stored() {
    return store.character_state->load_world_progress(kAccountId, character.id, kWorldId);
  }

  ProcessingSession session(ProcessingStationLocator placed_stations = {}) {
    ProcessingPersistence persistence;
    persistence.load_character = [this]() -> std::optional<Character> {
      for (const auto &candidate : store.characters->characters(kAccountId, kWorldId)) {
        if (candidate.id == character.id) {
          return candidate;
        }
      }
      return std::nullopt;
    };
    persistence.load_progress = [this] { return stored(); };
    persistence.commit = [this](const WorldProgress &before, const WorldProgress &after,
                                const std::vector<CharacterItem> &granted)
        -> std::optional<std::vector<CharacterItem>> {
      if (!allow_commit) {
        return std::nullopt;
      }
      ++commits;
      return store.character_state->commit_item_trade(kAccountId, character.id, kWorldId, before,
                                                      after, ItemTrade{granted, {}, {}});
    };
    return ProcessingSession(data, static_cast<std::uint64_t>(character.id),
                             std::move(persistence), std::move(placed_stations));
  }

  std::vector<GameplayPacket> receive(ProcessingSession &crafter, std::uint16_t opcode,
                                      const Json &request, std::int64_t ticks = kStartTicks,
                                      bool other_busy = false) {
    EXPECT_TRUE(crafter.handles(opcode, request, position)) << request.dump();
    return crafter.receive(opcode, request, ++sequence, position, ticks, other_busy);
  }

  std::vector<GameplayPacket> open_and_start(ProcessingSession &crafter,
                                             const Json &request = craft_request()) {
    receive(crafter, kProcessingDialogueStartRequest, dialogue_request());
    return receive(crafter, kProcessingStartInteractionRequest, request);
  }

  std::shared_ptr<const ProcessingData> data =
      std::make_shared<const ProcessingData>(processing_profile());
  MemoryRepositories store;
  Character character;
  WorldPosition position{kSharedMap, 0, 0, 0, 0};
  bool allow_commit = true;
  int commits{};
  std::uint32_t sequence{};
};

TEST_F(ProcessingTest, LearnedRecipeCannotCraftUntilItsDurableUnlockExists) {
  auto profile = processing_profile();
  profile["crafts"][0]["requires_learning"] = true;
  data = std::make_shared<const ProcessingData>(profile);
  auto crafter = session();
  EXPECT_EQ(error_code(open_and_start(crafter)), "Craft_SkillNotLearn");
  EXPECT_EQ(commits, 0);

  const auto before = stored();
  auto learned = before;
  ++learned.revision;
  learned.learned_crafts.insert(kCookingRecipe);
  ASSERT_TRUE(store.character_state->save_world_progress(kAccountId, character.id, kWorldId, before,
                                                         learned));
  auto reconnected = session();
  ASSERT_EQ(error_code(open_and_start(reconnected)), "");
  const auto result =
      receive(reconnected, kProcessingExecuteInteractionRequest, execute_request(kCookingAnimation),
              kStartTicks + kCookingExecuteMs * kTicksPerMs);
  EXPECT_EQ(error_code(result), "");
  EXPECT_EQ(commits, 1);
  EXPECT_EQ(stored().stackables.at(kCookedFood), 5);
  EXPECT_TRUE(stored().learned_crafts.contains(kCookingRecipe));
  expect_wire(result);
}

TEST_F(ProcessingTest, HousingConstructionConsumesBlueprintsAndPreservesInstalledCount) {
  constexpr int kBlueprint = 20001002;
  constexpr int kBuilding = 2000001002;
  auto profile = processing_profile();
  auto &recipe = profile["crafts"][1];
  recipe["product_item_cms_id"] = kBuilding;
  recipe["product_storage"] = "housing";
  recipe["materials"].push_back({{"item_cms_id", kBlueprint}, {"count", 1}});
  ASSERT_NO_THROW(data = std::make_shared<const ProcessingData>(profile));
  set_progress(2000, {{kBlueprint, 1}});

  const auto before = stored();
  auto stocked = before;
  ++stocked.revision;
  stocked.housing_items[kBuilding] = {3, 2};
  ASSERT_TRUE(store.character_state->save_world_progress(kAccountId, character.id, kWorldId, before,
                                                         stocked));

  auto crafter = session();
  receive(crafter, kProcessingDialogueStartRequest, dialogue_request(kSmelter));
  const auto start =
      craft_request(kEquipmentRecipe, 1, kSmelter, kSmeltingInteraction, kSmeltingAnimation);
  ASSERT_EQ(error_code(receive(crafter, kProcessingStartInteractionRequest, start)), "");
  EXPECT_EQ(stored().stackables.at(kBlueprint), 1);

  const auto result =
      receive(crafter, kProcessingExecuteInteractionRequest, execute_request(kSmeltingAnimation),
              kStartTicks + kCookingExecuteMs * kTicksPerMs);
  ASSERT_EQ(result.size(), 3U);
  const auto progress = stored();
  EXPECT_EQ(progress.stackables.at(kBlueprint), 0);
  EXPECT_EQ(progress.housing_items.at(kBuilding).quantity, 4);
  EXPECT_EQ(progress.housing_items.at(kBuilding).installed_count, 2);
  // Four owned, two placed: the client is told the two copies still in storage.
  EXPECT_EQ(
      result[0].message.at("housing_items"),
      Json::array({{{"item_cms_id", kBuilding}, {"quantity", 2}, {"housing_installed_count", 2}}}));
  EXPECT_FALSE(result[0].message.contains("items"));
  expect_wire(result);
}

TEST_F(ProcessingTest, HousingConstructionRefusesOverflowWithoutConsumingMaterials) {
  constexpr int kBuilding = 2000001002;
  auto profile = processing_profile();
  profile["crafts"][1]["product_storage"] = "housing";
  profile["crafts"][1]["product_item_cms_id"] = kBuilding;
  data = std::make_shared<const ProcessingData>(profile);
  const auto before = stored();
  auto full = before;
  ++full.revision;
  full.housing_items[kBuilding] = {std::numeric_limits<int>::max(), 0};
  ASSERT_TRUE(
      store.character_state->save_world_progress(kAccountId, character.id, kWorldId, before, full));

  auto crafter = session();
  receive(crafter, kProcessingDialogueStartRequest, dialogue_request(kSmelter));
  const auto response = receive(
      crafter, kProcessingStartInteractionRequest,
      craft_request(kEquipmentRecipe, 1, kSmelter, kSmeltingInteraction, kSmeltingAnimation));
  EXPECT_EQ(error_code(response), "ItemCountError");
  EXPECT_EQ(stored().stackables.at(kOre), 4);
  EXPECT_EQ(stored().housing_items.at(kBuilding).quantity, std::numeric_limits<int>::max());
  EXPECT_EQ(commits, 0);
}

TEST_F(ProcessingTest, CapturedCampfireCookingConsumesMaterialsAndGoldAtTheDeadline) {
  auto crafter = session();

  const auto opened = receive(crafter, kProcessingDialogueStartRequest, dialogue_request());
  ASSERT_EQ(opened.size(), 1U);
  EXPECT_EQ(opened[0].opcode, 161);
  EXPECT_EQ(opened[0].message.at("target_unit_id"), std::to_string(kCampfire));
  EXPECT_EQ(opened[0].message.at("attach_pos"), position_json(0));
  EXPECT_FALSE(opened[0].message.contains("ec"));

  const auto started = receive(crafter, kProcessingStartInteractionRequest, craft_request());
  ASSERT_EQ(started.size(), 1U);
  EXPECT_EQ(started[0].opcode, 166);
  EXPECT_EQ(started[0].message.at("interaction_anim_id"), kCookingAnimation);
  EXPECT_EQ(started[0].message.at("param"), craft_request().at("param"));
  EXPECT_TRUE(crafter.busy());

  const auto executed =
      receive(crafter, kProcessingExecuteInteractionRequest, execute_request(),
              kStartTicks + kCookingExecuteMs * kTicksPerMs);
  ASSERT_EQ(executed.size(), 3U);
  const auto &update = executed[0].message;
  EXPECT_EQ(executed[0].opcode, 169);
  EXPECT_TRUE(update.contains("craft_action"));
  EXPECT_EQ(update.at("stackables"),
            Json::array({{{"item_cms_id", kCookedFood}, {"quantity", 5}},
                         {{"item_cms_id", kHerb}, {"quantity", 8}}}));
  EXPECT_EQ(update.at("money_changes"),
            Json::array({{{"change", "-125"}, {"result", "1875"}}}));
  EXPECT_EQ(executed[1].opcode, kProcessingCraftResultNotification);
  EXPECT_EQ(executed[1].message.at("item_craft_id"), kCookingRecipe);
  EXPECT_EQ(executed[1].message.at("items"),
            Json::array({{{"item_cms_id", kCookedFood}, {"quantity", 5}}}));
  EXPECT_EQ(executed[2].opcode, 167);
  EXPECT_FALSE(crafter.busy());

  const auto progress = stored();
  EXPECT_EQ(progress.gold, 1875);
  EXPECT_EQ(progress.stackables.at(kHerb), 8);
  EXPECT_EQ(progress.stackables.at(kCookedFood), 5);

  const auto ended = receive(crafter, kProcessingEndInteractionRequest, end_request(),
                             kStartTicks + 3000 * kTicksPerMs);
  ASSERT_EQ(ended.size(), 1U);
  EXPECT_EQ(ended[0].opcode, 167);

  const auto closed = receive(crafter, kProcessingDialogueEndRequest, Json::object());
  ASSERT_EQ(closed.size(), 1U);
  EXPECT_EQ(closed[0].opcode, 162);
  EXPECT_FALSE(crafter.handles(kProcessingDialogueEndRequest, Json::object(), position));

  std::vector<GameplayPacket> all = opened;
  all.insert(all.end(), started.begin(), started.end());
  all.insert(all.end(), executed.begin(), executed.end());
  all.insert(all.end(), ended.begin(), ended.end());
  all.insert(all.end(), closed.begin(), closed.end());
  expect_wire(all);
}

TEST_F(ProcessingTest, EarlyExecuteIsRejectedAndRetryNeverCraftsTwice) {
  auto crafter = session();
  open_and_start(crafter);

  const auto early = receive(crafter, kProcessingExecuteInteractionRequest, execute_request(),
                             kStartTicks + 2000 * kTicksPerMs);
  EXPECT_EQ(error_code(early), "InteractionAnim_TimeNotMatch");
  EXPECT_EQ(early[0].opcode, 2);
  EXPECT_EQ(commits, 0);

  // The 100 ms polling allowance before the montage's execute notify is accepted.
  const auto executed = receive(crafter, kProcessingExecuteInteractionRequest, execute_request(),
                                kStartTicks + (kCookingExecuteMs - 100) * kTicksPerMs);
  EXPECT_EQ(executed.size(), 3U);

  const auto retried = receive(crafter, kProcessingExecuteInteractionRequest, execute_request(),
                               kStartTicks + 3000 * kTicksPerMs);
  ASSERT_EQ(retried.size(), 1U);
  EXPECT_EQ(retried[0].opcode, 167);
  EXPECT_EQ(commits, 1);
  EXPECT_EQ(stored().stackables.at(kCookedFood), 5);
  expect_wire(early);
}

TEST_F(ProcessingTest, CraftRequiresTheOpenStationDialogue) {
  auto crafter = session();
  const auto refused = receive(crafter, kProcessingStartInteractionRequest, craft_request());
  EXPECT_EQ(error_code(refused), "Craft_InvalidCraftingTable");
  EXPECT_EQ(refused[0].opcode, 165);
  EXPECT_FALSE(crafter.busy());

  // Crafting at the smelter after opening the campfire is also refused.
  receive(crafter, kProcessingDialogueStartRequest, dialogue_request());
  const auto other_station =
      receive(crafter, kProcessingStartInteractionRequest,
              craft_request(kEquipmentRecipe, 1, kSmelter, kSmeltingInteraction,
                            kSmeltingAnimation));
  EXPECT_EQ(error_code(other_station), "Craft_InvalidCraftingTable");
  expect_wire(refused);
}

TEST_F(ProcessingTest, RejectsRecipesInteractionsAndAnimationsTheStationDoesNotOffer) {
  auto crafter = session();
  receive(crafter, kProcessingDialogueStartRequest, dialogue_request());
  const auto expect_start_error = [&](const Json &request, const std::string &code) {
    const auto outcome = receive(crafter, kProcessingStartInteractionRequest, request);
    EXPECT_EQ(error_code(outcome), code) << request.dump();
    EXPECT_FALSE(crafter.busy());
  };

  expect_start_error(craft_request(kEquipmentRecipe), "Craft_CraftPackNotMatch");
  expect_start_error(craft_request(kLearnedRecipe), "Craft_Restricted");
  expect_start_error(craft_request(kCookingRecipe, 1, kCampfire, kSmeltingInteraction),
                     "Interaction_GroupIdNotMatch");
  expect_start_error(
      craft_request(kCookingRecipe, 1, kCampfire, kCampfireInteraction, kSmeltingAnimation),
      "InteractionAnim_Mismatch");
  expect_start_error(craft_request(kCookingRecipe, 1, spot_unit(99)),
                     "Craft_NotFoundCraftingTable");
  expect_start_error(craft_request(kCookingRecipe, 0), "Craft_InvalidReqCountNumber");
  expect_start_error(craft_request(kCookingRecipe, 1000), "Craft_InvalidReqCountNumber");

  auto with_material_items = craft_request();
  with_material_items["param"]["interaction"]["execute_craft"]["material_item_ids"] =
      Json::array({"42"});
  expect_start_error(with_material_items, "Craft_WrongAddMaterial");
  EXPECT_EQ(commits, 0);
}

TEST_F(ProcessingTest, MissingMaterialsOrGoldAreReportedAtStart) {
  auto crafter = session();
  receive(crafter, kProcessingDialogueStartRequest, dialogue_request());

  const auto two_batches =
      receive(crafter, kProcessingStartInteractionRequest, craft_request(kCookingRecipe, 2));
  EXPECT_EQ(error_code(two_batches), "Craft_MaterialNotEnough");

  set_progress(100, {});
  const auto poor = receive(crafter, kProcessingStartInteractionRequest, craft_request());
  EXPECT_EQ(error_code(poor), "MoneyNotEnough");
  EXPECT_FALSE(crafter.busy());
}

TEST_F(ProcessingTest, CountMultipliesCostMaterialsAndProducts) {
  set_progress(2000, {{kHerb, 30}});
  auto crafter = session();
  open_and_start(crafter, craft_request(kCookingRecipe, 3));

  const auto executed = receive(crafter, kProcessingExecuteInteractionRequest, execute_request(),
                                kStartTicks + kCookingExecuteMs * kTicksPerMs);
  ASSERT_EQ(executed.size(), 3U);
  EXPECT_EQ(executed[1].message.at("items")[0].at("quantity"), 15);
  const auto progress = stored();
  EXPECT_EQ(progress.gold, 2000 - 3 * 125);
  EXPECT_EQ(progress.stackables.at(kHerb), 3);
  EXPECT_EQ(progress.stackables.at(kCookedFood), 15);
}

TEST_F(ProcessingTest, MaterialsSpentBeforeExecuteAbandonTheCraft) {
  auto crafter = session();
  open_and_start(crafter);
  set_progress(2000, {{kHerb, 3}});

  const auto executed = receive(crafter, kProcessingExecuteInteractionRequest, execute_request(),
                                kStartTicks + kCookingExecuteMs * kTicksPerMs);
  EXPECT_EQ(error_code(executed), "Craft_MaterialNotEnough");
  EXPECT_FALSE(crafter.busy());
  EXPECT_FALSE(crafter.handles(kProcessingExecuteInteractionRequest, execute_request(), position));
  EXPECT_EQ(commits, 0);
  EXPECT_EQ(stored().stackables.at(kHerb), 3);
}

TEST_F(ProcessingTest, UtilityBagReservationsAreNotConsumed) {
  const auto before = stored();
  auto after = before;
  after.revision = before.revision + 1;
  after.utility_slots[0] = {kHerb, 10};
  ASSERT_TRUE(store.character_state->save_world_progress(kAccountId, character.id, kWorldId, before,
                                                         after));

  auto crafter = session();
  receive(crafter, kProcessingDialogueStartRequest, dialogue_request());
  const auto refused = receive(crafter, kProcessingStartInteractionRequest, craft_request());
  EXPECT_EQ(error_code(refused), "Craft_MaterialNotEnough");
}

TEST_F(ProcessingTest, EquipmentProductsBecomeNewItemInstances) {
  auto crafter = session();
  receive(crafter, kProcessingDialogueStartRequest, dialogue_request(kSmelter));
  const auto started = receive(
      crafter, kProcessingStartInteractionRequest,
      craft_request(kEquipmentRecipe, 2, kSmelter, kSmeltingInteraction, kSmeltingAnimation));
  ASSERT_EQ(started.size(), 1U);
  EXPECT_EQ(started[0].opcode, 166);

  const auto executed =
      receive(crafter, kProcessingExecuteInteractionRequest, execute_request(kSmeltingAnimation),
              kStartTicks + 2933 * kTicksPerMs);
  ASSERT_EQ(executed.size(), 3U);
  const auto &update = executed[0].message;
  ASSERT_EQ(update.at("items").size(), 2U);
  EXPECT_EQ(update.at("items")[0].at("item_cms_id"), kEquipment);
  EXPECT_EQ(update.at("items")[0].at("durability"), 10000);
  EXPECT_FALSE(update.contains("money_changes"));
  EXPECT_EQ(update.at("stackables"), Json::array({{{"item_cms_id", kOre}, {"quantity", 0}}}));
  EXPECT_EQ(executed[1].message.at("items")[0].at("quantity"), 2);

  const auto owned = store.characters->characters(kAccountId, kWorldId).front().items;
  EXPECT_EQ(std::ranges::count_if(owned, [](const CharacterItem &item) {
              return item.cms_id == kEquipment;
            }),
            2);
  // A depleted material keeps its zero row; storage never deletes progress rows.
  EXPECT_EQ(stored().stackables.at(kOre), 0);
  expect_wire(executed);
}

TEST_F(ProcessingTest, StationsOnOtherMapsOrOutOfRangeAreRejected) {
  auto crafter = session();
  EXPECT_FALSE(crafter.handles(kProcessingDialogueStartRequest,
                               dialogue_request(kOutsideCampfire), position));

  position.map_id = 8000;
  EXPECT_FALSE(crafter.handles(kProcessingDialogueStartRequest, dialogue_request(), position));
  position.map_id = kSharedMap;

  position.x_mm = 9000;
  const auto far = crafter.receive(kProcessingDialogueStartRequest, dialogue_request(kCampfire, 9000),
                                   1, position, kStartTicks, false);
  EXPECT_EQ(error_code(far), "TooFar");

  position.x_mm = 0;
  const auto mismatched = crafter.receive(kProcessingDialogueStartRequest,
                                          dialogue_request(kCampfire, 6000), 2, position,
                                          kStartTicks, false);
  EXPECT_EQ(error_code(mismatched), "Dialogue_InvalidPos");
  expect_wire(far);
}

// A station placed at runtime, such as a Smelting Furnace on a housing lot. The locator names
// where it stands and how far it reaches; the profile still decides whether the doodad is a
// station and which recipes it offers.
TEST_F(ProcessingTest, StationsFromTheLocatorOpenAndCraftLikeMapStations) {
  // Housing doodad unit IDs: lot 104, house 1, doodads 11 and 12.
  constexpr std::uint64_t kPlacedSmelter = (((1ULL << 14) | 11ULL) << 32) | (104ULL << 12) | 5;
  constexpr std::uint64_t kPlacedChair = (((1ULL << 14) | 12ULL) << 32) | (104ULL << 12) | 5;
  constexpr int kChairDoodad = 30001;
  constexpr int kLotRadiusMm = 20000;
  constexpr int kOnTheLotMm = 18000;
  constexpr int kSmeltingExecuteMs = 2933;

  auto crafter = session([](std::uint64_t unit_id,
                            int map_id) -> std::optional<ProcessingPlacement> {
    if (map_id != kSharedMap || (unit_id != kPlacedSmelter && unit_id != kPlacedChair)) {
      return std::nullopt;
    }
    ProcessingPlacement placement;
    placement.unit_id = unit_id;
    placement.doodad_cms_id = unit_id == kPlacedSmelter ? kSmelterDoodad : kChairDoodad;
    placement.range_mm = kLotRadiusMm + kProcessingStationRangeMm;
    return placement;
  });

  // Farther than the ordinary 7 m station allowance, but still on the lot.
  position.x_mm = kOnTheLotMm;
  EXPECT_FALSE(crafter.handles(kProcessingDialogueStartRequest,
                               dialogue_request(kPlacedChair, kOnTheLotMm), position));
  const auto opened = receive(crafter, kProcessingDialogueStartRequest,
                              dialogue_request(kPlacedSmelter, kOnTheLotMm));
  ASSERT_EQ(opened.size(), 1U);
  EXPECT_EQ(opened[0].opcode, 161);
  EXPECT_EQ(opened[0].message.at("target_unit_id"), std::to_string(kPlacedSmelter));
  EXPECT_FALSE(opened[0].message.contains("ec"));

  auto craft = craft_request(kEquipmentRecipe, 1, kPlacedSmelter, kSmeltingInteraction,
                             kSmeltingAnimation);
  craft["pos"] = position_json(kOnTheLotMm);
  const auto started = receive(crafter, kProcessingStartInteractionRequest, craft);
  ASSERT_EQ(started.size(), 1U);
  EXPECT_EQ(started[0].opcode, 166);

  const auto executed =
      receive(crafter, kProcessingExecuteInteractionRequest, execute_request(kSmeltingAnimation),
              kStartTicks + kSmeltingExecuteMs * kTicksPerMs);
  ASSERT_EQ(executed.size(), 3U);
  EXPECT_EQ(executed[1].opcode, kProcessingCraftResultNotification);
  EXPECT_EQ(stored().stackables.at(kOre), 2);
  EXPECT_EQ(commits, 1);
  expect_wire(opened);
  expect_wire(started);
  expect_wire(executed);
}

TEST_F(ProcessingTest, LocatorStationsOutsideTheirReachOrMapAreRejected) {
  constexpr std::uint64_t kPlacedSmelter = (((1ULL << 14) | 11ULL) << 32) | (104ULL << 12) | 5;
  constexpr int kReachMm = 10000;

  auto crafter = session([](std::uint64_t unit_id,
                            int map_id) -> std::optional<ProcessingPlacement> {
    if (map_id != kSharedMap || unit_id != kPlacedSmelter) {
      return std::nullopt;
    }
    ProcessingPlacement placement;
    placement.unit_id = unit_id;
    placement.doodad_cms_id = kSmelterDoodad;
    placement.range_mm = kReachMm;
    return placement;
  });

  position.map_id = 8000;
  EXPECT_FALSE(crafter.handles(kProcessingDialogueStartRequest,
                               dialogue_request(kPlacedSmelter), position));
  position.map_id = kSharedMap;

  position.x_mm = kReachMm + 1000;
  const auto far = crafter.receive(kProcessingDialogueStartRequest,
                                   dialogue_request(kPlacedSmelter, kReachMm + 1000), 1,
                                   position, kStartTicks, false);
  EXPECT_EQ(error_code(far), "TooFar");

  // A session without a locator serves only the profile's map stations.
  auto map_only = session();
  position.x_mm = 0;
  EXPECT_FALSE(map_only.handles(kProcessingDialogueStartRequest,
                                dialogue_request(kPlacedSmelter), position));
}

TEST_F(ProcessingTest, MovingAwayBeforeExecutionPreventsTheCraft) {
  auto crafter = session();
  open_and_start(crafter);
  position.x_mm = 9000;
  const auto executed = receive(crafter, kProcessingExecuteInteractionRequest, execute_request(),
                                kStartTicks + kCookingExecuteMs * kTicksPerMs);
  EXPECT_EQ(error_code(executed), "TooFar");
  EXPECT_EQ(commits, 0);
}

TEST_F(ProcessingTest, FailedCommitChangesNothing) {
  auto crafter = session();
  open_and_start(crafter);
  allow_commit = false;
  const auto executed = receive(crafter, kProcessingExecuteInteractionRequest, execute_request(),
                                kStartTicks + kCookingExecuteMs * kTicksPerMs);
  EXPECT_EQ(error_code(executed), "Craft_FailCraftAction");
  EXPECT_FALSE(crafter.busy());
  EXPECT_EQ(stored().gold, 2000);
  EXPECT_EQ(stored().stackables.at(kHerb), 17);
}

TEST_F(ProcessingTest, OverlappingActionsAreRefused) {
  auto crafter = session();
  receive(crafter, kProcessingDialogueStartRequest, dialogue_request());
  const auto other_busy = receive(crafter, kProcessingStartInteractionRequest, craft_request(),
                                  kStartTicks, true);
  EXPECT_EQ(error_code(other_busy), "InteractionAnim_UsingSkill");

  receive(crafter, kProcessingStartInteractionRequest, craft_request());
  const auto second = receive(crafter, kProcessingStartInteractionRequest, craft_request(),
                              kStartTicks + 100 * kTicksPerMs);
  EXPECT_EQ(error_code(second), "InteractionAnim_UsingSkill");

  const auto replaced = receive(crafter, kProcessingStartInteractionRequest, craft_request(),
                                kStartTicks + 31000 * kTicksPerMs);
  EXPECT_EQ(replaced[0].opcode, 166);
}

TEST_F(ProcessingTest, EndingTheDialogueOrAnimationCancelsAnUnexecutedCraft) {
  auto crafter = session();
  open_and_start(crafter);
  const auto ended = receive(crafter, kProcessingEndInteractionRequest, end_request(),
                             kStartTicks + 500 * kTicksPerMs);
  EXPECT_EQ(ended[0].opcode, 167);
  EXPECT_FALSE(crafter.busy());

  receive(crafter, kProcessingStartInteractionRequest, craft_request());
  receive(crafter, kProcessingDialogueEndRequest, Json::object());
  EXPECT_FALSE(crafter.busy());
  EXPECT_FALSE(crafter.handles(kProcessingExecuteInteractionRequest, execute_request(), position));
  EXPECT_EQ(commits, 0);
}

TEST_F(ProcessingTest, OnlyStationRequestsAreClaimed) {
  auto crafter = session();
  Json plain_interaction = craft_request();
  plain_interaction["param"] = {{"interaction", {{"interaction_id", 20001}}}};
  EXPECT_FALSE(
      crafter.handles(kProcessingStartInteractionRequest, plain_interaction, position));
  EXPECT_FALSE(crafter.handles(kProcessingDialogueStartRequest,
                               dialogue_request(1152921504606851074ULL), position));
  EXPECT_FALSE(crafter.handles(kProcessingDialogueEndRequest, Json::object(), position));
  EXPECT_FALSE(crafter.handles(kProcessingExecuteInteractionRequest, execute_request(), position));
  EXPECT_FALSE(crafter.handles(kProcessingEndInteractionRequest, end_request(), position));
  EXPECT_FALSE(crafter.handles(kProcessingDialogueStartRequest, dialogue_request(),
                               std::nullopt));
}

TEST_F(ProcessingTest, RequestBurstsAreBounded) {
  auto crafter = session();
  std::vector<GameplayPacket> last;
  for (int request = 0; request < 40; ++request) {
    last = crafter.receive(kProcessingStartInteractionRequest, craft_request(), 1, position,
                           kStartTicks, false);
  }
  EXPECT_EQ(error_code(last), "BadRequest");
}

TEST(ProcessingDataTest, RejectsProfilesFromOtherBuildsOrWithDanglingReferences) {
  auto other_build = processing_profile();
  other_build["build_id"] = "0000";
  EXPECT_THROW(ProcessingData{other_build}, std::runtime_error);

  auto unknown_recipe = processing_profile();
  unknown_recipe["stations"][0]["craft_ids"].push_back(424242);
  EXPECT_THROW(ProcessingData{unknown_recipe}, std::runtime_error);

  auto unknown_station = processing_profile();
  unknown_station["placements"].push_back({std::to_string(spot_unit(7)), 99999, 0, 0, 0});
  EXPECT_THROW(ProcessingData{unknown_station}, std::runtime_error);

  auto not_a_spot = processing_profile();
  not_a_spot["placements"][0][0] = std::to_string(static_cast<std::uint64_t>(kSector) << 32);
  EXPECT_THROW(ProcessingData{not_a_spot}, std::runtime_error);

  auto consumes_product = processing_profile();
  consumes_product["crafts"][0]["materials"][0]["item_cms_id"] = kCookedFood;
  EXPECT_THROW(ProcessingData{consumes_product}, std::runtime_error);

  const ProcessingData valid{processing_profile()};
  EXPECT_EQ(valid.placement_count(), 3U);
  EXPECT_EQ(valid.recipe_count(), 2U);
  EXPECT_TRUE(valid.is_unsupported_recipe(kLearnedRecipe));
  EXPECT_EQ(valid.maximum_craft_amount(), 999);
}

TEST(ProcessingDataTest, GeneratedProfileIncludesTheHousingConstructionWorkshop) {
  const auto *profile = std::getenv("CHRONICLES_PROCESSING_DEFINITIONS");
  if (!profile) {
    GTEST_SKIP() << "Set CHRONICLES_PROCESSING_DEFINITIONS to the generated processing profile";
  }
  const auto data = ProcessingData::load(profile);
  constexpr std::uint64_t kConstructionUnit =
      (static_cast<std::uint64_t>(172 | (297 << 14)) << 32) | (2ULL << 12) | 6;
  const auto *placement = data->placement(kConstructionUnit);
  ASSERT_NE(placement, nullptr);
  EXPECT_EQ(placement->doodad_cms_id, 81020);
  EXPECT_TRUE(data->map_contains(20101, *placement));
  const auto *station = data->station(81020);
  ASSERT_NE(station, nullptr);
  EXPECT_TRUE(station->recipe_ids.contains(89000006));
  const auto *recipe = data->recipe(89000006);
  ASSERT_NE(recipe, nullptr);
  EXPECT_EQ(recipe->product_storage, ProcessingProductStorage::housing);
  EXPECT_EQ(recipe->product_item_cms_id, 2000001002);
  EXPECT_EQ(recipe->gold_cost, 24000);
  ASSERT_EQ(recipe->materials.size(), 3U);
  EXPECT_EQ(recipe->materials[0].item_cms_id, 20001002);
}

TEST(ProcessingDataTest, GeneratedProfileResolvesTheCapturedCampfire) {
  const auto *profile = std::getenv("CHRONICLES_PROCESSING_DEFINITIONS");
  if (!profile) {
    GTEST_SKIP() << "Set CHRONICLES_PROCESSING_DEFINITIONS to the generated processing profile";
  }

  const auto data = ProcessingData::load(profile);
  // Sector (154, 291) local spot 1 was the first captured cooking target.
  const auto *campfire = data->placement(kCampfire);
  ASSERT_NE(campfire, nullptr);
  EXPECT_EQ(campfire->doodad_cms_id, kCampfireDoodad);
  EXPECT_TRUE(data->map_contains(kSharedMap, *campfire));

  const auto *station = data->station(kCampfireDoodad);
  ASSERT_NE(station, nullptr);
  EXPECT_EQ(station->interaction_group_id, kCampfireInteraction);
  EXPECT_TRUE(station->recipe_ids.contains(kCookingRecipe));
  EXPECT_EQ(data->interaction(kCampfireInteraction)->animation_id, kCookingAnimation);
  EXPECT_EQ(data->animation(kCookingAnimation)->execute_time_ms, kCookingExecuteMs);

  const auto *recipe = data->recipe(kCookingRecipe);
  ASSERT_NE(recipe, nullptr);
  EXPECT_EQ(recipe->product_item_cms_id, kCookedFood);
  EXPECT_EQ(recipe->product_count, 5);
  EXPECT_EQ(recipe->gold_cost, 125);
  ASSERT_EQ(recipe->materials.size(), 1U);
  EXPECT_EQ(recipe->materials[0].item_cms_id, kHerb);
  EXPECT_EQ(recipe->materials[0].count, 9);
}

} // namespace
} // namespace chronicles
