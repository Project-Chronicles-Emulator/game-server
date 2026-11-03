#include "auth/auth.hpp"
#include "persistence/memory_quest_service_coordinator.hpp"
#include "persistence/quest_service_effect_codec.hpp"
#include "testing/catalog_test_support.hpp"
#include "services/social/character_social.hpp"
#include "data/client_catalog.hpp"
#include "world/features/company/company.hpp"
#include "world/features/company/company_dialogues.hpp"
#include "testing/company_test_support.hpp"
#include "world/features/company/company_world.hpp"
#include "testing/memory_repositories.hpp"
#include "world/features/gm/gm_commands.hpp"
#include "world/features/guild/guild.hpp"
#include "world/features/gm/social_gm_commands.hpp"

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include <chrono>
#include <cstdlib>
#include <limits>
#include <map>
#include <tuple>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace chronicles {
namespace {

using Json = nlohmann::json;
using testing_support::kAntiqus;
using testing_support::kAntiqusThresholds;
using testing_support::kBlueSalt;
using testing_support::seat_in_company;
using testing_support::TestCatalog;

constexpr int kWorldId = 1;
constexpr std::int64_t kStartTicks = 1'700'000'000LL * kEpochTicksPerSecond;
constexpr std::int64_t kTicksPerMinute = 60 * kEpochTicksPerSecond;

TEST(MemoryCompanyQuestEffects, PendingRewardBlocksCompanyChangesUntilSettlement) {
  MemoryRepositories repositories;
  auto coordinator = repositories.database->quest_services;
  const auto epoch = repositories.character_state->acquire_monster_reward_owner_epoch();
  MemoryCompanyStore store({}, coordinator);
  store.adopt_quest_owner_epoch(epoch);
  CompanyRecord record;
  record.char_id = 1;
  record.company_type = kBlueSalt;
  store.save(record);

  QuestServiceEffectEvent event;
  event.character_id = record.char_id;
  event.world_id = kWorldId;
  event.effect_id = std::string(32, 'a');
  event.effects.expected_company_type = kBlueSalt;
  event.guild_applied = true;
  {
    std::lock_guard lock(coordinator->mutex);
    ASSERT_TRUE(coordinator->record_event(event));
  }

  auto changed = record;
  changed.company_type = kAntiqus;
  EXPECT_THROW(store.save(changed), std::runtime_error);
  EXPECT_EQ(store.load(kWorldId).front(), record);
  {
    std::lock_guard lock(coordinator->mutex);
    EXPECT_EQ(coordinator->company_memberships.at(record.char_id), kBlueSalt);
    auto &stored_event = coordinator->events.at({record.char_id, event.effect_id});
    ASSERT_TRUE(coordinator->can_mark_component(stored_event, QuestServiceComponent::company));
    coordinator->mark_component_applied(stored_event, QuestServiceComponent::company);
  }
  EXPECT_NO_THROW(store.save(changed));
  EXPECT_EQ(store.load(kWorldId).front(), changed);
}

TEST(MemoryCompanyQuestEffects, RejoiningTheSameCompanyRejectsTheOldCapturedGeneration) {
  MemoryRepositories repositories;
  Character character;
  character.account_id = 12;
  character.name = "QuestGenerationOwner";
  character = repositories.characters->create(character);
  const auto epoch = repositories.character_state->acquire_monster_reward_owner_epoch();
  auto coordinator = repositories.database->quest_services;
  MemoryCompanyStore store({}, coordinator);
  store.adopt_quest_owner_epoch(epoch);
  CompanyRecord record;
  record.char_id = character.id;
  record.company_type = kBlueSalt;
  store.save(record);
  QuestServiceEffects effects;
  effects.expected_company_type = kBlueSalt;
  effects.expected_company_generation = 1;
  effects.company_contribution = 1;
  record.company_type = 0;
  store.save(record);
  record.company_type = kBlueSalt;
  store.save(record);

  const auto before = repositories.character_state->load_world_progress(12, character.id, 1);
  auto after = before;
  ++after.revision;
  ++after.gold;
  const auto result = repositories.character_state->commit_quest_mutation(
      12, character.id, 1, std::string(32, 'c'), before, after, ItemTrade{}, effects);
  EXPECT_EQ(result.status, QuestMutationStatus::conflict);
  EXPECT_EQ(repositories.character_state->load_world_progress(12, character.id, 1), before);
  std::lock_guard lock(coordinator->mutex);
  EXPECT_EQ(coordinator->company_generations.at(character.id), 3);
  EXPECT_TRUE(coordinator->events.empty());
}

class LostCompanyQuestAcknowledgementStore final : public CompanyStore {
public:
  explicit LostCompanyQuestAcknowledgementStore(std::shared_ptr<CompanyStore> inner)
      : inner_(std::move(inner)) {
  }

  std::vector<CompanyRecord> load(int world_id) override {
    return inner_->load(world_id);
  }
  void save(const CompanyRecord &record) override {
    inner_->save(record);
  }
  void adopt_quest_owner_epoch(std::int64_t epoch) override {
    inner_->adopt_quest_owner_epoch(epoch);
  }
  CompanyQuestSettlementResult read_quest_effect(const QuestServiceEffectEvent &event) override {
    if (fail_readback) {
      throw std::runtime_error("Synthetic unavailable company readback");
    }
    return inner_->read_quest_effect(event);
  }
  CompanyQuestSettlementResult settle_quest_effect(const QuestServiceEffectEvent &event,
                                                    const CompanyRecord &before,
                                                    const CompanyQuestPlanner &planner) override {
    if (fail_before_write) {
      fail_before_write = false;
      throw std::bad_alloc();
    }
    auto result = inner_->settle_quest_effect(event, before, planner);
    if (lose_acknowledgement) {
      lose_acknowledgement = false;
      throw std::runtime_error("Synthetic lost company commit acknowledgement");
    }
    return result;
  }

  bool lose_acknowledgement{};
  bool fail_readback{};
  bool fail_before_write{};

private:
  std::shared_ptr<CompanyStore> inner_;
};

class CompanyQuestSettlementTest : public testing::Test {
protected:
  void SetUp() override {
    Character candidate;
    candidate.account_id = 12;
    candidate.name = "QuestCompanyOwner";
    character = repositories.characters->create(candidate);
    const auto epoch = repositories.character_state->acquire_monster_reward_owner_epoch();
    concrete = std::make_shared<MemoryCompanyStore>(repositories.character_state,
                                                    repositories.database->quest_services);
    store = std::make_shared<LostCompanyQuestAcknowledgementStore>(concrete);
    store->adopt_quest_owner_epoch(epoch);
    service = std::make_unique<CompanyService>(store, testing_support::company_rules(), kWorldId,
                                               [] { return kStartTicks; });
    CompanyRecord initial;
    initial.char_id = character.id;
    initial.company_type = kBlueSalt;
    ASSERT_EQ(service->set_record(initial).error, x5::ec::Success);
  }

  QuestServiceEffectEvent create_event(char digit, std::int64_t contribution,
                                        std::vector<QuestServiceUnlock> unlocks = {},
                                        std::int64_t activity_experience = 0,
                                        std::int64_t activity_rate = 20) {
    const auto membership = repositories.character_state->read_quest_service_membership(
        character.account_id, character.id, kWorldId);
    EXPECT_TRUE(membership);
    QuestServiceEffects effects;
    effects.company_contribution = contribution;
    effects.activity_experience = activity_experience;
    effects.activity_company_rate = activity_rate;
    effects.company_unlocks = std::move(unlocks);
    effects.expected_company_type = membership->company_type;
    effects.expected_guild_id = membership->guild_id;
    effects.expected_company_generation = membership->company_generation;
    effects.expected_guild_generation = membership->guild_generation;
    QuestServiceEffectEvent event;
    event.account_id = character.account_id;
    event.character_id = character.id;
    event.world_id = kWorldId;
    event.effect_id = std::string(32, digit);
    event.effects = std::move(effects);
    event.guild_applied = true;
    std::lock_guard lock(repositories.database->quest_services->mutex);
    EXPECT_TRUE(repositories.database->quest_services->record_event(event));
    return repositories.database->quest_services->events.at({character.id, event.effect_id});
  }

