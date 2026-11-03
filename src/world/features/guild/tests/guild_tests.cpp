#include "testing/catalog_test_support.hpp"
#include "persistence/memory_quest_service_coordinator.hpp"
#include "persistence/quest_service_effect_codec.hpp"
#include "data/client_catalog.hpp"
#include "world/features/company/company.hpp"
#include "testing/company_test_support.hpp"
#include "testing/memory_repositories.hpp"
#include "world/features/guild/guild.hpp"
#include "world/features/guild/guild_world.hpp"
#include "world/features/npc/npc_population.hpp"
#include "world/features/chat/native_chat.hpp"
#include "world/features/guild/native_guild.hpp"
#include "services/front/native_ws.hpp"
#include "p_c2f.ws.guild.pb.h"
#include "p_f2c.ws.guild.pb.h"
#include "p_system.pb.h"
#include "protocol/protocol.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

#include <chrono>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace chronicles {
namespace {

using Json = nlohmann::json;
using testing_support::kAntiqus;
using testing_support::seat_in_company;
using testing_support::TestCatalog;
using x5::ec::EErrorCode;

constexpr std::int64_t kTicksPerHour = 3600 * 10'000'000LL;
constexpr int kWorldId = 1;

TEST(MemoryGuildQuestEffects, PendingRewardBlocksDepartureAndDisbandUntilSettlement) {
  auto coordinator = std::make_shared<MemoryQuestServiceCoordinator>();
  coordinator->owner_epoch = 1;
  MemoryGuildStore store(coordinator);
  store.adopt_quest_service_owner_epoch(1);
  GuildRecord guild;
  guild.world_id = kWorldId;
  guild.name = "QuestGuard";
  GuildMemberRecord master;
  master.char_id = 1;
  master.grade = GuildGrade::master;
  const auto created = store.create_guild(guild, master);
  ASSERT_TRUE(created);

  QuestServiceEffectEvent event;
  event.character_id = master.char_id;
  event.world_id = kWorldId;
  event.effect_id = std::string(32, 'b');
  event.effects.expected_guild_id = created->id;
  event.company_applied = true;
  {
    std::lock_guard lock(coordinator->mutex);
    ASSERT_TRUE(coordinator->record_event(event));
  }
  GuildHistory history;
  history.char_id = master.char_id;
  const auto before = store.load(kWorldId);
  ASSERT_EQ(before.guilds.size(), 1U);
  ASSERT_EQ(before.members.size(), 1U);
  auto next_guild = before.guilds.front();
  auto next_member = before.members.front();
  next_guild.experience = 10;
  next_member.contribution = 10;
  EXPECT_THROW(store.commit_progression(before.guilds.front(), before.members.front(),
                                        next_guild, next_member), std::runtime_error);
  const auto after_refusal = store.load(kWorldId);
  EXPECT_EQ(after_refusal.guilds.front().experience, 0);
  EXPECT_EQ(after_refusal.members.front().contribution, 0);
  EXPECT_THROW(store.remove_member(created->id, master.char_id, history), std::runtime_error);
  EXPECT_THROW(store.delete_guild(created->id, {history}), std::runtime_error);
  EXPECT_EQ(store.load(kWorldId).members.size(), 1U);
  {
    std::lock_guard lock(coordinator->mutex);
    EXPECT_EQ(coordinator->guild_memberships.at(master.char_id), created->id);
    auto &stored = coordinator->events.at({master.char_id, event.effect_id});
    ASSERT_TRUE(coordinator->can_mark_component(stored, QuestServiceComponent::guild));
    coordinator->mark_component_applied(stored, QuestServiceComponent::guild);
  }
  EXPECT_NO_THROW(store.delete_guild(created->id, {history}));
  EXPECT_TRUE(store.load(kWorldId).members.empty());
}

TEST(MemoryGuildQuestEffects, RejoiningTheSameGuildRejectsTheOldCapturedGeneration) {
  MemoryRepositories repositories;
  Character character;
  character.account_id = 12;
  character.name = "QuestGuildGenerationOwner";
  character = repositories.characters->create(character);
  const auto owner_epoch = repositories.character_state->acquire_monster_reward_owner_epoch();
  auto coordinator = repositories.database->quest_services;
  MemoryGuildStore store(coordinator);
  store.adopt_quest_service_owner_epoch(owner_epoch);
  GuildRecord guild;
  guild.world_id = kWorldId;
  guild.name = "QuestGenerationGuild";
  GuildMemberRecord master;
  master.char_id = 999;
  master.grade = GuildGrade::master;
  const auto created = store.create_guild(guild, master);
  ASSERT_TRUE(created);
  GuildMemberRecord member;
  member.char_id = character.id;
  member.guild_id = created->id;
  store.save_members({member});
  QuestServiceEffects effects;
  effects.expected_guild_id = created->id;
  effects.expected_guild_generation = 1;
  effects.guild_experience = 1;
  GuildHistory history;
  history.char_id = character.id;
  store.remove_member(created->id, character.id, history);
  store.save_members({member});

  const auto before = repositories.character_state->load_world_progress(12, character.id, 1);
  auto after = before;
  ++after.revision;
  ++after.gold;
  const auto result = repositories.character_state->commit_quest_mutation(
      12, character.id, 1, std::string(32, 'd'), before, after, ItemTrade{}, effects);
  EXPECT_EQ(result.status, QuestMutationStatus::conflict);
  EXPECT_EQ(repositories.character_state->load_world_progress(12, character.id, 1), before);
  std::lock_guard lock(coordinator->mutex);
  EXPECT_EQ(coordinator->guild_generations.at(character.id), 3);
  EXPECT_TRUE(coordinator->events.empty());
}

TEST(MemoryGuildQuestEffects, StaleOwnerCannotChangeGuildOrAdoptTheCoordinatorEpoch) {
  auto coordinator = std::make_shared<MemoryQuestServiceCoordinator>();
  coordinator->owner_epoch = 3;
  MemoryGuildStore store(coordinator);
  store.adopt_quest_service_owner_epoch(2);

  GuildRecord guild;
  guild.world_id = kWorldId;
  guild.name = "StaleOwner";
  GuildMemberRecord master;
  master.char_id = 1;
  master.grade = GuildGrade::master;
  EXPECT_THROW(store.create_guild(guild, master), std::runtime_error);
  EXPECT_EQ(coordinator->owner_epoch, 3);

  store.adopt_quest_service_owner_epoch(3);
  EXPECT_TRUE(store.create_guild(guild, master));
}

TEST(MemoryGuildQuestEffects, MembershipPublicationFailureLeavesEveryStoreUnchanged) {
  auto coordinator = std::make_shared<MemoryQuestServiceCoordinator>();
  coordinator->owner_epoch = 1;
  MemoryGuildStore store(coordinator);
  store.adopt_quest_service_owner_epoch(1);
  GuildRecord guild;
  guild.world_id = kWorldId;
  guild.name = "AtomicGuild";
  GuildMemberRecord master;
  master.char_id = 1;
  master.grade = GuildGrade::master;
  const auto fail_publish = [] { throw std::bad_alloc(); };
  store.set_before_membership_publish_for_test(fail_publish);
  EXPECT_THROW(store.create_guild(guild, master), std::bad_alloc);
  EXPECT_TRUE(store.load(kWorldId).guilds.empty());
  EXPECT_TRUE(coordinator->guild_memberships.empty());
  EXPECT_TRUE(coordinator->guild_generations.empty());

  store.set_before_membership_publish_for_test({});
  const auto created = store.create_guild(guild, master);
  ASSERT_TRUE(created);
  EXPECT_EQ(created->id, 1);
  GuildMemberRecord member;
  member.guild_id = created->id;
  member.char_id = 2;
  store.set_before_membership_publish_for_test(fail_publish);
  EXPECT_THROW(store.save_members({member}), std::bad_alloc);
  EXPECT_EQ(store.load(kWorldId).members.size(), 1U);
  EXPECT_FALSE(coordinator->guild_memberships.contains(member.char_id));
  EXPECT_FALSE(coordinator->guild_generations.contains(member.char_id));

  store.set_before_membership_publish_for_test({});
  store.save_members({member});
  const auto memberships_before = coordinator->guild_memberships;
  const auto generations_before = coordinator->guild_generations;
  GuildHistory history;
  history.char_id = member.char_id;
  history.last_leave_epoch_ticks = 123;
  store.set_before_membership_publish_for_test(fail_publish);
  EXPECT_THROW(store.remove_member(created->id, member.char_id, history), std::bad_alloc);
  EXPECT_EQ(store.load(kWorldId).members.size(), 2U);
  EXPECT_TRUE(store.load(kWorldId).histories.empty());
  EXPECT_EQ(coordinator->guild_memberships, memberships_before);
  EXPECT_EQ(coordinator->guild_generations, generations_before);
  EXPECT_THROW(store.delete_guild(created->id, {history}), std::bad_alloc);
  EXPECT_EQ(store.load(kWorldId).guilds.size(), 1U);
  EXPECT_EQ(store.load(kWorldId).members.size(), 2U);
  EXPECT_TRUE(store.load(kWorldId).histories.empty());
  EXPECT_EQ(coordinator->guild_memberships, memberships_before);
  EXPECT_EQ(coordinator->guild_generations, generations_before);

  store.set_before_membership_publish_for_test({});
  EXPECT_NO_THROW(store.delete_guild(created->id, {history}));
  EXPECT_TRUE(store.load(kWorldId).guilds.empty());
  EXPECT_TRUE(coordinator->guild_memberships.empty());
}

TEST(MemoryGuildQuestEffects, ConflictingBatchCannotMoveNewMemberBetweenGuilds) {
  auto coordinator = std::make_shared<MemoryQuestServiceCoordinator>();
  coordinator->owner_epoch = 1;
  MemoryGuildStore store(coordinator);
  store.adopt_quest_service_owner_epoch(1);
  GuildRecord first;
  first.world_id = kWorldId;
  first.name = "FirstGuild";
  GuildRecord second = first;
  second.name = "SecondGuild";
  GuildMemberRecord first_master;
  first_master.char_id = 1;
  first_master.grade = GuildGrade::master;
  GuildMemberRecord second_master = first_master;
  second_master.char_id = 2;
  const auto first_guild = store.create_guild(first, first_master);
  const auto second_guild = store.create_guild(second, second_master);
  ASSERT_TRUE(first_guild);
  ASSERT_TRUE(second_guild);
  const auto memberships_before = coordinator->guild_memberships;
  const auto generations_before = coordinator->guild_generations;
  GuildMemberRecord member;
  member.char_id = 3;
  member.guild_id = first_guild->id;
  auto moved = member;
  moved.guild_id = second_guild->id;

  EXPECT_THROW(store.save_members({member, moved}), std::runtime_error);
  EXPECT_EQ(store.load(kWorldId).members.size(), 2U);
  EXPECT_EQ(coordinator->guild_memberships, memberships_before);
  EXPECT_EQ(coordinator->guild_generations, generations_before);
}

TEST(MemoryGuildQuestEffects, SettlementAppliesProgressionOnceAndChecksEventOrder) {
  auto coordinator = std::make_shared<MemoryQuestServiceCoordinator>();
  coordinator->owner_epoch = 1;
  MemoryGuildStore store(coordinator);
  store.adopt_quest_service_owner_epoch(1);
  GuildRecord guild;
  guild.world_id = kWorldId;
  guild.name = "OrderedRewards";
  GuildMemberRecord master;
  master.char_id = 1;
  master.account_id = 2;
  master.grade = GuildGrade::master;
  const auto created = store.create_guild(guild, master);
  ASSERT_TRUE(created);

  QuestServiceEffectEvent first;
  first.enqueue_sequence = 1;
  first.character_id = master.char_id;
  first.account_id = master.account_id;
  first.world_id = kWorldId;
  first.effect_id = std::string(32, 'a');
  first.mutation_fingerprint = std::string(64, 'a');
  first.effects.expected_guild_id = created->id;
  first.effects.expected_guild_generation = 1;
  first.effects.guild_experience = 10;
  first.company_applied = true;
  auto second = first;
  second.enqueue_sequence = 2;
  second.effect_id = std::string(32, 'b');
  {
    std::lock_guard lock(coordinator->mutex);
    ASSERT_TRUE(coordinator->record_event(first));
    ASSERT_TRUE(coordinator->record_event(second));
  }

  EXPECT_EQ(store.read_quest_effect(second).status, QuestEffectSettlementStatus::retry);
  const auto current = store.read_quest_effect(first);
  ASSERT_EQ(current.status, QuestEffectSettlementStatus::pending);
  auto next_guild = *current.guild;
  auto next_member = *current.member;
  next_guild.experience += 10;
  next_member.contribution += 10;
  const auto applied = store.settle_quest_effect(first, *current.guild, *current.member,
                                                 next_guild, next_member);
  EXPECT_EQ(applied.status, QuestEffectSettlementStatus::applied);
  const auto replay = store.settle_quest_effect(first, *current.guild, *current.member,
                                                next_guild, next_member);
  EXPECT_EQ(replay.status, QuestEffectSettlementStatus::already_applied);
  ASSERT_TRUE(replay.guild);
  ASSERT_TRUE(replay.member);
  EXPECT_EQ(replay.guild->experience, 10);
  EXPECT_EQ(replay.member->contribution, 10);
  EXPECT_EQ(store.read_quest_effect(second).status, QuestEffectSettlementStatus::pending);
  auto altered = first;
  altered.effects.guild_experience = 11;
  EXPECT_EQ(store.read_quest_effect(altered).status,
            QuestEffectSettlementStatus::identity_mismatch);
}

class GuildLostAcknowledgementStore final : public GuildStore {
public:
  explicit GuildLostAcknowledgementStore(std::shared_ptr<GuildStore> inner,
                                         std::shared_ptr<GuildStore> settlement = {})
      : inner_(std::move(inner)), settlement_(std::move(settlement)) {}

