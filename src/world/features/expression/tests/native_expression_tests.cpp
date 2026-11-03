#include "world/session/tests/native_world_test_support.hpp"

namespace chronicles {
namespace {

using namespace native_testing;

TEST(NativeWorld, EmotesGrantsAndSelfieModeReachNearbyPlayersOverTls) {
  constexpr std::uint16_t kStartPlayingRequest = 20086;
  constexpr std::uint16_t kStartPlayingResponse = 183;
  constexpr std::uint16_t kGmTextRequest = 20009;
  constexpr std::uint16_t kEmotionExecRequest = 20063;
  constexpr std::uint16_t kSelfieModeRequest = 20064;
  constexpr std::uint16_t kEmotionExecResponse = 122;
  constexpr std::uint16_t kEmotionExecNotification = 123;
  constexpr std::uint16_t kSelfieModeNotification = 124;
  constexpr std::uint16_t kWorldCharEmotionsNotification = 144;
  constexpr std::uint16_t kEmotionsUpdateNotification = 145;
  constexpr int kDance = 9;
  constexpr int kReward = 501;
  constexpr char kPassword[] = "test-world-password-123";

  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  const auto initial = select_world_test_character(auth, "emote_keeper_test", "EmoteKeeper", 1);
  const auto viewer = select_world_test_character(auth, "emote_viewer_test", "EmoteViewer", 3);
  const auto late = select_world_test_character(auth, "emote_late_test", "EmoteLate", 1);

  // The game-master role is copied into the handoff at selection, so select again after it.
  ASSERT_TRUE(auth.set_account_role("emote_keeper_test", AccountRole::game_master));
  const auto login = auth.login("emote_keeper_test", kPassword);
  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  ASSERT_EQ(auth.enter(login.access_token(), world_request).error_code(), x5::ec::Success);
  x5::pb::ws::front::CharSelectReq select_request;
  const auto keeper_id = initial.character().char_id();
  select_request.set_char_id(keeper_id);
  const auto keeper = auth.select(login.access_token(), select_request);
  ASSERT_EQ(keeper.ec(), x5::ec::Success);
  const auto keeper_unit_id = std::to_string(player_wire_unit_id(keeper_id));

  // A two-second cooldown keeps the immediate repeat inside it on a slow machine.
  TemporaryWorldCatalog catalog;
  catalog.write_items(
      {}, {{"Emotion",
            {{{"id", "9"}, {"category", "Basic"}, {"targetAnimStanceKey", ""}},
             {{"id", "501"}, {"category", "Reward"}, {"targetAnimStanceKey", ""}}}},
           {"GameVariable", {{{"name", "EmotionCooltimeSec"}, {"value", "2.0"}}}}});
  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate, {}, {}, catalog.open());
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);

  WorldTestClient keeper_client(server.port());
  enter_shared_world(keeper_client, protocol, keeper.packet_key());
  WorldTestClient viewer_client(server.port());
  enter_shared_world(viewer_client, protocol, viewer.packet_key());
  ASSERT_EQ(keeper_client.read().opcode(), 10);
  ASSERT_EQ(viewer_client.read().opcode(), 10);

  // A Basic-only character receives the observed empty snapshot once, after StartPlayingRes.
  keeper_client.send(protocol.encode(kStartPlayingRequest, nlohmann::json::object(), 2));
  ASSERT_EQ(keeper_client.read().opcode(), kStartPlayingResponse);
  const auto snapshot =
      protocol.decode(read_until_opcode(keeper_client, kWorldCharEmotionsNotification));
  EXPECT_EQ(snapshot.at("status"), "decoded");
  EXPECT_FALSE(snapshot.at("message").contains("emotions"));
  viewer_client.send(protocol.encode(kStartPlayingRequest, nlohmann::json::object(), 2));
  ASSERT_EQ(read_until_opcode(viewer_client, kStartPlayingResponse).opcode(),
            kStartPlayingResponse);

  const auto emote = [&](int emotion_id, std::uint32_t sequence) {
    keeper_client.send(protocol.encode(
        kEmotionExecRequest, {{"emotion_cms_id", emotion_id}, {"yaw", 45.0}}, sequence));
    const auto response = protocol.decode(read_until_opcode(keeper_client, kEmotionExecResponse));
    return response.at("message").value("ec", std::string("Success"));
  };

  EXPECT_EQ(emote(kReward, 3), "Emotion_NotOwned");

  keeper_client.send(protocol.encode(kGmTextRequest, {{"text", "emotion_add 501"}}, 4));
  const auto update =
      protocol.decode(read_until_opcode(keeper_client, kEmotionsUpdateNotification));
  ASSERT_EQ(update.at("message").at("emotions").size(), 1U);
  EXPECT_EQ(update.at("message").at("emotions").front().at("cms_id"), kReward);
  const auto keeper_account = repositories.accounts->account("emote_keeper_test");
  ASSERT_TRUE(keeper_account);
  EXPECT_EQ(repositories.character_state->load_world_progress(keeper_account->id, keeper_id, 1)
                .owned_emotions,
            (std::set<int>{kReward}));

  EXPECT_EQ(emote(kReward, 5), "Success");
  EXPECT_EQ(emote(kDance, 6), "Emotion_Cooltime");
  keeper_client.send(protocol.encode(kSelfieModeRequest, {{"enter", true}}, 7));

  // The viewer sees the granted emote and the selfie pose; refused emotes never reach it.
  std::vector<nlohmann::json> emotes;
  std::optional<nlohmann::json> selfie;
  for (int frame_index = 0; frame_index < 32 && !selfie; ++frame_index) {
    const auto frame = viewer_client.read();
    if (frame.opcode() == kEmotionExecNotification) {
      emotes.push_back(protocol.decode(frame).at("message"));
    } else if (frame.opcode() == kSelfieModeNotification) {
      selfie = protocol.decode(frame).at("message");
    }
  }
  ASSERT_EQ(emotes.size(), 1U);
  EXPECT_EQ(emotes.front().at("unit_id"), keeper_unit_id);
  EXPECT_EQ(emotes.front().at("emotion_cms_id"), kReward);
  EXPECT_FLOAT_EQ(emotes.front().at("yaw").get<float>(), 45.0F);
  ASSERT_TRUE(selfie);
  EXPECT_EQ(selfie->at("unit_id"), keeper_unit_id);
  EXPECT_EQ(selfie->at("enter"), true);

  // A player entering later gets the keeper's UnitAddNoti followed by its selfie state.
  WorldTestClient late_client(server.port());
  enter_shared_world(late_client, protocol, late.packet_key());
  std::optional<nlohmann::json> late_selfie;
  bool keeper_added_first{};
  for (int frame_index = 0; frame_index < 8 && !late_selfie; ++frame_index) {
    const auto frame = late_client.read();
    if (frame.opcode() == 10 &&
        protocol.decode(frame).at("message").at("unit_id") == keeper_unit_id) {
      keeper_added_first = true;
    } else if (frame.opcode() == kSelfieModeNotification) {
      late_selfie = protocol.decode(frame).at("message");
    }
  }
  EXPECT_TRUE(keeper_added_first);
  ASSERT_TRUE(late_selfie);
  EXPECT_EQ(late_selfie->at("unit_id"), keeper_unit_id);
  EXPECT_EQ(late_selfie->at("enter"), true);
}

} // namespace
} // namespace chronicles