  MemoryRepositories repositories;
  Character character;
  std::shared_ptr<MemoryCompanyStore> concrete;
  std::shared_ptr<LostCompanyQuestAcknowledgementStore> store;
  std::unique_ptr<CompanyService> service;
};

TEST_F(CompanyQuestSettlementTest, SettlementAppliesUnlockBeforeContributionOnce) {
  const auto event = create_event('a', 2000, {{kBlueSalt, 2}});
  const auto result = service->settle_quest_effect(event);
  EXPECT_EQ(result.status, QuestEffectSettlementStatus::applied);
  EXPECT_EQ(result.record.unlocked_position_level, 2);
  EXPECT_EQ(result.record.contribution, 2000);
  EXPECT_EQ(service->status(character.id).record, result.record);
  EXPECT_EQ(service->settle_quest_effect(event).status,
            QuestEffectSettlementStatus::already_applied);
  EXPECT_EQ(service->status(character.id).record.contribution, 2000);
  EXPECT_TRUE(repositories.character_state->pending_quest_service_effects(kWorldId, {}, 10).empty());
}

TEST_F(CompanyQuestSettlementTest, LostAcknowledgementQuarantinesUntilFencedReadback) {
  const auto event = create_event('b', 10);
  store->lose_acknowledgement = true;
  store->fail_readback = true;
  EXPECT_THROW(service->settle_quest_effect(event), std::runtime_error);
  EXPECT_THROW(service->status(character.id), std::runtime_error);
  EXPECT_TRUE(repositories.character_state->pending_quest_service_effects(kWorldId, {}, 10).empty());
  EXPECT_THROW(service->reconcile_uncertain_quest_effect(), std::runtime_error);
  EXPECT_THROW(service->status(character.id), std::runtime_error);

  store->fail_readback = false;
  const auto recovered = service->reconcile_uncertain_quest_effect();
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->status, QuestEffectSettlementStatus::already_applied);
  EXPECT_EQ(service->status(character.id).record.contribution, 10);
  EXPECT_FALSE(service->reconcile_uncertain_quest_effect());
}

TEST_F(CompanyQuestSettlementTest, EarlierUnfinishedEventBlocksLaterCompanyEffect) {
  const auto first = create_event('c', 10);
  const auto second = create_event('d', 20);
  EXPECT_EQ(service->settle_quest_effect(second).status, QuestEffectSettlementStatus::retry);
  EXPECT_EQ(service->status(character.id).record.contribution, 0);
  EXPECT_EQ(service->settle_quest_effect(first).status, QuestEffectSettlementStatus::applied);
  EXPECT_EQ(service->settle_quest_effect(second).status, QuestEffectSettlementStatus::applied);
  EXPECT_EQ(service->status(character.id).record.contribution, 30);
}

TEST_F(CompanyQuestSettlementTest, ActivityRemainderIsDurableAcrossQueuedEventsAndLostAck) {
  const auto first = create_event('f', 0, {}, 13);
  const auto second = create_event('a', 3, {}, 14);
  EXPECT_EQ(service->settle_quest_effect(second).status, QuestEffectSettlementStatus::retry);
  EXPECT_EQ(service->settle_quest_effect(first).status, QuestEffectSettlementStatus::applied);
  EXPECT_EQ(service->status(character.id).record.contribution, 0);
  store->lose_acknowledgement = true;
  EXPECT_THROW(service->settle_quest_effect(second), std::runtime_error);
  ASSERT_TRUE(service->reconcile_uncertain_quest_effect());
  EXPECT_EQ(service->status(character.id).record.contribution, 4);
  EXPECT_EQ(service->settle_quest_effect(second).status,
            QuestEffectSettlementStatus::already_applied);
  const auto coordinator = repositories.database->quest_services;
  std::lock_guard lock(coordinator->mutex);
  const auto key = std::make_tuple(kWorldId, character.id, QuestServiceComponent::company,
                                   first.effects.expected_company_generation, 20);
  EXPECT_EQ(coordinator->activity_remainders.at(key), 7);
}

TEST_F(CompanyQuestSettlementTest, PlannerFailureLeavesRecordRemainderAndMarkerUnchanged) {
  const auto event = create_event('a', 0, {}, 15);
  const auto before = service->status(character.id).record;
  const CompanyQuestPlanner failing = [](std::int64_t) -> CompanyRecord {
    throw std::runtime_error("Synthetic planner failure");
  };
  EXPECT_THROW(concrete->settle_quest_effect(event, before, failing), std::runtime_error);
  EXPECT_EQ(service->status(character.id).record, before);
  const auto coordinator = repositories.database->quest_services;
  {
    std::lock_guard lock(coordinator->mutex);
    EXPECT_TRUE(coordinator->activity_remainders.empty());
    EXPECT_FALSE(coordinator->events.at({character.id, event.effect_id}).company_applied);
  }
  EXPECT_EQ(service->settle_quest_effect(event).status, QuestEffectSettlementStatus::applied);
}

TEST_F(CompanyQuestSettlementTest, RejoiningCompanyDoesNotInheritOldActivityRemainder) {
  const auto first = create_event('a', 0, {}, 19);
  ASSERT_EQ(service->settle_quest_effect(first).status, QuestEffectSettlementStatus::applied);
  auto record = service->status(character.id).record;
  record.company_type = 0;
  ASSERT_EQ(service->set_record(record).error, x5::ec::Success);
  record.company_type = kBlueSalt;
  ASSERT_EQ(service->set_record(record).error, x5::ec::Success);

  const auto second = create_event('b', 0, {}, 5);
  ASSERT_NE(first.effects.expected_company_generation,
            second.effects.expected_company_generation);
  ASSERT_EQ(service->settle_quest_effect(second).status, QuestEffectSettlementStatus::applied);
  EXPECT_EQ(service->status(character.id).record.contribution, 0);
  const auto coordinator = repositories.database->quest_services;
  std::lock_guard lock(coordinator->mutex);
  const auto old_key = std::make_tuple(kWorldId, character.id, QuestServiceComponent::company,
                                       first.effects.expected_company_generation, 20);
  const auto new_key = std::make_tuple(kWorldId, character.id, QuestServiceComponent::company,
                                       second.effects.expected_company_generation, 20);
  EXPECT_EQ(coordinator->activity_remainders.at(old_key), 19);
  EXPECT_EQ(coordinator->activity_remainders.at(new_key), 5);
}

TEST_F(CompanyQuestSettlementTest, ActivityRateHasIndependentRemainderAndSaturatesAtCap) {
  const auto first = create_event('a', 0, {}, 19, 20);
  const auto second = create_event('b', 0, {}, 19, 10);
  EXPECT_EQ(service->settle_quest_effect(first).status, QuestEffectSettlementStatus::applied);
  EXPECT_EQ(service->settle_quest_effect(second).status, QuestEffectSettlementStatus::applied);
  EXPECT_EQ(service->status(character.id).record.contribution, 1);
  const auto huge = create_event('c', 0, {}, std::numeric_limits<std::int64_t>::max(), 1);
  EXPECT_EQ(service->settle_quest_effect(huge).status, QuestEffectSettlementStatus::applied);
  EXPECT_EQ(service->status(character.id).record.contribution,
            service->contribution_cap(service->status(character.id).record));
}

TEST_F(CompanyQuestSettlementTest, DefiniteMismatchBlocksOnlyItsCharacterUntilRepair) {
  Character other_candidate;
  other_candidate.account_id = character.account_id;
  other_candidate.slot = 1;
  other_candidate.name = "OtherCompanyOwner";
  const auto other = repositories.characters->create(other_candidate);
  CompanyRecord other_record;
  other_record.char_id = other.id;
  other_record.company_type = kBlueSalt;
  ASSERT_EQ(service->set_record(other_record).error, x5::ec::Success);
  const auto event = create_event('e', 10);
  {
    std::lock_guard lock(repositories.database->quest_services->mutex);
    ++repositories.database->quest_services->company_generations[character.id];
  }
  EXPECT_EQ(service->settle_quest_effect(event).status,
            QuestEffectSettlementStatus::identity_mismatch);
  EXPECT_THROW(service->status(character.id), std::runtime_error);
  EXPECT_FALSE(service->reconcile_uncertain_quest_effect());
  EXPECT_TRUE(service->add_contribution(other.id, 5));
  const auto other_membership = repositories.character_state->read_quest_service_membership(
      other.account_id, other.id, kWorldId);
  ASSERT_TRUE(other_membership);
  QuestServiceEffectEvent other_event;
  other_event.account_id = other.account_id;
  other_event.character_id = other.id;
  other_event.world_id = kWorldId;
  other_event.effect_id = std::string(32, 'b');
  other_event.effects.expected_company_type = other_membership->company_type;
  other_event.effects.expected_company_generation = other_membership->company_generation;
  other_event.effects.company_contribution = 7;
  other_event.guild_applied = true;
  {
    std::lock_guard lock(repositories.database->quest_services->mutex);
    ASSERT_TRUE(repositories.database->quest_services->record_event(other_event));
    other_event = repositories.database->quest_services->events.at({other.id, other_event.effect_id});
  }
  EXPECT_EQ(service->settle_quest_effect(other_event).status, QuestEffectSettlementStatus::applied);
  EXPECT_EQ(service->status(other.id).record.contribution, 12);
  {
    std::lock_guard lock(repositories.database->quest_services->mutex);
    --repositories.database->quest_services->company_generations[character.id];
  }
  EXPECT_EQ(service->settle_quest_effect(event).status, QuestEffectSettlementStatus::applied);
  EXPECT_EQ(service->status(character.id).record.contribution, 10);
}