  GuildSnapshot load(int world_id) override {
    return inner_->load(world_id);
  }
  std::optional<GuildRecord> create_guild(GuildRecord guild,
                                          const GuildMemberRecord &master) override {
    return inner_->create_guild(std::move(guild), master);
  }
  void update_guild(const GuildRecord &guild) override {
    auto &store = settlement_ ? settlement_ : inner_;
    store->update_guild(guild);
    lose_acknowledgement_if_armed();
  }
  void save_members(const std::vector<GuildMemberRecord> &members) override {
    inner_->save_members(members);
  }
  void remove_member(std::int64_t guild_id, std::int64_t character_id,
                     const GuildHistory &history) override {
    inner_->remove_member(guild_id, character_id, history);
  }
  void delete_guild(std::int64_t guild_id, const std::vector<GuildHistory> &histories) override {
    inner_->delete_guild(guild_id, histories);
  }
  void save_history(const GuildHistory &history) override {
    inner_->save_history(history);
  }
  GuildQuestEffectResult read_quest_effect(const QuestServiceEffectEvent &event) override {
    if (fail_read) {
      throw std::runtime_error("Injected fenced read failure");
    }
    return inner_->read_quest_effect(event);
  }
  GuildProgressionSnapshot read_progression(
      int world_id, std::int64_t guild_id,
      const std::optional<std::int64_t> &character_id) override {
    if (fail_read) {
      throw std::runtime_error("Injected fenced read failure");
    }
    return inner_->read_progression(world_id, guild_id, character_id);
  }
  void commit_progression(const GuildRecord &before_guild,
                          const GuildMemberRecord &before_member,
                          const GuildRecord &after_guild,
                          const GuildMemberRecord &after_member) override {
    auto &store = settlement_ ? settlement_ : inner_;
    store->commit_progression(before_guild, before_member, after_guild, after_member);
    lose_acknowledgement_if_armed();
  }
  GuildQuestEffectResult settle_quest_effect(const QuestServiceEffectEvent &event,
                                             const GuildRecord &before_guild,
                                             const GuildMemberRecord &before_member,
                                             const GuildRecord &after_guild,
                                             const GuildMemberRecord &after_member,
                                             std::int64_t before_remainder,
                                             std::int64_t after_remainder) override {
    auto &store = settlement_ ? settlement_ : inner_;
    auto result = store->settle_quest_effect(event, before_guild, before_member,
                                             after_guild, after_member, before_remainder,
                                             after_remainder);
    lose_acknowledgement_if_armed();
    return result;
  }

  bool lose_next_ack{};
  bool fail_read{};

  void use_direct_store() {
    settlement_.reset();
  }

private:
  void lose_acknowledgement_if_armed() {
    if (lose_next_ack) {
      lose_next_ack = false;
      throw std::runtime_error("Injected lost COMMIT acknowledgement");
    }
  }

  std::shared_ptr<GuildStore> inner_;
  std::shared_ptr<GuildStore> settlement_;
};

TEST(MemoryGuildQuestEffects, LostCommitAcknowledgementQuarantinesAndReconcilesCache) {
  auto coordinator = std::make_shared<MemoryQuestServiceCoordinator>();
  coordinator->owner_epoch = 1;
  auto inner = std::make_shared<MemoryGuildStore>(coordinator);
  inner->adopt_quest_service_owner_epoch(1);
  auto store = std::make_shared<GuildLostAcknowledgementStore>(inner);
  GuildRecord guild;
  guild.world_id = kWorldId;
  guild.name = "LostAckGuild";
  GuildMemberRecord master;
  master.char_id = 1;
  master.account_id = 2;
  master.grade = GuildGrade::master;
  const auto created = store->create_guild(guild, master);
  ASSERT_TRUE(created);

  QuestServiceEffectEvent event;
  event.enqueue_sequence = 1;
  event.character_id = master.char_id;
  event.account_id = master.account_id;
  event.world_id = kWorldId;
  event.effect_id = std::string(32, 'a');
  event.mutation_fingerprint = std::string(64, 'b');
  event.effects.expected_guild_id = created->id;
  event.effects.expected_guild_generation = 1;
  event.effects.guild_experience = 10;
  event.company_applied = true;
  {
    std::lock_guard lock(coordinator->mutex);
    ASSERT_TRUE(coordinator->record_event(event));
  }
  GuildRules rules;
  rules.levels = {{1, 1000, 4}, {2, 2000, 6}};
  GuildService service(store, rules, kWorldId);
  std::vector<GuildEvent> notifications;
  service.subscribe([&](const std::vector<GuildEvent> &events) {
    notifications.insert(notifications.end(), events.begin(), events.end());
  });
  store->lose_next_ack = true;
  EXPECT_EQ(service.settle_quest_effect(event), QuestEffectSettlementStatus::retry);
  EXPECT_THROW(service.gm_add_experience(created->id, 1), std::runtime_error);
  store->fail_read = true;
  const auto failed_read = service.reconcile_uncertain_quest_effect();
  ASSERT_TRUE(failed_read);
  EXPECT_EQ(failed_read->status, QuestEffectSettlementStatus::retry);
  EXPECT_THROW(service.gm_add_experience(created->id, 1), std::runtime_error);
  store->fail_read = false;
  const auto reconciled = service.reconcile_uncertain_quest_effect();
  ASSERT_TRUE(reconciled);
  EXPECT_EQ(reconciled->status, QuestEffectSettlementStatus::already_applied);
  EXPECT_EQ(service.members(created->id).front().contribution, 10);
  bool fail_notification = true;
  service.subscribe([&](const std::vector<GuildEvent> &) {
    if (fail_notification) {
      throw std::runtime_error("Injected notification failure");
    }
  });
  EXPECT_THROW(service.resynchronize_quest_effect(master.char_id), std::runtime_error);
  fail_notification = false;
  service.resynchronize_quest_effect(master.char_id);
  ASSERT_EQ(notifications.size(), 4U);
  EXPECT_TRUE(std::holds_alternative<GuildMemberUpdated>(notifications[2]));
  EXPECT_TRUE(std::holds_alternative<GuildExperienceChanged>(notifications[3]));
  EXPECT_EQ(std::get<GuildExperienceChanged>(notifications[3]).experience, 10);
  EXPECT_EQ(service.settle_quest_effect(event), QuestEffectSettlementStatus::already_applied);
  EXPECT_EQ(service.members(created->id).front().contribution, 10);
}

TEST(MemoryGuildQuestEffects, ActivityAndQuestAwardsShareOneDurableRemainder) {
  auto coordinator = std::make_shared<MemoryQuestServiceCoordinator>();
  coordinator->owner_epoch = 1;
  auto store = std::make_shared<MemoryGuildStore>(coordinator);
  store->adopt_quest_service_owner_epoch(1);
  GuildRecord guild;
  guild.world_id = kWorldId;
  guild.name = "ActivityGuild";
  GuildMemberRecord master;
  master.char_id = 1;
  master.account_id = 2;
  master.grade = GuildGrade::master;
  const auto created = store->create_guild(guild, master);
  ASSERT_TRUE(created);
  GuildRules rules;
  rules.levels = {{1, 1000, 4}, {2, 2000, 6}};
  GuildService service(store, rules, kWorldId);

  QuestServiceEffectEvent first;
  first.character_id = master.char_id;
  first.account_id = master.account_id;
  first.world_id = kWorldId;
  first.effect_id = std::string(32, 'a');
  first.mutation_fingerprint = std::string(64, 'a');
  first.effects.expected_guild_id = created->id;
  first.effects.expected_guild_generation = 1;
  first.effects.guild_experience = 3;
  first.effects.activity_experience = 7;
  first.effects.activity_guild_rate = 10;
  first.company_applied = true;
  {
    std::lock_guard lock(coordinator->mutex);
    ASSERT_TRUE(coordinator->record_event(first));
    first = coordinator->events.at({first.character_id, first.effect_id});
  }
  EXPECT_EQ(service.settle_quest_effect(first), QuestEffectSettlementStatus::applied);
  EXPECT_EQ(service.members(created->id).front().contribution, 3);
  EXPECT_EQ(store->read_quest_effect(first).activity_remainder, 7);

  auto second = first;
  second.effect_id = std::string(32, 'b');
  second.effects.guild_experience = 0;
  second.effects.activity_experience = 8;
  {
    std::lock_guard lock(coordinator->mutex);
    ASSERT_TRUE(coordinator->record_event(second));
    second = coordinator->events.at({second.character_id, second.effect_id});
  }
  EXPECT_EQ(service.settle_quest_effect(second), QuestEffectSettlementStatus::applied);
  EXPECT_EQ(service.members(created->id).front().contribution, 4);
  EXPECT_EQ(store->read_quest_effect(second).activity_remainder, 5);
  EXPECT_EQ(service.settle_quest_effect(second), QuestEffectSettlementStatus::already_applied);
  EXPECT_EQ(service.members(created->id).front().contribution, 4);

  auto maximum = second;
  maximum.effect_id = std::string(32, 'c');
  maximum.effects.guild_experience = 3;
  maximum.effects.activity_experience = std::numeric_limits<std::int64_t>::max();
  maximum.effects.activity_guild_rate = 1;
  {
    std::lock_guard lock(coordinator->mutex);
    ASSERT_TRUE(coordinator->record_event(maximum));
    maximum = coordinator->events.at({maximum.character_id, maximum.effect_id});
  }
  EXPECT_EQ(service.settle_quest_effect(maximum), QuestEffectSettlementStatus::applied);
  EXPECT_EQ(service.members(created->id).front().contribution,
            std::numeric_limits<std::int32_t>::max());
  EXPECT_EQ(store->read_quest_effect(maximum).activity_remainder, 0);
  EXPECT_EQ(store->read_quest_effect(second).activity_remainder, 5);

  GuildHistory history;
  history.char_id = master.char_id;
  store->remove_member(created->id, master.char_id, history);
  master.guild_id = created->id;
  store->save_members({master});
  GuildService rejoined(store, rules, kWorldId);
  auto after_rejoin = second;
  after_rejoin.effect_id = std::string(32, 'd');
  after_rejoin.effects.expected_guild_generation = 3;
  after_rejoin.effects.activity_experience = 5;
  {
    std::lock_guard lock(coordinator->mutex);
    ASSERT_TRUE(coordinator->record_event(after_rejoin));
    after_rejoin = coordinator->events.at({after_rejoin.character_id, after_rejoin.effect_id});
  }
  EXPECT_EQ(rejoined.settle_quest_effect(after_rejoin), QuestEffectSettlementStatus::applied);
  EXPECT_EQ(rejoined.members(created->id).front().contribution, 0);
  EXPECT_EQ(store->read_quest_effect(after_rejoin).activity_remainder, 5);
}

// Deterministic guild clock: wall ticks and the steady clock advance together.
struct FakeGuildClock {
  std::int64_t epoch_ticks = 1'700'000'000LL * 10'000'000LL;
  std::chrono::steady_clock::time_point steady = std::chrono::steady_clock::time_point{};

