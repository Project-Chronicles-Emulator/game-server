#include "world/features/combat/combat_player_session.hpp"
#include "world/features/combat/death_reconciliation.hpp"

#include "world/core/world_request_opcodes.hpp"
#include "world/features/quests/quest_rules.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace chronicles::combat {
namespace {

using world_request::kMapLoadCompleteRequest;
using world_request::kReviveRequest;
constexpr std::uint16_t kStartPlayingRequest = 20086;
constexpr std::uint16_t kDeathNotification = 23;
constexpr std::uint16_t kItemsUpdateNotification = 169;

} // namespace

std::unique_ptr<CombatPlayerSession> CombatFeature::create_session(CombatPlayerContext &context) {
  return std::make_unique<CombatPlayerSession>(*this, context);
}

CombatPlayerSession::CombatPlayerSession(CombatFeature &feature, CombatPlayerContext &context)
    : feature_(feature), context_(context), movement_vitals_(feature.movement_rules()) {
}

CombatSession *CombatPlayerSession::current_simulation() noexcept {
  return simulation_.get();
}

const CombatSession *CombatPlayerSession::current_simulation() const noexcept {
  return simulation_.get();
}

bool CombatPlayerSession::has_simulation() const noexcept {
  return static_cast<bool>(simulation_);
}

bool CombatPlayerSession::has_world_mobs() const {
  return simulation_ && simulation_->has_world_mobs();
}

bool CombatPlayerSession::busy() const {
  return simulation_ && simulation_->busy();
}

bool CombatPlayerSession::gliding() const noexcept {
  return gliding_.active();
}

bool CombatPlayerSession::handles(const WorldGameplayRequest &request) const {
  return simulation_ && simulation_->handles(request.opcode, request.message);
}

bool CombatPlayerSession::decorate_spawn(GameplayPacket &packet) const {
  return !simulation_ || simulation_->decorate_spawn(packet);
}

std::vector<GameplayPacket> CombatPlayerSession::receive(const WorldGameplayRequest &request,
                                                         const WorldPosition &position,
                                                         std::int64_t ticks) {
  auto packets = select_weapon_for_skill(request);
  auto replies = simulation_->receive(request.opcode, request.message, position, ticks);
  packets.insert(packets.end(), std::make_move_iterator(replies.begin()),
                 std::make_move_iterator(replies.end()));
  context_.combat_settle_quest_events(*simulation_, position, ticks, packets);
  return packets;
}

std::vector<GameplayPacket>
CombatPlayerSession::select_weapon_for_skill(const WorldGameplayRequest &request) {
  if (!simulation_ || request.opcode != world_request::kStartSkillRequest) {
    return {};
  }
  const auto start = request.message.find("start_skill");
  if (start == request.message.end() || !start->is_object()) {
    return {};
  }
  const auto skill_id = start->find("skill_id");
  if (skill_id == start->end() || !skill_id->is_number_integer() ||
      !simulation_->is_swap_weapon_skill(skill_id->get<int>())) {
    return {};
  }
  // Captured official switches: equip_slot Weapon_2 selects the second weapon and an omitted
  // field (the proto3 default) selects Weapon_1.
  return context_.combat_select_weapon_for_skill(
      request.message.value("equip_slot", nlohmann::json(0)));
}

std::optional<std::int64_t> CombatPlayerSession::next_action_tick() const {
  auto deadline = movement_vitals_.next_action_tick();
  if (simulation_) {
    const auto combat_deadline = simulation_->next_action_tick();
    if (combat_deadline && (!deadline || *combat_deadline < *deadline)) {
      deadline = combat_deadline;
    }
  }
  return deadline;
}

bool CombatPlayerSession::start_tutorial_combat(const Character &character, std::int64_t wire_id,
                                                bool has_followup) {
  if (!feature_.data() || !context_.combat_has_character_stats() || has_followup) {
    return false;
  }
  Character wire_character = character;
  wire_character.id = wire_id;
  simulation_ = std::make_unique<CombatSession>(
      feature_.data(), context_.combat_base_tutorial_data(), wire_character,
      context_.combat_character_stats(character), context_.combat_tutorial_progress());
  simulation_->set_item_inventory(context_.combat_item_inventory());
  return true;
}

void CombatPlayerSession::end_tutorial_combat() {
  reset_movement_vitals();
  simulation_.reset();
}

