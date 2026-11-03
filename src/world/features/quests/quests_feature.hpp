#pragma once

#include "world/core/frame_context.hpp"
#include "persistence/quest_service_effects.hpp"
#include "world/core/session_event_bus.hpp"
#include "world/features/character/character_progress_store.hpp"
#include "world/features/quests/quest_intent_ledger.hpp"
#include "world/features/quests/world_quests.hpp"

#include <cstddef>
#include <deque>
#include <mutex>
#include <chrono>
#include <stdexcept>
#include <set>
#include <unordered_set>

namespace chronicles::quests {

class QuestsSession;
class QuestsSessionContext;

// What an equipment reward needs to become an item row.
struct QuestInstanceItem {
  int grade{};
  int max_durability{};
};

// Immutable result of planning a doodad interaction together with its gathering reward.
// The owning operation must retain this exact plan through uncertain storage acknowledgements.
struct GatheringQuestPlan {
  WorldProgress after;
  ItemTrade trade;
  QuestServiceEffects effects;
  std::vector<GameplayPacket> packets;
  std::set<int> converted_item_ids;
  std::size_t original_granted_count{};
};

// A detached gathering operation owns this reservation until its durable receipt is
// conclusively committed or rejected. Destruction intentionally does not guess the outcome.
class GatheringQuestIntent final {
public:
  GatheringQuestIntent(std::shared_ptr<QuestIntentLedger> ledger,
                       QuestIntentLedger::Sequence sequence);

  void finish(bool committed);
  QuestIntentLedger::Sequence sequence() const noexcept;

private:
  const std::shared_ptr<QuestIntentLedger> ledger_;
  const QuestIntentLedger::Sequence sequence_;
  std::mutex mutex_;
  std::optional<bool> outcome_;
};

class QuestsFeature final {
public:
  // Called at the original late WorldState constructor-body position. Missing data disables
  // shared quests; present malformed data retains its fatal startup behavior.
  void load_catalog(const std::filesystem::path &catalog_root);
  const std::shared_ptr<const QuestCatalog> &catalog() const noexcept;
  std::unique_ptr<QuestsSession> create_session(character::CharacterProgressStore &store,
                                                QuestsSessionContext &context,
                                                std::shared_ptr<const Progression> progression,
                                                SessionEventBus &events);

private:
  std::shared_ptr<const QuestCatalog> catalog_;
};

class QuestsSessionContext {
public:
  virtual ~QuestsSessionContext() = default;

  virtual bool quests_has_handoff() const = 0;
  virtual std::optional<QuestServiceMembership> quests_service_membership() {
    throw std::runtime_error("Quest service membership capture is unavailable");
  }
  virtual std::string quests_new_effect_id() {
    throw std::runtime_error("Quest service effect identity is unavailable");
  }
  virtual std::optional<std::vector<CharacterItem>> quests_commit_unaffiliated(
      const QuestServiceMembership &, const WorldProgress &, const WorldProgress &,
      const ItemTrade &) {
    throw std::runtime_error("Unaffiliated quest transition is unavailable");
  }
  virtual QuestMutationResult quests_commit_service_effects(
      const std::string &, const WorldProgress &, const std::optional<WorldProgress> &,
      const ItemTrade &, const QuestServiceEffects &) {
    throw std::runtime_error("Durable quest service effects are unavailable");
  }

  virtual QuestMutationResult quests_commit_combat_event(const std::string &, const std::string &,
                                                         const WorldProgress &,
                                                         const std::optional<WorldProgress> &,
                                                         const ItemTrade &,
                                                         const QuestServiceEffects &) {
    throw std::runtime_error("Durable combat quest events are unavailable");
  }
  virtual MonsterRewardCommitResult quests_commit_monster_service_effects(
      const std::string &, const WorldProgress &, const std::optional<WorldProgress> &,
      const ItemTrade &, const QuestServiceEffects &) {
    throw std::runtime_error("Durable monster quest service effects are unavailable");
  }
  virtual std::shared_ptr<QuestIntentLedger> quests_intent_ledger() const {
    return {};
  }
  virtual std::optional<QuestIntentLedger::Sequence>
  quests_begin_mutation(const std::shared_ptr<QuestIntentLedger> &ledger,
                        const WorldProgress &after) {
    return ledger->begin_mutation(after);
  }
  // A shared owner must survive disconnect until an ambiguous mutation is fenced/read back.
  virtual void quests_watch_mutation(const std::shared_ptr<QuestIntentLedger> &,
                                     QuestIntentLedger::Sequence,
                                     const std::optional<std::string> &) {
  }
  // Record completed in-memory stages before the retained owner is retired. The native
  // owner uses these flags to resume if retirement itself fails after a definite result.
  virtual bool quests_mark_mutation_resolved(QuestIntentLedger::Sequence,
                                             bool) noexcept {
    return true;
  }
  virtual void quests_forget_mutation(QuestIntentLedger::Sequence) {
  }
  virtual void quests_retry_ambiguous_mutation(QuestIntentLedger::Sequence) {
  }
  virtual bool quests_has_tutorial() const = 0;
  virtual std::uint64_t quests_connection_id() const = 0;
  virtual std::uint64_t quests_player_unit_id(std::int64_t character_id) const = 0;
  virtual const AnnouncedUnit *quests_interaction_npc(std::uint64_t unit_id) const = 0;
  virtual void quests_apply_level_change(const WorldProgress &progress,
                                         std::vector<GameplayPacket> &packets) = 0;