  GuildClock clock() {
    return GuildClock{[this] { return epoch_ticks; }, [this] { return steady; }};
  }

  void advance_hours(int hours) {
    epoch_ticks += hours * kTicksPerHour;
    steady += std::chrono::hours(hours);
  }

  void advance(std::chrono::milliseconds elapsed) {
    epoch_ticks += elapsed.count() * 10'000LL;
    steady += elapsed;
  }
};

// Every test actor belongs to Antiqus at GuildCreatableCompanyPosition (3), so it may found a
// guild and join any Antiqus guild; company tests override these fields.
GuildActor actor(std::int64_t char_id, const std::string &name, int level = 10) {
  GuildActor result;
  result.world_id = kWorldId;
  result.char_id = char_id;
  result.name = name;
  result.level = level;
  result.char_class = 2;
  result.company_type = kAntiqus;
  result.company_position = 3;
  return result;
}

GuildCreateRequest create_request(const std::string &name, int join_type = 1,
                                  int join_level = 0) {
  GuildCreateRequest request;
  request.name = name;
  request.description = "Open to all";
  request.join_type = join_type;
  request.join_level = join_level;
  return request;
}

class GuildServiceTest : public testing::Test {
protected:
  GuildServiceTest() {
    rules_.levels = {{1, 1000, 4}, {2, 2000, 6}};
    rebuild();
  }

  void rebuild() {
    service_ = std::make_unique<GuildService>(store_, rules_, kWorldId, clock_.clock());
    service_->subscribe([this](const std::vector<GuildEvent> &events) {
      events_.insert(events_.end(), events.begin(), events.end());
    });
  }

  std::int64_t create_guild(const GuildActor &master, const std::string &name) {
    const auto result = service_->create(master, create_request(name));
    if (result.error != x5::ec::Success) {
      throw std::runtime_error("Cannot create test guild");
    }
    return result.guild.id;
  }

  template <typename Event> std::vector<Event> events_of() const {
    std::vector<Event> result;
    for (const auto &event : events_) {
      if (const auto *typed = std::get_if<Event>(&event)) {
        result.push_back(*typed);
      }
    }
    return result;
  }

