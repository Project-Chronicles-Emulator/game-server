#include "testing/housing_test_support.hpp"
#include "testing/memory_repositories.hpp"
#include "world/features/housing/housing_bootstrap.hpp"
#include "world/features/housing/housing_feature.hpp"

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace chronicles::housing {
namespace {

using namespace testing_support;
using Json = nlohmann::json;
constexpr std::int64_t kStartTicks = 638'000'000'000'000'000;
constexpr std::uint16_t kZoneChannelMoveRequest = 20090;
const WorldPosition kPublicPosition{9012, 100, 200, 300, 4};

class RecordingPort final : public SessionPort {
public:
  void deliver(std::vector<WorldPacket>) override {
  }

  bool post(std::function<void(const ProgressAccess &)> task) override {
    task(ProgressAccess{});
    return true;
  }

  void travel(const HousingDestination &destination) override {
    travels.push_back(destination);
  }

  std::vector<HousingDestination> travels;
};

class HousingContext final : public ::chronicles::ProgressAccess, public HousingSessionContext {
public:
  WorldProgress load() override {
    ++loads;
    return progress;
  }

  std::optional<Character> character() override {
    ADD_FAILURE() << "Housing storage already has the authenticated identity";
    return std::nullopt;
  }

  std::optional<std::vector<CharacterItem>> commit(const WorldProgress &, const WorldProgress &,
                                                   const ItemTrade &) override {
    ADD_FAILURE() << "Housing must commit through its established domain service";
    return std::nullopt;
  }

  bool housing_has_handoff() const override {
    return authenticated;
  }

  bool housing_closed() const override {
    ++closed_queries;
    return closed || (close_during_attach && closed_queries == 2);
  }

  std::int64_t housing_account_id() const override {
    return 11;
  }

  std::int64_t housing_character_id() const override {
    return 17;
  }

  int housing_world_id() const override {
    return 1;
  }

  const std::string *housing_character_name() const override {
    return selected ? &name : nullptr;
  }

  std::uint64_t housing_connection_id() const override {
    return 23;
  }

  std::uint64_t housing_player_unit_id(std::int64_t character_id) const override {
    EXPECT_EQ(character_id, 17);
    return 0x10000011;
  }

  bool housing_has_tutorial() const override {
    return tutorial;
  }

  const WorldProgress *housing_tutorial_progress() const override {
    return tutorial ? &progress : nullptr;
  }

  void housing_publish_progress(const WorldProgress &committed) override {
    ++adoptions;
    if (adoption_failure == 1) {
      throw std::logic_error("stale tutorial cache");
    }
    if (adoption_failure == 2) {
      throw std::runtime_error("storage unavailable");
    }
    progress = committed;
  }

  std::shared_ptr<SessionPort> housing_session_port() override {
    return port;
  }

  std::optional<WorldPosition> housing_current_position() const override {
    ++position_queries;
    return current_position;
  }

  bool housing_begin_map_move(const WorldPosition &destination) override {
    calls.push_back("begin");
    if (!map_move_allowed) {
      return false;
    }
    session->on_map_departure();
    map_target = destination;
    return true;
  }

  std::optional<WorldPosition>
  housing_public_return_origin(const std::optional<WorldPosition> &origin) const override {
    calls.push_back("origin");
    return public_origin ? public_origin : origin;
  }

  std::optional<int> housing_raid_parent_map(int map_id) const override {
    calls.push_back("raid-parent");
    return map_id == 12001 ? raid_parent : std::nullopt;
  }

  WorldPosition
  housing_default_return_position(const std::optional<WorldPosition> &current) const override {
    calls.push_back("default-return");
    if (current && current->map_id == kHousingMapId) {
      return {kPublicPosition.map_id, current->x_mm, current->y_mm, current->z_mm, current->yaw};
    }
    return {8000, 1, 2, 3, 4};
  }

  void housing_clear_pending_exit() override {
    calls.push_back("clear-exit");
    pending_exit.reset();
  }

  void housing_stage_pending_exit(const WorldPosition &destination) override {
    calls.push_back("stage-exit");
    pending_exit = destination;
  }

  void housing_gm_reply(const std::string &line) override {
    replies.push_back(line);
  }

  WorldProgress progress;
  std::shared_ptr<RecordingPort> port{std::make_shared<RecordingPort>()};
  HousingSession *session{};
  std::optional<WorldPosition> public_origin;
  std::optional<WorldPosition> current_position{kPublicPosition};
  std::optional<WorldPosition> map_target;
  std::optional<WorldPosition> pending_exit{kPublicPosition};
  std::optional<int> raid_parent;
  mutable std::vector<std::string> calls;
  std::vector<std::string> replies;
  std::string name{"Builder"};
  int loads{};
  int adoptions{};
  int adoption_failure{};
  mutable int closed_queries{};
  mutable int position_queries{};
  bool authenticated{true};
  bool selected{true};
  bool tutorial{};
  bool closed{};
  bool close_during_attach{};
  bool map_move_allowed{true};
};

class ExpiryOwner final : public HousingExpiryOwner {
public:
  bool housing_listener_open() const override {
    ++open_queries;
    return open;
  }

  mutable int open_queries{};
  bool open{true};
};

class HousingSessionTest : public ::testing::Test {
protected:
  HousingSessionTest()
      : feature(io.get_executor(), workers.get_executor(), std::chrono::milliseconds(1)) {
    service = std::make_shared<HousingService>(
        housing_catalog(), std::make_shared<MemoryHousingStore>(repositories.character_state), 1,
        [this] {
          return now;
        });
    hub = std::make_shared<HousingHub>(service);
    feature.set_hub(hub);
    session = feature.create_session(context, context);
    context.session = session.get();
  }

  HousingDestination destination(int channel = 1,
                                 std::optional<WorldPosition> origin = kPublicPosition) const {
    return {kZoneKey, kHousingMapId, channel, {kHousingMapId, 1, 2, 3, 4}, origin};
  }

  void enter(int channel = 1, std::optional<WorldPosition> origin = kPublicPosition) {
    session->stage_entry(destination(channel, origin));
    ASSERT_TRUE(session->complete_entry());
  }

  void create_expired_site() {
    Character actor;
    actor.account_id = 11;
    actor.name = "ExpiryOwner";
    const auto created = repositories.characters->create(actor);
    const auto before = repositories.character_state->load_world_progress(11, created.id, 1);
    auto paid = before;
    ++paid.revision;
    paid.stackables[kPermitItem] = 60;
    ASSERT_TRUE(repositories.character_state->save_world_progress(11, created.id, 1, before, paid));
    const HousingActor owner{11, created.id, 1, actor.name, kPublicPosition};
    const auto contracted =
        service->contract(owner, SiteType::small, kZoneKey, ContractType::monthly, nullptr);
    ASSERT_EQ(contracted.ec, x5::ec::Success);
    const auto site = service->personal_site(created.id);
    ASSERT_TRUE(site);
    expired_character = created.id;
    now = site->expiry_epoch_ticks + 1;
  }

  boost::asio::io_context io;
  boost::asio::io_context workers;
  MemoryRepositories repositories;
  std::int64_t now{kStartTicks};
  CharacterId expired_character{};
  std::shared_ptr<HousingService> service;
  std::shared_ptr<HousingHub> hub;
  HousingFeature feature;
  HousingContext context;
  std::unique_ptr<HousingSession> session;
};

TEST_F(HousingSessionTest,
       TravelCapturesPublicOriginBeforeMapDepartureAndStagesOnlyAfterAcceptance) {
  context.public_origin = WorldPosition{9011, 50, 60, 70, 80};
  context.map_move_allowed = false;
  session->travel(destination());
  EXPECT_EQ(context.calls, (std::vector<std::string>{"origin", "begin"}));
  EXPECT_FALSE(session->has_pending_entry());
  EXPECT_TRUE(context.pending_exit.has_value());

  context.calls.clear();
  context.map_move_allowed = true;
  session->travel(destination());
  EXPECT_EQ(context.calls, (std::vector<std::string>{"origin", "begin", "clear-exit"}));
  EXPECT_TRUE(session->has_pending_entry());
  EXPECT_FALSE(context.pending_exit);
  ASSERT_TRUE(session->complete_entry());
  auto leave = FrameContext(kZoneChannelMoveRequest,
                            WorldGameplayRequest{kZoneChannelMoveRequest, 1, Json::object()});
  ASSERT_TRUE(session->handle_departure(leave));
  ASSERT_TRUE(context.pending_exit);
  EXPECT_EQ(*context.pending_exit, *context.public_origin);
}

TEST_F(HousingSessionTest, ArrivalConsumesOnlyItsOwnPendingEntryAndPublishesTheChannelMap) {
  EXPECT_FALSE(session->complete_entry());
  session->stage_entry(destination(7));
  ASSERT_TRUE(session->complete_entry());
  EXPECT_FALSE(session->has_pending_entry());
  EXPECT_TRUE(session->in_housing());
  EXPECT_EQ(session->presence_map(), -(kZoneKey * 1000 + 7));
  EXPECT_TRUE(context.pending_exit.has_value());
  session->on_map_departure();
  EXPECT_FALSE(session->in_housing());
  EXPECT_EQ(session->presence_map(), 0);
}

TEST_F(HousingSessionTest, ReleasedRaidOriginIsRefusedBeforeTravelAndValidParentIsUsed) {
  const WorldPosition raid{12001, 10, 20, 30, 40};
  enter(1, raid);
  context.raid_parent = 0;
  auto leave = FrameContext(kZoneChannelMoveRequest,
                            WorldGameplayRequest{kZoneChannelMoveRequest, 1, Json::object()});
  EXPECT_TRUE(session->handle_departure(leave));
  EXPECT_TRUE(session->in_housing());
  EXPECT_EQ(context.calls, std::vector<std::string>({"raid-parent"}));

  context.calls.clear();
  context.raid_parent = 9012;
  EXPECT_TRUE(session->handle_departure(leave));
  ASSERT_TRUE(context.pending_exit);
  EXPECT_EQ(context.pending_exit->map_id, 9012);
  EXPECT_EQ(context.pending_exit->x_mm, raid.x_mm);
  EXPECT_EQ(context.calls, (std::vector<std::string>{"raid-parent", "begin", "stage-exit"}));
}

// A character that logged in inside the housing area (or whose origin a restart dropped) used
// to be sent to the tutorial start when it walked out; it now steps out onto the parent map
// where it stands.
TEST_F(HousingSessionTest, LeavingWithoutARecordedOriginStepsOutOntoTheParentMapInPlace) {
  const WorldPosition boundary{kHousingMapId, -480000, 3300000, -540000, 1.5F};
  context.current_position = boundary;
  enter(1, std::nullopt);
  auto leave = FrameContext(kZoneChannelMoveRequest,
                            WorldGameplayRequest{kZoneChannelMoveRequest, 1, Json::object()});

  ASSERT_TRUE(session->handle_departure(leave));

  ASSERT_TRUE(context.pending_exit);
  EXPECT_EQ(*context.pending_exit,
            (WorldPosition{kPublicPosition.map_id, boundary.x_mm, boundary.y_mm, boundary.z_mm,
                           boundary.yaw}));
  EXPECT_EQ(context.calls,
            (std::vector<std::string>{"default-return", "raid-parent", "begin", "stage-exit"}));
}

// Walking into the housing area's cells enters it where the character stands, without a return
// point; walking out at a reported spot then steps onto the parent map there.
TEST_F(HousingSessionTest, WalkingInEntersInPlaceAndWalkingOutReturnsToTheReportedSpot) {
  const WorldPosition arrival{kHousingMapId, -480000, 3300000, -540000, 1.5F};
  context.public_origin.reset();

  ASSERT_TRUE(session->walk_in(arrival));
  ASSERT_TRUE(context.map_target);
  EXPECT_EQ(*context.map_target, arrival);
  ASSERT_TRUE(session->complete_entry());
  EXPECT_TRUE(session->in_housing());

  context.current_position = arrival;
  context.calls.clear();
  const nlohmann::json reported{
      {"pos", {{"millimeter_x", -481000}, {"millimeter_y", 3300500}, {"millimeter_z", -540000}}}};
  auto leave = FrameContext(kZoneChannelMoveRequest,
                            WorldGameplayRequest{kZoneChannelMoveRequest, 1, reported});
  ASSERT_TRUE(session->handle_departure(leave));
  ASSERT_TRUE(context.pending_exit);
  EXPECT_EQ(*context.pending_exit,
            (WorldPosition{kPublicPosition.map_id, -481000, 3300500, -540000, arrival.yaw}));

  // A map without a housing zone cannot be walked into.
  EXPECT_FALSE(session->walk_in(WorldPosition{9012, 0, 0, 0, 0.0F}));
}

TEST_F(HousingSessionTest, CloseRacingWithAttachmentRemovesThePublishedDirectoryEntry) {
  context.close_during_attach = true;
  EXPECT_TRUE(session->publish_presence(true, kPublicPosition).empty());
  EXPECT_FALSE(hub->travel(17, destination()));
  EXPECT_TRUE(context.port->travels.empty());
  session->on_leave_world();
}

TEST_F(HousingSessionTest, MissingIdentityCanRetryAttachmentAndAnAttachedOwnerPublishesOnce) {
  context.selected = false;
  EXPECT_TRUE(session->publish_presence(true, kPublicPosition).empty());
  EXPECT_FALSE(hub->travel(17, destination()));
  context.selected = true;
  session->publish_presence(true, kPublicPosition);
  EXPECT_TRUE(hub->travel(17, destination()));
  ASSERT_EQ(context.port->travels.size(), 1);
  const auto queries = context.closed_queries;
  session->publish_presence(true, kPublicPosition);
  EXPECT_EQ(context.closed_queries, queries);
  session->on_leave_world();
  EXPECT_FALSE(hub->travel(17, destination()));
}

TEST_F(HousingSessionTest, TutorialStorageSnapshotUsesTheLiveBorrowedProgress) {
  context.tutorial = true;
  context.progress.housing_items[kChairItem] = {3, 2};
  const auto packets = session->publish_presence(true, kPublicPosition);
  ASSERT_EQ(packets.size(), 1);
  EXPECT_EQ(packets[0].opcode, kWorldCharHousingItemsNotification);
  // Three owned, two placed: the wire quantity is the one copy left in storage.
  EXPECT_EQ(packets[0].message.at("housing_items")[0].at("quantity"), 1);
  EXPECT_EQ(packets[0].message.at("housing_items")[0].at("housing_installed_count"), 2);
  EXPECT_EQ(context.loads, 0);
}

TEST_F(HousingSessionTest, TutorialAdoptionLogicErrorStaysNarrowAndOtherFailuresAbortOnlyTheTask) {
  context.tutorial = true;
  context.adoption_failure = 1;
  bool reached_after_adopt = false;
  session->run_task([&](const ProgressAccess &access) {
    EXPECT_EQ(access.character_id, 17);
    EXPECT_EQ(access.load(), context.progress);
    access.adopt(context.progress);
    reached_after_adopt = true;
  });
  EXPECT_TRUE(reached_after_adopt);
  EXPECT_EQ(context.adoptions, 1);
  context.adoption_failure = 2;
  reached_after_adopt = false;
  EXPECT_NO_THROW(session->run_task([&](const ProgressAccess &access) {
    access.adopt(context.progress);
    reached_after_adopt = true;
  }));
  EXPECT_FALSE(reached_after_adopt);
  EXPECT_EQ(context.adoptions, 2);
}

TEST_F(HousingSessionTest, NonHousingAnimationFallsThroughWithoutReplacingEarlierPackets) {
  enter();
  FrameContext frame(20069, WorldGameplayRequest{20069, 1, Json::object()});
  frame.session_packets.push_back({99, {}});
  EXPECT_FALSE(session->handle_toggle(frame));
  ASSERT_EQ(frame.session_packets.size(), 1);
  EXPECT_EQ(frame.session_packets[0].opcode, 99);
}

TEST_F(HousingSessionTest, ExpiryTimerDoesNotRetainTheListenerOrPostAfterItExpires) {
  auto owner = std::make_shared<ExpiryOwner>();
  feature.schedule_expiry(owner);
  owner.reset();
  EXPECT_EQ(io.run_one(), 1);
  EXPECT_EQ(workers.poll(), 0);
}

TEST_F(HousingSessionTest, ClosedListenerPreventsExpiryWorkerPostingAndTimerRearm) {
  auto owner = std::make_shared<ExpiryOwner>();
  owner->open = false;
  feature.schedule_expiry(owner);
  EXPECT_EQ(io.run_one(), 1);
  EXPECT_EQ(owner->open_queries, 1);
  EXPECT_EQ(workers.poll(), 0);
  EXPECT_EQ(io.poll(), 0);
}

TEST_F(HousingSessionTest, ExpiryWorkerUsesASecondWeakLeaseAndSkipsAfterListenerDestruction) {
  create_expired_site();
  ASSERT_GT(expired_character, 0);
  auto owner = std::make_shared<ExpiryOwner>();
  feature.schedule_expiry(owner);
  EXPECT_EQ(io.run_one(), 1);
  owner.reset();
  feature.stop();
  EXPECT_EQ(workers.run(), 1);
  EXPECT_TRUE(service->personal_site(expired_character));
  hub->expire_due();
  EXPECT_FALSE(service->personal_site(expired_character));
}

TEST_F(HousingSessionTest, AlreadyQueuedExpiryRunsAfterStopWhileTheListenerStillExists) {
  create_expired_site();
  ASSERT_GT(expired_character, 0);
  auto owner = std::make_shared<ExpiryOwner>();
  feature.schedule_expiry(owner);
  EXPECT_EQ(io.run_one(), 1);
  owner->open = false;
  feature.stop();
  EXPECT_EQ(workers.run(), 1);
  EXPECT_FALSE(service->personal_site(expired_character));
  EXPECT_EQ(owner->open_queries, 1);
}

TEST_F(HousingSessionTest, ListenerDestructionCancelsExpiryWhileTheSharedHubRemainsAvailable) {
  auto owner = std::make_shared<ExpiryOwner>();
  feature.schedule_expiry(owner);
  feature.on_listener_destroyed();
  EXPECT_EQ(io.run_one(), 1);
  EXPECT_EQ(owner->open_queries, 0);
  EXPECT_EQ(workers.poll(), 0);
  EXPECT_TRUE(feature.enabled());
  EXPECT_TRUE(service->sites_in_channel(kZoneKey, 1).empty());
}

TEST(HousingFeature, AbsentHubArmsNoExpiryTimer) {
  boost::asio::io_context io;
  boost::asio::io_context workers;
  HousingFeature feature(io.get_executor(), workers.get_executor());
  auto owner = std::make_shared<ExpiryOwner>();
  feature.schedule_expiry(owner);
  EXPECT_EQ(io.poll(), 0);
  EXPECT_EQ(workers.poll(), 0);
  EXPECT_EQ(owner->open_queries, 0);
}

TEST(HousingBootstrap, OwnsTheCacheAndUsesTheSameFilteredOrderedSnapshotAsLiveHousingStorage) {
  WorldProgress progress;
  progress.housing_items[100] = {0, 5};
  progress.housing_items[300] = {2, 7};
  progress.housing_items[200] = {4, 1};
  const auto live = housing_items_packet(progress);
  ASSERT_TRUE(live);
  HousingBootstrap bootstrap;
  bootstrap.set_items(std::move(progress.housing_items));
  const auto cached = bootstrap.snapshot();
  ASSERT_TRUE(cached);
  EXPECT_EQ(cached->opcode, kWorldCharHousingItemsNotification);
  EXPECT_EQ(cached->message, live->message);
  ASSERT_EQ(cached->message.at("housing_items").size(), 2);
  EXPECT_EQ(cached->message.at("housing_items")[0].at("item_cms_id"), 200);
  EXPECT_EQ(cached->message.at("housing_items")[1].at("housing_installed_count"), 7);
  bootstrap.clear();
  EXPECT_FALSE(bootstrap.snapshot());
}

} // namespace
} // namespace chronicles::housing
