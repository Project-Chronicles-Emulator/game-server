#pragma once

// Delegates ordinary operations to the in-memory repository and pauses one checkpoint.
#include "persistence/character_state_repository.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <utility>

namespace chronicles::native_testing {

class PausingRegionCheckpointStore final : public CharacterStateRepository {
public:
  enum class Outcome { commit, conflict, lost_acknowledgement };

  void lose_next_admin_quarantine_ack() {
    lose_admin_quarantine_ack_.store(true);
  }

  void fail_admin_quarantine_load() {
    fail_admin_quarantine_load_.store(true);
  }

  std::vector<std::int64_t> offline_admin_quarantines() override {
    if (fail_admin_quarantine_load_.exchange(false)) {
      throw std::runtime_error("Injected admin quarantine load failure");
    }
    return inner_->offline_admin_quarantines();
  }

  bool create_offline_admin_quarantine(std::int64_t character_id,
                                        const std::string &token) override {
    const bool created = inner_->create_offline_admin_quarantine(character_id, token);
    if (created && lose_admin_quarantine_ack_.exchange(false)) {
      throw std::runtime_error("Injected lost admin quarantine acknowledgement");
    }
    return created;
  }

  bool clear_offline_admin_quarantine(std::int64_t character_id,
                                       const std::string &token) override {
    return inner_->clear_offline_admin_quarantine(character_id, token);
  }

  explicit PausingRegionCheckpointStore(std::shared_ptr<CharacterStateRepository> inner)
      : inner_(std::move(inner)) {
  }

  // Route one armed quest mutation through a repository connected to the fault proxy.
  void arm_quest_commit_proxy(std::shared_ptr<CharacterStateRepository> proxy) {
    if (!proxy) {
      throw std::invalid_argument("Quest commit proxy is missing");
    }
    quest_commit_proxy_ = std::move(proxy);
    quest_commit_proxy_armed_.store(true);
  }

  // Route one boss-script settlement through the loopback COMMIT-acknowledgement proxy.
  void arm_boss_settlement_proxy(std::shared_ptr<CharacterStateRepository> proxy) {
    if (!proxy) {
      throw std::invalid_argument("Boss settlement proxy is missing");
    }
    boss_settlement_proxy_ = std::move(proxy);
    boss_settlement_proxy_armed_.store(true);
  }

  void arm_boss_publication_proxy(std::shared_ptr<CharacterStateRepository> proxy) {
    if (!proxy) {
      throw std::invalid_argument("Boss publication proxy is missing");
    }
    boss_publication_proxy_ = std::move(proxy);
    boss_publication_proxy_armed_.store(true);
  }

  void arm_death_commit_proxy(std::shared_ptr<CharacterStateRepository> proxy) {
    if (!proxy) {
      throw std::invalid_argument("Death commit proxy is missing");
    }
    death_commit_proxy_ = std::move(proxy);
    death_commit_proxy_armed_.store(true);
  }

  void arm_death_fence_pause() {
    death_fence_pause_armed_.store(true);
  }

  bool wait_for_death_fence_pause() {
    return death_fence_entered_future_.wait_for(std::chrono::seconds(5)) ==
           std::future_status::ready;
  }

  void release_death_fence_pause() {
    if (!death_fence_released_.exchange(true)) {
      death_fence_release_promise_.set_value();
    }
  }

  void refuse_boss_publication() {
    boss_publication_refused_.store(true);
  }

  void allow_boss_publication() {
    boss_publication_refused_.store(false);
  }

  bool saw_refused_boss_publication() const {
    return boss_publication_attempted_.load();
  }

  void fail_next_quest_commit_before_write() {
    fail_quest_commit_before_write_.store(true);
  }

  LootClaimResult commit_gathering_quest_claim(
      std::int64_t account, std::int64_t character, int world, const std::string &receipt_id,
      const std::string &source_fingerprint, const WorldProgress &before,
      const WorldProgress &after, const ItemTrade &trade, const QuestServiceEffects &effects,
      const std::optional<SharedGatheringSourceTransition> &source) override {
    return inner_->commit_gathering_quest_claim(account, character, world, receipt_id,
                                                source_fingerprint, before, after, trade, effects,
                                                source);
  }

