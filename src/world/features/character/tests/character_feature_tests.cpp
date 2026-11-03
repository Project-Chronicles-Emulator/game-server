#include "world/features/character/tests/character_feature_test_support.hpp"

#include "world/features/character/character_bootstrap.hpp"
#include "world/features/gm/gm_commands.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>

namespace chronicles::character {
namespace {

using testing_support::CharacterCatalog;
using testing_support::Json;
using testing_support::ProgressStore;

class SessionContext final : public CharacterSessionContext {
public:
  std::int64_t progression_character_id() const override {
    return store.actor.id;
  }

  std::optional<Character> progression_character() override {
    return store.progression_character();
  }

  WorldProgress progression_stored_progress() override {
    return store.progression_stored_progress();
  }

  bool progression_save(const WorldProgress &before, const WorldProgress &after) override {
    calls.push_back("commit");
    return store.progression_save(before, after);
  }

  bool progression_save_activity(const WorldProgress &before,
                                 const WorldProgress &after) override {
    calls.push_back("activity_commit");
    return store.progression_save(before, after);
  }

  bool character_has_handoff() const override {
    return has_handoff;
  }

  std::uint64_t character_player_unit_id(std::int64_t character_id) const override {
    return static_cast<std::uint64_t>(character_id) + 4096;
  }

  const WorldProgress *character_tutorial_progress() const override {
    return tutorial ? &*tutorial : nullptr;
  }

  bool character_has_combat() const override {
    return has_combat;
  }

  const Json *character_combat_stats() const override {
    return &combat_stats;
  }

  void character_update_combat_stats(const Json &stats) override {
    calls.push_back("combat");
    combat_stats = stats;
    combat_stats["live_current"] = true;
  }

  bool character_refill_player_vitals() override {
    calls.push_back("refill");
    return true;
  }

  std::optional<int> character_party_level() const override {
    return party_level;
  }

  world_metrics::TimedMutex &character_protocol_mutex() const override {
    return mutex;
  }

  GameplayPacket character_error(const WorldGameplayRequest &request,
                                 x5::ec::EErrorCode code) const override {
    return {2, {{"ec", x5::ec::EErrorCode_Name(code)}, {"sequence", request.sequence}}};
  }

  GameplayPacket character_stat_notification(std::int64_t unit_id, const Json &before,
                                             const Json &after,
                                             const Json &current) const override {
    return {170,
            {{"unit_id", unit_id}, {"before", before}, {"after", after}, {"current", current}}};
  }

  void character_deliver_observers(const std::vector<GameplayPacket> &packets) override {
    calls.push_back("observers");
    observer_packets = packets;
  }

  SessionEventBus::Subscriptions subscriptions() {
    SessionEventBus::Subscriptions subscriptions;
    subscriptions.progress.push_back([this](const PlayerProgressCommitted &event) {
      calls.push_back("tutorial");
      tutorial = event.progress;
    });
    subscriptions.progression_snapshot.push_back(
        [this](const PlayerProgressionSnapshotChanged &event) {
          EXPECT_EQ(store.progress.level, event.level);
          calls.push_back("progression");
          bootstrap.refresh_progression(event.level, event.experience, event.build);
        });
    subscriptions.inventory_snapshot.push_back([this](const PlayerInventorySnapshotChanged &event) {
      calls.push_back("inventory");
      EXPECT_EQ(event.prepared_stats, &combat_stats);
      bootstrap.refresh_items_and_stats(event.character, event.prepared_stats);
    });
    subscriptions.player_level.push_back([this](const PlayerLevelChanged &event) {
      calls.push_back("level");
      party_level = event.level;
    });
    return subscriptions;
  }

  ProgressStore store;
  bool has_handoff{true};
  bool has_combat{true};
  std::optional<WorldProgress> tutorial;
  std::optional<int> party_level{1};
  mutable world_metrics::TimedMutex mutex;
  Json combat_stats = Json::object();
  CharacterBootstrap bootstrap{{}};
  std::vector<std::string> calls;
  std::vector<GameplayPacket> observer_packets;
};

class CharacterOwner : public ::testing::Test {
protected:
  CharacterOwner() {
    context.bootstrap.select_character(context.store.actor);
    context.store.progress.level_up_points = 9;
  }

