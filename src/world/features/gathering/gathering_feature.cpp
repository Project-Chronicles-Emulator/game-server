#include "world/features/gathering/gathering_feature.hpp"
#include "world/features/gathering/construction_budget.hpp"
#include "world/features/gathering/gathering_recovery.hpp"

#include "common/clock_ticks.hpp"
#include "world/core/world_request_opcodes.hpp"
#include "world/core/world_sectors.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/post.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <mutex>
#include <random>
#include <stdexcept>
#include <utility>

namespace chronicles::gathering {
namespace {

using NativeTicks = std::chrono::duration<std::int64_t, std::ratio<1, kEpochTicksPerSecond>>;
using world_request::kMapLoadCompleteRequest;
constexpr int kCliffsMap = 30104;

void attach_state_plans(std::vector<GameplayPacket> &packets,
                        const std::vector<GatheringStateChange> &changes,
                        const std::shared_ptr<GatheringNodes> &nodes, std::uint64_t channel) {
  const auto guarded = gathering_state_packets(changes, nodes, channel);
  for (const auto &candidate : guarded) {
    const auto found = std::ranges::find_if(packets, [&](const GameplayPacket &packet) {
      return !packet.local_plan && packet.opcode == candidate.opcode &&
             packet.message == candidate.message;
    });
    if (found == packets.end()) {
      throw std::logic_error("Gathering source packet has no matching guarded payload");
    }
    found->local_plan = candidate.local_plan;
  }
}

std::int64_t monotonic_ticks() {
  return std::chrono::duration_cast<NativeTicks>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace

GatheringFeature::GatheringFeature(boost::asio::io_context &io_context,
                                   std::shared_ptr<const GatheringData> data)
    : data_(std::move(data)), nodes_(std::make_shared<GatheringNodes>(data_)),
      strand_(boost::asio::make_strand(io_context)), timer_(strand_) {
}

bool GatheringFeature::enabled() const noexcept {
  return static_cast<bool>(data_);
}

void GatheringFeature::set_shared_source_store(GatheringSharedSourceStore store) {
  if (!store.list || !store.transition || !store.fence || !store.post_worker) {
    throw std::invalid_argument("Shared gathering source store is incomplete");
  }
  shared_source_store_ = std::move(store);
}

SharedGatheringSourceRecord GatheringFeature::advance_shared_source(
    SharedGatheringSourceRecord record, std::int64_t now_epoch_ticks) {
  constexpr std::int64_t kTicksPerMillisecond = 10000;
  if (!record.state.next_transition_epoch_ticks ||
      *record.state.next_transition_epoch_ticks > now_epoch_ticks) {
    return record;
  }
  SharedGatheringSourceTransition transition;
  transition.map_id = record.key.map_id;
  transition.kind = record.key.kind;
  transition.sector_id = record.key.sector_id;
  transition.local_index = record.key.local_index;
  transition.expected_generation = record.state.generation;
  transition.expected_active = record.state.active;
  transition.expected_phase = record.state.phase;
  const auto kind = static_cast<GatheringNodeKind>(record.key.kind);
  if (kind == GatheringNodeKind::Placement) {
    transition.resulting_active = true;
  } else if (kind == GatheringNodeKind::Doodad) {
    const auto *point = data_ ? data_->doodad_point(record.key.map_id, record.key.sector_id,
                                                    record.key.local_index) : nullptr;
    const auto *definition = point ? data_->doodad(point->doodad_id) : nullptr;
    if (!definition || !record.state.timed_successor_phase) {
      throw std::runtime_error("Persisted gathering timer lacks an authored successor");
    }
    const int phase_id = *record.state.timed_successor_phase;
    transition.resulting_phase = phase_id;
    transition.resulting_active = phase_id != kDespawnedDoodadPhase;
    std::random_device entropy;
    std::mt19937 random(entropy());
    if (phase_id == kDespawnedDoodadPhase) {
      std::uniform_int_distribution<int> delay(definition->respawn_delay_min_ms,
                                                definition->respawn_delay_max_ms);
      transition.next_transition_delay_ticks =
          static_cast<std::int64_t>(delay(random)) * kTicksPerMillisecond;
      transition.timed_successor_phase = definition->initial_phase;
    } else {
      const auto *phase = definition->phase(phase_id);
      if (!phase) {
        throw std::runtime_error("Persisted gathering timer names an unknown phase");
      }
      if (!phase->interaction_id) {
        if (phase->duration_ms <= 0 || phase->next.empty()) {
          throw std::runtime_error("Persisted gathering timer has no supported transition");
        }
        std::vector<int> weights;
        weights.reserve(phase->next.size());
        for (const auto &choice : phase->next) {
          weights.push_back(choice.weight);
        }
        std::discrete_distribution<std::size_t> choose(weights.begin(), weights.end());
        transition.next_transition_delay_ticks =
            static_cast<std::int64_t>(phase->duration_ms) * kTicksPerMillisecond;
        transition.timed_successor_phase = phase->next.at(choose(random)).phase_id;
      }
    }
  } else {
    throw std::runtime_error("Invalid persisted gathering source kind");
  }
  const auto result = shared_source_store_.transition(transition, now_epoch_ticks, true);
  if (!result.source || result.source->pending_receipt_id ||
      (!result.committed && result.source->generation <= record.state.generation)) {
    throw std::runtime_error("Persisted gathering timer awaits source reconciliation");
  }
  record.state = *result.source;
  return record;
}

void GatheringFeature::hydrate_shared_sources() {
  constexpr std::size_t kPageSize = kSharedGatheringSourcePageSize;
  constexpr std::size_t kMaximumSources = kMaximumSharedGatheringSourcesPerWorld;
  if (!shared_source_store_.list || !shared_source_store_.transition ||
      !shared_source_store_.fence || !shared_source_store_.post_worker) {
    throw std::runtime_error("Shared gathering source store is unavailable");
  }
  std::optional<SharedGatheringSourceKey> cursor;
  std::size_t loaded = 0;
  while (true) {
    const auto remaining = kMaximumSources - loaded;
    const auto page = shared_source_store_.list(cursor, std::min(kPageSize, remaining + 1));
    if (page.size() > remaining) {
      throw std::runtime_error("Shared gathering sources exceed the startup limit");
    }
    if (page.empty()) {
      break;
    }
    for (auto record : page) {
      if ((cursor && !(record.key > *cursor)) || record.state.pending_receipt_id) {
        throw std::runtime_error("Invalid or unfinished shared gathering source at startup");
      }
      cursor = record.key;
      const auto epoch = unix_epoch_ticks_now();
      if (record.state.next_transition_epoch_ticks &&
          *record.state.next_transition_epoch_ticks <= epoch) {
        record = advance_shared_source(std::move(record), epoch);
      }
      const GatheringNodeKey node{record.key.map_id,
                                  static_cast<GatheringNodeKind>(record.key.kind),
                                  record.key.sector_id, record.key.local_index};
      if (!nodes_->install_shared_source(node, record.state, epoch, monotonic_ticks())) {
        throw std::runtime_error("Shared gathering source could not be restored");
      }
      ++loaded;
    }
    if (page.size() < kPageSize) {
      break;
    }
  }
  schedule_respawn();
}

BossScriptSourceLease::BossScriptSourceLease(std::shared_ptr<GatheringNodes> nodes,
                                                   std::string event_id,
                                                   std::vector<BossSourceTarget> targets,
                                                   std::vector<std::uint64_t> tokens)
    : nodes_(std::move(nodes)), event_id_(std::move(event_id)), targets_(std::move(targets)),
      tokens_(std::move(tokens)) {
  if (!nodes_ || event_id_.empty() || targets_.empty() || targets_.size() > 2 || targets_.size() != tokens_.size()) {
    throw std::invalid_argument("Invalid boss source lease");
  }
  node_keys_.reserve(targets_.size());
  for (const auto &target : targets_) {
    node_keys_.push_back(target.node);
  }
}

bool BossScriptSourceLease::mark_write_started() {
  std::lock_guard lock(mutex_);
  if (finished_) {
    return false;
  }
  write_started_ = true;
  return true;
}

std::optional<std::vector<GatheringStateChange>> BossScriptSourceLease::install(
    const BossScriptSettlement &settlement, std::int64_t epoch_ticks,
    std::int64_t monotonic_ticks) {
  std::lock_guard lock(mutex_);
  if (finished_ || !write_started_ || settlement.event_id != event_id_) {
    return std::nullopt;
  }
  if (installed_) {
    return pending_changes_;
  }
  if (!settlement.committed || settlement.sources.size() != targets_.size()) {
    return std::nullopt;
  }
  std::vector<GatheringStateChange> changes;
  changes.reserve(targets_.size());
  for (std::size_t index = 0; index < targets_.size(); ++index) {
    const auto &record = settlement.sources[index];
    const auto &target = targets_[index].node;
    const SharedGatheringSourceKey expected{target.map_id, static_cast<int>(target.kind),
                                            target.sector_id, target.index};
    if (record.key != expected || record.state.pending_receipt_id ||
        record.state.next_transition_delay_ticks) {
      return std::nullopt;
    }
    changes.push_back({target, record.state.active, record.state.phase, monotonic_ticks});
  }
  std::uint64_t first_revision{};
  std::uint64_t second_revision{};
  const bool installed = targets_.size() == 1
      ? nodes_->install_shared_source(targets_[0].node, settlement.sources[0].state,
                                      epoch_ticks, monotonic_ticks, tokens_[0], true,
                                      &first_revision)
      : nodes_->install_shared_source_pair(targets_[0].node, settlement.sources[0].state,
                                           tokens_[0], targets_[1].node,
                                           settlement.sources[1].state, tokens_[1], epoch_ticks,
                                           monotonic_ticks, true, &first_revision,
                                           &second_revision);
  if (!installed) {
    return std::nullopt;
  }
  changes[0].revision = first_revision;
  if (changes.size() > 1) {
    changes[1].revision = second_revision;
  }
  pending_changes_ = std::move(changes);
  installed_ = true;
  return pending_changes_;
}

bool BossScriptSourceLease::acknowledge_publication() {
  std::lock_guard lock(mutex_);
  if (finished_ || !installed_) {
    return false;
  }
  if (!nodes_->release_published_reservations(node_keys_, tokens_)) {
    return false;
  }
  finished_ = true;
  return true;
}

std::optional<std::vector<GatheringStateChange>> BossScriptSourceLease::install_private(
    std::int64_t monotonic_ticks) {
  std::lock_guard lock(mutex_);
  if (finished_ || !write_started_) {
    return std::nullopt;
  }
  if (installed_) {
    return pending_changes_;
  }
  auto changes = nodes_->install_private_boss_sources(targets_, tokens_, monotonic_ticks);
  if (!changes) {
    return std::nullopt;
  }
  pending_changes_ = std::move(*changes);
  installed_ = true;
  return pending_changes_;
}

bool BossScriptSourceLease::abort_before_write(std::int64_t monotonic_ticks) {
  std::lock_guard lock(mutex_);
  if (finished_ || write_started_) {
    return false;
  }
  const bool released = targets_.size() == 1
      ? nodes_->finish_reservation(targets_[0].node, tokens_[0], false, monotonic_ticks)
      : nodes_->finish_reservation_pair(targets_[0].node, tokens_[0], targets_[1].node,
                                        tokens_[1], false, monotonic_ticks);
  if (!released) {
    return false;
  }
  finished_ = true;
  return true;
}

std::shared_ptr<BossScriptSourceLease> GatheringFeature::reserve_boss_script_sources(
    const BossScriptIntent &intent, InstanceCopy *copy) {
  constexpr std::uint32_t kEliteSector = 170 | (289 << 14);
  constexpr std::uint32_t kRiftSector = 170 | (290 << 14);
  if (intent.event_id.empty() || intent.world_id <= 0 || intent.map_id != kCliffsMap ||
      (copy == nullptr) != (intent.copy_scope == "shared")) {
    return {};
  }
  if (copy) {
    const auto suffix = ":" + std::to_string(copy->serial());
    if (!intent.copy_scope.ends_with(suffix) || intent.copy_scope.size() == suffix.size()) {
      return {};
    }
  }
  std::vector<BossSourceTarget> targets;
  if (intent.npc_cms_id == 5001510) {
    targets = {{{kCliffsMap, GatheringNodeKind::Doodad, kEliteSector, 5}, 2},
               {{kCliffsMap, GatheringNodeKind::Doodad, kEliteSector, 8}, 1}};
  } else if (intent.npc_cms_id == 5001104) {
    targets = {{{kCliffsMap, GatheringNodeKind::Doodad, kRiftSector, 2}, 1}};
  } else {
    return {};
  }
  const auto nodes = nodes_for(copy);
  auto tokens = nodes->reserve_boss_source_batch(targets, copy == nullptr);
  if (!tokens) {
    return {};
  }
  try {
    return std::make_shared<BossScriptSourceLease>(nodes, intent.event_id, targets, *tokens);
  } catch (...) {
    const bool released = targets.size() == 1
        ? nodes->finish_reservation(targets[0].node, (*tokens)[0], false, monotonic_ticks())
        : nodes->finish_reservation_pair(targets[0].node, (*tokens)[0], targets[1].node,
                                          (*tokens)[1], false, monotonic_ticks());
    if (!released) {
      std::terminate();
    }
    throw;
  }
}

bool GatheringFeature::has_map(int map_id) const {
  return data_ && data_->has_map(map_id);
}

bool GatheringFeature::butcherable(int npc_cms_id) const {
  return data_ && data_->handling_reward(npc_cms_id) != nullptr;
}

std::shared_ptr<GatheringNodes> GatheringFeature::nodes_for(InstanceCopy *copy) {
  if (!copy) {
    return nodes_;
  }
  return copy->resource<GatheringNodes>([&] {
    auto created = std::make_shared<GatheringNodes>(data_);
    std::lock_guard lock(copy_nodes_mutex_);
    copy_nodes_.emplace_back(copy->serial(), created);
    return created;
  });
}

std::vector<GatheringFeature::ChannelNodes> GatheringFeature::live_nodes() {
  std::vector<ChannelNodes> live{{0, nodes_}};
  std::lock_guard lock(copy_nodes_mutex_);
  std::erase_if(copy_nodes_, [&](const auto &entry) {
    auto nodes = entry.second.lock();
    if (!nodes) {
      return true;
    }
    live.push_back({entry.first, std::move(nodes)});
    return false;
  });
  return live;
}

std::vector<GatheringStateChange> GatheringFeature::on_monster_defeated(
    int map_id, int npc_cms_id, std::int64_t now_ticks, InstanceCopy *copy) {
  if (!has_map(map_id)) {
    return {};
  }
  return nodes_for(copy)->cliffs_monster_defeated(map_id, npc_cms_id, now_ticks);
}

std::optional<int> GatheringFeature::doodad_phase(int map_id, std::uint64_t unit_id,
                                                  InstanceCopy *copy) {
  if (!data_) {
    return std::nullopt;
  }
  const auto point = data_->doodad_point_of_unit(map_id, unit_id);
  if (!point) {
    return std::nullopt;
  }
  const std::array<int, 3> point_mm{point->x_mm, point->y_mm, point->z_mm};
  const auto *definition = data_->doodad(point->doodad_id);
  if (!definition || !data_->map_contains(map_id, point_mm)) {
    return std::nullopt;
  }
  const GatheringNodeKey node{map_id, GatheringNodeKind::Doodad, point->sector_id,
                              point->local_id};
  return nodes_for(copy)->doodad_phase(node, *definition);
}

std::vector<GatheringStateChange>
GatheringFeature::record_volume_doodad_phases(int map_id, InstanceCopy *copy,
                                              const std::vector<VolumeDoodadPhase> &phases,
                                              std::int64_t now_ticks, bool restoring) {
  std::vector<GatheringStateChange> changes;
  if (phases.empty()) {
    return changes;
  }

  const auto nodes = nodes_for(copy);
  for (const auto &entry : phases) {
    const auto node = doodad_node_of_unit(map_id, entry.unit_id);
    if (!node) {
      spdlog::warn("Scripted doodad phase names a non-doodad unit {} on map {}", entry.unit_id,
                   map_id);
      continue;
    }
    const auto *point = data_ ? data_->doodad_point(map_id, node->sector_id, node->index) : nullptr;
    const auto *definition = point ? data_->doodad(point->doodad_id) : nullptr;
    if (!copy && definition && shared_source_store_.transition) {
      const auto current = nodes->shared_source(*node);
      const int phase = current && current->phase ? *current->phase
                                                  : nodes->doodad_phase(*node, *definition);
      if ((restoring && current) || phase == entry.phase) {
        continue;
      }
      if (!current && phase != definition->initial_phase) {
        throw std::runtime_error("Scripted gathering source lacks an authored initial phase");
      }
      SharedGatheringSourceTransition transition;
      transition.map_id = map_id;
      transition.kind = static_cast<int>(GatheringNodeKind::Doodad);
      transition.sector_id = node->sector_id;
      transition.local_index = node->index;
      transition.expected_generation = current ? current->generation : 0;
      transition.expected_active = current ? current->active : phase != kDespawnedDoodadPhase;
      transition.expected_phase = phase;
      transition.resulting_active = entry.phase != kDespawnedDoodadPhase;
      transition.resulting_phase = entry.phase;
      const auto epoch = unix_epoch_ticks_now();
      const auto persisted = shared_source_store_.transition(transition, epoch, false);
      std::uint64_t revision{};
      if (!persisted.source || persisted.source->pending_receipt_id ||
          !nodes->install_shared_source(*node, *persisted.source, epoch, now_ticks,
                                        std::nullopt, false, &revision)) {
        throw std::runtime_error("Scripted gathering phase awaits reconciliation");
      }
      if (persisted.committed) {
        changes.push_back({*node, persisted.source->active, persisted.source->phase, now_ticks,
                           revision});
      }
    } else {
      std::uint64_t revision{};
      if (nodes->record_volume_doodad_phase(*node, entry.phase, now_ticks, restoring, &revision)) {
        changes.push_back({*node, entry.phase != kDespawnedDoodadPhase, entry.phase, now_ticks,
                           revision});
      }
    }
  }
  return changes;
}

std::unique_ptr<GatheringSession>
GatheringFeature::create_session(std::uint64_t player_unit_id,
                                 std::shared_ptr<ProgressAccess> progress,
                                 GatheringSessionContext &context, SessionEventBus &events,
                                 std::optional<std::uint32_t> random_seed) {
  if (!enabled()) {
    return {};
  }

  GatheringPersistence persistence;
  persistence.load_character = [progress] {
    return progress->character();
  };
  persistence.load_progress = [progress] {
    // Shared-world gathering retains direct durable reads, rather than tutorial adoption.
    return progress->load();
  };
  persistence.conditions_met = [&context](const nlohmann::json &conditions,
                                           const WorldPosition &position) {
    return context.gathering_conditions_met(conditions, position);
  };
  persistence.commit = [progress](const WorldProgress &before, const WorldProgress &after,
                                  const ItemTrade &trade) {
    return progress->commit(before, after, trade).has_value();
  };
  std::weak_ptr<GatheringFeature> weak_feature = shared_from_this();
  persistence.commit_durable = [progress, &context, weak_feature](
                                   const GatheringDurableRequest &request) {
    GatheringDurableResult result;
    GatheringPreparedQuest prepared;
    std::shared_ptr<GatheringRecoveryOperation> operation;
    bool registered = false;
    bool owner_updated = false;
    std::uint64_t actor_channel = 0;
    try {
      prepared = context.gathering_prepare_quest(request.events, request.position,
                                                  request.before, request.after, request.trade);
      const auto identity = context.gathering_actor_identity();
      actor_channel = identity.channel;
      if (request.linked_node.has_value() != request.linked_reservation.has_value()) {
        throw std::runtime_error("Linked gathering reservation is incomplete");
      }
      GatheringCommitPlan plan;
      if (request.corpse_unit_id) {
        if (!request.corpse_handling || !request.corpse_reservation) {
          throw std::runtime_error("Corpse gathering reservation is missing");
        }
        plan.corpse_incarnation = request.corpse_handling->incarnation();
        plan.receipt_id = gathering_corpse_receipt_id(*plan.corpse_incarnation);
      } else {
        plan.receipt_id = gathering_receipt_id();
      }
      plan.account_id = identity.account_id;
      plan.character_id = identity.character_id;
      plan.world_id = identity.world_id;
      plan.channel = identity.channel;
      plan.source_scope = identity.source_scope;
      plan.node = request.node;
      plan.corpse_unit_id = request.corpse_unit_id;
      plan.expected_phase = request.expected_phase;
      plan.next_phase = request.next_phase;
      plan.respawn_delay_ticks = request.delay_ticks;
      plan.before = request.before;
      plan.after = prepared.after;
      plan.trade = prepared.trade;
      plan.effects = prepared.effects;
      plan.quest_events = request.events;
      if (identity.channel == 0 && !request.corpse_unit_id) {
        const auto current = request.nodes->shared_source(request.node);
        if (!current && request.node.kind == GatheringNodeKind::Doodad) {
          const auto feature = weak_feature.lock();
          const auto *point = feature && feature->data_
              ? feature->data_->doodad_point(request.node.map_id, request.node.sector_id,
                                               request.node.index)
              : nullptr;
          const auto *definition = point ? feature->data_->doodad(point->doodad_id) : nullptr;
          if (!definition || request.expected_phase != definition->initial_phase) {
            throw std::runtime_error("Shared gathering source lacks an authored initial phase");
          }
        }
        SharedGatheringSourceTransition source;
        source.map_id = request.node.map_id;
        source.kind = static_cast<int>(request.node.kind);
        source.sector_id = request.node.sector_id;
        source.local_index = request.node.index;
        source.expected_generation = current ? current->generation : 0;
        source.expected_active = current ? current->active : true;
        if (request.node.kind == GatheringNodeKind::Doodad) {
          source.expected_phase = request.expected_phase;
          source.resulting_phase = request.next_phase;
          source.resulting_active = request.next_phase != kDespawnedDoodadPhase;
        }
        source.next_transition_delay_ticks = request.delay_ticks;
        source.timed_successor_phase = request.timed_successor_phase;
        if (request.linked_node && request.linked_reservation) {
          const auto linked_current = request.nodes->shared_source(*request.linked_node);
          SharedGatheringLinkedTransition linked;
          linked.map_id = request.linked_node->map_id;
          linked.kind = static_cast<int>(request.linked_node->kind);
          linked.sector_id = request.linked_node->sector_id;
          linked.local_index = request.linked_node->index;
          linked.expected_generation = linked_current ? linked_current->generation : 0;
          linked.expected_active = linked_current ? linked_current->active : false;
          linked.expected_phase = linked_current ? linked_current->phase : std::nullopt;
          source.linked = linked;
        }
        plan.shared_source = source;
        plan.expected_incarnation = source.expected_generation;
      }
      const auto fingerprint = gathering_plan_fingerprint(plan);
      const auto immutable = std::make_shared<const GatheringCommitPlan>(plan);
      auto commit = [progress](const GatheringCommitPlan &input, const std::string &hash) {
        return progress->commit_gathering_quest_claim(input.receipt_id, hash, input.before,
                                                       input.after, input.trade, input.effects,
                                                       input.shared_source);
      };
      auto finalize = [progress, immutable, fingerprint](bool committed) {
        if (!committed) {
          return GatheringFinalizeResult{GatheringFinalizeStatus::finalized};
        }
        return progress->finalize_gathering_quest_claim(
            immutable->receipt_id, fingerprint, immutable->before, immutable->after,
            immutable->trade, immutable->effects, immutable->shared_source,
            unix_epoch_ticks_now());
      };
      auto publish = [weak_feature, channel = identity.channel](
                         const std::vector<GatheringStateChange> &changes) {
        if (const auto feature = weak_feature.lock()) {
          feature->publish_recovered_sources(changes, channel);
        }
      };
      const auto feature = weak_feature.lock();
      if (!feature) {
        throw std::runtime_error("Gathering feature owner is unavailable");
      }
      GatheringRecoveryOperation::FinalizeCorpse finalize_corpse;
      if (request.corpse_handling && request.corpse_reservation) {
        finalize_corpse = [handling = request.corpse_handling,
                           token = *request.corpse_reservation](bool committed) {
          if (!handling->finish(token, committed)) {
            throw std::runtime_error("Corpse gathering reservation owner changed");
          }
        };
      }
      operation = std::make_shared<GatheringRecoveryOperation>(
          std::move(plan), std::move(commit), monotonic_ticks,
          request.reservation ? request.nodes : nullptr, request.reservation,
          prepared.finalize_intent, std::move(finalize), unix_epoch_ticks_now,
          GatheringRecoveryOperation::PublishSource{},
          feature->shared_source_store_.fence, std::move(finalize_corpse),
          request.linked_node, request.linked_reservation);
      if (!context.gathering_register_recovery(operation)) {
        throw std::runtime_error("Gathering recovery admission is unavailable");
      }
      registered = true;
      try {
        operation->attempt();
      } catch (const std::exception &error) {
        spdlog::warn("Gathering commit requires reconciliation: {}", error.what());
      }
      if (operation->status() == GatheringRecoveryStatus::pending) {
        operation->set_publish_source(std::move(publish));
      }
      context.gathering_recovery_updated(operation);
      owner_updated = true;
      if (operation->status() == GatheringRecoveryStatus::pending) {
        result.unresolved = true;
        context.gathering_force_reconnect();
        return result;
      }
      if (operation->status() != GatheringRecoveryStatus::committed) {
        auto changes = operation->source_changes();
        if (!changes.empty()) {
          feature->publish_recovered_sources(std::move(changes), actor_channel);
        }
        return result;
      }
      result.committed = true;
      result.after = prepared.after;
      result.trade = prepared.trade;
      result.source_changes = operation->source_changes();
      const std::size_t expected_changes = request.corpse_unit_id ? 0 :
          (request.linked_node ? 2 : 1);
      if (result.source_changes.size() != expected_changes ||
          (!result.source_changes.empty() && result.source_changes.front().node != request.node) ||
          (request.linked_node && result.source_changes.back().node != *request.linked_node)) {
        throw std::runtime_error("Gathering source publication is incomplete");
      }
      const auto rows = operation->granted_items();
      try {
        result.packets = context.gathering_publish_committed(request.before, result.after, rows);
        if (prepared.packets) {
          auto quest_packets = prepared.packets(rows);
          result.packets.insert(result.packets.end(), std::make_move_iterator(quest_packets.begin()),
                                std::make_move_iterator(quest_packets.end()));
        }
      } catch (...) {
        context.gathering_force_reconnect();
        throw;
      }
      return result;
    } catch (...) {
      if (!registered) {
        try {
          if (prepared.finalize_intent) {
            prepared.finalize_intent(false);
          }
          if (request.nodes && request.reservation) {
            const bool released = request.linked_node && request.linked_reservation
                ? request.nodes->finish_reservation_pair(request.node, *request.reservation,
                                                         *request.linked_node,
                                                         *request.linked_reservation, false,
                                                         monotonic_ticks())
                : request.nodes->finish_reservation(request.node, *request.reservation, false,
                                                    monotonic_ticks());
            if (!released) {
              throw std::runtime_error("Gathering setup lost its reservation");
            }
          }
          if (request.corpse_handling && request.corpse_reservation &&
              !request.corpse_handling->finish(*request.corpse_reservation, false)) {
            throw std::runtime_error("Gathering setup lost its corpse reservation");
          }
        } catch (...) {
          // A failed pre-write cleanup must not leave an unowned admission or source claim.
          spdlog::critical("Gathering setup cleanup failed; stopping for recovery");
          std::terminate();
        }
      } else {
        try {
          if (!owner_updated) {
            context.gathering_recovery_updated(operation);
          }
          if (operation && operation->status() == GatheringRecoveryStatus::committed) {
            const auto feature = weak_feature.lock();
            if (!feature) {
              throw std::runtime_error("Committed gathering lost its scheduler");
            }
            auto changes = operation->source_changes();
            if (changes.empty()) {
              feature->schedule_respawn();
            } else {
              feature->publish_recovered_sources(std::move(changes), actor_channel);
            }
          }
          context.gathering_force_reconnect();
        } catch (...) {
          spdlog::critical("Gathering recovery owner could not be scheduled; stopping");
          std::terminate();
        }
      }
      throw;
    }
  };

  if (!random_seed) {
    std::random_device entropy;
    random_seed = entropy();
  }
  return std::make_unique<GatheringSession>(data_, nodes_, player_unit_id, std::move(persistence),
                                            *random_seed, vitals(context, events));
}

GatheringVitals GatheringFeature::vitals(GatheringSessionContext &context,
                                         SessionEventBus &events) {
  GatheringVitals access;
  access.dead = [&context] {
    return context.gathering_player_dead();
  };
  access.begin_stamina = [&context](const GatheringStaminaFlow &flow) {
    return context.gathering_begin_stamina(flow);
  };
  access.spend_stamina = [&context, &events](std::int64_t ticks) {
    if (context.gathering_spend_stamina(ticks)) {
      // Keep the caller's original unlocked cache scope; subscribers add no codec lock.
      events.publish(PlayerCombatSnapshotChanged{});
    }
  };
  access.cancel_stamina = [&context](std::int64_t ticks) {
    return context.gathering_cancel_stamina(ticks);
  };
  access.tool_worn = [&context](const CharacterItem &tool) {
    return context.gathering_tool_wear_updates(tool);
  };
  access.effect_available = [&context](int effect_id) {
    return context.gathering_effect_available(effect_id);
  };
  access.apply_effect = [&context](int effect_id, const WorldPosition &position, std::int64_t ticks) {
    return context.gathering_apply_effect(effect_id, position, ticks);
  };
  return access;
}

bool GatheringFeature::handle(GatheringSession *session, FrameContext &frame,
                              GatheringSessionContext &context,
                              std::vector<GatheringStateChange> &broadcasts) {
  const auto *request = frame.request();
  if (!request || !session || context.gathering_has_tutorial() || !frame.gameplay_position) {
    return false;
  }

  const auto &position = *frame.gameplay_position;
  // Corpses lie wherever monsters die, so butchering does not need a gathering map area.
  const bool mapped = has_map(position.map_id);
  if (!mapped && !session->corpse_request(request->opcode, request->message)) {
    return false;
  }

  if (request->opcode == kMapLoadCompleteRequest) {
    const auto &nodes = session->nodes();
    if (position.map_id == kCliffsMap) {
      auto restored = nodes->restore_cliffs_progress(position.map_id, session->load_progress(),
                                                     context.gathering_monotonic_ticks());
      auto restored_packets = gathering_state_packets(
          restored, nodes, context.gathering_actor_identity().channel);
      frame.session_packets.insert(frame.session_packets.end(),
                                   std::make_move_iterator(restored_packets.begin()),
                                   std::make_move_iterator(restored_packets.end()));
      broadcasts.insert(broadcasts.end(), restored.begin(), restored.end());
    }
    // Initial map snapshots are observers; this request still reaches later owners.
    return false;
  }
  if (!session->handles(request->opcode, request->message)) {
    return false;
  }

  const auto ticks = context.gathering_monotonic_ticks();
  if (session->busy()) {
    auto combat_packets = context.gathering_advance_combat(position, ticks);
    frame.session_packets.insert(frame.session_packets.end(),
                                 std::make_move_iterator(combat_packets.begin()),
                                 std::make_move_iterator(combat_packets.end()));
  }
  auto outcome =
      session->receive(request->opcode, request->message, request->sequence, position,
                       ticks, context.gathering_other_action_busy());
  if (!outcome.broadcasts.empty()) {
    try {
      const auto channel = context.gathering_actor_identity().channel;
      attach_state_plans(outcome.packets, outcome.broadcasts, session->nodes(), channel);
      schedule_respawn();
    } catch (...) {
      spdlog::critical("Gathering respawn could not be scheduled; stopping for recovery");
      std::terminate();
    }
  }
  if (outcome.cut_scene_id) {
    auto cut_scene = context.gathering_play_cut_scene(*outcome.cut_scene_id, position);
    const auto response = std::ranges::find_if(outcome.packets, [](const GameplayPacket &packet) {
      return packet.opcode == kInteractUnitResponse;
    });
    outcome.packets.insert(response, std::make_move_iterator(cut_scene.begin()),
                           std::make_move_iterator(cut_scene.end()));
  }
  frame.session_packets.insert(frame.session_packets.end(),
                               std::make_move_iterator(outcome.packets.begin()),
                               std::make_move_iterator(outcome.packets.end()));
  broadcasts.insert(broadcasts.end(), std::make_move_iterator(outcome.broadcasts.begin()),
                    std::make_move_iterator(outcome.broadcasts.end()));
  if (!outcome.observers.empty()) {
    context.gathering_deliver_observers(outcome.observers);
  }
  if (outcome.travel) {
    context.gathering_travel(*outcome.travel);
  }
  return true;
}

std::vector<GameplayPacket>
GatheringFeature::sector_snapshots(int map_id, std::span<const std::uint32_t> sector_ids,
                                   InstanceCopy *copy) {
  constexpr std::size_t kMaximumSnapshotSectors = 4096;
  constexpr std::size_t kMaximumSnapshotNodes = 65536;
  if (sector_ids.size() > kMaximumSnapshotSectors) {
    throw std::runtime_error("Gathering sight snapshot exceeds its sector limit");
  }
  const auto construction = ConstructionBudget::acquire();
  for (std::size_t index = 0; index < sector_ids.size(); ++index) {
    construction->charge(ConstructionBudget::kSectorBytes);
  }
  const auto charge_node = [construction] {
    construction->charge(ConstructionBudget::kNodeBytes);
  };

  // Read only sectors in the sight window; a busy map can contain many unrelated sources.
  const auto nodes = nodes_for(copy);
  std::vector<GameplayPacket> packets;
  packets.reserve(sector_ids.size());
  std::size_t snapshot_nodes = 0;
  for (const auto sector_id : sector_ids) {
    auto [placements, doodads] = nodes->sector_state(map_id, sector_id, charge_node);
    const auto sector_nodes = placements.size() + doodads.size();
    if (sector_nodes > kMaximumSnapshotNodes - snapshot_nodes) {
      throw std::runtime_error("Gathering sight snapshot exceeds its node limit");
    }
    snapshot_nodes += sector_nodes;

    std::vector<std::uint32_t> depleted;
    depleted.reserve(placements.size());
    for (const auto &change : placements) {
      depleted.push_back(change.node.index);
    }
    std::ranges::sort(depleted);
    packets.push_back(sector_update_packet(sector_id, depleted));
    if (!doodads.empty()) {
      packets.back().message["doodads"] = sector_doodad_nubs(doodads);
    }
    packets.back().local_plan = gathering_sector_plan(nodes, map_id, copy ? copy->serial() : 0,
                                                       sector_id, doodads);
    packets.back().local_construction = construction;
  }
  return packets;
}

void GatheringFeature::deliver_changes(const std::vector<GatheringStateChange> &changes,
                                       std::uint64_t channel, std::uint64_t excluded_connection,
                                       GatheringWorldContext &context) {
  // The world admission limit bounds recipient storage; the smaller frame limit also
  // bounds the amount of per-recipient bookkeeping retained before I/O reservation.
  constexpr std::size_t kMaximumRecipients = 100000;
  constexpr std::size_t kMaximumRecipientFrames = 65536;
  constexpr std::size_t kMaximumDistinctPackets = 4096;
  constexpr std::size_t kMaximumEncodedBytes = 16 * 1024 * 1024;

  if (changes.empty()) {
    return;
  }
  if (changes.size() > kMaximumRecipientFrames) {
    throw std::runtime_error("Gathering fanout exceeds its source change limit");
  }
  using SourceSector = std::pair<int, std::uint32_t>;
  std::map<SourceSector, std::vector<GatheringStateChange>> changes_by_sector;
  for (const auto &change : changes) {
    changes_by_sector[{change.node.map_id, change.node.sector_id}].push_back(change);
  }
  std::shared_ptr<GatheringNodes> scoped_nodes;
  for (const auto &live : live_nodes()) {
    if (live.channel == channel) {
      scoped_nodes = live.nodes;
      break;
    }
  }
  if (!scoped_nodes) {
    return;
  }

  struct CachedPacket {
    GameplayPacket packet;
    std::optional<Frame> encoded;
  };
  struct Recipient {
    std::function<void(Frame)> post;
    std::vector<std::pair<SourceSector, std::size_t>> packet_indices;
  };
  std::map<SourceSector, std::vector<CachedPacket>> packets_by_sector;
  std::size_t distinct_packets = 0;
  for (const auto &[source_sector, sector_changes] : changes_by_sector) {
    auto packets = gathering_state_packets(sector_changes, scoped_nodes, channel);
    if (packets.size() > kMaximumDistinctPackets - distinct_packets) {
      throw std::runtime_error("Gathering fanout exceeds its packet limit");
    }
    distinct_packets += packets.size();
    auto &cached = packets_by_sector[source_sector];
    cached.reserve(packets.size());
    for (auto &packet : packets) {
      cached.push_back({std::move(packet), std::nullopt});
    }
  }

  std::vector<Recipient> recipients;
  std::size_t recipient_frames = 0;
  std::size_t encoded_bytes = 0;
  context.gathering_each_peer_locked(excluded_connection, [&](GatheringPeer peer) {
    if (!peer.map_id || peer.channel != channel || !peer.sector_ready) {
      return;
    }
    if (recipients.size() >= kMaximumRecipients) {
      throw std::runtime_error("Gathering fanout exceeds its recipient limit");
    }
    Recipient recipient{peer.post, {}};
    auto matching = packets_by_sector.lower_bound({*peer.map_id, 0});
    for (; matching != packets_by_sector.end() && matching->first.first == *peer.map_id;
         ++matching) {
      if (!peer.sector_ready(matching->first.second)) {
        continue;
      }
      for (std::size_t index = 0; index < matching->second.size(); ++index) {
        auto &packet = matching->second[index];
        if (!packet.encoded) {
          Frame frame;
          try {
            frame = context.gathering_encode(packet.packet);
          } catch (const std::exception &error) {
            spdlog::warn("Gathering state notification encoding failed: {}", error.what());
            continue;
          }
          if (!frame.shared) {
            frame.shared = std::make_shared<const Bytes>(std::move(frame.bytes));
          }
          if (frame.shared->size() > kMaximumEncodedBytes - encoded_bytes) {
            throw std::runtime_error("Gathering fanout exceeds its encoded byte limit");
          }
          encoded_bytes += frame.shared->size();
          if (frame.local_retained_bytes > kMaximumEncodedBytes - encoded_bytes) {
            throw std::runtime_error("Gathering fanout exceeds its metadata byte limit");
          }
          encoded_bytes += frame.local_retained_bytes;
          packet.encoded = std::move(frame);
        }
        if (recipient_frames >= kMaximumRecipientFrames) {
          throw std::runtime_error("Gathering fanout exceeds its staged frame limit");
        }
        ++recipient_frames;
        recipient.packet_indices.emplace_back(matching->first, index);
      }
    }
    if (!recipient.packet_indices.empty()) {
      recipients.push_back(std::move(recipient));
    }
  });

  // No recipient callback runs while the codec mutex is held. Each copy shares the
  // immutable bytes and source plan; recipient binding and queue limits happen in post.
  for (auto &recipient : recipients) {
    for (const auto &[source_sector, index] : recipient.packet_indices) {
      recipient.post(*packets_by_sector.at(source_sector)[index].encoded);
    }
  }
}

void GatheringFeature::set_respawn_delivery(
    std::function<void(const std::vector<GatheringStateChange> &, std::uint64_t channel)>
        delivery) {
  respawn_delivery_ = std::move(delivery);
}

void GatheringFeature::deliver_respawn_changes(const std::vector<GatheringStateChange> &changes,
                                               std::uint64_t channel) {
  if (changes.empty()) {
    return;
  }

  if (respawn_delivery_) {
    respawn_delivery_(changes, channel);
    return;
  }

  if (!dropped_delivery_reported_) {
    dropped_delivery_reported_ = true;
    spdlog::error("Gathering respawn update dropped because no delivery is installed: {} node "
                  "changes",
                  changes.size());
  }
}

void GatheringFeature::publish_recovered_sources(std::vector<GatheringStateChange> changes,
                                                 std::uint64_t channel) {
  std::weak_ptr<GatheringFeature> weak = shared_from_this();
  boost::asio::post(strand_, [weak, changes = std::move(changes), channel] {
    if (const auto feature = weak.lock()) {
      feature->deliver_respawn_changes(changes, channel);
      feature->schedule_respawn();
    }
  });
}

void GatheringFeature::process_due_shared_sources(
    std::vector<SharedGatheringSourceRecord> records) {
  if (records.empty() || shared_transition_inflight_) {
    return;
  }
  shared_transition_inflight_ = true;
  std::weak_ptr<GatheringFeature> weak = shared_from_this();
  try {
    shared_source_store_.post_worker([weak, records = std::move(records)]() mutable {
      const auto feature = weak.lock();
      if (!feature) {
        return;
      }
      std::vector<GatheringStateChange> changes;
      std::size_t failed = 0;
      for (auto &record : records) {
        try {
          const auto epoch = unix_epoch_ticks_now();
          record = feature->advance_shared_source(std::move(record), epoch);
          const GatheringNodeKey node{record.key.map_id,
                                      static_cast<GatheringNodeKind>(record.key.kind),
                                      record.key.sector_id, record.key.local_index};
          const auto now = monotonic_ticks();
          std::uint64_t revision{};
          if (feature->nodes_->install_shared_source(node, record.state, epoch, now,
                                                     std::nullopt, false, &revision)) {
            changes.push_back({node, record.state.active, record.state.phase, now, revision});
          } else {
            ++failed;
          }
        } catch (...) {
          ++failed;
        }
      }
      try {
        boost::asio::post(feature->strand_, [weak, changes = std::move(changes), failed] {
          if (const auto current = weak.lock()) {
            current->shared_transition_inflight_ = false;
            const auto now = monotonic_ticks();
            current->shared_retry_not_before_ = failed
                ? std::optional<std::int64_t>(now + kEpochTicksPerSecond)
                : std::nullopt;
            constexpr auto kLogInterval = 60 * kEpochTicksPerSecond;
            if (failed && (!current->shared_last_retry_log_ ||
                           now - *current->shared_last_retry_log_ >= kLogInterval)) {
              spdlog::error("Shared gathering timer will retry {} unresolved sources", failed);
              current->shared_last_retry_log_ = now;
            }
            current->deliver_respawn_changes(changes, 0);
            current->schedule_respawn();
          }
        });
      } catch (...) {
        spdlog::critical("Shared gathering timer could not schedule recovery; stopping");
        std::terminate();
      }
    });
  } catch (...) {
    spdlog::critical("Shared gathering timer could not dispatch work; stopping");
    std::terminate();
  }
}

void GatheringFeature::schedule_respawn() {
  std::weak_ptr<GatheringFeature> weak_feature = weak_from_this();
  boost::asio::post(strand_, [weak_feature] {
    const auto feature = weak_feature.lock();
    if (!feature || feature->shared_transition_inflight_) {
      return;
    }

    std::optional<std::int64_t> next_tick;
    for (const auto &live : feature->live_nodes()) {
      const auto due = live.nodes->next_respawn_tick(live.channel == 0);
      if (due && (!next_tick || *due < *next_tick)) {
        next_tick = due;
      }
    }
    auto shared_due = feature->nodes_->next_shared_respawn_tick();
    if (shared_due && feature->shared_retry_not_before_ &&
        *shared_due < *feature->shared_retry_not_before_) {
      shared_due = feature->shared_retry_not_before_;
    }
    if (shared_due && (!next_tick || *shared_due < *next_tick)) {
      next_tick = shared_due;
    }
    if (!next_tick ||
        (feature->scheduled_respawn_tick_ && *feature->scheduled_respawn_tick_ <= *next_tick)) {
      return;
    }
    feature->scheduled_respawn_tick_ = *next_tick;
    const auto remaining_ticks = std::max<std::int64_t>(0, *next_tick - monotonic_ticks());
    feature->timer_.expires_after(NativeTicks(remaining_ticks));
    feature->timer_.async_wait(boost::asio::bind_executor(
        feature->strand_, [weak_feature](boost::system::error_code error) {
          const auto expired = weak_feature.lock();
          if (!expired || error == boost::asio::error::operation_aborted) {
            return;
          }
          if (error) {
            spdlog::critical("Gathering respawn timer failed; stopping for recovery");
            std::terminate();
          }

          expired->scheduled_respawn_tick_.reset();
          const auto now = monotonic_ticks();
          for (const auto &live : expired->live_nodes()) {
            const auto respawned = live.nodes->collect_respawns(now);
            expired->deliver_respawn_changes(respawned, live.channel);
          }
          if (expired->shared_retry_not_before_ && now < *expired->shared_retry_not_before_) {
            expired->schedule_respawn();
            return;
          }
          auto due = expired->nodes_->due_shared_sources(now, 128, expired->shared_due_cursor_);
          if (due.empty() && expired->shared_due_cursor_) {
            expired->shared_due_cursor_.reset();
            due = expired->nodes_->due_shared_sources(now, 128);
          }
          if (!due.empty()) {
            const auto &last = due.back().key;
            expired->shared_due_cursor_ = GatheringNodeKey{
                last.map_id, static_cast<GatheringNodeKind>(last.kind), last.sector_id,
                last.local_index};
            expired->process_due_shared_sources(std::move(due));
          } else {
            expired->shared_retry_not_before_.reset();
            expired->schedule_respawn();
          }
        }));
  });
}

} // namespace chronicles::gathering