void CombatPlayerSession::on_map_departure(bool preserve_simulation, bool preserve_movement) {
  // Detach can fail while preparing a shared-monster handoff. Leave the current
  // observer and movement state intact so the caller can retry or close safely.
  if (simulation_) {
    simulation_->detach_world_mobs(context_.combat_monotonic_ticks());
  }

  region_handoff_pending_ = preserve_simulation && preserve_movement;
  if (!region_handoff_pending_) {
    reset_movement_vitals();
  }
  if (simulation_) {
    simulation_->set_defeat_observer({});
    simulation_->set_player_map(std::nullopt);
  }
  if (!preserve_simulation) {
    simulation_.reset();
  }
}

void CombatPlayerSession::detach_world_mobs() {
  if (simulation_) {
    simulation_->detach_world_mobs(context_.combat_monotonic_ticks());
  }
}

bool CombatPlayerSession::has_pending_world_defeats() const {
  return simulation_ && !simulation_->pending_world_defeats().empty();
}

void CombatPlayerSession::retry_pending_world_monster_rewards(
    std::vector<GameplayPacket> &packets) {
  if (!has_pending_world_defeats()) {
    return;
  }
  const auto position = simulation_->pending_world_defeats().front().defeat.position;
  commit_world_monster_rewards(position, packets);
}

void CombatPlayerSession::ensure_world_mobs(const WorldPosition &position) {
  if (context_.combat_has_tutorial() || context_.combat_in_housing() || !simulation_ ||
      simulation_->has_world_mobs() || !context_.combat_has_handoff()) {
    return;
  }
  auto population =
      feature_.population_for(position.map_id, context_.combat_character_id(), context_);
  if (!population) {
    return;
  }
  const bool instance_copy = static_cast<bool>(context_.combat_instance_copy(position.map_id));
  simulation_->attach_world_mobs(std::move(population), instance_copy,
                                 context_.combat_monotonic_ticks());
  if (context_.combat_uses_durable_monster_rewards()) {
    simulation_->set_world_defeat_capture([this](const MonsterDefeat &defeat) {
      return context_.combat_snapshot_monster_rewards(defeat);
    });
  }
  context_.combat_attach_world_loot();
}

void CombatPlayerSession::synchronize_player_state() {
  context_.combat_publish_snapshot();
}

void CombatPlayerSession::publish_committed_player_life() {
  // Only durable death/revival calls this effect. Ordinary simulation snapshots must not
  // change what a later observer sees before the life transition has committed.
  context_.combat_publish_committed_life(simulation_->player_dead(), simulation_->player_stats());
}

std::vector<GameplayPacket> CombatPlayerSession::advance_tutorial(std::int64_t ticks,
                                                                  const WorldPosition &position) {
  return context_.combat_advance_tutorial(*simulation_, ticks, position);
}

PvpPresenceUpdate CombatPlayerSession::update_pvp_presence(bool entered_world,
                                                           bool services_entered,
                                                           const WorldPosition &position,
                                                           int presence_map, bool in_tutorial) {
  std::uint64_t instance_id{};
  if (presence_map != 0) {
    // Housing channels and instance copies use negative presence maps; PvP keeps the real map
    // and an explicit key space so players in other channels or copies are never targets.
    instance_id = (std::uint64_t{1} << 63) |
                  static_cast<std::uint64_t>(-static_cast<std::int64_t>(presence_map));
  } else if (in_tutorial) {
    instance_id = static_cast<std::uint64_t>(context_.combat_character_id());
  }
  PvpPresenceUpdate update;
  if (entered_world && !services_entered) {
    update.registration = feature_.player_pvp().enter(
        context_.combat_connection_id(),
        context_.combat_wire_player_id(context_.combat_character_id()), position, instance_id);
  } else if (services_entered) {
    auto reset = feature_.player_pvp().update_position(context_.combat_connection_id(), position,
                                                       instance_id);
    update.notification = std::move(reset.notification);
  }
  return update;
}

void CombatPlayerSession::reset_movement_vitals() {
  region_handoff_pending_ = false;
  fall_tracker_.reset();
  gliding_.reset();
  if (simulation_) {
    const auto ticks = context_.combat_monotonic_ticks();
    if (auto stopped = movement_vitals_.stop_swimming(*simulation_, ticks)) {
      pending_movement_reset_packets_.push_back(std::move(*stopped));
    }
    simulation_->set_movement_recovery_blocked(false, false, ticks);
  }
  movement_vitals_.reset();
}