  CharacterCatalog fixture;
  std::shared_ptr<ClientCatalog> catalog = std::make_shared<ClientCatalog>(fixture.root());
  std::shared_ptr<const CharacterStats> stats = CharacterStats::load(*catalog);
  CharacterFeature feature{stats, catalog, {}};
  SessionContext context;
  SessionEventBus events{context.subscriptions()};
  std::unique_ptr<CharacterSession> session = feature.create_session(context, events);
};

TEST_F(CharacterOwner, AchievementClaimCommitsBeforeReplyAndCannotPayTwice) {
  const auto send = [&](const char *state) {
    FrameContext frame(kUpdateAchievementRequest,
                       WorldGameplayRequest{kUpdateAchievementRequest,
                                            17,
                                            {{"cms_id", 1000}, {"achievement_state", state}}});
    EXPECT_TRUE(session->handle(frame));
    return frame.session_packets;
  };
  EXPECT_EQ(send("EX5AchievementState_Completed").front().opcode, 2);
  context.store.progress.level = 2;
  context.store.commit_succeeds = false;
  EXPECT_EQ(send("EX5AchievementState_Completed").front().opcode, 2);
  EXPECT_TRUE(context.store.progress.achievements.empty());
  context.store.commit_succeeds = true;
  EXPECT_EQ(send("EX5AchievementState_Completed").front().opcode, 121);
  const auto completed = context.store.progress;

  context.store.commit_succeeds = false;
  EXPECT_EQ(send("EX5AchievementState_Rewarded").front().opcode, 2);
  EXPECT_EQ(context.store.progress, completed);
  context.store.commit_succeeds = true;
  const auto claimed = send("EX5AchievementState_Rewarded");
  EXPECT_EQ(claimed.front().opcode, 169);
  EXPECT_EQ(context.store.progress.gold, completed.gold + 100);
  const auto saved = context.store.progress;
  const auto duplicate = send("EX5AchievementState_Rewarded");
  ASSERT_EQ(duplicate.size(), 1U);
  EXPECT_EQ(duplicate.front().opcode, 121);
  EXPECT_EQ(context.store.progress, saved);
}

TEST_F(CharacterOwner, InstanceAchievementExperienceUsesActivityCommitOnlyOnce) {
  context.store.progress.level = 2;
  context.store.progress.achievements[1001] = {1, 123};
  context.tutorial = context.store.progress;

  const auto claim = [&] {
    FrameContext frame(kUpdateAchievementRequest,
                       WorldGameplayRequest{kUpdateAchievementRequest,
                                            17,
                                            {{"cms_id", 1001},
                                             {"achievement_state", "EX5AchievementState_Rewarded"}}});
    EXPECT_TRUE(session->handle(frame));
    return frame.session_packets;
  };

  context.store.commit_succeeds = false;
  EXPECT_EQ(claim().front().opcode, 2);
  EXPECT_EQ(context.store.progress.experience, 0);
  EXPECT_EQ(context.calls, std::vector<std::string>{"activity_commit"});

  context.calls.clear();
  context.store.commit_succeeds = true;
  EXPECT_NE(claim().front().opcode, 2);
  EXPECT_EQ(context.store.progress.experience, 50);
  EXPECT_EQ(context.calls.front(), "activity_commit");

  context.calls.clear();
  EXPECT_NE(claim().front().opcode, 2);
  EXPECT_EQ(std::count(context.calls.begin(), context.calls.end(), "activity_commit"), 0);
  EXPECT_EQ(context.store.progress.experience, 50);
}

TEST_F(CharacterOwner, AchievementExperienceOutsideInstanceUsesOrdinaryCommit) {
  context.store.progress.level = 2;
  context.store.progress.achievements[1001] = {1, 123};

  FrameContext frame(kUpdateAchievementRequest,
                     WorldGameplayRequest{kUpdateAchievementRequest,
                                          17,
                                          {{"cms_id", 1001},
                                           {"achievement_state", "EX5AchievementState_Rewarded"}}});
  ASSERT_TRUE(session->handle(frame));
  EXPECT_NE(frame.session_packets.front().opcode, 2);
  EXPECT_EQ(context.calls.front(), "commit");
  EXPECT_EQ(context.store.progress.experience, 50);
}

TEST_F(CharacterOwner, ReconciliationCommitsOneRevisionAndChangesOnlyEntryLevel) {
  context.store.progress.experience = 150;
  auto actor = context.store.actor;
  feature.reconcile_entry(context.store, actor);
  EXPECT_EQ(actor.level, 2);
  EXPECT_EQ(actor.experience, context.store.actor.experience);
  EXPECT_EQ(context.store.progress.revision, 1);
  EXPECT_EQ(context.store.progress.level_up_points, 12);
  EXPECT_EQ(context.store.saves, 1);
}

TEST_F(CharacterOwner, ReconciliationConflictLeavesTheEntryCharacterUntouched) {
  context.store.progress.experience = 150;
  context.store.commit_succeeds = false;
  auto actor = context.store.actor;
  feature.reconcile_entry(context.store, actor);
  EXPECT_EQ(actor.level, 1);
  EXPECT_EQ(context.store.progress.level, 1);
  EXPECT_EQ(context.store.progress.revision, 0);
}

TEST_F(CharacterOwner, AttributeCommitPrecedesLiveStatsAndBothOrderedCacheEffects) {
  context.tutorial = context.store.progress;
  FrameContext frame(
      kUpdateStatBonusRequest,
      WorldGameplayRequest{kUpdateStatBonusRequest,
                           7,
                           {{"stats", {{{"stat_type", "EX5StatType_Str"}, {"value", 3}}}}}});
  ASSERT_TRUE(session->handle(frame));
  EXPECT_EQ(context.calls,
            (std::vector<std::string>{"commit", "tutorial", "combat", "progression", "inventory"}));
  ASSERT_EQ(frame.session_packets.size(), 3U);
  EXPECT_EQ(frame.session_packets[0].opcode, kChangeStatBonusNotification);
  EXPECT_EQ(frame.session_packets[1].opcode, 78);
  EXPECT_EQ(frame.session_packets[2].opcode, 170);
  EXPECT_EQ(frame.session_packets[2].message.at("current"), context.combat_stats);
  EXPECT_EQ(context.bootstrap.stats(), context.combat_stats);
  EXPECT_EQ(context.store.progress.revision, 1);
}

TEST_F(CharacterOwner, InvalidAttributeShapeIsRefusedBeforeCharacterRead) {
  FrameContext frame(kUpdateStatBonusRequest,
                     WorldGameplayRequest{kUpdateStatBonusRequest, 8, {{"stats", 7}}});
  ASSERT_TRUE(session->handle(frame));
  ASSERT_EQ(frame.session_packets.size(), 1U);
  EXPECT_EQ(frame.session_packets[0].message.at("ec"), "StatBonus_InvalidRequest");
  EXPECT_EQ(context.store.character_reads, 0);
  EXPECT_TRUE(context.calls.empty());
}

TEST_F(CharacterOwner, CommitConflictEmitsNoSuccessStatsOrNotifications) {
  context.store.commit_succeeds = false;
  FrameContext frame(
      kUpdateStatBonusRequest,
      WorldGameplayRequest{kUpdateStatBonusRequest,
                           9,
                           {{"stats", {{{"stat_type", "EX5StatType_Str"}, {"value", 3}}}}}});
  ASSERT_TRUE(session->handle(frame));
  ASSERT_EQ(frame.session_packets.size(), 1U);
  EXPECT_EQ(frame.session_packets[0].message.at("ec"), "BadRequest");
  EXPECT_EQ(context.calls, (std::vector<std::string>{"commit"}));
  EXPECT_TRUE(context.combat_stats.empty());
}

TEST_F(CharacterOwner, LevelChangeUpdatesPartyBeforeCacheRefillAndObserverEffects) {
  auto progress = context.store.progress;
  progress.level = 2;
  progress.experience = 150;
  context.store.progress = progress;
  std::vector<GameplayPacket> packets;
  session->apply_level_change(progress, packets);
  EXPECT_EQ(context.calls,
            (std::vector<std::string>{"level", "progression", "refill", "observers"}));
  ASSERT_EQ(packets.size(), 2U);
  EXPECT_EQ(packets[0].opcode, 78);
  ASSERT_EQ(context.observer_packets.size(), 1U);
  EXPECT_EQ(context.observer_packets[0].opcode, kChangeCharacterPublicNotification);
  EXPECT_EQ(context.observer_packets[0].message.at("unit_id"), "4113");
}

TEST_F(CharacterOwner, UnchangedOrMissingPartyIdentityKeepsLevelEffectsPassive) {
  std::vector<GameplayPacket> packets;
  session->apply_level_change(context.store.progress, packets);
  context.party_level.reset();
  auto changed = context.store.progress;
  changed.level = 3;
  session->apply_level_change(changed, packets);
  EXPECT_TRUE(context.calls.empty());
  EXPECT_TRUE(packets.empty());
}

TEST_F(CharacterOwner, OperatorLevelRefusalDoesNotCommitOrPublish) {
  const auto packets = session->apply_operator_edit({ProgressionEditKind::set_level, 4});
  EXPECT_TRUE(packets.empty());
  EXPECT_EQ(context.store.saves, 0);
  EXPECT_TRUE(context.calls.empty());
}

TEST_F(CharacterOwner, OperatorExperienceInInstanceUsesOrdinaryCommit) {
  context.tutorial = context.store.progress;

  const auto packets = session->apply_operator_edit({ProgressionEditKind::add_experience, 50});

  ASSERT_FALSE(packets.empty());
  ASSERT_FALSE(context.calls.empty());
  EXPECT_EQ(context.calls.front(), "commit");
  EXPECT_EQ(std::count(context.calls.begin(), context.calls.end(), "activity_commit"), 0);
  EXPECT_EQ(context.store.progress.experience, 50);
}

TEST(CharacterBootstrapOwner, ProgressionRefreshCopiesTheBorrowedBuildIntoOwnedState) {
  CharacterBootstrap bootstrap({});
  Character character;
  character.id = 19;
  bootstrap.select_character(character);
  CharacterBuild build;
  build.stat_bonuses[1] = 3;
  bootstrap.refresh_progression(2, 150, build);
  build.stat_bonuses[1] = 99;
  ASSERT_NE(bootstrap.selected_character(), nullptr);
  EXPECT_EQ(bootstrap.selected_character()->build.stat_bonuses.at(1), 3);
  EXPECT_EQ(bootstrap.selected_character()->experience, 150);
}

TEST_F(CharacterOwner, BootstrapOwnsBaseGmFlagAndPerkOverrideAtTheirOriginalOpcodes) {
  CharacterBootstrap bootstrap(stats);
  bootstrap.prepare_stats(context.store.actor);
  bootstrap.select_character(context.store.actor);
  bootstrap.set_game_master(true);
  ASSERT_TRUE(bootstrap.base_snapshot());
  EXPECT_EQ(bootstrap.base_snapshot()->opcode, 125);
  EXPECT_EQ(bootstrap.base_snapshot()->message.at("gm"), true);
  ASSERT_TRUE(bootstrap.perk_snapshot());
  EXPECT_EQ(bootstrap.perk_snapshot()->opcode, 135);
  const Json override{{"remaining_char_build_points", {{"1", 27}}}};
  bootstrap.set_perk_snapshot(override);
  EXPECT_EQ(bootstrap.perk_snapshot()->message, override);
}

TEST(CharacterBootstrapOwner, ForeignInventoryRefreshIsIgnoredAndSelectedStatsAreCopied) {
  CharacterBootstrap bootstrap({});
  Character selected;
  selected.id = 17;
  bootstrap.select_character(selected);
  Character foreign = selected;
  foreign.id = 19;
  Json stats{{"hp", 41}};
  bootstrap.refresh_items_and_stats(foreign, &stats);
  EXPECT_TRUE(bootstrap.stats().empty());
  bootstrap.refresh_items_and_stats(selected, &stats);
  stats["hp"] = 7;
  EXPECT_EQ(bootstrap.stats().at("hp"), 41);
  bootstrap.set_authoritative_player_state(stats, true);
  EXPECT_TRUE(bootstrap.dead());
}

TEST(CharacterBootstrapOwner, MissingSelectedCharacterRetainsAuthoritativeStateRefusal) {
  CharacterBootstrap bootstrap({});
  EXPECT_THROW(bootstrap.set_authoritative_player_state(Json::object(), true), std::logic_error);
  EXPECT_FALSE(bootstrap.base_snapshot());
  EXPECT_FALSE(bootstrap.perk_snapshot());
}

TEST(CharacterOwnerStartup, AbsentRequiredTableDisablesProgressionWhileMalformedPresentDataFails) {
  chronicles::testing_support::TestCatalog absent({{"CharacterLevel", {}}});
  auto missing_catalog = std::make_shared<ClientCatalog>(absent.root());
  CharacterFeature disabled({}, missing_catalog, {});
  EXPECT_FALSE(disabled.progression());
  ProgressStore store;
  disabled.reconcile_entry(store, store.actor);
  EXPECT_EQ(store.progress_reads, 0);

  auto tables = testing_support::character_tables();
  tables["CharacterLevel"][0]["nextLevelExp"] = "broken";
  chronicles::testing_support::TestCatalog malformed(tables);
  auto malformed_catalog = std::make_shared<ClientCatalog>(malformed.root());
  EXPECT_THROW(CharacterFeature({}, malformed_catalog, {}), std::runtime_error);
}

TEST(CharacterProgressionEdit, OnlyTheNineExistingProgressionCommandsProduceAnEdit) {
  const std::array kinds{GmCommandKind::exp_up,
                         GmCommandKind::set_level,
                         GmCommandKind::add_char_build_point,
                         GmCommandKind::set_char_build_point,
                         GmCommandKind::reset_stat_bonus,
                         GmCommandKind::force_reset_perk,
                         GmCommandKind::add_stat_bonus,
                         GmCommandKind::set_stat_bonus,
                         GmCommandKind::learn_all_perks};
  for (const auto kind : kinds) {
    GmCommand command;
    command.kind = kind;
    command.progression_value = 11;
    command.build_point_type = 2;
    command.stat_type = 3;
    const auto edit = character_progression_edit(command);
    ASSERT_TRUE(edit);
    EXPECT_EQ(edit->value, 11);
    EXPECT_EQ(edit->build_point_type, 2);
    EXPECT_EQ(edit->stat_type, 3);
    EXPECT_STREQ(progression_edit_name(edit->kind), gm_command_name(kind));
  }
  GmCommand command;
  command.kind = GmCommandKind::item_add;
  EXPECT_FALSE(character_progression_edit(command));
}

} // namespace
} // namespace chronicles::character
