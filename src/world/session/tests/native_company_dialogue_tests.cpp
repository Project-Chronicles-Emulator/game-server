// Company dialogue functions on the shared world map over the real TLS frame path.

#include "data/client_catalog.hpp"
#include "world/features/company/company.hpp"
#include "world/features/company/company_dialogues.hpp"
#include "world/session/tests/native_world_test_support.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace chronicles {
namespace {

using namespace native_testing;

constexpr int kDevicas = 1001175;
constexpr int kPermitDeskDialogue = 10500;
constexpr int kBlueSalt = 2;
constexpr std::uint16_t kStartPlayingRequest = 20086;
constexpr std::uint16_t kDialogueStartRequest = 20065;
constexpr std::uint16_t kDialogueStartResponse = 161;
constexpr std::uint16_t kDialogueFunctionRequest = 20067;
constexpr std::uint16_t kRegisterPioneerPermitResponse = 94;
constexpr std::uint16_t kCommonErrorResponse = 2;
constexpr std::int64_t kStartingGold = 100;
// BlueSalt CompanyPosition level 2 (Trainee Operative): pioneerPermitMoneyValue 10 Gold.
constexpr std::int64_t kTraineePermitFee = 10;

struct StaticNpc {
  std::string unit_id;
  WorldPosition position;
};

StaticNpc static_npc(const std::filesystem::path &catalog_path, int npc_cms_id) {
  std::ifstream input(catalog_path.parent_path() / "static-npcs" / "definitions.json");
  if (!input) {
    throw std::runtime_error("The static NPC profile is unavailable");
  }
  const auto document = nlohmann::json::parse(input);
  for (const auto &target : document.at("targets")) {
    if (target.at("map_id") == kOverworldMapId && target.at("npc_cms_id") == npc_cms_id) {
      const auto &position = target.at("position_mm");
      return {target.at("unit_id").get<std::string>(),
              WorldPosition{kOverworldMapId, position.at(0).get<int>(), position.at(1).get<int>(),
                            position.at(2).get<int>(), 0.0F}};
    }
  }
  throw std::runtime_error("The static NPC profile has no target for the requested NPC");
}

nlohmann::json position_json(const WorldPosition &position) {
  return {{"millimeter_x", position.x_mm},
          {"millimeter_y", position.y_mm},
          {"millimeter_z", position.z_mm}};
}

// Live 2026-11-03: a Bluesalt member with an expired permit chose Devicas's "I'd like to get a
// Pioneer's Permit" (dialogue 10500) on map 9012 and nothing happened. The company handler read
// the dialogue speaker and the progress to charge only from the tutorial session, which shared
// maps lack, and Devicas names no company. The selection is now resolved through the
// shared-world dialogue, charged from stored progress, and issued for the member's own company.
TEST(NativeWorld, DevicasRegistersAPioneerPermitThroughTheSharedWorldDialogueOverTls) {
  const auto *catalog_environment = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_environment) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the verified catalog export";
  }

  const std::filesystem::path catalog_path(catalog_environment);
  const auto catalog = std::make_shared<ClientCatalog>(catalog_path);
  const auto devicas = static_npc(catalog_path, kDevicas);
  WorldPosition beside_devicas = devicas.position;
  beside_devicas.x_mm += 1000;

  NpcPlacementSet empty_population;
  empty_population.map_id = kOverworldMapId;
  const auto population = std::make_shared<NpcPopulation>(std::move(empty_population), 1024);
  std::shared_ptr<CompanyService> companies;
  OverworldSessions world(population, {beside_devicas}, catalog, catalog_path, {}, {}, {}, {}, {},
                          {}, [&](MemoryRepositories &repositories) {
                            companies = std::make_shared<CompanyService>(
                                std::make_shared<MemoryCompanyStore>(
                                    repositories.character_state),
                                load_company_rules(*catalog), 1);
                            WorldSocialServices social;
                            social.companies = companies;
                            social.company_dialogues = CompanyDialogueIndex::load(*catalog);
                            return social;
                          });
  ASSERT_TRUE(companies);

  auto &session = world.sessions[0];
  ASSERT_EQ(companies->join(session.character_id, kBlueSalt).error, x5::ec::Success);
  ASSERT_TRUE(companies->add_contribution(session.character_id, 1).has_value());
  ASSERT_FALSE(companies->status(session.character_id).permit_valid);
  {
    const auto before = world.repositories.character_state->load_world_progress(
        session.account_id, session.character_id, 1);
    auto after = before;
    after.revision = before.revision + 1;
    after.gold = kStartingGold;
    ASSERT_TRUE(world.repositories.character_state->save_world_progress(
        session.account_id, session.character_id, 1, before, after));
  }

  world.enter(session);
  VehicleClient client(world, session, 3);
  client.send(kStartPlayingRequest, nlohmann::json::object());

  const auto opened = client.send(kDialogueStartRequest, {{"target_unit_id", devicas.unit_id},
                                                         {"pos", position_json(beside_devicas)},
                                                         {"yaw", 0.0}});
  ASSERT_EQ(frames_of(opened, kDialogueStartResponse).size(), 1U);
  ASSERT_TRUE(frames_of(opened, kCommonErrorResponse).empty());

  const auto registered = client.send(kDialogueFunctionRequest,
                                      {{"dialogue_cms_id", kPermitDeskDialogue},
                                       {"sequence_order", 1},
                                       {"select_order", 2}});
  EXPECT_TRUE(frames_of(registered, kCommonErrorResponse).empty())
      << frames_of(registered, kCommonErrorResponse).front().dump();
  ASSERT_EQ(frames_of(registered, kRegisterPioneerPermitResponse).size(), 1U);

  EXPECT_TRUE(companies->status(session.character_id).permit_valid);
  const auto saved = world.repositories.character_state->load_world_progress(
      session.account_id, session.character_id, 1);
  EXPECT_EQ(saved.gold, kStartingGold - kTraineePermitFee);
}

} // namespace
} // namespace chronicles