void CombatPlayerSession::ensure_player_vitals(const Character &character,
                                               const WorldProgress &progress) {
  if (simulation_ || !feature_.data() || !context_.combat_has_character_stats()) {
    return;
  }

  Character wire_character = character;
  wire_character.id = static_cast<std::int64_t>(context_.combat_wire_player_id(character.id));
  simulation_ =
      std::make_unique<CombatSession>(feature_.data(), nullptr, wire_character,
                                      context_.combat_character_stats(character), progress);
  simulation_->set_item_inventory(context_.combat_item_inventory());
}

std::vector<GameplayPacket> CombatPlayerSession::revive_player() {
  if (!simulation_ || !simulation_->player_dead()) {
    spdlog::info("Native world revive request ignored: player is not dead");
    return {};
  }

  if (context_.combat_has_handoff()) {
    try {
      persist_player_revival();
    } catch (const std::exception &error) {
      spdlog::warn("Native world revive request refused: {}", error.what());
      return {};
    }
  }

  auto packets = simulation_->revive_if_dead(context_.combat_monotonic_ticks());
  reset_movement_vitals();
  synchronize_player_state();
  publish_committed_player_life();
  return packets;
}

std::optional<WorldPosition> CombatPlayerSession::revival_position() const {
  if (const auto last_position = context_.combat_last_position()) {
    return last_position;
  }

  const auto character = context_.combat_character();
  if (character) {
    return character->world_position;
  }
  return std::nullopt;
}

std::vector<GameplayPacket>
CombatPlayerSession::persist_player_revival(std::optional<WorldPosition> destination,
                                            int revive_task_id) {
  const auto previous = context_.combat_has_tutorial() ? context_.combat_tutorial_progress()
                                                       : context_.combat_stored_progress();
  if (!previous.dead || previous.revision == std::numeric_limits<std::int64_t>::max()) {
    throw std::runtime_error("Player revival requires a dead, revisioned progress record");
  }

  const auto position = destination ? destination : revival_position();
  if (!position) {
    throw std::runtime_error("Player revival has no known or persisted world position");
  }
  auto committed = previous;
  ++committed.revision;
  committed.dead = false;
  std::vector<GameplayPacket> packets;
  if (revive_task_id != 0) {
    const auto character = context_.combat_character();
    if (!context_.combat_has_revival_supplies() || !character) {
      throw std::runtime_error("Revival supply requires verified shrine and character data");
    }
    try {
      packets = context_.combat_revival_supplies(revive_task_id, committed, *character);
    } catch (const quest_rules::RuleError &error) {
      throw std::runtime_error("Invalid revival supply state: " + error.code);
    }
  }
  const bool saved = destination
                         ? context_.combat_commit_shrine_revival(previous, committed, *position)
                         : context_.combat_commit_revival(previous.revision, *position);
  if (!saved) {
    throw std::runtime_error("Player revival revision conflict; the player stays dead");
  }
  if (context_.combat_has_tutorial()) {
    context_.combat_accept_tutorial_progress(std::move(committed));
  }
  return packets;
}

CombatPlayerSession::DeathCommitPlan CombatPlayerSession::prepare_death_commit_plan() const {
  const auto previous = context_.combat_has_tutorial() ? context_.combat_tutorial_progress()
                                                       : context_.combat_stored_progress();
  if (previous.dead || !simulation_->player_dead() || !feature_.has_catalog()) {
    throw std::runtime_error("Player death cannot be persisted from this session");
  }

  const auto character = context_.combat_character();
  if (!character) {
    throw std::runtime_error("Player death character ownership was lost");
  }

  DeathCommitPlan plan;
  plan.before = previous;
  plan.attempt_revision = previous.revision;
  for (const auto &item : character->items) {
    if (!item.equip_slot) {
      continue;
    }
    const auto definition = feature_.equipment_definition(item.cms_id);
    if (!definition) {
      throw std::runtime_error("Equipped item has no catalog durability definition");
    }
    const auto maximum = std::stoi(definition->value("maxDurability", std::string("0")));
    if (maximum < 0) {
      throw std::runtime_error("Equipped item has invalid maximum durability");
    }

    const auto loss = (static_cast<std::int64_t>(maximum) * 3) / 100;
    const int durability = static_cast<int>(
        std::max<std::int64_t>(0, static_cast<std::int64_t>(item.durability) - loss));
    plan.equipped_before.push_back(item);
    plan.changes.push_back({item.id, item.cms_id, *item.equip_slot, item.durability, durability});
    if (durability == item.durability) {
      continue;
    }
    plan.changed_items.push_back({{"item_id", std::to_string(item.id)},
                                  {"item_cms_id", item.cms_id},
                                  {"durability", durability},
                                  {"grade", item.grade},
                                  {"polish_step", item.polish_step},
                                  {"equip_slot", *item.equip_slot}});
  }
  return plan;
}