TEST_F(CompanyQuestSettlementTest, PrewriteAllocationFailureRecoversThroughFencedReadback) {
  const auto event = create_event('a', 10);
  store->fail_before_write = true;
  EXPECT_THROW(service->settle_quest_effect(event), std::bad_alloc);
  EXPECT_THROW(service->status(character.id), std::runtime_error);
  const auto recovered = service->reconcile_uncertain_quest_effect();
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->status, QuestEffectSettlementStatus::pending);
  EXPECT_EQ(service->status(character.id).record.contribution, 0);
  EXPECT_EQ(service->settle_quest_effect(event).status, QuestEffectSettlementStatus::applied);
  EXPECT_EQ(service->status(character.id).record.contribution, 10);
}

class CompanyServiceTest : public testing::Test {
protected:
  CompanyServiceTest() {
    rebuild();
  }

  void rebuild() {
    service_ = std::make_unique<CompanyService>(store_, testing_support::company_rules(),
                                                kWorldId, [this] { return now_; });
  }

  std::int64_t now_{kStartTicks};
  std::shared_ptr<MemoryCompanyStore> store_ = std::make_shared<MemoryCompanyStore>();
  std::unique_ptr<CompanyService> service_;
};

TEST_F(CompanyServiceTest, PositionLevelsFollowCumulativeThresholds) {
  for (int level = 1; level <= 6; ++level) {
    const auto threshold = kAntiqusThresholds[level - 1];
    EXPECT_EQ(service_->position_level(kAntiqus, threshold), level);
    EXPECT_EQ(service_->contribution_for_position(kAntiqus, level), threshold);
    if (threshold > 0) {
      EXPECT_EQ(service_->position_level(kAntiqus, threshold - 1), level - 1);
    }
  }
  EXPECT_EQ(service_->position_level(kAntiqus, 1'000'000), 6);
  EXPECT_FALSE(service_->contribution_for_position(kAntiqus, 7).has_value());
  EXPECT_EQ(service_->position_level(0, 100000), 0);
}

TEST_F(CompanyServiceTest, CappedLevelsHoldContributionUntilUnlocked) {
  ASSERT_EQ(service_->join(1, kBlueSalt).error, x5::ec::Success);

  // BlueSalt level 2 caps earned contribution one below its 1501 threshold.
  const auto capped = service_->add_contribution(1, 20000);
  ASSERT_TRUE(capped.has_value());
  EXPECT_EQ(capped->status.record.contribution, 1500);
  EXPECT_EQ(capped->status.position_level, 2);
  EXPECT_EQ(capped->contribution, 1500);
  // Held at a capped level's ceiling, as Donella's housing quest dialogues require.
  EXPECT_TRUE(capped->status.at_capped_level_ceiling);

  EXPECT_EQ(service_->unlock_position(1, kAntiqus, 2).error, x5::ec::Company_NotMatched);
  ASSERT_EQ(service_->unlock_position(1, kBlueSalt, 2).error, x5::ec::Success);
  const auto level_three = service_->add_contribution(1, 20000);
  ASSERT_TRUE(level_three.has_value());
  EXPECT_EQ(level_three->status.record.contribution, 11500);
  EXPECT_EQ(level_three->status.position_level, 3);
  EXPECT_TRUE(level_three->position_changed);
  EXPECT_TRUE(level_three->status.at_capped_level_ceiling);

  ASSERT_EQ(service_->unlock_position(1, kBlueSalt, 3).error, x5::ec::Success);
  const auto uncapped = service_->add_contribution(1, 20000);
  ASSERT_TRUE(uncapped.has_value());
  EXPECT_EQ(uncapped->status.record.contribution, 31500);
  EXPECT_EQ(uncapped->status.position_level, 4);
  EXPECT_FALSE(uncapped->status.at_capped_level_ceiling);

  // Losses apply regardless of caps.
  const auto lost = service_->add_contribution(1, -31000);
  ASSERT_TRUE(lost.has_value());
  EXPECT_EQ(lost->status.record.contribution, 500);
  EXPECT_FALSE(lost->status.at_capped_level_ceiling);
}

TEST_F(CompanyServiceTest, JoinAndChangeHonorTheCooldownRequirementAndPenalty) {
  EXPECT_EQ(service_->join(1, 9).error, x5::ec::Company_InvalidCmsId);
  EXPECT_EQ(service_->change(1, kBlueSalt).error, x5::ec::Company_IsNotJoined);

  const auto joined = service_->join(1, kAntiqus);
  ASSERT_EQ(joined.error, x5::ec::Success);
  EXPECT_EQ(joined.status.position_level, 1);
  EXPECT_EQ(joined.status.record.join_epoch_ticks, now_);
  EXPECT_EQ(service_->join(1, kBlueSalt).error, x5::ec::Company_IsNotJoinable_AlreadyJoined);

  ASSERT_TRUE(service_->add_contribution(1, 5000));
  EXPECT_EQ(service_->change(1, kAntiqus).error, x5::ec::Company_IsNotJoinable_AlreadyJoined);
  EXPECT_EQ(service_->change(1, kBlueSalt).error, x5::ec::Company_IsNotJoinable_ChangeLimit);

  now_ += 1440 * kTicksPerMinute;
  const auto changed = service_->change(1, kBlueSalt);
  ASSERT_EQ(changed.error, x5::ec::Success);
  EXPECT_EQ(changed.status.record.company_type, kBlueSalt);
  // 5000 contribution is Antiqus level 3, whose change penalty is 3000 in these rules.
  EXPECT_EQ(changed.status.record.contribution, 2000);
  EXPECT_EQ(changed.status.record.unlocked_position_level, 0);

  now_ += 1440 * kTicksPerMinute;
  EXPECT_EQ(service_->change(1, kAntiqus).error, x5::ec::Company_InsufficientContribution);

  rebuild();
  EXPECT_EQ(service_->status(1).record, changed.status.record);
}

TEST_F(CompanyServiceTest, CharacterExperienceBecomesContributionWithARemainder) {
  EXPECT_FALSE(service_->record_character_experience(1, 1000).has_value());
  ASSERT_EQ(service_->join(1, kAntiqus).error, x5::ec::Success);

  EXPECT_FALSE(service_->record_character_experience(1, 19).has_value());
  const auto first = service_->record_character_experience(1, 21);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->contribution, 2);
  EXPECT_TRUE(first->position_changed);
  EXPECT_EQ(first->status.position_level, 2);

  now_ += kEpochTicksPerSecond;
  const auto second = service_->record_character_experience(1, 20);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->status.record.contribution, 3);
  EXPECT_FALSE(second->position_changed);
  EXPECT_EQ(second->status.record.activity_epoch_ticks, now_);
}

