#include "world/features/quests/quests_feature.hpp"

#include "common/clock_ticks.hpp"
#include "persistence/item_wire.hpp"
#include "world/features/character/progression.hpp"
#include "world/features/inventory/inventory_items.hpp"
#include "world/features/quests/quest_rules.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace chronicles::quests {
namespace {

constexpr std::uint16_t kTimeSyncRequest = 20001;
constexpr std::uint16_t kCommonErrorResponse = 2;
constexpr std::uint16_t kItemsUpdated = 169;
constexpr std::uint16_t kWorldPlayRecordsNotification = 134;
// One quest transition never grants more item rows than a single item trade accepts.
constexpr std::size_t kMaximumGrantedItemRows = 64;

// A failure while preparing rewards is known to precede any database write. Release that intent
// so unrelated kill captures do not wait forever; once a write starts its outcome may be unknown
// and only durable reconciliation can resolve it.
class BeforeWriteIntentGuard {
public:
  BeforeWriteIntentGuard(
      std::shared_ptr<QuestIntentLedger> ledger,
      std::optional<QuestIntentLedger::Sequence> intent,
      std::map<std::string, QuestIntentLedger::Sequence> &unresolved,
      const std::optional<std::string> &receipt_id, bool replaying,
      QuestsSessionContext &context)
      : ledger_(std::move(ledger)), intent_(intent), unresolved_(unresolved),
        receipt_id_(receipt_id), context_(context), write_started_(replaying) {
  }

  ~BeforeWriteIntentGuard() {
    if (!ledger_ || !intent_ || finished_) {
      return;
    }
    try {
      if (write_started_) {
        if (watched_) {
          context_.quests_retry_ambiguous_mutation(*intent_);
        }
        return;
      }
      if (!resolved_) {
        ledger_->finish_prepared(*intent_, false);
        if (receipt_id_) {
          unresolved_.erase(*receipt_id_);
        }
        resolved(receipt_id_.has_value());
      }
      if (watched_) {
        context_.quests_forget_mutation(*intent_);
      }
    } catch (const std::exception &error) {
      // An unresolved intent fails closed. Never let exception unwinding terminate the server.
      try {
        spdlog::error("Could not release pre-write quest intent: {}", error.what());
      } catch (...) {
      }
      if (watched_) {
        try {
          context_.quests_retry_ambiguous_mutation(*intent_);
        } catch (...) {
        }
      }
    } catch (...) {
      try {
        spdlog::error("Could not release pre-write quest intent: unknown exception");
      } catch (...) {
      }
      if (watched_) {
        try {
          context_.quests_retry_ambiguous_mutation(*intent_);
        } catch (...) {
        }
      }
    }
  }

  void watch() {
    if (ledger_ && intent_) {
      context_.quests_watch_mutation(ledger_, *intent_, receipt_id_);
      watched_ = true;
    }
  }

  void write_started() noexcept {
    write_started_ = true;
  }

  void resolved(bool receipt_retired) {
    resolved_ = true;
    if (watched_ && !context_.quests_mark_mutation_resolved(*intent_, receipt_retired)) {
      throw std::logic_error("Quest reconciliation owner changed before retirement");
    }
  }

  void finished() noexcept {
    if (watched_) {
      try {
        context_.quests_forget_mutation(*intent_);
      } catch (const std::exception &error) {
        try {
          spdlog::error("Could not release quest reconciliation owner: {}", error.what());
        } catch (...) {
        }
        return;
      } catch (...) {
        try {
          spdlog::error("Could not release quest reconciliation owner: unknown exception");
        } catch (...) {
        }
        return;
      }
    }
    finished_ = true;
  }

private:
  std::shared_ptr<QuestIntentLedger> ledger_;
  std::optional<QuestIntentLedger::Sequence> intent_;
  std::map<std::string, QuestIntentLedger::Sequence> &unresolved_;
  const std::optional<std::string> &receipt_id_;
  QuestsSessionContext &context_;
  bool write_started_{};
  bool finished_{};
  bool watched_{};
  bool resolved_{};
};

std::int64_t now_epoch_ms() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

GameplayPacket common_error(const char *code, std::uint32_t sequence) {
  return {kCommonErrorResponse,
          {{"ec", code}, {"packet_seq", {{"seq", std::to_string(sequence)}}}}};
}

// Replaces the converted stack counts in the transition's ItemsUpdateNoti with the new rows.
void list_granted_rows(std::vector<GameplayPacket> &packets, const std::set<int> &converted,
                       const std::vector<CharacterItem> &stored) {
  GameplayPacket *target{};
  for (auto &packet : packets) {
    if (packet.opcode != kItemsUpdated || !packet.message.contains("stackables")) {
      continue;
    }
    auto &stackables = packet.message.at("stackables");
    const auto before = stackables.size();
    stackables.erase(std::remove_if(stackables.begin(), stackables.end(),
                                    [&converted](const nlohmann::json &entry) {
                                      return converted.contains(
                                          entry.at("item_cms_id").get<int>());
                                    }),
                     stackables.end());
    if (!target && stackables.size() != before) {
      target = &packet;
    }
  }
  if (!target) {
    packets.push_back({kItemsUpdated, {{"quest_reward", nlohmann::json::object()}}});
    target = &packets.back();
  }

  auto items = target->message.value("items", nlohmann::json::array());
  for (const auto &item : stored) {
    items.push_back(item_instance_json(item));
  }
  target->message["items"] = std::move(items);
}

} // namespace