  // The character's company standing for company quest conditions; nullopt when the realm
  // has no company service.
  virtual std::optional<QuestCompanyStanding> quests_company_standing() const = 0;

  // Applies the company rewards of quests whose completion committed: unlocks first (they
  // only raise a level, so repeating them is harmless), then contribution credited once.
  // Appends the client's company update to `packets` when the standing changed.
  virtual bool quests_apply_company_rewards(const QuestCompanyRewards &rewards,
                                             std::vector<GameplayPacket> &packets) = 0;

  virtual std::chrono::steady_clock::time_point quests_steady_now() const {
    return std::chrono::steady_clock::now();
  }

  // Level of the character's guild; nullopt without a guild or guild service.
  virtual std::optional<int> quests_guild_level() const = 0;

  // Credits GuildExp quest rewards to the character's guild once, after their commit. The guild
  // service delivers its own GuildExpUpdateNoti.
  virtual void quests_apply_guild_experience(std::int64_t experience) = 0;

  // Current health as a percentage of maximum; nullopt when combat does not track the player.
  virtual std::optional<int> quests_hp_percent() const = 0;

  // Current phase of a placed doodad the server tracks; nullopt otherwise.
  virtual std::optional<int> quests_doodad_phase(std::uint64_t unit_id) const = 0;

  // Catalog grade and maximum durability of an item the inventory stores as instances
  // (equipment, tools); nullopt for stackable items.
  virtual std::optional<QuestInstanceItem> quests_instance_item(int item_cms_id) const = 0;

  // Commits `after` together with new item rows in one transaction and refreshes the
  // session's inventory snapshot. Returns the stored rows, or nullopt on a revision conflict.
  virtual std::optional<std::vector<CharacterItem>>
  quests_commit_items(const WorldProgress &before, const WorldProgress &after,
                      const ItemTrade &trade) = 0;

  // Atomically applies a queued monster reward and marks its outbox receipt processed.
  virtual MonsterRewardCommitResult quests_commit_monster_reward(
      const std::string &receipt_id, const WorldProgress &before,
      const std::optional<WorldProgress> &after, const ItemTrade &trade) = 0;
};

// Owns the actual book/dialogue and the one-connection snapshot guard. Selecting/rebuilding
// the book on a map transition deliberately does not reset that old snapshot guard.
class QuestsSession final {
public:
  QuestsSession(QuestsFeature &feature, character::CharacterProgressStore &store,
                QuestsSessionContext &context, std::shared_ptr<const Progression> progression,
                SessionEventBus &events);

  void select_character(std::int64_t character_id);
  bool has_book() const noexcept;
  // NPC CMS ID of the open shared-world dialogue; nullopt without a book or open dialogue.
  std::optional<int> dialogue_npc_template_id() const;
  void retire_reconciled_reward_intent(const std::string &receipt_id,
                                       QuestIntentLedger::Sequence intent);
  bool handle(FrameContext &frame);
  void observe_snapshot(FrameContext &frame);
  void record_monster_reward(const WorldPosition &position, std::uint64_t credited_unit_id,
                             int npc_cms_id, std::vector<GameplayPacket> &packets,
                             bool award_monster_experience = true,
                             const std::optional<std::string> &receipt_id = std::nullopt,
                             const MonsterRewardReceipt *receipt = nullptr);
  // A party defeat is delivered once to this character's serialized worker. Repeat deliveries
  // within the same connection must not advance play records a second time.
  void record_party_monster_reward(const WorldPosition &position, std::uint64_t credited_unit_id,
                                   int npc_cms_id, std::vector<GameplayPacket> &packets);
  void record_queued_monster_reward(const MonsterRewardReceipt &receipt,
                                    std::vector<GameplayPacket> &packets);
  void seed_random_for_test(std::uint32_t seed);
  // Credits committed crafts and doodad interactions to shared-world quest objectives and
  // appends the resulting packets after their durable commit.
  void record_events(const QuestEvents &events, const WorldPosition &position,
                     std::vector<GameplayPacket> &packets);

