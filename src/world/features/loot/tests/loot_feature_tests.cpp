#include "testing/loot_test_support.hpp"
#include "world/features/loot/loot_feature.hpp"
#include "world/features/party/party_world.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace chronicles::loot {
namespace {

using namespace loot_test;

class LootContext final : public LootSessionContext {
public:
  LootContext() {
    actor.id = character_id;
    actor.name = "LootOwner";
    progress.revision = 7;
    progress.stackables[kHerb] = 4;
  }

  LootPersistence loot_tutorial_persistence(const Handoff &entry) override {
    ++private_factories;
    private_entry = entry;
    LootPersistence persistence;
    persistence.load_character = [this] {
      return std::optional<Character>{actor};
    };
    persistence.load_progress = [this] {
      return progress;
    };
    persistence.commit = [this](const LootCommit &commit) {
      progress = commit.after;
      return std::optional<std::vector<CharacterItem>>{commit.granted_items};
    };
    return persistence;
  }

  std::shared_ptr<party::PartyWorld> loot_party_world() override {
    ++party_queries;
    return party_world;
  }

  PartyChannelKey loot_party_channel_key() override {
    return [] {
      return std::uint64_t{0};
    };
  }

  PartyLootDelivery loot_party_delivery() override {
    return [](const party::PartyUpdate &) {
    };
  }

  void loot_install_defeat_observer(MonsterDefeatObserver replacement) override {
    ++observer_installs;
    observer = std::move(replacement);
  }

  std::int64_t loot_character_id() const override {
    ++identity_queries;
    return character_id;
  }

  std::optional<Character> loot_stored_character() override {
    calls.push_back("character");
    ++character_loads;
    return actor;
  }

  WorldProgress loot_stored_progress() override {
    ++progress_loads;
    return progress;
  }

  LootClaimResult loot_lookup_claim(const std::string &, const std::string &) override {
    if (receipt_committed) {
      return {LootClaimStatus::already_claimed_by_same_character, {}};
    }
    return {LootClaimStatus::not_found, {}};
  }

  LootClaimResult loot_commit_items(const LootCommit &commit) override {
    calls.push_back("commit");
    ++commits;
    EXPECT_EQ(commit.before, progress);
    if (!commit_allowed) {
      return {LootClaimStatus::conflict, {}};
    }
    progress = commit.after;
    if (lose_commit_acknowledgement) {
      receipt_committed = true;
      lose_commit_acknowledgement = false;
      throw std::runtime_error("Synthetic lost loot commit acknowledgement");
    }
    return {LootClaimStatus::committed, commit.granted_items};
  }

  void loot_force_reconnect() override {
    ++reconnect_requests;
  }

  bool loot_has_combat() const override {
    return combat;
  }

  bool loot_has_world_mobs() const override {
    return world_mobs;
  }

  const Json &loot_current_combat_stats() const override {
    return combat_stats;
  }

  std::int64_t loot_monotonic_ticks() const override {
    return kStartTicks + 1;
  }

  void loot_publish_snapshot(const Character &character, const Json *prepared_stats) override {
    calls.push_back("snapshot");
    ++snapshots;
    EXPECT_EQ(character.id, actor.id);
    EXPECT_EQ(prepared_stats, &combat_stats);
  }

