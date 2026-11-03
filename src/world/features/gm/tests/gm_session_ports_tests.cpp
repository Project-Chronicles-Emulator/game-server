#include "world/features/gm/gm_feature.hpp"
#include "world/features/gm/tests/unexpected_gm_context.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace chronicles::gm {
namespace {

enum class ProviderRole { runtime, combat, character, social, quests, world, mail, dispatch };

// Every unused capability throws through the existing fake. Each supplied object may
// service only its named role, so a fallback to any other provider fails this scenario.
class RoleProvider final : public testing::UnexpectedGmContext {
public:
  RoleProvider(ProviderRole role, std::vector<std::string> &effects)
      : role_(role), effects_(effects) {
  }

  std::optional<GmIdentity> gm_identity() const override {
    expect_role(ProviderRole::runtime);
    return GmIdentity{1, 19, 7, true};
  }

  std::optional<ChatIdentity> gm_chat_identity() const override {
    expect_role(ProviderRole::runtime);
    return ChatIdentity{1, 19, "Keeper"};
  }

  std::int64_t gm_monotonic_ticks() const override {
    expect_role(ProviderRole::runtime);
    ticks_ += 20'000'000;
    return ticks_;
  }

  world_metrics::TimedMutex &gm_protocol_mutex() const override {
    expect_role(ProviderRole::runtime);
    return mutex_;
  }

  std::optional<WorldPosition> gm_position_locked() const override {
    expect_role(ProviderRole::runtime);
    return WorldPosition{9012, 0, 0, 0, 0};
  }

  std::optional<std::int64_t> gm_next_gameplay_tick() const override {
    expect_role(ProviderRole::runtime);
    return 123;
  }

  bool gm_has_combat() const override {
    record(ProviderRole::combat, "combat");
    return false;
  }

  bool gm_has_tutorial() const override {
    expect_role(ProviderRole::character);
    return false;
  }

  std::vector<GameplayPacket> gm_execute_progression(const GmCommand &command) override {
    record(ProviderRole::character, "character");
    EXPECT_EQ(command.kind, GmCommandKind::set_level);
    return {{777, {{"provider", "character"}}}};
  }

  bool gm_has_guilds() const override {
    record(ProviderRole::social, "social");
    return false;
  }

  bool gm_has_quest_book() const override {
    record(ProviderRole::quests, "quests");
    return false;
  }

  GmOnlinePlayers gm_online_players() override {
    record(ProviderRole::world, "world");
    return {{"world-provider-only"}, {}};
  }

  bool gm_has_mail_storage() const override {
    record(ProviderRole::mail, "mail");
    return false;
  }

  std::optional<std::vector<GameplayPacket>>
  gm_dispatch_event_request(const WorldGameplayRequest &) override {
    record(ProviderRole::dispatch, "dispatch-event");
    return std::nullopt;
  }

  std::optional<std::vector<GameplayPacket>>
  gm_dispatch_expression_request(const WorldGameplayRequest &) override {
    record(ProviderRole::dispatch, "dispatch-expression");
    return std::nullopt;
  }

  std::optional<std::vector<GameplayPacket>>
  gm_dispatch_housing_request(const WorldGameplayRequest &) override {
    record(ProviderRole::dispatch, "dispatch-housing");
    return std::nullopt;
  }

private:
  void expect_role(ProviderRole expected) const {
    EXPECT_EQ(role_, expected);
  }

  void record(ProviderRole expected, const char *effect) const {
    expect_role(expected);
    effects_.emplace_back(effect);
  }

  ProviderRole role_;
  std::vector<std::string> &effects_;
  mutable world_metrics::TimedMutex mutex_;
  mutable std::int64_t ticks_{};
};

TEST(GmSessionPortsTest, DistinctProvidersDispatchCommandsAndPreserveReturnedEffects) {
  std::vector<std::string> effects;
  RoleProvider runtime(ProviderRole::runtime, effects);
  RoleProvider combat(ProviderRole::combat, effects);
  RoleProvider character(ProviderRole::character, effects);
  RoleProvider social(ProviderRole::social, effects);
  RoleProvider quests(ProviderRole::quests, effects);
  RoleProvider world(ProviderRole::world, effects);
  RoleProvider mail(ProviderRole::mail, effects);
  RoleProvider dispatch(ProviderRole::dispatch, effects);
  world_metrics::TimedMutex feature_mutex;
  GmFeature feature(feature_mutex);
  SessionEventBus events({});
  std::vector<std::string> replies;
  feature.set_reply_handler([&replies](const ChatIdentity &, const std::string &text) {
    replies.push_back(text);
  });

  auto session = feature.create_session(
      GmSessionPorts{runtime, combat, character, social, quests, world, mail, dispatch}, events);
  const auto run = [&session](const std::string &text, bool social_command = false) {
    FrameContext frame(kGmCommandTextRequestOpcode,
                       WorldGameplayRequest{kGmCommandTextRequestOpcode, 7, {{"text", text}}});
    const bool handled =
        social_command ? session->handle_social_gm(frame) : session->handle_generic_gm(frame);
    EXPECT_TRUE(handled);
    if (!social_command) {
      EXPECT_EQ(frame.action_deadline, 123);
    }
    return frame;
  };

  EXPECT_TRUE(run("heal").session_packets.empty());
  const auto progression = run("set_level 2");
  ASSERT_EQ(progression.session_packets.size(), 1);
  EXPECT_EQ(progression.session_packets.front().opcode, 777);
  EXPECT_EQ(progression.session_packets.front().message.at("provider"), "character");
  EXPECT_TRUE(run("guild_exp 1", true).session_packets.empty());
  EXPECT_TRUE(run("accept_quest 1").session_packets.empty());
  EXPECT_TRUE(run("players").session_packets.empty());
  EXPECT_TRUE(run("mail_send me gold 5").session_packets.empty());

  EXPECT_EQ(effects, (std::vector<std::string>{"combat", "combat", "dispatch-event",
                                               "dispatch-expression", "dispatch-housing",
                                               "character", "social", "quests", "world", "mail"}));
  ASSERT_EQ(replies.size(), 6);
  EXPECT_EQ(replies[3], "Online characters: 1");
  EXPECT_EQ(replies[4], "world-provider-only");
  EXPECT_EQ(replies[5], "Mail delivery requires storage and the item catalog.");
}

} // namespace
} // namespace chronicles::gm
