#include "world/features/chat/server_messages.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace chronicles {
namespace {

nlohmann::json configured_messages() {
  return {{"enabled", true},
          {"interval_seconds", 60},
          {"messages",
           {{{"type", "chat"}, {"text", "First scheduled message"}},
            {{"type", "announcement"}, {"text", "Second scheduled message"}}}},
          {"welcome", {{"enabled", true}, {"messages", {"Welcome!", "Type /help."}}}}};
}

TEST(ServerMessages, RotatesOnlyAfterIntervalAndNeverReplaysMissedIntervals) {
  const auto started = Clock::time_point{};
  std::vector<ServerMessage> delivered;
  ServerMessages service(
      parse_server_messages_config(configured_messages()),
      [&](const std::string &sender, const ServerMessage &message) {
        EXPECT_EQ(sender, "[Server]");
        delivered.push_back(message);
      },
      [](const auto &, const auto &, const auto &) {}, started);

  service.tick(started);
  service.tick(started + std::chrono::seconds(59));
  EXPECT_TRUE(delivered.empty());
  service.tick(started + std::chrono::seconds(60));
  ASSERT_EQ(delivered.size(), 1);
  EXPECT_EQ(delivered.front().type, ServerMessageType::chat);

  const auto late = started + std::chrono::hours(24);
  service.tick(late);
  service.tick(late);
  ASSERT_EQ(delivered.size(), 2);
  EXPECT_EQ(delivered.back().type, ServerMessageType::announcement);
  service.tick(late + std::chrono::seconds(60));
  ASSERT_EQ(delivered.size(), 3);
  EXPECT_EQ(delivered.back().text, "First scheduled message");
}

TEST(ServerMessages, WelcomeIsPrivateAndIndependentFromRecurringEnableFlag) {
  auto document = configured_messages();
  document["enabled"] = false;
  std::vector<std::string> welcomed;
  ServerMessages service(
      parse_server_messages_config(document), {},
      [&](const ChatIdentity &recipient, const std::string &sender, const std::string &message) {
        EXPECT_EQ(recipient.character_id, 42);
        EXPECT_EQ(recipient.world_id, 1);
        EXPECT_EQ(sender, "[Server]");
        welcomed.push_back(message);
      });
  service.tick(Clock::now() + std::chrono::hours(24));
  EXPECT_TRUE(welcomed.empty());
  service.world_entered({1, 42, "Selected"});
  EXPECT_EQ(welcomed, (std::vector<std::string>{"Welcome!", "Type /help."}));
  EXPECT_THROW(service.world_entered({}), Unauthorized);
}

TEST(ServerMessages, RuntimeConfigurationIsValidatedAtomicallyAndRestartsInterval) {
  const auto started = Clock::time_point{};
  unsigned deliveries = 0;
  ServerMessages service(
      parse_server_messages_config(configured_messages()),
      [&](const auto &, const auto &) { ++deliveries; },
      [](const auto &, const auto &, const auto &) {}, started);
  auto invalid = configured_messages();
  invalid["interval_seconds"] = 0;
  EXPECT_THROW(service.replace_configuration(invalid, started), std::invalid_argument);
  EXPECT_EQ(service.configuration()["interval_seconds"], 60);
  service.tick(started + std::chrono::seconds(60));
  EXPECT_EQ(deliveries, 1);

  auto replacement = configured_messages();
  replacement["interval_seconds"] = 120;
  service.replace_configuration(replacement, started + std::chrono::seconds(70));
  service.tick(started + std::chrono::seconds(189));
  EXPECT_EQ(deliveries, 1);
  service.tick(started + std::chrono::seconds(190));
  EXPECT_EQ(deliveries, 2);

  replacement["enabled"] = false;
  service.replace_configuration(replacement, started);
  service.tick(started + std::chrono::hours(24));
  EXPECT_EQ(deliveries, 2);
}

TEST(ServerMessages, RejectsTyposInvalidTypesControlsAndUnboundedConfiguration) {
  for (const auto &invalid :
       std::vector<nlohmann::json>{{{"enabled", true}},
                                   {{"enabld", false}},
                                   {{"interval_seconds", 1.5}},
                                   {{"interval_seconds", -1}},
                                   {{"interval_seconds", 604801}},
                                   {{"messages", {{{"type", "unknown"}, {"text", "Hello"}}}}},
                                   {{"welcome", {{"enabled", true}}}},
                                   {{"sender_name", std::string(33, 'x')}}}) {
    EXPECT_THROW(parse_server_messages_config(invalid), std::exception);
  }

  for (const auto &text : {std::string("Line\nbreak"), std::string("\xc0\xaf", 2),
                           std::string(1025, 'x'), std::string{}}) {
    auto document = configured_messages();
    document["messages"][0]["text"] = text;
    EXPECT_THROW(parse_server_messages_config(document), std::invalid_argument);
  }

  auto too_many = configured_messages();
  too_many["welcome"]["messages"] = std::vector<std::string>(9, "Bounded welcome");
  EXPECT_THROW(parse_server_messages_config(too_many), std::invalid_argument);
  EXPECT_NO_THROW(parse_server_messages_config(nlohmann::json::object()));
}

TEST(ServerMessages, RequiresDeliveryCallbackOnlyForEnabledFeatures) {
  EXPECT_NO_THROW(ServerMessages(ServerMessagesConfig{}, {}, {}));
  EXPECT_THROW(ServerMessages(parse_server_messages_config(configured_messages()), {}, {}),
               std::invalid_argument);
}

} // namespace
} // namespace chronicles