QuestsSession::CommitStatus QuestsSession::commit(const WorldProgress &current,
                                                  WorldProgress &next,
                                                  std::vector<GameplayPacket> &packets,
                                                  const std::optional<std::string> &receipt_id) {
  QuestServiceEffects service_effects;
  bool has_service_rewards = false;
  if (book_) {
    const auto company_rewards = book_->company_rewards_earned(current, next);
    service_effects.company_contribution = company_rewards.contribution;
    service_effects.guild_experience = book_->guild_experience_earned(current, next);
    for (const auto &unlock : company_rewards.unlocks) {
      service_effects.company_unlocks.push_back({unlock.company_type, unlock.level});
    }
    has_service_rewards = !company_rewards.empty() || service_effects.guild_experience != 0;
  }
  std::string effect_id;
  std::optional<QuestServiceMembership> service_membership;
  bool unaffiliated = false;
  if (has_service_rewards) {
    service_membership = context_.quests_service_membership();
    if (!service_membership) {
      return CommitStatus::conflict;
    }
    service_effects.expected_company_type = service_membership->company_type;
    service_effects.expected_guild_id = service_membership->guild_id;
    service_effects.expected_company_generation = service_membership->company_generation;
    service_effects.expected_guild_generation = service_membership->guild_generation;
    unaffiliated = !receipt_id && service_membership->company_type == 0 &&
                   service_membership->guild_id == 0;
    if (!unaffiliated) {
      effect_id = receipt_id ? *receipt_id : context_.quests_new_effect_id();
    }
  }
  const auto ledger = context_.quests_intent_ledger();
  if (has_service_rewards && !ledger) {
    throw std::runtime_error("Quest service rewards require a retained mutation ledger");
  }
  std::optional<QuestIntentLedger::Sequence> intent;
  bool replaying_intent = false;
  if (ledger) {
    if (receipt_id) {
      const auto existing = unresolved_reward_intents_.find(*receipt_id);
      if (existing != unresolved_reward_intents_.end()) {
        // The previous transaction may have committed. Its original item IDs, slots and
        // timestamp must not be replanned while fenced reconciliation is still pending.
        return CommitStatus::conflict;
      }
    }
    // A receipt-backed EXP, play-record or item change can commit without changing
    // quests. Its live cache still needs fenced readback if the acknowledgement is lost.
    const bool needs_intent = receipt_id.has_value() || current.quests != next.quests;
    if (!intent && needs_intent) {
      auto reserved = unresolved_reward_intents_.end();
      if (receipt_id) {
        reserved = unresolved_reward_intents_.emplace(*receipt_id, 0).first;
      }
      try {
        intent = context_.quests_begin_mutation(ledger, next);
      } catch (...) {
        if (receipt_id) {
          unresolved_reward_intents_.erase(reserved);
        }
        throw;
      }
      if (intent && receipt_id) {
        reserved->second = *intent;
      } else if (!intent && receipt_id) {
        unresolved_reward_intents_.erase(reserved);
      }
    }
    if (!intent && needs_intent) {
      return CommitStatus::conflict;
    }
  }
  BeforeWriteIntentGuard intent_guard(ledger, intent, unresolved_reward_intents_, receipt_id,
                                       replaying_intent, context_);
  intent_guard.watch();
  const auto finish = [&](CommitStatus status) {
    if (ledger && intent) {
      if (status == CommitStatus::committed) {
        if (replaying_intent) {
          ledger->finish_mutation(*intent, next);
        } else {
          ledger->finish_prepared(*intent, true);
        }
      } else if (status == CommitStatus::already_processed) {
        // A previous attempt may have committed but lost its acknowledgement.
        ledger->finish_mutation(*intent, store_.progression_stored_progress());
      } else {
        ledger->finish_prepared(*intent, false);
      }
      if (receipt_id) {
        unresolved_reward_intents_.erase(*receipt_id);
      }
      intent_guard.resolved(receipt_id.has_value());
    }
    intent_guard.finished();
    return status;
  };

  // Equipment counts credited by the quest rules become item rows; the stack keeps the count it
  // had before this transition (storage never deletes a stored progress row).
  std::vector<CharacterItem> granted;
  std::set<int> converted;
  for (auto stack = next.stackables.begin(); stack != next.stackables.end();) {
    const auto previous = current.stackables.find(stack->first);
    const int before = previous == current.stackables.end() ? 0 : previous->second;
    const auto instance = stack->second > before ? context_.quests_instance_item(stack->first)
                                                 : std::nullopt;
    if (!instance) {
      ++stack;
      continue;
    }

    const int added = stack->second - before;
    for (int unit = 0; unit < added && granted.size() < kMaximumGrantedItemRows; ++unit) {
      CharacterItem item;
      item.cms_id = stack->first;
      item.grade = instance->grade;
      item.durability = instance->max_durability;
      granted.push_back(item);
    }
    converted.insert(stack->first);
    if (previous == current.stackables.end()) {
      stack = next.stackables.erase(stack);
    } else {
      stack->second = before;
      ++stack;
    }
  }

  if (granted.empty()) {
    intent_guard.write_started();
    if (has_service_rewards) {
      if (unaffiliated) {
        const auto saved = context_.quests_commit_unaffiliated(*service_membership, current, next,
                                                               {});
        return finish(saved ? CommitStatus::committed : CommitStatus::conflict);
      }
      if (receipt_id) {
        const auto saved = context_.quests_commit_monster_service_effects(
            effect_id, current, next, {}, service_effects);
        if (saved.status == MonsterRewardCommitStatus::already_processed) {
          return finish(CommitStatus::already_processed);
        }
        return finish(saved.status == MonsterRewardCommitStatus::committed
                          ? CommitStatus::committed : CommitStatus::conflict);
      }
      const auto saved = context_.quests_commit_service_effects(effect_id, current, next, {},
                                                                 service_effects);
      if (saved.status == QuestMutationStatus::already_processed) {
        return finish(CommitStatus::already_processed);
      }
      return finish(saved.status == QuestMutationStatus::committed
                        ? CommitStatus::committed : CommitStatus::conflict);
    }
    if (receipt_id) {
      const auto saved = context_.quests_commit_monster_reward(*receipt_id, current, next, {});
      if (saved.status == MonsterRewardCommitStatus::already_processed) {
        return finish(CommitStatus::already_processed);
      }
      return finish(saved.status == MonsterRewardCommitStatus::committed ? CommitStatus::committed
                                                                          : CommitStatus::conflict);
    }
    return finish(store_.progression_save(current, next) ? CommitStatus::committed
                                                        : CommitStatus::conflict);
  }

  const auto character = store_.progression_character();
  if (!character) {
    return finish(CommitStatus::conflict);
  }
  const auto slots = lowest_free_inventory_slots(*character, granted.size());
  if (!slots) {
    return finish(CommitStatus::inventory_full);
  }
  const auto created_epoch_ticks = unix_epoch_ticks_now();
  for (std::size_t index = 0; index < granted.size(); ++index) {
    granted[index].inventory_slot = (*slots)[index];
    granted[index].created_epoch_ticks = created_epoch_ticks;
  }

  std::vector<CharacterItem> stored;
  intent_guard.write_started();
  if (has_service_rewards && unaffiliated) {
    auto saved = context_.quests_commit_unaffiliated(*service_membership, current, next,
                                                      ItemTrade{granted, {}, {}});
    if (!saved) {
      return finish(CommitStatus::conflict);
    }
    if (saved->size() != granted.size()) {
      throw std::runtime_error("Committed unaffiliated quest returned an unexpected item count");
    }
    stored = std::move(*saved);
  } else if (has_service_rewards && receipt_id) {
    auto saved = context_.quests_commit_monster_service_effects(
        effect_id, current, next, ItemTrade{granted, {}, {}}, service_effects);
    if (saved.status == MonsterRewardCommitStatus::already_processed) {
      return finish(CommitStatus::already_processed);
    }
    if (saved.status != MonsterRewardCommitStatus::committed) {
      return finish(CommitStatus::conflict);
    }
    if (saved.granted_items.size() != granted.size()) {
      throw std::runtime_error("Committed monster quest effect returned an unexpected item count");
    }
    stored = std::move(saved.granted_items);
  } else if (has_service_rewards) {
    auto saved = context_.quests_commit_service_effects(
        effect_id, current, next, ItemTrade{granted, {}, {}}, service_effects);
    if (saved.status == QuestMutationStatus::already_processed) {
      return finish(CommitStatus::already_processed);
    }
    if (saved.status != QuestMutationStatus::committed) {
      return finish(CommitStatus::conflict);
    }
    if (saved.granted_items.size() != granted.size()) {
      throw std::runtime_error("Committed quest effect returned an unexpected item count");
    }
    stored = std::move(saved.granted_items);
  } else if (receipt_id) {
    auto saved = context_.quests_commit_monster_reward(*receipt_id, current, next,
                                                       ItemTrade{granted, {}, {}});
    if (saved.status == MonsterRewardCommitStatus::already_processed) {
      return finish(CommitStatus::already_processed);
    }
    if (saved.status != MonsterRewardCommitStatus::committed) {
      return finish(CommitStatus::conflict);
    }
    if (saved.granted_items.size() != granted.size()) {
      throw std::runtime_error("Committed monster reward returned an unexpected item count");
    }
    stored = std::move(saved.granted_items);
  } else {
    auto saved = context_.quests_commit_items(current, next, ItemTrade{granted, {}, {}});
    if (!saved) {
      return finish(CommitStatus::conflict);
    }
    if (saved->size() != granted.size()) {
      throw std::runtime_error("Committed quest trade returned an unexpected item count");
    }
    stored = std::move(*saved);
  }
  list_granted_rows(packets, converted, stored);
  return finish(CommitStatus::committed);
}

