#include "world/features/companion/companion_feature.hpp"
#include "world/features/companion/tests/companion_test_support.hpp"

#include <limits>
#include <stdexcept>

namespace chronicles {
namespace {

using namespace companion_test_support;

class CompanionFeatureTest : public SessionTest, public companion::CompanionSessionContext {
protected:
  void SetUp() override {
    SessionTest::SetUp();
    feature_ = std::make_unique<companion::CompanionFeature>(companion_catalog(), item_catalog());
    world_ = feature_->world();
    companions_ = feature_->create_session(
        std::make_unique<RepositoryProgressAccess>(*store_.characters, *store_.character_state,
                                                   kAccountId, character_.id, kWorldId),
        100, character_.id);
    watcher_ = make_companions(other_.id, 200);
  }

  bool companion_has_handoff() const override {
    return authenticated_;
  }

  bool companion_is_game_master() const override {
    return authenticated_ && game_master_;
  }

  std::uint64_t companion_connection_id() const override {
    return 100;
  }

  std::int64_t companion_character_id() const override {
    return character_.id;
  }

  std::int64_t companion_epoch_ticks() const override {
    trace_.push_back("clock");
    return kNow;
  }

  void companion_entry_failed() noexcept override {
    ++entry_failures_;
  }

  bool companion_allow_gm_request() override {
    trace_.push_back("budget");
    return budget_allowed_;
  }

  bool companion_allow_player_car() override {
    trace_.push_back("car-budget");
    return car_allowed_;
  }

  void companion_reply(const std::string &message) override {
    trace_.push_back("reply");
    last_reply_ = message;
  }

  void companion_trade_commands(const std::vector<companion::CompanionGmCommand> &,
                                companion::SessionResult &) override {
    trace_.push_back("trade-command");
  }

  void companion_enter_trade(const WorldPosition &, companion::SessionResult &) override {
    trace_.push_back("trade-entry");
    if (fail_trade_entry_unknown_) {
      throw 7;
    }
    if (fail_trade_entry_) {
      throw std::runtime_error("injected trade entry failure");
    }
  }

  void companion_trade_dismounted(companion::SessionResult &result) override {
    trace_.push_back("dismount-settlement");
    dismounted_before_settlement_ = !companions_->riding() && !result.broadcast.packets.empty();
  }

  void companion_trade_boarded(companion::SessionResult &) override {
    trace_.push_back("trade-boarded");
  }

  void companion_capture_wagons(const std::vector<companion::VehicleSnapshot> &) override {
    if (fail_wagon_capture_) {
      throw std::runtime_error("injected wagon capture failure");
    }
  }

  bool companion_settle_wagons(const std::vector<companion::VehicleSnapshot> &) override {
    trace_.push_back("wagon-settlement");
    return true;
  }

  void companion_deliver_update(const companion::CompanionUpdate &) override {
    trace_.push_back("detach-delivery");
    if (fail_delivery_) {
      throw std::runtime_error("injected delivery failure");
    }
  }