  GatheringFinalizeResult finalize_gathering_quest_claim(
      std::int64_t account, std::int64_t character, int world, const std::string &receipt_id,
      const std::string &source_fingerprint, const WorldProgress &before,
      const WorldProgress &after, const ItemTrade &trade, const QuestServiceEffects &effects,
      const std::optional<SharedGatheringSourceTransition> &source,
      std::int64_t confirmed_epoch_ticks) override {
    return inner_->finalize_gathering_quest_claim(account, character, world, receipt_id,
                                                   source_fingerprint, before, after, trade,
                                                   effects, source, confirmed_epoch_ticks);
  }

  std::vector<SharedGatheringSourceRecord> shared_gathering_sources(
      int world, const std::optional<SharedGatheringSourceKey> &after,
      std::size_t limit) override {
    return inner_->shared_gathering_sources(world, after, limit);
  }

  SharedGatheringSourceUpdateResult fence_shared_gathering_source(
      int world, const SharedGatheringSourceTransition &initial) override {
    return inner_->fence_shared_gathering_source(world, initial);
  }

  SharedGatheringSourceUpdateResult update_shared_gathering_source(
      int world, const SharedGatheringSourceTransition &transition,
      std::int64_t epoch_ticks, bool require_due) override {
    return inner_->update_shared_gathering_source(world, transition, epoch_ticks, require_due);
  }

  bool enqueue_monster_rewards_with_script(
      const std::vector<MonsterRewardReceipt> &receipts,
      const std::optional<BossScriptIntent> &script) override {
    return inner_->enqueue_monster_rewards_with_script(receipts, script);
  }

  std::vector<BossScriptIntent> pending_boss_scripts(
      int world, const std::optional<std::string> &after, std::size_t limit) override {
    return inner_->pending_boss_scripts(world, after, limit);
  }

  BossScriptSettlement settle_shared_boss_script(const BossScriptIntent &intent) override {
    auto &target = boss_settlement_proxy_armed_.exchange(false) ? boss_settlement_proxy_ : inner_;
    return target->settle_shared_boss_script(intent);
  }

  bool mark_boss_script_published(const BossScriptIntent &intent) override {
    if (boss_publication_refused_.load()) {
      boss_publication_attempted_.store(true);
      return false;
    }
    auto &target = boss_publication_proxy_armed_.exchange(false) ? boss_publication_proxy_ : inner_;
    return target->mark_boss_script_published(intent);
  }

  bool mark_private_boss_script_applied(const BossScriptIntent &intent) override {
    return inner_->mark_private_boss_script_applied(intent);
  }

  bool retire_private_boss_script(const BossScriptIntent &intent) override {
    return inner_->retire_private_boss_script(intent);
  }

  std::optional<QuestServiceMembership> read_quest_service_membership(
      std::int64_t account, std::int64_t character, int world) override {
    return inner_->read_quest_service_membership(account, character, world);
  }

  std::vector<QuestServiceEffectEvent> pending_quest_service_effects(
      int world, const std::optional<std::int64_t> &character, std::size_t limit,
      std::uint64_t after_sequence = 0) override {
    return inner_->pending_quest_service_effects(world, character, limit, after_sequence);
  }

  std::optional<std::vector<CharacterItem>> commit_unaffiliated_activity(
      std::int64_t account, std::int64_t character, int world,
      const QuestServiceMembership &membership, const WorldProgress &before,
      const WorldProgress &after, const ItemTrade &trade) override {
    if (fail_quest_commit_before_write_.exchange(false)) {
      throw std::runtime_error("Injected failure before unaffiliated commit");
    }
    auto &target = quest_commit_proxy_armed_.exchange(false) ? quest_commit_proxy_ : inner_;
    return target->commit_unaffiliated_activity(account, character, world, membership, before,
                                                 after, trade);
  }

