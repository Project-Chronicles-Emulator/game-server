#include "world/features/party/party_feature.hpp"

#include "testing/log_capture.hpp"

#include <gtest/gtest.h>

#include <new>

namespace chronicles::party {
namespace {

class PartyContext final : public PartySessionContext {
public:
  std::uint64_t party_connection_id() const override {
    return connection;
  }

  bool party_closed() const override {
    ++closed_queries;
    return closed || (close_after_first_query && closed_queries > 1);
  }

  bool party_has_tutorial() const override {
    return tutorial;
  }

  std::optional<nlohmann::json> party_character_stats(const Character &) const override {
    return initial_stats;
  }

  const nlohmann::json *party_combat_stats() const override {
    return combat_stats ? &*combat_stats : nullptr;
  }

  const WorldProgress *party_tutorial_progress() const override {
    return tutorial ? &progress : nullptr;
  }

  TimePoint party_now() const override {
    return Clock::time_point{};
  }

  std::uint64_t connection{71};
  bool closed{};
  bool close_after_first_query{};
  mutable int closed_queries{};
  bool tutorial{};
  std::optional<nlohmann::json> initial_stats{nlohmann::json{{"18", 1000}, {"69", 800}}};
  std::optional<nlohmann::json> combat_stats;
  WorldProgress progress;
};

Character character() {
  Character selected;
  selected.id = 17;
  selected.name = "Selected";
  selected.level = 8;
  selected.class_id = 3;
  return selected;
}

TEST(PartySession, SelectsStoredIdentityAndUsesPrivateChannelOnlyForTutorial) {
  boost::asio::io_context io;
  auto feature = std::make_shared<PartyFeature>(io, PartyWorldConfig{});
  PartyContext context;
  auto session = feature->create_session(context);
  session->select_character(character(), std::nullopt);
  ASSERT_NE(session->identity(), nullptr);
  EXPECT_EQ(session->identity()->char_id, 17);
  EXPECT_EQ(session->identity()->connection_id, 71U);
  EXPECT_EQ(session->identity()->channel_key, 0U);
  EXPECT_EQ(session->identity()->hp, 800);
  EXPECT_EQ(session->identity()->max_hp, 1000);

  context.tutorial = true;
  auto private_session = feature->create_session(context);
  private_session->select_character(character(), std::nullopt);
  EXPECT_EQ(private_session->identity()->channel_key, 17U);
}

TEST(PartySession, LiveVitalsDoNotMutateThePublishedCapturedIdentity) {
  boost::asio::io_context io;
  PartyFeature feature(io, PartyWorldConfig{});
  PartyContext context;
  auto session = feature.create_session(context);
  session->select_character(character(), std::nullopt);
  context.combat_stats = nlohmann::json{{"18", 1200}, {"69", 600}};
  const auto current = session->presence(WorldPosition{9012, 100, 200, 300, 0.0F});
  EXPECT_EQ(current.hp, 600);
  EXPECT_EQ(current.max_hp, 1200);
  EXPECT_EQ(session->identity()->hp, 800);
  EXPECT_FALSE(session->identity()->position);
}

TEST(PartySession, AttachesOnlyAfterStartAndRefreshesWithoutReattaching) {
  boost::asio::io_context io;
  PartyFeature feature(io, PartyWorldConfig{});
  PartyContext context;
  auto session = feature.create_session(context);
  session->select_character(character(), std::nullopt);
  const WorldPosition position{9012, 100, 200, 300, 0.0F};
  EXPECT_TRUE(session->on_enter_or_refresh(position, false).packets.empty());
  EXPECT_FALSE(feature.world()->connection_of(17));
  session->on_enter_or_refresh(position, true);
  EXPECT_TRUE(session->attached());
  EXPECT_EQ(feature.world()->connection_of(17), 71U);
  EXPECT_TRUE(session->on_enter_or_refresh(position, true).packets.empty());
  session->on_leave_world();
  EXPECT_FALSE(feature.world()->connection_of(17));
}

TEST(PartySession, FailedCloseStagingLeavesTheSessionAttachedForRetry) {
  boost::asio::io_context io;
  PartyFeature feature(io, PartyWorldConfig{});
  PartyContext context;
  auto session = feature.create_session(context);
  session->select_character(character(), std::nullopt);
  session->on_enter_or_refresh(WorldPosition{9012, 0, 0, 0, 0.0F}, true);

  EXPECT_THROW(session->on_leave_world_staged([](const PartyUpdate &) {
    throw std::bad_alloc();
  }), std::bad_alloc);
  EXPECT_EQ(feature.world()->connection_of(17), 71U);

  bool retained = false;
  session->on_leave_world_staged([&](const PartyUpdate &) {
    retained = true;
  });
  EXPECT_TRUE(retained);
  EXPECT_FALSE(feature.world()->connection_of(17));
}

TEST(PartySession, IoCloseBeforeWorkerAttachCompletionRemovesTheLateDirectoryEntry) {
  boost::asio::io_context io;
  PartyFeature feature(io, PartyWorldConfig{});
  PartyContext context;
  context.close_after_first_query = true;
  auto session = feature.create_session(context);
  session->select_character(character(), std::nullopt);
  session->on_enter_or_refresh(WorldPosition{9012, 0, 0, 0, 0.0F}, true);
  EXPECT_TRUE(session->attached());
  EXPECT_FALSE(feature.world()->connection_of(17));
}

TEST(PartySession, AStaleConnectionCannotDetachANewerDirectoryEntry) {
  boost::asio::io_context io;
  PartyFeature feature(io, PartyWorldConfig{});
  PartyContext old_context;
  auto old_session = feature.create_session(old_context);
  old_session->select_character(character(), std::nullopt);
  const WorldPosition position{9012, 0, 0, 0, 0.0F};
  old_session->on_enter_or_refresh(position, true);

  PartyContext new_context;
  new_context.connection = 72;
  auto new_session = feature.create_session(new_context);
  new_session->select_character(character(), std::nullopt);
  new_session->on_enter_or_refresh(position, true);
  old_session->on_leave_world();
  EXPECT_EQ(feature.world()->connection_of(17), 72U);
}

TEST(PartyFeature, QueuedTickDoesNotRetainTheWorldFeature) {
  boost::asio::io_context io;
  auto feature = std::make_shared<PartyFeature>(io, PartyWorldConfig{});
  std::weak_ptr<PartyFeature> weak_feature = feature;
  feature->schedule_tick();
  feature.reset();
  io.run();
  EXPECT_TRUE(weak_feature.expired());
}

TEST(PartyFeature, ArmedTimerDoesNotRetainTheFeatureOrDeliverAfterDestruction) {
  boost::asio::io_context io;
  auto feature = std::make_shared<PartyFeature>(io, PartyWorldConfig{});
  MemberPresence leader;
  leader.char_id = 17;
  leader.connection_id = 71;
  leader.name = "Leader";
  leader.party_unlocked = true;
  MemberPresence member = leader;
  member.char_id = 18;
  member.connection_id = 72;
  member.name = "Member";
  feature->world()->attach(leader, Clock::now());
  feature->world()->attach(member, Clock::now());
  const auto invitation = feature->world()->receive(17, 71, kPartyInvitePopupRequest,
                                                    {{"target_char_id", "18"}}, 41, Clock::now());
  ASSERT_FALSE(invitation.packets.empty());
  ASSERT_TRUE(feature->world()->active());
  feature->set_delivery([](const PartyUpdate &) {
    ADD_FAILURE() << "A destroyed feature must not deliver a timer update";
  });
  feature->schedule_tick();
  feature->schedule_tick();
  io.poll();

  std::weak_ptr<PartyFeature> weak_feature = feature;
  feature.reset();
  io.restart();
  io.run();
  EXPECT_TRUE(weak_feature.expired());
}

PartyUpdate update_with_one_unit_change() {
  PartyUpdate update;
  update.unit_changes.push_back({71, 17, 5});
  return update;
}

TEST(PartyFeature, TickUpdateWithoutDeliveryIsDroppedAndReportedOnce) {
  testing_support::LogCapture log;
  boost::asio::io_context io;
  PartyFeature feature(io, PartyWorldConfig{});

  feature.deliver_tick_update(update_with_one_unit_change());
  feature.deliver_tick_update(update_with_one_unit_change());

  EXPECT_EQ(log.count("Party tick update dropped"), 1U);
}

TEST(PartyFeature, EmptyTickUpdateWithoutDeliveryIsNotReported) {
  testing_support::LogCapture log;
  boost::asio::io_context io;
  PartyFeature feature(io, PartyWorldConfig{});

  feature.deliver_tick_update(PartyUpdate{});

  EXPECT_EQ(log.count("dropped"), 0U);
}

TEST(PartyFeature, InstalledDeliveryReceivesEveryTickUpdateWithoutReports) {
  testing_support::LogCapture log;
  boost::asio::io_context io;
  PartyFeature feature(io, PartyWorldConfig{});
  int deliveries{};
  feature.set_delivery([&deliveries](const PartyUpdate &) { ++deliveries; });

  feature.deliver_tick_update(update_with_one_unit_change());
  feature.deliver_tick_update(PartyUpdate{});

  EXPECT_EQ(deliveries, 2);
  EXPECT_EQ(log.count("dropped"), 0U);
}

} // namespace
} // namespace chronicles::party