  Character actor;
  WorldProgress progress;
  Json combat_stats{{"69", 37}};
  Handoff private_entry;
  std::shared_ptr<party::PartyWorld> party_world{
      std::make_shared<party::PartyWorld>(party::PartyWorldConfig{})};
  MonsterDefeatObserver observer;
  std::vector<std::string> calls;
  std::int64_t character_id{17};
  mutable int identity_queries{};
  int private_factories{};
  int party_queries{};
  int progress_loads{};
  int character_loads{};
  int commits{};
  int reconnect_requests{};
  int snapshots{};
  int observer_installs{};
  bool combat{true};
  bool world_mobs{true};
  bool commit_allowed{true};
  bool receipt_committed{};
  bool lose_commit_acknowledgement{};
};

MonsterDefeat defeat(std::uint64_t unit_id = kCorpse) {
  return {unit_id, kSinglePackNpc, 999, {kMap, 0, 0, 0, 0}, kStartTicks, 0};
}

FrameContext take_frame(std::uint16_t opcode = kLootItemRequestOpcode) {
  FrameContext frame(opcode, WorldGameplayRequest{opcode, 1, loot_request(kCorpse)});
  frame.gameplay_position = WorldPosition{kMap, 0, 0, 0, 0};
  return frame;
}

std::vector<std::uint16_t> opcodes(const std::vector<GameplayPacket> &packets) {
  std::vector<std::uint16_t> result;
  for (const auto &packet : packets) {
    result.push_back(packet.opcode);
  }
  return result;
}

TEST(LootFeature, DisabledPrivateCreationIsColdButWorldAttachStillLoadsItsCache) {
  LootFeature feature(nullptr);
  LootContext context;
  auto player = feature.create_session(context);
  Handoff entry;
  entry.character_id = 17;
  player->create_tutorial(17, entry);
  EXPECT_EQ(context.private_factories, 0);
  EXPECT_EQ(context.party_queries, 0);
  EXPECT_EQ(context.progress_loads, 0);
  EXPECT_FALSE(player->has_session());
  EXPECT_FALSE(player->next_action_tick());
  player->attach_world_loot();
  EXPECT_EQ(context.progress_loads, 1);
  EXPECT_EQ(context.observer_installs, 0);
  EXPECT_FALSE(player->has_session());
}

TEST(LootFeature, PrivateFactoryUsesTheExplicitBootstrapIdentityAndRawObserverCapture) {
  LootFeature feature(std::make_shared<LootData>(profile_fixture()));
  LootContext context;
  context.character_id = 99;
  context.actor.id = 17;
  auto player = feature.create_session(context);
  Handoff entry;
  entry.account_id = 11;
  entry.character_id = 17;
  entry.world_id = 1;
  player->create_tutorial(17, entry);
  ASSERT_TRUE(context.observer);
  EXPECT_EQ(context.private_entry.account_id, 11);
  EXPECT_EQ(context.private_entry.character_id, 17);
  EXPECT_EQ(context.identity_queries, 0);
  const auto original = defeat();
  const auto reaction = context.observer(original);
  EXPECT_EQ(original.killer_character_id, 999);
  EXPECT_TRUE(reaction.manages_corpse_removal);
  EXPECT_EQ(context.identity_queries, 0);
  EXPECT_EQ(context.progress_loads, 0);
  EXPECT_EQ(context.party_queries, 1);
}

TEST(LootFeature, WorldDefeatUsesTheCachedProgressAndDoesNotConstructAPartyAudience) {
  LootFeature feature(std::make_shared<LootData>(profile_fixture()));
  LootContext context;
  auto player = feature.create_session(context);
  player->attach_world_loot();
  ASSERT_TRUE(context.observer);
  EXPECT_EQ(context.progress_loads, 1);
  const auto reaction = context.observer(defeat());
  EXPECT_TRUE(reaction.manages_corpse_removal);
  EXPECT_EQ(opcodes(reaction.packets), std::vector<std::uint16_t>({kUnitLootItemInfoOpcode}));
  EXPECT_EQ(context.progress_loads, 1);
  EXPECT_EQ(context.character_loads, 0);
  EXPECT_EQ(context.commits, 0);
  EXPECT_EQ(context.party_queries, 0);
}

TEST(LootFeature, SharedTakeReloadsThenCommitsAndPublishesTheBorrowedLiveStatsBeforeReplies) {
  LootFeature feature(std::make_shared<LootData>(profile_fixture()));
  LootContext context;
  auto player = feature.create_session(context);
  player->attach_world_loot();
  context.observer(defeat());
  context.calls.clear();
  auto frame = take_frame();
  frame.session_packets.push_back({99, {}});
  ASSERT_TRUE(player->handle_shared(frame));
  EXPECT_EQ(context.progress_loads, 3);
  EXPECT_EQ(context.progress.stackables.at(kHerb), 5);
  EXPECT_EQ(context.calls,
            (std::vector<std::string>{"character", "commit", "character", "snapshot"}));
  EXPECT_EQ(opcodes(frame.session_packets),
            std::vector<std::uint16_t>(
                {99, kLootItemsUpdateOpcode, kUnitLootItemInfoOpcode, kLootItemResponseOpcode}));
}

TEST(LootFeature, CommitRefusalPublishesNoSnapshotAndLeavesTheSameCorpseAvailableForRetry) {
  LootFeature feature(std::make_shared<LootData>(profile_fixture()));
  LootContext context;
  auto player = feature.create_session(context);
  player->attach_world_loot();
  context.observer(defeat());
  context.commit_allowed = false;
  auto first = take_frame();
  ASSERT_TRUE(player->handle_shared(first));
  EXPECT_EQ(opcodes(first.session_packets), std::vector<std::uint16_t>({kLootItemResponseOpcode}));
  EXPECT_EQ(context.snapshots, 0);
  EXPECT_EQ(context.progress.revision, 7);
  context.commit_allowed = true;
  auto retried = take_frame();
  ASSERT_TRUE(player->handle_shared(retried));
  EXPECT_EQ(context.commits, 2);
  EXPECT_EQ(context.snapshots, 1);
  EXPECT_EQ(context.progress.revision, 8);
}

TEST(LootFeature, LostCommitAcknowledgementRequiresFreshWorldEntryWithoutAnotherGrant) {
  LootFeature feature(std::make_shared<LootData>(profile_fixture()));
  LootContext context;
  auto player = feature.create_session(context);
  player->attach_world_loot();
  context.observer(defeat());
  context.lose_commit_acknowledgement = true;

  auto first = take_frame();
  ASSERT_TRUE(player->handle_shared(first));
  EXPECT_EQ(context.reconnect_requests, 1);
  EXPECT_EQ(context.commits, 1);
  const auto granted = context.progress;
  auto retry = take_frame();
  ASSERT_TRUE(player->handle_shared(retry));
  EXPECT_EQ(context.reconnect_requests, 2);
  EXPECT_EQ(context.commits, 1);
  EXPECT_EQ(context.progress, granted);
}

TEST(LootFeature, SharedGuardFallsThroughWithoutLoadingProgressOrReplacingEarlierPackets) {
  LootFeature feature(std::make_shared<LootData>(profile_fixture()));
  LootContext context;
  auto player = feature.create_session(context);
  player->attach_world_loot();
  auto unrelated = take_frame(20003);
  unrelated.session_packets.push_back({99, {}});
  EXPECT_FALSE(player->handle_shared(unrelated));
  context.world_mobs = false;
  auto private_combat = take_frame();
  EXPECT_FALSE(player->handle_shared(private_combat));
  EXPECT_EQ(context.progress_loads, 1);
  EXPECT_EQ(opcodes(unrelated.session_packets), std::vector<std::uint16_t>({99}));
}

TEST(LootFeature, ResetDropsOnlyTheDomainOwnerAndDoesNotInventAnObserverClear) {
  LootFeature feature(std::make_shared<LootData>(profile_fixture()));
  LootContext context;
  auto player = feature.create_session(context);
  player->attach_world_loot();
  context.observer(defeat());
  ASSERT_TRUE(player->next_action_tick());
  const auto installs = context.observer_installs;
  player->reset();
  EXPECT_FALSE(player->has_session());
  EXPECT_FALSE(player->next_action_tick());
  EXPECT_TRUE(player->advance(kStartTicks + 1).empty());
  EXPECT_EQ(context.observer_installs, installs);
  // The composition owner clears/replaces the callback at its original separate point.
  context.observer = {};
}

} // namespace
} // namespace chronicles::loot