void CombatPlayerSession::persist_player_death(std::vector<GameplayPacket> &packets) {
  if (!context_.combat_has_handoff() || !simulation_) {
    return;
  }
  if (has_pending_death()) {
    throw std::runtime_error("Previous player death outcome requires reconciliation");
  }
  if (!simulation_->player_dead()) {
    return;
  }

  // The listener reserves this slot before gameplay, so activating it cannot allocate. It
  // protects preparation failures as well as writes with an uncertain acknowledgement.
  death_preparation_pending_ = true;
  if (!context_.combat_register_pending_death()) {
    context_.combat_pending_death_updated();
    throw std::runtime_error("Player death recovery owner could not be registered");
  }
  death_owner_registered_ = true;
  try {
    const auto player_id =
        std::to_string(context_.combat_wire_player_id(context_.combat_character_id()));
    const bool player_died = std::ranges::any_of(packets, [&](const GameplayPacket &packet) {
      return packet.opcode == kDeathNotification &&
             packet.message.value("target_unit_id", std::string()) == player_id;
    });
    if (!player_died) {
      const auto progress = context_.combat_has_tutorial() ? context_.combat_tutorial_progress()
                                                          : context_.combat_stored_progress();
      if (!progress.dead) {
        throw std::runtime_error("Player death notification is missing; recovery is required");
      }
      if (!context_.combat_forget_pending_death()) {
        throw std::runtime_error("Player death recovery owner could not be retired");
      }
      death_preparation_pending_ = false;
      death_owner_registered_ = false;
      return;
    }

    pending_death_.emplace(prepare_death_commit_plan());
    if (!context_.combat_commit_death(pending_death_->attempt_revision, pending_death_->changes)) {
      throw std::runtime_error("Player death revision conflict; reconnect to reload state");
    }
    auto committed = pending_death_->before;
    ++committed.revision;
    committed.dead = true;
    if (context_.combat_has_tutorial()) {
      context_.combat_accept_tutorial_progress(std::move(committed));
    }
    reset_movement_vitals();
    synchronize_player_state();
    publish_committed_player_life();
    if (!pending_death_->changed_items.empty()) {
      // Observed (3 of 3 official deaths): the equipment-durability ItemsUpdateNoti follows the
      // Demise state and precedes the lethal DamageNoti.
      const auto lethal_damage = std::ranges::find_if(packets, [&](const GameplayPacket &packet) {
        return packet.opcode == 19 &&
               packet.message.value("target_unit_id", std::string()) == player_id;
      });
      GameplayPacket items_update{
          kItemsUpdateNotification,
          {{"items", pending_death_->changed_items}, {"equip", nlohmann::json::object()}}};
      packets.insert(lethal_damage, std::move(items_update));
    }
    if (!context_.combat_forget_pending_death()) {
      throw std::runtime_error("Player death recovery owner could not be retired");
    }
    pending_death_.reset();
    death_preparation_pending_ = false;
    death_owner_registered_ = false;
  } catch (...) {
    // Registration owns the character even when a write may have committed. The context
    // seals stale saves before scheduling reconciliation, and never throws from this hook.
    context_.combat_pending_death_updated();
    throw;
  }
}