  std::unique_ptr<companion::CompanionFeature> feature_;
  mutable std::vector<std::string> trace_;
  std::string last_reply_;
  bool authenticated_{true};
  bool game_master_{};
  bool budget_allowed_{true};
  bool car_allowed_{true};
  bool fail_trade_entry_{};
  bool fail_trade_entry_unknown_{};
  int entry_failures_{};
  bool fail_delivery_{};
  bool fail_wagon_capture_{};
  bool dismounted_before_settlement_{};
};

TEST_F(CompanionFeatureTest, RefusedCommandsDoNotSpendLaterBudgetsOrInvokeTrade) {
  const WorldGameplayRequest privileged{20009, 1, request(20009, {{"text", "spawn_operable 200"}})};
  companion::SessionResult result;
  EXPECT_TRUE(feature_->handle_command(*companions_, privileged, at(0, 0), *this, result));
  EXPECT_EQ(trace_, (std::vector<std::string>{"reply"}));
  EXPECT_EQ(last_reply_, "GM commands need a game master account.");

  trace_.clear();
  game_master_ = true;
  EXPECT_TRUE(feature_->handle_command(*companions_, privileged, std::nullopt, *this, result));
  EXPECT_EQ(trace_, (std::vector<std::string>{"reply"}));

  trace_.clear();
  budget_allowed_ = false;
  EXPECT_TRUE(feature_->handle_command(*companions_, privileged, at(0, 0), *this, result));
  EXPECT_EQ(trace_, (std::vector<std::string>{"budget", "reply"}));

  trace_.clear();
  const WorldGameplayRequest unrelated{20077, 2, {{"text", "car"}}};
  EXPECT_FALSE(feature_->handle_command(*companions_, unrelated, at(0, 0), *this, result));
  EXPECT_TRUE(trace_.empty());
}

TEST_F(CompanionFeatureTest, PlayerCarRunsItsBudgetsBeforeCompanionThenTradeAndReply) {
  companion::SessionResult result;
  feature_->enter_world(*companions_, at(0, 0), *this, result);
  trace_.clear();
  const WorldGameplayRequest car{20009, 1, request(20009, {{"text", "car"}})};
  ASSERT_TRUE(feature_->handle_command(*companions_, car, at(0, 0), *this, result));
  EXPECT_EQ(trace_,
            (std::vector<std::string>{"budget", "car-budget", "clock", "trade-command", "reply"}));
  EXPECT_EQ(last_reply_, "Your car is ready.");
  EXPECT_FALSE(result.broadcast.packets.empty());
}

TEST_F(CompanionFeatureTest, EntryFailureRetainsCompanionAndSettlementPrecedesDetachDelivery) {
  companion::SessionResult result;
  fail_trade_entry_ = true;
  EXPECT_NO_THROW(feature_->enter_world(*companions_, at(0, 0), *this, result));
  EXPECT_TRUE(companions_->entered());
  EXPECT_EQ(trace_, (std::vector<std::string>{"trade-entry"}));
  EXPECT_EQ(entry_failures_, 1);

  trace_.clear();
  feature_->leave_world(*companions_, *this);
  EXPECT_FALSE(companions_->entered());
  EXPECT_EQ(trace_, (std::vector<std::string>{"wagon-settlement", "detach-delivery"}));

  fail_trade_entry_ = false;
  feature_->enter_world(*companions_, at(0, 0), *this, result);
  trace_.clear();
  fail_delivery_ = true;
  EXPECT_NO_THROW(feature_->leave_world(*companions_, *this));
  EXPECT_EQ(trace_, (std::vector<std::string>{"wagon-settlement", "detach-delivery"}));
}

TEST_F(CompanionFeatureTest, UnknownEntryExceptionSignalsTheCloseOwner) {
  companion::SessionResult result;
  fail_trade_entry_unknown_ = true;
  EXPECT_NO_THROW(feature_->enter_world(*companions_, at(0, 0), *this, result));
  EXPECT_TRUE(companions_->entered());
  EXPECT_EQ(entry_failures_, 1);
}

TEST_F(CompanionFeatureTest, FailedWagonCaptureKeepsSessionAndVehicleForRetry) {
  companion::SessionResult result;
  feature_->enter_world(*companions_, at(0, 0), *this, result);
  const auto vehicle = world_->spawn_vehicle(100, kWagonId, at(0, 0), 4000, true, kNow);
  ASSERT_EQ(vehicle.error, x5::ec::Success);

  fail_wagon_capture_ = true;
  EXPECT_FALSE(feature_->leave_world(*companions_, *this));
  EXPECT_TRUE(companions_->entered());
  ASSERT_TRUE(world_->vehicle_of(character_.id));
  EXPECT_EQ(world_->vehicle_of(character_.id)->unit_id, vehicle.unit_id);

  fail_wagon_capture_ = false;
  EXPECT_TRUE(feature_->leave_world(*companions_, *this));
  EXPECT_FALSE(companions_->entered());
  EXPECT_FALSE(world_->vehicle_of(character_.id));
}

TEST_F(CompanionFeatureTest, DismountSettlesOnlyAfterTheCompanionStateAndPacketsChange) {
  change_progress([](WorldProgress &progress) {
    progress.registered_summons.insert(kSummonId);
  });
  companion::SessionResult result;
  feature_->enter_world(*companions_, at(0, 0), *this, result);
  const WorldGameplayRequest summon{
      companion::kSpawnSummonNpcRequest, 1,
      request(companion::kSpawnSummonNpcRequest, {{"summon_npc_cms_id", kSummonId}})};
  ASSERT_TRUE(feature_->handle_request(*companions_, summon, *this, result));
  ASSERT_TRUE(companions_->riding());

  trace_.clear();
  result = {};
  const WorldGameplayRequest dismount{companion::kGetOffUnitRequest, 2,
                                      request(companion::kGetOffUnitRequest, Json::object())};
  ASSERT_TRUE(feature_->handle_request(*companions_, dismount, *this, result));
  EXPECT_EQ(trace_, (std::vector<std::string>{"clock", "dismount-settlement"}));
  EXPECT_TRUE(dismounted_before_settlement_);
}

TEST_F(CompanionFeatureTest, DisabledDefinitionsAndUnsupportedIdentityDoNotCreateAPlayer) {
  companion::CompanionFeature disabled(static_cast<ClientCatalog *>(nullptr), item_catalog());
  EXPECT_FALSE(disabled.create_session({}, 100, character_.id));
  EXPECT_FALSE(feature_->create_session({}, 100, std::numeric_limits<std::int64_t>::max()));
}

} // namespace
} // namespace chronicles