TEST_F(CompanyServiceTest, PioneerPermitsRegisterOnceAndExtendInsideTheRenewalWindow) {
  EXPECT_EQ(service_->plan_permit(1, false).error, x5::ec::Company_IsNotJoined);
  seat_in_company(*service_, 1, 2);

  EXPECT_EQ(service_->plan_permit(1, true).error, x5::ec::PioneerPermit_NotExists);
  const auto plan = service_->plan_permit(1, false);
  ASSERT_EQ(plan.error, x5::ec::Success);
  EXPECT_EQ(plan.money_value, 10);
  EXPECT_EQ(plan.new_expiry_epoch_ticks, now_ + 4320 * kTicksPerMinute);
  ASSERT_EQ(service_->commit_permit(1, plan.new_expiry_epoch_ticks).error, x5::ec::Success);
  EXPECT_TRUE(service_->status(1).permit_valid);

  EXPECT_EQ(service_->plan_permit(1, false).error, x5::ec::PioneerPermit_Valid);
  EXPECT_EQ(service_->plan_permit(1, true).error, x5::ec::PioneerPermit_IsNotRenewableTime);

  now_ = plan.new_expiry_epoch_ticks - 60 * kTicksPerMinute;
  const auto extension = service_->plan_permit(1, true);
  ASSERT_EQ(extension.error, x5::ec::Success);
  EXPECT_EQ(extension.new_expiry_epoch_ticks,
            plan.new_expiry_epoch_ticks + 4320 * kTicksPerMinute);

  now_ = plan.new_expiry_epoch_ticks + 1;
  EXPECT_FALSE(service_->status(1).permit_valid);
  EXPECT_EQ(service_->plan_permit(1, true).error, x5::ec::PioneerPermit_Expired);
  EXPECT_EQ(service_->plan_permit(1, false).error, x5::ec::Success);
}

TEST(CompanyPermitCharge, DeductsTheCostAndReportsTheWalletChange) {
  WorldProgress before;
  before.revision = 4;
  before.gold = 25;

  PermitPlan free_plan;
  const auto free_charge = charge_permit(before, free_plan);
  EXPECT_EQ(free_charge.error, x5::ec::Success);
  auto expected_free = before;
  ++expected_free.revision;
  EXPECT_EQ(free_charge.after, expected_free);
  EXPECT_FALSE(free_charge.items_update.has_value());

  PermitPlan plan;
  plan.money_value = 10;
  const auto charge = charge_permit(before, plan);
  ASSERT_EQ(charge.error, x5::ec::Success);
  EXPECT_EQ(charge.after.gold, 15);
  EXPECT_EQ(charge.after.revision, 5);
  ASSERT_TRUE(charge.items_update.has_value());
  EXPECT_EQ(charge.items_update->opcode, kItemsUpdateNotificationOpcode);
  EXPECT_TRUE(charge.items_update->message.contains("company_pioneer_permit"));
  EXPECT_EQ(charge.items_update->message["money_changes"][0]["result"], "15");

  plan.money_value = 26;
  EXPECT_EQ(charge_permit(before, plan).error, x5::ec::PioneerPermit_NotEnoughMoneyForRegister);
}

class FailingCompanyStore final : public CompanyStore {
public:
  std::vector<CompanyRecord> load(int) override {
    return {};
  }

  void save(const CompanyRecord &) override {
    throw std::runtime_error("database unavailable");
  }
};

class PermitTransactionTestStore final : public CompanyStore {
public:
  std::vector<CompanyRecord> load(int) override {
    return record ? std::vector<CompanyRecord>{*record} : std::vector<CompanyRecord>{};
  }

  void save(const CompanyRecord &value) override {
    record = value;
  }

  bool commit_permit(std::int64_t, int, const CompanyRecord &expected,
                     const CompanyRecord &next, const WorldProgress &before,
                     const WorldProgress &after) override {
    if (fail || !record || *record != expected || progress != before) {
      return false;
    }
    record = next;
    progress = after;
    return true;
  }

  bool fail{};
  std::optional<CompanyRecord> record;
  WorldProgress progress;
};

TEST(CompanyServiceStorage, PermitFailureAndRetryPreserveCurrencyAndDoNotDoubleCharge) {
  auto store = std::make_shared<PermitTransactionTestStore>();
  CompanyService service(store, testing_support::company_rules(), kWorldId,
                         [] { return kStartTicks; });
  seat_in_company(service, 1, 2);
  store->progress.gold = 25;
  const auto before = store->progress;
  const auto plan = service.plan_permit(1, false);
  ASSERT_EQ(plan.error, x5::ec::Success);
  const auto charge = charge_permit(before, plan);
  ASSERT_EQ(charge.error, x5::ec::Success);

  store->fail = true;
  EXPECT_EQ(service.commit_paid_permit(10, plan, before, charge.after).error,
            x5::ec::SystemError);
  EXPECT_EQ(store->progress, before);
  EXPECT_EQ(store->record->permit_expiry_epoch_ticks, 0);
  EXPECT_FALSE(service.status(1).permit_valid);

  store->fail = false;
  EXPECT_EQ(service.commit_paid_permit(10, plan, before, charge.after).error, x5::ec::Success);
  EXPECT_EQ(store->progress.gold, 15);
  EXPECT_EQ(service.commit_paid_permit(10, plan, before, charge.after).error,
            x5::ec::SystemError);
  EXPECT_EQ(store->progress.gold, 15);
  EXPECT_EQ(store->progress.revision, charge.after.revision);
}

TEST(CompanyServiceStorage, MemoryPermitRejectsStaleProgressAndCommitsBothRecords) {
  MemoryRepositories characters;
  Account account;
  account.name = "permit_memory";
  ASSERT_TRUE(characters.accounts->add_account(account));
  const auto owner = characters.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character created;
  created.account_id = owner->id;
  created.name = "MemoryPermit";
  const auto character = characters.characters->create(created);

  auto store = std::make_shared<MemoryCompanyStore>(
      characters.character_state);
  CompanyService service(store, testing_support::company_rules(), kWorldId,
                         [] { return kStartTicks; });
  ASSERT_EQ(service.join(character.id, kAntiqus).error, x5::ec::Success);
  auto initial = characters.character_state->load_world_progress(owner->id, character.id, kWorldId);
  auto funded = initial;
  funded.gold = 25;
  ++funded.revision;
  ASSERT_TRUE(characters.character_state->save_world_progress(owner->id, character.id, kWorldId,
                                                              initial, funded));
  const auto plan = service.plan_permit(character.id, false);

  auto stale = funded;
  --stale.revision;
  const auto stale_charge = charge_permit(stale, plan);
  EXPECT_EQ(service.commit_paid_permit(owner->id, plan, stale, stale_charge.after).error,
            x5::ec::SystemError);
  EXPECT_EQ(characters.character_state->load_world_progress(owner->id, character.id, kWorldId),
            funded);
  EXPECT_FALSE(service.status(character.id).permit_valid);

  const auto charge = charge_permit(funded, plan);
  ASSERT_EQ(service.commit_paid_permit(owner->id, plan, funded, charge.after).error,
            x5::ec::Success);
  EXPECT_EQ(characters.character_state->load_world_progress(owner->id, character.id, kWorldId),
            charge.after);
  EXPECT_TRUE(service.status(character.id).permit_valid);
  EXPECT_EQ(service.commit_paid_permit(owner->id, plan, funded, charge.after).error,
            x5::ec::SystemError);
}

TEST(CompanyServiceStorage, StoreFailuresLeaveTheRecordUnchanged) {
  CompanyService service(std::make_shared<FailingCompanyStore>(),
                         testing_support::company_rules(), kWorldId);
  EXPECT_EQ(service.join(1, kAntiqus).error, x5::ec::SystemError);
  EXPECT_EQ(service.status(1).record.company_type, kCompanyNone);
  EXPECT_THROW(service.set_record(CompanyRecord{1, 9}), std::invalid_argument);
}

// The proxy forwards all reads to the direct database and drops the acknowledgement only
// for the settlement transaction. The committed row is then reconciled through a fresh fence.
class CompanyCommitProxyStore final : public CompanyStore {
public:
  CompanyCommitProxyStore(std::shared_ptr<CompanyStore> direct,
                          std::shared_ptr<CompanyStore> proxy)
      : direct_(std::move(direct)), proxy_(std::move(proxy)) {
  }

  std::vector<CompanyRecord> load(int world_id) override {
    return direct_->load(world_id);
  }

  void save(const CompanyRecord &record) override {
    direct_->save(record);
  }

  CompanyQuestSettlementResult read_quest_effect(const QuestServiceEffectEvent &event) override {
    return direct_->read_quest_effect(event);
  }

  CompanyQuestSettlementResult settle_quest_effect(const QuestServiceEffectEvent &event,
                                                    const CompanyRecord &before,
                                                    const CompanyQuestPlanner &planner) override {
    return proxy_->settle_quest_effect(event, before, planner);
  }

private:
  std::shared_ptr<CompanyStore> direct_;
  std::shared_ptr<CompanyStore> proxy_;
};