  QuestMutationResult commit_quest_mutation(
      std::int64_t account, std::int64_t character, int world, const std::string &effect_id,
      const WorldProgress &before, const std::optional<WorldProgress> &after,
      const ItemTrade &trade, const QuestServiceEffects &effects) override {
    if (fail_quest_commit_before_write_.exchange(false)) {
      throw std::runtime_error("Injected failure before quest commit");
    }
    auto &target = quest_commit_proxy_armed_.exchange(false) ? quest_commit_proxy_ : inner_;
    return target->commit_quest_mutation(account, character, world, effect_id, before, after,
                                         trade, effects);
  }

  MonsterRewardCommitResult commit_monster_reward_with_effects(
      std::int64_t account, std::int64_t character, int world, const std::string &receipt_id,
      const WorldProgress &before, const std::optional<WorldProgress> &after,
      const ItemTrade &trade, const QuestServiceEffects &effects) override {
    if (fail_quest_commit_before_write_.exchange(false)) {
      throw std::runtime_error("Injected failure before monster quest commit");
    }
    auto &target = quest_commit_proxy_armed_.exchange(false) ? quest_commit_proxy_ : inner_;
    return target->commit_monster_reward_with_effects(account, character, world, receipt_id,
                                                       before, after, trade, effects);
  }

  ~PausingRegionCheckpointStore() override {
    release();
    release_quest_write();
    release_death_fence_pause();
  }

  void arm(std::int64_t character_id, Outcome outcome) {
    target_character_ = character_id;
    outcome_ = outcome;
    armed_.store(true);
  }

  bool wait_until_blocked() {
    return entered_future_.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  }

  void release() {
    if (!released_.exchange(true)) {
      release_promise_.set_value();
    }
  }

  void refuse_monster_rewards() {
    rewards_refused_.store(true);
  }

  bool wait_for_refused_monster_reward() {
    return reward_refused_future_.wait_for(std::chrono::seconds(5)) ==
           std::future_status::ready;
  }

  void allow_monster_rewards() {
    rewards_refused_.store(false);
  }

  void lose_next_monster_commit_acknowledgement(std::int64_t character_id) {
    reward_commit_character_ = character_id;
    lose_reward_commit_ack_.store(true);
  }

  bool wait_for_lost_monster_commit_acknowledgement() {
    return reward_commit_future_.wait_for(std::chrono::seconds(5)) ==
           std::future_status::ready;
  }

  void lose_next_quest_write_acknowledgement(std::int64_t character_id, bool commit_first) {
    quest_write_character_ = character_id;
    quest_write_commit_first_ = commit_first;
    lose_quest_write_ack_.store(true);
  }

  bool wait_for_lost_quest_write_acknowledgement() {
    return quest_write_future_.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  }

  void release_quest_write() {
    if (!quest_write_released_.exchange(true)) {
      quest_write_release_promise_.set_value();
    }
  }

  void refuse_fences_for(std::int64_t character_id) {
    fence_character_ = character_id;
    fences_refused_.store(true);
  }

  void allow_fences() {
    fences_refused_.store(false);
  }

  bool wait_for_refused_quest_fence() {
    return quest_fence_refused_future_.wait_for(std::chrono::seconds(10)) ==
           std::future_status::ready;
  }

  void observe_monster_batch(int npc_cms_id, std::int64_t character_id) {
    observed_npc_ = npc_cms_id;
    observed_character_ = character_id;
    observe_reward_batch_.store(true);
  }

  bool wait_for_monster_batch(std::chrono::milliseconds timeout) {
    return observed_reward_future_.wait_for(timeout) == std::future_status::ready;
  }

  void observe_next_position_checkpoint() {
    observe_position_.store(true);
  }

  bool wait_for_position_checkpoint() {
    return position_future_.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  }

  void lose_next_ready_acknowledgement() {
    lose_ready_acknowledgement_.store(true);
  }

  bool wait_for_lost_ready_acknowledgement() {
    return ready_ack_future_.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  }

  std::optional<CharacterItem> grant_item(std::int64_t account, std::int64_t character,
                                          int world, CharacterItem item) override {
    return inner_->grant_item(account, character, world, std::move(item));
  }

