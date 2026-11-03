#include "world/features/gathering/gathering_recovery.hpp"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace chronicles {
namespace {

std::string hex(const unsigned char *bytes, std::size_t length) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result;
  result.reserve(length * 2);
  for (std::size_t index = 0; index < length; ++index) {
    result.push_back(digits[bytes[index] >> 4]);
    result.push_back(digits[bytes[index] & 0x0f]);
  }
  return result;
}

} // namespace

std::string gathering_receipt_id() {
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
    throw std::runtime_error("Unable to create a gathering receipt");
  }
  return hex(bytes.data(), bytes.size());
}

std::string gathering_corpse_receipt_id(const std::string &incarnation) {
  if (incarnation.empty()) {
    throw std::invalid_argument("Corpse incarnation is missing");
  }
  const std::string input = "gathering-corpse-receipt-v1:" + incarnation;
  std::array<unsigned char, 32> digest{};
  unsigned int length{};
  if (EVP_Digest(input.data(), input.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1 ||
      length != digest.size()) {
    throw std::runtime_error("Unable to derive a corpse gathering receipt");
  }
  return hex(digest.data(), 16);
}

std::string gathering_plan_fingerprint(const GatheringCommitPlan &plan) {
  nlohmann::json events = nlohmann::json::array();
  for (const auto &event : plan.quest_events) {
    events.push_back({{"kind", static_cast<int>(event.kind)}, {"target_id", event.target_id},
                      {"phase_id", event.phase_id}, {"count", event.count},
                      {"context_id", event.context_id}});
  }
  nlohmann::json source = nullptr;
  if (plan.shared_source) {
    const auto &entry = *plan.shared_source;
    source = {{"map_id", entry.map_id}, {"kind", entry.kind},
              {"sector_id", entry.sector_id}, {"local_index", entry.local_index},
              {"generation", entry.expected_generation}, {"expected_active", entry.expected_active},
              {"expected_phase", entry.expected_phase ? nlohmann::json(*entry.expected_phase)
                                                       : nlohmann::json(nullptr)},
              {"resulting_active", entry.resulting_active},
              {"resulting_phase", entry.resulting_phase ? nlohmann::json(*entry.resulting_phase)
                                                         : nlohmann::json(nullptr)},
              {"delay", entry.next_transition_delay_ticks
                            ? nlohmann::json(*entry.next_transition_delay_ticks)
                            : nlohmann::json(nullptr)},
              {"successor", entry.timed_successor_phase
                                ? nlohmann::json(*entry.timed_successor_phase)
                                : nlohmann::json(nullptr)}};
    if (entry.linked) {
      const auto &linked = *entry.linked;
      source["linked"] = {{"map_id", linked.map_id}, {"kind", linked.kind},
                          {"sector_id", linked.sector_id}, {"local_index", linked.local_index},
                          {"generation", linked.expected_generation},
                          {"expected_active", linked.expected_active},
                          {"expected_phase", linked.expected_phase
                                                 ? nlohmann::json(*linked.expected_phase)
                                                 : nlohmann::json(nullptr)}};
    }
  }
  const nlohmann::json identity{
      {"domain", "gathering-receipt-v1"}, {"receipt", plan.receipt_id},
      {"account_id", plan.account_id}, {"character_id", plan.character_id},
      {"world_id", plan.world_id}, {"channel", plan.channel},
      {"source_scope", plan.source_scope}, {"expected_incarnation", plan.expected_incarnation},
      {"expected_phase", plan.expected_phase}, {"next_phase", plan.next_phase},
      {"respawn_delay_ticks", plan.respawn_delay_ticks
                                  ? nlohmann::json(*plan.respawn_delay_ticks)
                                  : nlohmann::json(nullptr)},
      {"map_id", plan.node.map_id}, {"node_kind", static_cast<int>(plan.node.kind)},
      {"sector_id", plan.node.sector_id}, {"node_index", plan.node.index},
      {"corpse_unit_id", plan.corpse_unit_id ? nlohmann::json(*plan.corpse_unit_id)
                                               : nlohmann::json(nullptr)},
      {"corpse_incarnation", plan.corpse_incarnation ? nlohmann::json(*plan.corpse_incarnation)
                                                     : nlohmann::json(nullptr)},
      {"shared_source", std::move(source)},
      {"mutation", canonical_quest_mutation(plan.before, plan.after, plan.trade, plan.effects)},
      {"quest_events", std::move(events)}};
  const auto bytes = identity.dump();
  std::array<unsigned char, 32> digest{};
  unsigned int length{};
  if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1 ||
      length != digest.size()) {
    throw std::runtime_error("Unable to fingerprint a gathering action");
  }
  return hex(digest.data(), digest.size());
}