  // Retains one fully planned transition before storage. A retry uses the same quest
  // attempt, item rolls and receipt, including after a lost COMMIT acknowledgement.
  void record_combat_events(const QuestEvents &events, const WorldPosition &position,
                            std::int64_t logical_tick, std::vector<GameplayPacket> &packets);
  bool has_pending_combat_events() const noexcept;
  void retry_combat_events(std::vector<GameplayPacket> &packets, bool view_departed = false);
  // Plans a doodad's quest transition into the same revision and item trade as gathering.
  // This does not persist state or publish packets. The caller must retain the exact result
  // and commit it with a durable source receipt before publishing the doodad phase.
  GatheringQuestPlan plan_gathering_events(const QuestEvents &events,
                                            const WorldPosition &position,
                                            const WorldProgress &before,
                                            const WorldProgress &gathered,
                                            const ItemTrade &gathering_trade);
  // Reserve before storage and transfer the returned owner to detached admission recovery.
  // A caller that cannot register recovery must finish(false) before any write begins.
  std::shared_ptr<GatheringQuestIntent> reserve_gathering_intent(const WorldProgress &after);
  // Replace temporary equipment stack notifications with the actual committed item rows.
  static void finalize_gathering_packets(GatheringQuestPlan &plan,
                                          const std::vector<CharacterItem> &stored);
  // Whether the persisted character at `position` meets catalog-shaped condition rows (see
  // WorldQuestBook::requirements_met). Without a shared quest book, a handoff or a persisted
  // character nothing can be checked, so the answer is false (fail closed).
  bool requirements_met(const nlohmann::json &conditions, const WorldPosition &position) const;
  // Re-evaluates state objectives right after another service committed a change they read
  // (an activated adventure object for AdventureObjectStatus, a looted item for GetItem)
  // instead of waiting for the next time sync. Commits separately and appends its packets.
  void refresh_objectives(const WorldPosition &position, std::vector<GameplayPacket> &packets);
  QuestResult plan_acceptance(int quest_id, bool check_conditions, const WorldProgress &progress,
                              const QuestWorldView &view);
  WorldQuestBook::GmCompletion plan_completion(const std::vector<int> &quest_ids,
                                               const WorldProgress &progress,
                                               const QuestWorldView &view);

private:
  bool request(const WorldGameplayRequest *request,
               const std::optional<WorldPosition> &position) const;
  std::vector<GameplayPacket> receive(const WorldGameplayRequest &request,
                                      const WorldPosition &position);
  // The server-owned facts quest rules read, for the persisted `character` at `position`.
  QuestWorldView make_view(const WorldPosition &position, const Character &character) const;
  bool apply_company_rewards(const QuestCompanyRewards &rewards,
                             std::vector<GameplayPacket> &packets);
  // Server-offered map starts and time/health/exploration objectives, committed separately.
  void run_periodic_update(const WorldPosition &position, std::vector<GameplayPacket> &packets);

  enum class CommitStatus { committed, already_processed, conflict, inventory_full };
  // Commits a quest transition. Equipment the quest rules credited as stacks (rewards,
  // supplies, rolled loot packs) is moved into new item rows in the same transaction and the
  // ItemsUpdateNoti in `packets` lists those rows instead of stack counts.
  CommitStatus commit(const WorldProgress &current, WorldProgress &next,
                      std::vector<GameplayPacket> &packets,
                      const std::optional<std::string> &receipt_id = std::nullopt);

  void prepare_reward_items(const Character &character, const WorldProgress &before,
                            GatheringQuestPlan &plan);
  void prepare_combat_events();

  struct PendingCombatEvents {
    std::string operation_id;
    std::string source_fingerprint;
    QuestEvents events;
    WorldProgress before;
    Character character;
    QuestWorldView view;
    QuestServiceMembership membership;
    std::int64_t logical_tick{};
    GatheringQuestPlan plan;
    std::unique_ptr<WorldQuestBook> book;
    std::shared_ptr<QuestIntentLedger> ledger;
    std::optional<QuestIntentLedger::Sequence> intent;
    std::optional<QuestMutationResult> committed;
    bool watched{};
    bool published{};
    bool ledger_finished{};
    bool owner_forgotten{};
    bool plan_ready{};
  };

  std::unique_ptr<PendingCombatEvents> pending_combat_events_;

  QuestsFeature &feature_;
  character::CharacterProgressStore &store_;
  QuestsSessionContext &context_;
  const std::shared_ptr<const Progression> progression_;
  SessionEventBus &events_;
  std::unique_ptr<WorldQuestBook> book_;
  std::map<std::string, QuestIntentLedger::Sequence> unresolved_reward_intents_;
  std::deque<std::uint64_t> recent_party_kills_;
  std::unordered_set<std::uint64_t> recent_party_kill_ids_;
  bool snapshot_sent_{};
  bool unlock_repair_pending_{};
  std::chrono::steady_clock::time_point next_unlock_repair_{};
};

} // namespace chronicles::quests
