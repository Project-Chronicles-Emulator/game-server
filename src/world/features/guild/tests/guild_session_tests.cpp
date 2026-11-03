#include "world/features/guild/guild_feature.hpp"

#include "testing/log_capture.hpp"

#include <gtest/gtest.h>

namespace chronicles::guild {
namespace {

class GuildContext final : public GuildSessionContext {
public:
  std::optional<GuildActor> guild_identity() const override {
    ++identity_queries;
    return selected;
  }

  std::optional<CompanyStatus> guild_company_status(std::int64_t character_id) const override {
    EXPECT_EQ(character_id, 17);
    return company;
  }

  std::uint64_t guild_connection_id() const override {
    return 71;
  }

  std::int64_t guild_requester_id() const override {
    return selected ? selected->char_id : 0;
  }

  bool guild_closed() const override {
    ++closed_queries;
    return close_after_first_query && closed_queries > 1;
  }

  std::optional<GuildActor> selected{GuildActor{.world_id = 1, .char_id = 17, .name = "Selected"}};
  std::optional<CompanyStatus> company;
  mutable int identity_queries{};
  mutable int closed_queries{};
  bool close_after_first_query{};
};

std::shared_ptr<GuildService> guild_service() {
  GuildRules rules;
  rules.companies_enabled = false;
  return std::make_shared<GuildService>(std::make_shared<MemoryGuildStore>(), rules, 1);
}

TEST(GuildSession, EnrichesOnlyAuthenticatedActorWithLiveCompanyPosition) {
  GuildFeature feature(guild_service(), {});
  GuildContext context;
  CompanyStatus company;
  company.record.company_type = 2;
  company.position_level = 3;
  context.company = company;
  auto session = feature.create_session(context);
  const auto actor = session->actor();
  ASSERT_TRUE(actor);
  EXPECT_EQ(actor->char_id, 17);
  EXPECT_EQ(actor->company_type, 2);
  EXPECT_EQ(actor->company_position, 3);
  context.selected.reset();
  EXPECT_FALSE(session->actor());
}

TEST(GuildSession, DelaysEntrySnapshotAndDoesNotRepeatItOnDuplicateStarts) {
  GuildFeature feature(guild_service(), {});
  GuildContext context;
  auto session = feature.create_session(context);
  EXPECT_TRUE(session->on_enter_world(false).empty());
  EXPECT_EQ(context.identity_queries, 0);
  const auto entered = session->on_enter_world(true);
  ASSERT_EQ(entered.packets.size(), 1U);
  EXPECT_EQ(entered.packets[0].opcode, guild_world::kWorldCharGuildNotification);
  EXPECT_EQ(feature.world()->connection_of(17), 71U);
  EXPECT_TRUE(session->on_enter_world(true).empty());
  EXPECT_EQ(context.identity_queries, 1);
  session->on_leave_world();
  EXPECT_FALSE(feature.world()->connection_of(17));
}

TEST(GuildSession, CloseDuringWorkerAttachRemovesTheLateDirectoryEntry) {
  GuildFeature feature(guild_service(), {});
  GuildContext context;
  context.close_after_first_query = true;
  auto session = feature.create_session(context);
  session->on_enter_world(true);
  EXPECT_FALSE(feature.world()->connection_of(17));
}

TEST(GuildFeature, OptionalGuildServiceDoesNotConstructOrQueryAPlayerDirectory) {
  GuildFeature feature({}, {});
  GuildContext context;
  auto session = feature.create_session(context);
  EXPECT_FALSE(feature.world());
  EXPECT_TRUE(session->on_enter_world(true).empty());
  EXPECT_EQ(context.identity_queries, 0);
  EXPECT_NO_THROW(session->on_leave_world());
}

TEST(GuildSession, ActivityExperienceUsesTheExistingRealmServiceAndConversion) {
  auto service = guild_service();
  GuildFeature feature(service, {});
  GuildContext context;
  auto session = feature.create_session(context);
  GuildCreateRequest request;
  request.name = "Seafarers";
  const auto created = service->create(*session->actor(), request);
  ASSERT_EQ(created.error, x5::ec::Success);
  session->record_activity_experience(17, 25);
  const auto status = service->status(17);
  EXPECT_EQ(status.guild_id, created.guild.id);
  const auto detail = service->detail(*session->actor(), created.guild.id);
  EXPECT_EQ(detail.guild.experience, 2);
}

TEST(GuildFeature, ServiceSubscriptionRetainsOnlyWeakFeatureOwnership) {
  auto service = guild_service();
  auto feature = std::make_shared<GuildFeature>(service, [](const auto &) {
    ADD_FAILURE() << "Expired feature must not receive guild updates";
  });
  feature->subscribe();
  std::weak_ptr<GuildFeature> weak_feature = feature;
  feature.reset();
  EXPECT_TRUE(weak_feature.expired());
  GuildActor actor;
  actor.world_id = 1;
  actor.char_id = 17;
  actor.name = "Selected";
  GuildCreateRequest request;
  request.name = "Seafarers";
  EXPECT_EQ(service->create(actor, request).error, x5::ec::Success);
}

guild_world::GuildWorldUpdate update_with_one_unit_change() {
  guild_world::GuildWorldUpdate update;
  update.unit_changes.push_back({17, 5, "Seafarers"});
  return update;
}

TEST(GuildFeature, UpdateWithoutDeliveryIsDroppedAndReportedOnce) {
  testing_support::LogCapture log;
  GuildFeature feature(guild_service(), {});

  feature.deliver_update(update_with_one_unit_change());
  feature.deliver_update(update_with_one_unit_change());

  EXPECT_EQ(log.count("Guild update dropped"), 1U);
}

TEST(GuildFeature, EmptyUpdateIsNeitherDeliveredNorReported) {
  testing_support::LogCapture log;
  int deliveries{};
  GuildFeature feature(guild_service(), [&deliveries](const auto &) { ++deliveries; });

  feature.deliver_update(guild_world::GuildWorldUpdate{});

  EXPECT_EQ(deliveries, 0);
  EXPECT_EQ(log.count("dropped"), 0U);
}

TEST(GuildFeature, InstalledDeliveryReceivesNonEmptyUpdatesWithoutReports) {
  testing_support::LogCapture log;
  int deliveries{};
  GuildFeature feature(guild_service(), [&deliveries](const auto &) { ++deliveries; });

  feature.deliver_update(update_with_one_unit_change());

  EXPECT_EQ(deliveries, 1);
  EXPECT_EQ(log.count("dropped"), 0U);
}

TEST(GuildFeature, ServiceEventsReachTheDropReportWhenNoDeliveryIsInstalled) {
  testing_support::LogCapture log;
  auto service = guild_service();
  auto feature = std::make_shared<GuildFeature>(
      service, std::function<void(const guild_world::GuildWorldUpdate &)>{});
  feature->subscribe();

  GuildActor actor;
  actor.world_id = 1;
  actor.char_id = 17;
  actor.name = "Selected";
  GuildCreateRequest request;
  request.name = "Seafarers";
  ASSERT_EQ(service->create(actor, request).error, x5::ec::Success);

  EXPECT_EQ(log.count("Guild update dropped"), 1U);
}

} // namespace
} // namespace chronicles::guild