QuestsSession::QuestsSession(QuestsFeature &feature, character::CharacterProgressStore &store,
                             QuestsSessionContext &context,
                             std::shared_ptr<const Progression> progression,
                             SessionEventBus &events)
    : feature_(feature), store_(store), context_(context), progression_(std::move(progression)),
      events_(events) {
}

bool QuestsSession::has_book() const noexcept {
  return static_cast<bool>(book_);
}

std::optional<int> QuestsSession::dialogue_npc_template_id() const {
  if (!book_) {
    return std::nullopt;
  }
  return book_->dialogue_npc_template_id();
}

void QuestsSession::retire_reconciled_reward_intent(
    const std::string &receipt_id, QuestIntentLedger::Sequence intent) {
  const auto found = unresolved_reward_intents_.find(receipt_id);
  if (found == unresolved_reward_intents_.end() || found->second != intent) {
    throw std::logic_error("Unknown reconciled monster reward intent");
  }
  unresolved_reward_intents_.erase(found);
}

void QuestsSession::seed_random_for_test(std::uint32_t seed) {
  if (!book_) {
    throw std::logic_error("Cannot seed a missing quest book");
  }
  book_->seed_random_for_test(seed);
}

void QuestsSession::select_character(std::int64_t character_id) {
  recent_party_kills_.clear();
  recent_party_kill_ids_.clear();
  if (!feature_.catalog()) {
    return;
  }
  const auto wire_unit_id = static_cast<std::int64_t>(context_.quests_player_unit_id(character_id));
  book_ = std::make_unique<WorldQuestBook>(feature_.catalog(), wire_unit_id, progression_);
}

