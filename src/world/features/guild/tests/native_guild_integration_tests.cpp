#include "testing/company_test_support.hpp"
#include "world/features/guild/guild.hpp"
#include "world/session/tests/native_world_test_support.hpp"

#include <gtest/gtest.h>

namespace chronicles {
namespace {

using namespace native_testing;
TEST(NativeWorld, TwoTlsClientsJoinAGuildThroughAWorldInvitation) {
  constexpr std::uint16_t kStartPlayingRequest = 20086;
  constexpr std::uint16_t kStartPlayingResponse = 183;
  constexpr std::uint16_t kUnitAddNotification = 10;
  constexpr std::uint16_t kWorldCharGuildNotification = 146;
  constexpr std::uint16_t kGuildInvitePopupNotification = 216;
  constexpr std::uint16_t kChangeGuildUnitNotification = 218;
  constexpr std::uint16_t kGuildInvitePopupResponse = 219;

  MemoryRepositories repositories;
  Config config;
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  auth.acquire_monster_reward_owner_epoch();
  auto guilds =
      std::make_shared<GuildService>(std::make_shared<MemoryGuildStore>(), GuildRules{}, 1);

  const auto master = select_world_test_character(auth, "guild_master_test", "GuildMaster", 1);
  const auto recruit = select_world_test_character(auth, "guild_recruit_test", "GuildRecruit", 3);
  const auto master_id = std::to_string(master.character().char_id());
  const auto recruit_id = std::to_string(recruit.character().char_id());

  // Both characters belong to Antiqus; the founder holds GuildCreatableCompanyPosition.
  auto companies = std::make_shared<CompanyService>(std::make_shared<MemoryCompanyStore>(),
                                                    testing_support::company_rules(), 1);
  testing_support::seat_in_company(*companies, master.character().char_id(), 3);
  testing_support::seat_in_company(*companies, recruit.character().char_id(), 1);

  GuildActor founder;
  founder.world_id = 1;
  founder.char_id = master.character().char_id();
  founder.name = "GuildMaster";
  founder.company_type = testing_support::kAntiqus;
  founder.company_position = 3;
  GuildCreateRequest create;
  create.name = "Seafarers";
  const auto created = guilds->create(founder, create);
  ASSERT_EQ(created.error, x5::ec::Success);
  const auto guild_id = std::to_string(created.guild.id);

  TemporaryWorldCertificate certificate;
  TwoClientWorldServer server(auth, certificate, {}, {}, {}, {}, {},
                              guilds, {}, {}, {}, {},
                              WorldSocialServices{guilds, companies, {}, {}});
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);

  WorldTestClient master_client(server.port());
  enter_shared_world(master_client, protocol, master.packet_key());
  WorldTestClient recruit_client(server.port());
  enter_shared_world(recruit_client, protocol, recruit.packet_key());
  ASSERT_EQ(master_client.read().opcode(), kUnitAddNotification);
  const auto observed_master =
      protocol.decode(recruit_client.read()).at("message").at("nub").at("player");
  EXPECT_EQ(observed_master.at("guild_id"), guild_id);
  EXPECT_EQ(observed_master.at("guild_name"), "Seafarers");

  master_client.send(protocol.encode(kStartPlayingRequest, nlohmann::json::object(), 2));
  ASSERT_EQ(master_client.read().opcode(), kStartPlayingResponse);
  const auto master_snapshot =
      protocol.decode(read_until_opcode(master_client, kWorldCharGuildNotification));
  EXPECT_EQ(master_snapshot.at("message").at("guild_id"), guild_id);
  const auto own_label =
      protocol.decode(read_until_opcode(master_client, kChangeGuildUnitNotification));
  EXPECT_EQ(own_label.at("message").at("unit_id"),
            std::to_string(player_wire_unit_id(master.character().char_id())));

  recruit_client.send(protocol.encode(kStartPlayingRequest, nlohmann::json::object(), 2));
  ASSERT_EQ(recruit_client.read().opcode(), kStartPlayingResponse);
  const auto recruit_snapshot =
      protocol.decode(read_until_opcode(recruit_client, kWorldCharGuildNotification));
  EXPECT_EQ(recruit_snapshot.at("status"), "decoded");
  EXPECT_FALSE(recruit_snapshot.at("message").contains("guild_id"));

  master_client.send(protocol.encode(
      20113, {{"guild_id", guild_id}, {"target_char_id", recruit_id}}, 3));
  const auto invite_response =
      protocol.decode(read_until_opcode(master_client, kGuildInvitePopupResponse));
  EXPECT_EQ(invite_response.at("message").at("target_char_name"), "GuildRecruit");
  const auto popup =
      protocol.decode(read_until_opcode(recruit_client, kGuildInvitePopupNotification));
  EXPECT_EQ(popup.at("message").at("guild_char_id"), master_id);
  EXPECT_EQ(popup.at("message").at("guild_name"), "Seafarers");

  recruit_client.send(
      protocol.encode(20114, {{"guild_id", guild_id}, {"guild_char_id", master_id}}, 3));
  const auto recruit_label =
      protocol.decode(read_until_opcode(recruit_client, kChangeGuildUnitNotification));
  EXPECT_EQ(recruit_label.at("message").at("unit_id"),
            std::to_string(player_wire_unit_id(recruit.character().char_id())));
  EXPECT_EQ(recruit_label.at("message").at("guild_name"), "Seafarers");
  const auto observed_label =
      protocol.decode(read_until_opcode(master_client, kChangeGuildUnitNotification));
  EXPECT_EQ(observed_label.at("message").at("unit_id"),
            std::to_string(player_wire_unit_id(recruit.character().char_id())));
  EXPECT_EQ(guilds->status(recruit.character().char_id()).guild_id, created.guild.id);
}

} // namespace
} // namespace chronicles
