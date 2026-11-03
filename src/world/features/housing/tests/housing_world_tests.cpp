#include "testing/housing_test_support.hpp"
#include "testing/memory_repositories.hpp"
#include "world/features/housing/housing_world.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace chronicles::housing {
namespace {

using namespace testing_support;
using Json = nlohmann::json;

constexpr std::int64_t kStartTicks = 638'000'000'000'000'000;
constexpr int kWorldId = 1;

// A synchronous session port: tasks run immediately, packets are recorded.
class RecordingPort final : public SessionPort {
public:
  void deliver(std::vector<WorldPacket> packets) override {
    for (auto &packet : packets) {
      delivered.push_back(std::move(packet));
    }
  }

  bool post(std::function<void(const ProgressAccess &)> task) override {
    ++posted_tasks;
    task(access);
    return true;
  }

  void travel(const HousingDestination &destination) override {
    travels.push_back(destination);
  }

  std::vector<std::uint16_t> opcodes() const {
    std::vector<std::uint16_t> result;
    for (const auto &packet : delivered) {
      result.push_back(packet.opcode);
    }
    return result;
  }

  ProgressAccess access;
  std::vector<WorldPacket> delivered;
  std::vector<HousingDestination> travels;
  int posted_tasks{};
  int adoptions{};
};

// Holds one task so timeout and task-start ordering can be controlled independently.
class QueuedPort final : public SessionPort {
public:
  void deliver(std::vector<WorldPacket>) override {
  }

  bool post(std::function<void(const ProgressAccess &)> task) override {
    {
      std::lock_guard lock(mutex_);
      task_ = std::move(task);
    }
    posted_.set_value();
    return true;
  }

  void travel(const HousingDestination &) override {
  }

  void wait_until_posted() {
    posted_.get_future().wait();
  }

  void run_task() {
    std::function<void(const ProgressAccess &)> task;
    {
      std::lock_guard lock(mutex_);
      task = std::move(task_);
    }
    task(ProgressAccess{});
  }

private:
  std::mutex mutex_;
  std::promise<void> posted_;
  std::function<void(const ProgressAccess &)> task_;
};

class HousingWorldTest : public testing::Test {
protected:
  void SetUp() override {
    auto housing_store =
        std::make_shared<MemoryHousingStore>(repositories.character_state);
    service = std::make_shared<HousingService>(housing_catalog(), housing_store, kWorldId,
                                               [this] { return now; });
    hub = std::make_shared<HousingHub>(service);
    owner = create("Builder", 11);
    visitor = create("Visitor", 12);
    stranger = create("Stranger", 13);
  }

  struct Online {
    HousingActor actor;
    std::shared_ptr<RecordingPort> port;
  };

  Online create(const std::string &name, std::int64_t account_id) {
    Character created;
    created.account_id = account_id;
    created.name = name;
    const auto character = repositories.characters->create(created);
    Online online;
    online.actor = HousingActor{account_id, character.id, kWorldId, name,
                                WorldPosition{8000, 1, 2, 3, 0.0F}};
    online.port = std::make_shared<RecordingPort>();
    online.port->access.character_id = character.id;
    online.port->access.load = [this, actor = online.actor] {
      return repositories.character_state->load_world_progress(actor.account_id, actor.character_id,
                                                               kWorldId);
    };
    online.port->access.adopt = [port = online.port.get()](const WorldProgress &) {
      ++port->adoptions;
    };
    hub->attach(online.actor, static_cast<std::uint64_t>(character.id), 100 + character.id,
                online.port);
    return online;
  }

  void give(const HousingActor &actor, std::map<int, int> stackables,
            std::map<int, HousingItemRecord> housing_items = {}) {
    const auto before = repositories.character_state->load_world_progress(
        actor.account_id, actor.character_id, kWorldId);
    auto after = before;
    after.revision = before.revision + 1;
    for (const auto &[cms_id, count] : stackables) {
      after.stackables[cms_id] += count;
    }
    for (const auto &[cms_id, record] : housing_items) {
      after.housing_items[cms_id] = record;
    }
    ASSERT_TRUE(repositories.character_state->save_world_progress(
        actor.account_id, actor.character_id, kWorldId, before, after));
  }

  void enter_channel(const Online &online, int channel = 1) {
    hub->set_location(online.actor.character_id,
                      static_cast<std::uint64_t>(online.actor.character_id),
                      HousingLocation{kZoneKey, channel});
  }

  // A PC2S_GmCmdTextReq (20009) line from `online`, as the world session forwards it.
  std::optional<GmHousingReply> gm_text(const Online &online, const std::string &text) {
    constexpr std::uint16_t kGmTextRequest = 20009;
    constexpr std::uint32_t kSequence = 9;
    return hub->receive_gm(online.actor, kGmTextRequest, Json{{"text", text}}, kSequence,
                           online.port->access);
  }

