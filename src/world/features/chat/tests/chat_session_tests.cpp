#include "world/features/chat/chat_feature.hpp"

#include <gtest/gtest.h>

#include <future>
#include <map>
#include <stdexcept>
#include <thread>

namespace chronicles::chat {
namespace {

class ChatContext final : public ChatSessionContext {
public:
  const party::MemberPresence *chat_party_identity() const override {
    return selected ? &identity : nullptr;
  }

  std::optional<int> chat_authenticated_world() const override {
    return authenticated ? std::optional<int>(1) : std::nullopt;
  }

  bool chat_has_handoff() const override {
    return authenticated;
  }

  ChatIdentity chat_exit_identity() const override {
    ++exit_identity_queries;
    return {1, identity.char_id, {}};
  }

  ChatIdentity chat_entry_identity() const override {
    return {1, identity.char_id, identity.name};
  }

  std::uint64_t chat_connection_id() const override {
    return identity.connection_id;
  }

  bool chat_closed() const override {
    return closed;
  }

  std::vector<std::uint64_t> chat_party_connections(std::int64_t character_id) const override {
    EXPECT_EQ(character_id, identity.char_id);
    ++audience_queries;
    return connections;
  }

  ChatFeature::EntryHandler chat_entry_handler() const override {
    return entry;
  }

  ChatFeature::EntryHandler chat_exit_handler() const override {
    return exit;
  }

  ChatFeature::CommandHandler chat_command_handler() const override {
    return command;
  }