bool QuestsSession::request(const WorldGameplayRequest *request,
                            const std::optional<WorldPosition> &position) const {
  return request && position && book_ && !context_.quests_has_tutorial() &&
         context_.quests_has_handoff() && WorldQuestBook::handles(request->opcode);
}

QuestWorldView QuestsSession::make_view(const WorldPosition &position,
                                        const Character &character) const {
  QuestWorldView view;
  view.position = position;
  view.character = &character;
  view.find_unit = [this](std::uint64_t unit_id) {
    return context_.quests_interaction_npc(unit_id);
  };
  view.company = context_.quests_company_standing();
  view.guild_level = context_.quests_guild_level();
  view.now_epoch_ms = now_epoch_ms();
  view.hp_percent = context_.quests_hp_percent();
  view.doodad_phase = [this](std::uint64_t unit_id) {
    return context_.quests_doodad_phase(unit_id);
  };
  return view;
}

std::vector<GameplayPacket> QuestsSession::receive(const WorldGameplayRequest &request,
                                                   const WorldPosition &position) {
  const auto character = store_.progression_character();
  if (!character) {
    throw std::runtime_error("World quest request has no persisted character");
  }
  const auto current = store_.progression_stored_progress();
  const auto view = make_view(position, *character);

  auto result = book_->receive(request.opcode, request.message, request.sequence, current, view);
  if (!result.next) {
    return std::move(result.packets);
  }

  auto next = std::move(*result.next);
  if (next.revision == std::numeric_limits<std::int64_t>::max()) {
    throw std::runtime_error("World progress revision exhausted");
  }
  ++next.revision;
  const auto status = commit(current, next, result.packets);
  if (status == CommitStatus::inventory_full) {
    // Nothing was committed: the quest waits until the bag has room for its equipment.
    return {common_error("ItemSlotLimit", request.sequence)};
  }
  if (status == CommitStatus::conflict) {
    // Nothing was committed, so nothing is published; the client may retry.
    spdlog::warn("World quest commit conflict connection={} opcode={}",
                 context_.quests_connection_id(), request.opcode);
    return {common_error("BadRequest", request.sequence)};
  }
  events_.publish(PlayerWorldLootProgressCommitted{next});
  return std::move(result.packets);
}

bool QuestsSession::apply_company_rewards(const QuestCompanyRewards &rewards,
                                          std::vector<GameplayPacket> &packets) {
  if (!rewards.empty()) {
    return context_.quests_apply_company_rewards(rewards, packets);
  }
  return true;
}

void QuestsSession::run_periodic_update(const WorldPosition &position,
                                        std::vector<GameplayPacket> &packets) {
  // Periodic work commits separately; a failure must not break the time-sync reply.
  try {
    const auto character = store_.progression_character();
    if (!character) {
      return;
    }
    const auto current = store_.progression_stored_progress();
    const auto view = make_view(position, *character);
    auto result = book_->periodic_update(current, view);
    if (!result.next) {
      return;
    }
    if (commit(current, *result.next, result.packets) != CommitStatus::committed) {
      // Another writer won or the bag is full; the next time sync evaluates again.
      return;
    }
    events_.publish(PlayerWorldLootProgressCommitted{*result.next});
    context_.quests_apply_level_change(*result.next, result.packets);
    packets.insert(packets.end(), std::make_move_iterator(result.packets.begin()),
                   std::make_move_iterator(result.packets.end()));
  } catch (const std::exception &error) {
    spdlog::error("World quest periodic update failed connection={}: {}",
                  context_.quests_connection_id(), error.what());
  }
}

void QuestsSession::observe_snapshot(FrameContext &frame) {
  const auto *gameplay_request = frame.request();
  auto &session_packets = frame.session_packets;
  if (!gameplay_request || gameplay_request->opcode != kTimeSyncRequest || !book_ ||
      context_.quests_has_tutorial() || !context_.quests_has_handoff()) {
    return;
  }

  // Shared-world entry lists saved quests once, like the tutorial's time-sync snapshot.
  // A later map move keeps the client's list (the official 9011 entry resent none).
  std::optional<WorldProgress> entry_progress;
  if (!snapshot_sent_) {
    entry_progress = store_.progression_stored_progress();
    const auto &progress = *entry_progress;
    session_packets.push_back(book_->snapshot(progress));
    if (!progress.play_records.empty()) {
      auto records = nlohmann::json::array();
      for (const auto &[record_id, count] : progress.play_records) {
        const auto bounded_count = std::clamp<std::int64_t>(count, 0, std::numeric_limits<int>::max());
        records.push_back({{"cms_id", record_id}, {"count", bounded_count}});
      }
      session_packets.push_back({kWorldPlayRecordsNotification, {{"play_records", std::move(records)}}});
    }
    snapshot_sent_ = true;
    // Repair legacy unlocks without guessing an unrecoverable contribution amount.
    unlock_repair_pending_ = true;
  }

  const auto now = context_.quests_steady_now();
  if (unlock_repair_pending_ && now >= next_unlock_repair_) {
    // A pending v41 event fences company writes. Retry on this session's time-sync strand
    // with a fixed minimum interval instead of losing the repair or spinning on failure.
    next_unlock_repair_ = now + std::chrono::seconds(2);
    try {
      QuestCompanyRewards owed;
      if (entry_progress) {
        owed.unlocks = book_->company_unlocks_owed(*entry_progress);
      } else {
        owed.unlocks = book_->company_unlocks_owed(store_.progression_stored_progress());
      }
      unlock_repair_pending_ = !apply_company_rewards(owed, session_packets);
    } catch (const std::exception &error) {
      spdlog::warn("Legacy company unlock repair will retry connection={}: {}",
                   context_.quests_connection_id(), error.what());
    }
  }

  if (frame.gameplay_position) {
    run_periodic_update(*frame.gameplay_position, session_packets);
  }
}