QuestServiceEffectEvent insert_company_effect(const std::string &connection_string,
                                              const Character &character,
                                              const QuestServiceMembership &membership,
                                              char event_digit, std::int64_t experience,
                                              std::int64_t rate, std::int64_t contribution) {
  QuestServiceEffectEvent event;
  event.account_id = character.account_id;
  event.character_id = character.id;
  event.world_id = kWorldId;
  event.effect_id = std::string(32, event_digit);
  event.mutation_fingerprint = std::string(64, 'e');
  event.effects.expected_company_type = membership.company_type;
  event.effects.expected_company_generation = membership.company_generation;
  event.effects.expected_guild_id = membership.guild_id;
  event.effects.expected_guild_generation = membership.guild_generation;
  event.effects.activity_experience = experience;
  event.effects.activity_company_rate = rate;
  event.effects.company_contribution = contribution;
  event.guild_applied = true;
  pqxx::connection connection(connection_string);
  pqxx::work transaction(connection);
  const auto rows = transaction.exec(
      "INSERT INTO emulator_quest_service_effects "
      "(character_id,account_id,world_id,effect_id,mutation_fingerprint,monster_receipt,"
      "effect_payload,expected_company_type,expected_guild_id,expected_company_generation,"
      "expected_guild_generation,granted_items,company_applied,guild_applied) "
      "VALUES ($1,$2,$3,$4,$5,false,$6::jsonb,$7,$8,$9,$10,'[]'::jsonb,false,true) "
      "RETURNING enqueue_sequence",
      pqxx::params{event.character_id, event.account_id, event.world_id, event.effect_id,
                   event.mutation_fingerprint, encode_quest_service_effects(event.effects).dump(),
                   event.effects.expected_company_type, event.effects.expected_guild_id,
                   event.effects.expected_company_generation, event.effects.expected_guild_generation});
  event.enqueue_sequence = rows.front()[0].as<std::uint64_t>();
  transaction.commit();
  return event;
}

TEST(CompanyServiceStorage, PostgresCompanyQuestLostCommitAcknowledgementIsReconciledOnce) {
  const char *direct_url = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  const char *proxy_url = std::getenv("CHRONICLES_COMPANY_PROXY_TEST_DATABASE_URL");
  if (!direct_url || !proxy_url) {
    GTEST_SKIP() << "Requires the disposable PostgreSQL lost-ack proxy";
  }
  {
    pqxx::connection connection(direct_url);
    pqxx::read_transaction transaction(connection);
    const auto identity = transaction.exec1("SELECT current_database(), host(inet_server_addr())");
    ASSERT_EQ(identity[0].as<std::string>(), "company_quest_test");
    ASSERT_EQ(identity[1].as<std::string>(), "127.0.0.1");
  }
  auto repositories = make_postgres_character_repositories(direct_url);
  const auto epoch = repositories.character_state->acquire_monster_reward_owner_epoch();
  Account account;
  account.name = "company_actual_lost_ack";
  ASSERT_TRUE(repositories.accounts->add_account(account));
  const auto owner = repositories.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character candidate;
  candidate.account_id = owner->id;
  candidate.name = "CompanyLostAck";
  const auto character = repositories.characters->create(candidate);
  auto direct_store = std::make_shared<PostgresCompanyStore>(direct_url);
  direct_store->adopt_quest_owner_epoch(epoch);
  CompanyService initial(direct_store, testing_support::company_rules(), kWorldId,
                         [] { return kStartTicks; });
  ASSERT_EQ(initial.join(character.id, kBlueSalt).error, x5::ec::Success);
  const auto membership = repositories.character_state->read_quest_service_membership(
      character.account_id, character.id, kWorldId);
  ASSERT_TRUE(membership);
  const auto event = insert_company_effect(direct_url, character, *membership, 'a', 17, 10, 3);
  auto proxy_store = std::make_shared<PostgresCompanyStore>(proxy_url);
  proxy_store->adopt_quest_owner_epoch(epoch);
  auto faulted_store = std::make_shared<CompanyCommitProxyStore>(direct_store, proxy_store);
  CompanyService service(faulted_store, testing_support::company_rules(), kWorldId,
                         [] { return kStartTicks; });
  EXPECT_THROW(service.settle_quest_effect(event), std::exception);
  EXPECT_THROW(service.status(character.id), std::runtime_error);
  const auto recovered = service.reconcile_uncertain_quest_effect();
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->status, QuestEffectSettlementStatus::already_applied);
  EXPECT_EQ(service.status(character.id).record.contribution, 4);

  {
    pqxx::connection connection(direct_url);
    pqxx::read_transaction transaction(connection);
    const auto rows = transaction.exec(
        "SELECT c.contribution,r.remainder,e.company_applied FROM emulator_character_company c "
        "JOIN emulator_quest_activity_remainders r ON r.character_id=c.character_id "
        "AND r.world_id=$1 AND r.component=0 AND r.membership_generation=$2 "
        "AND r.conversion_rate=10 JOIN emulator_quest_service_effects e "
        "ON e.character_id=c.character_id AND e.effect_id=$3 WHERE c.character_id=$4",
        pqxx::params{kWorldId, membership->company_generation, event.effect_id, character.id});
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows.front()[0].as<std::int64_t>(), 4);
    EXPECT_EQ(rows.front()[1].as<std::int64_t>(), 7);
    EXPECT_TRUE(rows.front()[2].as<bool>());
  }
  CompanyService restored(direct_store, testing_support::company_rules(), kWorldId,
                          [] { return kStartTicks; });
  EXPECT_EQ(restored.settle_quest_effect(event).status,
            QuestEffectSettlementStatus::already_applied);
  EXPECT_EQ(restored.status(character.id).record.contribution, 4);
  const auto next = insert_company_effect(direct_url, character, *membership, 'b', 4, 10, 0);
  EXPECT_EQ(restored.settle_quest_effect(next).status, QuestEffectSettlementStatus::applied);
  EXPECT_EQ(restored.status(character.id).record.contribution, 5);
}

// The disposable harness launches this in a separate process after the lost-ack test.
TEST(CompanyServiceStorage, PostgresCompanyActivityRemaindersSurviveProcessRestart) {
  const char *direct_url = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!direct_url) {
    GTEST_SKIP() << "Requires the disposable PostgreSQL restart harness";
  }
  {
    pqxx::connection connection(direct_url);
    pqxx::read_transaction transaction(connection);
    const auto identity = transaction.exec1("SELECT current_database(), host(inet_server_addr())");
    ASSERT_EQ(identity[0].as<std::string>(), "company_quest_test");
    ASSERT_EQ(identity[1].as<std::string>(), "127.0.0.1");
  }
  auto repositories = make_postgres_character_repositories(direct_url);
  const auto epoch = repositories.character_state->acquire_monster_reward_owner_epoch();
  const auto owner = repositories.accounts->account("company_actual_lost_ack");
  ASSERT_TRUE(owner);
  const auto characters = repositories.characters->characters(owner->id, kWorldId);
  ASSERT_EQ(characters.size(), 1U);
  const auto character = characters.front();
  auto store = std::make_shared<PostgresCompanyStore>(direct_url);
  store->adopt_quest_owner_epoch(epoch);
  CompanyService service(store, testing_support::company_rules(), kWorldId,
                         [] { return kStartTicks; });
  ASSERT_EQ(service.status(character.id).record.contribution, 5);
  auto membership = repositories.character_state->read_quest_service_membership(
      character.account_id, character.id, kWorldId);
  ASSERT_TRUE(membership);
  const auto rate_event = insert_company_effect(direct_url, character, *membership, 'c', 19, 20, 0);
  EXPECT_EQ(service.settle_quest_effect(rate_event).status, QuestEffectSettlementStatus::applied);
  EXPECT_EQ(service.status(character.id).record.contribution, 5);
  auto record = service.status(character.id).record;
  record.company_type = 0;
  ASSERT_EQ(service.set_record(record).error, x5::ec::Success);
  record.company_type = kBlueSalt;
  ASSERT_EQ(service.set_record(record).error, x5::ec::Success);
  const auto rejoined = repositories.character_state->read_quest_service_membership(
      character.account_id, character.id, kWorldId);
  ASSERT_TRUE(rejoined);
  ASSERT_NE(membership->company_generation, rejoined->company_generation);
  const auto new_event = insert_company_effect(direct_url, character, *rejoined, 'd', 5, 20, 0);
  EXPECT_EQ(service.settle_quest_effect(new_event).status, QuestEffectSettlementStatus::applied);
  EXPECT_EQ(service.status(character.id).record.contribution, 5);
  pqxx::connection connection(direct_url);
  pqxx::read_transaction transaction(connection);
  const auto remainders = transaction.exec(
      "SELECT membership_generation,conversion_rate,remainder "
      "FROM emulator_quest_activity_remainders WHERE world_id=$1 AND character_id=$2 "
      "AND component=0 ORDER BY membership_generation,conversion_rate",
      pqxx::params{kWorldId, character.id});
  ASSERT_EQ(remainders.size(), 3U);
  EXPECT_EQ(remainders[0][2].as<std::int64_t>(), 1);
  EXPECT_EQ(remainders[1][2].as<std::int64_t>(), 19);
  EXPECT_EQ(remainders[2][2].as<std::int64_t>(), 5);
}