  bool update_item(std::int64_t account, std::int64_t character, int world,
                   const CharacterItem &item) override {
    return inner_->update_item(account, character, world, item);
  }

  bool update_items(std::int64_t account, std::int64_t character, int world,
                    std::int64_t revision, const std::vector<CharacterItem> &items) override {
    return inner_->update_items(account, character, world, revision, items);
  }

  bool commit_world_death(std::int64_t account, std::int64_t character, int world,
                          std::int64_t revision,
                          const std::vector<EquippedDurabilityChange> &changes) override {
    if (death_commit_proxy_armed_.exchange(false)) {
      return death_commit_proxy_->commit_world_death(account, character, world, revision,
                                                     changes);
    }
    return inner_->commit_world_death(account, character, world, revision, changes);
  }

  bool commit_world_revival(std::int64_t account, std::int64_t character, int world,
                            std::int64_t revision, const WorldPosition &position) override {
    return inner_->commit_world_revival(account, character, world, revision, position);
  }

  bool commit_world_shrine_revival(std::int64_t account, std::int64_t character, int world,
                                  const WorldProgress &before, const WorldProgress &after,
                                  const WorldPosition &position) override {
    return inner_->commit_world_shrine_revival(account, character, world, before, after, position);
  }

  bool discard_items(std::int64_t account, std::int64_t character, int world,
                     std::int64_t revision, const std::vector<std::int64_t> &items,
                     const std::map<int, int> &stackables) override {
    return inner_->discard_items(account, character, world, revision, items, stackables);
  }

  bool apply_item_polish(std::int64_t account, std::int64_t character, int world,
                         std::int64_t revision, const ItemPolishChange &change) override {
    return inner_->apply_item_polish(account, character, world, revision, change);
  }

  bool remove_item(std::int64_t account, std::int64_t character, int world,
                   std::int64_t item_id) override {
    return inner_->remove_item(account, character, world, item_id);
  }

  WorldProgress load_world_progress(std::int64_t account, std::int64_t character,
                                    int world) override {
    return inner_->load_world_progress(account, character, world);
  }

  bool save_world_progress(std::int64_t account, std::int64_t character, int world,
                           const WorldProgress &before, const WorldProgress &after) override {
    if (lose_quest_write_ack_.load() && character == quest_write_character_ &&
        before.quests != after.quests && lose_quest_write_ack_.exchange(false)) {
      if (quest_write_commit_first_ &&
          !inner_->save_world_progress(account, character, world, before, after)) {
        throw std::runtime_error("Injected quest write did not commit");
      }
      quest_write_promise_.set_value();
      if (quest_write_release_future_.wait_for(std::chrono::seconds(10)) !=
          std::future_status::ready) {
        throw std::runtime_error("Injected quest write was not released");
      }
      throw std::runtime_error("Injected ambiguous quest write acknowledgement");
    }
    return inner_->save_world_progress(account, character, world, before, after);
  }

  bool fence_world_entry(std::int64_t account, std::int64_t character, int world) override {
    if (fences_refused_.load() && character == fence_character_) {
      if (!quest_fence_refusal_reported_.exchange(true)) {
        quest_fence_refused_promise_.set_value();
      }
      throw std::runtime_error("Injected quest reconciliation fence failure");
    }
    return inner_->fence_world_entry(account, character, world);
  }

  std::optional<DeathReconciliationSnapshot> fence_world_death_reconciliation(
      std::int64_t account, std::int64_t character, int world) override {
    if (death_fence_pause_armed_.exchange(false)) {
      death_fence_entered_promise_.set_value();
      if (death_fence_release_future_.wait_for(std::chrono::seconds(15)) !=
          std::future_status::ready) {
        throw std::runtime_error("Death reconciliation fence test was not released");
      }
    }
    return inner_->fence_world_death_reconciliation(account, character, world);
  }