bool QuestsSession::handle(FrameContext &frame) {
  const auto *gameplay_request = frame.request();
  auto &gameplay_position = frame.gameplay_position;
  auto &session_packets = frame.session_packets;

  if (request(gameplay_request, gameplay_position)) {
    auto quest_packets = receive(*gameplay_request, *gameplay_position);
    session_packets.insert(session_packets.end(), std::make_move_iterator(quest_packets.begin()),
                           std::make_move_iterator(quest_packets.end()));
    return true;
  }

  return false;
}

void QuestsSession::record_monster_reward(const WorldPosition &position,
                                          std::uint64_t credited_unit_id, int npc_cms_id,
                                          std::vector<GameplayPacket> &packets,
                                          bool award_monster_experience,
                                          const std::optional<std::string> &receipt_id,
                                          const MonsterRewardReceipt *receipt) {
  constexpr int kCommitAttempts = 3;
  for (int attempt = 0; attempt < kCommitAttempts; ++attempt) {
    const auto character = store_.progression_character();
    if (!character) {
      throw std::runtime_error("World monster reward lost character ownership");
    }
    const auto current = store_.progression_stored_progress();
    QuestResult result;
    std::optional<WorldQuestBook> planned_book;
    if (book_) {
      // A rejected revision must not consume a different random reward on retry.
      planned_book = *book_;
      const auto view = make_view(position, *character);
      result = planned_book->record_monster_defeat(credited_unit_id, npc_cms_id, current, view,
                                                   award_monster_experience, receipt);
    } else if (progression_ && award_monster_experience) {
      auto next = current;
      quest_rules::grant_experience(next, progression_->monster_experience(npc_cms_id),
                                    result.packets, progression_.get());
      if (!result.packets.empty()) {
        if (next.revision == std::numeric_limits<std::int64_t>::max()) {
          throw std::runtime_error("World monster reward revision exhausted");
        }
        ++next.revision;
        result.next = std::move(next);
      }
    }
    if (result.next) {
      const auto status = commit(current, *result.next, result.packets, receipt_id);
      if (status == CommitStatus::conflict) {
        continue;
      }
      if (status == CommitStatus::already_processed) {
        return;
      }
      if (status != CommitStatus::committed) {
        throw std::runtime_error("World monster reward inventory is full");
      }
      if (planned_book) {
        *book_ = std::move(*planned_book);
      }
      events_.publish(PlayerWorldLootProgressCommitted{*result.next});
      context_.quests_apply_level_change(*result.next, result.packets);
    } else if (receipt_id) {
      const auto saved = context_.quests_commit_monster_reward(*receipt_id, current,
                                                               std::nullopt, {});
      if (saved.status == MonsterRewardCommitStatus::conflict) {
        continue;
      }
      const auto outstanding = unresolved_reward_intents_.find(*receipt_id);
      if (outstanding != unresolved_reward_intents_.end()) {
        const auto intent = outstanding->second;
        try {
          if (const auto ledger = context_.quests_intent_ledger()) {
            ledger->finish_mutation(intent, store_.progression_stored_progress());
          }
          unresolved_reward_intents_.erase(outstanding);
          if (!context_.quests_mark_mutation_resolved(intent, true)) {
            throw std::logic_error("Quest reconciliation owner changed before retirement");
          }
          context_.quests_forget_mutation(intent);
        } catch (...) {
          context_.quests_retry_ambiguous_mutation(intent);
          throw;
        }
      }
      if (saved.status == MonsterRewardCommitStatus::already_processed) {
        return;
      }
      if (planned_book) {
        *book_ = std::move(*planned_book);
      }
    } else if (planned_book) {
      // A valid no-op can still consume an authored probability roll.
      *book_ = std::move(*planned_book);
    }
    packets.insert(packets.end(), std::make_move_iterator(result.packets.begin()),
                   std::make_move_iterator(result.packets.end()));
    return;
  }
  throw std::runtime_error("World monster reward commit conflict after retries");
}

void QuestsSession::record_queued_monster_reward(const MonsterRewardReceipt &receipt,
                                                 std::vector<GameplayPacket> &packets) {
  if (unresolved_reward_intents_.contains(receipt.receipt_id)) {
    throw std::runtime_error("Monster reward is waiting for fenced reconciliation");
  }
  record_monster_reward(receipt.position, receipt.credited_unit_id, receipt.npc_cms_id, packets,
                        receipt.award_experience, receipt.receipt_id, &receipt);
}

void QuestsSession::record_party_monster_reward(const WorldPosition &position,
                                                std::uint64_t credited_unit_id, int npc_cms_id,
                                                std::vector<GameplayPacket> &packets) {
  constexpr std::size_t kRecentPartyKillLimit = 4096;
  if (credited_unit_id == 0 || !book_ || !context_.quests_has_handoff() ||
      context_.quests_has_tutorial() || recent_party_kill_ids_.contains(credited_unit_id)) {
    return;
  }

  // No party EXP sharing was established in captures; only quest objectives and play records
  // use this shared kill. Quest completion can still grant its own authored rewards.
  record_monster_reward(position, credited_unit_id, npc_cms_id, packets, false);
  recent_party_kills_.push_back(credited_unit_id);
  recent_party_kill_ids_.insert(credited_unit_id);
  if (recent_party_kills_.size() > kRecentPartyKillLimit) {
    recent_party_kill_ids_.erase(recent_party_kills_.front());
    recent_party_kills_.pop_front();
  }
}

