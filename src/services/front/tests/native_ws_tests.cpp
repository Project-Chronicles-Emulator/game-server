#include "services/front/native_ws.hpp"
#include "testing/memory_repositories.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace chronicles {
namespace {

template <typename Response>
Response decode_response(const std::string &wire, std::uint16_t expected_packet_id) {
  const auto frames = native_ws::decode_frames(wire);
  if (frames.size() != 1 || frames.front().packet_id != expected_packet_id) {
    throw std::runtime_error("Unexpected native response packet");
  }

  Response response;
  const auto payload = frames.front().payload;
  if (!response.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    throw std::runtime_error("Invalid native response protobuf");
  }

  return response;
}

TEST(NativeWebSocket, EncodesVerifiedBinaryHeaderWithoutSequenceField) {
  x5::pb::authwebsocket::WorldEnterReq request;
  request.set_world_id(1);
  const std::string expected_world_enter{"\x06\x00\x21\x4e\x08\x01", 6};
  EXPECT_EQ(native_ws::encode_frame(native_ws::kWorldEnterRequest, request), expected_world_enter);

  x5::pb::ws::front::CharListReq list_request;
  const std::string expected_character_list{"\x04\x00\x21\x4e", 4};
  EXPECT_EQ(native_ws::encode_frame(native_ws::kCharacterListRequest, list_request),
            expected_character_list);

  const auto combined_frames = expected_world_enter + expected_character_list;
  const auto frames = native_ws::decode_frames(combined_frames);
  ASSERT_EQ(frames.size(), 2);
  EXPECT_EQ(frames.front().payload, std::string("\x08\x01", 2));
  EXPECT_TRUE(frames.back().payload.empty());
}

TEST(NativeWebSocket, RejectsTruncationInvalidLengthsAndOversizedPayloads) {
  const std::string short_header{"\x04\x00\x21", 3};
  const std::string short_body{"\x06\x00\x21\x4e\x08", 5};
  const std::string zero_length{"\x00\x00\x00\x00", 4};
  const std::string undersized_length{"\x03\x00\x00\x00", 4};
  EXPECT_THROW(native_ws::decode_frames(short_header), std::invalid_argument);
  EXPECT_THROW(native_ws::decode_frames(short_body), std::invalid_argument);
  EXPECT_THROW(native_ws::decode_frames(zero_length), std::invalid_argument);
  EXPECT_THROW(native_ws::decode_frames(undersized_length), std::invalid_argument);
  EXPECT_THROW(native_ws::decode_frames(""), std::invalid_argument);
  EXPECT_THROW(native_ws::decode_frames(std::string(native_ws::kMaximumMessageBytes + 1, 'x')),
               std::invalid_argument);

  const std::string empty_frame{"\x04\x00\x21\x4e", 4};
  std::string excessive_packets;
  for (std::size_t index = 0; index <= native_ws::kMaximumPacketsPerMessage; ++index) {
    excessive_packets.append(empty_frame);
  }
  EXPECT_THROW(native_ws::decode_frames(excessive_packets), std::invalid_argument);

  x5::pb::frontwebsocket::Announce oversized_response;
  oversized_response.set_message(std::string(65532, 'x'));
  EXPECT_THROW(native_ws::encode_frame(3, oversized_response), std::length_error);
}

TEST(NativeWebSocket, WorldEntryCharacterCreationAndSelectionUseDistinctServiceIdSpaces) {
  MemoryRepositories repositories;
  Config config;
  config.lobby_address = "http://127.0.0.1:8080";
  config.game_address = "127.0.0.1:8081";
  AuthService auth(repositories.character_repositories(), config);
  NativeWsDispatcher dispatcher(auth);
  ASSERT_TRUE(auth.create_account("wsnative", "native-test-password"));
  const auto token = auth.login("wsnative", "native-test-password").access_token();
  ASSERT_FALSE(token.empty());

  x5::pb::ws::front::CharListReq list_request;
  const auto list_frame = native_ws::encode_frame(native_ws::kCharacterListRequest, list_request);
  EXPECT_THROW(dispatcher.handle(NativeWsChannel::front, token, list_frame), Unauthorized);

  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  const auto world_frame = native_ws::encode_frame(native_ws::kWorldEnterRequest, world_request);
  const auto entered = decode_response<x5::pa2c::WorldEnterRes>(
      dispatcher.handle(NativeWsChannel::auth, token, world_frame), native_ws::kWorldEnterResponse);
  ASSERT_EQ(entered.error_code(), x5::ec::Success);
  EXPECT_EQ(entered.granted().front_rest_address(), config.lobby_address);

  x5::pb::ws::front::CharCreateReq create_request;
  create_request.set_slot(0);
  create_request.set_char_name("NativeHero");
  create_request.set_char_class(x5::cms::EX5CharacterClass_Fight);
  const auto create_frame =
      native_ws::encode_frame(native_ws::kCharacterCreateRequest, create_request);
  const auto created = decode_response<x5::pb::frontwebsocket::CharCreateRes>(
      dispatcher.handle(NativeWsChannel::front, token, create_frame),
      native_ws::kCharacterCreateResponse);
  ASSERT_EQ(created.ec(), x5::ec::Success);
  ASSERT_TRUE(created.has_char_());

  x5::pb::ws::front::CharSelectReq select_request;
  select_request.set_char_id(created.char_().char_id());
  const auto select_frame =
      native_ws::encode_frame(native_ws::kCharacterSelectRequest, select_request);
  const auto selected = decode_response<x5::pb::frontwebsocket::CharSelectRes>(
      dispatcher.handle(NativeWsChannel::front, token, select_frame),
      native_ws::kCharacterSelectResponse);
  ASSERT_EQ(selected.ec(), x5::ec::Success);
  EXPECT_EQ(selected.character().char_id(), created.char_().char_id());
  EXPECT_EQ(selected.game_address(), config.game_address);
  EXPECT_EQ(selected.packet_key().size(), 32);

  const auto listed = decode_response<x5::pb::frontwebsocket::CharListRes>(
      dispatcher.handle(NativeWsChannel::front, token, list_frame),
      native_ws::kCharacterListResponse);
  ASSERT_EQ(listed.char_list_size(), 1);
  EXPECT_EQ(listed.char_list(0).name(), "NativeHero");

  EXPECT_THROW(dispatcher.handle(NativeWsChannel::auth, token, create_frame),
               std::invalid_argument);
  EXPECT_THROW(dispatcher.handle(NativeWsChannel::front, "invalid-token", list_frame),
               Unauthorized);

  const std::string malformed_protobuf{"\x05\x00\x21\x4e\x80", 5};
  EXPECT_THROW(dispatcher.handle(NativeWsChannel::auth, token, malformed_protobuf),
               std::invalid_argument);
}

TEST(NativeWebSocket, FriendListsPreservePresenceAndRequireAnEnteredLiveSession) {
  MemoryRepositories repositories;
  auto now = Clock::now();
  AuthService auth(repositories.character_repositories(), Config{}, [&] { return now; });
  NativeWsDispatcher dispatcher(auth);
  ASSERT_TRUE(auth.create_account("friends", "friend-list-test-password"));
  const auto token = auth.login("friends", "friend-list-test-password").access_token();
  ASSERT_FALSE(token.empty());

  x5::pb::ws::front::FriendListReq request;
  const std::string absent_request{"\x04\x00\x38\x4e", 4};
  EXPECT_THROW(auth.friends("invalid-token", request), Unauthorized);
  EXPECT_THROW(dispatcher.handle(NativeWsChannel::front, "invalid-token", absent_request),
               Unauthorized);
  EXPECT_THROW(auth.friends(token, request), Unauthorized);
  EXPECT_THROW(dispatcher.handle(NativeWsChannel::front, token, absent_request), Unauthorized);

  x5::pb::authwebsocket::WorldEnterReq world_request;
  world_request.set_world_id(1);
  ASSERT_EQ(auth.enter(token, world_request).error_code(), x5::ec::Success);

  struct FriendListFixture {
    const char *name;
    std::string request;
    std::string response;
    bool recommend_present;
    bool recommend;
  };
  const std::array<FriendListFixture, 3> fixtures{{
      {"absent", absent_request, std::string("\x04\x00\x1c\x00", 4), false, false},
      {"false", std::string("\x06\x00\x38\x4e\x08\x00", 6),
       std::string("\x06\x00\x1c\x00\x08\x00", 6), true, false},
      {"true", std::string("\x06\x00\x38\x4e\x08\x01", 6),
       std::string("\x06\x00\x1c\x00\x08\x01", 6), true, true},
  }};
  for (const auto &fixture : fixtures) {
    SCOPED_TRACE(fixture.name);
    const auto wire = dispatcher.handle(NativeWsChannel::front, token, fixture.request);
    EXPECT_EQ(wire, fixture.response);
    const auto response = decode_response<x5::pb::frontwebsocket::FriendListRes>(wire, 28);
    EXPECT_EQ(response.friends_size(), 0);
    EXPECT_EQ(response.has_recommend(), fixture.recommend_present);
    EXPECT_EQ(response.recommend(), fixture.recommend);
  }

  const std::string malformed_request{"\x05\x00\x38\x4e\x80", 5};
  EXPECT_THROW(dispatcher.handle(NativeWsChannel::front, token, malformed_request),
               std::invalid_argument);
  EXPECT_THROW(dispatcher.handle(NativeWsChannel::auth, token, absent_request),
               std::invalid_argument);

  now += std::chrono::seconds(3601);
  EXPECT_THROW(auth.friends(token, request), Unauthorized);
  EXPECT_THROW(dispatcher.handle(NativeWsChannel::front, token, absent_request), Unauthorized);
}

// Front lobby deletion (WSC2F 20003-20006) and name lookup (WSC2F 20010). Deletion is
// fail-closed until a pending-deletion record exists, so no request may remove character rows.
// The lookup resolves any character of the entered realm by exact name, not only the caller's.
class NativeWebSocketLobbyTest : public ::testing::Test {
protected:
  static constexpr int kRealmId = 1;
  static constexpr int kOtherRealmId = 2;
  static constexpr const char *kPassword = "lobby-test-password";

