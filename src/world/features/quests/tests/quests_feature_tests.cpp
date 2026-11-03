#include "world/features/inventory/inventory_items.hpp"
#include "world/features/quests/quest_intent_ledger.hpp"
#include "world/features/quests/quests_feature.hpp"

#include "world/features/character/tests/character_feature_test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <type_traits>

namespace chronicles::quests {
namespace {

using character::testing_support::CharacterCatalog;
using character::testing_support::Json;
using character::testing_support::ProgressStore;

constexpr std::uint64_t kGuide = (1ULL << 60) | (1ULL << 12) | 2;
constexpr int kGuideTemplate = 1001539;
constexpr int kMap = 9012;
constexpr int kCompanyQuest = 502;
constexpr int kContributionQuest = 503;
constexpr int kBlueSalt = 2;

static_assert(!std::is_copy_constructible_v<QuestIntentLedger::CaptureLease>);
static_assert(!std::is_move_constructible_v<QuestIntentLedger::CaptureLease>);

TEST(QuestIntentLedger, CaptureWaitsForPriorIntentAndExcludesLaterMutations) {
  WorldProgress initial;
  initial.quests[501] = QuestProgress{};
  QuestIntentLedger ledger(initial);
  const auto before = ledger.capture();
  ASSERT_TRUE(before);
  auto accepted = initial;
  accepted.quests[502].attempt = 1;
  accepted.quests[502].started_epoch_ms = 100;
  const auto first_intent = ledger.begin_mutation(accepted);
  ASSERT_TRUE(first_intent);
  const auto during = ledger.capture();
  ASSERT_TRUE(during);
  EXPECT_FALSE(ledger.resolve(*during));

  ledger.finish_mutation(*first_intent, accepted);
  ASSERT_TRUE(ledger.resolve(*before));
  EXPECT_FALSE(ledger.resolve(*before)->contains(502));
  ASSERT_TRUE(ledger.resolve(*during));
  EXPECT_EQ(ledger.resolve(*during)->at(502).attempt, 1);

  auto restarted = accepted;
  restarted.quests[502].attempt = 2;
  restarted.quests[502].started_epoch_ms = 200;
  const auto later_intent = ledger.begin_mutation(restarted);
  ASSERT_TRUE(later_intent);
  ledger.finish_mutation(*later_intent, restarted);
  EXPECT_EQ(ledger.resolve(*during)->at(502).attempt, 1);
  ledger.release(*before);
  ledger.release(*during);
  EXPECT_FALSE(ledger.resolve(*during));
}

TEST(QuestIntentLedger, AmbiguousMutationRemainsPendingAndFailedMutationKeepsPriorState) {
  WorldProgress initial;
  initial.quests[501] = QuestProgress{};
  QuestIntentLedger ledger(initial);
  const auto intent = ledger.begin_mutation(initial);
  ASSERT_TRUE(intent);
  const auto captured = ledger.capture();
  ASSERT_TRUE(captured);
  EXPECT_FALSE(ledger.resolve(*captured));
  EXPECT_FALSE(ledger.begin_mutation(initial));

  // Only a definitively failed write may settle as unchanged.
  ledger.finish_mutation(*intent, std::nullopt);
  ASSERT_TRUE(ledger.resolve(*captured));
  EXPECT_TRUE(ledger.resolve(*captured)->contains(501));
  ledger.release(*captured);
}

TEST(QuestIntentLedger, BoundedCaptureAndVersionHistoryResumeAfterOutOfOrderRelease) {
  WorldProgress progress;
  progress.quests[501] = QuestProgress{};
  QuestIntentLedger ledger(progress);
  std::vector<QuestIntentLedger::Capture> captures;
  for (int index = 0; index < 1024; ++index) {
    const auto capture = ledger.capture();
    ASSERT_TRUE(capture);
    captures.push_back(*capture);
  }
  EXPECT_FALSE(ledger.capture());
  ledger.release(captures.back());
  captures.pop_back();
  const auto replacement = ledger.capture();
  ASSERT_TRUE(replacement);

  for (int index = 0; index < 255; ++index) {
    progress.quests[501].attempt = index + 1;
    const auto intent = ledger.begin_mutation(progress);
    ASSERT_TRUE(intent);
    ledger.finish_mutation(*intent, progress);
  }
  EXPECT_FALSE(ledger.begin_mutation(progress));
  for (const auto &capture : captures) {
    ledger.release(capture);
  }
  ledger.release(*replacement);
  const auto after_prune = ledger.begin_mutation(progress);
  ASSERT_TRUE(after_prune);
  ledger.finish_mutation(*after_prune, progress);
}

TEST(QuestIntentLedger, QuestEntryBudgetAppliesBackpressureUntilOldCaptureIsReleased) {
  WorldProgress progress;
  for (int id = 1; id <= 4096; ++id) {
    progress.quests[id] = QuestProgress{};
  }
  QuestIntentLedger ledger(progress);
  const auto held = ledger.capture();
  ASSERT_TRUE(held);
  int completed = 0;
  for (; completed < 256; ++completed) {
    progress.quests[1].attempt = completed + 1;
    const auto intent = ledger.begin_mutation(progress);
    if (!intent) {
      break;
    }
    ledger.finish_mutation(*intent, progress);
  }
  EXPECT_GT(completed, 0);
  EXPECT_LT(completed, 256);
  ledger.release(*held);
  const auto after_prune = ledger.begin_mutation(progress);
  ASSERT_TRUE(after_prune);
  ledger.finish_mutation(*after_prune, progress);
}

TEST(QuestIntentLedger, MaximumPersistedQuestStateCanBeMutatedWithACaptureHeld) {
  WorldProgress progress;
  for (int quest_id = 1; quest_id <= 4096; ++quest_id) {
    auto &quest = progress.quests[quest_id];
    for (int objective = 1; objective <= 64; ++objective) {
      quest.objectives.emplace(objective, 1);
    }
  }

  QuestIntentLedger ledger(progress);
  const auto capture = ledger.capture();
  ASSERT_TRUE(capture);
  progress.quests[1].attempt = 1;
  const auto prepared = QuestIntentLedger::prepare(progress);
  const auto intent = ledger.begin_prepared(prepared);
  ASSERT_TRUE(intent);
  ledger.finish_prepared(*intent, true);

  const auto original = ledger.resolve_filtered(*capture, {1, 4096});
  ASSERT_TRUE(original);
  ASSERT_EQ(original->size(), 2U);
  EXPECT_EQ(original->at(1).attempt, 0);
  EXPECT_EQ(original->at(4096).objectives.size(), 64U);
  ledger.release(*capture);
}

TEST(QuestIntentLedger, MaximumStateReusesPreviousSnapshotAfterUncommittedWrite) {
  WorldProgress progress;
  for (int quest_id = 1; quest_id <= 4096; ++quest_id) {
    auto &quest = progress.quests[quest_id];
    for (int objective = 1; objective <= 64; ++objective) {
      quest.objectives.emplace(objective, 1);
    }
  }

  QuestIntentLedger ledger(progress);
  const auto capture = ledger.capture();
  ASSERT_TRUE(capture);
  progress.quests[1].attempt = 1;
  const auto committed = ledger.begin_mutation(progress);
  ASSERT_TRUE(committed);
  ledger.finish_prepared(*committed, true);

  auto proposal = progress;
  proposal.quests[1].attempt = 2;
  const auto unknown = ledger.begin_mutation(proposal);
  ASSERT_TRUE(unknown);
  EXPECT_NO_THROW(ledger.finish_mutation(*unknown, progress));
  const auto original = ledger.resolve_filtered(*capture, {1});
  ASSERT_TRUE(original);
  EXPECT_EQ(original->at(1).attempt, 0);
  ledger.release(*capture);
}

Json quest_document() {
  const Json quest{
      {"cms_id", 501},
      {"type", "None"},
      {"accept", {{{"kind", "NpcId"}, {"npc_id", kGuideTemplate}}}},
      {"conditions", Json::array()},
      {"completion", {{"kind", "Npc"}, {"npc_ids", {kGuideTemplate}}}},
      {"supplies", {{{"item_cms_id", 120059}, {"quantity", 1}}}},
      {"auto_complete", false},
      {"sequential", false},
      {"progresses", Json::array()},
      {"experience", 0},
      {"rewards", Json::array()},
      {"dependent_items", Json::array()},
      {"unsupported", Json::array()},
  };
  // Shaped like Donella's quest 80101045: a Blue Salt member with a valid pioneer permit and
  // 500 contribution may accept it, and completing it unlocks Blue Salt level 2.
  const Json company_quest{
      {"cms_id", kCompanyQuest},
      {"type", "HiddenAutoComplete"},
      {"accept", {{{"kind", "NpcId"}, {"npc_id", kGuideTemplate}}}},
      {"conditions",
       {{{"group", 1},
         {"operator", "AND"},
         {"negated", false},
         {"kind", "Company"},
         {"company_type", kBlueSalt},
         {"ignore_permit", false},
         {"level", 0},
         {"is_max_contribution", false},
         {"contribution", 500}}}},
      {"completion", {{"kind", "Auto"}}},
      {"supplies", Json::array()},
      {"auto_complete", true},
      {"sequential", false},
      {"progresses", Json::array()},
      {"experience", 0},
      {"rewards", Json::array()},
      {"company_unlocks", {{{"company_type", kBlueSalt}, {"level", 2}}}},
      {"dependent_items", Json::array()},
      {"unsupported", Json::array()},
  };
  // Shaped like Donella's hidden quest 90116021: accepting it completes it and grants one
  // contribution point to the character's current company.
  const Json contribution_quest{
      {"cms_id", kContributionQuest},
      {"type", "HiddenAutoComplete"},
      {"accept", {{{"kind", "NpcId"}, {"npc_id", kGuideTemplate}}}},
      {"conditions", Json::array()},
      {"completion", {{"kind", "Auto"}}},
      {"supplies", Json::array()},
      {"auto_complete", true},
      {"sequential", false},
      {"progresses", Json::array()},
      {"experience", 0},
      {"rewards", Json::array()},
      {"company_unlocks", Json::array()},
      {"company_contribution", 1},
      {"dependent_items", Json::array()},
      {"unsupported", Json::array()},
  };
  return {{"format_version", 1},
          {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
          {"map_worlds", {{"9012", "ArcheWorld"}}},
          {"quests", {quest, company_quest, contribution_quest}}};
}

class QuestContext final : public QuestsSessionContext {
public:
  bool quests_has_handoff() const override {
    return handoff;
  }

  std::optional<QuestServiceMembership> quests_service_membership() override {
    if (fail_membership_capture) {
      return std::nullopt;
    }
    QuestServiceMembership membership;
    membership.company_type = company ? company->company_type : 0;
    membership.guild_id = guild_level ? 7 : 0;
    membership.company_generation = 2;
    membership.guild_generation = 3;
    return membership;
  }

  std::string quests_new_effect_id() override {
    return std::string(32, 'a');
  }

  std::optional<std::vector<CharacterItem>> quests_commit_unaffiliated(
      const QuestServiceMembership &membership, const WorldProgress &before,
      const WorldProgress &after, const ItemTrade &trade) override {
    if (!commit_unaffiliated) {
      return std::nullopt;
    }
    return commit_unaffiliated(membership, before, after, trade);
  }

  QuestMutationResult quests_commit_service_effects(
      const std::string &effect_id, const WorldProgress &before,
      const std::optional<WorldProgress> &after, const ItemTrade &trade,
      const QuestServiceEffects &effects) override {
    if (!commit_service_effect) {
      return {};
    }
    return commit_service_effect(effect_id, before, after, trade, effects);
  }

  QuestMutationResult
  quests_commit_combat_event(const std::string &operation_id, const std::string &source_fingerprint,
                             const WorldProgress &before, const std::optional<WorldProgress> &after,
                             const ItemTrade &trade, const QuestServiceEffects &effects) override {
    return commit_combat_event(operation_id, source_fingerprint, before, after, trade, effects);
  }

  MonsterRewardCommitResult quests_commit_monster_service_effects(
      const std::string &receipt_id, const WorldProgress &before,
      const std::optional<WorldProgress> &after, const ItemTrade &trade,
      const QuestServiceEffects &effects) override {
    if (!commit_monster_effect) {
      return {};
    }
    return commit_monster_effect(receipt_id, before, after, trade, effects);
  }

  std::shared_ptr<QuestIntentLedger> quests_intent_ledger() const override {
    return ledger;
  }

  void quests_watch_mutation(const std::shared_ptr<QuestIntentLedger> &,
                             QuestIntentLedger::Sequence intent,
                             const std::optional<std::string> &) override {
    watched_intent = intent;
  }

  void quests_forget_mutation(QuestIntentLedger::Sequence intent) override {
    if (watched_intent == intent) {
      watched_intent.reset();
    }
  }

  void quests_retry_ambiguous_mutation(QuestIntentLedger::Sequence intent) override {
    if (watched_intent == intent) {
      ++ambiguous_retries;
    }
  }

  bool quests_has_tutorial() const override {
    return tutorial;
  }

  std::uint64_t quests_connection_id() const override {
    return 41;
  }

  std::uint64_t quests_player_unit_id(std::int64_t character_id) const override {
    return static_cast<std::uint64_t>(character_id) + 4096;
  }

  const AnnouncedUnit *quests_interaction_npc(std::uint64_t unit_id) const override {
    return unit_id == guide.unit_id ? &guide : nullptr;
  }

  void quests_apply_level_change(const WorldProgress &progress,
                                 std::vector<GameplayPacket> &packets) override {
    calls.push_back("level");
    observed_level = progress.level;
    packets.push_back({78, Json::object()});
  }

  std::optional<QuestCompanyStanding> quests_company_standing() const override {
    return company;
  }

  bool quests_apply_company_rewards(const QuestCompanyRewards &rewards,
                                     std::vector<GameplayPacket> &packets) override {
    calls.push_back("company");
    ++company_repair_attempts;
    if (company_repair_blocked) {
      return false;
    }
    for (const auto &unlock : rewards.unlocks) {
      if (company && company->company_type == unlock.company_type) {
        unlocked.push_back(unlock);
      }
    }
    contribution += rewards.contribution;
    if (rewards.contribution > 0) {
      packets.push_back({93, Json::object()});
    }
    return true;
  }

  std::chrono::steady_clock::time_point quests_steady_now() const override {
    return steady_now;
  }

  std::optional<int> quests_guild_level() const override {
    return guild_level;
  }

  void quests_apply_guild_experience(std::int64_t experience) override {
    calls.push_back("guild");
    guild_experience += experience;
  }

  std::optional<int> quests_hp_percent() const override {
    return hp_percent;
  }

  std::optional<int> quests_doodad_phase(std::uint64_t) const override {
    return std::nullopt;
  }

  std::optional<QuestInstanceItem> quests_instance_item(int item_cms_id) const override {
    if (fail_instance_lookup) {
      throw std::runtime_error("Injected item lookup failure before the write");
    }
    const auto found = instance_items.find(item_cms_id);
    if (found == instance_items.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  std::optional<std::vector<CharacterItem>>
  quests_commit_items(const WorldProgress &before, const WorldProgress &after,
                      const ItemTrade &trade) override {
    calls.push_back("items");
    if (!commit_items) {
      return std::nullopt;
    }
    return commit_items(before, after, trade);
  }

  MonsterRewardCommitResult quests_commit_monster_reward(
      const std::string &receipt_id, const WorldProgress &before,
      const std::optional<WorldProgress> &after, const ItemTrade &trade) override {
    if (!commit_monster_reward) {
      return {};
    }
    return commit_monster_reward(receipt_id, before, after, trade);
  }

  bool handoff{true};
  bool tutorial{};
  std::shared_ptr<QuestIntentLedger> ledger;
  std::optional<QuestIntentLedger::Sequence> watched_intent;
  int ambiguous_retries{};
  AnnouncedUnit guide{kGuide, kGuideTemplate, kMap, "", {1000, 0, 0}};
  std::vector<std::string> calls;
  int observed_level{};
  std::optional<QuestCompanyStanding> company;
  std::vector<QuestCompanyUnlock> unlocked;
  std::int64_t contribution{};
  std::optional<int> guild_level;
  std::int64_t guild_experience{};
  std::optional<int> hp_percent;
  std::map<int, QuestInstanceItem> instance_items;
  bool fail_instance_lookup{};
  bool fail_membership_capture{};
  bool company_repair_blocked{};
  int company_repair_attempts{};
  std::chrono::steady_clock::time_point steady_now{};
  std::function<std::optional<std::vector<CharacterItem>>(
      const WorldProgress &, const WorldProgress &, const ItemTrade &)>
      commit_items;
  std::function<MonsterRewardCommitResult(const std::string &, const WorldProgress &,
                                           const std::optional<WorldProgress> &, const ItemTrade &)>
      commit_monster_reward;
  std::function<QuestMutationResult(const std::string &, const WorldProgress &,
                                    const std::optional<WorldProgress> &, const ItemTrade &,
                                    const QuestServiceEffects &)>
      commit_service_effect;
  std::function<std::optional<std::vector<CharacterItem>>(
      const QuestServiceMembership &, const WorldProgress &, const WorldProgress &,
      const ItemTrade &)>
      commit_unaffiliated;
  std::function<MonsterRewardCommitResult(const std::string &, const WorldProgress &,
                                           const std::optional<WorldProgress> &, const ItemTrade &,
                                           const QuestServiceEffects &)>
      commit_monster_effect;
  std::vector<QuestServiceEffects> stored_effects;
  std::function<QuestMutationResult(const std::string &, const std::string &, const WorldProgress &,
                                    const std::optional<WorldProgress> &, const ItemTrade &,
                                    const QuestServiceEffects &)>
      commit_combat_event;
};

class QuestOwner : public ::testing::Test {
protected:
  QuestOwner() {
    const auto path = fixture.root() / "quests/catalog.json";
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path);
    output << quest_document();
    output.close();
    feature.load_catalog(fixture.root() / "client");
    session = feature.create_session(store, context, progression, events);
    session->select_character(store.actor.id);
    store.before_save = [this](const WorldProgress &, const WorldProgress &) {
      context.calls.push_back("commit");
    };
    context.commit_service_effect = [this](const std::string &, const WorldProgress &before,
                                            const std::optional<WorldProgress> &after,
                                            const ItemTrade &trade,
                                            const QuestServiceEffects &effects) {
      if (!after || !store.progression_save(before, *after)) {
        return QuestMutationResult{};
      }
      context.stored_effects.push_back(effects);
      return QuestMutationResult{QuestMutationStatus::committed, trade.granted_items};
    };
    context.commit_unaffiliated = [this](const QuestServiceMembership &membership,
                                          const WorldProgress &before, const WorldProgress &after,
                                          const ItemTrade &trade)
        -> std::optional<std::vector<CharacterItem>> {
      EXPECT_EQ(membership.company_type, 0);
      EXPECT_EQ(membership.guild_id, 0);
      if (!store.progression_save(before, after)) {
        return std::nullopt;
      }
      return trade.granted_items;
    };
  }

  SessionEventBus::Subscriptions subscriptions() {
    SessionEventBus::Subscriptions subscriptions;
    subscriptions.world_loot_progress.push_back(
        [this](const PlayerWorldLootProgressCommitted &event) {
          EXPECT_EQ(event.progress, store.progress);
          context.calls.push_back("cache");
        });
    return subscriptions;
  }

  FrameContext acceptance(int quest_id = 501) {
    FrameContext frame(
        20016, WorldGameplayRequest{
                   20016,
                   9,
                   {{"quest_cms_id", quest_id},
                    {"unit", {{"unit_id", std::to_string(kGuide)}}},
                    {"pos", {{"millimeter_x", 0}, {"millimeter_y", 0}, {"millimeter_z", 0}}}}});
    frame.gameplay_position = WorldPosition{kMap, 0, 0, 0, 0};
    return frame;
  }

  static QuestCompanyStanding blue_salt(std::int64_t contribution, bool permit_valid) {
    QuestCompanyStanding standing;
    standing.company_type = kBlueSalt;
    standing.contribution = contribution;
    standing.position_level = 2;
    standing.pioneer_permit_valid = permit_valid;
    return standing;
  }

  void prepare_combat_quest() {
    auto document = quest_document();
    auto quest = document.at("quests").at(0);
    quest["supplies"] = Json::array();
    quest["auto_complete"] = true;
    quest["completion"] = {{"kind", "Auto"}};
    quest["progresses"] = Json::array({{{"sequence", 1},
                                        {"group", 1},
                                        {"kind", "CounterUseSkill"},
                                        {"parameters", {{"skillId", 100}, {"count", 1}}}}});
    quest["rewards"] = Json::array({{{"item_cms_id", 120059}, {"quantity", 1}}});
    document["quests"] = Json::array({quest});
    {
      std::ofstream output(fixture.root() / "quests/catalog.json");
      output << document;
    }
    feature.load_catalog(fixture.root() / "client");
    session = feature.create_session(store, context, progression, events);
    session->select_character(store.actor.id);
    store.progress.quests[501] = QuestProgress{};
    context.ledger = std::make_shared<QuestIntentLedger>(store.progress);
    context.instance_items[120059] = QuestInstanceItem{1, 5000};
  }

  CharacterCatalog fixture;
  ClientCatalog catalog{fixture.root()};
  std::shared_ptr<const Progression> progression = Progression::load(catalog);
  ProgressStore store;
  QuestContext context;
  SessionEventBus events{subscriptions()};
  QuestsFeature feature;
  std::unique_ptr<QuestsSession> session;
};

TEST_F(QuestOwner, CombatQuestLostAcknowledgementRetriesExactPlanAndGrantedRows) {
  prepare_combat_quest();
  std::string first_id;
  std::string first_fingerprint;
  Json first_mutation;
  std::vector<CharacterItem> granted;
  int attempts = 0;
  context.commit_combat_event = [&](const std::string &id, const std::string &fingerprint,
                                    const WorldProgress &before,
                                    const std::optional<WorldProgress> &after,
                                    const ItemTrade &trade, const QuestServiceEffects &effects) {
    ++attempts;
    const auto mutation = canonical_quest_mutation(before, after, trade, effects);
    if (attempts == 1) {
      first_id = id;
      first_fingerprint = fingerprint;
      first_mutation = mutation;
      EXPECT_TRUE(after.has_value());
      EXPECT_TRUE(store.progression_save(before, *after));
      granted = trade.granted_items;
      granted.front().id = 9001;
      throw std::runtime_error("COMMIT succeeded but its acknowledgement was lost");
    }
    EXPECT_EQ(id, first_id);
    EXPECT_EQ(fingerprint, first_fingerprint);
    EXPECT_EQ(mutation, first_mutation);
    return QuestMutationResult{QuestMutationStatus::already_processed, granted};
  };
  std::vector<GameplayPacket> packets;
  const QuestEvents events_to_record{{QuestEventKind::SkillUsed, 100}};
  EXPECT_THROW(session->record_combat_events(events_to_record, WorldPosition{kMap}, 123, packets),
               std::runtime_error);
  EXPECT_TRUE(packets.empty());
  ASSERT_TRUE(session->has_pending_combat_events());
  ASSERT_TRUE(context.watched_intent);
  EXPECT_THROW(session->record_combat_events(events_to_record, WorldPosition{kMap}, 124, packets),
               std::logic_error);

  // Neither a changed view nor the already committed progress may replan the retry.
  context.hp_percent = 1;
  context.fail_instance_lookup = true;
  session->retry_combat_events(packets);
  EXPECT_EQ(attempts, 2);
  EXPECT_FALSE(session->has_pending_combat_events());
  EXPECT_FALSE(context.watched_intent);
  EXPECT_TRUE(store.progress.quests.at(501).complete);
  EXPECT_FALSE(packets.empty());
  const auto packet_count = packets.size();
  session->retry_combat_events(packets);
  EXPECT_EQ(packets.size(), packet_count);
  EXPECT_EQ(attempts, 2);
}

TEST_F(QuestOwner, CombatQuestConflictRetainsOriginalAttemptUntilExactSettlement) {
  prepare_combat_quest();
  int attempts = 0;
  Json original;
  context.commit_combat_event = [&](const std::string &, const std::string &,
                                    const WorldProgress &before,
                                    const std::optional<WorldProgress> &after,
                                    const ItemTrade &trade, const QuestServiceEffects &effects) {
    const auto mutation = canonical_quest_mutation(before, after, trade, effects);
    if (++attempts == 1) {
      original = mutation;
      return QuestMutationResult{};
    }
    EXPECT_EQ(mutation, original);
    EXPECT_TRUE(store.progression_save(before, *after));
    return QuestMutationResult{QuestMutationStatus::committed, trade.granted_items};
  };
  std::vector<GameplayPacket> packets;
  EXPECT_THROW(session->record_combat_events({{QuestEventKind::SkillUsed, 100}},
                                             WorldPosition{kMap}, 123, packets),
               std::runtime_error);
  ASSERT_TRUE(session->has_pending_combat_events());
  session->retry_combat_events(packets, true);
  EXPECT_TRUE(packets.empty());
  EXPECT_FALSE(session->has_pending_combat_events());
  EXPECT_TRUE(store.progress.quests.at(501).complete);
}

TEST_F(QuestOwner, NonmatchingCombatQuestEventDoesNotCreateAReceipt) {
  prepare_combat_quest();
  std::vector<GameplayPacket> packets;
  session->record_combat_events({{QuestEventKind::SkillUsed, 999}}, WorldPosition{kMap}, 123,
                                packets);
  EXPECT_TRUE(packets.empty());
  EXPECT_FALSE(session->has_pending_combat_events());
  EXPECT_FALSE(context.watched_intent);
}

TEST_F(QuestOwner, CombatQuestPreparationRetryKeepsTheCapturedCharacterAndEvents) {
  prepare_combat_quest();
  const auto original_slots = lowest_free_inventory_slots(store.actor, 1);
  ASSERT_TRUE(original_slots);
  context.fail_instance_lookup = true;
  std::vector<GameplayPacket> packets;
  EXPECT_THROW(session->record_combat_events({{QuestEventKind::SkillUsed, 100}},
                                             WorldPosition{kMap}, 123, packets),
               std::runtime_error);
  ASSERT_TRUE(session->has_pending_combat_events());
  EXPECT_FALSE(context.watched_intent);

  CharacterItem later_item;
  later_item.id = 999;
  later_item.cms_id = 120059;
  later_item.inventory_slot = original_slots->front();
  store.actor.items.push_back(later_item);
  context.hp_percent = 1;
  context.fail_instance_lookup = false;
  int commits = 0;
  context.commit_combat_event = [&](const std::string &, const std::string &,
                                    const WorldProgress &before,
                                    const std::optional<WorldProgress> &after,
                                    const ItemTrade &trade, const QuestServiceEffects &) {
    ++commits;
    EXPECT_EQ(trade.granted_items.front().inventory_slot, original_slots->front());
    EXPECT_TRUE(store.progression_save(before, *after));
    return QuestMutationResult{QuestMutationStatus::committed, trade.granted_items};
  };
  session->retry_combat_events(packets, true);
  EXPECT_EQ(commits, 1);
  EXPECT_FALSE(session->has_pending_combat_events());
  EXPECT_TRUE(store.progress.quests.at(501).complete);
  EXPECT_TRUE(packets.empty());
}

TEST_F(QuestOwner, GatheringQuestPlanCombinesRewardsWithoutWritingOrPublishing) {
  auto document = quest_document();
  auto quest = document.at("quests").at(0);
  quest["cms_id"] = 777;
  quest["auto_complete"] = true;
  quest["completion"] = {{"kind", "Auto"}};
  quest["supplies"] = Json::array();
  quest["progresses"] = Json::array({{{"sequence", 1},
                                      {"group", 1},
                                      {"kind", "InteractDoodad"},
                                      {"parameters", {{"doodadId", 763},
                                                      {"targetPhase", 1},
                                                      {"count", 1}}}}});
  quest["rewards"] = Json::array({{{"item_cms_id", 120059}, {"quantity", 1}}});
  quest["company_contribution"] = 2;
  quest["guild_experience"] = 3;
  document["quests"] = Json::array({quest});
  {
    std::ofstream output(fixture.root() / "quests/catalog.json");
    output << document;
  }
  QuestsFeature combined_feature;
  combined_feature.load_catalog(fixture.root() / "client");
  auto combined = combined_feature.create_session(store, context, progression, events);
  combined->select_character(store.actor.id);
  context.company = blue_salt(0, true);
  context.guild_level = 1;
  context.instance_items[120059] = QuestInstanceItem{1, 5000};

  auto before = store.progress;
  before.quests[777] = QuestProgress{};
  auto gathered = before;
  ++gathered.revision;
  gathered.stackables[120001] = 4;
  ItemTrade gathering_trade;
  gathering_trade.durability_updates.push_back({44, 10, 9});
  const QuestEvents interaction{{QuestEventKind::DoodadInteracted, 763, 1, 1}};
  auto plan = combined->plan_gathering_events(interaction, WorldPosition{kMap, 0, 0, 0, 0},
                                               before, gathered, gathering_trade);

  EXPECT_EQ(plan.after.revision, before.revision + 1);
  EXPECT_EQ(plan.after.stackables.at(120001), 4);
  EXPECT_TRUE(plan.after.quests.at(777).complete);
  EXPECT_EQ(plan.effects.company_contribution, 2);
  EXPECT_EQ(plan.effects.guild_experience, 3);
  EXPECT_EQ(plan.effects.expected_company_type, kBlueSalt);
  EXPECT_EQ(plan.effects.expected_guild_id, 7);
  ASSERT_EQ(plan.trade.granted_items.size(), 1U);
  EXPECT_EQ(plan.trade.granted_items.front().cms_id, 120059);
  ASSERT_EQ(plan.trade.durability_updates.size(), 1U);
  EXPECT_EQ(plan.trade.durability_updates.front().item_id, 44);
  EXPECT_TRUE(context.calls.empty());
  EXPECT_EQ(store.progress.revision, before.revision);

  auto stored = plan.trade.granted_items;
  stored.front().id = 123;
  EXPECT_NO_THROW(QuestsSession::finalize_gathering_packets(plan, stored));
  EXPECT_FALSE(plan.packets.empty());
  EXPECT_THROW(QuestsSession::finalize_gathering_packets(plan, {}), std::runtime_error);
}

TEST_F(QuestOwner, GatheringPlanCreditsStagedStackAndItemOwnershipWithoutEquipping) {
  auto document = quest_document();
  auto stack_quest = document.at("quests").at(0);
  stack_quest["cms_id"] = 778;
  stack_quest["supplies"] = Json::array();
  stack_quest["progresses"] = Json::array({{{"sequence", 1},
                                            {"group", 1},
                                            {"kind", "GetItem"},
                                            {"parameters", {{"itemId", 120001}, {"count", 1}}}}});
  auto instance_quest = stack_quest;
  instance_quest["cms_id"] = 779;
  instance_quest["progresses"][0]["parameters"]["itemId"] = 120002;
  auto equip_quest = instance_quest;
  equip_quest["cms_id"] = 780;
  equip_quest["progresses"][0]["kind"] = "EquipItem";
  document["quests"] = Json::array({stack_quest, instance_quest, equip_quest});
  {
    std::ofstream output(fixture.root() / "quests/catalog.json");
    output << document;
  }
  QuestsFeature combined_feature;
  combined_feature.load_catalog(fixture.root() / "client");
  auto combined = combined_feature.create_session(store, context, progression, events);
  combined->select_character(store.actor.id);

  auto before = store.progress;
  before.quests[778] = QuestProgress{};
  before.quests[779] = QuestProgress{};
  before.quests[780] = QuestProgress{};
  auto gathered = before;
  ++gathered.revision;
  gathered.stackables[120001] = 1;
  ItemTrade trade;
  CharacterItem staged_item;
  staged_item.cms_id = 120002;
  staged_item.quantity = 1;
  staged_item.inventory_slot = 10;
  trade.granted_items.push_back(staged_item);

  const auto plan = combined->plan_gathering_events({}, WorldPosition{kMap, 0, 0, 0, 0},
                                                     before, gathered, trade);
  EXPECT_EQ(plan.after.revision, before.revision + 1);
  EXPECT_EQ(plan.after.quests.at(778).objectives.at(1), 1);
  EXPECT_EQ(plan.after.quests.at(779).objectives.at(1), 1);
  EXPECT_FALSE(plan.after.quests.at(780).objectives.contains(1));
  EXPECT_TRUE(context.calls.empty());
}

TEST_F(QuestOwner, GatheringPlanAlwaysPinsMembershipEvenWithoutQuestRewards) {
  const auto before = store.progress;
  auto gathered = before;
  ++gathered.revision;
  const auto position = WorldPosition{kMap, 0, 0, 0, 0};
  const auto unaffiliated = session->plan_gathering_events({}, position, before, gathered, {});
  EXPECT_EQ(unaffiliated.effects.expected_company_type, 0);
  EXPECT_EQ(unaffiliated.effects.expected_guild_id, 0);
  EXPECT_EQ(unaffiliated.effects.expected_company_generation, 2);
  EXPECT_EQ(unaffiliated.effects.expected_guild_generation, 3);

  context.company = blue_salt(0, true);
  context.guild_level = 1;
  const auto affiliated = session->plan_gathering_events({}, position, before, gathered, {});
  EXPECT_EQ(affiliated.effects.expected_company_type, kBlueSalt);
  EXPECT_EQ(affiliated.effects.expected_guild_id, 7);
  EXPECT_EQ(affiliated.effects.expected_company_generation, 2);
  EXPECT_EQ(affiliated.effects.expected_guild_generation, 3);
  EXPECT_EQ(affiliated.effects.company_contribution, 0);
  EXPECT_EQ(affiliated.effects.guild_experience, 0);

  context.fail_membership_capture = true;
  EXPECT_THROW(session->plan_gathering_events({}, position, before, gathered, {}),
               std::runtime_error);
  EXPECT_TRUE(context.calls.empty());
}

TEST_F(QuestOwner, DetachedGatheringQuestIntentWaitsUntilAConfirmedOutcome) {
  context.ledger = std::make_shared<QuestIntentLedger>(store.progress);
  auto after = store.progress;
  after.quests[777].objectives[1] = 1;
  auto intent = session->reserve_gathering_intent(after);
  ASSERT_TRUE(intent);
  const auto capture = context.ledger->capture();
  ASSERT_TRUE(capture);
  EXPECT_FALSE(context.ledger->resolve(*capture));
  EXPECT_THROW(session->reserve_gathering_intent(after), std::runtime_error);

  auto detached = intent;
  intent.reset();
  EXPECT_FALSE(context.ledger->resolve(*capture));
  detached->finish(true);
  EXPECT_NO_THROW(detached->finish(true));
  EXPECT_THROW(detached->finish(false), std::logic_error);
  const auto snapshot = context.ledger->resolve(*capture);
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(snapshot->at(777).objectives.at(1), 1);
  context.ledger->release(*capture);
}

TEST_F(QuestOwner, GatheringQuestIntentFailureRestoresPreviousStateButUnknownDoesNot) {
  context.ledger = std::make_shared<QuestIntentLedger>(store.progress);
  auto after = store.progress;
  after.quests[777].objectives[1] = 1;
  auto refused = session->reserve_gathering_intent(after);
  const auto failed_capture = context.ledger->capture();
  ASSERT_TRUE(failed_capture);
  refused->finish(false);
  const auto previous = context.ledger->resolve(*failed_capture);
  ASSERT_TRUE(previous);
  EXPECT_FALSE(previous->contains(777));
  context.ledger->release(*failed_capture);

  auto unknown = session->reserve_gathering_intent(after);
  const auto sequence = unknown->sequence();
  const auto unknown_capture = context.ledger->capture();
  ASSERT_TRUE(unknown_capture);
  unknown.reset();
  EXPECT_FALSE(context.ledger->resolve(*unknown_capture));
  EXPECT_THROW(session->reserve_gathering_intent(after), std::runtime_error);
  // Test-only recovery simulates an authoritative readback after owner loss. Production
  // retains the owner in the admission registry until the transaction is resolved.
  context.ledger->finish_prepared(sequence, false);
  EXPECT_TRUE(context.ledger->resolve(*unknown_capture));
  context.ledger->release(*unknown_capture);
}

TEST_F(QuestOwner, GatheringQuestPlanRejectsWrongRevisionBeforeAnyWrite) {
  const auto before = store.progress;
  auto gathered = before;
  gathered.revision += 2;
  EXPECT_THROW(session->plan_gathering_events({}, WorldPosition{kMap, 0, 0, 0, 0}, before,
                                                gathered, {}),
               std::runtime_error);
  EXPECT_TRUE(context.calls.empty());
}

TEST_F(QuestOwner, AcceptanceCommitsBeforeCachePublicationAndDoesNotAddLevelEffects) {
  auto frame = acceptance();
  ASSERT_TRUE(session->handle(frame));
  EXPECT_EQ(context.calls, (std::vector<std::string>{"commit", "cache"}));
  EXPECT_EQ(store.progress.revision, 1);
  EXPECT_EQ(store.progress.stackables.at(120059), 1);
  EXPECT_FALSE(frame.session_packets.empty());
  EXPECT_EQ(context.observed_level, 0);
}

TEST_F(QuestOwner, EquipmentSuppliesAndRewardsBecomeItemRowsInTheSameCommit) {
  // The supplied 120059 is equipment in this test: it must arrive as an item row.
  context.instance_items[120059] = QuestInstanceItem{1, 5000};
  ItemTrade committed_trade;
  context.commit_items = [this, &committed_trade](const WorldProgress &before,
                                                  const WorldProgress &after,
                                                  const ItemTrade &trade)
      -> std::optional<std::vector<CharacterItem>> {
    if (!store.progression_save(before, after)) {
      return std::nullopt;
    }
    committed_trade = trade;
    auto rows = trade.granted_items;
    rows.front().id = 77;
    return rows;
  };

  auto frame = acceptance();
  ASSERT_TRUE(session->handle(frame));
  EXPECT_FALSE(store.progress.stackables.contains(120059));
  ASSERT_EQ(committed_trade.granted_items.size(), 1U);
  EXPECT_EQ(committed_trade.granted_items.front().cms_id, 120059);
  EXPECT_EQ(committed_trade.granted_items.front().durability, 5000);
  EXPECT_EQ(committed_trade.granted_items.front().grade, 1);

  const auto update = std::ranges::find_if(frame.session_packets, [](const auto &packet) {
    return packet.opcode == 169 && packet.message.contains("items");
  });
  ASSERT_NE(update, frame.session_packets.end());
  EXPECT_EQ(update->message.at("items").size(), 1U);
  for (const auto &stack : update->message.value("stackables", Json::array())) {
    EXPECT_NE(stack.at("item_cms_id"), 120059);
  }
}

TEST_F(QuestOwner, PreWriteFailureReleasesQuestIntentForLaterCaptures) {
  context.ledger = std::make_shared<QuestIntentLedger>(store.progress);
  context.fail_instance_lookup = true;
  auto frame = acceptance();
  EXPECT_THROW(session->handle(frame), std::runtime_error);
  EXPECT_EQ(store.saves, 0);

  const auto capture = context.ledger->capture();
  ASSERT_TRUE(capture);
  const auto snapshot = context.ledger->resolve(*capture);
  ASSERT_TRUE(snapshot);
  EXPECT_TRUE(snapshot->empty());
  context.ledger->release(*capture);

  context.fail_instance_lookup = false;
  auto retry = acceptance();
  ASSERT_TRUE(session->handle(retry));
  EXPECT_EQ(store.progress.revision, 1);
}

TEST_F(QuestOwner, AmbiguousWriteKeepsItsIntentUntilDurableReadback) {
  for (const bool committed : {false, true}) {
    SCOPED_TRACE(committed);
    store.progress = WorldProgress{};
    context.ledger = std::make_shared<QuestIntentLedger>(store.progress);
    context.ambiguous_retries = 0;
    store.before_save = [this, committed](const WorldProgress &, const WorldProgress &after) {
      if (committed) {
        store.progress = after;
      }
      throw std::runtime_error("Injected ambiguous quest write");
    };
    auto frame = acceptance();
    EXPECT_THROW(session->handle(frame), std::runtime_error);
    ASSERT_TRUE(context.watched_intent);
    EXPECT_EQ(context.ambiguous_retries, 1);

    const auto capture = context.ledger->capture();
    ASSERT_TRUE(capture);
    EXPECT_FALSE(context.ledger->resolve(*capture));
    context.ledger->finish_mutation(*context.watched_intent, store.progress);
    context.quests_forget_mutation(*context.watched_intent);
    const auto resolved = context.ledger->resolve(*capture);
    ASSERT_TRUE(resolved);
    EXPECT_EQ(resolved->contains(501), committed);
    context.ledger->release(*capture);
  }
}

TEST_F(QuestOwner, CommittedItemCountMismatchDoesNotPublishPreviousQuestState) {
  context.ledger = std::make_shared<QuestIntentLedger>(store.progress);
  context.instance_items[120059] = QuestInstanceItem{1, 5000};
  context.commit_items = [this](const WorldProgress &before, const WorldProgress &after,
                                const ItemTrade &) -> std::optional<std::vector<CharacterItem>> {
    if (!store.progression_save(before, after)) {
      return std::nullopt;
    }
    return std::vector<CharacterItem>{};
  };
  auto frame = acceptance();
  EXPECT_THROW(session->handle(frame), std::runtime_error);
  ASSERT_TRUE(context.watched_intent);
  EXPECT_EQ(context.ambiguous_retries, 1);
  const auto capture = context.ledger->capture();
  ASSERT_TRUE(capture);
  EXPECT_FALSE(context.ledger->resolve(*capture));

  context.ledger->finish_mutation(*context.watched_intent, store.progress);
  context.quests_forget_mutation(*context.watched_intent);
  const auto resolved = context.ledger->resolve(*capture);
  ASSERT_TRUE(resolved);
  EXPECT_TRUE(resolved->contains(501));
  context.ledger->release(*capture);
}

TEST_F(QuestOwner, EquipmentRewardWithAFullBagIsRefusedWithoutAnyCommit) {
  context.instance_items[120059] = QuestInstanceItem{1, 5000};
  for (int slot = 0; slot <= kMaximumInventorySlot; ++slot) {
    store.actor.items.push_back({slot + 1, 101, slot});
  }

  auto frame = acceptance();
  ASSERT_TRUE(session->handle(frame));
  ASSERT_EQ(frame.session_packets.size(), 1U);
  EXPECT_EQ(frame.session_packets[0].message.at("ec"), "ItemSlotLimit");
  EXPECT_EQ(store.progress.revision, 0);
  EXPECT_EQ(store.saves, 0);
}

TEST_F(QuestOwner, ConflictReturnsOnlyTheOriginalSequencedErrorWithoutPublishing) {
  store.commit_succeeds = false;
  auto frame = acceptance();
  ASSERT_TRUE(session->handle(frame));
  ASSERT_EQ(frame.session_packets.size(), 1U);
  EXPECT_EQ(frame.session_packets[0].opcode, 2);
  EXPECT_EQ(frame.session_packets[0].message.at("ec"), "BadRequest");
  EXPECT_EQ(frame.session_packets[0].message.at("packet_seq").at("seq"), "9");
  EXPECT_EQ(store.progress.revision, 0);
  EXPECT_EQ(context.calls, (std::vector<std::string>{"commit"}));
}

TEST_F(QuestOwner, RevisionExhaustionRefusesBeforeStorageCommitOrPublication) {
  store.progress.revision = std::numeric_limits<std::int64_t>::max();
  auto frame = acceptance();
  EXPECT_THROW(session->handle(frame), std::runtime_error);
  EXPECT_EQ(store.saves, 0);
  EXPECT_TRUE(context.calls.empty());
}

TEST_F(QuestOwner, MissingAuthoritativeCharacterStillFailsBeforeReadingProgress) {
  store.character_exists = false;
  auto frame = acceptance();
  EXPECT_THROW(session->handle(frame), std::runtime_error);
  EXPECT_EQ(store.progress_reads, 0);
  EXPECT_EQ(store.saves, 0);
}

TEST_F(QuestOwner, TutorialAndMissingPositionKeepSharedQuestRequestsUnhandled) {
  auto frame = acceptance();
  context.tutorial = true;
  EXPECT_FALSE(session->handle(frame));
  context.tutorial = false;
  frame.gameplay_position.reset();
  EXPECT_FALSE(session->handle(frame));
  EXPECT_EQ(store.character_reads, 0);
}

TEST_F(QuestOwner, RebuildingTheBookDoesNotResetTheOncePerConnectionSnapshotGuard) {
  FrameContext first(20001, WorldGameplayRequest{20001, 0, Json::object()});
  session->observe_snapshot(first);
  ASSERT_EQ(first.session_packets.size(), 1U);
  EXPECT_EQ(first.session_packets[0].opcode, 136);
  session->select_character(store.actor.id);
  FrameContext second(20001, WorldGameplayRequest{20001, 1, Json::object()});
  session->observe_snapshot(second);
  EXPECT_TRUE(second.session_packets.empty());
  EXPECT_EQ(store.progress_reads, 1);
}

TEST_F(QuestOwner, WorldEntryRestoresSavedMonsterPlayRecords) {
  store.progress.play_records[524] = 1;
  FrameContext entry(20001, WorldGameplayRequest{20001, 0, Json::object()});
  session->observe_snapshot(entry);
  ASSERT_EQ(entry.session_packets.size(), 2U);
  EXPECT_EQ(entry.session_packets[1].opcode, 134);
  EXPECT_EQ(entry.session_packets[1].message.at("play_records")[0].at("cms_id"), 524);
  EXPECT_EQ(entry.session_packets[1].message.at("play_records")[0].at("count"), 1);
}

TEST_F(QuestOwner, PartyMonsterRewardCreditsQuestAndRecordOnceWithoutMonsterExperience) {
  auto document = quest_document();
  document["quests"][0]["progresses"] = Json::array({
      {{"sequence", 1}, {"group", 1}, {"kind", "HuntMonster"},
       {"parameters", {{"npcId", 5001}, {"count", 2}}}}});
  document["kill_play_records"] = {{{"id", 524}, {"npc_id", 5001}}};
  const auto path = fixture.root() / "quests/catalog.json";
  std::ofstream output(path, std::ios::trunc);
  output << document;
  output.close();
  QuestsFeature shared_feature;
  shared_feature.load_catalog(fixture.root() / "client");
  auto shared_session = shared_feature.create_session(store, context, progression, events);
  shared_session->select_character(store.actor.id);
  store.progress.quests[501] = QuestProgress{};
  store.progress.experience = 99;
  const WorldPosition position{kMap, 0, 0, 0, 0};
  std::vector<GameplayPacket> packets;

  shared_session->record_party_monster_reward(position, 9001, 5001, packets);
  EXPECT_EQ(store.progress.quests.at(501).objectives.at(1), 1);
  EXPECT_EQ(store.progress.play_records.at(524), 1);
  EXPECT_EQ(store.progress.experience, 99);
  const auto first_packet_count = packets.size();
  const auto first_revision = store.progress.revision;

  shared_session->record_party_monster_reward(position, 9001, 5001, packets);
  EXPECT_EQ(store.progress.quests.at(501).objectives.at(1), 1);
  EXPECT_EQ(store.progress.play_records.at(524), 1);
  EXPECT_EQ(store.progress.revision, first_revision);
  EXPECT_EQ(packets.size(), first_packet_count);

  shared_session->record_party_monster_reward(position, 9002, 5001, packets);
  EXPECT_EQ(store.progress.quests.at(501).objectives.at(1), 2);
  EXPECT_EQ(store.progress.play_records.at(524), 2);
  EXPECT_EQ(store.progress.experience, 99);
}

TEST_F(QuestOwner, QueuedMonsterRewardRetriesConflictAndNeverPublishesADuplicate) {
  auto document = quest_document();
  document["quests"][0]["progresses"] = Json::array({
      {{"sequence", 1}, {"group", 1}, {"kind", "HuntMonster"},
       {"parameters", {{"npcId", 5001}, {"count", 2}}}}});
  document["kill_play_records"] = {{{"id", 524}, {"npc_id", 5001}}};
  const auto path = fixture.root() / "quests/catalog.json";
  std::ofstream output(path, std::ios::trunc);
  output << document;
  output.close();
  QuestsFeature shared_feature;
  shared_feature.load_catalog(fixture.root() / "client");
  auto shared_session = shared_feature.create_session(store, context, progression, events);
  shared_session->select_character(store.actor.id);
  store.progress.quests[501] = QuestProgress{};
  bool raced = false;
  bool processed = false;
  context.commit_monster_reward = [&](const std::string &, const WorldProgress &before,
                                       const std::optional<WorldProgress> &after,
                                       const ItemTrade &) {
    if (processed) {
      return MonsterRewardCommitResult{MonsterRewardCommitStatus::already_processed, {}};
    }
    if (!raced) {
      raced = true;
      ++store.progress.revision;
      return MonsterRewardCommitResult{};
    }
    if (after && !store.progression_save(before, *after)) {
      return MonsterRewardCommitResult{};
    }
    processed = true;
    return MonsterRewardCommitResult{MonsterRewardCommitStatus::committed, {}};
  };
  MonsterRewardReceipt receipt;
  receipt.receipt_id = "0123456789abcdef0123456789abcdef";
  receipt.credited_unit_id = 9001;
  receipt.npc_cms_id = 5001;
  receipt.position = WorldPosition{kMap, 0, 0, 0, 0};
  receipt.quest_attempts[501] = {0, 0};
  std::vector<GameplayPacket> packets;

  shared_session->record_queued_monster_reward(receipt, packets);
  EXPECT_TRUE(processed);
  EXPECT_EQ(store.progress.quests.at(501).objectives.at(1), 1);
  EXPECT_EQ(store.progress.play_records.at(524), 1);
  const auto packet_count = packets.size();
  const auto saved_progress = store.progress;
  shared_session->record_queued_monster_reward(receipt, packets);
  EXPECT_EQ(store.progress, saved_progress);
  EXPECT_EQ(packets.size(), packet_count);
}

TEST_F(QuestOwner, LostRewardAcknowledgementWaitsForFencedReadback) {
  auto document = quest_document();
  document["quests"][0]["progresses"] = Json::array({
      {{"sequence", 1}, {"group", 1}, {"kind", "HuntMonster"},
       {"parameters", {{"npcId", 5001}, {"count", 2}}}}});
  const auto path = fixture.root() / "quests/catalog.json";
  std::ofstream output(path, std::ios::trunc);
  output << document;
  output.close();
  QuestsFeature shared_feature;
  shared_feature.load_catalog(fixture.root() / "client");
  store.progress.quests[501] = QuestProgress{};
  context.ledger = std::make_shared<QuestIntentLedger>(store.progress);
  auto shared_session = shared_feature.create_session(store, context, progression, events);
  shared_session->select_character(store.actor.id);
  bool processed = false;
  context.commit_monster_reward = [&](const std::string &, const WorldProgress &before,
                                       const std::optional<WorldProgress> &after,
                                       const ItemTrade &) {
    if (processed) {
      return MonsterRewardCommitResult{MonsterRewardCommitStatus::already_processed, {}};
    }
    if (after && !store.progression_save(before, *after)) {
      return MonsterRewardCommitResult{};
    }
    processed = true;
    throw std::runtime_error("simulated lost acknowledgement");
  };
  MonsterRewardReceipt receipt;
  receipt.receipt_id = "fedcba9876543210fedcba9876543210";
  receipt.credited_unit_id = 9001;
  receipt.npc_cms_id = 5001;
  receipt.position = WorldPosition{kMap, 0, 0, 0, 0};
  receipt.quest_attempts[501] = {0, 0};
  std::vector<GameplayPacket> packets;

  EXPECT_THROW(shared_session->record_queued_monster_reward(receipt, packets), std::runtime_error);
  const auto blocked = context.ledger->capture();
  ASSERT_TRUE(blocked);
  EXPECT_FALSE(context.ledger->resolve(*blocked));
  EXPECT_THROW(shared_session->record_queued_monster_reward(receipt, packets), std::runtime_error);
  EXPECT_FALSE(context.ledger->resolve(*blocked));
  ASSERT_TRUE(context.watched_intent);
  const auto intent = *context.watched_intent;
  context.ledger->finish_mutation(intent, store.progress);
  shared_session->retire_reconciled_reward_intent(receipt.receipt_id, intent);
  context.quests_forget_mutation(intent);
  ASSERT_TRUE(context.ledger->resolve(*blocked));
  EXPECT_EQ(store.progress.quests.at(501).objectives.at(1), 1);
  EXPECT_TRUE(packets.empty());
  context.ledger->release(*blocked);
}

TEST_F(QuestOwner, PlayRecordOnlyReceiptRetainsIntentAfterLostAcknowledgement) {
  auto document = quest_document();
  document["kill_play_records"] = {{{"id", 524}, {"npc_id", 5001}}};
  const auto path = fixture.root() / "quests/catalog.json";
  std::ofstream output(path, std::ios::trunc);
  output << document;
  output.close();
  QuestsFeature shared_feature;
  shared_feature.load_catalog(fixture.root() / "client");
  context.ledger = std::make_shared<QuestIntentLedger>(store.progress);
  auto shared_session = shared_feature.create_session(store, context, progression, events);
  shared_session->select_character(store.actor.id);
  context.commit_monster_reward = [&](const std::string &, const WorldProgress &before,
                                       const std::optional<WorldProgress> &after,
                                       const ItemTrade &) -> MonsterRewardCommitResult {
    EXPECT_TRUE(after);
    if (!after) {
      throw std::logic_error("Expected a play-record progress transition");
    }
    EXPECT_EQ(after->quests, before.quests);
    EXPECT_TRUE(store.progression_save(before, *after));
    throw std::runtime_error("simulated lost acknowledgement");
  };
  MonsterRewardReceipt receipt;
  receipt.receipt_id = "abcdefabcdefabcdefabcdefabcdefab";
  receipt.credited_unit_id = 9001;
  receipt.npc_cms_id = 5001;
  receipt.position = WorldPosition{kMap, 0, 0, 0, 0};
  receipt.award_experience = false;
  std::vector<GameplayPacket> packets;

  EXPECT_THROW(shared_session->record_queued_monster_reward(receipt, packets), std::runtime_error);
  EXPECT_EQ(store.progress.play_records.at(524), 1);
  ASSERT_TRUE(context.watched_intent);
  const auto capture = context.ledger->capture();
  ASSERT_TRUE(capture);
  EXPECT_FALSE(context.ledger->resolve(*capture));
  const auto intent = *context.watched_intent;
  context.ledger->finish_mutation(intent, store.progress);
  shared_session->retire_reconciled_reward_intent(receipt.receipt_id, intent);
  context.quests_forget_mutation(intent);
  EXPECT_TRUE(context.ledger->resolve(*capture));
  context.ledger->release(*capture);
  EXPECT_TRUE(packets.empty());
}

TEST_F(QuestOwner, RewardReadbackRetiresIntentEvenWhenProcessedReceiptIsNotListed) {
  auto document = quest_document();
  document["quests"][0]["progresses"] = Json::array({
      {{"sequence", 1}, {"group", 1}, {"kind", "HuntMonster"},
       {"parameters", {{"npcId", 5001}, {"count", 2}}}}});
  const auto path = fixture.root() / "quests/catalog.json";
  std::ofstream output(path, std::ios::trunc);
  output << document;
  output.close();
  QuestsFeature shared_feature;
  shared_feature.load_catalog(fixture.root() / "client");
  store.progress.quests[501] = QuestProgress{};
  context.ledger = std::make_shared<QuestIntentLedger>(store.progress);
  auto shared_session = shared_feature.create_session(store, context, progression, events);
  shared_session->select_character(store.actor.id);
  bool processed = false;
  context.commit_monster_reward = [&](const std::string &, const WorldProgress &before,
                                       const std::optional<WorldProgress> &after,
                                       const ItemTrade &) {
    if (processed) {
      return MonsterRewardCommitResult{MonsterRewardCommitStatus::already_processed, {}};
    }
    if (after && !store.progression_save(before, *after)) {
      return MonsterRewardCommitResult{};
    }
    processed = true;
    throw std::runtime_error("simulated lost acknowledgement");
  };
  MonsterRewardReceipt receipt;
  receipt.receipt_id = "0123456789abcdef0123456789abcdef";
  receipt.credited_unit_id = 9001;
  receipt.npc_cms_id = 5001;
  receipt.position = WorldPosition{kMap, 0, 0, 0, 0};
  receipt.quest_attempts[501] = {0, 0};
  std::vector<GameplayPacket> packets;

  EXPECT_THROW(shared_session->record_queued_monster_reward(receipt, packets), std::runtime_error);
  ASSERT_TRUE(context.watched_intent);
  const auto capture = context.ledger->capture();
  ASSERT_TRUE(capture);
  EXPECT_FALSE(context.ledger->resolve(*capture));
  const auto intent = *context.watched_intent;
  context.ledger->finish_mutation(intent, store.progress);
  shared_session->retire_reconciled_reward_intent(receipt.receipt_id, intent);
  context.quests_forget_mutation(intent);
  const auto resolved = context.ledger->resolve(*capture);
  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved->at(501).objectives.at(1), 1);
  shared_session->record_queued_monster_reward(receipt, packets);
  EXPECT_EQ(store.progress.quests.at(501).objectives.at(1), 1);
  context.ledger->release(*capture);
}

TEST_F(QuestOwner, QueuedKillCommitsCompanyRewardWithItsReceipt) {
  auto document = quest_document();
  auto &quest = document["quests"][0];
  quest["auto_complete"] = true;
  quest["company_contribution"] = 1;
  quest["progresses"] = Json::array({
      {{"sequence", 1}, {"group", 1}, {"kind", "HuntMonster"},
       {"parameters", {{"npcId", 5001}, {"count", 1}}}}});
  const auto path = fixture.root() / "quests/catalog.json";
  std::ofstream output(path, std::ios::trunc);
  output << document;
  output.close();
  QuestsFeature shared_feature;
  shared_feature.load_catalog(fixture.root() / "client");
  store.progress.quests[501] = QuestProgress{};
  context.ledger = std::make_shared<QuestIntentLedger>(store.progress);
  context.company = blue_salt(500, true);
  auto shared_session = shared_feature.create_session(store, context, progression, events);
  shared_session->select_character(store.actor.id);
  int commits = 0;
  context.commit_monster_effect = [&](const std::string &id, const WorldProgress &before,
                                       const std::optional<WorldProgress> &after, const ItemTrade &,
                                       const QuestServiceEffects &effects) {
    ++commits;
    EXPECT_EQ(id, "abcdef0123456789abcdef0123456789");
    EXPECT_EQ(effects.expected_company_type, kBlueSalt);
    EXPECT_EQ(effects.company_contribution, 1);
    EXPECT_TRUE(after);
    EXPECT_TRUE(store.progression_save(before, *after));
    context.stored_effects.push_back(effects);
    return MonsterRewardCommitResult{MonsterRewardCommitStatus::committed, {}};
  };
  MonsterRewardReceipt receipt;
  receipt.receipt_id = "abcdef0123456789abcdef0123456789";
  receipt.credited_unit_id = 9002;
  receipt.npc_cms_id = 5001;
  receipt.position = WorldPosition{kMap, 0, 0, 0, 0};
  receipt.quest_attempts[501] = {0, 0};
  std::vector<GameplayPacket> packets;

  shared_session->record_queued_monster_reward(receipt, packets);
  EXPECT_EQ(commits, 1);
  EXPECT_EQ(store.progress.revision, 1);
  EXPECT_TRUE(store.progress.quests.at(501).complete);
  EXPECT_EQ(context.contribution, 0);
}

TEST_F(QuestOwner, QueuedQuestEffectLostAckCannotReplanBeforeReadback) {
  auto document = quest_document();
  auto &quest = document["quests"][0];
  quest["auto_complete"] = true;
  quest["company_contribution"] = 1;
  quest["progresses"] = Json::array({
      {{"sequence", 1}, {"group", 1}, {"kind", "HuntMonster"},
       {"parameters", {{"npcId", 5001}, {"count", 1}}}}});
  const auto path = fixture.root() / "quests/catalog.json";
  std::ofstream output(path, std::ios::trunc);
  output << document;
  output.close();
  QuestsFeature shared_feature;
  shared_feature.load_catalog(fixture.root() / "client");
  store.progress.quests[501] = QuestProgress{};
  context.ledger = std::make_shared<QuestIntentLedger>(store.progress);
  context.company = blue_salt(500, true);
  auto shared_session = shared_feature.create_session(store, context, progression, events);
  shared_session->select_character(store.actor.id);
  int writes = 0;
  context.commit_monster_effect = [&](const std::string &id, const WorldProgress &before,
                                       const std::optional<WorldProgress> &after, const ItemTrade &,
                                       const QuestServiceEffects &effects) -> MonsterRewardCommitResult {
    ++writes;
    EXPECT_EQ(id, "abcdef0123456789abcdef0123456789");
    EXPECT_EQ(effects.company_contribution, 1);
    EXPECT_TRUE(after);
    EXPECT_TRUE(store.progression_save(before, *after));
    throw std::runtime_error("Injected lost monster quest acknowledgement");
  };
  MonsterRewardReceipt receipt;
  receipt.receipt_id = "abcdef0123456789abcdef0123456789";
  receipt.credited_unit_id = 9002;
  receipt.npc_cms_id = 5001;
  receipt.position = WorldPosition{kMap, 0, 0, 0, 0};
  receipt.quest_attempts[501] = {0, 0};
  std::vector<GameplayPacket> packets;
  EXPECT_THROW(shared_session->record_queued_monster_reward(receipt, packets), std::runtime_error);
  ASSERT_TRUE(context.watched_intent);
  EXPECT_THROW(shared_session->record_queued_monster_reward(receipt, packets), std::runtime_error);
  EXPECT_EQ(writes, 1);
  EXPECT_TRUE(store.progress.quests.at(501).complete);
  EXPECT_TRUE(packets.empty());
  const auto intent = *context.watched_intent;
  context.ledger->finish_mutation(intent, store.progress);
  shared_session->retire_reconciled_reward_intent(receipt.receipt_id, intent);
  context.quests_forget_mutation(intent);
}

TEST_F(QuestOwner, NoBookMonsterExperienceCommitsBeforeCacheAndLevelEffects) {
  QuestsFeature without_catalog;
  auto fallback = without_catalog.create_session(store, context, progression, events);
  store.progress.experience = 99;
  std::vector<GameplayPacket> packets;
  fallback->record_monster_reward(WorldPosition{kMap, 0, 0, 0, 0}, 5009, 5001, packets);
  EXPECT_EQ(context.calls, (std::vector<std::string>{"commit", "cache", "level"}));
  EXPECT_EQ(store.progress.level, 2);
  EXPECT_EQ(store.progress.revision, 1);
  ASSERT_EQ(packets.size(), 2U);
  EXPECT_EQ(packets[0].opcode, kChangeCharacterBaseNotification);
  EXPECT_EQ(packets[1].opcode, 78);
}

TEST_F(QuestOwner, MonsterCommitConflictThrowsWithoutPublishingOrAppendingPackets) {
  QuestsFeature without_catalog;
  auto fallback = without_catalog.create_session(store, context, progression, events);
  store.commit_succeeds = false;
  std::vector<GameplayPacket> packets;
  EXPECT_THROW(
      fallback->record_monster_reward(WorldPosition{kMap, 0, 0, 0, 0}, 5009, 5001, packets),
      std::runtime_error);
  EXPECT_EQ(context.calls, (std::vector<std::string>{"commit", "commit", "commit"}));
  EXPECT_TRUE(packets.empty());
}

TEST_F(QuestOwner, MonsterRewardRetriesAConcurrentRevisionBeforePublishing) {
  QuestsFeature without_catalog;
  auto fallback = without_catalog.create_session(store, context, progression, events);
  store.progress.experience = 99;
  bool raced = false;
  store.before_save = [this, &raced](const WorldProgress &, const WorldProgress &) {
    context.calls.push_back("commit");
    if (!raced) {
      raced = true;
      ++store.progress.revision;
    }
  };
  std::vector<GameplayPacket> packets;
  fallback->record_monster_reward(WorldPosition{kMap, 0, 0, 0, 0}, 5009, 5001, packets);
  EXPECT_EQ(store.saves, 2);
  EXPECT_EQ(store.progress.revision, 2);
  EXPECT_EQ(store.progress.level, 2);
  EXPECT_EQ(context.calls, (std::vector<std::string>{"commit", "commit", "cache", "level"}));
  ASSERT_EQ(packets.size(), 2U);
}

TEST_F(QuestOwner, FailedEtherealRollAdvancesRandomStateForTheNextKill) {
  auto document = quest_document();
  document["quests"][0]["progresses"] = Json::array({
      {{"sequence", 1}, {"group", 1}, {"kind", "EtherealHuntMonster"},
       {"parameters", {{"npcId", 5001}, {"count", 1}, {"rate", 50.0},
                        {"minValue", 1}, {"maxValue", 1}}}}});
  const auto path = fixture.root() / "quests/catalog.json";
  std::ofstream output(path, std::ios::trunc);
  output << document;
  output.close();
  QuestsFeature test_feature;
  test_feature.load_catalog(fixture.root() / "client");
  auto quest_session = test_feature.create_session(store, context, {}, events);
  quest_session->select_character(store.actor.id);
  store.progress.quests[501] = QuestProgress{};
  store.progress.quests[501].objectives[1] = 0;

  std::optional<std::uint32_t> seed;
  for (std::uint32_t candidate = 0; candidate < 1000; ++candidate) {
    std::mt19937 generator(candidate);
    std::uniform_real_distribution<double> percent(0.0, 100.0);
    if (percent(generator) >= 50.0 && percent(generator) < 50.0) {
      seed = candidate;
      break;
    }
  }
  ASSERT_TRUE(seed);
  quest_session->seed_random_for_test(*seed);
  std::vector<GameplayPacket> packets;
  quest_session->record_monster_reward(WorldPosition{kMap, 0, 0, 0, 0}, 1, 5001, packets);
  EXPECT_EQ(store.saves, 0);
  quest_session->record_monster_reward(WorldPosition{kMap, 0, 0, 0, 0}, 2, 5001, packets);
  EXPECT_EQ(store.saves, 1);
  EXPECT_EQ(store.progress.quests.at(501).objectives.at(1), 1);
}

TEST_F(QuestOwner, DisabledMonsterExperienceStillReadsCharacterThenProgress) {
  QuestsFeature without_catalog;
  auto disabled = without_catalog.create_session(store, context, {}, events);
  std::vector<GameplayPacket> packets;
  disabled->record_monster_reward(WorldPosition{kMap, 0, 0, 0, 0}, 5009, 5001, packets);
  EXPECT_EQ(store.character_reads, 1);
  EXPECT_EQ(store.progress_reads, 1);
  EXPECT_EQ(store.saves, 0);
  EXPECT_TRUE(packets.empty());
}

TEST_F(QuestOwner, MissingCatalogDisablesButMalformedPresentCatalogRemainsFatal) {
  QuestsFeature absent;
  absent.load_catalog({});
  EXPECT_FALSE(absent.catalog());
  const auto path = fixture.root() / "quests/catalog.json";
  std::ofstream output(path, std::ios::trunc);
  output << "{}";
  output.close();
  EXPECT_THROW(absent.load_catalog(fixture.root() / "client"), Json::out_of_range);
}

TEST_F(QuestOwner, CompanyQuestIsRefusedWithoutCompanyStandingOrPermit) {
  auto without_service = acceptance(kCompanyQuest);
  ASSERT_TRUE(session->handle(without_service));
  ASSERT_EQ(without_service.session_packets.size(), 1U);
  EXPECT_EQ(without_service.session_packets[0].opcode, 2);

  context.company = blue_salt(1500, false);
  auto without_permit = acceptance(kCompanyQuest);
  ASSERT_TRUE(session->handle(without_permit));
  ASSERT_EQ(without_permit.session_packets.size(), 1U);
  EXPECT_EQ(without_permit.session_packets[0].opcode, 2);

  context.company = blue_salt(499, true);
  auto too_little = acceptance(kCompanyQuest);
  ASSERT_TRUE(session->handle(too_little));
  EXPECT_EQ(too_little.session_packets[0].opcode, 2);

  EXPECT_EQ(store.saves, 0);
  EXPECT_FALSE(store.progress.quests.contains(kCompanyQuest));
  EXPECT_TRUE(context.unlocked.empty());
}

TEST_F(QuestOwner, CompanyQuestUnlocksTheCompanyLevelOnlyAfterItsCompletionCommits) {
  context.company = blue_salt(500, true);
  context.ledger = std::make_shared<QuestIntentLedger>(store.progress);

  auto frame = acceptance(kCompanyQuest);
  ASSERT_TRUE(session->handle(frame));

  ASSERT_TRUE(store.progress.quests.contains(kCompanyQuest));
  EXPECT_TRUE(store.progress.quests.at(kCompanyQuest).complete);
  EXPECT_EQ(context.calls, (std::vector<std::string>{"commit", "cache"}));
  ASSERT_EQ(context.stored_effects.size(), 1U);
  EXPECT_EQ(context.stored_effects.front().expected_company_type, kBlueSalt);
  EXPECT_EQ(context.stored_effects.front().company_unlocks,
            (std::vector<QuestServiceUnlock>{{kBlueSalt, 2}}));

  // A repeated acceptance resends the saved quest without unlocking again.
  auto repeated = acceptance(kCompanyQuest);
  ASSERT_TRUE(session->handle(repeated));
  EXPECT_EQ(context.stored_effects.size(), 1U);
}

TEST_F(QuestOwner, FailedCompletionCommitAppliesNoCompanyUnlock) {
  context.company = blue_salt(500, true);
  context.ledger = std::make_shared<QuestIntentLedger>(store.progress);
  store.commit_succeeds = false;

  auto frame = acceptance(kCompanyQuest);
  ASSERT_TRUE(session->handle(frame));

  EXPECT_EQ(frame.session_packets.size(), 1U);
  EXPECT_TRUE(context.unlocked.empty());
}

TEST_F(QuestOwner, LostQuestEffectAcknowledgementKeepsItsIntentUntilReadback) {
  context.company = blue_salt(500, true);
  context.ledger = std::make_shared<QuestIntentLedger>(store.progress);
  context.commit_service_effect = [this](const std::string &effect_id,
                                          const WorldProgress &before,
                                          const std::optional<WorldProgress> &after,
                                          const ItemTrade &,
                                          const QuestServiceEffects &effects) -> QuestMutationResult {
    EXPECT_EQ(effect_id, std::string(32, 'a'));
    EXPECT_EQ(effects.expected_company_type, kBlueSalt);
    EXPECT_EQ(effects.expected_company_generation, 2);
    EXPECT_EQ(effects.company_contribution, 1);
    EXPECT_TRUE(after);
    EXPECT_TRUE(store.progression_save(before, *after));
    context.stored_effects.push_back(effects);
    throw std::runtime_error("Injected lost quest effect acknowledgement");
  };

  auto frame = acceptance(kContributionQuest);
  EXPECT_THROW(session->handle(frame), std::runtime_error);
  ASSERT_TRUE(context.watched_intent);
  EXPECT_EQ(context.ambiguous_retries, 1);
  ASSERT_EQ(context.stored_effects.size(), 1U);
  const auto capture = context.ledger->capture();
  ASSERT_TRUE(capture);
  EXPECT_FALSE(context.ledger->resolve(*capture));
  const auto intent = *context.watched_intent;
  context.ledger->finish_mutation(intent, store.progress);
  context.quests_forget_mutation(intent);
  ASSERT_TRUE(context.ledger->resolve(*capture));
  EXPECT_TRUE(store.progress.quests.at(kContributionQuest).complete);
  EXPECT_TRUE(frame.session_packets.empty());
  context.ledger->release(*capture);
}

TEST_F(QuestOwner, WorldEntryReappliesUnlocksOfCompletedCompanyQuests) {
  context.company = blue_salt(500, true);
  QuestProgress completed;
  completed.complete = true;
  completed.success_count = 1;
  store.progress.quests.emplace(kCompanyQuest, completed);

  FrameContext entry(20001, WorldGameplayRequest{20001, 0, Json::object()});
  session->observe_snapshot(entry);

  EXPECT_EQ(context.unlocked, (std::vector<QuestCompanyUnlock>{{kBlueSalt, 2}}));
}

TEST_F(QuestOwner, HistoricalUnlockRepairFollowsCurrentCompanyOnLaterEntry) {
  QuestProgress completed;
  completed.complete = true;
  completed.success_count = 1;
  store.progress.quests.emplace(kCompanyQuest, completed);

  FrameContext first(20001, WorldGameplayRequest{20001, 0, Json::object()});
  session->observe_snapshot(first);
  EXPECT_TRUE(context.unlocked.empty());

  context.company = blue_salt(500, true);
  auto later_session = feature.create_session(store, context, progression, events);
  later_session->select_character(store.actor.id);
  FrameContext later(20001, WorldGameplayRequest{20001, 0, Json::object()});
  later_session->observe_snapshot(later);
  EXPECT_EQ(context.unlocked, (std::vector<QuestCompanyUnlock>{{kBlueSalt, 2}}));
}

TEST_F(QuestOwner, BlockedHistoricalUnlockRepairRetriesAfterSettlement) {
  QuestProgress completed;
  completed.complete = true;
  completed.success_count = 1;
  store.progress.quests.emplace(kCompanyQuest, completed);
  context.company = blue_salt(500, true);
  context.company_repair_blocked = true;

  FrameContext first(20001, WorldGameplayRequest{20001, 0, Json::object()});
  session->observe_snapshot(first);
  EXPECT_EQ(context.company_repair_attempts, 1);
  EXPECT_TRUE(context.unlocked.empty());
  FrameContext early(20001, WorldGameplayRequest{20001, 0, Json::object()});
  session->observe_snapshot(early);
  EXPECT_EQ(context.company_repair_attempts, 1);

  context.company_repair_blocked = false;
  context.steady_now += std::chrono::seconds(2);
  FrameContext retry(20001, WorldGameplayRequest{20001, 0, Json::object()});
  session->observe_snapshot(retry);
  EXPECT_EQ(context.company_repair_attempts, 2);
  EXPECT_EQ(context.unlocked, (std::vector<QuestCompanyUnlock>{{kBlueSalt, 2}}));
  context.steady_now += std::chrono::seconds(2);
  FrameContext done(20001, WorldGameplayRequest{20001, 0, Json::object()});
  session->observe_snapshot(done);
  EXPECT_EQ(context.company_repair_attempts, 2);
}

TEST_F(QuestOwner, ContributionRewardIsCreditedOnceAfterTheCompletionCommits) {
  context.company = blue_salt(500, true);
  context.ledger = std::make_shared<QuestIntentLedger>(store.progress);
  auto frame = acceptance(kContributionQuest);
  ASSERT_TRUE(session->handle(frame));

  ASSERT_TRUE(store.progress.quests.contains(kContributionQuest));
  EXPECT_TRUE(store.progress.quests.at(kContributionQuest).complete);
  EXPECT_EQ(context.calls, (std::vector<std::string>{"commit", "cache"}));
  ASSERT_EQ(context.stored_effects.size(), 1U);
  EXPECT_EQ(context.stored_effects.front().company_contribution, 1);

  auto repeated = acceptance(kContributionQuest);
  ASSERT_TRUE(session->handle(repeated));
  EXPECT_EQ(context.stored_effects.size(), 1U);
}

TEST_F(QuestOwner, ContributionIsNeitherCreditedOnAFailedCommitNorReplayedAtWorldEntry) {
  context.ledger = std::make_shared<QuestIntentLedger>(store.progress);
  store.commit_succeeds = false;
  auto failed = acceptance(kContributionQuest);
  ASSERT_TRUE(session->handle(failed));
  EXPECT_EQ(context.contribution, 0);

  QuestProgress completed;
  completed.complete = true;
  completed.success_count = 1;
  store.progress.quests.emplace(kContributionQuest, completed);
  FrameContext entry(20001, WorldGameplayRequest{20001, 0, Json::object()});
  session->observe_snapshot(entry);
  EXPECT_EQ(context.contribution, 0);
}

} // namespace
} // namespace chronicles::quests