  party::MemberPresence identity{.char_id = 17, .connection_id = 71, .name = "Selected"};
  bool selected{true};
  bool authenticated{true};
  bool closed{};
  mutable int audience_queries{};
  mutable int exit_identity_queries{};
  std::vector<std::uint64_t> connections{71, 72};
  ChatFeature::EntryHandler entry;
  ChatFeature::EntryHandler exit;
  ChatFeature::CommandHandler command;
};

WorldGameplayRequest request(int type = 5) {
  return {20038, 41, {{"type", type}, {"message", "ready"}}};
}

TEST(ChatSession, UsesAuthenticatedIdentityAndRetainsPartyDeliveryOrder) {
  ChatFeature feature;
  ChatContext context;
  auto session = feature.create_session(context);
  party::PartyUpdate update;
  EXPECT_TRUE(session->receive(request(), update).empty());
  ASSERT_EQ(update.packets.size(), 2U);
  EXPECT_EQ(update.packets[0].connection_id, 71U);
  EXPECT_EQ(update.packets[1].connection_id, 72U);
  EXPECT_EQ(update.packets[0].message.at("sender_name"), "Selected");
  EXPECT_EQ(context.audience_queries, 1);
}

TEST(ChatSession, InterceptsPrivateCommandsBeforeConsumingChatAllowance) {
  ChatFeature feature;
  ChatContext context;
  context.command = [](const ChatIdentity &identity, const std::string &message) {
    EXPECT_EQ(identity.character_id, 17);
    EXPECT_EQ(message, "ready");
    return true;
  };
  auto session = feature.create_session(context);
  party::PartyUpdate update;
  for (unsigned index = 0; index < kWorldChatMaximumMessagesPerWindow + 1; ++index) {
    EXPECT_TRUE(session->receive(request(), update).empty());
  }
  EXPECT_TRUE(update.packets.empty());
  EXPECT_EQ(context.audience_queries, 0);

  context.command = {};
  EXPECT_TRUE(session->receive(request(), update).empty());
  EXPECT_EQ(update.packets.size(), 2U);
}

TEST(ChatSession, NonPartyChannelsDoNotUsePartyAudienceOrNotJoinedError) {
  ChatFeature feature;
  ChatContext context;
  auto session = feature.create_session(context);
  party::PartyUpdate update;
  EXPECT_TRUE(session->receive(request(1), update).empty());
  EXPECT_TRUE(update.packets.empty());
  EXPECT_EQ(context.audience_queries, 0);
}

TEST(ChatSession, UnjoinedPartyUsesOriginalSequenceAndMissingIdentityHasNoAudience) {
  ChatFeature feature;
  ChatContext context;
  context.connections.clear();
  auto session = feature.create_session(context);
  party::PartyUpdate update;
  const auto packets = session->receive(request(), update);
  ASSERT_EQ(packets.size(), 1U);
  EXPECT_EQ(packets[0].opcode, party::kCommonErrorResponse);
  EXPECT_EQ(packets[0].message,
            party::PartyWorld::error_message(party::PartyError::kNotJoined, 41));

  context.selected = false;
  EXPECT_TRUE(session->receive(request(), update).empty());
  EXPECT_EQ(context.audience_queries, 1);
}

TEST(ChatSession, LateEntryAfterIoCloseReportsOnePairedDeparture) {
  ChatFeature feature;
  ChatContext context;
  auto session = feature.create_session(context);
  std::vector<std::string> calls;
  context.entry = [&](const ChatIdentity &) {
    calls.push_back("entry");
    context.closed = true;
    session->on_leave_world();
  };
  context.exit = [&](const ChatIdentity &identity) {
    calls.push_back("exit");
    EXPECT_TRUE(identity.name.empty());
  };

  session->on_leave_world();
  EXPECT_EQ(context.exit_identity_queries, 0);
  session->on_enter_world(true);
  session->on_leave_world();
  EXPECT_EQ(calls, (std::vector<std::string>{"entry", "exit"}));
  EXPECT_EQ(context.exit_identity_queries, 1);
}

TEST(ChatSession, FailedEntryLeavesExternalPairingUncertain) {
  ChatFeature feature;
  ChatContext context;
  auto session = feature.create_session(context);
  int departures{};
  context.entry = [](const ChatIdentity &) {
    throw std::runtime_error("entry failed");
  };
  context.exit = [&](const ChatIdentity &) {
    ++departures;
    throw std::runtime_error("exit failed");
  };
  EXPECT_THROW(session->on_enter_world(true), std::runtime_error);
  EXPECT_EQ(session->on_leave_world(), ChatSession::DepartureStatus::uncertain);
  EXPECT_EQ(session->departure_status(), ChatSession::DepartureStatus::uncertain);
  EXPECT_EQ(departures, 0);
}

TEST(ChatSession, FailedExitStaysUncertainAndIsNotReplayed) {
  ChatFeature feature;
  ChatContext context;
  auto session = feature.create_session(context);
  int departures{};
  context.entry = [](const ChatIdentity &) {
  };
  context.exit = [&](const ChatIdentity &) {
    ++departures;
    throw std::runtime_error("exit failed");
  };
  session->on_enter_world(true);
  EXPECT_EQ(session->on_leave_world(), ChatSession::DepartureStatus::uncertain);
  EXPECT_EQ(session->on_leave_world(), ChatSession::DepartureStatus::uncertain);
  EXPECT_EQ(session->departure_status(), ChatSession::DepartureStatus::uncertain);
  EXPECT_EQ(departures, 1);
}

TEST(ChatSession, ConcurrentDepartureWaitsForOneExternalExit) {
  ChatFeature feature;
  ChatContext context;
  auto session = feature.create_session(context);
  std::promise<void> entered;
  std::promise<void> release;
  auto released = release.get_future();
  int departures{};
  context.entry = [](const ChatIdentity &) {
  };
  context.exit = [&](const ChatIdentity &) {
    ++departures;
    entered.set_value();
    released.wait();
  };
  session->on_enter_world(true);
  auto first = std::async(std::launch::async, [&] {
    return session->on_leave_world();
  });
  entered.get_future().wait();
  EXPECT_EQ(session->departure_status(), ChatSession::DepartureStatus::pending);
  EXPECT_EQ(session->on_leave_world(), ChatSession::DepartureStatus::pending);
  release.set_value();
  EXPECT_EQ(first.get(), ChatSession::DepartureStatus::complete);
  EXPECT_EQ(session->departure_status(), ChatSession::DepartureStatus::complete);
  EXPECT_EQ(departures, 1);
}

TEST(ChatSession, LateEntryFailedDepartureRemainsUncertainWithoutReplay) {
  ChatFeature feature;
  ChatContext context;
  auto session = feature.create_session(context);
  int departures{};
  context.entry = [&](const ChatIdentity &) {
    context.closed = true;
  };
  context.exit = [&](const ChatIdentity &) {
    ++departures;
    throw 1;
  };

  EXPECT_EQ(session->on_leave_world(), ChatSession::DepartureStatus::complete);
  session->on_enter_world(true);
  EXPECT_EQ(session->departure_status(), ChatSession::DepartureStatus::uncertain);
  EXPECT_EQ(session->on_leave_world(), ChatSession::DepartureStatus::uncertain);
  EXPECT_EQ(departures, 1);
}

class LocalView final : public LocalChatView {
public:
  std::optional<std::uint64_t> speaker_connection(std::int64_t character_id) override {
    EXPECT_EQ(character_id, 17);
    return connection;
  }

  std::vector<std::uint64_t> observers(std::uint64_t connection_id) const override {
    EXPECT_EQ(connection_id, 71U);
    return visible;
  }

  std::optional<std::int64_t> observer_character(std::uint64_t connection_id) override {
    const auto found = characters.find(connection_id);
    return found == characters.end() ? std::nullopt : std::optional<std::int64_t>(found->second);
  }

  std::optional<std::uint64_t> connection{71};
  std::vector<std::uint64_t> visible{74, 72, 75, 71};
  std::map<std::uint64_t, std::int64_t> characters{{74, 24}, {72, 22}, {71, 17}};
};

TEST(ChatFeature, LocalAudienceKeepsEchoAndRegistryOrderWhileSkippingInactiveObservers) {
  ChatFeature feature;
  LocalView view;
  EXPECT_EQ(feature.local_audience_locked(17, view), (std::vector<std::int64_t>{17, 24, 22}));
  view.visible.clear();
  EXPECT_EQ(feature.local_audience_locked(17, view), (std::vector<std::int64_t>{17}));
  view.connection.reset();
  EXPECT_TRUE(feature.local_audience_locked(17, view).empty());
}

} // namespace
} // namespace chronicles::chat