  bool commit_world_progress_and_position(std::int64_t account, std::int64_t character,
                                          int world, const WorldProgress &before,
                                          const WorldProgress &after,
                                          const WorldPosition &position) override {
    if (!armed_.load() || character != target_character_ || !armed_.exchange(false)) {
      const bool committed = inner_->commit_world_progress_and_position(
          account, character, world, before, after, position);
      if (observe_position_.exchange(false)) {
        position_promise_.set_value();
      }
      return committed;
    }
    entered_promise_.set_value();
    if (release_future_.wait_for(std::chrono::seconds(15)) != std::future_status::ready) {
      throw std::runtime_error("Region checkpoint test was not released");
    }
    if (outcome_ == Outcome::conflict) {
      return false;
    }
    const bool committed = inner_->commit_world_progress_and_position(account, character, world,
                                                                       before, after, position);
    if (outcome_ == Outcome::lost_acknowledgement && committed) {
      throw std::runtime_error("Simulated lost region commit acknowledgement");
    }
    return committed;
  }

  std::optional<std::vector<CharacterItem>> commit_item_trade(
      std::int64_t account, std::int64_t character, int world, const WorldProgress &before,
      const WorldProgress &after, const ItemTrade &trade) override {
    return inner_->commit_item_trade(account, character, world, before, after, trade);
  }

  LootClaimResult lookup_loot_claim(std::int64_t account, std::int64_t character, int world,
                                    const std::string &receipt_id,
                                    const std::string &corpse_fingerprint) override {
    return inner_->lookup_loot_claim(account, character, world, receipt_id, corpse_fingerprint);
  }

  LootClaimResult commit_loot_claim(std::int64_t account, std::int64_t character, int world,
                                    const std::string &receipt_id,
                                    const std::string &corpse_fingerprint,
                                    const WorldProgress &before, const WorldProgress &after,
                                    const ItemTrade &trade) override {
    return inner_->commit_loot_claim(account, character, world, receipt_id, corpse_fingerprint,
                                      before, after, trade);
  }

  bool enqueue_monster_rewards(const std::vector<MonsterRewardReceipt> &receipts) override {
    if (rewards_refused_.load()) {
      if (!reward_refusal_reported_.exchange(true)) {
        reward_refused_promise_.set_value();
      }
      return false;
    }
    const bool enqueued = inner_->enqueue_monster_rewards(receipts);
    if (enqueued && observe_reward_batch_.load()) {
      for (const auto &receipt : receipts) {
        if (receipt.npc_cms_id == observed_npc_ && receipt.character_id == observed_character_ &&
            observe_reward_batch_.exchange(false)) {
          observed_reward_promise_.set_value();
          break;
        }
      }
    }
    return enqueued;
  }

  bool mark_monster_reward_ready(const std::string &receipt_id) override {
    const bool marked = inner_->mark_monster_reward_ready(receipt_id);
    if (marked && lose_ready_acknowledgement_.exchange(false)) {
      ready_ack_promise_.set_value();
      throw std::runtime_error("Simulated lost monster reward ready acknowledgement");
    }
    return marked;
  }

  std::size_t recover_staged_monster_rewards(std::size_t limit) override {
    return inner_->recover_staged_monster_rewards(limit);
  }

  bool has_staged_monster_rewards() override {
    return inner_->has_staged_monster_rewards();
  }

  std::int64_t acquire_monster_reward_owner_epoch() override {
    return inner_->acquire_monster_reward_owner_epoch();
  }

  void adopt_monster_reward_owner_epoch(std::int64_t epoch) override {
    inner_->adopt_monster_reward_owner_epoch(epoch);
  }

  std::vector<MonsterRewardReceipt> pending_monster_rewards(
      std::int64_t account, std::int64_t character, int world) override {
    return inner_->pending_monster_rewards(account, character, world);
  }