  // A character ID that no test creates; the memory database assigns IDs from 1.
  static constexpr std::int64_t kUnknownCharacterId = 999999;

  struct Player {
    std::int64_t account_id{};
    std::string token;
  };

  Player enter_realm(const std::string &account_name) {
    Player player;
    if (!auth_.create_account(account_name, kPassword)) {
      ADD_FAILURE() << "Account creation failed for " << account_name;
      return player;
    }

    player.token = auth_.login(account_name, kPassword).access_token();
    x5::pb::authwebsocket::WorldEnterReq enter;
    enter.set_world_id(kRealmId);
    EXPECT_EQ(auth_.enter(player.token, enter).error_code(), x5::ec::Success);

    const auto account = repositories_.accounts->account(account_name);
    EXPECT_TRUE(account.has_value());
    if (account) {
      player.account_id = account->id;
    }
    return player;
  }

  std::int64_t create_character(const Player &player, const std::string &name) {
    x5::pb::ws::front::CharCreateReq request;
    request.set_slot(0);
    request.set_char_name(name);
    request.set_char_class(x5::cms::EX5CharacterClass_Fight);

    const auto response = auth_.create(player.token, request);
    EXPECT_EQ(response.ec(), x5::ec::Success);
    return response.char_().char_id();
  }

  std::size_t character_count(const Player &player) {
    return repositories_.characters->characters(player.account_id, kRealmId).size();
  }