  FakeGuildClock clock_;
  GuildRules rules_;
  std::shared_ptr<MemoryGuildStore> store_ = std::make_shared<MemoryGuildStore>();
  std::unique_ptr<GuildService> service_;
  std::vector<GuildEvent> events_;
};

TEST(GuildText, NameAndDescriptionFollowTheExportedCharacterClasses) {
  EXPECT_TRUE(valid_guild_name_text("Chronicles_Guard-1.x"));
  EXPECT_TRUE(valid_guild_name_text("\xEA\xB8\xB8\xEB\x93\x9C")); // Hangul syllables
  EXPECT_FALSE(valid_guild_name_text("Two Words"));
  EXPECT_FALSE(valid_guild_name_text("Bad!"));
  EXPECT_FALSE(valid_guild_name_text(""));
  EXPECT_FALSE(valid_guild_name_text("\xC3\x28")); // malformed UTF-8

  EXPECT_TRUE(valid_guild_description_text(""));
  EXPECT_TRUE(valid_guild_description_text("We sail at dawn\nAll welcome"));
  EXPECT_FALSE(valid_guild_description_text("No punctuation, please"));

  EXPECT_EQ(utf8_code_points("\xEA\xB8\xB8\xEB\x93\x9C"), 2u);
  EXPECT_EQ(utf8_code_points("\xED\xA0\x80"), std::nullopt); // surrogate
  EXPECT_EQ(utf8_code_points("\xC0\xAF"), std::nullopt);     // overlong
}

TEST_F(GuildServiceTest, CreateMakesTheCreatorMasterAndRejectsInvalidOrDuplicateNames) {
  const auto master = actor(1, "Founder");
  const auto created = service_->create(master, create_request("Seafarers"));
  ASSERT_EQ(created.error, x5::ec::Success);
  EXPECT_GT(created.guild.id, 0);
  EXPECT_EQ(created.guild.level, 1);
  EXPECT_EQ(created.owner.grade, GuildGrade::master);
  EXPECT_EQ(created.owner.name, "Founder");

  const auto joined = events_of<GuildMemberJoined>();
  ASSERT_EQ(joined.size(), 1u);
  EXPECT_EQ(joined.front().member.char_id, 1);
  EXPECT_TRUE(joined.front().recipients.empty());

  EXPECT_EQ(service_->create(master, create_request("Second")).error,
            x5::ec::Guild_AlreadyJoined);

  const auto other = actor(2, "Rival");
  EXPECT_EQ(service_->create(other, create_request("SEAFARERS")).error,
            x5::ec::Guild_CreateNameAlreadyUsed);
  EXPECT_EQ(service_->create(other, create_request("A")).error,
            x5::ec::Guild_CreateNameLengthInvalid);
  EXPECT_EQ(service_->create(other, create_request(std::string(21, 'a'))).error,
            x5::ec::Guild_CreateNameLengthInvalid);
  EXPECT_EQ(service_->create(other, create_request("Bad Name")).error,
            x5::ec::Guild_CreateNameInvalid);
  EXPECT_EQ(service_->create(other, create_request("Valid", 7)).error,
            x5::ec::Guild_InvalidReqParam);

  auto bad_description = create_request("Valid");
  bad_description.description = "Hello, world";
  EXPECT_EQ(service_->create(other, bad_description).error, x5::ec::Guild_CreateDescInvalid);
  bad_description.description = std::string(201, 'a');
  EXPECT_EQ(service_->create(other, bad_description).error,
            x5::ec::Guild_CreateDescLengthInvalid);

  auto wrong_world = other;
  wrong_world.world_id = 2;
  EXPECT_EQ(service_->create(wrong_world, create_request("Valid")).error,
            x5::ec::Guild_InvalidChar);
}

TEST_F(GuildServiceTest, JoinHonorsJoinTypeLevelCapacityAndRejoinCooldown) {
  const auto guild_id = create_guild(actor(1, "Founder"), "Seafarers");
  const auto closed = service_->create(actor(9, "Hermit"), create_request("Closed", 0));
  ASSERT_EQ(closed.error, x5::ec::Success);

  EXPECT_EQ(service_->join(actor(2, "Joiner"), closed.guild.id).error,
            x5::ec::Guild_NotJoinableGuild);
  EXPECT_EQ(service_->join(actor(2, "Joiner"), 999).error, x5::ec::Guild_NotExist);

  GuildManageRequest manage;
  manage.guild_id = guild_id;
  manage.join_type = 1;
  manage.join_level = 20;
  ASSERT_EQ(service_->manage(actor(1, "Founder"), manage).error, x5::ec::Success);
  EXPECT_EQ(service_->join(actor(2, "Joiner", 19), guild_id).error,
            x5::ec::Guild_InvalidCharLevel);

  const auto joined = service_->join(actor(2, "Joiner", 20), guild_id);
  ASSERT_EQ(joined.error, x5::ec::Success);
  EXPECT_EQ(joined.member.grade, GuildGrade::member);
  ASSERT_EQ(joined.members.size(), 2u);
  EXPECT_EQ(joined.members.front().grade, GuildGrade::master);
  const auto joined_events = events_of<GuildMemberJoined>();
  EXPECT_EQ(joined_events.back().recipients, std::vector<std::int64_t>{1});

  EXPECT_EQ(service_->join(actor(2, "Joiner", 20), guild_id).error, x5::ec::Guild_AlreadyJoined);
  ASSERT_EQ(service_->join(actor(3, "Third", 20), guild_id).error, x5::ec::Success);
  ASSERT_EQ(service_->join(actor(4, "Fourth", 20), guild_id).error, x5::ec::Success);
  EXPECT_EQ(service_->join(actor(5, "Fifth", 20), guild_id).error,
            x5::ec::Guild_MemberLimitExceeded);

  const auto left = service_->leave(actor(4, "Fourth"), guild_id);
  ASSERT_EQ(left.error, x5::ec::Success);
  EXPECT_EQ(left.left_epoch_ticks, clock_.epoch_ticks);
  EXPECT_EQ(service_->join(actor(4, "Fourth", 20), guild_id).error,
            x5::ec::Guild_InvalidTimeJoin);

  clock_.advance_hours(24);
  EXPECT_EQ(service_->join(actor(4, "Fourth", 20), guild_id).error, x5::ec::Success);
}

TEST_F(GuildServiceTest, OnlyTheMasterChangesGradesAndManagersKickOnlyMembers) {
  const auto guild_id = create_guild(actor(1, "Founder"), "Seafarers");
  for (std::int64_t char_id = 2; char_id <= 4; ++char_id) {
    ASSERT_EQ(service_->join(actor(char_id, "Member" + std::to_string(char_id)), guild_id).error,
              x5::ec::Success);
  }

  const auto sub_master = static_cast<int>(GuildGrade::sub_master);
  const auto member = static_cast<int>(GuildGrade::member);
  const auto kick = static_cast<int>(GuildGrade::none);
  const auto master = static_cast<int>(GuildGrade::master);

  EXPECT_EQ(service_->manage_member(actor(2, "Member2"), guild_id, 3, sub_master).error,
            x5::ec::Guild_NoPermission);
  ASSERT_EQ(service_->manage_member(actor(1, "Founder"), guild_id, 2, sub_master).error,
            x5::ec::Success);
  EXPECT_EQ(service_->manage_member(actor(1, "Founder"), guild_id, 2, sub_master).error,
            x5::ec::Guild_MemberGradeAlreadyChanged);
  EXPECT_EQ(service_->manage_member(actor(2, "Member2"), guild_id, 1, kick).error,
            x5::ec::Guild_InvalidGrade);
  EXPECT_EQ(service_->manage_member(actor(1, "Founder"), guild_id, 1, member).error,
            x5::ec::Guild_InvalidReqParam);
  EXPECT_EQ(service_->manage_member(actor(1, "Founder"), guild_id, 77, member).error,
            x5::ec::Guild_NotExistSelectedGuildMember);
  EXPECT_EQ(service_->manage_member(actor(1, "Founder"), guild_id, 3, 9).error,
            x5::ec::Guild_InvalidGrade);

  const auto grade_events = events_of<GuildMemberGradeChanged>();
  ASSERT_EQ(grade_events.size(), 1u);
  EXPECT_EQ(grade_events.front().char_id, 2);
  EXPECT_EQ(grade_events.front().recipients, (std::vector<std::int64_t>{2, 3, 4}));

  // A SubMaster may kick a Member but not another SubMaster.
  ASSERT_EQ(service_->manage_member(actor(1, "Founder"), guild_id, 3, sub_master).error,
            x5::ec::Success);
  EXPECT_EQ(service_->manage_member(actor(2, "Member2"), guild_id, 3, kick).error,
            x5::ec::Guild_NoPermission);
  const auto kicked = service_->manage_member(actor(2, "Member2"), guild_id, 4, kick);
  ASSERT_EQ(kicked.error, x5::ec::Success);
  EXPECT_EQ(kicked.grade, GuildGrade::none);
  const auto left = events_of<GuildMemberLeft>();
  ASSERT_EQ(left.size(), 1u);
  EXPECT_TRUE(left.front().kicked);
  EXPECT_EQ(left.front().char_id, 4);
  EXPECT_EQ(left.front().recipients, (std::vector<std::int64_t>{1, 3}));
  EXPECT_EQ(service_->status(4).guild_id, 0);
  EXPECT_EQ(service_->status(4).last_leave_epoch_ticks, clock_.epoch_ticks);

  // Transferring mastership demotes the former master to Member.
  ASSERT_EQ(service_->manage_member(actor(1, "Founder"), guild_id, 2, master).error,
            x5::ec::Success);
  EXPECT_EQ(service_->status(2).grade, GuildGrade::master);
  EXPECT_EQ(service_->status(1).grade, GuildGrade::member);
  const auto transfers = events_of<GuildMemberGradeChanged>();
  ASSERT_GE(transfers.size(), 2u);
  const auto &demotion = transfers.back();
  EXPECT_EQ(demotion.char_id, 1);
  EXPECT_EQ(demotion.grade, GuildGrade::member);
  EXPECT_EQ(demotion.recipients, (std::vector<std::int64_t>{1, 2, 3}));
}

TEST_F(GuildServiceTest, SubMasterCountIsLimitedByTheRules) {
  rules_.levels = {{1, 1000, 10}};
  rules_.max_sub_masters = 2;
  rebuild();
  const auto guild_id = create_guild(actor(1, "Founder"), "Seafarers");
  for (std::int64_t char_id = 2; char_id <= 4; ++char_id) {
    ASSERT_EQ(service_->join(actor(char_id, "Member" + std::to_string(char_id)), guild_id).error,
              x5::ec::Success);
  }

  const auto sub_master = static_cast<int>(GuildGrade::sub_master);
  ASSERT_EQ(service_->manage_member(actor(1, "Founder"), guild_id, 2, sub_master).error,
            x5::ec::Success);
  ASSERT_EQ(service_->manage_member(actor(1, "Founder"), guild_id, 3, sub_master).error,
            x5::ec::Success);
  EXPECT_EQ(service_->manage_member(actor(1, "Founder"), guild_id, 4, sub_master).error,
            x5::ec::Guild_ReachedSubmasterLimit);
}

TEST_F(GuildServiceTest, MastersDisbandAfterTheMinimumAgeAndThenWaitToCreateAgain) {
  const auto guild_id = create_guild(actor(1, "Founder"), "Seafarers");
  ASSERT_EQ(service_->join(actor(2, "Member"), guild_id).error, x5::ec::Success);

  EXPECT_EQ(service_->leave(actor(1, "Founder"), guild_id).error, x5::ec::Guild_InvalidGrade);
  EXPECT_EQ(service_->disband(actor(2, "Member"), guild_id).error, x5::ec::Guild_NoPermission);
  EXPECT_EQ(service_->disband(actor(1, "Founder"), guild_id).error,
            x5::ec::Guild_DisbandCooltime);

  clock_.advance_hours(168);
  const auto disbanded = service_->disband(actor(1, "Founder"), guild_id);
  ASSERT_EQ(disbanded.error, x5::ec::Success);
  EXPECT_EQ(disbanded.disbanded_epoch_ticks, clock_.epoch_ticks);
  EXPECT_EQ(service_->status(2).guild_id, 0);
  // Members of a disbanded guild are not penalized like a voluntary leave.
  EXPECT_EQ(service_->status(2).last_leave_epoch_ticks, 0);

  const auto disband_events = events_of<GuildDisbanded>();
  ASSERT_EQ(disband_events.size(), 1u);
  EXPECT_EQ(disband_events.front().former_members, (std::vector<std::int64_t>{1, 2}));
  EXPECT_EQ(disband_events.front().recipients, std::vector<std::int64_t>{2});

  // The name is free again, but the former master waits GuildRecreatableTimeHours.
  EXPECT_EQ(service_->create(actor(1, "Founder"), create_request("Seafarers")).error,
            x5::ec::Guild_InvalidTimeCreate);
  EXPECT_EQ(service_->create(actor(3, "Other"), create_request("Seafarers")).error,
            x5::ec::Success);
  clock_.advance_hours(168);
  EXPECT_EQ(service_->create(actor(1, "Founder"), create_request("Harbor")).error,
            x5::ec::Success);
}

TEST_F(GuildServiceTest, ManagersUpdateInformationAndTheNoticeRecordsItsAuthor) {
  const auto guild_id = create_guild(actor(1, "Founder"), "Seafarers");
  ASSERT_EQ(service_->join(actor(2, "Member"), guild_id).error, x5::ec::Success);

  GuildManageRequest manage;
  manage.guild_id = guild_id;
  manage.description = "Traders of the east";
  manage.notice = "Meet at the harbor";
  manage.join_type = 0;
  EXPECT_EQ(service_->manage(actor(2, "Member"), manage).error, x5::ec::Guild_NoPermission);

  const auto updated = service_->manage(actor(1, "Founder"), manage);
  ASSERT_EQ(updated.error, x5::ec::Success);
  EXPECT_EQ(updated.guild.notice, "Meet at the harbor");
  EXPECT_EQ(updated.guild.notice_char_id, 1);
  EXPECT_EQ(updated.guild.notice_epoch_ticks, clock_.epoch_ticks);
  EXPECT_EQ(updated.guild.join_type, GuildJoinType::none);

  const auto info = events_of<GuildInfoChanged>();
  ASSERT_EQ(info.size(), 1u);
  EXPECT_EQ(info.front().recipients, std::vector<std::int64_t>{2});

  manage.notice = "Bad notice!";
  EXPECT_EQ(service_->manage(actor(1, "Founder"), manage).error,
            x5::ec::Guild_CreateNoticeInvalid);
  manage.notice = "";
  manage.join_level = -1;
  EXPECT_EQ(service_->manage(actor(1, "Founder"), manage).error, x5::ec::Guild_InvalidReqParam);
}

TEST_F(GuildServiceTest, InvitationsRequireAManagerExpireAndReportRejections) {
  const auto guild_id = create_guild(actor(1, "Founder"), "Seafarers");
  ASSERT_EQ(service_->join(actor(2, "Member"), guild_id).error, x5::ec::Success);
  const auto target = actor(5, "Target");

  EXPECT_EQ(service_->invite(actor(2, "Member"), guild_id, target).error,
            x5::ec::Guild_NotLeaderOrSubLeader);
  EXPECT_EQ(service_->invite(actor(1, "Founder"), guild_id, actor(2, "Member")).error,
            x5::ec::Guild_AlreadyGuildJoin);

  const auto invited = service_->invite(actor(1, "Founder"), guild_id, target);
  ASSERT_EQ(invited.error, x5::ec::Success);
  EXPECT_EQ(invited.target_name, "Target");
  const auto issued = events_of<GuildInviteIssued>();
  ASSERT_EQ(issued.size(), 1u);
  EXPECT_EQ(issued.front().target_char_id, 5);
  EXPECT_EQ(issued.front().inviter_char_id, 1);
  EXPECT_EQ(issued.front().guild_name, "Seafarers");
  EXPECT_EQ(service_->invite(actor(1, "Founder"), guild_id, target).error,
            x5::ec::Guild_AlreadyInvited);

  EXPECT_EQ(service_->accept_invite(target, guild_id, 2), x5::ec::Guild_NotExistsInviter);
  EXPECT_EQ(service_->reject_invite(target, guild_id, 1), x5::ec::Success);
  const auto rejected = events_of<GuildInviteRejected>();
  ASSERT_EQ(rejected.size(), 1u);
  EXPECT_EQ(rejected.front().inviter_char_id, 1);
  EXPECT_EQ(rejected.front().target_name, "Target");

  ASSERT_EQ(service_->invite(actor(1, "Founder"), guild_id, target).error, x5::ec::Success);
  clock_.advance(rules_.invite_timeout);
  EXPECT_EQ(service_->accept_invite(target, guild_id, 1), x5::ec::Guild_NotExistsInviter);

  ASSERT_EQ(service_->invite(actor(1, "Founder"), guild_id, target).error, x5::ec::Success);
  ASSERT_EQ(service_->accept_invite(target, guild_id, 1), x5::ec::Success);
  const auto joined = events_of<GuildMemberJoined>();
  ASSERT_FALSE(joined.empty());
  EXPECT_TRUE(joined.back().via_invitation);
  EXPECT_EQ(joined.back().member.char_id, 5);
  EXPECT_EQ(service_->status(5).guild_id, guild_id);
}

TEST_F(GuildServiceTest, StateSurvivesReloadAndMasterlessGuildsPromoteASuccessor) {
  const auto guild_id = create_guild(actor(1, "Founder"), "Seafarers");
  ASSERT_EQ(service_->join(actor(2, "Senior"), guild_id).error, x5::ec::Success);
  clock_.advance_hours(1);
  ASSERT_EQ(service_->join(actor(3, "Junior"), guild_id).error, x5::ec::Success);
  ASSERT_EQ(service_->leave(actor(3, "Junior"), guild_id).error, x5::ec::Success);

  rebuild();
  EXPECT_EQ(service_->status(1).grade, GuildGrade::master);
  EXPECT_EQ(service_->status(2).guild_name, "Seafarers");
  EXPECT_EQ(service_->status(3).guild_id, 0);
  EXPECT_EQ(service_->status(3).last_leave_epoch_ticks, clock_.epoch_ticks);
  EXPECT_EQ(service_->search(actor(9, "Viewer"), "seafarers").error, x5::ec::Success);

  // Deleting the master character cascades its membership row away in PostgreSQL; removing
  // the row from the store models the same state.
  store_->remove_member(guild_id, 1, GuildHistory{1, 0, 0});

  rebuild();
  EXPECT_EQ(service_->status(1).guild_id, 0);
  EXPECT_EQ(service_->status(2).grade, GuildGrade::master);
}

TEST_F(GuildServiceTest, JoinableListPagesOpenGuildsWithFreeSeats) {
  rules_.joinable_list_page_size = 2;
  rebuild();
  for (std::int64_t char_id = 1; char_id <= 3; ++char_id) {
    create_guild(actor(char_id, "Master" + std::to_string(char_id)),
                 "Guild" + std::to_string(char_id));
  }
  ASSERT_EQ(service_->create(actor(4, "Closed"), create_request("Closed", 0)).error,
            x5::ec::Success);

  const auto first = service_->joinable(actor(9, "Viewer"), 0);
  ASSERT_EQ(first.error, x5::ec::Success);
  ASSERT_EQ(first.guilds.size(), 2u);
  EXPECT_EQ(first.next_start_index, 2);
  EXPECT_EQ(first.guilds.front().master_name, "Master1");
  EXPECT_EQ(first.guilds.front().member_count, 1u);

  const auto second = service_->joinable(actor(9, "Viewer"), first.next_start_index);
  ASSERT_EQ(second.guilds.size(), 1u);
  EXPECT_EQ(second.next_start_index, 0);
  EXPECT_EQ(service_->joinable(actor(9, "Viewer"), -1).error, x5::ec::Guild_InvalidReqParam);
}

TEST_F(GuildServiceTest, RefreshPublishesChangedMemberDisplayFields) {
  const auto guild_id = create_guild(actor(1, "Founder", 10), "Seafarers");
  ASSERT_EQ(service_->join(actor(2, "Member", 10), guild_id).error, x5::ec::Success);

  service_->refresh_member(actor(2, "Member", 10));
  EXPECT_TRUE(events_of<GuildMemberUpdated>().empty());

  service_->refresh_member(actor(2, "Member", 12));
  const auto updates = events_of<GuildMemberUpdated>();
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_EQ(updates.front().member.level, 12);
  EXPECT_EQ(updates.front().recipients, std::vector<std::int64_t>{1});
  EXPECT_EQ(service_->guild_mates(2), (std::vector<std::int64_t>{1, 2}));
}

TEST_F(GuildServiceTest, CreationAndAdmissionFollowTheCompanyRules) {
  auto homeless = actor(1, "Founder");
  homeless.company_type = 0;
  homeless.company_position = 0;
  EXPECT_EQ(service_->create(homeless, create_request("Seafarers")).error,
            x5::ec::Guild_InvalidCharCompay);

  auto junior = actor(1, "Founder");
  junior.company_position = rules_.creatable_company_position - 1;
  EXPECT_EQ(service_->create(junior, create_request("Seafarers")).error,
            x5::ec::Guild_InvalidCompanyPosition);

  const auto created = service_->create(actor(1, "Founder"), create_request("Seafarers"));
  ASSERT_EQ(created.error, x5::ec::Success);
  EXPECT_EQ(created.guild.join_company_type, kAntiqus);
  const auto guild_id = created.guild.id;

  auto rival = actor(2, "Rival");
  rival.company_type = testing_support::kBlueSalt;
  EXPECT_EQ(service_->join(rival, guild_id).error, x5::ec::Guild_InvalidCompanyType);

  GuildManageRequest manage;
  manage.guild_id = guild_id;
  manage.join_type = 1;
  manage.join_company_position = 4;
  ASSERT_EQ(service_->manage(actor(1, "Founder"), manage).error, x5::ec::Success);
  EXPECT_EQ(service_->join(actor(3, "Recruit"), guild_id).error,
            x5::ec::Guild_InvalidCompanyPosition);

  auto veteran = actor(3, "Recruit");
  veteran.company_position = 4;
  EXPECT_EQ(service_->join(veteran, guild_id).error, x5::ec::Success);
  EXPECT_EQ(service_->members(guild_id).back().company_position, 4);
}

TEST_F(GuildServiceTest, RealmsWithoutCompaniesSkipTheCompanyRules) {
  rules_.companies_enabled = false;
  rebuild();

  auto founder = actor(1, "Founder");
  founder.company_type = 0;
  founder.company_position = 0;
  const auto created = service_->create(founder, create_request("Seafarers"));
  ASSERT_EQ(created.error, x5::ec::Success);

  auto joiner = actor(2, "Joiner");
  joiner.company_type = testing_support::kBlueSalt;
  joiner.company_position = 0;
  EXPECT_EQ(service_->join(joiner, created.guild.id).error, x5::ec::Success);
}

TEST_F(GuildServiceTest, ExperienceAdvancesLevelsRaisesTheMemberLimitAndStopsAtTheMaximum) {
  const auto guild_id = create_guild(actor(1, "Founder"), "Seafarers");
  EXPECT_FALSE(service_->award_experience(9, 100));
  EXPECT_FALSE(service_->award_experience(1, 0));

  ASSERT_TRUE(service_->award_experience(1, 999));
  EXPECT_EQ(service_->status(1).guild_level, 1);
  EXPECT_EQ(service_->status(1).max_members, 4u);

  ASSERT_TRUE(service_->award_experience(1, 1));
  const auto status = service_->status(1);
  EXPECT_EQ(status.guild_level, 2);
  EXPECT_EQ(status.max_members, 6u);
  EXPECT_EQ(service_->detail(actor(1, "Founder"), guild_id).guild.experience, 0);

  // Level 2 is the maximum of these rules, so further experience is not kept.
  ASSERT_TRUE(service_->award_experience(1, 5000));
  EXPECT_EQ(service_->detail(actor(1, "Founder"), guild_id).guild.experience, 0);
  EXPECT_EQ(service_->members(guild_id).front().contribution, 6000);

  const auto changes = events_of<GuildExperienceChanged>();
  ASSERT_EQ(changes.size(), 2u);
  EXPECT_EQ(changes.front().experience, 999);
  EXPECT_FALSE(changes.front().level_changed);
  EXPECT_TRUE(changes.back().level_changed);
  EXPECT_EQ(changes.back().level, 2);
  EXPECT_EQ(changes.back().recipients, std::vector<std::int64_t>{1});

  rebuild();
  EXPECT_EQ(service_->status(1).guild_level, 2);
  EXPECT_EQ(service_->members(guild_id).front().contribution, 6000);
}

TEST_F(GuildServiceTest, CharacterExperienceFeedsGuildExperienceWithARemainder) {
  rules_.character_experience_per_guild_experience = 10;
  rebuild();
  const auto guild_id = create_guild(actor(1, "Founder"), "Seafarers");

  EXPECT_FALSE(service_->record_character_experience(1, 9));
  EXPECT_TRUE(service_->record_character_experience(1, 16));
  EXPECT_EQ(service_->detail(actor(1, "Founder"), guild_id).guild.experience, 2);
  EXPECT_TRUE(service_->record_character_experience(1, 5));
  EXPECT_EQ(service_->detail(actor(1, "Founder"), guild_id).guild.experience, 3);
  EXPECT_FALSE(service_->record_character_experience(2, 1000));
  EXPECT_FALSE(service_->record_character_experience(1, -50));
}

TEST_F(GuildServiceTest, GameMasterCommandsBypassGradesButKeepInvariants) {
  const auto guild_id = create_guild(actor(1, "Founder"), "Seafarers");
  ASSERT_EQ(service_->join(actor(2, "Member"), guild_id).error, x5::ec::Success);

  EXPECT_EQ(service_->gm_add_experience(999, 10), x5::ec::Guild_NotExist);
  EXPECT_EQ(service_->gm_add_experience(guild_id, 1500), x5::ec::Success);
  EXPECT_EQ(service_->status(1).guild_level, 2);
  EXPECT_EQ(service_->gm_add_experience(guild_id, -100), x5::ec::Success);
  EXPECT_EQ(service_->status(1).guild_level, 2);

  EXPECT_EQ(service_->gm_set_level(guild_id, 3), x5::ec::Guild_InvalidLevel);
  EXPECT_EQ(service_->gm_set_level(guild_id, 1), x5::ec::Success);
  EXPECT_EQ(service_->status(1).guild_level, 1);

  EXPECT_EQ(service_->gm_set_master(guild_id, 77), x5::ec::Guild_NotExistSelectedGuildMember);
  EXPECT_EQ(service_->gm_set_master(guild_id, 2), x5::ec::Success);
  EXPECT_EQ(service_->status(2).grade, GuildGrade::master);
  EXPECT_EQ(service_->status(1).grade, GuildGrade::member);

  ASSERT_EQ(service_->leave(actor(1, "Founder"), guild_id).error, x5::ec::Success);
  EXPECT_EQ(service_->join(actor(1, "Founder"), guild_id).error, x5::ec::Guild_InvalidTimeJoin);
  EXPECT_EQ(service_->gm_reset_cooldowns(1), x5::ec::Success);
  EXPECT_EQ(service_->join(actor(1, "Founder"), guild_id).error, x5::ec::Success);

  // An operator disband needs no minimum age and starts no cooldowns.
  EXPECT_EQ(service_->gm_disband(guild_id), x5::ec::Success);
  EXPECT_EQ(service_->status(1).guild_id, 0);
  const auto disbanded = events_of<GuildDisbanded>();
  ASSERT_EQ(disbanded.size(), 1u);
  EXPECT_EQ(disbanded.front().recipients, (std::vector<std::int64_t>{1, 2}));
  EXPECT_EQ(service_->create(actor(2, "Member"), create_request("Harbor")).error,
            x5::ec::Success);
}

TEST_F(GuildServiceTest, GmProgressionPreparesNotificationBeforeTheStoreWrite) {
  const auto guild_id = create_guild(actor(1, "Founder"), "PreparedNotice");
  events_.clear();
  service_->set_before_progression_notification_for_test([] { throw std::bad_alloc(); });
  EXPECT_THROW(service_->gm_add_experience(guild_id, 10), std::bad_alloc);
  EXPECT_TRUE(events_.empty());
  EXPECT_EQ(service_->detail(actor(1, "Founder"), guild_id).guild.experience, 0);
  EXPECT_EQ(store_->load(kWorldId).guilds.front().experience, 0);

  service_->set_before_progression_notification_for_test({});
  ASSERT_EQ(service_->gm_add_experience(guild_id, 10), x5::ec::Success);
  const auto notifications = events_of<GuildExperienceChanged>();
  ASSERT_EQ(notifications.size(), 1U);
  EXPECT_EQ(notifications.front().experience, 10);
  EXPECT_EQ(store_->load(kWorldId).guilds.front().experience, 10);
}

// A failing store leaves the registry unchanged and reports the recovered failure code.
class FailingGuildStore final : public GuildStore {
public:
  GuildSnapshot load(int) override {
    return {};
  }

