#include "world/session/tests/native_world_test_support.hpp"

#include <gtest/gtest.h>

namespace chronicles {
namespace {

using namespace native_testing;
TEST(NativeWorld, TwoTlsClientsFormPartyAndExchangePartyChat) {
  constexpr std::uint16_t kStartPlayingRequest = 20086;
  constexpr std::uint16_t kStartPlayingResponse = 183;

  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();

  const auto leader = select_world_test_character(auth, "party_leader_test", "PartyLeader", 1);
  const auto member = select_world_test_character(auth, "party_member_test", "PartyMember", 3);
  const auto leader_id = std::to_string(leader.character().char_id());
  const auto member_id = std::to_string(member.character().char_id());
  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate);
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);

  WorldTestClient leader_client(server.port());
  enter_shared_world(leader_client, protocol, leader.packet_key());
  WorldTestClient member_client(server.port());
  enter_shared_world(member_client, protocol, member.packet_key());
  ASSERT_EQ(leader_client.read().opcode(), 10);
  ASSERT_EQ(member_client.read().opcode(), 10);

  leader_client.send(protocol.encode(kStartPlayingRequest, nlohmann::json::object(), 2));
  ASSERT_EQ(leader_client.read().opcode(), kStartPlayingResponse);
  member_client.send(protocol.encode(kStartPlayingRequest, nlohmann::json::object(), 2));
  ASSERT_EQ(member_client.read().opcode(), kStartPlayingResponse);

  leader_client.send(protocol.encode(20097, {{"target_char_id", member_id}}, 3));
  const auto popup = protocol.decode(read_until_opcode(member_client, 188)).at("message");
  EXPECT_EQ(popup.at("leader").at("char_id"), leader_id);
  EXPECT_EQ(popup.at("leader").at("name"), "PartyLeader");
  const auto party_id = popup.at("party_id").get<std::string>();

  member_client.send(protocol.encode(
      20098,
      {{"leader_char_id", leader_id}, {"target_char_id", member_id}, {"party_id", party_id}}, 3));
  const auto join = protocol.decode(read_until_opcode(member_client, 184)).at("message");
  EXPECT_EQ(join.at("party_id"), party_id);
  EXPECT_EQ(join.at("leader_char_id"), leader_id);
  EXPECT_EQ(join.at("members").size(), 2u);

  const auto update = protocol.decode(read_until_opcode(leader_client, 186)).at("message");
  EXPECT_EQ(update.at("members").size(), 2u);
  const auto unit_change = protocol.decode(read_until_opcode(leader_client, 187)).at("message");
  EXPECT_EQ(unit_change.at("party_id"), party_id);

  leader_client.send(protocol.encode(
      20038, {{"type", "EX5ChatMessageType_Party"}, {"message", "party ready"}}, 4));
  const auto chat = protocol.decode(read_until_opcode(member_client, 75)).at("message");
  EXPECT_EQ(chat.at("type"), "EX5ChatMessageType_Party");
  EXPECT_EQ(chat.at("message"), "party ready");
  EXPECT_EQ(chat.at("sender_name"), "PartyLeader");

  member_client.send(protocol.encode(20095, nlohmann::json::object(), 4));
  const auto left = protocol.decode(read_until_opcode(member_client, 185)).at("message");
  EXPECT_EQ(left.at("party_id"), party_id);
  EXPECT_FALSE(left.value("kicked", false));
}

TEST(NativeWorld, PartyMemberDisconnectNotifiesTheRemainingTlsClient) {
  constexpr std::uint16_t kStartPlayingRequest = 20086;
  constexpr std::uint16_t kStartPlayingResponse = 183;

  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();

  const auto leader = select_world_test_character(auth, "party_close_leader", "CloseLeader", 1);
  const auto member = select_world_test_character(auth, "party_close_member", "CloseMember", 3);
  const auto leader_id = std::to_string(leader.character().char_id());
  const auto member_id = std::to_string(member.character().char_id());
  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate);
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);

  WorldTestClient leader_client(server.port());
  enter_shared_world(leader_client, protocol, leader.packet_key());
  WorldTestClient member_client(server.port());
  enter_shared_world(member_client, protocol, member.packet_key());
  ASSERT_EQ(leader_client.read().opcode(), 10);
  ASSERT_EQ(member_client.read().opcode(), 10);

  leader_client.send(protocol.encode(kStartPlayingRequest, nlohmann::json::object(), 2));
  ASSERT_EQ(leader_client.read().opcode(), kStartPlayingResponse);
  member_client.send(protocol.encode(kStartPlayingRequest, nlohmann::json::object(), 2));
  ASSERT_EQ(member_client.read().opcode(), kStartPlayingResponse);

  leader_client.send(protocol.encode(20097, {{"target_char_id", member_id}}, 3));
  const auto popup = protocol.decode(read_until_opcode(member_client, 188)).at("message");
  const auto party_id = popup.at("party_id").get<std::string>();
  member_client.send(protocol.encode(
      20098,
      {{"leader_char_id", leader_id}, {"target_char_id", member_id}, {"party_id", party_id}}, 3));
  read_until_opcode(member_client, 184);
  read_until_opcode(leader_client, 186);

  member_client.disconnect();
  const auto update = protocol.decode(read_until_opcode(leader_client, 186)).at("message");
  ASSERT_EQ(update.at("members").size(), 2U);
  bool found_retained_member = false;
  for (const auto &entry : update.at("members")) {
    if (entry.at("char_id") == member_id) {
      EXPECT_EQ(entry.at("connection_state"), "EX5PartyMemberConnStateType_Retain");
      found_retained_member = true;
    }
  }
  EXPECT_TRUE(found_retained_member);
}

} // namespace
} // namespace chronicles