  x5::ec::EErrorCode lookup(const Player &player, const std::string &name) {
    x5::pb::ws::front::LookupCharIdByNameReq request;
    request.set_char_name(name);
    return auth_.lookup_char_id(player.token, request).ec();
  }

  MemoryRepositories repositories_;
  AuthService auth_{repositories_.character_repositories(), Config{}};
};

TEST_F(NativeWebSocketLobbyTest, DeletePreCheckReportsOnlyTheCallersCharacters) {
  const auto owner = enter_realm("lobbyowner");
  const auto other = enter_realm("lobbyother");
  const auto owned_id = create_character(owner, "LobbyOwned");
  const auto foreign_id = create_character(other, "LobbyForeign");

  x5::pb::ws::front::CharDeletePreCheckReq request;
  request.set_char_id(owned_id);
  const auto owned = auth_.delete_precheck(owner.token, request);
  EXPECT_EQ(owned.ec(), x5::ec::Success);
  EXPECT_EQ(owned.char_id(), owned_id);
  EXPECT_EQ(owned.name(), "LobbyOwned");
  EXPECT_EQ(owned.level(), 1u);
  EXPECT_EQ(owned.block_reasons_size(), 0);

  request.set_char_id(foreign_id);
  EXPECT_EQ(auth_.delete_precheck(owner.token, request).ec(), x5::ec::InvalidChar);

  request.set_char_id(kUnknownCharacterId);
  EXPECT_EQ(auth_.delete_precheck(owner.token, request).ec(), x5::ec::InvalidChar);
}

TEST_F(NativeWebSocketLobbyTest, DeleteRefusesWithoutRemovingTheCharacter) {
  const auto owner = enter_realm("lobbydelete");
  const auto character_id = create_character(owner, "LobbyKeeper");

  x5::pb::ws::front::CharDeleteReq request;
  request.set_char_id(character_id);
  request.set_char_name("LobbyOther");
  EXPECT_EQ(auth_.delete_character(owner.token, request).ec(), x5::ec::CharDeleteNameMismatch);
  EXPECT_EQ(character_count(owner), 1u);

  // A correctly named, owned character still reaches the pending-deletion refusal.
  request.set_char_name("LobbyKeeper");
  EXPECT_EQ(auth_.delete_character(owner.token, request).ec(), x5::ec::CharDeleteInvalid);
  EXPECT_EQ(character_count(owner), 1u);

  request.set_char_id(kUnknownCharacterId);
  EXPECT_EQ(auth_.delete_character(owner.token, request).ec(), x5::ec::InvalidChar);
  EXPECT_EQ(character_count(owner), 1u);
}

TEST_F(NativeWebSocketLobbyTest, CancelAndImmediateDeletionReportNothingPending) {
  const auto owner = enter_realm("lobbypending");
  const auto character_id = create_character(owner, "LobbyPending");

  x5::pb::ws::front::CharDeleteCancelReq cancel;
  cancel.set_char_id(character_id);
  EXPECT_EQ(auth_.delete_cancel(owner.token, cancel).ec(), x5::ec::CharDeleteNotPending);

  x5::pb::ws::front::CharDeleteImmediateReq immediate;
  immediate.set_char_id(character_id);
  EXPECT_EQ(auth_.delete_immediate(owner.token, immediate).ec(), x5::ec::CharDeleteNotPending);

  cancel.set_char_id(kUnknownCharacterId);
  EXPECT_EQ(auth_.delete_cancel(owner.token, cancel).ec(), x5::ec::InvalidChar);
  immediate.set_char_id(kUnknownCharacterId);
  EXPECT_EQ(auth_.delete_immediate(owner.token, immediate).ec(), x5::ec::InvalidChar);

  EXPECT_EQ(character_count(owner), 1u);
}

TEST_F(NativeWebSocketLobbyTest, LookupResolvesAnyCharacterOfTheRealmByExactName) {
  const auto searcher = enter_realm("lobbysearch");
  const auto target = enter_realm("lobbytarget");
  const auto target_id = create_character(target, "LookupTarget");

  x5::pb::ws::front::LookupCharIdByNameReq request;
  request.set_char_name("LookupTarget");
  const auto found = auth_.lookup_char_id(searcher.token, request);
  EXPECT_EQ(found.ec(), x5::ec::Success);
  EXPECT_EQ(found.char_id(), target_id);
  EXPECT_EQ(found.char_name(), "LookupTarget");

  EXPECT_EQ(lookup(searcher, "lookuptarget"), x5::ec::InvalidChar);
  EXPECT_EQ(lookup(searcher, "NobodyHere"), x5::ec::InvalidChar);
  EXPECT_EQ(lookup(searcher, "ab"), x5::ec::BadRequest);
  EXPECT_EQ(lookup(searcher, std::string(17, 'a')), x5::ec::BadRequest);
}

TEST_F(NativeWebSocketLobbyTest, LookupIgnoresCharactersOfOtherRealms) {
  const auto searcher = enter_realm("lobbyrealm");

  Character elsewhere;
  elsewhere.account_id = searcher.account_id;
  elsewhere.world_id = kOtherRealmId;
  elsewhere.name = "OtherRealmHero";
  repositories_.characters->create(elsewhere);

  EXPECT_EQ(lookup(searcher, "OtherRealmHero"), x5::ec::InvalidChar);
}

TEST_F(NativeWebSocketLobbyTest, FrontDispatchRoutesDeletionAndLookupRequests) {
  const auto owner = enter_realm("lobbywire");
  const auto character_id = create_character(owner, "LobbyWire");
  NativeWsDispatcher dispatcher(auth_);

  x5::pb::ws::front::CharDeletePreCheckReq precheck_request;
  precheck_request.set_char_id(character_id);
  const auto precheck = decode_response<x5::pb::frontwebsocket::CharDeletePreCheckRes>(
      dispatcher.handle(
          NativeWsChannel::front, owner.token,
          native_ws::encode_frame(native_ws::kCharacterDeletePreCheckRequest, precheck_request)),
      native_ws::kCharacterDeletePreCheckResponse);
  EXPECT_EQ(precheck.ec(), x5::ec::Success);
  EXPECT_EQ(precheck.name(), "LobbyWire");

  x5::pb::ws::front::LookupCharIdByNameReq lookup_request;
  lookup_request.set_char_name("LobbyWire");
  const auto found = decode_response<x5::pb::frontwebsocket::LookupCharIdByNameRes>(
      dispatcher.handle(
          NativeWsChannel::front, owner.token,
          native_ws::encode_frame(native_ws::kLookupCharIdByNameRequest, lookup_request)),
      native_ws::kLookupCharIdByNameResponse);
  EXPECT_EQ(found.ec(), x5::ec::Success);
  EXPECT_EQ(found.char_id(), character_id);

  x5::pb::ws::front::CharDeleteReq delete_request;
  delete_request.set_char_id(character_id);
  delete_request.set_char_name("LobbyWire");
  const auto deleted = decode_response<x5::pb::frontwebsocket::CharDeleteRes>(
      dispatcher.handle(NativeWsChannel::front, owner.token,
                        native_ws::encode_frame(native_ws::kCharacterDeleteRequest,
                                                delete_request)),
      native_ws::kCharacterDeleteResponse);
  EXPECT_EQ(deleted.ec(), x5::ec::CharDeleteInvalid);
  EXPECT_EQ(character_count(owner), 1u);
}

} // namespace
} // namespace chronicles
