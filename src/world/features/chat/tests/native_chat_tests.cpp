#include "world/features/chat/native_chat.hpp"
#include "testing/memory_repositories.hpp"
#include "services/front/native_ws.hpp"
#include "p_system.pb.h"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace chronicles {
namespace {

template <class Message> Message decode_chat(const std::string &wire, std::uint16_t packet_id) {
  const auto frames = native_ws::decode_frames(wire);
  if (frames.size() != 1 || frames.front().packet_id != packet_id) {
    throw std::runtime_error("Unexpected chat frame");
  }
  Message result;
  const auto payload = frames.front().payload;
  if (!result.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    throw std::runtime_error("Malformed chat response");
  }
  return result;
}

struct ChatPlayer {
  std::string token;
  std::int64_t character_id{};
  std::uint64_t connection_id{};
};

class NativeChat : public testing::Test {
protected:
  ChatPlayer player(const std::string &name, bool select_character = true) {
    if (!auth_.create_account(name, "chat-test-password")) {
      throw std::runtime_error("Cannot provision test account");
    }
    ChatPlayer result;
    result.token = auth_.login(name, "chat-test-password").access_token();
    x5::pb::authwebsocket::WorldEnterReq enter;
    enter.set_world_id(1);
    if (auth_.enter(result.token, enter).error_code() != x5::ec::Success) {
      throw std::runtime_error("Cannot enter test realm");
    }
    result.connection_id = chat_.connect(result.token);
    if (select_character) {
      x5::pb::ws::front::CharCreateReq create;
      create.set_slot(0);
      create.set_char_name(name);
      create.set_char_class(x5::cms::EX5CharacterClass_Wild);
      const auto created = auth_.create(result.token, create);
      if (created.ec() != x5::ec::Success || !created.has_char_()) {
        throw std::runtime_error("Cannot create test character");
      }
      result.character_id = created.char_().char_id();
      x5::pb::ws::front::CharSelectReq select;
      select.set_char_id(result.character_id);
      if (auth_.select(result.token, select).ec() != x5::ec::Success) {
        throw std::runtime_error("Cannot select test character");
      }
    }
    return result;
  }

  x5::pb::ws::front::ChatReq world_message(const std::string &text) {
    x5::pb::ws::front::ChatReq request;
    request.set_type(x5::cms::EX5ChatMessageType_World);
    request.set_message(text);
    return request;
  }

