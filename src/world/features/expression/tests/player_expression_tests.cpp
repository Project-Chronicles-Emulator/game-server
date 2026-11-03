// Behavioral tests for src/world/features/expression/player_expression.cpp. Every outbound payload
// is encoded and decoded with the recovered descriptors, so field names and wire types are
// schema-checked.

#include "data/client_catalog.hpp"
#include "world/features/expression/player_expression.hpp"
#include "protocol/protocol.hpp"
#include "testing/memory_repositories.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr std::uint64_t kPlayerUnitId = 4097;
constexpr std::int64_t kNow = 1'000'000'000;
constexpr std::int64_t kTicksPerMillisecond = 10'000;
constexpr std::int64_t kCooldownTicks = 1'000 * kTicksPerMillisecond;
// Internal 100 ns ticks are sent as nanoseconds.
constexpr std::int64_t kWireNanosecondsPerTick = 100;

// Ids follow the exported Emotion table: 9 Dance and 14 Chair_Sit_01 are Basic (14 loops). The
// Reward and BM rows are synthetic; the export has none.
constexpr int kDance = 9;
constexpr int kChairSit = 14;
constexpr int kReward = 501;
constexpr int kPaid = 502;
constexpr int kUnknown = 777;

constexpr std::int64_t kAccountId = 1;
constexpr int kWorldId = 1;

const WorldPosition kStanding{9012, -1228376, 2995630, -647711, 37.5F};

std::vector<Json> emotion_rows() {
  return {
      {{"id", "9"}, {"category", "Basic"}, {"x5MotionKey", "Dance"}, {"targetAnimStanceKey", ""}},
      {{"id", "14"},
       {"category", "Basic"},
       {"x5MotionKey", "Chair_Sit_01_Loop_Emote"},
       {"targetAnimStanceKey", "Chair_Sit_01"}},
      {{"id", "501"}, {"category", "Reward"}, {"targetAnimStanceKey", ""}},
      {{"id", "502"}, {"category", "BM"}, {"targetAnimStanceKey", ""}},
  };
}

std::shared_ptr<const EmotionCatalog> test_catalog() {
  return std::make_shared<const EmotionCatalog>(
      EmotionCatalog::from_rows(emotion_rows(), kCooldownTicks));
}

ExpressionContext standing() {
  ExpressionContext context;
  context.position = kStanding;
  return context;
}

Json emote_request(int emotion_id, std::optional<double> yaw = 90.0) {
  Json request{{"emotion_cms_id", emotion_id},
               {"pos", {{"millimeter_x", 1}, {"millimeter_y", 2}, {"millimeter_z", 3}}}};
  if (yaw) {
    request["yaw"] = *yaw;
  }
  return request;
}

Json selfie_request(bool enter) {
  return {{"enter", enter}};
}

std::string wire_tick(std::int64_t internal_tick) {
  return std::to_string(internal_tick * kWireNanosecondsPerTick);
}

class PlayerExpressionTest : public ::testing::Test {
protected:
  // Round-trips one packet through the descriptors and returns the decoded message.
  Json decoded(const GameplayPacket &packet) {
    const auto frame = protocol_.decode(protocol_.encode(packet.opcode, packet.message));
    EXPECT_EQ(frame.at("status"), "decoded");
    EXPECT_EQ(frame.at("opcode").get<int>(), packet.opcode);
    return frame.at("message");
  }

  // The EmotionExecRes error code name, or "Success" when the default was omitted.
  std::string response_code(const ExpressionOutcome &outcome) {
    EXPECT_EQ(outcome.own.size(), 1U);
    if (outcome.own.empty()) {
      return {};
    }
    EXPECT_EQ(outcome.own.front().opcode, kEmotionExecResponseOpcode);
    return decoded(outcome.own.front()).value("ec", std::string("Success"));
  }

  ExpressionOutcome emote(PlayerExpression &expression, int emotion_id,
                          const ExpressionContext &context, std::int64_t now) {
    return expression.receive(kEmotionExecRequestOpcode, emote_request(emotion_id), context, now);
  }

  Protocol protocol_{CHRONICLES_PROTOCOL_DIR};
};