  std::int64_t now{kStartTicks};
  MemoryRepositories repositories;
  std::shared_ptr<HousingService> service;
  std::shared_ptr<HousingHub> hub;
  Online owner;
  Online visitor;
  Online stranger;
};

TEST_F(HousingWorldTest, TimedOutQueuedOperationCannotRunLater) {
  auto port = std::make_shared<QueuedPort>();
  auto timed_hub = std::make_shared<HousingHub>(service, std::chrono::milliseconds(30));
  timed_hub->attach(owner.actor, 1, 1, port);
  std::atomic<bool> operation_ran{false};

  auto caller = std::async(std::launch::async, [&] {
    return timed_hub->run_for(owner.actor.character_id, [&](const ProgressAccess *) {
      operation_ran.store(true);
    });
  });
  port->wait_until_posted();
  EXPECT_FALSE(caller.get());

  port->run_task();
  EXPECT_FALSE(operation_ran.load());
}

TEST_F(HousingWorldTest, RunningOperationFinishesBeforeCallerCanReturn) {
  auto port = std::make_shared<QueuedPort>();
  auto timed_hub = std::make_shared<HousingHub>(service, std::chrono::milliseconds(30));
  timed_hub->attach(owner.actor, 1, 1, port);
  std::promise<void> entered;
  std::promise<void> release;
  auto entered_future = entered.get_future();
  auto release_future = release.get_future();
  int result = 0;

  auto caller = std::async(std::launch::async, [&] {
    return timed_hub->run_for(owner.actor.character_id, [&](const ProgressAccess *) {
      entered.set_value();
      release_future.wait();
      result = 42;
    });
  });
  port->wait_until_posted();
  auto worker = std::async(std::launch::async, [&] { port->run_task(); });
  entered_future.wait();
  EXPECT_EQ(caller.wait_for(std::chrono::milliseconds(80)), std::future_status::timeout);

  release.set_value();
  worker.get();
  EXPECT_TRUE(caller.get());
  EXPECT_EQ(result, 42);
}

TEST(HousingPayloads, HousingNubCarriesSiteLocalObjects) {
  SiteRecord site;
  site.key = SiteKey{kZoneKey, 1, kSmallLot};
  site.owner_character_id = 42;
  site.owner_name = "Builder";
  site.contract_type = ContractType::periodic;
  site.expiry_epoch_ticks = 1234;
  site.contents.house_part_custom_ids = std::string("\x01\x02", 2);
  HouseRecord house;
  house.house_id = 1;
  house.attach_index = -1;
  house.housing_id = kMainBuilding;
  house.position = {-4800, -300, 0};
  house.parts_state = 5;
  site.contents.houses.emplace(1, house);
  DoodadRecord doodad;
  doodad.doodad_id = 7;
  doodad.house_id = 1;
  doodad.cms_id = kChairDoodad;
  doodad.phase = 1;
  site.contents.doodads.emplace(DoodadKey{1, 7}, doodad);

  const auto packet = housing_add_packet(site, *housing_catalog());
  EXPECT_EQ(packet.opcode, kHousingAddNotification);
  EXPECT_EQ(packet.message.at("sector_id"),
            housing_catalog()->site(kSmallLot)->sector_id);
  const auto &nub = packet.message.at("nub");
  EXPECT_EQ(nub.at("lot_number"), kSmallLot);
  EXPECT_EQ(nub.at("owner_char_id"), "42");
  EXPECT_EQ(nub.at("expiry_epochtime"), "1234");
  EXPECT_EQ(nub.at("contract_type"), 2);
  EXPECT_EQ(nub.at("house_part_custom_ids"), "AQI=");
  EXPECT_EQ(nub.at("houses").at("1").at("pos").at("millimeter_x"), -4800);
  EXPECT_EQ(nub.at("houses").at("1").at("parts_state"), "5");
  EXPECT_EQ(nub.at("housing_doodads").at(std::to_string((1 << 14) | 7)).at("cms_id"),
            kChairDoodad);
}

TEST(HousingPayloads, UpdateRequestDecodesProtobufJson) {
  const auto doodad_unit = housing_doodad_unit_id(kSmallLot, 0, 3);
  const Json message{
      {"housing_lot_number", kSmallLot},
      {"house_part_custom_ids", "AQID"},
      {"house_nubs",
       Json::array({{{"house_id", 2},
                     {"attach_index", 3},
                     {"cms_id", kFacility},
                     {"pos", {{"millimeter_x", 10}, {"millimeter_y", -20}}},
                     {"yaw", 90.0}}})},
      {"housing_doodad_nubs",
       Json::array({{{"doodad_id", 9}, {"cms_id", kChairDoodad}, {"roll", 1.5}}})},
      {"remove_house_ids", Json::array({4})},
      {"remove_doodad_unit_ids", Json::array({std::to_string(doodad_unit)})}};

  const auto request = parse_update_request(message);
  EXPECT_EQ(request.lot_number, kSmallLot);
  ASSERT_TRUE(request.house_part_custom_ids.has_value());
  EXPECT_EQ(*request.house_part_custom_ids, std::string("\x01\x02\x03", 3));
  ASSERT_EQ(request.houses.size(), 1U);
  EXPECT_EQ(request.houses.front().position, (LocalPosition{10, -20, 0}));
  EXPECT_FLOAT_EQ(request.houses.front().yaw, 90.0F);
  ASSERT_EQ(request.doodads.size(), 1U);
  EXPECT_FLOAT_EQ(request.doodads.front().roll, 1.5F);
  EXPECT_EQ(request.remove_house_ids, std::vector<std::uint32_t>{4});
  EXPECT_EQ(request.remove_doodad_unit_ids, std::vector<std::uint64_t>{doodad_unit});

  EXPECT_THROW(parse_update_request(Json{{"house_nubs", "nope"}}), std::invalid_argument);
  EXPECT_THROW(parse_update_request(Json{{"housing_lot_number", "x"}}), std::invalid_argument);
}

TEST(HousingPayloads, UpdateRequestTreatsMissingAttachIndexAsMainBuilding) {
  // Shape of a stock-client main-building move: attach_index is left unset.
  const Json message{
      {"housing_lot_number", kSmallLot},
      {"house_nubs",
       Json::array({{{"house_id", 1},
                     {"cms_id", 1002},
                     {"pos", {{"millimeter_x", -10349}, {"millimeter_y", -5250}}}}})}};

  const auto request = parse_update_request(message);

  ASSERT_EQ(request.houses.size(), 1U);
  EXPECT_EQ(request.houses.front().attach_index, kUnattachedHouseIndex);
}

TEST(HousingPayloads, ProgressUpdateListsOnlyChangedRows) {
  ProgressNotice notice;
  notice.before.stackables = {{kPermitItem, 100}, {5, 1}};
  notice.before.hersander_token = 2000;
  notice.after = notice.before;
  notice.after.stackables[kPermitItem] = 40;
  notice.after.hersander_token = 1000;
  notice.after.housing_items[kChairItem] = {1, 1};

  const auto packet = progress_update_packet(notice);
  ASSERT_TRUE(packet.has_value());
  EXPECT_EQ(packet->opcode, kItemsUpdateNotification);
  EXPECT_TRUE(packet->message.contains("housing"));
  ASSERT_EQ(packet->message.at("stackables").size(), 1U);
  EXPECT_EQ(packet->message.at("stackables")[0].at("quantity"), 40);
  EXPECT_EQ(packet->message.at("money_changes")[0].at("change"), "-1000");
  EXPECT_EQ(packet->message.at("money_changes")[0].at("money_type"),
            "EX5MoneyType_HersanderToken");
  EXPECT_EQ(packet->message.at("housing_items")[0].at("housing_installed_count"), 1);
  EXPECT_EQ(packet->message.at("housing_items")[0].at("quantity"), 0);

  ProgressNotice unchanged;
  EXPECT_FALSE(progress_update_packet(unchanged).has_value());
}

TEST_F(HousingWorldTest, ContractsAreAnnouncedOnlyToTheChannelsViewers) {
  give(owner.actor, {{kPermitItem, 60}});
  enter_channel(visitor);

  ContractResult result;
  ASSERT_TRUE(hub->run_for(owner.actor.character_id, [&](const ProgressAccess *access) {
    result = service->contract(owner.actor, SiteType::small, kZoneKey, ContractType::monthly,
                               access);
  }));
  ASSERT_EQ(result.ec, x5::ec::Success);
  EXPECT_EQ(owner.port->posted_tasks, 1);
  EXPECT_EQ(owner.port->adoptions, 1);
  hub->publish(result.outcome, owner.actor.character_id);

  // The visitor stands in channel 1 and sees the new house; the stranger is elsewhere.
  ASSERT_EQ(visitor.port->opcodes(), std::vector<std::uint16_t>{kHousingAddNotification});
  EXPECT_TRUE(stranger.port->delivered.empty());
  // The owner learns its own site and its permit balance.
  EXPECT_EQ(owner.port->opcodes(),
            (std::vector<std::uint16_t>{kCharSiteUpdateNotification, kItemsUpdateNotification}));
  EXPECT_EQ(owner.port->delivered[0].message.at("housing_site").at("lot_number"), kSmallLot);
}

TEST_F(HousingWorldTest, WorldRequestsUseTheCurrentChannel) {
  give(owner.actor, {{kPermitItem, 60}}, {{kChairItem, {1, 0}}});
  ASSERT_EQ(service->contract(owner.actor, SiteType::small, kZoneKey, ContractType::monthly,
                              nullptr)
                .ec,
            x5::ec::Success);

  // Outside the housing area, edit mode is refused.
  auto refused = hub->receive(owner.actor, 501, kEditModeRequest, Json::object(), 3,
                              owner.port->access);
  ASSERT_EQ(refused.size(), 1U);
  EXPECT_EQ(refused[0].message.at("ec"), "Housing_NotOwner");

  enter_channel(owner);
  enter_channel(visitor);
  auto entered = hub->receive(owner.actor, 501, kEditModeRequest, Json::object(), 4,
                              owner.port->access);
  ASSERT_EQ(entered.size(), 1U);
  EXPECT_EQ(entered[0].opcode, kEditModeNotification);
  EXPECT_EQ(entered[0].message.at("ec"), "Success");
  EXPECT_EQ(entered[0].message.at("unit_id"), "501");
  EXPECT_EQ(visitor.port->opcodes(), std::vector<std::uint16_t>{kEditModeNotification});

  const Json update{
      {"housing_lot_number", kSmallLot},
      {"housing_doodad_nubs",
       Json::array({{{"doodad_id", 9},
                     {"cms_id", kChairDoodad},
                     {"pos", {{"millimeter_x", 100}, {"millimeter_y", 100}}}}})}};
  const auto saved =
      hub->receive(owner.actor, 501, kUpdateHousingRequest, update, 5, owner.port->access);
  std::vector<std::uint16_t> saved_opcodes;
  for (const auto &packet : saved) {
    saved_opcodes.push_back(packet.opcode);
  }
  EXPECT_EQ(saved_opcodes,
            (std::vector<std::uint16_t>{kUpdateHousingResponse, kItemsUpdateNotification}));
  EXPECT_EQ(visitor.port->delivered.back().opcode, kUpdateHousingResponse);

  // An immediate second save is inside the 5 s cooldown.
  const auto again =
      hub->receive(owner.actor, 501, kUpdateHousingRequest, update, 6, owner.port->access);
  ASSERT_EQ(again.size(), 1U);
  EXPECT_EQ(again[0].opcode, kCommonErrorResponse);
  EXPECT_EQ(again[0].message.at("ec"), "Housing_InvalidEditModeSaveCooldown");

  const auto malformed = hub->receive(owner.actor, 501, kUpdateHousingRequest,
                                      Json{{"house_nubs", 3}}, 7, owner.port->access);
  ASSERT_EQ(malformed.size(), 1U);
  EXPECT_EQ(malformed[0].message.at("ec"), "BadRequest");
}

// Live order from the stock client on 2026-10-30: HousingEditModeReq{} to enter, then
// HousingEditModeReq{end} when the player saves, and UpdateHousingReq only after
// HousingEditModeNoti{end} arrived. Every such save used to be refused.
TEST_F(HousingWorldTest, StockClientSaveOrderPlacesFurnitureThatReentryShows) {
  give(owner.actor, {{kPermitItem, 60}}, {{kChairItem, {1, 0}}});
  ASSERT_EQ(service->contract(owner.actor, SiteType::small, kZoneKey, ContractType::monthly,
                              nullptr)
                .ec,
            x5::ec::Success);
  enter_channel(owner);

  const auto entered = hub->receive(owner.actor, 501, kEditModeRequest, Json::object(), 1,
                                    owner.port->access);
  ASSERT_EQ(entered.size(), 1U);
  EXPECT_EQ(entered[0].message.at("ec"), "Success");

  const auto ended = hub->receive(owner.actor, 501, kEditModeRequest, Json{{"end", true}}, 2,
                                  owner.port->access);
  ASSERT_EQ(ended.size(), 1U);
  EXPECT_EQ(ended[0].opcode, kEditModeNotification);
  EXPECT_EQ(ended[0].message.at("end"), true);

  const Json save{
      {"housing_lot_number", kSmallLot},
      {"housing_doodad_nubs",
       Json::array({{{"doodad_id", 2},
                     {"house_id", 1},
                     {"cms_id", kChairDoodad},
                     {"cur_phase", 1},
                     {"pos", {{"millimeter_x", -4500}, {"millimeter_y", 200}}},
                     {"yaw", 90.0}}})}};
  const auto saved =
      hub->receive(owner.actor, 501, kUpdateHousingRequest, save, 3, owner.port->access);
  std::vector<std::uint16_t> saved_opcodes;
  for (const auto &packet : saved) {
    saved_opcodes.push_back(packet.opcode);
  }
  ASSERT_EQ(saved_opcodes,
            (std::vector<std::uint16_t>{kUpdateHousingResponse, kItemsUpdateNotification}));
  EXPECT_EQ(saved[1].message.at("housing_items")[0].at("housing_installed_count"), 1);
  // The only chair is placed, so none is left in storage: the stock client offers an object
  // while this quantity is above zero (AAC_Housing_CanPlaceSelectedItem).
  EXPECT_EQ(saved[1].message.at("housing_items")[0].at("quantity"), 0);

  // Entering the channel again announces the lot with the chair inside the house.
  const auto snapshot = channel_snapshot(*service, kZoneKey, 1);
  ASSERT_EQ(snapshot.size(), 1U);
  const auto &doodads = snapshot[0].message.at("nub").at("housing_doodads");
  const auto chair_key = std::to_string((1U << 14) | 2U);
  ASSERT_TRUE(doodads.contains(chair_key));
  EXPECT_EQ(doodads.at(chair_key).at("cms_id"), kChairDoodad);
  EXPECT_EQ(doodads.at(chair_key).at("house_id"), 1);
}

TEST_F(HousingWorldTest, PlacedChairsOfferTheirPoseToEveryoneInTheChannel) {
  give(owner.actor, {{kPermitItem, 60}}, {{kChairItem, {1, 0}}});
  ASSERT_EQ(service->contract(owner.actor, SiteType::small, kZoneKey, ContractType::monthly,
                              nullptr)
                .ec,
            x5::ec::Success);
  enter_channel(owner);
  enter_channel(visitor);
  ASSERT_EQ(service->edit_mode(owner.actor, EditMode::build, false, kZoneKey, 1).ec,
            x5::ec::Success);
  UpdateRequest place;
  place.lot_number = kSmallLot;
  DoodadRecord chair;
  chair.doodad_id = 10;
  chair.cms_id = kChairDoodad;
  place.doodads = {chair};
  ASSERT_EQ(service->update(owner.actor, place, kZoneKey, 1, nullptr).ec, x5::ec::Success);
  visitor.port->delivered.clear();

  const auto chair_unit = housing_doodad_unit_id(kSmallLot, 0, 10);
  const Json sit{{"interaction_anim_id", kSitAnimation},
                 {"target_unit_id", std::to_string(chair_unit)},
                 {"param", {{"interaction", {{"interaction_id", kSitInteraction}}}}},
                 {"pos", {{"millimeter_x", 1}, {"millimeter_y", 2}, {"millimeter_z", 3}}},
                 {"yaw", 90.0},
                 {"tick", "123"}};

  // The visitor sits on the owner's chair; the owner sees it.
  const auto seated = hub->receive_furniture_pose(visitor.actor, 20069, sit, 4);
  ASSERT_TRUE(seated.has_value());
  ASSERT_EQ(seated->size(), 1U);
  const auto &notification = seated->front();
  EXPECT_EQ(notification.opcode, kStartInteractionAnimNotification);
  EXPECT_EQ(notification.message.at("unit_id"),
            std::to_string(100 + visitor.actor.character_id));
  EXPECT_EQ(notification.message.at("interaction_anim_id"), kSitAnimation);
  EXPECT_EQ(notification.message.at("target_unit_id"), std::to_string(chair_unit));
  EXPECT_EQ(notification.message.at("tick"), "123");
  EXPECT_EQ(notification.message.at("pos"), sit.at("pos"));
  ASSERT_FALSE(owner.port->delivered.empty());
  EXPECT_EQ(owner.port->delivered.back().opcode, kStartInteractionAnimNotification);
  EXPECT_TRUE(stranger.port->delivered.empty());

  // An end for another animation is not this pose's; the matching end finishes it once.
  const Json wrong_end{{"end_interaction_anim", {{"interaction_anim_id", kStandAnimation}}}};
  EXPECT_FALSE(hub->receive_furniture_pose(visitor.actor, 20071, wrong_end, 5).has_value());
  const Json end{{"end_interaction_anim", {{"interaction_anim_id", kSitAnimation}}}};
  const auto finished = hub->receive_furniture_pose(visitor.actor, 20071, end, 6);
  ASSERT_TRUE(finished.has_value());
  ASSERT_EQ(finished->size(), 1U);
  EXPECT_EQ(finished->front().opcode, kInteractionAnimResponse);
  EXPECT_EQ(owner.port->delivered.back().opcode, kInteractionAnimResponse);
  EXPECT_FALSE(hub->receive_furniture_pose(visitor.actor, 20071, end, 7).has_value());

  // Standing up is the chair's second pose.
  auto stand = sit;
  stand["interaction_anim_id"] = kStandAnimation;
  stand["param"] = {{"interaction", {{"interaction_id", kStandInteraction}}}};
  const auto stood = hub->receive_furniture_pose(visitor.actor, 20069, stand, 8);
  ASSERT_TRUE(stood.has_value());
  EXPECT_EQ(stood->front().opcode, kStartInteractionAnimNotification);
}

TEST_F(HousingWorldTest, PlacedObjectsResolveToTheirLotForOtherServices) {
  give(owner.actor, {{kPermitItem, 60}}, {{kChairItem, {1, 0}}});
  ASSERT_EQ(service->contract(owner.actor, SiteType::small, kZoneKey, ContractType::monthly,
                              nullptr)
                .ec,
            x5::ec::Success);
  enter_channel(owner);
  ASSERT_EQ(service->edit_mode(owner.actor, EditMode::build, false, kZoneKey, 1).ec,
            x5::ec::Success);
  UpdateRequest place;
  place.lot_number = kSmallLot;
  DoodadRecord chair;
  chair.doodad_id = 10;
  chair.cms_id = kChairDoodad;
  chair.position = LocalPosition{500, -400, 0};
  place.doodads = {chair};
  ASSERT_EQ(service->update(owner.actor, place, kZoneKey, 1, nullptr).ec, x5::ec::Success);
  const auto chair_unit = housing_doodad_unit_id(kSmallLot, 0, 10);

  // A visitor in the same channel sees the object at the lot it stands on.
  enter_channel(visitor);
  const auto placed = hub->placed_doodad(visitor.actor.character_id, chair_unit, kHousingMapId);
  ASSERT_TRUE(placed.has_value());
  EXPECT_EQ(placed->doodad_cms_id, kChairDoodad);
  // HousingSite x/y/z of lot 101 in centimetres; the small lot is 2100 cm square.
  EXPECT_EQ(placed->lot_centre_x_mm, -535301);
  EXPECT_EQ(placed->lot_centre_y_mm, 3439415);
  EXPECT_EQ(placed->lot_centre_z_mm, -625252);
  EXPECT_EQ(placed->lot_radius_mm, 14849);

  // Nothing resolves on another map, in another channel, outside housing or for a removed ID.
  EXPECT_FALSE(hub->placed_doodad(visitor.actor.character_id, chair_unit, 9012).has_value());
  EXPECT_FALSE(hub->placed_doodad(visitor.actor.character_id,
                                  housing_doodad_unit_id(kSmallLot, 0, 11), kHousingMapId)
                   .has_value());
  EXPECT_FALSE(hub->placed_doodad(stranger.actor.character_id, chair_unit, kHousingMapId)
                   .has_value());
  enter_channel(visitor, 2);
  EXPECT_FALSE(hub->placed_doodad(visitor.actor.character_id, chair_unit, kHousingMapId)
                   .has_value());

  // A craft aimed at a placed object belongs to processing, not to furniture poses.
  enter_channel(visitor);
  const Json craft{{"interaction_anim_id", kSitAnimation},
                   {"target_unit_id", std::to_string(chair_unit)},
                   {"param",
                    {{"interaction",
                      {{"interaction_id", kSitInteraction},
                       {"execute_craft", {{"item_craft_id", 60204301}, {"count", 1}}}}}}}};
  EXPECT_FALSE(hub->receive_furniture_pose(visitor.actor, 20069, craft, 4).has_value());
}

TEST_F(HousingWorldTest, FurniturePosesRefuseUnknownObjectsAndAnimations) {
  give(owner.actor, {{kPermitItem, 60}});
  ASSERT_EQ(service->contract(owner.actor, SiteType::small, kZoneKey, ContractType::monthly,
                              nullptr)
                .ec,
            x5::ec::Success);
  const auto refusal = [](const std::optional<std::vector<WorldPacket>> &reply) {
    EXPECT_TRUE(reply.has_value());
    if (!reply || reply->size() != 1U || reply->front().opcode != kStartInteractionAnimResponse) {
      ADD_FAILURE() << "expected one StartInteractionAnimRes";
      return std::string{};
    }
    return reply->front().message.at("ec").get<std::string>();
  };

  // Outside the housing area the object cannot be found.
  Json request{{"interaction_anim_id", kSitAnimation},
               {"target_unit_id", std::to_string(housing_doodad_unit_id(kSmallLot, 0, 1))}};
  EXPECT_EQ(refusal(hub->receive_furniture_pose(owner.actor, 20069, request, 1)),
            "Housing_DoodadNotFound");

  enter_channel(owner);
  // The preset decoration offers no pose.
  EXPECT_EQ(refusal(hub->receive_furniture_pose(owner.actor, 20069, request, 2)),
            "Interaction_Invalid");
  // No object 9 stands on the lot.
  request["target_unit_id"] = std::to_string(housing_doodad_unit_id(kSmallLot, 0, 9));
  EXPECT_EQ(refusal(hub->receive_furniture_pose(owner.actor, 20069, request, 3)),
            "Housing_DoodadNotFound");
  // A lot nobody rented.
  request["target_unit_id"] = std::to_string(housing_doodad_unit_id(kSecondSmallLot, 0, 1));
  EXPECT_EQ(refusal(hub->receive_furniture_pose(owner.actor, 20069, request, 4)),
            "Housing_NotRegisteredHousingSite");

  // Targets that are not housing objects stay with the other interaction handlers.
  request["target_unit_id"] = std::to_string(housing_unit_id(kSmallLot));
  EXPECT_FALSE(hub->receive_furniture_pose(owner.actor, 20069, request, 5).has_value());
  request["target_unit_id"] = "4294967297";
  EXPECT_FALSE(hub->receive_furniture_pose(owner.actor, 20069, request, 6).has_value());
  EXPECT_FALSE(hub->receive_furniture_pose(owner.actor, 20071,
                                           Json{{"end_interaction_anim",
                                                 {{"interaction_anim_id", kSitAnimation}}}},
                                           7)
                   .has_value());
}

// Records the SP calls a garden interaction makes; the markers stand for the session's
// StatSyncNoti packets.
struct VitalsRecorder {
  static constexpr std::uint16_t kDrainAnnouncement = 9001;
  static constexpr std::uint16_t kDrainCancelled = 9002;

