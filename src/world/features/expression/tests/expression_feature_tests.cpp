#include "world/features/expression/expression_feature.hpp"

#include "protocol/protocol.hpp"

#include <functional>
#include <stdexcept>
#include <utility>

#include <gtest/gtest.h>

namespace chronicles::expression {
namespace {

class ExpressionContextFake final : public ExpressionSessionContext {
public:
  bool expression_has_character() const override {
    return authenticated;
  }

  bool expression_closed() const override {
    return is_closed;
  }

  std::uint64_t expression_connection_id() const override {
    return 7;
  }

  std::int64_t expression_character_id() const override {
    return 19;
  }

  std::uint64_t expression_player_unit_id() const override {
    return unit_id;
  }

  std::int64_t expression_now_ticks() const override {
    return now;
  }

  WorldProgress expression_progress() const override {
    trace.push_back("progress");
    ++progress_reads;
    return progress;
  }

  bool save_expression_progress(const WorldProgress &before, const WorldProgress &after) override {
    trace.push_back("save");
    if (throw_on_save) {
      throw std::runtime_error("synthetic revision conflict");
    }
    if (!save_succeeds) {
      return false;
    }

    EXPECT_EQ(before.revision, progress.revision);
    progress = after;
    return true;
  }

  void adopt_expression_progress(const WorldProgress &) override {
    trace.push_back("adopt");
    ++adoptions;
  }

  ExpressionBlockers expression_blockers() const override {
    return blockers;
  }

  void deliver_expression_observers(const std::vector<GameplayPacket> &packets) override {
    if (on_observers) {
      on_observers();
    }
    observer_packets.insert(observer_packets.end(), packets.begin(), packets.end());
  }

  void reply_to_expression_command(const std::string &message) override {
    trace.push_back("reply");
    replies.push_back(message);
  }

  void append_expression_snapshot(const GameplayPacket &packet,
                                  std::vector<Frame> &pending) override {
    ++snapshot_attempts;
    if (throw_on_snapshot) {
      throw std::runtime_error("synthetic encoder failure");
    }
    snapshots.push_back(packet);
    pending.push_back(Frame{});
  }