  MonsterRewardCommitResult commit_monster_reward(
      std::int64_t account, std::int64_t character, int world,
      const std::string &receipt_id, const WorldProgress &before,
      const std::optional<WorldProgress> &after, const ItemTrade &trade) override {
    auto &target = quest_commit_proxy_armed_.exchange(false) ? quest_commit_proxy_ : inner_;
    auto result = target->commit_monster_reward(account, character, world, receipt_id, before,
                                                after, trade);
    if (lose_reward_commit_ack_.load() && character == reward_commit_character_ &&
        result.status == MonsterRewardCommitStatus::committed &&
        lose_reward_commit_ack_.exchange(false)) {
      reward_commit_promise_.set_value();
      throw std::runtime_error("Injected lost monster reward commit acknowledgement");
    }
    return result;
  }

private:
  std::shared_ptr<CharacterStateRepository> inner_;
  std::shared_ptr<CharacterStateRepository> quest_commit_proxy_;
  std::atomic<bool> quest_commit_proxy_armed_{};
  std::shared_ptr<CharacterStateRepository> boss_settlement_proxy_;
  std::atomic<bool> boss_settlement_proxy_armed_{};
  std::shared_ptr<CharacterStateRepository> boss_publication_proxy_;
  std::atomic<bool> boss_publication_proxy_armed_{};
  std::shared_ptr<CharacterStateRepository> death_commit_proxy_;
  std::atomic<bool> death_commit_proxy_armed_{};
  std::atomic<bool> death_fence_pause_armed_{};
  std::atomic<bool> death_fence_released_{};
  std::promise<void> death_fence_entered_promise_;
  std::future<void> death_fence_entered_future_{death_fence_entered_promise_.get_future()};
  std::promise<void> death_fence_release_promise_;
  std::shared_future<void> death_fence_release_future_{
      death_fence_release_promise_.get_future().share()};
  std::atomic<bool> boss_publication_refused_{};
  std::atomic<bool> boss_publication_attempted_{};
  std::atomic<bool> fail_quest_commit_before_write_{};
  std::atomic<bool> lose_admin_quarantine_ack_{};
  std::atomic<bool> fail_admin_quarantine_load_{};
  std::atomic<bool> lose_reward_commit_ack_{};
  std::int64_t reward_commit_character_{};
  std::promise<void> reward_commit_promise_;
  std::future<void> reward_commit_future_{reward_commit_promise_.get_future()};
  std::atomic<bool> lose_quest_write_ack_{};
  std::int64_t quest_write_character_{};
  bool quest_write_commit_first_{};
  std::promise<void> quest_write_promise_;
  std::future<void> quest_write_future_{quest_write_promise_.get_future()};
  std::atomic<bool> quest_write_released_{};
  std::promise<void> quest_write_release_promise_;
  std::shared_future<void> quest_write_release_future_{
      quest_write_release_promise_.get_future().share()};
  std::atomic<bool> fences_refused_{};
  std::atomic<bool> quest_fence_refusal_reported_{};
  std::promise<void> quest_fence_refused_promise_;
  std::future<void> quest_fence_refused_future_{quest_fence_refused_promise_.get_future()};
  std::int64_t fence_character_{};
  std::atomic<bool> observe_reward_batch_{};
  int observed_npc_{};
  std::int64_t observed_character_{};
  std::promise<void> observed_reward_promise_;
  std::future<void> observed_reward_future_{observed_reward_promise_.get_future()};
  std::atomic<bool> rewards_refused_{};
  std::atomic<bool> reward_refusal_reported_{};
  std::promise<void> reward_refused_promise_;
  std::future<void> reward_refused_future_{reward_refused_promise_.get_future()};
  std::atomic<bool> observe_position_{};
  std::promise<void> position_promise_;
  std::future<void> position_future_{position_promise_.get_future()};
  std::atomic<bool> lose_ready_acknowledgement_{};
  std::promise<void> ready_ack_promise_;
  std::future<void> ready_ack_future_{ready_ack_promise_.get_future()};
  std::atomic<bool> armed_{};
  std::atomic<bool> released_{};
  std::int64_t target_character_{};
  Outcome outcome_{Outcome::commit};
  std::promise<void> entered_promise_;
  std::future<void> entered_future_{entered_promise_.get_future()};
  std::promise<void> release_promise_;
  std::shared_future<void> release_future_{release_promise_.get_future().share()};
};

} // namespace chronicles::native_testing