TEST(EmotionCatalog, ReadsCategoriesLoopsAndRejectsMalformedRows) {
  const auto catalog = EmotionCatalog::from_rows(emotion_rows(), kCooldownTicks);
  EXPECT_EQ(catalog.size(), 4U);
  EXPECT_EQ(catalog.cooldown_ticks(), kCooldownTicks);
  ASSERT_NE(catalog.find(kDance), nullptr);
  EXPECT_EQ(catalog.find(kDance)->category, EmotionCategory::basic);
  EXPECT_FALSE(catalog.find(kDance)->looping);
  EXPECT_TRUE(catalog.find(kChairSit)->looping);
  EXPECT_EQ(catalog.find(kReward)->category, EmotionCategory::reward);
  EXPECT_EQ(catalog.find(kPaid)->category, EmotionCategory::paid);
  EXPECT_EQ(catalog.find(kUnknown), nullptr);

  auto duplicated = emotion_rows();
  duplicated.push_back(duplicated.front());
  EXPECT_THROW(EmotionCatalog::from_rows(duplicated), std::runtime_error);
  EXPECT_THROW(EmotionCatalog::from_rows({{{"id", "3"}, {"category", "Secret"}}}),
               std::runtime_error);
  EXPECT_THROW(EmotionCatalog::from_rows({{{"category", "Basic"}}}), std::runtime_error);
  EXPECT_THROW(EmotionCatalog::from_rows(emotion_rows(), -1), std::invalid_argument);
}

TEST(EmotionCatalog, OwnershipIsBasicOrGranted) {
  const auto catalog = EmotionCatalog::from_rows(emotion_rows());
  EXPECT_TRUE(owns_emotion(catalog, {}, kDance));
  EXPECT_FALSE(owns_emotion(catalog, {}, kReward));
  EXPECT_TRUE(owns_emotion(catalog, {kReward}, kReward));
  EXPECT_FALSE(owns_emotion(catalog, {kUnknown}, kUnknown));
}

// Optional real-catalog check: set CHRONICLES_CLIENT_CATALOG to data/generated/catalog.
TEST(EmotionCatalog, ExportedCatalogHasTwelveBasicEmotionsAndAOneSecondCooldown) {
  const char *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the exported data/generated/catalog";
  }

  ClientCatalog client_catalog{std::filesystem::path(catalog_path)};
  const auto catalog = EmotionCatalog::load(client_catalog);
  EXPECT_EQ(catalog.size(), 12U);
  EXPECT_EQ(catalog.cooldown_ticks(), kCooldownTicks);
  for (const int emotion_id : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 13, 14}) {
    ASSERT_NE(catalog.find(emotion_id), nullptr) << emotion_id;
    EXPECT_EQ(catalog.find(emotion_id)->category, EmotionCategory::basic) << emotion_id;
    EXPECT_EQ(catalog.find(emotion_id)->looping, emotion_id == 13 || emotion_id == 14);
  }
}

TEST_F(PlayerExpressionTest, AcceptedEmoteAnswersTheRequesterAndNotifiesViewers) {
  PlayerExpression expression(test_catalog(), kPlayerUnitId, {});
  const auto outcome = emote(expression, kDance, standing(), kNow);

  EXPECT_EQ(outcome.emotion_result, x5::ec::Success);
  EXPECT_EQ(response_code(outcome), "Success");
  ASSERT_EQ(outcome.observers.size(), 1U);
  EXPECT_EQ(outcome.observers.front().opcode, kEmotionExecNotificationOpcode);
  const auto notification = decoded(outcome.observers.front());
  EXPECT_EQ(notification.at("unit_id"), std::to_string(kPlayerUnitId));
  EXPECT_EQ(notification.at("emotion_cms_id"), kDance);
  EXPECT_EQ(notification.at("tick"), wire_tick(kNow));
  // Viewers see the emote at the server position, not at the client-reported one.
  EXPECT_EQ(notification.at("pos").at("millimeter_x"), kStanding.x_mm);
  EXPECT_EQ(notification.at("pos").at("millimeter_y"), kStanding.y_mm);
  EXPECT_EQ(notification.at("pos").at("millimeter_z"), kStanding.z_mm);
  EXPECT_FLOAT_EQ(notification.at("yaw").get<float>(), 90.0F);
}

