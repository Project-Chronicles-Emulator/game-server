#include "world/features/chat/chat.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <set>
#include <string>

namespace chronicles {
namespace {

using Json = nlohmann::json;

WorldChatIdentity sender_identity(std::int64_t character_id = 4242,
                                  std::uint64_t connection_id = 7,
                                  std::uint64_t unit_id = 900001,
                                  std::string name = "SessionName") {
  return {connection_id, character_id, unit_id, std::move(name)};
}

WorldChatAudience fixed_audience(std::vector<std::uint64_t> recipients) {
  return [recipients = std::move(recipients)](const WorldChatIdentity &, int) {
    return recipients;
  };
}

TEST(WorldChatService, LocalChatDeliversToAudienceWithSessionIdentity) {
  WorldChatService service;
  const auto result = service.receive(
      sender_identity(), {{"type", 1}, {"message", "hello"}},
      fixed_audience({7, 8}));

  ASSERT_EQ(result.status, WorldChatStatus::delivered);
  ASSERT_EQ(result.deliveries.size(), 2u);
  EXPECT_EQ(result.deliveries[0].recipient_connection_id, 7u);
  EXPECT_EQ(result.deliveries[1].recipient_connection_id, 8u);
  for (const auto &delivery : result.deliveries) {
    EXPECT_EQ(delivery.message.at("type").get<std::string>(), "EX5ChatMessageType_Local");
    EXPECT_EQ(delivery.message.at("message").get<std::string>(), "hello");
    // Identity comes from the session, not from the request body.
    EXPECT_EQ(delivery.message.at("sender_name").get<std::string>(), "SessionName");
    EXPECT_EQ(delivery.message.at("sender_unit_id").get<std::string>(), "900001");
    EXPECT_FALSE(delivery.message.contains("sender"));
  }
}

TEST(WorldChatService, WorldChannelAcceptsEnumNameAndIntegerForms) {
  WorldChatService service;
  for (const Json type : {Json(2), Json("World"), Json("EX5ChatMessageType_World")}) {
    const auto result = service.receive(sender_identity(),
                                        {{"type", type}, {"message", "w"}},
                                        fixed_audience({7}));
    ASSERT_EQ(result.status, WorldChatStatus::delivered) << type.dump();
    EXPECT_EQ(result.deliveries.front().message.at("type").get<std::string>(), "EX5ChatMessageType_World");
  }
}

TEST(WorldChatService, PartyChannelUsesWorldTransportOpcode) {
  WorldChatService service;
  const auto result = service.receive(sender_identity(),
                                      {{"type", 5}, {"message", "p"}},
                                      fixed_audience({7, 9, 9, 7}));
  ASSERT_EQ(result.status, WorldChatStatus::delivered);
  // Duplicate audience entries collapse to one delivery per connection.
  EXPECT_EQ(result.deliveries.size(), 2u);
  EXPECT_EQ(result.deliveries.front().message.at("type").get<std::string>(), "EX5ChatMessageType_Party");
}

TEST(WorldChatService, FrontOnlyChannelsAreRefusedNotBroadcast) {
  WorldChatService service;
  bool audience_called = false;
  const WorldChatAudience probe = [&](const WorldChatIdentity &, int) {
    audience_called = true;
    return std::vector<std::uint64_t>{7};
  };

  for (const int type : {3, 4, 6}) {
    const auto result = service.receive(sender_identity(),
                                        {{"type", type}, {"message", "x"}}, probe);
    EXPECT_EQ(result.status, WorldChatStatus::unsupported_channel) << type;
    EXPECT_TRUE(result.deliveries.empty());
  }
  EXPECT_FALSE(audience_called);
}

TEST(WorldChatService, MalformedInputsProduceNoDeliveries) {
  WorldChatService service;
  bool audience_called = false;
  const WorldChatAudience probe = [&](const WorldChatIdentity &, int) {
    audience_called = true;
    return std::vector<std::uint64_t>{7};
  };

  const std::vector<Json> malformed{
      Json::object({{"message", "x"}}),                   // missing type
      Json::object({{"type", 0}, {"message", "x"}}),      // None
      Json::object({{"type", 7}, {"message", "x"}}),      // outside enum
      Json::object({{"type", "Bogus"}, {"message", "x"}}),
      Json::object({{"type", true}, {"message", "x"}}),
      Json::object({{"type", 1}}),                        // missing message
      Json::object({{"type", 1}, {"message", 42}}),
      Json(5),                                            // not an object
  };
  for (const auto &request : malformed) {
    const auto result = service.receive(sender_identity(), request, probe);
    EXPECT_EQ(result.status, WorldChatStatus::malformed) << request.dump();
    EXPECT_TRUE(result.deliveries.empty());
  }
  EXPECT_FALSE(audience_called);
}

TEST(WorldChatService, MessagePolicyRejectsBadTextBeforeAudience) {
  WorldChatService service;
  bool audience_called = false;
  const WorldChatAudience probe = [&](const WorldChatIdentity &, int) {
    audience_called = true;
    return std::vector<std::uint64_t>{7};
  };

  EXPECT_EQ(service.receive(sender_identity(), {{"type", 1}, {"message", ""}}, probe).status,
            WorldChatStatus::empty_message);

  std::string oversized(kWorldChatMaximumMessageBytes + 1, 'a');
  EXPECT_EQ(service.receive(sender_identity(), {{"type", 1}, {"message", oversized}}, probe).status,
            WorldChatStatus::message_too_long);

  // Overlong encoding of '/' is invalid UTF-8.
  const std::string overlong{'\xc0', '\xaf'};
  EXPECT_EQ(service.receive(sender_identity(), {{"type", 1}, {"message", overlong}}, probe).status,
            WorldChatStatus::invalid_utf8);

  const std::string embedded_control{"hi\ttab"};
  EXPECT_EQ(
      service.receive(sender_identity(), {{"type", 1}, {"message", embedded_control}}, probe).status,
      WorldChatStatus::control_character);

  EXPECT_FALSE(audience_called);
}

TEST(WorldChatService, ValidUtf8MultibyteTextDelivers) {
  WorldChatService service;
  // U+20AC and U+1F600, both valid non-ASCII scalars.
  const std::string text{"\xe2\x82\xac\xf0\x9f\x98\x80"};
  const auto result =
      service.receive(sender_identity(), {{"type", 1}, {"message", text}}, fixed_audience({7}));
  ASSERT_EQ(result.status, WorldChatStatus::delivered);
  EXPECT_EQ(result.deliveries.front().message.at("message").get<std::string>(), text);
}

TEST(WorldChatService, RateLimitAppliesPerCharacterAndWindow) {
  WorldChatClock::time_point now;
  WorldChatService service([&now] { return now; });
  const WorldChatAudience two = fixed_audience({7, 8});

  for (unsigned sent = 0; sent < kWorldChatMaximumMessagesPerWindow; ++sent) {
    ASSERT_EQ(service.receive(sender_identity(), {{"type", 1}, {"message", "ok"}}, two).status,
              WorldChatStatus::delivered);
  }

  // Eleventh message in the window is refused; a different character still works.
  EXPECT_EQ(service.receive(sender_identity(), {{"type", 1}, {"message", "ok"}}, two).status,
            WorldChatStatus::rate_limited);
  EXPECT_EQ(service
                .receive(sender_identity(5150, 11, 900002, "Other"),
                         {{"type", 1}, {"message", "ok"}}, two)
                .status,
            WorldChatStatus::delivered);

  // After the window the same sender is allowed again.
  now += kWorldChatRateWindow;
  EXPECT_EQ(service.receive(sender_identity(), {{"type", 1}, {"message", "ok"}}, two).status,
            WorldChatStatus::delivered);
}

TEST(WorldChatService, EmptyAudienceCountsButDeliversNothing) {
  WorldChatService service;
  const auto result = service.receive(sender_identity(), {{"type", 1}, {"message", "x"}},
                                      fixed_audience({}));
  EXPECT_EQ(result.status, WorldChatStatus::no_recipients);
  EXPECT_TRUE(result.deliveries.empty());
}

TEST(WorldChatService, RejectedRequestsConsumeRateAllowance) {
  WorldChatClock::time_point now;
  WorldChatService service([&now] { return now; });
  bool audience_called = false;
  const WorldChatAudience probe = [&](const WorldChatIdentity &, int) {
    audience_called = true;
    return std::vector<std::uint64_t>{7};
  };

  // Ten malformed requests still consume the sender's whole window.
  for (unsigned sent = 0; sent < kWorldChatMaximumMessagesPerWindow; ++sent) {
    ASSERT_EQ(service.receive(sender_identity(), {{"type", 99}}, probe).status,
              WorldChatStatus::malformed);
  }

  const auto blocked =
      service.receive(sender_identity(), {{"type", 1}, {"message", "ok"}}, probe);
  EXPECT_EQ(blocked.status, WorldChatStatus::rate_limited);
  EXPECT_TRUE(blocked.deliveries.empty());
  EXPECT_FALSE(audience_called);
}

TEST(WorldChatService, RateIsKeyedByCharacterNotConnection) {
  WorldChatClock::time_point now;
  WorldChatService service([&now] { return now; });
  const WorldChatAudience two = fixed_audience({7, 8});

  for (unsigned sent = 0; sent < kWorldChatMaximumMessagesPerWindow; ++sent) {
    ASSERT_EQ(service.receive(sender_identity(), {{"type", 1}, {"message", "ok"}}, two).status,
              WorldChatStatus::delivered);
  }

  // The same character on a different connection id is still exhausted.
  EXPECT_EQ(service
                .receive(sender_identity(4242, 99, 900099, "SessionName"),
                         {{"type", 1}, {"message", "ok"}}, two)
                .status,
            WorldChatStatus::rate_limited);
}

TEST(WorldChatService, SenderTableEvictsOldestWindowInsteadOfStarving) {
  WorldChatClock::time_point now;
  WorldChatService service([&now] { return now; });
  const WorldChatAudience none = fixed_audience({});

  // Fill the bounded sender table with distinct characters over time.
  for (std::size_t id = 0; id < kWorldChatMaximumTrackedSenders; ++id) {
    now += std::chrono::milliseconds(1);
    ASSERT_EQ(service
                  .receive(sender_identity(static_cast<std::int64_t>(1000 + id),
                                           static_cast<std::uint64_t>(id + 1), 1, "c"),
                           {{"type", 1}, {"message", "x"}}, none)
                  .status,
              WorldChatStatus::no_recipients);
  }

  // A new sender is not starved: the oldest window is evicted to admit it.
  const auto admitted = service.receive(sender_identity(9999, 500, 900500, "New"),
                                        {{"type", 1}, {"message", "x"}}, fixed_audience({500}));
  EXPECT_EQ(admitted.status, WorldChatStatus::delivered);
  EXPECT_EQ(admitted.deliveries.front().recipient_connection_id, 500u);
}

} // namespace
} // namespace chronicles