TEST(CompanyServiceStorage, PostgresRecordsSurviveReconstruction) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Requires the isolated PostgreSQL test cluster";
  }

  auto characters = make_postgres_character_repositories(connection);
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  Account account;
  account.name = "company_" + suffix;
  ASSERT_TRUE(characters.accounts->add_account(account));
  const auto owner = characters.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character created;
  created.account_id = owner->id;
  created.name = "Comp" + suffix.substr(suffix.size() - 10);
  const auto character = characters.characters->create(created);

  const auto epoch = characters.character_state->acquire_monster_reward_owner_epoch();
  auto company_store = std::make_shared<PostgresCompanyStore>(connection);
  company_store->adopt_quest_owner_epoch(epoch);
  CompanyService service(company_store, testing_support::company_rules(), kWorldId);
  ASSERT_EQ(service.join(character.id, kAntiqus).error, x5::ec::Success);
  ASSERT_TRUE(service.add_contribution(character.id, 3001));
  ASSERT_EQ(service.commit_permit(character.id, 12345).error, x5::ec::Success);

  auto restored_store = std::make_shared<PostgresCompanyStore>(connection);
  restored_store->adopt_quest_owner_epoch(epoch);
  CompanyService restored(restored_store, testing_support::company_rules(), kWorldId);
  const auto status = restored.status(character.id);
  EXPECT_EQ(status.record.company_type, kAntiqus);
  EXPECT_EQ(status.record.contribution, 3001);
  EXPECT_EQ(status.record.permit_expiry_epoch_ticks, 12345);
  EXPECT_EQ(status.position_level, 3);
}

TEST(CompanyServiceStorage, PostgresPermitAndCurrencyCommitTogether) {
  const char *connection = std::getenv("CHRONICLES_TUTORIAL_TEST_DATABASE_URL");
  if (!connection) {
    GTEST_SKIP() << "Requires the isolated PostgreSQL test cluster";
  }

  auto characters = make_postgres_character_repositories(connection);
  const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  Account account;
  account.name = "permit_" + suffix;
  ASSERT_TRUE(characters.accounts->add_account(account));
  const auto owner = characters.accounts->account(account.name);
  ASSERT_TRUE(owner);
  Character created;
  created.account_id = owner->id;
  created.name = "Perm" + suffix.substr(suffix.size() - 10);
  const auto character = characters.characters->create(created);

  const auto epoch = characters.character_state->acquire_monster_reward_owner_epoch();
  auto company_store = std::make_shared<PostgresCompanyStore>(connection);
  company_store->adopt_quest_owner_epoch(epoch);
  CompanyService service(company_store, testing_support::company_rules(), kWorldId,
                         [] { return kStartTicks; });
  ASSERT_EQ(service.join(character.id, kAntiqus).error, x5::ec::Success);
  auto progress =
      characters.character_state->load_world_progress(owner->id, character.id, kWorldId);
  auto funded = progress;
  funded.gold = 25;
  ++funded.revision;
  ASSERT_TRUE(characters.character_state->save_world_progress(owner->id, character.id, kWorldId,
                                                              progress, funded));

  const auto plan = service.plan_permit(character.id, false);
  const auto charge = charge_permit(funded, plan);
  ASSERT_EQ(charge.error, x5::ec::Success);
  auto stale = funded;
  --stale.revision;
  auto stale_charge = charge_permit(stale, plan);
  EXPECT_EQ(service.commit_paid_permit(owner->id, plan, stale, stale_charge.after).error,
            x5::ec::SystemError);
  EXPECT_EQ(characters.character_state->load_world_progress(owner->id, character.id, kWorldId),
            funded);
  EXPECT_FALSE(service.status(character.id).permit_valid);

  ASSERT_EQ(service.commit_paid_permit(owner->id, plan, funded, charge.after).error,
            x5::ec::Success);
  EXPECT_EQ(characters.character_state->load_world_progress(owner->id, character.id, kWorldId),
            charge.after);
  auto restored_store = std::make_shared<PostgresCompanyStore>(connection);
  restored_store->adopt_quest_owner_epoch(epoch);
  CompanyService restored(restored_store, testing_support::company_rules(), kWorldId,
                          [] { return kStartTicks; });
  EXPECT_EQ(restored.status(character.id).record.permit_expiry_epoch_ticks,
            plan.new_expiry_epoch_ticks);
  EXPECT_EQ(service.commit_paid_permit(owner->id, plan, funded, charge.after).error,
            x5::ec::SystemError);
  EXPECT_EQ(characters.character_state->load_world_progress(owner->id, character.id, kWorldId),
            charge.after);
}

Json company_row(const std::string &id) {
  return {{"id", id}, {"requireContributionToChangeCompany", "4000"}};
}

Json position_row(const std::string &company, int level, const std::string &next,
                  const std::string &locks = "False") {
  return {{"company", company},
          {"level", std::to_string(level)},
          {"safeContribution", "False"},
          {"nextLevelContribution", next},
          {"isMaxPositionLevel", locks},
          {"pioneerPermitLifeValueMin", "4320"},
          {"pioneerPermitMoneyType", "Gold"},
          {"pioneerPermitMoneyValue", "10"},
          {"changePenaltyContribution", "3000"}};
}

TEST(CompanyRulesCatalog, ReadsCompaniesPositionsAndGameVariables) {
  TestCatalog fixture(
      {{"Company", {company_row("Antiqus"), company_row("BlueSalt"), company_row("Nobody")}},
       {"CompanyPosition",
        {position_row("Antiqus", 1, "1"), position_row("Antiqus", 2, "3001", "True"),
         position_row("BlueSalt", 1, "1"), position_row("BlueSalt", 3, "11501"),
         position_row("Antiqus", 3, "not-a-number")}},
       {"GameVariable",
        {{{"name", "CompanyChangeLimitMin"}, {"value", "60"}},
         {{"name", "RenewPioneerPermitDurationMinuteBeforeExpiry"}, {"value", "30"}}}}});
  ClientCatalog catalog(fixture.root());
  const auto rules = load_company_rules(catalog);

  ASSERT_EQ(rules.companies.size(), 2u);
  const auto &antiqus = rules.companies.at(kAntiqus);
  ASSERT_EQ(antiqus.positions.size(), 2u);
  EXPECT_TRUE(antiqus.positions[1].locks_advancement);
  EXPECT_EQ(antiqus.required_contribution_to_change, 4000);
  // BlueSalt skips level 2, so it has no valid positions and is not joinable.
  EXPECT_TRUE(rules.companies.at(kBlueSalt).positions.empty());
  EXPECT_EQ(rules.change_cooldown, std::chrono::minutes(60));
  EXPECT_EQ(rules.permit_renewal_window, std::chrono::minutes(30));

  CompanyService service(std::make_shared<MemoryCompanyStore>(), rules, kWorldId);
  EXPECT_EQ(service.join(1, kBlueSalt).error, x5::ec::Company_InvalidCmsId);
  EXPECT_EQ(service.join(1, kAntiqus).error, x5::ec::Success);
}