  Clock::time_point now_ = Clock::now();
  MemoryRepositories repositories_;
  AuthService auth_{repositories_.character_repositories(), Config{},
                    [this] { return now_; }};
  NativeChatService chat_{auth_, [this] { return now_; }};
};

TEST_F(NativeChat, BroadcastsWorldMessagesWithAuthenticatedSenderAndExcludesLobbyOnlyClients) {
  const auto alice = player("Alice");
  const auto bob = player("Bobby");
  const auto lobby = player("Lobby", false);
  chat_.world_chat(alice.token, world_message("Hello world"));

  for (const auto connection_id : {alice.connection_id, bob.connection_id}) {
    const auto messages = chat_.drain(connection_id);
    ASSERT_EQ(messages.size(), 1);
    const auto notification = decode_chat<x5::pb::frontwebsocket::ChatNoti>(messages.front(), 13);
    EXPECT_EQ(notification.sender_name(), "Alice");
    EXPECT_EQ(notification.message(), "Hello world");
    EXPECT_EQ(notification.type(), x5::cms::EX5ChatMessageType_World);
  }
  EXPECT_TRUE(chat_.drain(lobby.connection_id).empty());
  EXPECT_THROW(chat_.world_chat(lobby.token, world_message("No selected character")), Unauthorized);
}

TEST_F(NativeChat, WhispersResolveNameAndIdWithoutDeliveringToOtherPlayers) {
  const auto alice = player("Alice");
  const auto bob = player("Bobby");
  const auto observer = player("Observer");
  x5::pb::ws::front::DirectMessageReq request;
  request.set_char_name("Bobby");
  request.set_message("Private hello");
  const auto response = chat_.direct_message(alice.token, request);
  ASSERT_EQ(response.ec(), x5::ec::Success);
  EXPECT_EQ(response.char_id(), bob.character_id);
  EXPECT_EQ(response.char_name(), "Bobby");

  const auto messages = chat_.drain(bob.connection_id);
  ASSERT_EQ(messages.size(), 1);
  const auto notification =
      decode_chat<x5::pb::frontwebsocket::DirectMessageNoti>(messages.front(), 15);
  EXPECT_EQ(notification.char_id(), alice.character_id);
  EXPECT_EQ(notification.char_name(), "Alice");
  EXPECT_EQ(notification.message(), request.message());
  EXPECT_TRUE(chat_.drain(observer.connection_id).empty());
  EXPECT_TRUE(chat_.drain(alice.connection_id).empty());

  request.set_char_id(bob.character_id);
  EXPECT_EQ(chat_.direct_message(alice.token, request).ec(), x5::ec::Success);
  EXPECT_EQ(chat_.drain(bob.connection_id).size(), 1);
  chat_.disconnect(bob.connection_id);
  EXPECT_EQ(chat_.direct_message(alice.token, request).ec(), x5::ec::DM_CharNotFound);
}

TEST_F(NativeChat, RevokedAndReselectedRecipientsDoNotReceivePreviouslyQueuedWhispers) {
  const auto alice = player("Alice");
  const auto bob = player("Bobby");
  x5::pb::ws::front::DirectMessageReq request;
  request.set_char_id(bob.character_id);
  request.set_message("For the original character only");
  ASSERT_EQ(chat_.direct_message(alice.token, request).ec(), x5::ec::Success);

  x5::pb::ws::front::CharCreateReq create;
  create.set_slot(1);
  create.set_char_name("OtherBob");
  create.set_char_class(x5::cms::EX5CharacterClass_Wild);
  const auto created = auth_.create(bob.token, create);
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(created.char_().char_id());
  ASSERT_EQ(auth_.select(bob.token, select).ec(), x5::ec::Success);
  EXPECT_TRUE(chat_.drain(bob.connection_id).empty());

  chat_.world_chat(alice.token, world_message("Queued before logout"));
  auth_.logout(bob.token);
  EXPECT_THROW(chat_.drain(bob.connection_id), Unauthorized);
  EXPECT_EQ(chat_.direct_message(alice.token, request).ec(), x5::ec::DM_CharNotFound);
  auth_.logout(alice.token);
  EXPECT_THROW(chat_.world_chat(alice.token, world_message("Revoked sender")), Unauthorized);
}

TEST_F(NativeChat, RejectsUnimplementedAudiencesInvalidTextAndUnselectedCharacters) {
  const auto alice = player("Alice");
  for (const auto channel :
       {x5::cms::EX5ChatMessageType_Local, x5::cms::EX5ChatMessageType_Company,
        x5::cms::EX5ChatMessageType_Guild, x5::cms::EX5ChatMessageType_Party}) {
    auto request = world_message("Must not become public");
    request.set_type(channel);
    EXPECT_THROW(chat_.world_chat(alice.token, request), std::invalid_argument);
  }
  EXPECT_TRUE(chat_.drain(alice.connection_id).empty());
  for (const auto &text :
       {std::string{}, std::string(kMaximumChatTextBytes + 1, 'x'),
        std::string("embedded\0nul", 12), std::string("new\nline"), std::string("\xc0\xaf", 2)}) {
    EXPECT_THROW(chat_.world_chat(alice.token, world_message(text)), std::invalid_argument);
  }
  chat_.world_chat(alice.token, world_message("UTF-8: \xc3\xa1 \xf0\x9f\x98\x80"));
  EXPECT_EQ(chat_.drain(alice.connection_id).size(), 1);
}

TEST_F(NativeChat, RateLimitExpiresAndSlowRecipientDoesNotBlockOtherRecipients) {
  const auto alice = player("Alice");
  const auto slow = player("SlowReader");
  for (unsigned count = 0; count < kMaximumChatMessagesPerWindow; ++count) {
    chat_.world_chat(alice.token, world_message("Bounded message"));
  }
  EXPECT_THROW(chat_.world_chat(alice.token, world_message("Too fast")), std::length_error);
  chat_.drain(alice.connection_id);
  chat_.drain(slow.connection_id);
  now_ += kChatRateWindow;
  for (std::size_t count = 0; count <= kMaximumQueuedChatMessages; ++count) {
    chat_.world_chat(alice.token, world_message("Slow reader backlog"));
    EXPECT_EQ(chat_.drain(alice.connection_id).size(), 1);
    now_ += std::chrono::seconds(2);
  }
  EXPECT_THROW(chat_.drain(slow.connection_id), std::length_error);
}

TEST_F(NativeChat, NativeDispatcherUsesVerifiedIdsAndNoFabricatedChatAcknowledgement) {
  const auto alice = player("Alice");
  NativeWsDispatcher dispatcher(auth_);
  const auto connection_id = dispatcher.chat().connect(alice.token);
  const auto wire = native_ws::encode_frame(20008, world_message("Native world chat"));
  EXPECT_TRUE(dispatcher.handle(NativeWsChannel::front, alice.token, wire).empty());
  const auto messages = dispatcher.chat().drain(connection_id);
  ASSERT_EQ(messages.size(), 1);
  EXPECT_EQ(decode_chat<x5::pb::frontwebsocket::ChatNoti>(messages.front(), 13).message(),
            "Native world chat");

  x5::pb::ws::front::DirectMessageReq request;
  request.set_char_name("Unknown");
  request.set_message("Offline target");
  const auto response = dispatcher.handle(NativeWsChannel::front, alice.token,
                                          native_ws::encode_frame(20009, request));
  EXPECT_EQ(decode_chat<x5::pb::frontwebsocket::DirectMessageRes>(response, 14).ec(),
            x5::ec::DM_CharNotFound);

  auto local = world_message("Local requires spatial membership");
  local.set_type(x5::cms::EX5ChatMessageType_Local);
  const auto rejected =
      dispatcher.handle(NativeWsChannel::front, alice.token, native_ws::encode_frame(20008, local));
  EXPECT_EQ(decode_chat<x5::pb::CommonErrorRes>(rejected, 1).ec(), x5::ec::BadRequest);
}

TEST_F(NativeChat, LocalChatReachesOnlyTheWorldSuppliedAudienceIncludingTheSpeaker) {
  const auto alice = player("Alice");
  const auto nearby = player("Nearby");
  const auto distant = player("Distant");
  NativeWsDispatcher dispatcher(auth_);
  const auto alice_connection = dispatcher.chat().connect(alice.token);
  const auto nearby_connection = dispatcher.chat().connect(nearby.token);
  const auto distant_connection = dispatcher.chat().connect(distant.token);

  // Stands in for the world service's sight registry; the client never supplies an audience.
  bool speaker_in_world = true;
  dispatcher.set_local_chat_audience([&](const ChatIdentity &speaker) {
    if (!speaker_in_world || speaker.character_id != alice.character_id) {
      return std::vector<std::int64_t>{};
    }
    return std::vector<std::int64_t>{alice.character_id, nearby.character_id};
  });

  auto local = world_message("Hello nearby");
  local.set_type(x5::cms::EX5ChatMessageType_Local);
  const auto wire = native_ws::encode_frame(native_ws::kChatRequest, local);
  EXPECT_TRUE(dispatcher.handle(NativeWsChannel::front, alice.token, wire).empty());

  for (const auto connection_id : {alice_connection, nearby_connection}) {
    const auto messages = dispatcher.chat().drain(connection_id);
    ASSERT_EQ(messages.size(), 1);
    const auto notification =
        decode_chat<x5::pb::frontwebsocket::ChatNoti>(messages.front(), native_ws::kChatNotification);
    EXPECT_EQ(notification.type(), x5::cms::EX5ChatMessageType_Local);
    EXPECT_EQ(notification.message(), "Hello nearby");
    EXPECT_EQ(notification.sender_name(), "Alice");
  }
  EXPECT_TRUE(dispatcher.chat().drain(distant_connection).empty());

  speaker_in_world = false;
  const auto refused = dispatcher.handle(NativeWsChannel::front, alice.token, wire);
  EXPECT_EQ(decode_chat<x5::pb::CommonErrorRes>(refused, 1).ec(), x5::ec::BadRequest);
  EXPECT_TRUE(dispatcher.chat().drain(alice_connection).empty());

  // Company still has no membership service and must not fall back to a realm broadcast.
  auto company = world_message("Company stays unavailable");
  company.set_type(x5::cms::EX5ChatMessageType_Company);
  const auto company_refused = dispatcher.handle(
      NativeWsChannel::front, alice.token, native_ws::encode_frame(native_ws::kChatRequest, company));
  EXPECT_EQ(decode_chat<x5::pb::CommonErrorRes>(company_refused, 1).ec(), x5::ec::BadRequest);
  EXPECT_TRUE(dispatcher.chat().drain(nearby_connection).empty());
}

} // namespace
} // namespace chronicles