TEST_F(PlayerExpressionTest, RefusalsAnswerOnlyTheRequesterWithTheEmotionErrorCodes) {
  PlayerExpression expression(test_catalog(), kPlayerUnitId, {});
  const auto expect_refused = [&](int emotion_id, const ExpressionContext &context,
                                  const char *code) {
    const auto outcome = emote(expression, emotion_id, context, kNow);
    EXPECT_EQ(response_code(outcome), code);
    EXPECT_TRUE(outcome.observers.empty()) << code;
  };

  expect_refused(kUnknown, standing(), "Emotion_InvalidData");
  expect_refused(kDance, ExpressionContext{}, "Emotion_InvalidData");
  expect_refused(kReward, standing(), "Emotion_NotOwned");
  expect_refused(kPaid, standing(), "Emotion_NotOwned");

  auto dead = standing();
  dead.dead = true;
  dead.using_skill = true;
  expect_refused(kDance, dead, "Emotion_Dead");

  auto casting = standing();
  casting.using_skill = true;
  casting.interacting = true;
  expect_refused(kDance, casting, "Emotion_UsingSkill");

  auto gathering = standing();
  gathering.interacting = true;
  gathering.off_ground = true;
  expect_refused(kDance, gathering, "Emotion_Interaction");

  auto gliding = standing();
  gliding.off_ground = true;
  expect_refused(kDance, gliding, "Emotion_NonGroundIdle");

  // No refusal started the cooldown.
  EXPECT_EQ(emote(expression, kDance, standing(), kNow).emotion_result, x5::ec::Success);
}