  std::optional<GuildRecord> create_guild(GuildRecord, const GuildMemberRecord &) override {
    throw std::runtime_error("database unavailable");
  }

  void update_guild(const GuildRecord &) override {
    throw std::runtime_error("database unavailable");
  }

  void save_members(const std::vector<GuildMemberRecord> &) override {
    throw std::runtime_error("database unavailable");
  }

  void remove_member(std::int64_t, std::int64_t, const GuildHistory &) override {
    throw std::runtime_error("database unavailable");
  }

  void delete_guild(std::int64_t, const std::vector<GuildHistory> &) override {
    throw std::runtime_error("database unavailable");
  }

  void save_history(const GuildHistory &) override {
    throw std::runtime_error("database unavailable");
  }
};

TEST(GuildServiceStorage, StoreFailuresDoNotChangeTheRegistry) {
  GuildService service(std::make_shared<FailingGuildStore>(), GuildRules{}, kWorldId);
  EXPECT_EQ(service.create(actor(1, "Founder"), create_request("Seafarers")).error,
            x5::ec::Guild_FailedCreate);
  EXPECT_EQ(service.status(1).guild_id, 0);
  EXPECT_EQ(service.search(actor(1, "Founder"), "Seafarers").error, x5::ec::Guild_NotExist);
}

Json game_variable(const std::string &name, const std::string &value) {
  return {{"name", name}, {"value", value}};
}

Json guild_level(int level, int max_members) {
  return {{"level", std::to_string(level)},
          {"requiredGuildExp", "1000"},
          {"maxMemberCount", std::to_string(max_members)},
          {"isMaxGuildLevel", "False"}};
}

TEST(GuildRulesCatalog, ReadsGameVariablesAndKeepsDefaultsForImplausibleRows) {
  TestCatalog fixture({{"GameVariable",
                        {game_variable("GuildNameUnitLengthMin", "3"),
                         game_variable("GuildNameUnitLengthMax", "12"),
                         game_variable("MaxGuildSubMasterCount", "5"),
                         game_variable("GuildRejoinableTimeHours", "2"),
                         game_variable("GuildInviteTimeoutMillis", "not-a-number"),
                         game_variable("GuildJoinableListCount", "4")}},
                       {"GuildLevel", {guild_level(1, 8), guild_level(2, 16)}}});
  ClientCatalog catalog(fixture.root());
  const auto rules = load_guild_rules(catalog);
  EXPECT_EQ(rules.name_min_length, 3u);
  EXPECT_EQ(rules.name_max_length, 12u);
  EXPECT_EQ(rules.max_sub_masters, 5u);
  EXPECT_EQ(rules.rejoin_cooldown, std::chrono::hours(2));
  EXPECT_EQ(rules.invite_timeout, std::chrono::milliseconds(20000));
  EXPECT_EQ(rules.joinable_list_page_size, 4u);
  ASSERT_EQ(rules.levels.size(), 2u);
  EXPECT_EQ(rules.levels.back().max_members, 16u);

  TestCatalog gapped({{"GuildLevel", {guild_level(1, 8), guild_level(3, 16)}}});
  ClientCatalog gapped_catalog(gapped.root());
  EXPECT_EQ(load_guild_rules(gapped_catalog).levels.size(), GuildRules{}.levels.size());
}

// Uses the exported catalog when CHRONICLES_CLIENT_CATALOG names it.
TEST(GuildRulesCatalog, ExportedCatalogMatchesTheDocumentedRules) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the exported data/generated/catalog";
  }

  ClientCatalog catalog(catalog_path);
  const auto rules = load_guild_rules(catalog);
  EXPECT_EQ(rules.name_min_length, 2u);
  EXPECT_EQ(rules.name_max_length, 20u);
  EXPECT_EQ(rules.description_max_length, 200u);
  EXPECT_EQ(rules.max_sub_masters, 3u);
  EXPECT_EQ(rules.rejoin_cooldown, std::chrono::hours(24));
  EXPECT_EQ(rules.recreate_cooldown, std::chrono::hours(168));
  EXPECT_EQ(rules.disband_cooldown, std::chrono::hours(168));
  EXPECT_EQ(rules.invite_timeout, std::chrono::milliseconds(20000));
  EXPECT_EQ(rules.joinable_list_page_size, 10u);
  ASSERT_EQ(rules.levels.size(), 5u);
  EXPECT_EQ(rules.levels.front().max_members, 10u);
  EXPECT_EQ(rules.levels.back().max_members, 30u);
}

template <class Message> Message decode_front(const std::string &wire, std::uint16_t packet_id) {
  const auto frames = native_ws::decode_frames(wire);
  if (frames.size() != 1 || frames.front().packet_id != packet_id) {
    throw std::runtime_error("Unexpected front frame");
  }

  Message message;
  const auto payload = frames.front().payload;
  if (!message.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    throw std::runtime_error("Malformed front frame");
  }
  return message;
}