GatheringRecoveryOperation::GatheringRecoveryOperation(
    GatheringCommitPlan plan, Commit commit, Clock clock, std::shared_ptr<GatheringNodes> nodes,
    std::optional<std::uint64_t> reservation, FinalizeIntent finalize_intent,
    FinalizeReceipt finalize_receipt, Clock epoch_clock, PublishSource publish_source,
    FenceSource fence_source, FinalizeCorpse finalize_corpse,
    std::optional<GatheringNodeKey> linked_node,
    std::optional<std::uint64_t> linked_reservation)
    : plan_(std::move(plan)), fingerprint_(gathering_plan_fingerprint(plan_)),
      commit_(std::move(commit)), clock_(std::move(clock)), nodes_(std::move(nodes)),
      reservation_(reservation), finalize_intent_(std::move(finalize_intent)),
      finalize_receipt_(std::move(finalize_receipt)), epoch_clock_(std::move(epoch_clock)),
      publish_source_(std::move(publish_source)), fence_source_(std::move(fence_source)),
      finalize_corpse_(std::move(finalize_corpse)), linked_node_(linked_node),
      linked_reservation_(linked_reservation) {
  const bool valid_receipt = plan_.receipt_id.size() == 32 &&
      std::ranges::all_of(plan_.receipt_id, [](char digit) {
        return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
      });
  if (plan_.account_id <= 0 || plan_.character_id <= 0 || plan_.world_id <= 0 ||
      !valid_receipt || plan_.source_scope.empty() || plan_.source_scope.size() > 128 ||
      plan_.expected_incarnation < 0 ||
      (plan_.respawn_delay_ticks && *plan_.respawn_delay_ticks <= 0) || !commit_ || !clock_ ||
      ((!plan_.quest_events.empty() || plan_.after.quests != plan_.before.quests) &&
       !finalize_intent_) ||
      (plan_.source_scope == "shared" && nodes_ && (!finalize_receipt_ || !epoch_clock_)) ||
      (plan_.corpse_unit_id && (!plan_.corpse_incarnation || !finalize_corpse_)) ||
      (!plan_.corpse_unit_id && (plan_.corpse_incarnation || finalize_corpse_)) ||
      linked_node_.has_value() != linked_reservation_.has_value() ||
      (linked_node_ && !nodes_) ||
      (plan_.shared_source && plan_.shared_source->linked && !linked_node_) ||
      (plan_.source_scope == "shared" && linked_node_ &&
       (!plan_.shared_source || !plan_.shared_source->linked)) ||
      static_cast<bool>(nodes_) != reservation_.has_value()) {
    throw std::invalid_argument("Invalid detached gathering operation");
  }
}

