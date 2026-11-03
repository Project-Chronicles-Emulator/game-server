#pragma once

#include "data/client_catalog.hpp"
#include "world/features/npc/npc_population.hpp"
#include "world/session/tests/native_world_test_support.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace chronicles::vehicle_native_testing {

using namespace native_testing;

// One shared opt-in TLS fixture; moving ownership preserves its original gates.
inline std::ptrdiff_t first_index(const std::vector<DecodedWorldFrame> &frames,
                                  std::uint16_t opcode) {
  const auto found = std::ranges::find_if(frames, [opcode](const DecodedWorldFrame &frame) {
    return frame.opcode == opcode;
  });
  return found == frames.end() ? -1 : found - frames.begin();
}

inline std::string npc_unit_id(const OverworldEntry &entry, int npc_cms_id) {
  for (const auto &add : entry.unit_adds) {
    if (add.at("nub").at("npc").at("npc_cms_id") == npc_cms_id) {
      return add.at("unit_id").get<std::string>();
    }
  }
  throw std::runtime_error("Placement file has no unit for the requested NPC");
}

// The server-owned id of a placed NPC that may lie outside the entry sight. Population ids are
// deterministic for one placement file, so the id loaded here equals the server's.
inline std::string placed_npc_unit_id(int npc_cms_id) {
  const auto population = load_npc_population_file(std::getenv("CHRONICLES_NPC_PLACEMENTS_9012"),
                                                   kMaxPopulatedNpcUnits);
  for (const auto &unit : population->units()) {
    if (unit.placement.npc_cms_id == npc_cms_id) {
      return std::to_string(unit.unit_id);
    }
  }
  throw std::runtime_error("Placement file has no unit for the requested NPC");
}

inline nlohmann::json world_pos_json(const WorldPosition &position) {
  return {{"millimeter_x", position.x_mm},
          {"millimeter_y", position.y_mm},
          {"millimeter_z", position.z_mm}};
}

// Ileste (1001021, trade manager of post 1100) and Vida (1001316, post 1000) on map 9012.
inline const WorldPosition kNearIleste{kOverworldMapId, -1486000, 3371000, -601670, 0.0F};
inline const WorldPosition kNearIlesteWatcher{kOverworldMapId, -1484000, 3371000, -601670, 0.0F};
inline const WorldPosition kPost1000Sphere{kOverworldMapId, -848599, 3700201, -628329, 0.0F};
inline const WorldPosition kNearVida{kOverworldMapId, -854000, 3693000, -628502, 0.0F};

class VehicleWorldFixture {
public:
  static bool available() {
    return std::getenv("CHRONICLES_CLIENT_CATALOG") &&
           std::getenv("CHRONICLES_NPC_PLACEMENTS_9012");
  }

  explicit VehicleWorldFixture(bool enable_combat = false,
                               OverworldSessions::StateWrapper state_wrapper = {})
      : catalog_path_(std::getenv("CHRONICLES_CLIENT_CATALOG")),
        catalog_(std::make_shared<ClientCatalog>(catalog_path_)),
        world_(load_npc_population_file(std::getenv("CHRONICLES_NPC_PLACEMENTS_9012"),
                                        kMaxPopulatedNpcUnits),
               {kNearIleste, kNearIlesteWatcher}, catalog_, catalog_path_, {},
               enable_combat ? CharacterStats::load(*catalog_) : nullptr, {}, {}, {},
               std::move(state_wrapper)) {
    auto &rider = world_.sessions[0];
    const auto before = world_.repositories.character_state->load_world_progress(
        rider.account_id, rider.character_id, 1);
    auto after = before;
    after.revision = before.revision + 1;
    after.gold = 10000;
    after.registered_summons = {2001};
    if (!world_.repositories.character_state->save_world_progress(
            rider.account_id, rider.character_id, 1, before, after)) {
      throw std::runtime_error("Vehicle fixture cannot prepare the rider's progress");
    }
  }

  // Grants a session's account the game-master role before it enters the world. The session
  // logs in again because a role takes effect for newly authenticated bearers only.
  void make_game_master(std::size_t index) {
    auto &session = world_.sessions[index];
    const auto account_name = "overworld_sessions" + std::to_string(index);
    if (!world_.auth->set_account_role(account_name, AccountRole::game_master)) {
      throw std::runtime_error("Vehicle fixture cannot grant the GM role");
    }

    const auto login = world_.auth->login(account_name, "test-world-password-123");
    session.access_token = login.access_token();
    x5::pb::authwebsocket::WorldEnterReq enter_request;
    enter_request.set_world_id(1);
    if (world_.auth->enter(session.access_token, enter_request).error_code() != x5::ec::Success) {
      throw std::runtime_error("Vehicle fixture cannot re-enter the GM session");
    }
  }

  // Full entry plus StartPlayingReq (sequence 3); the next free sequence is 4.
  std::vector<DecodedWorldFrame> start(std::size_t index, OverworldEntry &entry) {
    auto &session = world_.sessions[index];
    entry = world_.enter(session);
    clients_.emplace_back(std::make_unique<VehicleClient>(world_, session, 3));
    return clients_.back()->send(20086, nlohmann::json::object());
  }

  VehicleClient &client(std::size_t index) {
    return *clients_.at(index);
  }

  WorldProgress progress(std::size_t index) {
    const auto &session = world_.sessions[index];
    return world_.repositories.character_state->load_world_progress(session.account_id,
                                                                    session.character_id, 1);
  }

  std::uint64_t player_unit(std::size_t index) const {
    return player_wire_unit_id(world_.sessions[index].character_id);
  }

  NativeWorldListener &listener() {
    return world_.server->listener();
  }

  OverworldSessions &world() {
    return world_;
  }

  // The chat identity of a session's character on the fixture's realm (world 1).
  ChatIdentity identity(std::size_t index) const {
    const auto &session = world_.sessions[index];
    return {1, session.character_id, "Overworld" + std::to_string(index)};
  }

private:
  std::filesystem::path catalog_path_;
  std::shared_ptr<ClientCatalog> catalog_;
  OverworldSessions world_;
  std::vector<std::unique_ptr<VehicleClient>> clients_;
};

} // namespace chronicles::vehicle_native_testing