TEST_F(PlayerExpressionTest, CooldownFollowsTheCatalogWithAJitterAllowance) {
  PlayerExpression expression(test_catalog(), kPlayerUnitId, {});
  ASSERT_EQ(emote(expression, kDance, standing(), kNow).emotion_result, x5::ec::Success);

  const auto early = emote(expression, kChairSit, standing(), kNow + 500 * kTicksPerMillisecond);
  EXPECT_EQ(response_code(early), "Emotion_Cooltime");
  EXPECT_TRUE(early.observers.empty());

  // The client gates on its own send times; 850 ms between arrivals is accepted.
  const auto jittered = emote(expression, kChairSit, standing(), kNow + 850 * kTicksPerMillisecond);
  EXPECT_EQ(jittered.emotion_result, x5::ec::Success);

  const auto after_jittered =
      emote(expression, kDance, standing(), kNow + 1'500 * kTicksPerMillisecond);
  EXPECT_EQ(response_code(after_jittered), "Emotion_Cooltime");
}

TEST_F(PlayerExpressionTest, GrantedEmotionsBecomeUsableAndAreListedInTheSnapshot) {
  PlayerExpression expression(test_catalog(), kPlayerUnitId, {kPaid});
  EXPECT_EQ(decoded(expression.snapshot()).at("emotions").size(), 1U);
  EXPECT_EQ(emote(expression, kPaid, standing(), kNow).emotion_result, x5::ec::Success);

  const auto later = kNow + kCooldownTicks;
  EXPECT_EQ(emote(expression, kReward, standing(), later).emotion_result, x5::ec::Emotion_NotOwned);
  expression.adopt_grant(kReward);
  EXPECT_EQ(emote(expression, kReward, standing(), later).emotion_result, x5::ec::Success);

  const auto snapshot = expression.snapshot();
  EXPECT_EQ(snapshot.opcode, kWorldCharEmotionsNotificationOpcode);
  const auto emotions = decoded(snapshot).at("emotions");
  ASSERT_EQ(emotions.size(), 2U);
  EXPECT_EQ(emotions.at(0).at("cms_id"), kReward);
  EXPECT_EQ(emotions.at(1).at("cms_id"), kPaid);
}

TEST_F(PlayerExpressionTest, BasicOnlySnapshotIsTheObservedEmptyList) {
  PlayerExpression expression(test_catalog(), kPlayerUnitId, {});
  const auto snapshot = expression.snapshot();
  const auto frame = protocol_.decode(protocol_.encode(snapshot.opcode, snapshot.message));
  EXPECT_EQ(frame.at("protobuf_bytes").get<std::size_t>(), 0U);
  EXPECT_FALSE(decoded(snapshot).contains("emotions"));
}

TEST_F(PlayerExpressionTest, YawDefaultsToZeroAndImplausibleValuesKeepTheServerFacing) {
  PlayerExpression expression(test_catalog(), kPlayerUnitId, {});
  auto now = kNow;
  const auto yaw_of = [&](std::optional<double> yaw) {
    const auto outcome =
        expression.receive(kEmotionExecRequestOpcode, emote_request(kDance, yaw), standing(), now);
    now += kCooldownTicks;
    EXPECT_EQ(outcome.observers.size(), 1U);
    return decoded(outcome.observers.front()).value("yaw", 0.0F);
  };

  EXPECT_FLOAT_EQ(yaw_of(std::nullopt), 0.0F);
  EXPECT_FLOAT_EQ(yaw_of(-170.25), -170.25F);
  EXPECT_FLOAT_EQ(yaw_of(5000.0), kStanding.yaw);
  EXPECT_FLOAT_EQ(yaw_of(std::numeric_limits<double>::infinity()), kStanding.yaw);
}

TEST_F(PlayerExpressionTest, SelfieModeNotifiesViewersOnlyWhenTheStateChanges) {
  PlayerExpression expression(test_catalog(), kPlayerUnitId, {});

  const auto entered =
      expression.receive(kSelfieModeRequestOpcode, selfie_request(true), standing(), kNow);
  EXPECT_TRUE(entered.own.empty());
  EXPECT_FALSE(entered.emotion_result.has_value());
  ASSERT_EQ(entered.observers.size(), 1U);
  EXPECT_EQ(entered.observers.front().opcode, kSelfieModeNotificationOpcode);
  const auto enter_message = decoded(entered.observers.front());
  EXPECT_EQ(enter_message.at("unit_id"), std::to_string(kPlayerUnitId));
  EXPECT_EQ(enter_message.at("enter"), true);
  EXPECT_TRUE(expression.selfie_active());

  const auto repeated =
      expression.receive(kSelfieModeRequestOpcode, selfie_request(true), standing(), kNow);
  EXPECT_TRUE(repeated.observers.empty());

  const auto left =
      expression.receive(kSelfieModeRequestOpcode, selfie_request(false), standing(), kNow);
  ASSERT_EQ(left.observers.size(), 1U);
  EXPECT_EQ(decoded(left.observers.front()).value("enter", false), false);
  EXPECT_FALSE(expression.selfie_active());

  auto dead = standing();
  dead.dead = true;
  EXPECT_TRUE(expression.receive(kSelfieModeRequestOpcode, selfie_request(true), dead, kNow)
                  .observers.empty());
  EXPECT_TRUE(
      expression.receive(kSelfieModeRequestOpcode, selfie_request(true), ExpressionContext{}, kNow)
          .observers.empty());

  ASSERT_FALSE(expression.receive(kSelfieModeRequestOpcode, selfie_request(true), standing(), kNow)
                   .observers.empty());
  expression.reset_selfie();
  EXPECT_FALSE(expression.selfie_active());
}

TEST_F(PlayerExpressionTest, GrantPlansOnlyUnownedNonBasicCatalogEmotions) {
  const auto catalog = EmotionCatalog::from_rows(emotion_rows());
  WorldProgress before;
  before.revision = 7;

  EXPECT_EQ(plan_emotion_grant(catalog, before, kUnknown).error, "unknown emotion id");
  EXPECT_EQ(plan_emotion_grant(catalog, before, kDance).error, "Basic emotions are always owned");

  const auto grant = plan_emotion_grant(catalog, before, kReward);
  ASSERT_TRUE(grant.error.empty());
  EXPECT_EQ(grant.after.revision, 8);
  EXPECT_EQ(grant.after.owned_emotions, (std::set<int>{kReward}));
  EXPECT_EQ(grant.notification.opcode, kEmotionsUpdateNotificationOpcode);
  const auto emotions = decoded(grant.notification).at("emotions");
  ASSERT_EQ(emotions.size(), 1U);
  EXPECT_EQ(emotions.front().at("cms_id"), kReward);

  EXPECT_EQ(plan_emotion_grant(catalog, grant.after, kReward).error,
            "the emotion is already owned");

  auto full = before;
  for (std::size_t index = 1; index <= kMaximumOwnedEmotions; ++index) {
    full.owned_emotions.insert(10'000 + static_cast<int>(index));
  }
  EXPECT_EQ(plan_emotion_grant(catalog, full, kPaid).error, "the owned emotion limit is reached");
}

TEST_F(PlayerExpressionTest, SelfieRegistryAnnouncesActiveUnitsToLateViewers) {
  SelfieRegistry registry;
  const Json unit_add{{"unit_id", std::to_string(kPlayerUnitId)}, {"nub", Json::object()}};
  EXPECT_FALSE(registry.late_viewer_notification(unit_add).has_value());

  registry.set(kPlayerUnitId, true);
  const auto notification = registry.late_viewer_notification(unit_add);
  ASSERT_TRUE(notification.has_value());
  const auto message = decoded(*notification);
  EXPECT_EQ(message.at("unit_id"), std::to_string(kPlayerUnitId));
  EXPECT_EQ(message.at("enter"), true);

  EXPECT_FALSE(registry.late_viewer_notification({{"unit_id", "4098"}}).has_value());
  EXPECT_FALSE(registry.late_viewer_notification({{"unit_id", "not-a-number"}}).has_value());

  registry.set(kPlayerUnitId, false);
  EXPECT_FALSE(registry.late_viewer_notification(unit_add).has_value());
}

TEST(EmotionGrantCommand, ParsesTheEmulatorGmTextCommand) {
  const auto parse = [](const std::string &text) {
    return parse_emotion_grant_command(Json{{"text", text}});
  };

  const auto parsed = parse("emotion_add 501");
  EXPECT_TRUE(parsed.matched);
  EXPECT_EQ(parsed.emotion_id, kReward);
  EXPECT_EQ(parse("  ADD_EMOTION\t502 ").emotion_id, kPaid);
  EXPECT_EQ(parse("emote_add 13").emotion_id, 13);

  for (const auto *invalid :
       {"emotion_add", "emotion_add 0", "emotion_add -4", "emotion_add x", "emotion_add 1 2"}) {
    const auto refused = parse(invalid);
    EXPECT_TRUE(refused.matched) << invalid;
    EXPECT_FALSE(refused.emotion_id.has_value()) << invalid;
    EXPECT_EQ(refused.error, "emotion_add <emotion_id>");
  }

  EXPECT_FALSE(parse("item_add 501").matched);
  EXPECT_FALSE(parse_emotion_grant_command(Json{{"cmds", Json::array()}}).matched);
}

TEST(EmotionStorage, OwnedEmotionsRoundTripAndCannotBeRemoved) {
  MemoryRepositories store;
  Character created;
  created.account_id = kAccountId;
  created.name = "Dancer";
  const auto character = store.characters->create(created);
  const auto before =
      store.character_state->load_world_progress(kAccountId, character.id, kWorldId);
  EXPECT_TRUE(before.owned_emotions.empty());

  auto after = before;
  after.revision = before.revision + 1;
  after.owned_emotions = {kReward, kPaid};
  ASSERT_TRUE(store.character_state->save_world_progress(kAccountId, character.id, kWorldId, before,
                                                         after));
  EXPECT_EQ(
      store.character_state->load_world_progress(kAccountId, character.id, kWorldId).owned_emotions,
      after.owned_emotions);

  auto removed = after;
  removed.revision = after.revision + 1;
  removed.owned_emotions.erase(kPaid);
  EXPECT_THROW(store.character_state->save_world_progress(kAccountId, character.id, kWorldId, after,
                                                          removed),
               std::invalid_argument);

  auto invalid = after;
  invalid.revision = after.revision + 1;
  invalid.owned_emotions.insert(0);
  EXPECT_THROW(store.character_state->save_world_progress(kAccountId, character.id, kWorldId, after,
                                                          invalid),
               std::invalid_argument);

  auto oversized = after;
  oversized.revision = after.revision + 1;
  for (std::size_t index = 1; index <= kMaximumOwnedEmotions; ++index) {
    oversized.owned_emotions.insert(10'000 + static_cast<int>(index));
  }
  EXPECT_THROW(store.character_state->save_world_progress(kAccountId, character.id, kWorldId, after,
                                                          oversized),
               std::invalid_argument);
  EXPECT_EQ(store.character_state->load_world_progress(kAccountId, character.id, kWorldId), after);
}

// Needs a disposable cluster migrated to schema version 27 (never the development database).
TEST(EmotionStorage, PostgresOwnedEmotionsRoundTripAndStaleRevisionsWriteNothing) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Only run against the isolated PostgreSQL test cluster";
  }

  auto store = make_postgres_character_repositories(connection);
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  Account owner;
  owner.name = "dancer_" + suffix;
  owner.salt = "test-salt";
  owner.hash = "test-hash";
  ASSERT_TRUE(store.accounts->add_account(owner));
  const auto account = store.accounts->account(owner.name);
  ASSERT_TRUE(account);
  Character created;
  created.account_id = account->id;
  created.name = "Dancer" + suffix.substr(suffix.size() - 8);
  const auto character = store.characters->create(created);

  const auto before =
      store.character_state->load_world_progress(account->id, character.id, kWorldId);
  auto after = before;
  after.revision = before.revision + 1;
  after.owned_emotions = {kReward};
  ASSERT_TRUE(store.character_state->save_world_progress(account->id, character.id, kWorldId,
                                                         before, after));
  EXPECT_EQ(store.character_state->load_world_progress(account->id, character.id, kWorldId), after);

  auto stale = before;
  stale.revision = before.revision + 1;
  stale.owned_emotions = {kPaid};
  EXPECT_FALSE(store.character_state->save_world_progress(account->id, character.id, kWorldId,
                                                          before, stale));
  EXPECT_EQ(store.character_state->load_world_progress(account->id, character.id, kWorldId), after);
}

} // namespace
} // namespace chronicles
