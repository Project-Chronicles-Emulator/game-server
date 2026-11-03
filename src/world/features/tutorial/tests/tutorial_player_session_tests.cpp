#include "common/clock_ticks.hpp"
#include "world/core/frame_context.hpp"
#include "world/features/tutorial/tutorial_player_session.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <utility>

namespace chronicles::tutorial {
namespace {

using Json = nlohmann::json;

std::shared_ptr<TutorialData> minimal_profile(int map_id = 8000) {
  return std::make_shared<TutorialData>(
      Json{{"format_version", 1},
           {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
           {"map_id", map_id},
           {"objects", Json::array()},
           {"quests", Json::array()},
           {"doodads", Json::array()},
           {"utility_items", Json::array()},
           {"potions", Json::array()},
           {"civilians", Json::array()},
           {"sectors", {98317}}});
}

class TutorialContext final : public TutorialSessionContext {
public:
  std::int64_t tutorial_monotonic_ticks() const override {
    return 100000000;
  }

  bool tutorial_has_combat() const override {
    return combat;
  }

  bool tutorial_combat_handles(const WorldGameplayRequest &) const override {
    return combat_handles;
  }

  bool tutorial_combat_busy() const override {
    return combat_busy;
  }

  std::vector<GameplayPacket> tutorial_advance_combat(std::int64_t,
                                                      const WorldPosition &) override {
    calls.push_back("combat advance");
    return {{9001, Json::object()}};
  }

  std::vector<GameplayPacket> tutorial_receive_combat(const WorldGameplayRequest &,
                                                      const WorldPosition &,
                                                      std::int64_t) override {
    calls.push_back("combat receive");
    return {{9002, Json::object()}};
  }

  std::vector<GameplayPacket> tutorial_revive_at_shrine(const WorldGameplayRequest &) override {
    calls.push_back("revive");
    return {{9003, Json::object()}};
  }

  bool tutorial_has_loot() const override {
    return loot;
  }

  bool tutorial_loot_handles(std::uint16_t) const override {
    return loot_handles;
  }

  std::vector<GameplayPacket> tutorial_receive_loot(const Json &, const WorldPosition &,
                                                    std::int64_t) override {
    calls.push_back("loot receive");
    return {{9004, Json::object()}};
  }

  std::vector<GameplayPacket> tutorial_advance_loot(std::int64_t) override {
    calls.push_back("loot advance");
    return {{9005, Json::object()}};
  }

  void tutorial_apply_level_change(const WorldProgress &, std::vector<GameplayPacket> &) override {
    calls.push_back("level");
  }

  void tutorial_append_encumbrance_changes(std::vector<GameplayPacket> &, bool initial) override {
    EXPECT_FALSE(initial);
    calls.push_back("encumbrance");
  }

  void tutorial_persist_player_death(std::vector<GameplayPacket> &) override {
    calls.push_back("death");
  }

  void tutorial_collect_activity_rewards(std::vector<GameplayPacket> &) override {
    calls.push_back("company");
  }

  bool tutorial_authenticated() const override {
    return authenticated;
  }

  void tutorial_end_combat() override {
    calls.push_back("combat end");
    combat = false;
  }

  void tutorial_reset_loot() override {
    calls.push_back("loot reset");
    loot = false;
  }

  std::optional<int> tutorial_population_map() const override {
    return population_map;
  }

  bool tutorial_has_party_identity() const override {
    return party;
  }

  void tutorial_party_graduation(const WorldProgress &progress) override {
    calls.push_back("party graduation");
    EXPECT_TRUE(owner->has_instance());
    EXPECT_EQ(progress.revision, owner->current_instance()->progress().revision);
  }

  bool tutorial_has_mail_repository() const override {
    return mail_repository;
  }

  bool tutorial_has_shop_catalog() const override {
    return shop_catalog;
  }

  std::optional<MailRecord> tutorial_deliver_completion_mail(std::int64_t ticks) override {
    calls.push_back("completion mail");
    EXPECT_FALSE(owner->has_instance());
    EXPECT_GT(ticks, 0);
    MailRecord mail;
    mail.id = 41;
    return mail;
  }

  std::optional<std::string> tutorial_graduation_notice() const override {
    return notice;
  }

  void tutorial_gm_reply(const std::string &text) override {
    EXPECT_EQ(text, *notice);
    calls.push_back("notice");
  }

  void tutorial_set_population_map(std::optional<int> map_id) override {
    calls.push_back("population map");
    entered_map = map_id;
  }

  std::optional<Character> tutorial_stored_character() override {
    calls.push_back("character");
    EXPECT_FALSE(owner->has_instance());
    return character;
  }

  void tutorial_create_gathering(const Character &) override {
    calls.push_back("gathering");
  }

  void tutorial_create_companions(const Character &) override {
    calls.push_back("companions");
  }

  void tutorial_create_quests(std::int64_t character_id) override {
    EXPECT_EQ(character_id, character->id);
    calls.push_back("quests");
  }

  bool tutorial_save_world_position(const WorldPosition &position) override {
    calls.push_back("save position");
    saved_position = position;
    return save_allowed;
  }

  std::uint64_t tutorial_connection_id() const override {
    return 13;
  }

  TutorialPlayerSession *owner{};
  std::vector<std::string> calls;
  std::optional<Character> character;
  std::optional<int> population_map{9011};
  std::optional<int> entered_map;
  std::optional<WorldPosition> saved_position;
  std::optional<std::string> notice;
  bool combat{};
  bool combat_handles{};
  bool combat_busy{};
  bool loot{};
  bool loot_handles{};
  bool authenticated{true};
  bool party{};
  bool mail_repository{true};
  bool shop_catalog{true};
  bool save_allowed{true};
};

class TutorialOwnerTest : public ::testing::Test {
protected:
  TutorialOwnerTest() {
    context.owner = &session;
    context.character = Character{};
    context.character->id = 17;
  }

  void initialize(WorldProgress progress = {}) {
    session.initialize(273, std::move(progress),
                       [](const WorldProgress &, const WorldProgress &) {
                         return true;
                       },
                       {}, {});
  }

  std::shared_ptr<TutorialData> data = minimal_profile();
  TutorialFeature feature{data};
  TutorialContext context;
  TutorialPlayerSession session{feature, context};
  WorldPosition position{8000, 0, 0, 0, 0};
};

TEST_F(TutorialOwnerTest, ColdOwnerDoesNotReadPortsOrAcceptAFrame) {
  const WorldGameplayRequest request{20001, 1, Json::object()};
  EXPECT_FALSE(session.handle_frame(&request, position));
  initialize();
  EXPECT_FALSE(session.handle_frame(nullptr, position));
  EXPECT_TRUE(context.calls.empty());
  EXPECT_FALSE(session.next_action_tick());
}

TEST_F(TutorialOwnerTest, BootstrapKeepsPreloadedProgressAndTheProvidedCommitIdentity) {
  WorldProgress preload;
  preload.revision = 7;
  preload.stackables[40101101] = 2;
  unsigned commits = 0;
  session.initialize(273, preload,
                     [&](const WorldProgress &before, const WorldProgress &after) {
                       EXPECT_EQ(before, preload);
                       EXPECT_EQ(after.revision, 8);
                       ++commits;
                       return true;
                     },
                     {}, {});

  session.current_instance()->consume_used_item(40101101);
  EXPECT_EQ(commits, 1);
  EXPECT_EQ(session.current_instance()->progress().stackables.at(40101101), 1);
  EXPECT_TRUE(context.calls.empty());
}

TEST_F(TutorialOwnerTest, SkillItemPlanDoesNotCommitOrChangeTutorialProgress) {
  WorldProgress preload;
  preload.revision = 7;
  preload.stackables[40101101] = 2;
  int commits = 0;
  session.initialize(273, preload,
                     [&](const WorldProgress &, const WorldProgress &) {
                       ++commits;
                       return true;
                     },
                     {}, {});
  ASSERT_NE(session.current_instance(), nullptr);
  const auto plan = session.current_instance()->prepare_skill_item_use(
      "0123456789abcdef", 40101101);
  EXPECT_EQ(session.current_instance()->progress(), preload);
  EXPECT_EQ(plan.after.stackables.at(40101101), 1);
  EXPECT_EQ(commits, 0);
}

TEST_F(TutorialOwnerTest, SnapshotAndSectorFlagsRemainIndependentAndTheRequestRemainsBorrowed) {
  initialize();
  FrameContext frame(20001, WorldGameplayRequest{20001, 9, Json::object()});
  auto first = session.handle_frame(frame.request(), std::nullopt);
  ASSERT_TRUE(first);
  EXPECT_EQ(first->packets.size(), 5);
  EXPECT_NE(frame.request(), nullptr);
  EXPECT_TRUE(session.handle_frame(frame.request(), std::nullopt)->packets.empty());

  const WorldGameplayRequest map_load{20085, 10, Json::object()};
  auto sectors = session.handle_frame(&map_load, position);
  ASSERT_TRUE(sectors);
  ASSERT_EQ(sectors->packets.size(), 1);
  EXPECT_EQ(sectors->packets.front().message.at("sector_id"), 98317);
  EXPECT_TRUE(session.handle_frame(&map_load, position)->packets.empty());

  session.reset_instance();
  session.reset_bootstrap_flags();
  initialize();
  EXPECT_EQ(session.handle_frame(frame.request(), std::nullopt)->packets.size(), 5);
}

TEST_F(TutorialOwnerTest, CombatPrecedesLootAndBothAdvancesBracketTheChosenRequest) {
  initialize();
  context.combat = true;
  context.combat_handles = true;
  context.loot = true;
  context.loot_handles = true;
  const WorldGameplayRequest request{20168, 3, Json::object()};
  const auto result = session.handle_frame(&request, position);
  ASSERT_TRUE(result);
  EXPECT_EQ(context.calls,
            (std::vector<std::string>{"combat advance", "combat receive", "combat advance",
                                      "loot advance", "level", "encumbrance", "death", "company"}));
  ASSERT_EQ(result->packets.size(), 4);
  EXPECT_EQ(result->packets[0].opcode, 9001);
  EXPECT_EQ(result->packets[1].opcode, 9002);
  EXPECT_EQ(result->packets[2].opcode, 9001);
  EXPECT_EQ(result->packets[3].opcode, 9005);
}

TEST_F(TutorialOwnerTest, BusyCombatRefusesNoncombatStartBeforeLootOrTutorialReceivesIt) {
  initialize();
  context.combat = true;
  context.combat_busy = true;
  context.loot = true;
  context.loot_handles = true;
  const WorldGameplayRequest request{
      20010, 3, {{"start_skill", {{"skill_id", 31}, {"instance_id", 7}}}}};
  const auto result = session.handle_frame(&request, position);
  ASSERT_TRUE(result);
  ASSERT_EQ(result->packets.size(), 4);
  EXPECT_EQ(result->packets[1].opcode, 13);
  EXPECT_EQ(result->packets[1].message.at("ec"), "Skill_UsingSkill");
  EXPECT_EQ(context.calls,
            (std::vector<std::string>{"combat advance", "combat advance", "loot advance", "level",
                                      "encumbrance", "death", "company"}));
}

TEST_F(TutorialOwnerTest, ReviveHasPriorityOverCombatAndLootWhileNoncombatLootStaysOutsideCodec) {
  initialize();
  context.combat = true;
  context.combat_handles = true;
  context.loot = true;
  context.loot_handles = true;
  const WorldGameplayRequest revive{20039, 3, Json::object()};
  ASSERT_TRUE(session.handle_frame(&revive, position));
  EXPECT_EQ(context.calls[1], "revive");

  context.calls.clear();
  context.combat_handles = false;
  const WorldGameplayRequest loot{20168, 4, Json::object()};
  ASSERT_TRUE(session.handle_frame(&loot, position));
  EXPECT_EQ(context.calls[1], "loot receive");
  EXPECT_EQ(context.calls.back(), "company");
}

TEST_F(TutorialOwnerTest, FollowupDropsCombatBeforeLootButKeepsSnapshotFlagAndRearmsSectors) {
  initialize();
  const WorldGameplayRequest time_sync{20001, 1, Json::object()};
  ASSERT_TRUE(session.handle_frame(&time_sync, std::nullopt));
  auto followup = minimal_profile(10117);
  data->followup_profiles[10117] = followup;
  context.calls.clear();

  const auto result = session.leave_map({10117, {10117, 1, 2, 3, 0}});
  ASSERT_TRUE(session.has_instance());
  EXPECT_EQ(&session.current_instance()->data(), followup.get());
  EXPECT_EQ(context.calls, (std::vector<std::string>{"combat end", "loot reset", "save position"}));
  EXPECT_FALSE(result.completion_mail);
  EXPECT_TRUE(session.handle_frame(&time_sync, std::nullopt)->packets.empty());
  const WorldGameplayRequest map_load{20085, 2, Json::object()};
  EXPECT_EQ(session.handle_frame(&map_load, WorldPosition{10117, 1, 2, 3, 0})->packets.size(), 1);
}

TEST_F(TutorialOwnerTest, GraduationUsesLiveProgressBeforeResetThenMailAndServicesBeforeSaving) {
  initialize();
  data->npcs[12] = {{"initially_active", true}};
  data->npcs[13] = {{"initially_active", false}};
  context.party = true;
  context.notice = "Graduated";
  context.save_allowed = false;
  const auto result = session.leave_map({9011, {9011, 1, 2, 3, 0}});

  EXPECT_FALSE(session.has_instance());
  EXPECT_EQ(result.removed_unit_ids, (std::vector<std::string>{"12"}));
  ASSERT_TRUE(result.completion_mail);
  EXPECT_EQ(result.completion_mail->id, 41);
  EXPECT_EQ(context.entered_map, 9011);
  ASSERT_TRUE(context.saved_position);
  EXPECT_EQ(*context.saved_position, result.spawn);
  EXPECT_EQ(context.calls,
            (std::vector<std::string>{"combat end", "loot reset", "party graduation",
                                      "completion mail", "notice", "population map", "character",
                                      "gathering", "companions", "quests", "save position"}));
}

TEST_F(TutorialOwnerTest, UnauthenticatedMapMoveFailsBeforeAnyLifecycleEffect) {
  initialize();
  context.authenticated = false;
  EXPECT_THROW(session.leave_map({9011, {9011, 0, 0, 0, 0}}), std::logic_error);
  EXPECT_TRUE(session.has_instance());
  EXPECT_TRUE(context.calls.empty());
}

TEST(TutorialRewardOwner, AssignsOneTimestampAndLowestSlotsBeforeAtomicCommitAndSnapshot) {
  Character character;
  character.id = 17;
  character.items = {{1, 101, 0}, {2, 102, 2}};
  std::vector<std::string> calls;
  TutorialRewardPersistence persistence;
  persistence.load_character = [&] {
    calls.push_back("character");
    return std::optional<Character>{character};
  };
  WorldProgress before;
  before.revision = 7;
  auto after = before;
  after.revision = 8;
  const auto earliest = unix_epoch_ticks_now();
  persistence.commit = [&](const WorldProgress &old, const WorldProgress &next,
                           const ItemTrade &trade) {
    calls.push_back("commit");
    EXPECT_EQ(old, before);
    EXPECT_EQ(next, after);
    EXPECT_EQ(trade.granted_items.size(), 2);
    EXPECT_EQ(trade.granted_items[0].inventory_slot, 1);
    EXPECT_EQ(trade.granted_items[1].inventory_slot, 3);
    EXPECT_EQ(trade.granted_items[0].created_epoch_ticks,
              trade.granted_items[1].created_epoch_ticks);
    EXPECT_GE(trade.granted_items[0].created_epoch_ticks, earliest);
    auto stored = trade.granted_items;
    stored[0].id = 31;
    stored[1].id = 32;
    return std::optional<std::vector<CharacterItem>>{std::move(stored)};
  };
  persistence.items_changed = [&](const Character &live) {
    calls.push_back("snapshot");
    ASSERT_EQ(live.items.size(), 4);
    EXPECT_EQ(live.items[2].id, 31);
    EXPECT_EQ(live.items[3].id, 32);
  };

  const auto result =
      commit_tutorial_instances(persistence, before, after, {{0, 103, -1}, {0, 104, -1}});
  EXPECT_EQ(result.status, TutorialSession::InstanceCommitStatus::committed);
  ASSERT_EQ(result.stored.size(), 2);
  EXPECT_EQ(result.stored[0].id, 31);
  EXPECT_EQ(calls, (std::vector<std::string>{"character", "commit", "snapshot"}));
}

TEST(TutorialRewardOwner, EmptyStoredGrantStillRefreshesSnapshotAndConflictDoesNot) {
  unsigned snapshots = 0;
  bool allowed = true;
  TutorialRewardPersistence persistence;
  persistence.load_character = [] {
    return std::optional<Character>{Character{}};
  };
  persistence.commit = [&](const WorldProgress &, const WorldProgress &,
                           const ItemTrade &) -> std::optional<std::vector<CharacterItem>> {
    if (!allowed) {
      return std::nullopt;
    }
    return std::vector<CharacterItem>{};
  };
  persistence.items_changed = [&](const Character &) {
    ++snapshots;
  };
  EXPECT_EQ(commit_tutorial_instances(persistence, {}, {}, {}).status,
            TutorialSession::InstanceCommitStatus::committed);
  EXPECT_EQ(snapshots, 1);

  allowed = false;
  EXPECT_EQ(commit_tutorial_instances(persistence, {}, {}, {}).status,
            TutorialSession::InstanceCommitStatus::conflict);
  EXPECT_EQ(snapshots, 1);
}

TEST(TutorialRewardOwner, MissingCharacterAndFullInventoryDoNotAttemptAnyCommit) {
  TutorialRewardPersistence missing;
  missing.load_character = [] {
    return std::optional<Character>{};
  };
  EXPECT_EQ(commit_tutorial_instances(missing, {}, {}, {{0, 101, -1}}).status,
            TutorialSession::InstanceCommitStatus::conflict);

  Character full;
  for (int slot = 0; slot <= kMaximumInventorySlot; ++slot) {
    full.items.push_back({slot + 1, 101, slot});
  }
  missing.load_character = [&] {
    return std::optional<Character>{full};
  };
  EXPECT_EQ(commit_tutorial_instances(missing, {}, {}, {{0, 101, -1}}).status,
            TutorialSession::InstanceCommitStatus::inventory_full);
}

} // namespace
} // namespace chronicles::tutorial
