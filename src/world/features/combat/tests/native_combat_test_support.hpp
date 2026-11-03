#pragma once

#include "world/session/tests/native_world_test_support.hpp"

namespace chronicles::combat_native_testing {

using namespace native_testing;

// Shared only by the unchanged Combat cases and the retained unit-ID case.
class DeathFixture {
public:
  explicit DeathFixture(const char *profile_path, int filler_characters = 0,
                        int standing_distance_mm = 0)
      : definitions_(TutorialData::load(profile_path)),
        catalog_root_(std::filesystem::path(profile_path).parent_path().parent_path() / "catalog"),
        catalog_(std::make_shared<ClientCatalog>(catalog_root_)),
        stats_(CharacterStats::load(*catalog_)),
        auth_(repositories_.character_repositories(), game_config()),
        protocol_(CHRONICLES_PROTOCOL_DIR) {
    EXPECT_TRUE(auth_.create_account("revive_tls", "revive-tutorial-fixture-password"));
    login_ = auth_.login("revive_tls", "revive-tutorial-fixture-password");
    x5::pb::authwebsocket::WorldEnterReq world;
    world.set_world_id(1);
    auth_.enter(login_.access_token(), world);

    const auto spawn = std::ranges::find_if(
        definitions_->document.at("npc_spawns"),
        [](const nlohmann::json &npc) { return npc.at("cms_id") == kDeathMonsterCmsId; });
    EXPECT_TRUE(spawn != definitions_->document.at("npc_spawns").end());
    const auto &monster_position = spawn->at("position");
    // Half of the monster's combat distance, as in the profile-monster combat tests, unless a
    // test asks for an explicit standing distance (the movement test starts outside the
    // engagement distance but inside perception).
    const int standing_distance =
        standing_distance_mm > 0
            ? standing_distance_mm
            : spawn->at("ai").at("combat_distance_max_mm").get<int>() / 2;
    const int standing_x = monster_position.at(0).get<int>() + standing_distance;
    position_ = {{"millimeter_x", standing_x},
                 {"millimeter_y", monster_position.at(1)},
                 {"millimeter_z", monster_position.at(2)}};

    // Filler characters on other accounts move the fixture character to a chosen database id
    // (production's character 19 has wire-unit-type nibble 3 before the mapping).
    for (int filler = 0; filler < filler_characters; ++filler) {
      Character other;
      other.account_id = 100000 + filler;
      other.name = "Filler" + std::to_string(filler);
      other.class_id = 1;
      repositories_.characters->create(other);
    }

    Character character;
    character.account_id = login_.account_id();
    character.name = "ReviveTls";
    character.class_id = 2;
    character.world_position = WorldPosition{8000, standing_x, monster_position.at(1).get<int>(),
                                             monster_position.at(2).get<int>(), 180.0F};
    character_ = repositories_.characters->create(character);
    EXPECT_TRUE(repositories_.characters->save_world_position(character_.account_id, character_.id,
                                                              1, *character_.world_position));
    server_ = std::make_unique<TwoClientWorldServer>(auth_, certificate_, definitions_, stats_,
                                                     catalog_, catalog_root_);
  }

  struct Session {
    std::unique_ptr<WorldTestClient> client;
    std::uint32_t sequence{};
  };

  // Logs in the character again (a reconnect when a session already existed) and walks the
  // bootstrap up to the ping reply, so later reads only see world traffic.
  Session connect() {
    x5::pb::ws::front::CharSelectReq select;
    select.set_char_id(character_.id);
    const auto selected = auth_.select(login_.access_token(), select);
    EXPECT_EQ(selected.packet_key().size(), 32u);
    Session session{std::make_unique<WorldTestClient>(server_->port()), 0};
    session.client->send_key(selected.packet_key());
    EXPECT_EQ(session.client->read().opcode(), 5);
    send(session, 20001, nlohmann::json::object());
    read_until(session, 140);
    send(session, 20085, nlohmann::json::object());
    entry_player_unit_id_ = read_until(session, 176).at("player_unit_id").get<std::uint64_t>();
    send(session, 20003, nlohmann::json::object());
    read_until(session, 4);
    return session;
  }

  void send(Session &session, std::uint16_t opcode, const nlohmann::json &message) {
    session.client->send(protocol_.encode(opcode, message, session.sequence++));
  }