// Uses the exported catalog when CHRONICLES_CLIENT_CATALOG names it.
TEST(CompanyRulesCatalog, ExportedCatalogMatchesTheDocumentedRules) {
  const auto *catalog_path = std::getenv("CHRONICLES_CLIENT_CATALOG");
  if (!catalog_path) {
    GTEST_SKIP() << "Set CHRONICLES_CLIENT_CATALOG to the exported data/generated/catalog";
  }

  ClientCatalog catalog(catalog_path);
  const auto rules = load_company_rules(catalog);
  ASSERT_EQ(rules.companies.size(), 5u);
  // Fairwind and YokedIron export five positions; the other companies six.
  const std::map<int, std::size_t> position_counts{{1, 6}, {2, 6}, {3, 5}, {4, 6}, {5, 5}};
  for (const auto &[type, company] : rules.companies) {
    EXPECT_EQ(company.positions.size(), position_counts.at(type)) << company.name;
    EXPECT_EQ(company.required_contribution_to_change, 4000) << company.name;
  }
  EXPECT_EQ(rules.change_cooldown, std::chrono::minutes(1440));
  EXPECT_EQ(rules.permit_renewal_window, std::chrono::minutes(1440));

  const auto dialogues = CompanyDialogueIndex::load(catalog);
  EXPECT_GT(dialogues->selection_count(), 0u);
}

class CompanyDialogueTest : public testing::Test {
protected:
  static constexpr int kJoinDialogue = 1401;
  static constexpr int kPermitDialogue = 1430;
  static constexpr int kRecruiterNpc = 145;
  static constexpr int kStrangerNpc = 146;

  CompanyDialogueTest() {
    const std::vector<Json> selections{
        {{"id", "1401"},
         {"sequenceOrder", "1"},
         {"selectOrder", "3"},
         {"dialogueConditionId", "2000"},
         {"playerTaskCase", "CompanyJoin"}},
        {{"id", "1430"},
         {"sequenceOrder", "1"},
         {"selectOrder", "2"},
         {"dialogueConditionId", ""},
         {"playerTaskCase", "PioneerPermitRegist"}},
        {{"id", "1430"},
         {"sequenceOrder", "1"},
         {"selectOrder", "4"},
         {"dialogueConditionId", ""},
         {"playerTaskCase", "Quest"}}};
    const std::vector<Json> dialogues{
        {{"id", "1401"}, {"speakerName", "Npc"}, {"speakerNameValue", "145"}},
        {{"id", "1430"}, {"speakerName", "Npc"}, {"speakerNameValue", "145"}}};
    const std::vector<Json> conditions{
        {{"conditionId", "2000"},
         {"typeCase", "SystemCompany"},
         {"type", R"("companyType":"EX5CompanyType_BlueSalt","companyDialogueConditionType":)"
                  R"("EX5CompanyDialogueConditionType_Join")"}}};
    const std::vector<Json> npcs{{{"id", "145"}, {"company", "Antiqus"}},
                                 {{"id", "146"}, {"company", "BlueSalt"}}};
    index_ = CompanyDialogueIndex::from_rows(selections, dialogues, conditions, npcs);
  }

  std::shared_ptr<const CompanyDialogueIndex> index_;
};

TEST_F(CompanyDialogueTest, ResolvesTheTaskAndCompanyOfASpokenSelection) {
  EXPECT_EQ(index_->selection_count(), 2u);

  const auto join = index_->resolve(kJoinDialogue, 1, 3, kRecruiterNpc);
  ASSERT_EQ(join.error, x5::ec::Success);
  EXPECT_EQ(join.task, CompanyDialogueTask::join);
  // The selection's SystemCompany condition wins over the speaker's own company.
  EXPECT_EQ(join.company_type, kBlueSalt);

  const auto permit = index_->resolve(kPermitDialogue, 1, 2, kRecruiterNpc);
  ASSERT_EQ(permit.error, x5::ec::Success);
  EXPECT_EQ(permit.task, CompanyDialogueTask::permit_register);
  EXPECT_EQ(permit.company_type, kAntiqus);
}

TEST_F(CompanyDialogueTest, RefusesUnknownSelectionsAndOtherSpeakers) {
  EXPECT_EQ(index_->resolve(kPermitDialogue, 1, 4, kRecruiterNpc).error,
            x5::ec::Dialouge_InvalidSelectionData);
  EXPECT_EQ(index_->resolve(kJoinDialogue, 1, 3, std::nullopt).error,
            x5::ec::Dialogue_InvalidTarget);
  EXPECT_EQ(index_->resolve(kJoinDialogue, 1, 3, kStrangerNpc).error,
            x5::ec::Dialogue_InvalidTarget);
}

// Devicas (NPC 1001175, company None) issues permits for every company through dialogue 10500,
// whose selections carry only PioneerPermit conditions. Live 2026-11-03: the selection was
// refused with Company_InvalidCmsId, so an expired permit could not be renewed.
TEST(CompanyDialogueIndexTest, PermitDeskWithoutACompanyIssuesTheCharactersOwnCompanyPermit) {
  constexpr int kPermitDesk = 10500;
  constexpr int kDevicas = 1001175;
  const std::vector<Json> selections{
      {{"id", "10500"},
       {"sequenceOrder", "1"},
       {"selectOrder", "2"},
       {"dialogueConditionId", "2031"},
       {"playerTaskCase", "PioneerPermitRegist"}},
      {{"id", "10500"},
       {"sequenceOrder", "1"},
       {"selectOrder", "3"},
       {"dialogueConditionId", "2030"},
       {"playerTaskCase", "PioneerPermitExtend"}},
      {{"id", "10500"},
       {"sequenceOrder", "1"},
       {"selectOrder", "5"},
       {"dialogueConditionId", ""},
       {"playerTaskCase", "CompanyJoin"}}};
  const std::vector<Json> dialogues{
      {{"id", "10500"}, {"speakerName", "Npc"}, {"speakerNameValue", "1001175"}}};
  const std::vector<Json> conditions{
      {{"conditionId", "2031"}, {"typeCase", "PioneerPermit"}, {"type", R"("hasPermit":false)"}},
      {{"conditionId", "2030"}, {"typeCase", "PioneerPermit"}, {"type", R"("hasPermit":true)"}}};
  const std::vector<Json> npcs{{{"id", "1001175"}, {"company", "None"}}};
  const auto index = CompanyDialogueIndex::from_rows(selections, dialogues, conditions, npcs);

  const auto registered = index->resolve(kPermitDesk, 1, 2, kDevicas);
  ASSERT_EQ(registered.error, x5::ec::Success);
  EXPECT_EQ(registered.task, CompanyDialogueTask::permit_register);
  EXPECT_EQ(registered.company_type, 0);

  const auto extended = index->resolve(kPermitDesk, 1, 3, kDevicas);
  ASSERT_EQ(extended.error, x5::ec::Success);
  EXPECT_EQ(extended.task, CompanyDialogueTask::permit_extend);

  // A join still needs to know which company it joins.
  EXPECT_EQ(index->resolve(kPermitDesk, 1, 5, kDevicas).error, x5::ec::Company_InvalidCmsId);
}

TEST(CompanyWorldPackets, CompanyMessagesCarryTheWorldCharCompany) {
  CompanyRecord record;
  record.char_id = 1;
  record.company_type = kBlueSalt;
  record.contribution = 42;
  record.join_epoch_ticks = 100;
  const auto packet = company_packet(company_world::kCompanyUpdateNotification, record);
  EXPECT_EQ(packet.opcode, company_world::kCompanyUpdateNotification);
  EXPECT_EQ(packet.message["company"]["type"], "EX5CompanyType_BlueSalt");
  EXPECT_EQ(packet.message["company"]["contribution"], "42");

  const auto empty = world_char_company(CompanyRecord{});
  EXPECT_EQ(empty.type(), x5::cms::EX5CompanyType_None);
  EXPECT_FALSE(empty.has_contribution());
  EXPECT_EQ(company_type_name(kAntiqus), "EX5CompanyType_Antiqus");
  EXPECT_TRUE(company_type_name(0).empty());

  const auto refused = company_error_packet(7, x5::ec::Company_IsNotJoined);
  EXPECT_EQ(refused.opcode, company_world::kCommonErrorResponse);
  EXPECT_EQ(refused.message["category"], "EErrorCategory_Company");
  EXPECT_EQ(refused.message["packet_seq"]["seq"], "7");
}