struct FrontPlayer {
  std::string token;
  std::int64_t character_id{};
  std::uint64_t connection_id{};
};

class GuildFrontTest : public testing::Test {
protected:
  FrontPlayer player(const std::string &name) {
    if (!auth_.create_account(name, "guild-test-password")) {
      throw std::runtime_error("Cannot provision test account");
    }

    FrontPlayer result;
    result.token = auth_.login(name, "guild-test-password").access_token();
    x5::pb::authwebsocket::WorldEnterReq enter;
    enter.set_world_id(kWorldId);
    if (auth_.enter(result.token, enter).error_code() != x5::ec::Success) {
      throw std::runtime_error("Cannot enter test realm");
    }

    x5::pb::ws::front::CharCreateReq create;
    create.set_slot(0);
    create.set_char_name(name);
    create.set_char_class(x5::cms::EX5CharacterClass_Wild);
    const auto created = auth_.create(result.token, create);
    if (created.ec() != x5::ec::Success) {
      throw std::runtime_error("Cannot create test character");
    }
    result.character_id = created.char_().char_id();
    seat_in_company(*companies_, result.character_id, 3);

    x5::pb::ws::front::CharSelectReq select;
    select.set_char_id(result.character_id);
    if (auth_.select(result.token, select).ec() != x5::ec::Success) {
      throw std::runtime_error("Cannot select test character");
    }
    result.connection_id = dispatcher_.chat().connect(result.token);
    return result;
  }

  std::string request(const FrontPlayer &player, std::uint16_t packet_id,
                      const google::protobuf::Message &message) {
    return dispatcher_.handle(NativeWsChannel::front, player.token,
                              native_ws::encode_frame(packet_id, message));
  }

  MemoryRepositories repositories_;
  AuthService auth_{repositories_.character_repositories(), Config{}};
  std::shared_ptr<GuildService> guilds_ =
      std::make_shared<GuildService>(std::make_shared<MemoryGuildStore>(), GuildRules{}, kWorldId);
  std::shared_ptr<CompanyService> companies_ = std::make_shared<CompanyService>(
      std::make_shared<MemoryCompanyStore>(), testing_support::company_rules(), kWorldId);
  NativeWsDispatcher dispatcher_{auth_, NativeWsServices{guilds_, companies_, {}}};
};

TEST_F(GuildFrontTest, CreateJoinAndNotifyMembersThroughTheFrontSocket) {
  const auto founder = player("Founder");
  const auto joiner = player("Joiner");

  x5::pb::guild::GuildCreateReq create;
  create.set_name("Seafarers");
  create.set_desc("Open to all");
  create.set_join_type(x5::cms::EX5GuildJoinType_Auto);
  const auto created = decode_front<x5::pb::guild::GuildCreateRes>(
      request(founder, native_guild::kCreateRequest, create), native_guild::kCreateResponse);
  ASSERT_EQ(created.ec(), x5::ec::Success);
  EXPECT_EQ(created.guild().name(), "Seafarers");
  EXPECT_EQ(created.owner().char_id(), founder.character_id);
  EXPECT_EQ(created.owner().member_grade(), x5::cms::EX5GuildMemberGrade_Master);
  const auto guild_id = created.guild().guild_id();

  x5::pb::guild::GuildJoinableListReq list;
  const auto listed = decode_front<x5::pb::guild::GuildJoinableListRes>(
      request(joiner, native_guild::kJoinableListRequest, list),
      native_guild::kJoinableListResponse);
  ASSERT_EQ(listed.guilds_size(), 1);
  EXPECT_EQ(listed.guilds(0).master_name(), "Founder");

  x5::pb::guild::GuildJoinReq join;
  join.set_guild_id(guild_id);
  const auto joined = decode_front<x5::pb::guild::GuildJoinRes>(
      request(joiner, native_guild::kJoinRequest, join), native_guild::kJoinResponse);
  ASSERT_EQ(joined.ec(), x5::ec::Success);
  EXPECT_EQ(joined.members_size(), 2);
  EXPECT_EQ(joined.member().name(), "Joiner");

  const auto founder_frames = dispatcher_.chat().drain(founder.connection_id);
  ASSERT_EQ(founder_frames.size(), 1u);
  const auto notification = decode_front<x5::pb::guild::GuildJoinNoti>(
      founder_frames.front(), native_guild::kJoinNotification);
  EXPECT_EQ(notification.guild_id(), guild_id);
  EXPECT_EQ(notification.member().char_id(), joiner.character_id);
  EXPECT_TRUE(dispatcher_.chat().drain(joiner.connection_id).empty());

  x5::pb::guild::GuildReq detail;
  detail.set_guild_id(guild_id);
  const auto described = decode_front<x5::pb::guild::GuildRes>(
      request(joiner, native_guild::kGuildRequest, detail), native_guild::kGuildResponse);
  ASSERT_EQ(described.ec(), x5::ec::Success);
  EXPECT_EQ(described.members(0).member_grade(), x5::cms::EX5GuildMemberGrade_Master);
  EXPECT_FALSE(described.has_housing());

  x5::pb::guild::GuildMemberManageReq kick;
  kick.set_guild_id(guild_id);
  kick.set_char_id(joiner.character_id);
  kick.set_grade(x5::cms::EX5GuildMemberGrade_None);
  const auto kicked = decode_front<x5::pb::guild::GuildMemberManageRes>(
      request(founder, native_guild::kMemberManageRequest, kick),
      native_guild::kMemberManageResponse);
  EXPECT_EQ(kicked.ec(), x5::ec::Success);
  EXPECT_EQ(kicked.char_id(), joiner.character_id);

  const auto joiner_frames = dispatcher_.chat().drain(joiner.connection_id);
  ASSERT_EQ(joiner_frames.size(), 1u);
  const auto kick_notification = decode_front<x5::pb::guild::GuildKickNoti>(
      joiner_frames.front(), native_guild::kKickNotification);
  EXPECT_EQ(kick_notification.char_id(), joiner.character_id);
  EXPECT_GT(kick_notification.last_guild_leave_epochtime(), 0);
}

TEST_F(GuildFrontTest, GuildChatReachesOnlyGuildMembers) {
  const auto founder = player("Founder");
  const auto mate = player("Mate");
  const auto outsider = player("Outsider");

  x5::pb::ws::front::ChatReq chat;
  chat.set_type(x5::cms::EX5ChatMessageType_Guild);
  chat.set_message("hello guild");
  const auto refused = decode_front<x5::pb::CommonErrorRes>(
      request(founder, native_ws::kChatRequest, chat), 1);
  EXPECT_EQ(refused.ec(), x5::ec::Guild_NotJoined);

  x5::pb::guild::GuildCreateReq create;
  create.set_name("Seafarers");
  create.set_join_type(x5::cms::EX5GuildJoinType_Auto);
  const auto created = decode_front<x5::pb::guild::GuildCreateRes>(
      request(founder, native_guild::kCreateRequest, create), native_guild::kCreateResponse);
  ASSERT_EQ(created.ec(), x5::ec::Success);
  x5::pb::guild::GuildJoinReq join;
  join.set_guild_id(created.guild().guild_id());
  ASSERT_EQ(decode_front<x5::pb::guild::GuildJoinRes>(
                request(mate, native_guild::kJoinRequest, join), native_guild::kJoinResponse)
                .ec(),
            x5::ec::Success);
  dispatcher_.chat().drain(founder.connection_id);

  EXPECT_TRUE(request(mate, native_ws::kChatRequest, chat).empty());
  for (const auto *member : {&founder, &mate}) {
    const auto frames = dispatcher_.chat().drain(member->connection_id);
    ASSERT_EQ(frames.size(), 1u);
    const auto received = decode_front<x5::pb::frontwebsocket::ChatNoti>(
        frames.front(), native_ws::kChatNotification);
    EXPECT_EQ(received.message(), "hello guild");
    EXPECT_EQ(received.sender_name(), "Mate");
  }
  EXPECT_TRUE(dispatcher_.chat().drain(outsider.connection_id).empty());
}

TEST_F(GuildFrontTest, RequestsWithoutASelectedCharacterOrValidPayloadAreRefused) {
  ASSERT_TRUE(auth_.create_account("Lobby", "guild-test-password"));
  const auto token = auth_.login("Lobby", "guild-test-password").access_token();
  x5::pb::authwebsocket::WorldEnterReq enter;
  enter.set_world_id(kWorldId);
  ASSERT_EQ(auth_.enter(token, enter).error_code(), x5::ec::Success);

  x5::pb::guild::GuildJoinableListReq list;
  const auto refused = decode_front<x5::pb::guild::GuildJoinableListRes>(
      dispatcher_.handle(NativeWsChannel::front, token,
                         native_ws::encode_frame(native_guild::kJoinableListRequest, list)),
      native_guild::kJoinableListResponse);
  EXPECT_EQ(refused.ec(), x5::ec::Guild_InvalidChar);

  const std::string malformed{"\x05\x00\x3c\x4e\x80", 5};
  EXPECT_THROW(dispatcher_.handle(NativeWsChannel::front, token, malformed),
               std::invalid_argument);
}

TEST(GuildServiceStorage, PostgresMembershipTransferAndCooldownSurviveReconstruction) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Requires the isolated PostgreSQL test cluster";
  }
  auto characters = make_postgres_character_repositories(connection);
  const auto owner_epoch = characters.character_state->acquire_monster_reward_owner_epoch();
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  Account account;
  account.name = "guild_" + suffix;
  ASSERT_TRUE(characters.accounts->add_account(account));
  const auto owner = characters.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character first;
  first.account_id = owner->id;
  first.name = "GuildA" + suffix.substr(suffix.size() - 8);
  const auto founder = characters.characters->create(first);
  first.name = "GuildB" + suffix.substr(suffix.size() - 8);
  first.slot = 1;
  const auto member = characters.characters->create(first);
  const auto master_actor = actor(founder.id, founder.name);
  const auto member_actor = actor(member.id, member.name);
  GuildRules rules;
  rules.levels = {{1, 1000, 4}, {2, 2000, 6}};
  FakeGuildClock clock;
  auto store = std::make_shared<PostgresGuildStore>(connection);
  store->adopt_quest_service_owner_epoch(owner_epoch);
  GuildService service(store, rules, kWorldId, clock.clock());
  const auto created = service.create(master_actor, create_request("G" + suffix.substr(suffix.size() - 12)));
  ASSERT_EQ(created.error, x5::ec::Success);
  ASSERT_EQ(service.join(member_actor, created.guild.id).error, x5::ec::Success);
  ASSERT_EQ(service.manage_member(master_actor, created.guild.id, member.id,
                                   static_cast<int>(GuildGrade::master)).error, x5::ec::Success);
  auto restored_store = std::make_shared<PostgresGuildStore>(connection);
  restored_store->adopt_quest_service_owner_epoch(owner_epoch);
  GuildService restored(restored_store, rules, kWorldId, clock.clock());
  EXPECT_EQ(restored.status(member.id).grade, GuildGrade::master);
  EXPECT_EQ(restored.status(founder.id).grade, GuildGrade::member);
  ASSERT_EQ(restored.leave(master_actor, created.guild.id).error, x5::ec::Success);
  auto after_leave_store = std::make_shared<PostgresGuildStore>(connection);
  after_leave_store->adopt_quest_service_owner_epoch(owner_epoch);
  GuildService after_leave(after_leave_store, rules, kWorldId, clock.clock());
  EXPECT_EQ(after_leave.status(founder.id).guild_id, 0);
  EXPECT_EQ(after_leave.join(master_actor, created.guild.id).error, x5::ec::Guild_InvalidTimeJoin);
  clock.advance_hours(169);
  ASSERT_EQ(after_leave.disband(member_actor, created.guild.id).error, x5::ec::Success);
  auto after_disband_store = std::make_shared<PostgresGuildStore>(connection);
  after_disband_store->adopt_quest_service_owner_epoch(owner_epoch);
  GuildService after_disband(after_disband_store, rules, kWorldId, clock.clock());
  EXPECT_EQ(after_disband.status(member.id).guild_id, 0);
  EXPECT_EQ(after_disband.detail(member_actor, created.guild.id).error, x5::ec::Guild_NotExist);
}