CombatPlayerSession::DeathRecovery CombatPlayerSession::reconcile_pending_death() {
  if (!has_pending_death()) {
    return DeathRecovery::none;
  }
  if (!death_owner_registered_) {
    // A refused registration has a definite no-owner outcome. Keep the pending
    // death untouched until the reserved admission can be activated on a retry.
    if (!context_.combat_register_pending_death()) {
      return DeathRecovery::retry;
    }
    death_owner_registered_ = true;
    context_.combat_pending_death_updated();
  }
  if (pending_death_ && pending_death_->reconciled_committed) {
    return DeathRecovery::committed_reconnect;
  }

  try {
    if (!pending_death_) {
      pending_death_.emplace(prepare_death_commit_plan());
    }
    const auto snapshot = context_.combat_fence_death_reconciliation();
    if (!snapshot) {
      return DeathRecovery::conflict;
    }
    auto attempt_before = pending_death_->before;
    attempt_before.revision = pending_death_->attempt_revision;
    const auto outcome = classify_fenced_death(
        attempt_before, pending_death_->equipped_before, pending_death_->changes,
        snapshot->progress, snapshot->equipped_items);
    if (outcome == DeathReconciliationOutcome::conflict) {
      return DeathRecovery::conflict;
    }
    if (outcome == DeathReconciliationOutcome::not_committed) {
      // The confirmed fence invalidates the previous attempt. Record the new revision
      // before retrying so a lost retry acknowledgement can itself be fenced safely.
      pending_death_->attempt_revision = snapshot->progress.revision;
      if (!context_.combat_commit_death(pending_death_->attempt_revision,
                                        pending_death_->changes)) {
        return DeathRecovery::retry;
      }
    }
    pending_death_->reconciled_committed = true;
    return DeathRecovery::committed_reconnect;
  } catch (const std::exception &error) {
    spdlog::warn("Player death reconciliation remains unresolved: {}", error.what());
    return DeathRecovery::retry;
  } catch (...) {
    spdlog::warn("Player death reconciliation remains unresolved: unknown error");
    return DeathRecovery::retry;
  }
}

bool CombatPlayerSession::has_pending_death() const noexcept {
  return death_preparation_pending_ || pending_death_.has_value();
}

bool CombatPlayerSession::acknowledge_reconciled_death() noexcept {
  if (!pending_death_ || !pending_death_->reconciled_committed ||
      !context_.combat_forget_pending_death()) {
    return false;
  }
  pending_death_.reset();
  death_preparation_pending_ = false;
  death_owner_registered_ = false;
  return true;
}

void CombatPlayerSession::observe_map_load_vitals(FrameContext &frame, bool has_response) {
  const auto *gameplay_request = frame.request();

  if (gameplay_request && gameplay_request->opcode == kMapLoadCompleteRequest && has_response &&
      context_.combat_has_handoff() && !simulation_) {
    if (const auto character = context_.combat_character()) {
      const auto progress = context_.combat_has_tutorial() ? context_.combat_tutorial_progress()
                                                           : context_.combat_stored_progress();
      ensure_player_vitals(*character, progress);
    }
  }
}

void CombatPlayerSession::observe_shared_map_mobs(FrameContext &frame, bool has_response) {
  const auto *gameplay_request = frame.request();
  auto &party_position = frame.party_position;
  auto &session_packets = frame.session_packets;

  if (!gameplay_request || !has_response || !party_position || context_.combat_has_tutorial()) {
    return;
  }

  const bool map_loaded = gameplay_request->opcode == kMapLoadCompleteRequest;
  const bool started_playing = gameplay_request->opcode == kStartPlayingRequest;
  if (map_loaded && simulation_) {
    simulation_->set_player_map(party_position->map_id);
  }

  const bool delayed = feature_.announce_population_after_start(party_position->map_id);
  if ((map_loaded && delayed) || (!map_loaded && !(started_playing && delayed))) {
    return;
  }

  // The captured instance entry sends NPC UnitAddNoti after StartPlayingRes. Sending the
  // initial nubs while the client is still loading can leave attackable monsters invisible.
  ensure_world_mobs(*party_position);
  if (context_.combat_uses_ordered_schedule()) {
    // Entry attaches the population without simulating it. The completion registers its
    // first deadline before another request can enter; the admitted timer sends the nubs.
    return;
  }
  auto mob_packets = advance_session_combat(context_.combat_monotonic_ticks(), *party_position);
  session_packets.insert(session_packets.end(), std::make_move_iterator(mob_packets.begin()),
                         std::make_move_iterator(mob_packets.end()));
}