void QuestsSession::record_events(const QuestEvents &events, const WorldPosition &position,
                                  std::vector<GameplayPacket> &packets) {
  if (events.empty() || !book_ || context_.quests_has_tutorial() ||
      !context_.quests_has_handoff()) {
    return;
  }

  // The producer already committed its own change, so a concurrent writer can only make this
  // separate quest commit stale; it is planned again once against the newer revision. A failure
  // here must not hide the producer's committed reply, so it is logged instead of thrown.
  constexpr int kCommitAttempts = 2;
  try {
    for (int attempt = 0; attempt < kCommitAttempts; ++attempt) {
      const auto character = store_.progression_character();
      if (!character) {
        spdlog::warn("World quest events lost character ownership connection={}",
                     context_.quests_connection_id());
        return;
      }
      const auto current = store_.progression_stored_progress();
      const auto view = make_view(position, *character);

      auto result = book_->record_events(events, current, view);
      if (!result.next) {
        return;
      }
      if (commit(current, *result.next, result.packets) == CommitStatus::committed) {
        events_.publish(PlayerWorldLootProgressCommitted{*result.next});
        context_.quests_apply_level_change(*result.next, result.packets);
        packets.insert(packets.end(), std::make_move_iterator(result.packets.begin()),
                       std::make_move_iterator(result.packets.end()));
        return;
      }
    }
  } catch (const std::exception &error) {
    spdlog::error("World quest events failed connection={}: {}", context_.quests_connection_id(),
                  error.what());
    return;
  }
  spdlog::warn("World quest event commit conflict connection={} events={}",
               context_.quests_connection_id(), events.size());
}

void QuestsSession::record_combat_events(const QuestEvents &events, const WorldPosition &position,
                                         std::int64_t logical_tick,
                                         std::vector<GameplayPacket> &packets) {
  if (pending_combat_events_) {
    throw std::logic_error("A later combat event cannot pass an unresolved quest batch");
  }
  if (events.empty() || !book_ || context_.quests_has_tutorial() ||
      !context_.quests_has_handoff()) {
    return;
  }

  const auto character = store_.progression_character();
  if (!character) {
    throw std::runtime_error("Combat quest event lost character ownership");
  }
  const auto membership = context_.quests_service_membership();
  if (!membership) {
    throw std::runtime_error("Cannot capture combat quest membership");
  }
  auto pending = std::make_unique<PendingCombatEvents>();
  pending->events = events;
  pending->before = store_.progression_stored_progress();
  pending->character = *character;
  pending->view = make_view(position, pending->character);
  // Combat events use the captured character, position and scalar view. NPC dialogue and
  // doodad interaction callbacks belong to their own producer and must not read a later map.
  pending->view.find_unit = {};
  pending->view.doodad_phase = {};
  pending->membership = *membership;
  pending->logical_tick = logical_tick;
  pending->book = std::make_unique<WorldQuestBook>(*book_);
  pending->ledger = context_.quests_intent_ledger();
  if (!pending->ledger) {
    throw std::runtime_error("Combat quest events require a retained intent ledger");
  }
  pending_combat_events_ = std::move(pending);
  retry_combat_events(packets);
}

void QuestsSession::prepare_combat_events() {
  auto &pending = *pending_combat_events_;
  // Retry preparation from the captured book, so an exception cannot consume another RNG roll.
  auto staged_book = std::make_unique<WorldQuestBook>(*pending.book);
  auto result = staged_book->record_events(pending.events, pending.before, pending.view);
  if (!result.next) {
    book_ = std::move(staged_book);
    pending_combat_events_.reset();
    return;
  }

  GatheringQuestPlan plan;
  plan.after = std::move(*result.next);
  plan.packets = std::move(result.packets);
  plan.effects.expected_company_type = pending.membership.company_type;
  plan.effects.expected_guild_id = pending.membership.guild_id;
  plan.effects.expected_company_generation = pending.membership.company_generation;
  plan.effects.expected_guild_generation = pending.membership.guild_generation;
  const auto company_rewards = staged_book->company_rewards_earned(pending.before, plan.after);
  plan.effects.company_contribution = company_rewards.contribution;
  plan.effects.guild_experience = staged_book->guild_experience_earned(pending.before, plan.after);
  for (const auto &unlock : company_rewards.unlocks) {
    plan.effects.company_unlocks.push_back({unlock.company_type, unlock.level});
  }
  prepare_reward_items(pending.character, pending.before, plan);

  nlohmann::json source_events = nlohmann::json::array();
  for (const auto &event : pending.events) {
    source_events.push_back({{"kind", static_cast<int>(event.kind)},
                             {"target_id", event.target_id},
                             {"phase_id", event.phase_id},
                             {"count", event.count},
                             {"context_id", event.context_id}});
  }
  nlohmann::json output = nlohmann::json::array();
  for (const auto &packet : plan.packets) {
    output.push_back({{"opcode", packet.opcode}, {"message", packet.message}});
  }
  const auto &position = pending.view.position;
  auto source_fingerprint = combat_quest_source_fingerprint(
      {{"character_id", pending.character.id},
       {"logical_tick", pending.logical_tick},
       {"position", {position.map_id, position.x_mm, position.y_mm, position.z_mm}},
       {"events", source_events},
       {"output", output},
       {"epoch_ms", pending.view.now_epoch_ms},
       {"mutation",
        canonical_quest_mutation(pending.before, plan.after, plan.trade, plan.effects)}});
  auto operation_id = context_.quests_new_effect_id();
  pending.plan = std::move(plan);
  pending.source_fingerprint = std::move(source_fingerprint);
  pending.operation_id = std::move(operation_id);
  pending.book = std::move(staged_book);
  pending.plan_ready = true;
}