TEST(GuildServiceStorage, PostgresQuestSettlementChecksIndexesAndAppliesOnlyOnce) {
  const char *connection_string = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection_string) {
    GTEST_SKIP() << "Requires the isolated PostgreSQL test cluster";
  }
  {
    pqxx::connection connection(connection_string);
    pqxx::read_transaction transaction(connection);
    const auto identity = transaction.exec1("SELECT current_database(), host(inet_server_addr())");
    ASSERT_EQ(identity[0].as<std::string>(), "guild_quest_test");
    ASSERT_EQ(identity[1].as<std::string>(), "127.0.0.1");
  }
  auto repositories = make_postgres_character_repositories(connection_string);
  const auto owner_epoch = repositories.character_state->acquire_monster_reward_owner_epoch();
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  Account account;
  account.name = "guild_effect_" + suffix;
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "GuildQ" + suffix.substr(suffix.size() - 8);
  character = repositories.characters->create(character);

  PostgresGuildStore store(connection_string);
  store.adopt_quest_service_owner_epoch(owner_epoch);
  GuildRecord guild;
  guild.world_id = kWorldId;
  guild.name = "Q" + suffix.substr(suffix.size() - 12);
  GuildMemberRecord master;
  master.char_id = character.id;
  master.account_id = owner->id;
  master.grade = GuildGrade::master;
  const auto created = store.create_guild(guild, master);
  ASSERT_TRUE(created);
  master.guild_id = created->id;
  auto interrupted_guild = *created;
  auto interrupted_member = master;
  interrupted_guild.experience = 9;
  interrupted_member.contribution = 9;
  {
    pqxx::connection connection(connection_string);
    pqxx::work transaction(connection);
    transaction.exec("CREATE FUNCTION test_guild_progression_interrupt() RETURNS trigger "
                     "LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'injected interruption'; "
                     "END; $$");
    transaction.exec("CREATE TRIGGER test_guild_progression_interrupt BEFORE UPDATE OF "
                     "contribution ON emulator_guild_members FOR EACH ROW "
                     "EXECUTE FUNCTION test_guild_progression_interrupt()");
    transaction.commit();
  }
  EXPECT_THROW(store.commit_progression(*created, master, interrupted_guild, interrupted_member),
               std::exception);
  {
    pqxx::connection connection(connection_string);
    pqxx::work transaction(connection);
    const auto unchanged = transaction.exec(
        "SELECT g.experience,m.contribution FROM emulator_guilds g "
        "JOIN emulator_guild_members m ON m.guild_id=g.id WHERE g.id=$1 AND m.character_id=$2",
        pqxx::params{created->id, character.id});
    ASSERT_EQ(unchanged.size(), 1U);
    EXPECT_EQ(unchanged.front()[0].as<int>(), 0);
    EXPECT_EQ(unchanged.front()[1].as<int>(), 0);
    transaction.exec("DROP TRIGGER test_guild_progression_interrupt ON emulator_guild_members");
    transaction.exec("DROP FUNCTION test_guild_progression_interrupt()");
    transaction.commit();
  }

  QuestServiceEffectEvent event;
  event.character_id = character.id;
  event.account_id = owner->id;
  event.world_id = kWorldId;
  event.effect_id = std::string(32, 'd');
  event.mutation_fingerprint = std::string(64, 'e');
  event.effects.expected_guild_id = created->id;
  event.effects.expected_guild_generation = 1;
  event.effects.guild_experience = 10;
  event.company_applied = true;
  {
    pqxx::connection connection(connection_string);
    pqxx::work transaction(connection);
    const auto inserted = transaction.exec(
        "INSERT INTO emulator_quest_service_effects "
        "(character_id,account_id,world_id,effect_id,mutation_fingerprint,monster_receipt,"
        "effect_payload,expected_company_type,expected_guild_id,expected_company_generation,"
        "expected_guild_generation,granted_items,company_applied,guild_applied) "
        "VALUES ($1,$2,$3,$4,$5,false,$6::jsonb,0,$7,0,1,'[]'::jsonb,true,false) "
        "RETURNING enqueue_sequence",
        pqxx::params{event.character_id, event.account_id, event.world_id, event.effect_id,
                     event.mutation_fingerprint, encode_quest_service_effects(event.effects).dump(),
                     created->id});
    event.enqueue_sequence = inserted.front()[0].as<std::uint64_t>();
    transaction.commit();
  }
  EXPECT_THROW(store.commit_progression(*created, master, interrupted_guild, interrupted_member),
               std::exception);

  {
    pqxx::connection connection(connection_string);
    pqxx::work transaction(connection);
    transaction.exec("UPDATE emulator_quest_service_effects SET expected_company_generation=1 "
                     "WHERE character_id=$1 AND effect_id=$2",
                     pqxx::params{event.character_id, event.effect_id});
    transaction.commit();
  }
  EXPECT_EQ(store.read_quest_effect(event).status, QuestEffectSettlementStatus::identity_mismatch);
  {
    pqxx::connection connection(connection_string);
    pqxx::work transaction(connection);
    transaction.exec("UPDATE emulator_quest_service_effects SET expected_company_generation=0 "
                     "WHERE character_id=$1 AND effect_id=$2",
                     pqxx::params{event.character_id, event.effect_id});
    transaction.commit();
  }
  const auto current = store.read_quest_effect(event);
  ASSERT_EQ(current.status, QuestEffectSettlementStatus::pending);
  auto next_guild = *current.guild;
  auto next_member = *current.member;
  next_guild.experience += 10;
  next_member.contribution += 10;
  EXPECT_EQ(store.settle_quest_effect(event, *current.guild, *current.member,
                                      next_guild, next_member).status,
            QuestEffectSettlementStatus::applied);
  const auto replay = store.settle_quest_effect(event, *current.guild, *current.member,
                                                next_guild, next_member);
  EXPECT_EQ(replay.status, QuestEffectSettlementStatus::already_applied);
  ASSERT_TRUE(replay.guild);
  ASSERT_TRUE(replay.member);
  EXPECT_EQ(replay.guild->experience, 10);
  EXPECT_EQ(replay.member->contribution, 10);

  repositories.character_state->acquire_monster_reward_owner_epoch();
  EXPECT_THROW(store.read_quest_effect(event), std::runtime_error);
}

TEST(GuildServiceStorage, PostgresGuildQuestSettlementSurvivesLostCommitAcknowledgement) {
  const char *direct_url = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  const char *proxy_url = std::getenv("CHRONICLES_GUILD_PROXY_TEST_DATABASE_URL");
  if (!direct_url || !proxy_url) {
    GTEST_SKIP() << "Requires the disposable PostgreSQL lost-ack proxy";
  }
  {
    pqxx::connection connection(direct_url);
    pqxx::read_transaction transaction(connection);
    const auto identity = transaction.exec1("SELECT current_database(), host(inet_server_addr())");
    ASSERT_EQ(identity[0].as<std::string>(), "guild_quest_test");
    ASSERT_EQ(identity[1].as<std::string>(), "127.0.0.1");
  }
  auto repositories = make_postgres_character_repositories(direct_url);
  const auto owner_epoch = repositories.character_state->acquire_monster_reward_owner_epoch();
  Account account;
  account.name = "guild_actual_lost_ack";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = "GuildLostAck";
  character = repositories.characters->create(character);

  auto direct_store = std::make_shared<PostgresGuildStore>(direct_url);
  direct_store->adopt_quest_service_owner_epoch(owner_epoch);
  GuildRecord guild;
  guild.world_id = kWorldId;
  guild.name = "LostAck";
  GuildMemberRecord master;
  master.char_id = character.id;
  master.account_id = owner->id;
  master.grade = GuildGrade::master;
  const auto created = direct_store->create_guild(guild, master);
  ASSERT_TRUE(created);

  QuestServiceEffectEvent event;
  event.character_id = character.id;
  event.account_id = owner->id;
  event.world_id = kWorldId;
  event.effect_id = std::string(32, 'c');
  event.mutation_fingerprint = std::string(64, 'd');
  event.effects.expected_guild_id = created->id;
  event.effects.expected_guild_generation = 1;
  event.effects.guild_experience = 3;
  event.effects.activity_experience = 17;
  event.effects.activity_guild_rate = 10;
  event.company_applied = true;
  {
    pqxx::connection connection(direct_url);
    pqxx::work transaction(connection);
    const auto inserted = transaction.exec(
        "INSERT INTO emulator_quest_service_effects "
        "(character_id,account_id,world_id,effect_id,mutation_fingerprint,monster_receipt,"
        "effect_payload,expected_company_type,expected_guild_id,expected_company_generation,"
        "expected_guild_generation,granted_items,company_applied,guild_applied) "
        "VALUES ($1,$2,$3,$4,$5,false,$6::jsonb,0,$7,0,1,'[]'::jsonb,true,false) "
        "RETURNING enqueue_sequence",
        pqxx::params{event.character_id, event.account_id, event.world_id, event.effect_id,
                     event.mutation_fingerprint, encode_quest_service_effects(event.effects).dump(),
                     created->id});
    event.enqueue_sequence = inserted.front()[0].as<std::uint64_t>();
    transaction.commit();
  }

  auto proxy_store = std::make_shared<PostgresGuildStore>(proxy_url);
  proxy_store->adopt_quest_service_owner_epoch(owner_epoch);
  auto faulted_store = std::make_shared<GuildLostAcknowledgementStore>(direct_store, proxy_store);
  GuildRules rules;
  rules.levels = {{1, 1000, 4}, {2, 2000, 6}};
  GuildService service(faulted_store, rules, kWorldId);
  EXPECT_EQ(service.settle_quest_effect(event), QuestEffectSettlementStatus::retry);
  EXPECT_THROW(service.gm_add_experience(created->id, 1), std::runtime_error);
  const auto reconciled = service.reconcile_uncertain_quest_effect();
  ASSERT_TRUE(reconciled);
  EXPECT_EQ(reconciled->status, QuestEffectSettlementStatus::already_applied);
  EXPECT_EQ(service.members(created->id).front().contribution, 4);
  const auto settled = direct_store->read_quest_effect(event);
  EXPECT_EQ(settled.status, QuestEffectSettlementStatus::already_applied);
  EXPECT_EQ(settled.activity_remainder, 7);

  GuildService restored(direct_store, rules, kWorldId);
  EXPECT_EQ(restored.members(created->id).front().contribution, 4);
  EXPECT_EQ(restored.detail(actor(character.id, character.name), created->id).guild.experience, 4);
  EXPECT_EQ(restored.settle_quest_effect(event), QuestEffectSettlementStatus::already_applied);
  EXPECT_EQ(restored.members(created->id).front().contribution, 4);
}

enum class LostGuildWrite { activity, game_master, settings };