void CombatPlayerSession::observe_movement_vitals(
    FrameContext &frame, const std::optional<nlohmann::json> &movement_to_broadcast) {
  const auto *gameplay_request = frame.request();
  auto &party_position = frame.party_position;
  auto &session_packets = frame.session_packets;

  // Fall impacts share player vitals across private and shared maps. Only movement
  // accepted by the handshake contributes; persist a lethal impact before encoding it.
  if (gameplay_request && simulation_ && party_position) {
    if (gameplay_request->opcode == kMapLoadCompleteRequest) {
      if (region_handoff_pending_) {
        fall_tracker_.continue_on_map(party_position->map_id);
        movement_vitals_.continue_on_map(party_position->map_id,
                                         context_.combat_monotonic_ticks());
        region_handoff_pending_ = false;
      } else {
        reset_movement_vitals();
        fall_tracker_.observe(*party_position, "MoveMode_Normal");
      }
    }
    if (movement_to_broadcast) {
      gliding_.observe_movement(*movement_to_broadcast);
      const auto ticks = context_.combat_monotonic_ticks();
      if (context_.combat_riding()) {
        reset_movement_vitals();
      } else {
        auto movement_packets =
            movement_vitals_.move(*movement_to_broadcast, *party_position, *simulation_, ticks);
        interrupt_gathering_on_damage(movement_packets, ticks);
        session_packets.insert(session_packets.end(),
                               std::make_move_iterator(movement_packets.begin()),
                               std::make_move_iterator(movement_packets.end()));
      }
      if (simulation_->player_dead()) {
        reset_movement_vitals();
      } else {
        const auto height_mm = fall_tracker_.observe(
            *party_position, movement_to_broadcast->value("mode", std::string("MoveMode_Normal")));
        if (height_mm) {
          const auto maximum_health = simulation_->player_stats().value("18", 0);
          const auto damage = feature_.fall_rules().damage(*height_mm, maximum_health);
          auto impact = simulation_->apply_fall_damage(damage, *party_position, ticks);
          interrupt_gathering_on_damage(impact, ticks);
          session_packets.insert(session_packets.end(), std::make_move_iterator(impact.begin()),
                                 std::make_move_iterator(impact.end()));
        }
      }
      persist_player_death(session_packets);
      synchronize_player_state();
    }
  }
}

bool CombatPlayerSession::handle_shared_revival(FrameContext &frame) {
  const auto *gameplay_request = frame.request();
  auto &party_position = frame.party_position;
  auto &session_packets = frame.session_packets;

  if (gameplay_request && simulation_ && party_position) {
    if (!context_.combat_has_tutorial() && gameplay_request->opcode == kReviveRequest) {
      session_packets = context_.combat_revive_at_shrine(*gameplay_request);
      return true;
    }
  }

  return false;
}

void CombatPlayerSession::observe_riding_vitals_reset() {
  if (context_.combat_riding()) {
    reset_movement_vitals();
  }
  for (const auto &packet : pending_movement_reset_packets_) {
    context_.combat_send_owner(packet);
  }
  pending_movement_reset_packets_.clear();
}

bool CombatPlayerSession::handle_private_combat(FrameContext &frame) {
  const auto *gameplay_request = frame.request();
  auto &gameplay_position = frame.gameplay_position;
  auto &session_packets = frame.session_packets;
  auto &action_deadline = frame.action_deadline;

  if (gameplay_request && (gameplay_request->opcode == world_request::kClientProjectileHitRequest ||
                           gameplay_request->opcode == world_request::kUpdateProjectileRequest)) {
    if (simulation_ && frame.party_position) {
      const auto observers = simulation_->receive_projectile_report(
          gameplay_request->opcode, gameplay_request->message, *frame.party_position,
          context_.combat_monotonic_ticks());
      context_.combat_send_observers(observers);
    }
    return true;
  }

  if (gameplay_request && (gameplay_request->opcode == world_request::kStartRootMotionRequest ||
                           gameplay_request->opcode == world_request::kAdaptedRotationRequest)) {
    if (!simulation_ || !frame.party_position) {
      session_packets.push_back(
          {2,
           {{"ec", "InvalidState"},
            {"packet_seq", {{"seq", std::to_string(gameplay_request->sequence)}}}}});
      return true;
    }

    auto outcome = simulation_->receive_player_motion(
        gameplay_request->opcode, gameplay_request->message, gameplay_request->sequence,
        *frame.party_position, context_.combat_monotonic_ticks());
    session_packets.insert(session_packets.end(), std::make_move_iterator(outcome.own.begin()),
                           std::make_move_iterator(outcome.own.end()));
    context_.combat_send_observers(outcome.observers);
    return true;
  }

  // Gliding also belongs to players on empty maps and in the tutorial. It does not require
  // an NPC encounter, and must be refused explicitly during loading instead of falling through.
  if (gameplay_request && gameplay_request->opcode == world_request::kStartGlidingRequest) {
    const auto now = context_.combat_monotonic_ticks();
    GlidingConditions conditions;
    conditions.dead = !simulation_ || simulation_->player_dead();
    conditions.riding = context_.combat_riding();
    conditions.using_skill = busy();
    conditions.interacting = context_.combat_other_action_busy();
    conditions.crowd_controlled = simulation_ && simulation_->player_stunned(now);
    if (auto refusal =
            gliding_.start(gameplay_request->message, frame.party_position, conditions, now)) {
      session_packets.push_back(std::move(*refusal));
    }
    return true;
  }

  // Unpopulated fields still own the player's item actions and their deadlines.
  // Private GM encounters use the same path without creating a tutorial instance.
  if (!context_.combat_has_tutorial() && simulation_ &&
      (simulation_->private_encounter_active() || simulation_->owns_item_use()) &&
      gameplay_request && gameplay_position &&
      simulation_->handles(gameplay_request->opcode, gameplay_request->message)) {
    const auto ticks = context_.combat_monotonic_ticks();
    context_.combat_sync_protection();
    auto packets = advance_session_combat(ticks, *gameplay_position);
    auto selection = select_weapon_for_skill(*gameplay_request);
    packets.insert(packets.end(), std::make_move_iterator(selection.begin()),
                   std::make_move_iterator(selection.end()));
    auto replies = simulation_->receive(gameplay_request->opcode, gameplay_request->message,
                                        *gameplay_position, ticks);
    interrupt_gathering_on_damage(replies, ticks);
    packets.insert(packets.end(), std::make_move_iterator(replies.begin()),
                   std::make_move_iterator(replies.end()));
    auto due = advance_session_combat(ticks, *gameplay_position);
    packets.insert(packets.end(), std::make_move_iterator(due.begin()),
                   std::make_move_iterator(due.end()));
    synchronize_player_state();
    persist_player_death(packets);
    session_packets.insert(session_packets.end(), std::make_move_iterator(packets.begin()),
                           std::make_move_iterator(packets.end()));
    action_deadline = context_.combat_next_gameplay_tick();
    return true;
  }

  return false;
}