bool QuestsSession::has_pending_combat_events() const noexcept {
  return static_cast<bool>(pending_combat_events_);
}

void QuestsSession::retry_combat_events(std::vector<GameplayPacket> &packets, bool view_departed) {
  if (!pending_combat_events_) {
    return;
  }
  if (!pending_combat_events_->plan_ready) {
    prepare_combat_events();
    if (!pending_combat_events_) {
      return;
    }
  }
  auto &pending = *pending_combat_events_;
  auto &plan = pending.plan;
  if (!pending.intent) {
    pending.intent = context_.quests_begin_mutation(pending.ledger, plan.after);
    if (!pending.intent) {
      throw std::runtime_error("Combat quest event is waiting for an earlier quest intent");
    }
  }
  if (!pending.watched) {
    context_.quests_watch_mutation(pending.ledger, *pending.intent, std::nullopt);
    pending.watched = true;
  }
  if (!pending.committed) {
    auto saved =
        context_.quests_commit_combat_event(pending.operation_id, pending.source_fingerprint,
                                            pending.before, plan.after, plan.trade, plan.effects);
    if (saved.status != QuestMutationStatus::committed &&
        saved.status != QuestMutationStatus::already_processed) {
      throw std::runtime_error("Combat quest event conflicts with its captured state; retained");
    }
    pending.committed = std::move(saved);
  }
  if (!pending.published) {
    auto finalized = plan;
    finalize_gathering_packets(finalized, pending.committed->granted_items);
    events_.publish(PlayerWorldLootProgressCommitted{plan.after});
    context_.quests_apply_level_change(plan.after, finalized.packets);
    plan.packets = std::move(finalized.packets);
    book_ = std::move(pending.book);
    pending.published = true;
  }
  if (!pending.ledger_finished) {
    pending.ledger->finish_prepared(*pending.intent, true);
    pending.ledger_finished = true;
  }
  if (!pending.owner_forgotten) {
    if (!context_.quests_mark_mutation_resolved(*pending.intent, true)) {
      throw std::logic_error("Combat quest reconciliation owner changed");
    }
    context_.quests_forget_mutation(*pending.intent);
    pending.owner_forgotten = true;
  }
  if (!view_departed) {
    // Copy into the result before releasing the retained batch. Allocation failure leaves
    // the original packets available; the producer itself is never run again.
    packets.reserve(packets.size() + plan.packets.size());
    packets.insert(packets.end(), plan.packets.begin(), plan.packets.end());
  }
  pending_combat_events_.reset();
}

GatheringQuestIntent::GatheringQuestIntent(std::shared_ptr<QuestIntentLedger> ledger,
                                           QuestIntentLedger::Sequence sequence)
    : ledger_(std::move(ledger)), sequence_(sequence) {
  if (!ledger_ || sequence_ == 0) {
    throw std::invalid_argument("Invalid gathering quest intent");
  }
}

void GatheringQuestIntent::finish(bool committed) {
  std::lock_guard lock(mutex_);
  if (outcome_) {
    if (*outcome_ != committed) {
      throw std::logic_error("Gathering quest intent outcome changed after resolution");
    }
    return;
  }
  ledger_->finish_prepared(sequence_, committed);
  outcome_ = committed;
}

QuestIntentLedger::Sequence GatheringQuestIntent::sequence() const noexcept {
  return sequence_;
}

std::shared_ptr<GatheringQuestIntent>
QuestsSession::reserve_gathering_intent(const WorldProgress &after) {
  const auto ledger = context_.quests_intent_ledger();
  if (!ledger) {
    throw std::runtime_error("Gathering quest mutation requires an intent ledger");
  }
  const auto sequence = context_.quests_begin_mutation(ledger, after);
  if (!sequence) {
    throw std::runtime_error("Gathering quest mutation is waiting for an earlier intent");
  }
  try {
    return std::make_shared<GatheringQuestIntent>(ledger, *sequence);
  } catch (...) {
    ledger->finish_prepared(*sequence, false);
    throw;
  }
}