void verify_postgres_lost_progression_ack(LostGuildWrite operation) {
  const char *direct_url = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  const char *proxy_url = std::getenv("CHRONICLES_GUILD_PROXY_TEST_DATABASE_URL");
  if (!direct_url || !proxy_url) {
    GTEST_SKIP() << "Requires the disposable PostgreSQL lost-ack proxy";
  }
  {
    pqxx::connection connection(direct_url);
    pqxx::read_transaction transaction(connection);
    const auto identity = transaction.exec1("SELECT current_database(), host(inet_server_addr())");
    ASSERT_EQ(identity[0].as<std::string>(), "guild_progression_test");
    ASSERT_EQ(identity[1].as<std::string>(), "127.0.0.1");
  }
  auto repositories = make_postgres_character_repositories(direct_url);
  const auto epoch = repositories.character_state->acquire_monster_reward_owner_epoch();
  const bool game_master = operation == LostGuildWrite::game_master;
  const bool settings = operation == LostGuildWrite::settings;
  std::string suffix = "activity";
  std::string character_name = "GuildActivityAck";
  std::string guild_name = "ActivityAck";
  if (game_master) {
    suffix = "gm";
    character_name = "GuildGmAck";
    guild_name = "GmAck";
  } else if (settings) {
    suffix = "settings";
    character_name = "GuildSettingsAck";
    guild_name = "SettingsAck";
  }
  Account account;
  account.name = "guild_progression_" + suffix;
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character character;
  character.account_id = owner->id;
  character.name = character_name;
  character = repositories.characters->create(character);

  auto direct = std::make_shared<PostgresGuildStore>(direct_url);
  direct->adopt_quest_service_owner_epoch(epoch);
  GuildRecord guild;
  guild.world_id = kWorldId;
  guild.name = guild_name;
  GuildMemberRecord master;
  master.char_id = character.id;
  master.account_id = owner->id;
  master.grade = GuildGrade::master;
  const auto created = direct->create_guild(guild, master);
  ASSERT_TRUE(created);
  auto proxy = std::make_shared<PostgresGuildStore>(proxy_url);
  proxy->adopt_quest_service_owner_epoch(epoch);
  auto faulted = std::make_shared<GuildLostAcknowledgementStore>(direct, proxy);
  GuildRules rules;
  rules.levels = {{1, 1000, 4}, {2, 2000, 6}};
  GuildService service(faulted, rules, kWorldId);
  if (settings) {
    GuildManageRequest request;
    request.guild_id = created->id;
    request.description = "Recovered settings";
    request.notice = "Updated notice";
    request.join_type = 1;
    EXPECT_EQ(service.manage(actor(character.id, character.name), request).error,
              x5::ec::Guild_FailedUpdate);
  } else if (game_master) {
    EXPECT_EQ(service.gm_add_experience(created->id, 10), x5::ec::Guild_FailedUpdate);
  } else {
    EXPECT_FALSE(service.award_experience(character.id, 10));
  }
  EXPECT_THROW(service.status(character.id), std::runtime_error);
  EXPECT_THROW(service.gm_add_experience(created->id, 5), std::runtime_error);
  faulted->fail_read = true;
  EXPECT_FALSE(service.reconcile_uncertain_progression());
  EXPECT_THROW(service.status(character.id), std::runtime_error);
  faulted->fail_read = false;

  bool fail_notification = true;
  std::vector<GuildEvent> observed_notifications;
  service.subscribe([&](const std::vector<GuildEvent> &events) {
    observed_notifications.insert(observed_notifications.end(), events.begin(), events.end());
    if (fail_notification) {
      throw std::runtime_error("Injected resynchronization failure");
    }
  });
  EXPECT_FALSE(service.reconcile_uncertain_progression());
  EXPECT_THROW(service.gm_add_experience(created->id, 5), std::runtime_error);
  fail_notification = false;
  ASSERT_TRUE(service.reconcile_uncertain_progression());
  EXPECT_EQ(service.detail(actor(character.id, character.name), created->id).guild.experience,
            settings ? 0 : 10);
  EXPECT_EQ(service.members(created->id).front().contribution,
            (game_master || settings) ? 0 : 10);
  if (settings) {
    EXPECT_EQ(service.detail(actor(character.id, character.name), created->id).guild.description,
              "Recovered settings");
    EXPECT_EQ(service.detail(actor(character.id, character.name), created->id).guild.notice,
              "Updated notice");
    const auto notice = std::ranges::find_if(observed_notifications, [](const GuildEvent &event) {
      const auto *changed = std::get_if<GuildInfoChanged>(&event);
      return changed && changed->guild.description == "Recovered settings";
    });
    EXPECT_NE(notice, observed_notifications.end());
  }

  faulted->use_direct_store();
  ASSERT_TRUE(service.award_experience(character.id, 5));
  EXPECT_EQ(service.detail(actor(character.id, character.name), created->id).guild.experience,
            settings ? 5 : 15);
  EXPECT_EQ(service.members(created->id).front().contribution,
            (game_master || settings) ? 5 : 15);
  GuildService restored(direct, rules, kWorldId);
  const auto restored_guild = restored.detail(actor(character.id, character.name), created->id).guild;
  EXPECT_EQ(restored_guild.experience, settings ? 5 : 15);
  EXPECT_EQ(restored.members(created->id).front().contribution,
            (game_master || settings) ? 5 : 15);
  if (settings) {
    EXPECT_EQ(restored_guild.description, "Recovered settings");
    EXPECT_EQ(restored_guild.notice, "Updated notice");
  }
}

TEST(GuildServiceStorage, PostgresActivityProgressionFencesLostCommitAcknowledgement) {
  verify_postgres_lost_progression_ack(LostGuildWrite::activity);
}

TEST(GuildServiceStorage, PostgresGmProgressionFencesLostCommitAcknowledgement) {
  verify_postgres_lost_progression_ack(LostGuildWrite::game_master);
}

TEST(GuildServiceStorage, PostgresSettingsFencesLostCommitAcknowledgement) {
  verify_postgres_lost_progression_ack(LostGuildWrite::settings);
}

TEST(GuildFrontDisabled, GuildPacketsStayUnsupportedWithoutAGuildService) {
  MemoryRepositories store;
  AuthService auth(store.character_repositories(), Config{});
  NativeWsDispatcher dispatcher(auth);
  ASSERT_TRUE(auth.create_account("NoGuild", "guild-test-password"));
  const auto token = auth.login("NoGuild", "guild-test-password").access_token();
  x5::pb::authwebsocket::WorldEnterReq enter;
  enter.set_world_id(kWorldId);
  ASSERT_EQ(auth.enter(token, enter).error_code(), x5::ec::Success);

  x5::pb::guild::GuildJoinableListReq list;
  EXPECT_THROW(dispatcher.handle(NativeWsChannel::front, token,
                                 native_ws::encode_frame(native_guild::kJoinableListRequest, list)),
               std::invalid_argument);
}

class GuildWorldTest : public testing::Test {
protected:
  void SetUp() override {
    world_.attach(actor(1, "Founder"), 101);
    world_.attach(actor(2, "Target"), 102);
    guild_id_ = guilds_->create(actor(1, "Founder"), create_request("Seafarers")).guild.id;
  }

  // Every produced payload must encode with the recovered schema of its opcode.
  void expect_encodes(const guild_world::GuildWorldUpdate &update) {
    for (const auto &packet : update.packets) {
      SCOPED_TRACE(packet.opcode);
      EXPECT_NO_THROW(protocol_.encode(packet.opcode, packet.message));
    }
    for (const auto &change : update.unit_changes) {
      EXPECT_NO_THROW(protocol_.encode(guild_world::kChangeGuildUnitNotification,
                                       GuildWorld::change_guild_unit_message(change)));
    }
  }

  Protocol protocol_{CHRONICLES_PROTOCOL_DIR};
  std::shared_ptr<GuildService> guilds_ =
      std::make_shared<GuildService>(std::make_shared<MemoryGuildStore>(), GuildRules{}, kWorldId);
  GuildWorld world_{guilds_};
  std::int64_t guild_id_{};
};

TEST_F(GuildWorldTest, AttachSendsTheEntrySnapshotAndOwnGuildLabel) {
  const auto update = world_.attach(actor(1, "Founder"), 101);
  ASSERT_EQ(update.packets.size(), 2u);
  EXPECT_EQ(update.packets.front().opcode, guild_world::kWorldCharGuildNotification);
  EXPECT_EQ(update.packets.front().message.at("guild_id"), std::to_string(guild_id_));
  EXPECT_TRUE(update.unit_changes.empty());
  EXPECT_EQ(update.packets.back().char_id, 1);
  EXPECT_EQ(update.packets.back().message.at("unit_id"),
            std::to_string(player_wire_unit_id(1)));
  EXPECT_EQ(update.packets.back().message.at("guild_name"), "Seafarers");
  expect_encodes(update);

  const auto unguilded = world_.attach(actor(2, "Target"), 102);
  EXPECT_TRUE(unguilded.packets.front().message.empty());
  EXPECT_TRUE(unguilded.unit_changes.empty());
  EXPECT_EQ(world_.connection_of(2), 102u);

  world_.detach(2, 999);
  EXPECT_EQ(world_.connection_of(2), 102u);
  world_.detach(2, 102);
  EXPECT_EQ(world_.connection_of(2), std::nullopt);
}

TEST_F(GuildWorldTest, InvitationFlowProducesSchemaValidPackets) {
  const Json invite{{"guild_id", std::to_string(guild_id_)}, {"target_char_id", "2"}};
  const auto response = world_.receive(1, guild_world::kInvitePopupRequest, invite, 7);
  ASSERT_EQ(response.packets.size(), 1u);
  EXPECT_EQ(response.packets.front().char_id, 1);
  EXPECT_EQ(response.packets.front().opcode, guild_world::kInvitePopupResponse);
  EXPECT_EQ(response.packets.front().message.value("target_char_name", ""), "Target");
  EXPECT_FALSE(response.packets.front().message.contains("ec"));
  expect_encodes(response);

  const Json missing{{"guild_id", std::to_string(guild_id_)}, {"target_char_id", "77"}};
  const auto refused = world_.receive(1, guild_world::kInvitePopupRequest, missing, 8);
  EXPECT_EQ(refused.packets.front().message.at("ec"), "Guild_NotExistsInvitee");

  const Json wrong_inviter{{"guild_id", std::to_string(guild_id_)}, {"guild_char_id", "5"}};
  const auto error = world_.receive(2, guild_world::kAcceptInviteRequest, wrong_inviter, 9);
  ASSERT_EQ(error.packets.size(), 1u);
  EXPECT_EQ(error.packets.front().opcode, guild_world::kCommonErrorResponse);
  EXPECT_EQ(error.packets.front().message.at("category"), "EErrorCategory_Guild");
  expect_encodes(error);

  const Json accept{{"guild_id", std::to_string(guild_id_)}, {"guild_char_id", "1"}};
  EXPECT_TRUE(world_.receive(2, guild_world::kAcceptInviteRequest, accept, 10).packets.empty());
  EXPECT_EQ(guilds_->status(2).guild_id, guild_id_);

  const auto unattached = world_.receive(42, guild_world::kRejectInviteRequest, accept, 11);
  EXPECT_EQ(unattached.packets.front().message.at("ec"), "Guild_InvalidChar");
}

TEST_F(GuildWorldTest, TranslatesEventsIntoPopupsRejectionsAndUnitLabels) {
  GuildInviteIssued issued{guild_id_, "Seafarers", 1, 2};
  GuildInviteRejected rejected{1, "Target", GuildInviteRejectReason::user_rejected};
  GuildMemberLeft left{guild_id_, 2, true, 1, {1}};
  GuildDisbanded disbanded{guild_id_, {1, 2}, {2}};
  const auto update = world_.translate({issued, rejected, left, disbanded});

  ASSERT_EQ(update.packets.size(), 2u);
  EXPECT_EQ(update.packets[0].char_id, 2);
  EXPECT_EQ(update.packets[0].opcode, guild_world::kInvitePopupNotification);
  EXPECT_EQ(update.packets[0].message.at("guild_char_id"), "1");
  EXPECT_EQ(update.packets[1].char_id, 1);
  EXPECT_EQ(update.packets[1].opcode, guild_world::kInviteRejectNotification);
  ASSERT_EQ(update.unit_changes.size(), 3u);
  EXPECT_EQ(update.unit_changes.front().guild_id, 0);
  expect_encodes(update);

  const auto cleared = GuildWorld::change_guild_unit_message(update.unit_changes.front());
  EXPECT_FALSE(cleared.contains("guild_id"));
  EXPECT_EQ(cleared.at("unit_id"), std::to_string(player_wire_unit_id(2)));
}

TEST_F(GuildWorldTest, DecoratesObservedGuildMembers) {
  Json member_unit{{"unit_id", std::to_string(player_wire_unit_id(1))},
                   {"nub", {{"player", Json::object()}}}};
  world_.decorate_unit_add(member_unit);
  EXPECT_EQ(member_unit["nub"]["player"]["guild_id"], std::to_string(guild_id_));
  EXPECT_EQ(member_unit["nub"]["player"]["guild_name"], "Seafarers");

  Json other_unit{{"unit_id", std::to_string(player_wire_unit_id(2))},
                  {"nub", {{"player", Json::object()}}}};
  world_.decorate_unit_add(other_unit);
  EXPECT_FALSE(other_unit["nub"]["player"].contains("guild_id"));

  Json npc_unit{{"unit_id", "1"}, {"nub", {{"npc", Json::object()}}}};
  world_.decorate_unit_add(npc_unit);
  EXPECT_FALSE(npc_unit["nub"].contains("player"));
}

} // namespace
} // namespace chronicles