TEST(SocialGmCommands, ParsesTheTextGrammar) {
  const auto parse = [](const std::string &text) {
    return parse_social_gm_request(kGmCommandTextRequestOpcode, Json{{"text", text}});
  };

  EXPECT_FALSE(parse("item_add 1 2").has_value());
  EXPECT_FALSE(parse("").has_value());

  const auto experience = parse("guild_exp 500");
  ASSERT_TRUE(experience && experience->error.empty());
  EXPECT_EQ(experience->commands.front().kind, SocialGmCommandKind::guild_experience);
  EXPECT_EQ(experience->commands.front().amount, 500);

  const auto master = parse("guild_master 12 3");
  ASSERT_TRUE(master && master->error.empty());
  EXPECT_EQ(master->commands.front().char_id, 12);
  EXPECT_EQ(master->commands.front().guild_id, 3);

  const auto company = parse("company bluesalt 4000");
  ASSERT_TRUE(company && company->error.empty());
  EXPECT_EQ(company->commands.front().company_type, kBlueSalt);
  EXPECT_EQ(company->commands.front().contribution_delta, 4000);

  const auto numeric = parse("company 1");
  ASSERT_TRUE(numeric && numeric->error.empty());
  EXPECT_EQ(numeric->commands.front().company_type, kAntiqus);

  for (const auto *invalid : {"guild_exp 0", "guild_exp 1x", "guild_level", "company none",
                              "company_position 101", "pioneer_permit -1", "guild_master 0"}) {
    const auto refused = parse(invalid);
    ASSERT_TRUE(refused.has_value()) << invalid;
    EXPECT_FALSE(refused->error.empty()) << invalid;
  }
}

TEST(SocialGmCommands, ParsesStructuredCommandsAndRefusesMixedRequests) {
  // Entries are built one at a time so nlohmann::json cannot mistake an entry for a pair.
  const Json level_entry = {{"guild_level", {{"level", 3}}}};
  const Json company_entry = {{"company",
                               {{"type", "EX5CompanyType_Antiqus"},
                                {"add_contribution", "250"},
                                {"pioneer_permit", 0},
                                {"unlock_max_position", true}}}};
  const Json money_entry = {{"money", {{"value", 1}}}};
  const auto request = [](std::vector<Json> entries) {
    Json message = Json::object();
    message["cmds"] = Json::array();
    for (auto &entry : entries) {
      message["cmds"].push_back(std::move(entry));
    }
    return message;
  };

  const auto structured = request({level_entry, company_entry});
  const auto parsed = parse_social_gm_request(kGmCommandRequestOpcode, structured);
  ASSERT_TRUE(parsed && parsed->error.empty());
  ASSERT_EQ(parsed->commands.size(), 2u);
  EXPECT_EQ(parsed->commands[0].amount, 3);
  EXPECT_EQ(parsed->commands[1].company_type, kAntiqus);
  EXPECT_EQ(parsed->commands[1].contribution_delta, 250);
  EXPECT_EQ(parsed->commands[1].permit_minutes, 0);
  EXPECT_TRUE(parsed->commands[1].unlock_level.has_value());

  const auto refused =
      parse_social_gm_request(kGmCommandRequestOpcode, request({level_entry, money_entry}));
  ASSERT_TRUE(refused.has_value());
  EXPECT_FALSE(refused->error.empty());

  EXPECT_FALSE(
      parse_social_gm_request(kGmCommandRequestOpcode, request({money_entry})).has_value());
}

// Lobby, selection and deletion views through AuthService with companies and guilds attached.
class CharacterSocialTest : public testing::Test {
protected:
  struct Player {
    std::string token;
    std::int64_t character_id{};
    std::string name;
  };

  CharacterSocialTest() {
    auth_.set_character_social_state(
        std::make_shared<CharacterSocialDirectory>(companies_, guilds_, nullptr));
  }

  Player player(const std::string &name) {
    if (!auth_.create_account(name, "social-test-password")) {
      throw std::runtime_error("Cannot provision test account");
    }

    Player result;
    result.name = name;
    result.token = auth_.login(name, "social-test-password").access_token();
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
    return result;
  }

  GuildActor guild_actor(const Player &player) {
    const auto company = companies_->status(player.character_id);
    GuildActor actor;
    actor.world_id = kWorldId;
    actor.char_id = player.character_id;
    actor.name = player.name;
    actor.level = 10;
    actor.company_type = company.record.company_type;
    actor.company_position = company.position_level;
    return actor;
  }

  x5::pb::frontwebsocket::CharDeletePreCheckRes precheck(const Player &player) {
    x5::pb::ws::front::CharDeletePreCheckReq request;
    request.set_char_id(player.character_id);
    return auth_.delete_precheck(player.token, request);
  }

  x5::pb::frontwebsocket::CharDeleteRes remove(const Player &player) {
    x5::pb::ws::front::CharDeleteReq request;
    request.set_char_id(player.character_id);
    request.set_char_name(player.name);
    return auth_.delete_character(player.token, request);
  }

  MemoryRepositories repositories_;
  AuthService auth_{repositories_.character_repositories(), Config{}};
  std::shared_ptr<CompanyService> companies_ = std::make_shared<CompanyService>(
      std::make_shared<MemoryCompanyStore>(), testing_support::company_rules(), kWorldId);
  std::shared_ptr<GuildService> guilds_ =
      std::make_shared<GuildService>(std::make_shared<MemoryGuildStore>(), GuildRules{}, kWorldId);
};

TEST_F(CharacterSocialTest, LobbyAndSelectionShowTheCompanyAndGuild) {
  const auto founder = player("Founder");
  seat_in_company(*companies_, founder.character_id, 3);
  GuildCreateRequest create;
  create.name = "Seafarers";
  create.join_type = 1;
  ASSERT_EQ(guilds_->create(guild_actor(founder), create).error, x5::ec::Success);

  const auto listed = auth_.list(founder.token);
  ASSERT_EQ(listed.char_list_size(), 1);
  EXPECT_EQ(listed.char_list(0).company_type(), x5::cms::EX5CompanyType_Antiqus);
  EXPECT_EQ(listed.char_list(0).company_contribution(), kAntiqusThresholds[2]);

  x5::pb::ws::front::CharSelectReq select;
  select.set_char_id(founder.character_id);
  const auto selected = auth_.select(founder.token, select);
  ASSERT_EQ(selected.ec(), x5::ec::Success);
  EXPECT_EQ(selected.character().company().type(), x5::cms::EX5CompanyType_Antiqus);
  EXPECT_EQ(selected.character().guild().guild_name(), "Seafarers");
  EXPECT_EQ(selected.character().guild().member_grade(), x5::cms::EX5GuildMemberGrade_Master);
}

TEST_F(CharacterSocialTest, DeletionWarnsAndRefusesWhileTheCharacterIsInAGuild) {
  const auto founder = player("Founder");
  const auto member = player("Member");
  const auto loner = player("Loner");
  seat_in_company(*companies_, founder.character_id, 3);
  seat_in_company(*companies_, member.character_id, 3);
  GuildCreateRequest create;
  create.name = "Seafarers";
  create.join_type = 1;
  const auto created = guilds_->create(guild_actor(founder), create);
  ASSERT_EQ(created.error, x5::ec::Success);
  ASSERT_EQ(guilds_->join(guild_actor(member), created.guild.id).error, x5::ec::Success);

  const auto master_check = precheck(founder);
  EXPECT_EQ(master_check.ec(), x5::ec::Success);
  ASSERT_EQ(master_check.block_reasons_size(), 1);
  EXPECT_EQ(master_check.block_reasons(0), x5::pb::frontwebsocket::CHAR_DELETE_BLOCK_GUILD_MASTER);
  EXPECT_EQ(remove(founder).ec(), x5::ec::CharDeleteGuildMaster);

  const auto member_check = precheck(member);
  EXPECT_EQ(member_check.ec(), x5::ec::CharDeleteGuildMember);
  EXPECT_EQ(member_check.block_reasons_size(), 0);
  EXPECT_EQ(remove(member).ec(), x5::ec::CharDeleteGuildMember);

  const auto free_check = precheck(loner);
  EXPECT_EQ(free_check.ec(), x5::ec::Success);
  EXPECT_EQ(free_check.block_reasons_size(), 0);
  // Without blockers the deletion reaches AuthService's own pending-deletion refusal.
  const auto free_delete = remove(loner);
  EXPECT_NE(free_delete.ec(), x5::ec::CharDeleteGuildMember);
  EXPECT_NE(free_delete.ec(), x5::ec::CharDeleteGuildMaster);
}

} // namespace
} // namespace chronicles