  HousingVitals vitals() {
    HousingVitals result;
    result.begin_stamina = [this](const HousingStaminaFlow &flow) {
      flows.push_back(flow);
      return std::optional<std::vector<WorldPacket>>{{{kDrainAnnouncement, Json::object()}}};
    };
    result.spend_stamina = [this](std::int64_t) {
      ++spent;
    };
    result.cancel_stamina = [this](std::int64_t) {
      ++cancelled;
      return std::optional<WorldPacket>{WorldPacket{kDrainCancelled, Json::object()}};
    };
    return result;
  }

  std::vector<HousingStaminaFlow> flows;
  int spent{};
  int cancelled{};
};

std::vector<std::uint16_t> opcodes_of(const std::optional<std::vector<WorldPacket>> &packets) {
  std::vector<std::uint16_t> result;
  if (packets) {
    for (const auto &packet : *packets) {
      result.push_back(packet.opcode);
    }
  }
  return result;
}

TEST_F(HousingWorldTest, GardenPlotIsPlantedAndHarvestedThroughTheInteractionFlow) {
  give(owner.actor, {{kPermitItem, 60}, {kSeedItem, 7}}, {{kFarmItem, {1, 0}}});
  ASSERT_EQ(service->contract(owner.actor, SiteType::small, kZoneKey, ContractType::monthly,
                              nullptr)
                .ec,
            x5::ec::Success);
  service->set_doodad_profile(std::make_shared<const HousingDoodadProfile>(garden_profile_json()),
                              5);
  ASSERT_EQ(service->edit_mode(owner.actor, EditMode::build, false, kZoneKey, 1).ec,
            x5::ec::Success);
  UpdateRequest place;
  place.lot_number = kSmallLot;
  DoodadRecord plot;
  plot.doodad_id = 30;
  plot.cms_id = kFarmDoodad;
  place.doodads = {plot};
  ASSERT_EQ(service->update(owner.actor, place, kZoneKey, 1, nullptr).ec, x5::ec::Success);
  enter_channel(owner);
  enter_channel(visitor);
  visitor.port->delivered.clear();

  VitalsRecorder recorder;
  const auto vitals = recorder.vitals();
  const auto plot_unit = housing_doodad_unit_id(kSmallLot, 0, 30);
  const Json plant{{"interaction_anim_id", kPlantAnimation},
                   {"target_unit_id", std::to_string(plot_unit)},
                   {"param", {{"interaction", {{"interaction_id", kPlantInteraction}}}}},
                   {"pos", {{"millimeter_x", 1}, {"millimeter_y", 2}, {"millimeter_z", 3}}},
                   {"yaw", 45.0},
                   {"tick", "500"}};
  const std::int64_t started = 1'000'000'000;

  // The start announces the SP drain, then the animation to the requester and the channel.
  const auto first = hub->receive_doodad_use(owner.actor, 20069, plant, 1, owner.port->access,
                                             vitals, started);
  EXPECT_EQ(opcodes_of(first), (std::vector<std::uint16_t>{VitalsRecorder::kDrainAnnouncement,
                                                           kStartInteractionAnimNotification}));
  ASSERT_EQ(recorder.flows.size(), 1U);
  EXPECT_EQ(recorder.flows.front().cost, 30);
  EXPECT_EQ(recorder.flows.front().wire_start_ns, 500);
  EXPECT_EQ(first->back().message.at("tick"), "500");
  EXPECT_EQ(visitor.port->delivered.back().opcode, kStartInteractionAnimNotification);

  // An execute before the montage's execute event ends the action without spending seeds.
  const Json execute{{"interaction_anim_id", kPlantAnimation}, {"tick", "900"}};
  const auto early = hub->receive_doodad_use(owner.actor, 20070, execute, 2, owner.port->access,
                                             vitals, started + 1000 * 10'000LL);
  EXPECT_EQ(opcodes_of(early),
            (std::vector<std::uint16_t>{VitalsRecorder::kDrainCancelled, kCommonErrorResponse}));
  EXPECT_EQ(repositories.character_state
                ->load_world_progress(owner.actor.account_id, owner.actor.character_id, kWorldId)
                .stackables.at(kSeedItem),
            7);

  // A second attempt runs to its execute event.
  const auto restart = started + 10 * 10'000'000LL;
  ASSERT_EQ(opcodes_of(hub->receive_doodad_use(owner.actor, 20069, plant, 3, owner.port->access,
                                               vitals, restart))
                .back(),
            kStartInteractionAnimNotification);
  visitor.port->delivered.clear();
  const auto executed =
      hub->receive_doodad_use(owner.actor, 20070, execute, 4, owner.port->access, vitals,
                              restart + kPlantExecuteMilliseconds * 10'000LL);
  EXPECT_EQ(opcodes_of(executed),
            (std::vector<std::uint16_t>{kItemsUpdateNotification, kChangeDoodadPhaseNotification,
                                        kInteractionAnimResponse}));
  EXPECT_EQ(recorder.spent, 1);
  const auto &stacks = executed->at(0).message.at("stackables");
  ASSERT_EQ(stacks.size(), 1U);
  EXPECT_EQ(stacks[0].at("item_cms_id"), kSeedItem);
  EXPECT_EQ(stacks[0].at("quantity"), 2);
  EXPECT_TRUE(executed->at(0).message.contains("system"));
  const auto &phase = executed->at(1).message;
  EXPECT_EQ(phase.at("unit_id"), std::to_string(plot_unit));
  EXPECT_EQ(phase.at("phase_id"), kSownPhase);
  EXPECT_NE(phase.at("phase_begin_tick"), "0");
  EXPECT_EQ(opcodes_of(visitor.port->delivered),
            (std::vector<std::uint16_t>{kChangeDoodadPhaseNotification,
                                        kInteractionAnimResponse}));

  // A retried execute grants nothing more; the end closes the action once.
  EXPECT_EQ(opcodes_of(hub->receive_doodad_use(owner.actor, 20070, execute, 5,
                                               owner.port->access, vitals, restart)),
            (std::vector<std::uint16_t>{kInteractionAnimResponse}));
  const Json end{{"end_interaction_anim", {{"interaction_anim_id", kPlantAnimation}}}};
  EXPECT_EQ(opcodes_of(hub->receive_doodad_use(owner.actor, 20071, end, 6, owner.port->access,
                                               vitals, restart)),
            (std::vector<std::uint16_t>{kInteractionAnimResponse}));
  EXPECT_FALSE(hub->receive_doodad_use(owner.actor, 20071, end, 7, owner.port->access, vitals,
                                       restart)
                   .has_value());
  EXPECT_EQ(recorder.cancelled, 1);

  // The re-entry snapshot carries the sown phase.
  const auto snapshot = channel_snapshot(*service, kZoneKey, 1);
  const auto key = std::to_string(30U);
  EXPECT_EQ(snapshot[0].message.at("nub").at("housing_doodads").at(key).at("cur_phase"),
            kSownPhase);
}

TEST_F(HousingWorldTest, GardenRequestsForOtherTargetsAreLeftToOtherHandlers) {
  give(owner.actor, {{kPermitItem, 60}});
  ASSERT_EQ(service->contract(owner.actor, SiteType::small, kZoneKey, ContractType::monthly,
                              nullptr)
                .ec,
            x5::ec::Success);
  service->set_doodad_profile(std::make_shared<const HousingDoodadProfile>(garden_profile_json()),
                              5);
  enter_channel(owner);
  VitalsRecorder recorder;
  const auto vitals = recorder.vitals();

  // The preset decoration has no phase graph; the housing unit is a door target.
  Json request{{"interaction_anim_id", kPlantAnimation},
               {"target_unit_id", std::to_string(housing_doodad_unit_id(kSmallLot, 0, 1))},
               {"param", {{"interaction", {{"interaction_id", kPlantInteraction}}}}}};
  EXPECT_FALSE(hub->receive_doodad_use(owner.actor, 20069, request, 1, owner.port->access,
                                       vitals, 1)
                   .has_value());
  request["target_unit_id"] = std::to_string(housing_unit_id(kSmallLot));
  EXPECT_FALSE(hub->receive_doodad_use(owner.actor, 20069, request, 2, owner.port->access,
                                       vitals, 1)
                   .has_value());
  // Execute and end without an open garden action belong to gathering or crafting.
  EXPECT_FALSE(hub->receive_doodad_use(owner.actor, 20070,
                                       Json{{"interaction_anim_id", kPlantAnimation}}, 3,
                                       owner.port->access, vitals, 1)
                   .has_value());
  EXPECT_TRUE(recorder.flows.empty());
}

TEST_F(HousingWorldTest, SeatedPlayersArePresentedToLaterViewersUntilTheyStandOrLeave) {
  give(owner.actor, {{kPermitItem, 60}}, {{kChairItem, {1, 0}}});
  ASSERT_EQ(service->contract(owner.actor, SiteType::small, kZoneKey, ContractType::monthly,
                              nullptr)
                .ec,
            x5::ec::Success);
  ASSERT_EQ(service->edit_mode(owner.actor, EditMode::build, false, kZoneKey, 1).ec,
            x5::ec::Success);
  UpdateRequest place;
  place.lot_number = kSmallLot;
  DoodadRecord chair;
  chair.doodad_id = 10;
  chair.cms_id = kChairDoodad;
  place.doodads = {chair};
  ASSERT_EQ(service->update(owner.actor, place, kZoneKey, 1, nullptr).ec, x5::ec::Success);
  enter_channel(owner);

  const auto unit_add = Json{{"unit_id", std::to_string(100 + owner.actor.character_id)}};
  EXPECT_FALSE(hub->late_viewer_pose(unit_add).has_value());

  const Json sit{{"interaction_anim_id", kSitAnimation},
                 {"target_unit_id", std::to_string(housing_doodad_unit_id(kSmallLot, 0, 10))},
                 {"param", {{"interaction", {{"interaction_id", kSitInteraction}}}}},
                 {"pos", {{"millimeter_x", 1000}, {"millimeter_y", 2000}, {"millimeter_z", 0}}}};
  ASSERT_TRUE(hub->receive_furniture_pose(owner.actor, 20069, sit, 1).has_value());
  // The sit holds after its animation, so a viewer arriving later sees it.
  const auto end = Json{{"end_interaction_anim", {{"interaction_anim_id", kSitAnimation}}}};
  ASSERT_TRUE(hub->receive_furniture_pose(owner.actor, 20071, end, 2).has_value());
  const auto late = hub->late_viewer_pose(unit_add);
  ASSERT_TRUE(late.has_value());
  EXPECT_EQ(late->opcode, kStartInteractionAnimNotification);
  EXPECT_EQ(late->message.at("interaction_anim_id"), kSitAnimation);

  // Small adjustments keep the seat; walking away ends it.
  hub->observe_movement(owner.actor.character_id, WorldPosition{kHousingMapId, 1200, 2000, 0});
  EXPECT_TRUE(hub->late_viewer_pose(unit_add).has_value());
  hub->observe_movement(owner.actor.character_id, WorldPosition{kHousingMapId, 9000, 2000, 0});
  EXPECT_FALSE(hub->late_viewer_pose(unit_add).has_value());

  // Standing up releases the pose as well.
  ASSERT_TRUE(hub->receive_furniture_pose(owner.actor, 20069, sit, 3).has_value());
  auto stand = sit;
  stand["interaction_anim_id"] = kStandAnimation;
  stand["param"] = {{"interaction", {{"interaction_id", kStandInteraction}}}};
  ASSERT_TRUE(hub->receive_furniture_pose(owner.actor, 20069, stand, 4).has_value());
  EXPECT_FALSE(hub->late_viewer_pose(unit_add).has_value());
}

TEST_F(HousingWorldTest, GoHomeTravelsThroughTheSessionPort) {
  give(owner.actor, {{kPermitItem, 60}});
  ASSERT_EQ(service->contract(owner.actor, SiteType::small, kZoneKey, ContractType::monthly,
                              nullptr)
                .ec,
            x5::ec::Success);

  const auto refused = hub->receive(visitor.actor, 502, kGoHomeRequest, Json::object(), 1,
                                    visitor.port->access);
  ASSERT_EQ(refused.size(), 1U);
  EXPECT_EQ(refused[0].message.at("ec"), "Housing_NotRegisteredHousingSite");

  const auto home =
      hub->receive(owner.actor, 501, kGoHomeRequest, Json::object(), 2, owner.port->access);
  EXPECT_TRUE(home.empty());
  ASSERT_EQ(owner.port->travels.size(), 1U);
  EXPECT_EQ(owner.port->travels[0].map_id, kHousingMapId);
  EXPECT_EQ(owner.port->travels[0].channel, 1);
}

TEST_F(HousingWorldTest, ChannelSnapshotListsOccupiedLots) {
  give(owner.actor, {{kPermitItem, 60}});
  give(visitor.actor, {{kPermitItem, 60}});
  ASSERT_EQ(service->contract(owner.actor, SiteType::small, kZoneKey, ContractType::monthly,
                              nullptr)
                .ec,
            x5::ec::Success);
  ASSERT_EQ(service->contract(visitor.actor, SiteType::small, kZoneKey, ContractType::monthly,
                              nullptr)
                .ec,
            x5::ec::Success);

  const auto snapshot = channel_snapshot(*service, kZoneKey, 1);
  ASSERT_EQ(snapshot.size(), 2U);
  EXPECT_EQ(snapshot[0].message.at("nub").at("lot_number"), kSmallLot);
  EXPECT_EQ(snapshot[1].message.at("nub").at("lot_number"), kSecondSmallLot);
  EXPECT_TRUE(channel_snapshot(*service, kZoneKey, 2).empty());
}

TEST_F(HousingWorldTest, DetachedCharactersRunAgainstStorage) {
  hub->detach(owner.actor.character_id, static_cast<std::uint64_t>(owner.actor.character_id));
  give(owner.actor, {{kPermitItem, 60}});
  bool storage_path{};
  ASSERT_TRUE(hub->run_for(owner.actor.character_id, [&](const ProgressAccess *access) {
    storage_path = access == nullptr;
  }));
  EXPECT_TRUE(storage_path);
  EXPECT_EQ(owner.port->posted_tasks, 0);
}

constexpr std::uint16_t kGmCommandRequest = 20008;

TEST_F(HousingWorldTest, GmContractRentsALotAndRepliesInChat) {
  give(owner.actor, {{kPermitItem, 60}});

  const auto reply = gm_text(owner, "housing_contract small");
  ASSERT_TRUE(reply.has_value());
  ASSERT_EQ(reply->messages.size(), 1U);
  EXPECT_EQ(reply->messages[0], "Rented Small lot 101 in housing channel 1. Use "
                                "teleport_to_home to go there.");
  std::vector<std::uint16_t> opcodes;
  for (const auto &packet : reply->packets) {
    opcodes.push_back(packet.opcode);
  }
  EXPECT_EQ(opcodes,
            (std::vector<std::uint16_t>{kCharSiteUpdateNotification, kItemsUpdateNotification}));

  const auto site = service->personal_site(owner.actor.character_id);
  ASSERT_TRUE(site.has_value());
  EXPECT_EQ(site->key.lot_number, kSmallLot);
  EXPECT_EQ(site->contract_type, ContractType::monthly);
  const auto progress = repositories.character_state->load_world_progress(
      owner.actor.account_id, owner.actor.character_id, kWorldId);
  const auto permits = progress.stackables.find(kPermitItem);
  EXPECT_TRUE(permits == progress.stackables.end() || permits->second == 0);

  // The rented lot is now reachable with the GM teleport.
  const auto home = gm_text(owner, "teleport_to_home");
  ASSERT_TRUE(home.has_value());
  EXPECT_EQ(home->messages, std::vector<std::string>{"Moving to your housing lot."});
  ASSERT_EQ(owner.port->travels.size(), 1U);
  EXPECT_EQ(owner.port->travels[0].map_id, kHousingMapId);
}

TEST_F(HousingWorldTest, GmHousingRefusalsExplainTheNextStep) {
  const auto home = gm_text(owner, "teleport_to_home");
  ASSERT_TRUE(home.has_value());
  ASSERT_EQ(home->packets.size(), 1U);
  EXPECT_EQ(home->packets[0].message.at("ec"), "Housing_NotRegisteredHousingSite");
  ASSERT_EQ(home->messages.size(), 1U);
  EXPECT_NE(home->messages[0].find("housing_contract small"), std::string::npos);

  const auto unpaid = gm_text(owner, "housing_contract small periodic");
  ASSERT_TRUE(unpaid.has_value());
  ASSERT_EQ(unpaid->messages.size(), 1U);
  EXPECT_NE(unpaid->messages[0].find("Housing_RentItemNotEnough"), std::string::npos);
  EXPECT_NE(unpaid->messages[0].find("item_add 123"), std::string::npos);
  EXPECT_FALSE(service->personal_site(owner.actor.character_id).has_value());

  const auto usage = gm_text(owner, "housing_contract");
  ASSERT_TRUE(usage.has_value());
  ASSERT_EQ(usage->packets.size(), 1U);
  EXPECT_EQ(usage->packets[0].message.at("ec"), "BadRequest");
  ASSERT_EQ(usage->messages.size(), 1U);
  EXPECT_NE(usage->messages[0].find("Usage: housing_contract"), std::string::npos);
}

TEST_F(HousingWorldTest, GmUnlockConfirmsInChat) {
  const auto reply = gm_text(owner, "housing_unlock medium");
  ASSERT_TRUE(reply.has_value());
  EXPECT_TRUE(reply->packets.empty());
  EXPECT_EQ(reply->messages, std::vector<std::string>{"housing_unlock applied."});
}

TEST_F(HousingWorldTest, GmContractIsTextOnlyAndOtherCommandsPassThrough) {
  // housing_contract is not a GmCmd field, so the structured request is not a housing command.
  const Json structured{{"cmds", Json::array({{{"housing_contract", Json::object()}}})}};
  EXPECT_FALSE(hub->receive_gm(owner.actor, kGmCommandRequest, structured, 1, owner.port->access)
                   .has_value());
  EXPECT_FALSE(gm_text(owner, "item_add 123 60").has_value());
}

} // namespace
} // namespace chronicles::housing