  // Reads frames until `opcode` arrives, appending every decoded frame to `seen`.
  nlohmann::json read_until(Session &session, std::uint16_t opcode,
                            std::vector<nlohmann::json> *seen = nullptr,
                            int frame_bound = 20000) {
    for (int count = 0; count < frame_bound; ++count) {
      auto decoded = protocol_.decode(session.client->read());
      if (seen) {
        seen->push_back(decoded);
      }
      if (decoded.at("opcode") == opcode) {
        return decoded.at("message");
      }
    }
    throw std::runtime_error("Expected frame did not arrive within the frame bound");
  }

  // The monster acquires the player by perception and kills it through the real death path.
  void await_player_death(Session &session) {
    const auto player_text = std::to_string(player_wire_unit_id(character_.id));
    for (int count = 0; count < 20000; ++count) {
      const auto decoded = protocol_.decode(session.client->read());
      if (decoded.at("opcode") == 23 &&
          decoded.at("message").value("target_unit_id", std::string()) == player_text) {
        return;
      }
    }
    throw std::runtime_error("The player never died within the frame bound");
  }

  // Frames the server sent between a request and the ping reply that follows it.
  std::vector<nlohmann::json> frames_until_ping(Session &session) {
    std::vector<nlohmann::json> seen;
    send(session, 20003, nlohmann::json::object());
    read_until(session, 4, &seen);
    return seen;
  }

  static bool is_revival_frame(const nlohmann::json &frame) {
    const auto &message = frame.at("message");
    const auto opcode = frame.at("opcode").get<int>();
    if (opcode == 47) {
      return message.value("default_source", std::string()) == "SCS_Revive";
    }
    if (opcode == 49) {
      return message.at("status_effect").at("status_effect_id") == kSpawnResetStatusId;
    }
    if (opcode == 51) {
      return message.at("status_effects").at(0).at("status_effect_id") == kSpawnResetStatusId;
    }
    return false;
  }

  static bool contains_revival_frame(const std::vector<nlohmann::json> &frames) {
    return std::ranges::any_of(frames, is_revival_frame);
  }

  bool is_player_idle_frame(const nlohmann::json &frame) const {
    const auto &message = frame.at("message");
    return frame.at("opcode") == 32 &&
           message.value("unit_id", std::string()) ==
               std::to_string(player_wire_unit_id(character_.id)) &&
           message.value("state", std::string()) == "EX5UnitStateType_Idle";
  }

  // Sends 20039 to a dead player and returns the revival opcodes (plus the player's idle state)
  // seen up to that idle frame, which must be exactly 49, 51, 47, 32 in that order.
  std::vector<int> revive_and_collect_revival_opcodes(Session &session) {
    send(session, 20039, nlohmann::json::object());
    std::vector<int> opcodes;
    for (int count = 0; count < 20000; ++count) {
      const auto frame = protocol_.decode(session.client->read());
      if (is_revival_frame(frame) || is_player_idle_frame(frame)) {
        opcodes.push_back(frame.at("opcode").get<int>());
      }
      if (is_player_idle_frame(frame)) {
        return opcodes;
      }
    }
    throw std::runtime_error("The revival never reached the idle state frame");
  }

  WorldProgress progress() {
    return repositories_.character_state->load_world_progress(character_.account_id, character_.id,
                                                              1);
  }

  std::int64_t character_id() const {
    return character_.id;
  }

  // The player unit id the server announced in the last world entry (MapMoveCompleteNoti).
  std::uint64_t entry_player_unit_id() const {
    return entry_player_unit_id_;
  }

  bool bump_revision_behind_the_session() {
    const auto before = progress();
    auto after = before;
    ++after.revision;
    ++after.gold;
    return repositories_.character_state->save_world_progress(character_.account_id, character_.id,
                                                              1, before, after);
  }

  static constexpr int kDeathMonsterCmsId = 5001084;
  static constexpr int kSpawnResetStatusId = 50030002;

private:
  static Config game_config() {
    Config config;
    config.game_address = "127.0.0.1:8081";
    return config;
  }

  std::shared_ptr<const TutorialData> definitions_;
  std::filesystem::path catalog_root_;
  std::shared_ptr<ClientCatalog> catalog_;
  std::shared_ptr<const CharacterStats> stats_;
  MemoryRepositories repositories_;
  AuthService auth_;
  Protocol protocol_;
  x5::pa2c::AuthChallengeRes login_;
  nlohmann::json position_;
  Character character_;
  std::uint64_t entry_player_unit_id_{};
  TemporaryWorldCertificate certificate_;
  std::unique_ptr<TwoClientWorldServer> server_;
};

} // namespace chronicles::combat_native_testing
