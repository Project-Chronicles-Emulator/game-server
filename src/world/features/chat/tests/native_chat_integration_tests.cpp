#include "world/session/tests/native_world_test_support.hpp"

#include <gtest/gtest.h>

namespace chronicles {
namespace {

using namespace native_testing;

TEST(NativeWorld, LocalChatAudienceIsTheSpeakerAndTheCharactersThatSeeIt) {
  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  const auto speaker = select_world_test_character(auth, "local_chat_speaker", "LocalSpeaker", 1);
  const auto listener = select_world_test_character(auth, "local_chat_listener", "LocalHearer", 3);
  const auto speaker_id = speaker.character().char_id();
  const auto listener_id = listener.character().char_id();
  const ChatIdentity speaker_identity{1, speaker_id, "LocalSpeaker"};

  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate);
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  EXPECT_TRUE(server.listener().local_chat_audience(speaker_identity).empty());

  WorldTestClient speaker_client(server.port());
  enter_shared_world(speaker_client, protocol, speaker.packet_key());
  WorldTestClient listener_client(server.port());
  enter_shared_world(listener_client, protocol, listener.packet_key());
  ASSERT_EQ(speaker_client.read().opcode(), 10);
  ASSERT_EQ(listener_client.read().opcode(), 10);

  auto audience = server.listener().local_chat_audience(speaker_identity);
  std::ranges::sort(audience);
  auto expected = std::vector<std::int64_t>{speaker_id, listener_id};
  std::ranges::sort(expected);
  EXPECT_EQ(audience, expected);

  // Another realm's identity with the same character ID has no in-world speaker here.
  EXPECT_TRUE(
      server.listener().local_chat_audience(ChatIdentity{2, speaker_id, "LocalSpeaker"}).empty());

  listener_client.disconnect();
  ASSERT_EQ(read_until_opcode(speaker_client, 11).opcode(), 11);
  EXPECT_EQ(server.listener().local_chat_audience(speaker_identity),
            std::vector<std::int64_t>{speaker_id});
  speaker_client.disconnect();
}

} // namespace
} // namespace chronicles