GatheringQuestPlan QuestsSession::plan_gathering_events(
    const QuestEvents &events, const WorldPosition &position, const WorldProgress &before,
    const WorldProgress &gathered, const ItemTrade &gathering_trade) {
  if (before.revision == std::numeric_limits<std::int64_t>::max() ||
      gathered.revision != before.revision + 1 || gathered.quests != before.quests) {
    throw std::runtime_error("Invalid staged gathering progress");
  }

  GatheringQuestPlan plan;
  plan.after = gathered;
  plan.trade = gathering_trade;
  plan.original_granted_count = gathering_trade.granted_items.size();
  const auto membership = context_.quests_service_membership();
  if (!membership) {
    throw std::runtime_error("Cannot capture gathering membership");
  }
  plan.effects.expected_company_type = membership->company_type;
  plan.effects.expected_guild_id = membership->guild_id;
  plan.effects.expected_company_generation = membership->company_generation;
  plan.effects.expected_guild_generation = membership->guild_generation;
  if (context_.quests_has_tutorial()) {
    return plan;
  }
  if (!book_ || !context_.quests_has_handoff()) {
    if (events.empty()) {
      return plan;
    }
    throw std::runtime_error("Cannot plan gathering quest without a shared quest book");
  }

  const auto character = store_.progression_character();
  if (!character) {
    throw std::runtime_error("Cannot plan gathering quest without character ownership");
  }
  auto staged_character = *character;
  std::erase_if(staged_character.items, [&](const CharacterItem &item) {
    return std::ranges::find(gathering_trade.removed_item_ids, item.id) !=
           gathering_trade.removed_item_ids.end();
  });
  staged_character.items.insert(staged_character.items.end(), gathering_trade.granted_items.begin(),
                                gathering_trade.granted_items.end());
  const auto view = make_view(position, staged_character);

  // Both quest passes share the gathering revision. Reset only the temporary input revision;
  // the final result is committed together with the resource award in one transition.
  auto quest_input = gathered;
  quest_input.revision = before.revision;
  auto quest_result = book_->record_events(events, quest_input, view);
  if (quest_result.next) {
    quest_input = std::move(*quest_result.next);
    plan.packets = std::move(quest_result.packets);
  }
  quest_input.revision = before.revision;
  auto item_result = book_->refresh_gathered_items(quest_input, view);
  if (item_result.next) {
    quest_input = std::move(*item_result.next);
    plan.packets.insert(plan.packets.end(),
                        std::make_move_iterator(item_result.packets.begin()),
                        std::make_move_iterator(item_result.packets.end()));
  }
  quest_input.revision = gathered.revision;
  plan.after = std::move(quest_input);

  const auto company_rewards = book_->company_rewards_earned(gathered, plan.after);
  plan.effects.company_contribution = company_rewards.contribution;
  plan.effects.guild_experience = book_->guild_experience_earned(gathered, plan.after);
  for (const auto &unlock : company_rewards.unlocks) {
    plan.effects.company_unlocks.push_back({unlock.company_type, unlock.level});
  }
  prepare_reward_items(*character, gathered, plan);
  return plan;
}

void QuestsSession::prepare_reward_items(const Character &character, const WorldProgress &before,
                                         GatheringQuestPlan &plan) {
  for (auto stack = plan.after.stackables.begin(); stack != plan.after.stackables.end();) {
    const auto previous = before.stackables.find(stack->first);
    const int previous_count = previous == before.stackables.end() ? 0 : previous->second;
    const auto instance = stack->second > previous_count
                              ? context_.quests_instance_item(stack->first)
                              : std::nullopt;
    if (!instance) {
      ++stack;
      continue;
    }

    const auto added = static_cast<std::size_t>(stack->second - previous_count);
    if (plan.trade.granted_items.size() > kMaximumGrantedItemRows ||
        added > kMaximumGrantedItemRows - plan.trade.granted_items.size()) {
      throw std::runtime_error("Gathering quest item reward exceeds the item trade limit");
    }
    for (std::size_t index = 0; index < added; ++index) {
      CharacterItem item;
      item.cms_id = stack->first;
      item.grade = instance->grade;
      item.durability = instance->max_durability;
      plan.trade.granted_items.push_back(item);
    }
    plan.converted_item_ids.insert(stack->first);
    if (previous == before.stackables.end()) {
      stack = plan.after.stackables.erase(stack);
    } else {
      stack->second = previous_count;
      ++stack;
    }
  }

  const auto quest_item_count = plan.trade.granted_items.size() - plan.original_granted_count;
  if (quest_item_count != 0) {
    auto inventory = character;
    for (std::size_t index = 0; index < plan.original_granted_count; ++index) {
      inventory.items.push_back(plan.trade.granted_items[index]);
    }
    const auto slots = lowest_free_inventory_slots(inventory, quest_item_count);
    if (!slots) {
      throw std::runtime_error("Inventory is full for quest rewards");
    }
    const auto created_epoch_ticks = unix_epoch_ticks_now();
    for (std::size_t index = 0; index < quest_item_count; ++index) {
      auto &item = plan.trade.granted_items[plan.original_granted_count + index];
      item.inventory_slot = (*slots)[index];
      item.created_epoch_ticks = created_epoch_ticks;
    }
  }
}

void QuestsSession::finalize_gathering_packets(
    GatheringQuestPlan &plan, const std::vector<CharacterItem> &stored) {
  if (stored.size() != plan.trade.granted_items.size()) {
    throw std::runtime_error("Committed gathering quest returned an unexpected item count");
  }
  if (!plan.converted_item_ids.empty()) {
    const auto first_quest_item = stored.begin() + plan.original_granted_count;
    const std::vector<CharacterItem> quest_items(first_quest_item, stored.end());
    list_granted_rows(plan.packets, plan.converted_item_ids, quest_items);
  }
}

bool QuestsSession::requirements_met(const nlohmann::json &conditions,
                                     const WorldPosition &position) const {
  if (!book_ || context_.quests_has_tutorial() || !context_.quests_has_handoff()) {
    return false;
  }

  const auto character = store_.progression_character();
  if (!character) {
    return false;
  }

  const auto progress = store_.progression_stored_progress();
  return book_->requirements_met(conditions, progress, make_view(position, *character));
}

void QuestsSession::refresh_objectives(const WorldPosition &position,
                                       std::vector<GameplayPacket> &packets) {
  if (!book_ || context_.quests_has_tutorial() || !context_.quests_has_handoff()) {
    return;
  }
  run_periodic_update(position, packets);
}

QuestResult QuestsSession::plan_acceptance(int quest_id, bool check_conditions,
                                           const WorldProgress &progress,
                                           const QuestWorldView &view) {
  return book_->gm_accept(quest_id, check_conditions, progress, view);
}

WorldQuestBook::GmCompletion QuestsSession::plan_completion(const std::vector<int> &quest_ids,
                                                            const WorldProgress &progress,
                                                            const QuestWorldView &view) {
  return book_->gm_complete(quest_ids, progress, view);
}

} // namespace chronicles::quests
