#include "world/features/chat/player_commands.hpp"

#include "testing/memory_repositories.hpp"
#include "services/front/native_ws.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace chronicles {
namespace {

TEST(PlayerCommands, HelpIsSplitIntoReadablePrivateLinesAndUnknownCommandsAreConsumed) {
  PlayerCommands commands;
  EXPECT_FALSE(commands.execute({1, 42, "Player"}, "ordinary chat").handled);
  const auto help = commands.execute({1, 42, "Player"}, "!help");
  ASSERT_TRUE(help.handled);
  ASSERT_EQ(help.replies.size(), 8);
  for (const auto &line : help.replies) {
    EXPECT_LT(line.size(), 100);
    EXPECT_EQ(line.find('\n'), std::string::npos);
  }
  EXPECT_EQ(commands.execute({1, 42, "Player"}, "/help").replies, help.replies);
  const auto unknown = commands.execute({1, 42, "Player"}, "!unknown secret arguments");
  EXPECT_TRUE(unknown.handled);
  ASSERT_EQ(unknown.replies.size(), 1);
  EXPECT_EQ(unknown.replies.front().find("secret"), std::string::npos);
  EXPECT_EQ(commands.execute({1, 42, "Player"}, "!help extra").replies.front(), "Usage: !help");
  EXPECT_TRUE(commands.execute({1, 42, "Player"}, "/unknown").handled);
}

TEST(PlayerCommands, UnstuckDelegatesTheAuthenticatedCharacterAndRejectsArguments) {
  const ChatIdentity identity{1, 42, "Player"};
  PlayerCommands unavailable;
  EXPECT_NE(unavailable.execute(identity, "!unstuck").replies.front().find("unavailable"),
            std::string::npos);

  int calls = 0;
  PlayerCommands commands({}, Clock::now, {}, [&](const ChatIdentity &sender) {
    EXPECT_EQ(sender.character_id, identity.character_id);
    ++calls;
    return std::string{};
  });
  EXPECT_TRUE(commands.execute(identity, "!unstuck").replies.empty());
  EXPECT_EQ(commands.execute(identity, "!unstuck now").replies.front(), "Usage: !unstuck");
  EXPECT_EQ(calls, 1);
}

TEST(PlayerCommands, PvPDelegatesOnlyValidatedRequestsAndNeverInventsSuccess) {
  const ChatIdentity identity{1, 42, "Player"};
  PlayerCommands unavailable;
  EXPECT_NE(unavailable.execute(identity, "/pvp on").replies.front().find("unavailable"),
            std::string::npos);

  std::vector<std::optional<bool>> requests;
  PlayerCommands commands([&](const ChatIdentity &sender, std::optional<bool> enabled) {
    EXPECT_EQ(sender.character_id, identity.character_id);
    requests.push_back(enabled);
    return "PvP update requested.";
  });
  commands.execute(identity, "!pvp on");
  commands.execute(identity, "!pvp off");
  commands.execute(identity, "!pvp status");
  commands.execute(identity, "/pvp");
  commands.execute(identity, "!pvp maybe");
  commands.execute(identity, "!pvp on extra");
  commands.execute(identity, "/pvp on");
  ASSERT_EQ(requests.size(), 5);
  EXPECT_EQ(requests[0], true);
  EXPECT_EQ(requests[1], false);
  EXPECT_FALSE(requests[2].has_value());
  EXPECT_FALSE(requests[3].has_value());
  EXPECT_EQ(requests[4], true);
}

TEST(PlayerCommands, MountAndDismountDelegateWithoutArgumentsAndQueuedRequestsStayQuiet) {
  const ChatIdentity identity{1, 42, "Player"};
  PlayerCommands unavailable;
  EXPECT_NE(unavailable.execute(identity, "!mount").replies.front().find("unavailable"),
            std::string::npos);

  std::vector<PlayerMountAction> requests;
  std::string next_reply;
  PlayerCommands commands({}, Clock::now,
                          [&](const ChatIdentity &sender, PlayerMountAction action) {
                            EXPECT_EQ(sender.character_id, identity.character_id);
                            requests.push_back(action);
                            return next_reply;
                          });

  // A queued request produces no immediate line: the world session reports the outcome.
  const auto queued = commands.execute(identity, "!mount");
  EXPECT_TRUE(queued.handled);
  EXPECT_TRUE(queued.replies.empty());

  next_reply = "Enter the game world before using mount commands.";
  const auto refused = commands.execute(identity, "!dismount");
  ASSERT_EQ(refused.replies.size(), 1);
  EXPECT_EQ(refused.replies.front(), next_reply);

  EXPECT_EQ(commands.execute(identity, "!mount fast").replies.front(), "Usage: !mount");
  EXPECT_EQ(commands.execute(identity, "!dismount now").replies.front(), "Usage: !dismount");
  ASSERT_EQ(requests.size(), 2);
  EXPECT_EQ(requests[0], PlayerMountAction::mount);
  EXPECT_EQ(requests[1], PlayerMountAction::dismount);
}

TEST(PlayerCommands, RateLimitIsPerCharacterAndExpiresUsingMonotonicTime) {
  auto now = Clock::time_point{};
  PlayerCommands commands({}, [&] { return now; });
  for (unsigned index = 0; index < 10; ++index) {
    commands.execute({1, 42, "Player"}, index % 2 == 0 ? "!help" : "/help");
  }
  EXPECT_THROW(commands.execute({1, 42, "Player"}, "/help"), std::length_error);
  EXPECT_NO_THROW(commands.execute({1, 43, "Other"}, "/help"));
  now += std::chrono::seconds(10);
  EXPECT_NO_THROW(commands.execute({1, 42, "Player"}, "/help"));
  EXPECT_THROW(commands.execute({}, "/help"), Unauthorized);
  EXPECT_THROW(commands.execute({1, 42, "Player"}, "/help\nsecret"), std::invalid_argument);
}

TEST(PlayerCommands, FailedWorldCallbackCannotProduceASuccessReply) {
  PlayerCommands commands([](const ChatIdentity &, std::optional<bool>) -> std::string {
    throw std::runtime_error("World callback failed");
  });
  EXPECT_THROW(commands.execute({1, 42, "Player"}, "/pvp on"), std::runtime_error);

  PlayerCommands invalid_response(
      [](const ChatIdentity &, std::optional<bool>) { return std::string("Invalid\nreply"); });
  EXPECT_THROW(invalid_response.execute({1, 42, "Player"}, "/pvp on"), std::invalid_argument);
}

struct CommandPlayer {
  std::string token;
  std::uint64_t connection{};
};

CommandPlayer command_player(AuthService &auth, NativeChatService &chat, const std::string &name) {
  if (!auth.create_account(name, "command-test-password")) {
    throw std::runtime_error("Cannot create command test account");
  }
  const auto token = auth.login(name, "command-test-password").access_token();
  x5::pb::authwebsocket::WorldEnterReq enter;
  enter.set_world_id(1);
  if (auth.enter(token, enter).error_code() != x5::ec::Success) {
    throw std::runtime_error("Cannot enter command test realm");
  }
  x5::pb::ws::front::CharCreateReq create;
  create.set_slot(0);
  create.set_char_name(name);
  create.set_char_class(x5::cms::EX5CharacterClass_Wild);
  const auto created = auth.create(token, create);
  if (created.ec() != x5::ec::Success || !created.has_char_()) {
    throw std::runtime_error("Cannot create command test character");
  }
  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(created.char_().char_id());
  if (auth.select(token, select).ec() != x5::ec::Success) {
    throw std::runtime_error("Cannot select command test character");
  }
  return {token, chat.connect(token)};
}

TEST(PlayerCommands, NativeFrontInterceptsEveryChannelBeforePublicRouting) {
  const MemoryRepositories repositories;
  AuthService auth(repositories.character_repositories(), Config{});
  NativeWsDispatcher dispatcher(auth);
  auto &chat = dispatcher.chat();
  const auto alice = command_player(auth, chat, "CommandAlice");
  const auto observer = command_player(auth, chat, "CommandBob");

  for (const auto prefix : {'!', '/'}) {
    for (const auto type : {x5::cms::EX5ChatMessageType_World, x5::cms::EX5ChatMessageType_Local,
                            x5::cms::EX5ChatMessageType_Guild}) {
      x5::pb::ws::front::ChatReq request;
      request.set_type(type);
      request.set_message(std::string(1, prefix) + "help");
      EXPECT_TRUE(dispatcher
                      .handle(NativeWsChannel::front, alice.token,
                              native_ws::encode_frame(native_ws::kChatRequest, request))
                      .empty());
      const auto replies = chat.drain(alice.connection);
      ASSERT_EQ(replies.size(), 8);
      for (const auto &wire : replies) {
        const auto frames = native_ws::decode_frames(wire);
        ASSERT_EQ(frames.size(), 1);
        EXPECT_EQ(frames.front().packet_id, native_ws::kChatNotification);
        x5::pb::frontwebsocket::ChatNoti notification;
        ASSERT_TRUE(notification.ParseFromArray(frames.front().payload.data(),
                                                static_cast<int>(frames.front().payload.size())));
        EXPECT_EQ(notification.sender_name(), "[Server]");
      }
      EXPECT_TRUE(chat.drain(observer.connection).empty());
    }
  }
  EXPECT_TRUE(chat.world_chat_history(0, 100).records.empty());
}

} // namespace
} // namespace chronicles