void CombatPlayerSession::commit_world_monster_rewards(const WorldPosition &position,
                                                       std::vector<GameplayPacket> &packets) {
  if (!simulation_ || !simulation_->has_world_mobs()) {
    return;
  }
  while (!simulation_->pending_world_defeats().empty()) {
    auto pending = simulation_->pending_world_defeats().front();
    try {
      if (!pending.finalized) {
        bool snapshots_ready = true;
        for (std::size_t index = 0; index < pending.capture.recipients.size(); ++index) {
          const auto snapshot = pending.capture.quest_captures[index]->resolve_filtered(
              pending.capture.relevant_quest_ids);
          if (!snapshot) {
            snapshots_ready = false;
            break;
          }
          pending.capture.recipients[index].quest_attempts = *snapshot;
        }
        if (!snapshots_ready) {
          context_.combat_schedule_monster_reward_retry();
          break;
        }
        if (!context_.combat_enqueue_monster_rewards_with_script(pending.capture.recipients,
                                                                   pending.capture.boss_script)) {
          spdlog::error("World monster reward batch conflicts with a stored event");
          context_.combat_schedule_monster_reward_retry();
          break;
        }
        simulation_->finalize_world_defeat(pending.defeat.reward_event_id,
                                            context_.combat_monotonic_ticks());
        if (simulation_->pending_world_defeats().empty() ||
            !simulation_->pending_world_defeats().front().finalized) {
          throw std::runtime_error("Durable monster defeat could not be finalized");
        }
      }
      if (!simulation_->stage_finalized_world_defeat(pending.defeat.reward_event_id, packets)) {
        throw std::runtime_error("Finalized monster packet batch is unavailable");
      }
      if (!context_.combat_mark_monster_reward_ready(pending.defeat.reward_event_id)) {
        throw std::runtime_error("Durable monster reward event is missing");
      }
      if (pending.capture.boss_script &&
          !context_.combat_publish_boss_script(*pending.capture.boss_script)) {
        context_.combat_schedule_monster_reward_retry();
        break;
      }
      context_.combat_notify_monster_rewards(pending.capture.recipients);
      if (!simulation_->acknowledge_world_defeat(pending.defeat.reward_event_id)) {
        throw std::runtime_error("Finalized monster reward could not be acknowledged");
      }
      if (pending.capture.boss_script) {
        context_.combat_retire_boss_script(*pending.capture.boss_script);
      }
    } catch (const std::exception &error) {
      // A lost database acknowledgement can follow COMMIT. Retry the identical immutable
      // batch; the repository verifies the complete event payload before accepting replay.
      spdlog::error("World monster reward batch outcome unresolved: {}", error.what());
      context_.combat_schedule_monster_reward_retry();
      break;
    }
  }
  for (const auto &defeat : simulation_->take_world_defeats()) {
    context_.combat_record_monster_reward(position, defeat, packets);
  }
}