GatheringRecoveryStatus GatheringRecoveryOperation::attempt() {
  std::lock_guard lock(mutex_);
  if (status_ != GatheringRecoveryStatus::pending) {
    return status_;
  }
  if (!committed_) {
    // A unique receipt INSERT serializes a retry with an earlier still-running COMMIT.
    // A missing receipt lookup alone is not a fence and must not release the node.
    const auto result = commit_(plan_, fingerprint_);
    const bool committed = result.status == LootClaimStatus::committed ||
                           result.status == LootClaimStatus::already_claimed_by_same_character;
    if (result.status == LootClaimStatus::source_conflict) {
      if (!nodes_ || plan_.source_scope != "shared" || !plan_.shared_source) {
        throw std::runtime_error("Unexpected gathering source conflict");
      }
      // The conflict proves this claim did not write, but the snapshot alone does not
      // fence a source transition already queued on another worker.
      receipt_finalized_ = true;
      committed_ = false;
    } else {
      if (!committed && result.status != LootClaimStatus::conflict) {
        throw std::runtime_error("Gathering receipt identity cannot be reconciled");
      }
      if (committed) {
        // Allocation failure leaves the receipt retryable and the source still reserved.
        granted_items_ = result.granted_items;
      }
      committed_ = committed;
    }
  }
  if (!intent_finalized_) {
    if (finalize_intent_) {
      finalize_intent_(*committed_);
    }
    intent_finalized_ = true;
  }
  if (!*committed_ && nodes_ && plan_.source_scope == "shared" && !confirmed_source_) {
    // A generation bump invalidates queued timer/script CAS operations before the
    // authoritative phase can replace the reserved local snapshot.
    if (!fence_source_ || !plan_.shared_source) {
      throw std::runtime_error("Gathering source awaits authoritative conflict reconciliation");
    }
    const auto fenced = fence_source_(*plan_.shared_source);
    if (!fenced.committed || !fenced.source || fenced.source->pending_receipt_id ||
        fenced.source->next_transition_delay_ticks) {
      throw std::runtime_error("Gathering source fence requires operator or receipt recovery");
    }
    confirmed_source_ = fenced.source;
    if (linked_node_) {
      if (!fenced.linked_source_snapshot ||
          fenced.linked_source_snapshot->pending_receipt_id ||
          fenced.linked_source_snapshot->next_transition_delay_ticks) {
        confirmed_source_.reset();
        throw std::runtime_error("Linked gathering source awaits fenced reconciliation");
      }
      confirmed_linked_source_ = fenced.linked_source_snapshot;
    }
    confirmed_deadline_ = confirmed_source_->next_transition_epoch_ticks;
    receipt_finalized_ = true;
  }
  if (!receipt_finalized_) {
    if (finalize_receipt_) {
      auto finalized = finalize_receipt_(*committed_);
      if (finalized.status != GatheringFinalizeStatus::finalized &&
          finalized.status != GatheringFinalizeStatus::already_finalized) {
        throw std::runtime_error("Gathering receipt finalization could not be reconciled");
      }
      confirmed_source_ = std::move(finalized.source_snapshot);
      confirmed_linked_source_ = std::move(finalized.linked_source_snapshot);
      confirmed_deadline_ = confirmed_source_ ? confirmed_source_->next_transition_epoch_ticks
                                             : finalized.transition_epoch_ticks;
      if ((confirmed_deadline_ && !epoch_clock_) ||
          (plan_.source_scope == "shared" && nodes_ && !confirmed_source_) ||
          (plan_.source_scope == "shared" && linked_node_ && !confirmed_linked_source_)) {
        throw std::runtime_error("Gathering finalization lacks an authoritative source clock");
      }
    }
    receipt_finalized_ = true;
  }
  if (!corpse_finalized_) {
    if (finalize_corpse_) {
      finalize_corpse_(*committed_);
    }
    corpse_finalized_ = true;
  }
  if (!source_finalized_) {
    std::optional<std::int64_t> remaining;
    if (confirmed_deadline_) {
      const auto now_epoch = epoch_clock_();
      if (now_epoch < 0 || *confirmed_deadline_ < 0) {
        throw std::runtime_error("Invalid gathering recovery epoch");
      }
      remaining = *confirmed_deadline_ <= now_epoch ? 0 : *confirmed_deadline_ - now_epoch;
    }
    source_begin_tick_ = clock_();
    std::vector<GatheringStateChange> changes;
    if ((*committed_ || confirmed_source_) && nodes_) {
      const bool active = confirmed_source_ ? confirmed_source_->active :
          (plan_.node.kind == GatheringNodeKind::Doodad &&
           plan_.next_phase != kDespawnedDoodadPhase);
      const auto phase = confirmed_source_ ? confirmed_source_->phase :
          (plan_.node.kind == GatheringNodeKind::Doodad
               ? std::optional<int>(plan_.next_phase) : std::nullopt);
      changes.push_back({plan_.node, active, phase, source_begin_tick_});
      if (linked_node_) {
        const bool linked_active = confirmed_linked_source_ && confirmed_linked_source_->active;
        const auto linked_phase = confirmed_linked_source_
            ? confirmed_linked_source_->phase : std::optional<int>(kDespawnedDoodadPhase);
        if (linked_phase) {
          changes.push_back({*linked_node_, linked_active, linked_phase, source_begin_tick_});
        }
      }
    }
    if (nodes_) {
      bool installed = false;
      std::uint64_t first_revision{};
      std::uint64_t second_revision{};
      if (linked_node_ && confirmed_source_ && confirmed_linked_source_) {
        installed = nodes_->install_shared_source_pair(
            plan_.node, *confirmed_source_, *reservation_, *linked_node_,
            *confirmed_linked_source_, *linked_reservation_, epoch_clock_(), source_begin_tick_,
            false, &first_revision, &second_revision);
      } else if (linked_node_ && plan_.source_scope != "shared") {
        installed = nodes_->finish_reservation_pair(plan_.node, *reservation_, *linked_node_,
                                                    *linked_reservation_, *committed_,
                                                    source_begin_tick_, &first_revision,
                                                    &second_revision);
      } else if (confirmed_source_ && !linked_node_) {
        installed = nodes_->install_shared_source(plan_.node, *confirmed_source_, epoch_clock_(),
                                                   source_begin_tick_, reservation_, false,
                                                   &first_revision);
      } else if (!linked_node_) {
        installed = nodes_->finish_reservation(plan_.node, *reservation_, *committed_,
                                                source_begin_tick_, remaining, &first_revision);
      }
      if (!installed) {
        throw std::runtime_error("Gathering reservation owner changed before finalization");
      }
      if (!changes.empty()) {
        changes[0].revision = first_revision;
        if (changes.size() > 1) {
          changes[1].revision = second_revision;
        }
      }
    }
    source_changes_ = std::move(changes);
    source_finalized_ = true;
  }
  if (!source_published_) {
    if (!source_changes_.empty() && publish_source_) {
      publish_source_(source_changes_);
    }
    source_published_ = true;
  }
  status_ = *committed_ ? GatheringRecoveryStatus::committed : GatheringRecoveryStatus::rejected;
  return status_;
}

GatheringRecoveryStatus GatheringRecoveryOperation::status() const {
  std::lock_guard lock(mutex_);
  return status_;
}

void GatheringRecoveryOperation::set_publish_source(PublishSource publish_source) {
  std::lock_guard lock(mutex_);
  if (status_ != GatheringRecoveryStatus::pending || source_published_) {
    throw std::logic_error("Gathering source was already published");
  }
  publish_source_ = std::move(publish_source);
}

std::vector<CharacterItem> GatheringRecoveryOperation::granted_items() const {
  std::lock_guard lock(mutex_);
  return granted_items_;
}

std::vector<GatheringStateChange> GatheringRecoveryOperation::source_changes() const {
  std::lock_guard lock(mutex_);
  return source_changes_;
}

const GatheringCommitPlan &GatheringRecoveryOperation::plan() const noexcept {
  return plan_;
}

} // namespace chronicles