  bool authenticated{true};
  bool is_closed{};
  bool save_succeeds{true};
  bool throw_on_save{};
  bool throw_on_snapshot{};
  std::uint64_t unit_id{0x1000000000000013ULL};
  std::int64_t now{100'000'000};
  WorldProgress progress;
  ExpressionBlockers blockers;
  mutable int progress_reads{};
  int adoptions{};
  int snapshot_attempts{};
  mutable std::vector<std::string> trace;
  std::vector<std::string> replies;
  std::vector<GameplayPacket> observer_packets;
  std::vector<GameplayPacket> snapshots;
  std::function<void()> on_observers;
};

std::shared_ptr<const EmotionCatalog> test_catalog() {
  return std::make_shared<const EmotionCatalog>(EmotionCatalog::from_rows(
      {{{"id", "1"}, {"category", "Basic"}}, {{"id", "11"}, {"category", "Reward"}}}));
}

FrameContext request_frame(std::uint16_t opcode, nlohmann::json message) {
  FrameContext frame(opcode, WorldGameplayRequest{opcode, 1, std::move(message)});
  frame.party_position = WorldPosition{8000, 100, 200, 300, 45.0F};
  return frame;
}

TEST(ExpressionFeature, ConstructionAndUnrelatedRequestsDoNotLoadPlayerProgress) {
  ExpressionFeature feature(test_catalog());
  ExpressionContextFake context;
  auto session = feature.create_session(context);
  auto unrelated = request_frame(20003, nlohmann::json::object());

  EXPECT_FALSE(session->handle(unrelated));
  EXPECT_EQ(context.progress_reads, 0);
  context.authenticated = false;
  auto emotion = request_frame(kEmotionExecRequestOpcode, {{"emotion_cms_id", 1}});
  EXPECT_FALSE(session->handle(emotion));
  EXPECT_EQ(context.progress_reads, 0);
}

TEST(ExpressionFeature, EntrySnapshotIsOncePerConnectionAcrossMapLoads) {
  ExpressionFeature feature(test_catalog());
  ExpressionContextFake context;
  context.progress.owned_emotions.insert(11);
  auto session = feature.create_session(context);
  std::vector<Frame> pending(1);

  session->on_enter_world(false, pending);
  session->on_enter_world(true, pending);
  session->on_map_load_complete();
  session->on_enter_world(true, pending);

  ASSERT_EQ(context.snapshots.size(), 1U);
  EXPECT_EQ(context.snapshots.front().opcode, kWorldCharEmotionsNotificationOpcode);
  EXPECT_EQ(context.snapshots.front().message.at("emotions").front().at("cms_id"), 11);
  EXPECT_EQ(context.progress_reads, 1);
  EXPECT_EQ(pending.size(), 2U);
}

TEST(ExpressionFeature, FailedSnapshotRetainsItsOnceOnlyAttemptAndNarrowCatch) {
  ExpressionFeature feature(test_catalog());
  ExpressionContextFake context;
  context.throw_on_snapshot = true;
  auto session = feature.create_session(context);
  std::vector<Frame> pending;

  EXPECT_NO_THROW(session->on_enter_world(true, pending));
  context.throw_on_snapshot = false;
  session->on_enter_world(true, pending);

  EXPECT_EQ(context.snapshot_attempts, 1);
  EXPECT_TRUE(pending.empty());
}

TEST(ExpressionFeature, ObserverDeliveryStillPrecedesAppendingTheOwnersResponse) {
  ExpressionFeature feature(test_catalog());
  ExpressionContextFake context;
  auto session = feature.create_session(context);
  auto frame = request_frame(kEmotionExecRequestOpcode, {{"emotion_cms_id", 1}});
  frame.session_packets.push_back({999, nlohmann::json::object()});
  context.on_observers = [&frame] {
    ASSERT_EQ(frame.session_packets.size(), 1U);
    EXPECT_EQ(frame.session_packets.front().opcode, 999);
  };

  ASSERT_TRUE(session->handle(frame));
  ASSERT_EQ(frame.session_packets.size(), 2U);
  EXPECT_EQ(frame.session_packets.back().opcode, kEmotionExecResponseOpcode);
  ASSERT_EQ(context.observer_packets.size(), 1U);
  EXPECT_EQ(context.observer_packets.front().opcode, kEmotionExecNotificationOpcode);
}

TEST(ExpressionFeature, GroundMovementObservationAndMapLoadKeepExistingRefusals) {
  ExpressionFeature feature(test_catalog());
  ExpressionContextFake context;
  auto session = feature.create_session(context);
  session->observe_movement(nlohmann::json{{"mode", "MoveMode_Falling"}});
  auto falling = request_frame(kEmotionExecRequestOpcode, {{"emotion_cms_id", 1}});
  ASSERT_TRUE(session->handle(falling));
  EXPECT_EQ(falling.session_packets.front().message.at("ec"), "Emotion_NonGroundIdle");

  session->observe_movement(nlohmann::json{{"mode", "MoveMode_Crouching"}});
  auto crouching = request_frame(kEmotionExecRequestOpcode, {{"emotion_cms_id", 1}});
  ASSERT_TRUE(session->handle(crouching));
  EXPECT_TRUE(crouching.session_packets.front().message.empty());

  context.now += EmotionCatalog::kDefaultCooldownTicks;
  session->observe_movement(nlohmann::json{{"mode", "MoveMode_Swimming"}});
  session->on_map_load_complete();
  auto after_load = request_frame(kEmotionExecRequestOpcode, {{"emotion_cms_id", 1}});
  ASSERT_TRUE(session->handle(after_load));
  EXPECT_TRUE(after_load.session_packets.front().message.empty());
}

TEST(ExpressionFeature, SelfieStateIsReplayedToLateViewersAndClearedAtExistingHooks) {
  ExpressionFeature feature(test_catalog());
  ExpressionContextFake context;
  auto session = feature.create_session(context);
  auto selfie = request_frame(kSelfieModeRequestOpcode, {{"enter", true}});
  ASSERT_TRUE(session->handle(selfie));
  EXPECT_TRUE(selfie.session_packets.empty());

  const nlohmann::json subject{{"unit_id", std::to_string(context.unit_id)}};
  const auto late = feature.late_viewer_notification(subject);
  ASSERT_TRUE(late);
  EXPECT_EQ(late->opcode, kSelfieModeNotificationOpcode);
  EXPECT_TRUE(late->message.at("enter").get<bool>());

  session->on_map_load_complete();
  EXPECT_FALSE(feature.late_viewer_notification(subject));
  auto repeat_after_load = request_frame(kSelfieModeRequestOpcode, {{"enter", true}});
  ASSERT_TRUE(session->handle(repeat_after_load));
  EXPECT_TRUE(feature.late_viewer_notification(subject));
  session->on_leave_world();
  EXPECT_FALSE(feature.late_viewer_notification(subject));
}

TEST(ExpressionFeature, ClosedSessionDoesNotRepublishSelfieRegistryState) {
  ExpressionFeature feature(test_catalog());
  ExpressionContextFake context;
  auto session = feature.create_session(context);
  context.is_closed = true;
  auto selfie = request_frame(kSelfieModeRequestOpcode, {{"enter", true}});
  ASSERT_TRUE(session->handle(selfie));
  EXPECT_FALSE(feature.late_viewer_notification({{"unit_id", std::to_string(context.unit_id)}}));
}

TEST(ExpressionFeature, GrantCommitsAndAdoptsBeforeInitializingPlayerStateAndReplying) {
  ExpressionFeature feature(test_catalog());
  ExpressionContextFake context;
  context.progress.revision = 8;
  auto session = feature.create_session(context);

  const auto packets = session->grant_emotion(11);
  EXPECT_EQ(context.trace,
            (std::vector<std::string>{"progress", "save", "adopt", "progress", "reply"}));
  EXPECT_EQ(context.progress.revision, 9);
  EXPECT_TRUE(context.progress.owned_emotions.contains(11));
  ASSERT_EQ(packets.size(), 1U);
  EXPECT_EQ(packets.front().opcode, kEmotionsUpdateNotificationOpcode);

  auto granted = request_frame(kEmotionExecRequestOpcode, {{"emotion_cms_id", 11}});
  ASSERT_TRUE(session->handle(granted));
  EXPECT_TRUE(granted.session_packets.front().message.empty());
}

TEST(ExpressionFeature, RefusedAndThrowingGrantCommitsDoNotAdoptOrNotifySuccess) {
  for (const bool throw_on_save : {false, true}) {
    ExpressionFeature feature(test_catalog());
    ExpressionContextFake context;
    context.save_succeeds = false;
    context.throw_on_save = throw_on_save;
    auto session = feature.create_session(context);

    EXPECT_TRUE(session->grant_emotion(11).empty());
    EXPECT_EQ(context.adoptions, 0);
    EXPECT_FALSE(context.progress.owned_emotions.contains(11));
    EXPECT_EQ(context.progress_reads, 1);
    EXPECT_EQ(context.replies,
              (std::vector<std::string>{"emotion_add was not saved (see the server log)."}));
  }
}

TEST(ExpressionFeature, UnmatchedGmDispatchLeavesOtherCommandsAndLazyStateUntouched) {
  ExpressionFeature feature(test_catalog());
  ExpressionContextFake context;
  auto session = feature.create_session(context);
  const WorldGameplayRequest request{20009, 1, {{"text", "status"}}};

  EXPECT_FALSE(session->dispatch_gm(request));
  EXPECT_EQ(context.progress_reads, 0);
  EXPECT_TRUE(context.replies.empty());
  EXPECT_TRUE(context.trace.empty());
}

TEST(ExpressionFeature, MalformedGmDispatchRepliesWithoutConstructingTheLazyPlayer) {
  ExpressionFeature feature(test_catalog());
  ExpressionContextFake context;
  auto session = feature.create_session(context);
  const WorldGameplayRequest request{20009, 1, {{"text", "emotion_add"}}};

  const auto result = session->dispatch_gm(request);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->empty());
  EXPECT_EQ(context.progress_reads, 0);
  EXPECT_EQ(context.adoptions, 0);
  EXPECT_EQ(context.trace, (std::vector<std::string>{"reply"}));
  EXPECT_EQ(context.replies, (std::vector<std::string>{"Usage: emotion_add <emotion_id>"}));
}

TEST(ExpressionFeature, SuccessfulGmDispatchUsesCurrentOwnedProgressAndOriginalGrantOrdering) {
  ExpressionFeature feature(test_catalog());
  ExpressionContextFake context;
  auto session = feature.create_session(context);
  context.progress.revision = 8;
  // The existing adapter routes by body here; it never added a second opcode admission guard.
  const WorldGameplayRequest request{20003, 7, {{"text", "emotion_add 11"}}};

  const auto result = session->dispatch_gm(request);
  ASSERT_TRUE(result);
  ASSERT_EQ(result->size(), 1U);
  EXPECT_EQ(result->front().opcode, kEmotionsUpdateNotificationOpcode);
  EXPECT_EQ(context.progress.revision, 9);
  EXPECT_TRUE(context.progress.owned_emotions.contains(11));
  EXPECT_EQ(context.trace,
            (std::vector<std::string>{"progress", "save", "adopt", "progress", "reply"}));
  EXPECT_EQ(context.replies, (std::vector<std::string>{"emotion_add applied."}));
}

} // namespace
} // namespace chronicles::expression