std::vector<GameplayPacket>
CombatPlayerSession::advance_session_combat(std::int64_t ticks, const WorldPosition &position) {
  if (!simulation_) {
    return {};
  }
  const world_metrics::ScopedTimer advance_timer(world_metrics::HistogramId::CombatAdvanceUs);
  auto packets = context_.combat_has_tutorial()
                     ? context_.combat_advance_tutorial(*simulation_, ticks, position)
                     : simulation_->advance_private_encounter(ticks, position);
  interrupt_gathering_on_damage(packets, ticks);
  context_.combat_settle_quest_events(*simulation_, position, ticks, packets);
  commit_world_monster_rewards(position, packets);
  return packets;
}

void CombatPlayerSession::interrupt_gathering_on_damage(std::vector<GameplayPacket> &packets,
                                                        std::int64_t ticks) {
  const auto player_id =
      std::to_string(context_.combat_wire_player_id(context_.combat_character_id()));
  const bool damaged = std::ranges::any_of(packets, [&](const GameplayPacket &packet) {
    return packet.opcode == 19 &&
           packet.message.value("target_unit_id", std::string()) == player_id &&
           packet.message.value("base_damage_amount", 0) > 0;
  });
  if (damaged) {
    auto interrupted = context_.combat_interrupt_gathering(ticks);
    packets.insert(packets.end(), std::make_move_iterator(interrupted.begin()),
                   std::make_move_iterator(interrupted.end()));
  }
}

void CombatPlayerSession::player_pvp_command(std::optional<bool> requested,
                                             const std::function<void(std::string)> &completion) {
  try {
    if (context_.combat_closed() || !context_.combat_services_entered()) {
      completion("Enter the game world before using /pvp.");
      return;
    }
    if (!requested) {
      completion(feature_.player_pvp().enabled(context_.combat_connection_id()) ? "PvP flag: ON."
                                                                                : "PvP flag: OFF.");
      return;
    }

    bool player_alive{};
    if (simulation_) {
      player_alive = !simulation_->player_dead();
    } else if (context_.combat_has_tutorial()) {
      player_alive = !context_.combat_tutorial_progress().dead;
    } else if (context_.combat_has_handoff()) {
      player_alive = !context_.combat_stored_progress().dead;
    }
    const auto result =
        feature_.player_pvp().set_enabled(context_.combat_connection_id(), *requested, player_alive,
                                          context_.combat_other_action_busy());
    if (result.status == PlayerPvpStatus::player_dead) {
      completion("You cannot change the PvP flag while dead.");
      return;
    }
    if (result.status == PlayerPvpStatus::player_in_combat) {
      completion("Finish your current action before changing the PvP flag.");
      return;
    }
    if (result.status == PlayerPvpStatus::unknown_player) {
      completion("PvP is unavailable for this world connection.");
      return;
    }
    if (result.notification) {
      const auto &packet = *result.notification;
      context_.combat_send_owner(packet);
      context_.combat_send_observers({packet});
    }
    completion(result.enabled ? "PvP flag enabled." : "PvP flag disabled.");
  } catch (const std::exception &) {
    completion("The server could not update your PvP flag. Try again.");
  }
}

void CombatPlayerSession::advance_gameplay_tick(std::int64_t ticks, const WorldPosition &position,
                                                std::vector<GameplayPacket> &packets) {
  if (simulation_) {
    if (context_.combat_riding()) {
      reset_movement_vitals();
    }
    packets.insert(packets.end(),
                   std::make_move_iterator(pending_movement_reset_packets_.begin()),
                   std::make_move_iterator(pending_movement_reset_packets_.end()));
    pending_movement_reset_packets_.clear();
    context_.combat_sync_protection();
    auto movement_packets = movement_vitals_.advance(position, *simulation_, ticks);
    interrupt_gathering_on_damage(movement_packets, ticks);
    packets.insert(packets.end(), std::make_move_iterator(movement_packets.begin()),
                   std::make_move_iterator(movement_packets.end()));
    auto combat_packets = advance_session_combat(ticks, position);
    packets.insert(packets.end(), std::make_move_iterator(combat_packets.begin()),
                   std::make_move_iterator(combat_packets.end()));
    synchronize_player_state();
  }
}

} // namespace chronicles::combat
