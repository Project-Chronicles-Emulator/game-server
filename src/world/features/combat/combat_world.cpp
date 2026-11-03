#include "common/clock_ticks.hpp"
#include "world/features/combat/combat.hpp"
#include "world/features/combat/combat_timeline.hpp"
#include "world/features/npc/npc_patrol.hpp"
#include "world/core/unit_registry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <openssl/rand.h>
#include <stdexcept>
#include <type_traits>

namespace chronicles {
namespace {

using Json = nlohmann::json;
constexpr std::int64_t kTicksPerMillisecond = 10000;
constexpr std::int64_t kWorldPollMs = 100;
constexpr std::int64_t kWorldCorpseMs = 90000;
constexpr double kSightRangeMm = 150000;
constexpr std::size_t kMaxWorldJournal = 4096;
constexpr std::size_t kMaxWorldSpawns = 16384;
constexpr std::size_t kMaxWorldLives = kMaxWorldSpawns + 8192;
constexpr std::size_t kMaxCombatTimelineEntries = 65536;

std::uint64_t next_world_unit_id() {
  std::uint64_t random{};
  if (RAND_bytes(reinterpret_cast<unsigned char *>(&random), sizeof(random)) != 1) {
    throw std::runtime_error("Cannot allocate a world monster life identity");
  }
  // The client's MoveNoti dispatch zeroes the high dword before actor lookup. A 64-bit
  // protobuf field does not make a high-dword identity usable by that lookup.
  return 0x90000002ULL | (random & 0x0FFFFFF0ULL);
}

std::uint64_t next_world_credit_id() {
  std::uint64_t random{};
  if (RAND_bytes(reinterpret_cast<unsigned char *>(&random), sizeof(random)) != 1) {
    throw std::runtime_error("Cannot allocate a world monster reward identity");
  }
  // Keep persistence receipts independent from the smaller, transient wire namespace.
  return (random & 0x7FFFFFFFFFFFFFFFULL) | (1ULL << 62);
}

std::string next_world_reward_event_id() {
  std::array<unsigned char, 16> random{};
  if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) {
    throw std::runtime_error("Cannot allocate a world monster reward event identity");
  }

  constexpr std::string_view kHexDigits = "0123456789abcdef";
  std::string result;
  result.reserve(random.size() * 2);
  for (const auto byte : random) {
    result.push_back(kHexDigits[byte >> 4]);
    result.push_back(kHexDigits[byte & 0x0f]);
  }
  return result;
}

Json wire_position(const WorldPosition &position) {
  return {{"millimeter_x", position.x_mm},
          {"millimeter_y", position.y_mm},
          {"millimeter_z", position.z_mm}};
}

bool public_combat_packet(std::uint16_t opcode) {
  // Only visual combat notifications. Inventory, quest progress, EXP and private responses
  // never enter the shared journal. These are the recovered world namespace numbers.
  switch (opcode) {
  case 10:
  case 11:
  case 14:
  case 15:
  case 17:
  case 19:
  case 23:
  case 32:
  case 40:
  case 42:
  case 44:
  case 45:
  case 47:
  case 49:
  case 50:
  case 51:
  case 52:
  case 54:
  case 64:
  case 98:
  case 106:
  case 114:
  case 177:
    return true;
  default:
    return false;
  }
}

} // namespace

struct CombatSession::WorldMobs {
  struct Slot {
    Monster prototype;
    Json definition;
    std::uint64_t current_unit{};
    std::optional<std::int64_t> respawn_tick;
    std::shared_ptr<const NpcPatrolRoute> patrol;
    std::int64_t patrol_elapsed{};
    std::optional<std::int64_t> patrol_updated_tick;
    bool patrol_paused{};
  };

  struct JournalEntry {
    std::uint64_t sequence{};
    std::uint64_t source{};
    GameplayPacket packet;
  };

  world_metrics::RecursiveTimedMutex mutex;
  int map_id{};
  std::shared_ptr<const CombatData> data;
  std::map<std::uint64_t, Monster> monsters;
  std::map<std::uint64_t, Json> definitions;
  // Some client consumers resolve the low 32 bits. Keep those unique among every life in
  // this channel, including removed corpses, and outside the civilian allocation prefix.
  std::set<std::uint32_t> allocated_low_ids;
  std::vector<Slot> slots;
  std::map<std::uint64_t, std::int64_t> corpses;
  std::deque<JournalEntry> journal;
  std::uint64_t sequence{};
  std::shared_ptr<combat::CombatTimeline> timeline =
      std::make_shared<combat::CombatTimeline>(kMaxCombatTimelineEntries, true);
  std::map<std::uint64_t, const CombatSession *> milestone_owners;
};

std::shared_ptr<CombatSession::WorldMobs>
CombatSession::create_world_mobs(const Json &map, std::shared_ptr<const CombatData> data) {
  auto world = std::make_shared<WorldMobs>();
  world->map_id = map.at("map_id").get<int>();
  if (!data || world->map_id <= 0 || world->map_id >= (1 << 24) || !map.at("spawns").is_array() ||
      map.at("spawns").size() > kMaxWorldSpawns) {
    throw std::runtime_error("Invalid world monster map or population bound");
  }
  world->data = std::move(data);
  for (const auto &definition : map.at("spawns")) {
    const auto id_text = definition.at("unit_id").get<std::string>();
    const auto id = parse_unit_id_text(id_text);
    const int cms = definition.at("cms_id").get<int>();
    const auto &point = definition.at("position");
    const auto stats = world->data->npc_stats.find(std::to_string(cms));
    const auto respawn_ms = definition.at("respawn_ms").get<std::int64_t>();
    if (!id || (*id & 15) != 2 || !point.is_array() || point.size() != 3 ||
        stats == world->data->npc_stats.end() || stats->value("18", 0) <= 0 || respawn_ms < 0 ||
        respawn_ms > 86400000) {
      throw std::runtime_error("Invalid world monster spawn " + id_text);
    }
    WorldPosition position{world->map_id, point.at(0).get<int>(), point.at(1).get<int>(),
                           point.at(2).get<int>(), definition.at("yaw").get<float>()};
    if (!std::isfinite(position.yaw)) {
      throw std::runtime_error("Invalid world monster spawn yaw");
    }
    Monster monster{cms, *stats, position, true};
    monster.reward_credit_id = next_world_credit_id();
    monster.reward_event_id = next_world_reward_event_id();
    monster.stats["69"] = stats->at("18");
    if (!monster.stats.contains("84")) {
      monster.stats["84"] = 1000;
    }
    const auto faction = definition.at("faction").get<std::string>();
    // Local faction policy: herbivores retaliate; player-aligned placements are friendly.
    // The official relationship matrix and conditional faction changes remain unrecovered.
    monster.attackable = faction != "EX5Faction_Player" && faction != "EX5Faction_TownNpc";
    monster.hostile = monster.attackable;
    monster.proactive = monster.attackable && faction != "EX5Faction_Herbivore";
    if (const auto locomotion = world->data->npc_locomotion.find(cms);
        locomotion != world->data->npc_locomotion.end()) {
      monster.ai.emplace(locomotion->second, position);
    }
    std::uint64_t runtime_id = next_world_unit_id();
    while (!world->allocated_low_ids.insert(static_cast<std::uint32_t>(runtime_id)).second) {
      runtime_id = next_world_unit_id();
    }
    if (!world->monsters.emplace(runtime_id, monster).second) {
      throw std::runtime_error("Duplicate world monster identity");
    }
    world->slots.push_back({std::move(monster), definition, runtime_id, std::nullopt});
    const auto route = definition.find(definition.contains("patrol") ? "patrol" : "roaming");
    if (route != definition.end()) {
      world->slots.back().patrol = std::make_shared<const NpcPatrolRoute>(
          NpcPatrolRoute::from_json(*route));
    }
    world->definitions.emplace(runtime_id, definition);
  }
  return world;
}

CombatSession::~CombatSession() {
  detach_world_mobs();
}

std::unique_lock<world_metrics::RecursiveTimedMutex> CombatSession::lock_world_mobs() const {
  if (!world_mobs_) {
    return {};
  }
  return std::unique_lock(world_mobs_->mutex);
}

std::shared_ptr<combat::CombatTimeline> CombatSession::world_combat_timeline() const {
  const auto lock = lock_world_mobs();
  return world_mobs_ ? world_mobs_->timeline : nullptr;
}

std::optional<combat::CombatTimeline::Owner> CombatSession::bind_world_milestone_owner(
    std::uint64_t owner_id, std::shared_ptr<CombatMilestoneOwner> enclosing_owner) {
  const auto lock = lock_world_mobs();
  if (!world_mobs_ || owner_id == 0 || world_milestone_owner_ || !enclosing_owner ||
      enclosing_owner->owned_combat_session() != this ||
      world_mobs_->milestone_owners.contains(owner_id)) {
    return std::nullopt;
  }
  const auto owner = world_mobs_->timeline->register_owner(owner_id);
  if (!owner) {
    return std::nullopt;
  }
  try {
    if (!world_mobs_->milestone_owners.emplace(owner_id, this).second) {
      throw std::logic_error("Duplicate combat milestone owner generation");
    }
  } catch (...) {
    world_mobs_->timeline->seal_owner(*owner);
    world_mobs_->timeline->retire_owner_if_idle(*owner);
    throw;
  }
  world_milestone_owner_ = *owner;
  world_milestone_enclosing_owner_ = std::move(enclosing_owner);
  world_milestone_timeline_retired_ = false;
  return owner;
}

int CombatSession::world_map_id() const {
  return world_mobs_ ? world_mobs_->map_id : 0;
}

std::map<std::uint64_t, CombatSession::Monster> &CombatSession::monsters() {
  return world_mobs_ ? world_mobs_->monsters : local_monsters_;
}

const std::map<std::uint64_t, CombatSession::Monster> &CombatSession::monsters() const {
  return world_mobs_ ? world_mobs_->monsters : local_monsters_;
}

bool CombatSession::has_world_mobs() const {
  return world_mobs_ != nullptr;
}

bool CombatSession::is_visible_world_monster(std::uint64_t unit_id) const {
  return world_mobs_ && visible_world_mobs_.contains(unit_id);
}

void CombatSession::attach_world_mobs(std::shared_ptr<WorldMobs> world, bool instance_copy,
                                      std::int64_t initial_tick) {
  if (world_milestone_detach_pending_) {
    throw std::logic_error("Cannot reuse a population while its detach is pending");
  }
  if (world_mobs_ == world) {
    return;
  }
  if (!pending_world_defeats_.empty()) {
    throw std::runtime_error("Cannot replace a population with unresolved monster defeats");
  }
  detach_world_mobs();
  if (tutorial_data_ || !world || world->data != data_) {
    throw std::invalid_argument("World mobs require matching non-tutorial combat data");
  }
  world_mobs_ = std::move(world);
  const auto lock = lock_world_mobs();
  world_mobs_are_instance_copy_ = instance_copy;
  world_journal_cursor_ = world_mobs_->sequence;
  world_refresh_tick_ = initial_tick;
  local_monsters_.clear();
}

void CombatSession::detach_world_mobs(std::optional<std::int64_t> detach_tick) {
  if (!world_mobs_) {
    return;
  }
  // Keep the enclosing session alive until detach has released the population and its lock.
  const auto enclosing_owner_lifetime = world_milestone_enclosing_owner_;
  {
    const auto lock = lock_world_mobs();
    if (world_milestone_owner_ && !world_milestone_timeline_retired_) {
      if (!world_mobs_->timeline->seal_owner(*world_milestone_owner_)) {
        throw std::logic_error("Combat milestone owner could not be sealed");
      }
      world_milestone_detach_pending_ = true;
      if (retained_world_milestone_ ||
          !world_mobs_->timeline->retire_owner_if_idle(*world_milestone_owner_)) {
        throw std::logic_error("Cannot detach a population with retained or queued combat work");
      }
      world_milestone_timeline_retired_ = true;
    }
    if (retained_world_milestone_) {
      throw std::logic_error("Cannot detach a population with retained or queued combat work");
    }
    if (world_milestone_owner_) {
      const auto owner = world_mobs_->milestone_owners.find(world_milestone_owner_->id);
      if (owner == world_mobs_->milestone_owners.end() || owner->second != this) {
        throw std::logic_error("Combat milestone owner binding is inconsistent");
      }
    }
    // The copy owns this population, including while another member is still loading.
    const bool preserve_encounter = world_mobs_are_instance_copy_;
    for (auto &[id, monster] : monsters()) {
      if (monster.simulation_owner != player_id_) {
        continue;
      }
      // Each monster is a separate transaction. Earlier successful detachments stay
      // committed, while this snapshot permits retry without replaying their notices.
      // The interruption epoch is shared with other sessions' flights and therefore
      // needs its own value snapshot in addition to the shallow Monster copy.
      static_assert(std::is_nothrow_destructible_v<WorldMobs::JournalEntry>);
      // A preallocated map node lets rollback replace the complete Monster without
      // moving its potentially throwing optional<Action>. Both maps use the same
      // standard allocator and the integral-key comparison cannot throw.
      std::map<std::uint64_t, Monster> previous_monster;
      previous_monster.emplace(id, monster);
      auto previous_projectiles = projectiles_;
      const auto previous_journal_size = world_mobs_->journal.size();
      const auto previous_sequence = world_mobs_->sequence;
      const auto interruption = monster.projectile_interruption;
      const auto interruption_tick = interruption ? interruption->tick : std::nullopt;

      try {
        world_detach_journal_start_ = previous_journal_size;
        world_detach_journal_transaction_ = true;
        if (preserve_encounter && monster.stats.value("69", 0) > 0) {
          using NativeTicks = std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>;
          const auto ticks = detach_tick.value_or(std::chrono::duration_cast<NativeTicks>(
              std::chrono::steady_clock::now().time_since_epoch()).count());
          std::vector<GameplayPacket> cancelled;
          interrupt_monster_action(id, monster, ticks, ticks, cancelled);
          if (world_detach_hook_for_test_) {
            world_detach_hook_for_test_("interrupted");
          }
          publish_world_packets(cancelled);
        } else {
          monster.action.reset();
        }
        monster.simulation_owner = 0;
        monster.aggro = preserve_encounter && !monster.ai;
        monster.damaged = false;
        monster.move_announced = false;
        if (world_detach_hook_for_test_) {
          world_detach_hook_for_test_("owner_cleared");
        }
        if (monster.stats.value("69", 0) <= 0) {
          world_detach_journal_transaction_ = false;
          continue;
        }
        if (preserve_encounter) {
          // A remaining member can acquire this monster on its next AI step. Keep the shared
          // health and position, but clear the departed player's action and target.
          if (monster.ai) {
            monster.ai->reset_at(monster.position);
            monster.damaged = true;
          }
          monster.awaiting_target_transfer = true;
          publish_world_packets({{98, {{"unit_id", std::to_string(id)}}}});
          world_detach_journal_transaction_ = false;
          while (world_mobs_->journal.size() > kMaxWorldJournal) {
            world_mobs_->journal.pop_front();
          }
          continue;
        }
        // A disconnected target cannot leave a mob permanently engaged. Return and heal using
        // its authored home; the next nearby client receives the current authoritative pose.
        if (monster.ai) {
          for (const auto &slot : world_mobs_->slots) {
            if (slot.current_unit == id) {
              monster.position = slot.prototype.position;
              monster.ai->reset_at(monster.position);
              if (monster.stats.value("69", 0) > 0) {
                monster.stats["69"] = monster.stats.at("18");
              }
              const auto id_text = std::to_string(id);
              const auto tick = wire_monotonic_tick(monster.last_move_tick + 1);
              const std::vector<GameplayPacket> reset{
                  {98, {{"unit_id", id_text}}},
                  {32, {{"unit_id", id_text}, {"state", "EX5UnitStateType_Idle"}, {"tick", tick}}},
                  {177,
                   {{"unit_id", id_text},
                    {"mode", "MoveMode_Normal"},
                    {"detail", "MoveDetail_Stop"},
                    {"pos", wire_position(monster.position)},
                    {"body_yaw", monster.position.yaw},
                    {"tick", tick}}},
                  {47,
                   {{"unit_id", id_text},
                    {"tick", tick},
                    {"stat_changes",
                     Json::array({{{"stat_type", 69}, {"result", monster.stats.at("69")}}})}}}};
              publish_world_packets(reset);
              break;
            }
          }
        }
        world_detach_journal_transaction_ = false;
        while (world_mobs_->journal.size() > kMaxWorldJournal) {
          world_mobs_->journal.pop_front();
        }
      } catch (...) {
        world_detach_journal_transaction_ = false;
        if (interruption) {
          interruption->tick = interruption_tick;
        }
        auto changed = monsters().extract(id);
        auto saved = previous_monster.extract(id);
        if (changed.empty() || saved.empty()) {
          std::terminate();
        }
        const auto restored = monsters().insert(std::move(saved));
        if (!restored.inserted) {
          std::terminate();
        }
        projectiles_.swap(previous_projectiles);
        while (world_mobs_->journal.size() > previous_journal_size) {
          world_mobs_->journal.pop_back();
        }
        world_mobs_->sequence = previous_sequence;
        throw;
      }
    }
    std::erase_if(monsters(),
                  [this](const auto &entry) { return entry.second.private_owner == player_id_; });
    if (world_milestone_owner_) {
      world_mobs_->milestone_owners.erase(world_milestone_owner_->id);
      world_milestone_owner_.reset();
      world_milestone_enclosing_owner_.reset();
      world_milestone_detach_pending_ = false;
      world_milestone_timeline_retired_ = false;
    }
  }
  world_mobs_.reset();
  world_mobs_are_instance_copy_ = false;
  executed_world_milestone_.reset();
  visible_world_mobs_.clear();
  engaged_world_mobs_.clear();
  world_defeats_.clear();
  action_.reset();
  projectiles_.clear();
  projectile_retry_not_before_.reset();
  areas_.clear();
  static_cast<void>(enclosing_owner_lifetime);
}

void CombatSession::set_world_detach_hook_for_test(
    std::function<void(std::string_view)> hook) {
  const auto lock = lock_world_mobs();
  world_detach_hook_for_test_ = std::move(hook);
}

nlohmann::json CombatSession::world_detach_state_for_test() const {
  const auto lock = lock_world_mobs();
  Json result{{"monsters", monster_state_fingerprint_for_test()},
              {"projectiles", Json::array()},
              {"epochs", Json::array()},
              {"journal", Json::array()}};
  if (!world_mobs_) {
    return result;
  }
  result["sequence"] = world_mobs_->sequence;
  for (const auto &[id, monster] : monsters()) {
    Json tick = nullptr;
    if (monster.projectile_interruption && monster.projectile_interruption->tick) {
      tick = *monster.projectile_interruption->tick;
    }
    result["epochs"].push_back({{"unit", id}, {"tick", std::move(tick)}});
  }
  for (const auto &flight : projectiles_) {
    Json interruption = nullptr;
    if (flight.npc_interruption && flight.npc_interruption->tick) {
      interruption = *flight.npc_interruption->tick;
    }
    result["projectiles"].push_back({{"instance", flight.instance},
                                     {"caster", flight.caster},
                                     {"deadline", flight.deadline},
                                     {"interruption", std::move(interruption)}});
  }
  for (const auto &entry : world_mobs_->journal) {
    result["journal"].push_back({{"sequence", entry.sequence}, {"source", entry.source},
                                 {"opcode", entry.packet.opcode},
                                 {"message", entry.packet.message}});
  }
  return result;
}

void CombatSession::seed_world_detach_journal_for_test(std::size_t count) {
  const auto lock = lock_world_mobs();
  if (!world_mobs_ || count > kMaxWorldJournal) {
    throw std::invalid_argument("Invalid detach journal test size");
  }
  const std::vector<GameplayPacket> packet{{98, {{"unit_id", "0"}}}};
  for (std::size_t index = 0; index < count; ++index) {
    publish_world_packets(packet);
  }
}

void CombatSession::world_monster_defeated(std::uint64_t id, std::int64_t ticks) {
  if (!world_mobs_) {
    return;
  }
  const auto &monster = monsters().at(id);
  if (monster.private_owner != 0) {
    return;
  }

  WorldMobs::Slot *respawn_slot = nullptr;
  std::optional<std::int64_t> respawn_tick;
  for (auto &slot : world_mobs_->slots) {
    if (slot.current_unit != id || slot.definition.at("one_time_use").get<bool>()) {
      continue;
    }
    const auto delay =
        std::max<std::int64_t>(kWorldPollMs, slot.definition.at("respawn_ms").get<std::int64_t>());
    respawn_slot = &slot;
    respawn_tick = ticks + delay * kTicksPerMillisecond;
    break;
  }

  world_defeats_.push_back(
      {id, monster.cms_id, static_cast<std::int64_t>(player_id_), monster.position, ticks,
       monster.reward_credit_id, monster.reward_event_id});
  try {
    world_mobs_->corpses.insert_or_assign(id, ticks + kWorldCorpseMs * kTicksPerMillisecond);
  } catch (...) {
    world_defeats_.pop_back();
    throw;
  }
  if (respawn_slot) {
    respawn_slot->respawn_tick = respawn_tick;
  }
}

std::vector<MonsterDefeat> CombatSession::take_world_defeats() {
  for (const auto &pending : pending_world_defeats_) {
    if ((pending.death_state_applied && !pending.finalized) || pending.capture.boss_script) {
      return {};
    }
  }
  auto defeats = std::move(world_defeats_);
  world_defeats_.clear();
  return defeats;
}

void CombatSession::set_world_defeat_capture(
    std::function<MonsterRewardCapture(const MonsterDefeat &)> capture) {
  world_defeat_capture_ = std::move(capture);
}

const std::deque<PendingWorldDefeat> &CombatSession::pending_world_defeats() const noexcept {
  return pending_world_defeats_;
}

std::vector<GameplayPacket> CombatSession::finalize_world_defeat(const std::string &event_id,
                                                                 std::int64_t ticks) {
  const auto world_lock = lock_world_mobs();
  const auto pending = std::ranges::find_if(pending_world_defeats_, [&](const auto &entry) {
    return entry.defeat.reward_event_id == event_id;
  });
  if (pending == pending_world_defeats_.end() || !world_mobs_ ||
      !monsters().contains(pending->defeat.unit_id) || pending->finalized) {
    return {};
  }

  if (!pending->loot_reaction) {
    auto defeat = pending->defeat;
    defeat.ticks = ticks;
    MonsterDefeatReaction reaction;
    if (defeat_observer_) {
      reaction = defeat_observer_(defeat);
    }
    pending->loot_reaction = std::move(reaction);
  }

  if (!pending->final_packets) {
    std::vector<GameplayPacket> packets{pending->damage};
    packets.insert(packets.end(), pending->loot_reaction->packets.begin(),
                   pending->loot_reaction->packets.end());
    const auto target = std::to_string(pending->defeat.unit_id);
    packets.push_back({32, {{"unit_id", target}, {"state", "EX5UnitStateType_Demise"},
                            {"tick", wire_monotonic_tick(ticks)}}});
    packets.push_back({23, {{"base_caster_unit_id", std::to_string(pending->caster)},
                            {"target_unit_id", target},
                            {"pos", wire_position(pending->defeat.position)}}});
    pending->final_packets = std::move(packets);
  }

  if (!pending->death_state_applied) {
    auto &monster = monsters().at(pending->defeat.unit_id);
    auto final_stats = monster.stats;
    final_stats["71"] = 0;
    defeated_.push_back(pending->defeat.unit_id);
    try {
      world_monster_defeated(pending->defeat.unit_id, ticks);
    } catch (...) {
      defeated_.pop_back();
      throw;
    }
    monster.stats.swap(final_stats);
    monster.statuses.clear();
    monster.gauges.clear();
    monster.shield.reset();
    monster.action.reset();
    pending->death_state_applied = true;
  }

  // Copy all allocations before publishing. If journal insertion fails halfway, restore the
  // prior journal and sequence so the next retry never duplicates public combat packets.
  auto packets = *pending->final_packets;
  auto previous_journal = world_mobs_->journal;
  const auto previous_sequence = world_mobs_->sequence;
  try {
    publish_world_packets(packets);
    if (pending->loot_reaction->on_published) {
      pending->loot_reaction->on_published(ticks);
    }
    const auto corpse = world_mobs_->corpses.find(pending->defeat.unit_id);
    if (corpse != world_mobs_->corpses.end()) {
      corpse->second = std::max(corpse->second, ticks + kWorldCorpseMs * kTicksPerMillisecond);
    }
  } catch (...) {
    world_mobs_->journal.swap(previous_journal);
    world_mobs_->sequence = previous_sequence;
    throw;
  }
  monsters().at(pending->defeat.unit_id).pending_defeat_visible_hp.reset();
  pending->finalized = true;
  return packets;
}

bool CombatSession::stage_finalized_world_defeat(const std::string &event_id,
                                                  std::vector<GameplayPacket> &destination) {
  static_assert(std::is_nothrow_move_constructible_v<GameplayPacket>);
  const auto world_lock = lock_world_mobs();
  const auto pending = std::ranges::find_if(pending_world_defeats_, [&](const auto &entry) {
    return entry.defeat.reward_event_id == event_id;
  });
  if (pending == pending_world_defeats_.end() || !pending->finalized || !pending->final_packets) {
    return false;
  }
  if (pending->owner_packets_staged) {
    return true;
  }

  if (!pending->delivery_packets) {
    pending->delivery_packets =
        std::make_shared<const std::vector<GameplayPacket>>(*pending->final_packets);
  }
  auto batch = *pending->delivery_packets;
  if (batch.size() > destination.max_size() - destination.size()) {
    throw std::length_error("Finalized monster packet batch exceeds its destination limit");
  }
  if (world_defeat_stage_hook_for_test_) {
    world_defeat_stage_hook_for_test_();
  }
  destination.reserve(destination.size() + batch.size());
  for (auto &packet : batch) {
    destination.push_back(std::move(packet));
  }
  pending->owner_packets_staged = true;
  return true;
}

std::shared_ptr<const std::vector<GameplayPacket>>
CombatSession::finalized_world_defeat_batch(const std::string &event_id) {
  const auto world_lock = lock_world_mobs();
  const auto pending = std::ranges::find_if(pending_world_defeats_, [&](const auto &entry) {
    return entry.defeat.reward_event_id == event_id;
  });
  if (pending == pending_world_defeats_.end() || !pending->finalized || !pending->final_packets) {
    return {};
  }
  if (!pending->delivery_packets) {
    pending->delivery_packets =
        std::make_shared<const std::vector<GameplayPacket>>(*pending->final_packets);
  }
  return pending->delivery_packets;
}

bool CombatSession::acknowledge_delivered_world_defeat(
    const std::string &event_id,
    const std::shared_ptr<const std::vector<GameplayPacket>> &batch) {
  const auto world_lock = lock_world_mobs();
  const auto pending = std::ranges::find_if(pending_world_defeats_, [&](const auto &entry) {
    return entry.defeat.reward_event_id == event_id;
  });
  if (pending == pending_world_defeats_.end() || !pending->finalized ||
      !pending->owner_packets_staged || !batch || pending->delivery_packets != batch) {
    return false;
  }
  return acknowledge_world_defeat(event_id);
}

void CombatSession::set_world_defeat_stage_hook_for_test(std::function<void()> hook) {
  const auto world_lock = lock_world_mobs();
  world_defeat_stage_hook_for_test_ = std::move(hook);
}

bool CombatSession::acknowledge_world_defeat(const std::string &event_id) {
  const auto world_lock = lock_world_mobs();
  const auto pending = std::ranges::find_if(pending_world_defeats_, [&](const auto &entry) {
    return entry.defeat.reward_event_id == event_id;
  });
  if (pending == pending_world_defeats_.end() || !pending->finalized) {
    return false;
  }
  if (pending->capture.boss_script) {
    std::erase_if(world_defeats_, [&](const auto &defeat) {
      return defeat.reward_event_id == event_id;
    });
  }
  pending_world_defeats_.erase(pending);
  if (pending_world_defeats_.size() < kMaximumPendingWorldDefeats) {
    pending_world_defeat_capacity_reported_ = false;
  }
  return true;
}

void CombatSession::set_world_journal_hook_for_test(std::function<void()> hook) {
  const auto lock = lock_world_mobs();
  world_journal_hook_for_test_ = std::move(hook);
}

void CombatSession::publish_world_journal_for_test(const std::vector<GameplayPacket> &packets) {
  const auto lock = lock_world_mobs();
  publish_world_packets(packets);
}

void CombatSession::publish_world_packets(const std::vector<GameplayPacket> &packets) {
  if (!world_mobs_) {
    return;
  }
  const auto previous_size = world_mobs_->journal.size();
  const auto previous_sequence = world_mobs_->sequence;
  try {
    for (const auto &packet : packets) {
      if (!public_combat_packet(packet.opcode)) {
        continue;
      }
      bool private_packet = false;
      for (const auto *field :
           {"unit_id", "caster_unit_id", "base_caster_unit_id", "target_unit_id"}) {
        const auto value = packet.message.find(field);
        if (value == packet.message.end() || !value->is_string()) {
          continue;
        }
        const auto id = parse_unit_id_text(value->get_ref<const std::string &>());
        const auto monster = id ? monsters().find(*id) : monsters().end();
        if (monster != monsters().end() && monster->second.private_owner != 0) {
          private_packet = true;
          break;
        }
      }
      if (private_packet) {
        continue;
      }
      if (world_detach_journal_transaction_ &&
          world_mobs_->journal.size() - world_detach_journal_start_ >= kMaxWorldJournal) {
        throw std::runtime_error("Monster detach exceeds its journal transaction bound");
      }
      if (world_mobs_->sequence == (std::numeric_limits<std::uint64_t>::max)()) {
        throw std::overflow_error("World combat journal sequence exhausted");
      }
      const auto next_sequence = world_mobs_->sequence + 1;
      world_mobs_->journal.push_back({next_sequence, player_id_, packet});
      world_mobs_->sequence = next_sequence;
      if (world_journal_hook_for_test_) {
        world_journal_hook_for_test_();
      }
      if (world_detach_journal_transaction_ && world_detach_hook_for_test_) {
        world_detach_hook_for_test_("journal_inserted");
      }
    }
  } catch (...) {
    while (world_mobs_->journal.size() > previous_size) {
      world_mobs_->journal.pop_back();
    }
    world_mobs_->sequence = previous_sequence;
    throw;
  }
  while (!world_detach_journal_transaction_ && world_mobs_->journal.size() > kMaxWorldJournal) {
    world_mobs_->journal.pop_front();
  }
}

std::vector<GameplayPacket> CombatSession::receive(std::uint16_t opcode, const Json &request,
                                                   const WorldPosition &position,
                                                   std::int64_t ticks) {
  const auto lock = lock_world_mobs();
  auto packets = receive_impl(opcode, request, position, ticks);
  publish_world_packets(packets);
  return packets;
}

std::vector<GameplayPacket> CombatSession::refresh_world_mobs(const WorldPosition &position,
                                                              std::int64_t ticks) {
  std::vector<GameplayPacket> packets;
  if (position.map_id != world_map_id()) {
    return packets;
  }
  world_refresh_tick_ = ticks + kWorldPollMs * kTicksPerMillisecond;
  for (auto &slot : world_mobs_->slots) {
    if (slot.patrol && monsters().contains(slot.current_unit)) {
      auto &monster = monsters().at(slot.current_unit);
      const bool idle = !monster.gm_ai_paused && !monster_stunned(monster, ticks) &&
                        monster.stats.value("69", 0) > 0 && !monster.aggro &&
                        (!monster.ai || monster.ai->state() == NpcAiState::Idle);
      if (idle && slot.patrol_paused) {
        // Combat returns the monster to its home. Resume from the route's beginning,
        // rather than teleporting back to a stale point partway through an idle walk.
        slot.patrol_elapsed = 0;
      } else if (idle && slot.patrol_updated_tick && ticks >= *slot.patrol_updated_tick) {
        slot.patrol_elapsed += ticks - *slot.patrol_updated_tick;
      }
      slot.patrol_paused = !idle;
      slot.patrol_updated_tick = ticks;
      if (idle) {
        const auto pose = slot.patrol->sample(slot.patrol_elapsed);
        monster.position = {world_map_id(), pose.position.x_mm, pose.position.y_mm,
                            pose.position.z_mm, pose.yaw};
        if (monster.ai) {
          monster.ai->place(monster.position);
        }
        if (ticks - monster.last_move_tick >= 500 * kTicksPerMillisecond) {
          const auto prediction =
              slot.patrol->sample(slot.patrol_elapsed + 500 * kTicksPerMillisecond);
          Json movement{{"unit_id", std::to_string(slot.current_unit)},
                        {"pos", wire_position(monster.position)},
                        {"body_yaw", pose.yaw},
                        {"tick", wire_monotonic_tick(ticks)},
                        {"mode", "MoveMode_Normal"},
                        {"detail", pose.moving ? pose.move_detail : "MoveDetail_Stop"}};
          if (pose.moving) {
            movement["pred_milli"] = 500;
            movement["pred_pos"] = {{"millimeter_x", prediction.position.x_mm},
                                    {"millimeter_y", prediction.position.y_mm},
                                    {"millimeter_z", prediction.position.z_mm}};
          }
          const std::vector<GameplayPacket> moved{{177, std::move(movement)}};
          publish_world_packets(moved);
          if (visible_world_mobs_.contains(slot.current_unit)) {
            packets.insert(packets.end(), moved.begin(), moved.end());
          }
          monster.last_move_tick = ticks;
          monster.move_announced = pose.moving;
        }
      }
    }
    if (!slot.respawn_tick || ticks < *slot.respawn_tick) {
      continue;
    }
    const auto previous_life = monsters().find(slot.current_unit);
    if (previous_life != monsters().end() && previous_life->second.pending_defeat_visible_hp) {
      continue;
    }
    if (monsters().size() >= kMaxWorldLives) {
      // Retain existing corpse rights. Population recovery waits for their expiry rather
      // than growing the map state without a bound under sustained kills.
      continue;
    }
    // Every life has a fresh typed ID, so a delayed projectile or loot request cannot name
    // the next occupant of the same spawn slot. The life identity also isolates corpse rights.
    slot.current_unit = next_world_unit_id();
    if (world_mobs_->allocated_low_ids.size() >= (1ULL << 23)) {
      throw std::runtime_error("World monster life identity budget exhausted");
    }
    while (!world_mobs_->allocated_low_ids.insert(static_cast<std::uint32_t>(slot.current_unit))
                .second) {
      slot.current_unit = next_world_unit_id();
    }
    monsters().emplace(slot.current_unit, slot.prototype);
    auto &spawned = monsters().at(slot.current_unit);
    // A spawn prototype is copied across lives; its flight epochs and identity must not be.
    spawned.life_identity = std::make_shared<MonsterLifeIdentity>();
    spawned.projectile_interruption = std::make_shared<ProjectileInterruption>();
    spawned.reward_credit_id = next_world_credit_id();
    spawned.reward_event_id = next_world_reward_event_id();
    world_mobs_->definitions.emplace(slot.current_unit, slot.definition);
    slot.respawn_tick.reset();
    slot.patrol_elapsed = 0;
    slot.patrol_updated_tick = ticks;
    slot.patrol_paused = false;
  }
  for (auto corpse = world_mobs_->corpses.begin(); corpse != world_mobs_->corpses.end();) {
    const auto pending_life = monsters().find(corpse->first);
    if (pending_life != monsters().end() && pending_life->second.pending_defeat_visible_hp) {
      ++corpse;
      continue;
    }
    if (ticks < corpse->second) {
      ++corpse;
      continue;
    }
    const auto expired = monsters().find(corpse->first);
    if (expired != monsters().end()) {
      // Preserve the authored removal time for flights whose owner polls after corpse cleanup.
      expired->second.projectile_interruption->tick = corpse->second;
    }
    monsters().erase(corpse->first);
    world_mobs_->definitions.erase(corpse->first);
    corpse = world_mobs_->corpses.erase(corpse);
  }

  if (!world_mobs_->journal.empty() &&
      world_journal_cursor_ + 1 < world_mobs_->journal.front().sequence) {
    Json forgotten = Json::array();
    for (const auto id : visible_world_mobs_) {
      forgotten.push_back(std::to_string(id));
    }
    if (!forgotten.empty()) {
      packets.push_back({11, {{"unit_ids", std::move(forgotten)}}});
    }
    visible_world_mobs_.clear();
    world_journal_cursor_ = world_mobs_->sequence;
  }

  std::set<std::uint64_t> visible;
  for (const auto &[id, monster] : monsters()) {
    if (monster.private_owner != 0) {
      continue;
    }
    const double x = double(monster.position.x_mm) - position.x_mm;
    const double y = double(monster.position.y_mm) - position.y_mm;
    const double z = double(monster.position.z_mm) - position.z_mm;
    if (x * x + y * y + z * z > kSightRangeMm * kSightRangeMm) {
      continue;
    }
    if (visible.size() >= 1024) {
      break;
    }
    visible.insert(id);
    if (visible_world_mobs_.contains(id)) {
      continue;
    }
    const auto definition = world_mobs_->definitions.find(id);
    const char *state = "EX5UnitStateType_Idle";
    if (monster.stats.value("69", 0) <= 0 && !monster.pending_defeat_visible_hp) {
      state = "EX5UnitStateType_Demise";
    } else if (monster.aggro) {
      state = "EX5UnitStateType_Combat";
    }
    auto visible_stats = monster.stats;
    if (monster.pending_defeat_visible_hp) {
      visible_stats["69"] = *monster.pending_defeat_visible_hp;
    }
    Json npc{{"npc_cms_id", monster.cms_id},
             {"base_unit_id", std::to_string(id)},
             {"stats", std::move(visible_stats)},
             {"state", state}};
    if (definition != world_mobs_->definitions.end()) {
      npc["faction_type"] = definition->second.at("faction");
      npc["spawner_id_x"] = definition->second.at("spawner_guid").at(0);
      npc["spawner_id_y"] = definition->second.at("spawner_guid").at(1);
      npc["anim_stance_key"] = definition->second.at("anim_stance_key");
      // Captured official nubs of butcherable monsters (Alpha Wolf) carry this flag; the client
      // offers the knife interaction on such a corpse.
      if (definition->second.value("handling_loot_candidate", false)) {
        npc["is_handling_loot_candidate"] = true;
      }
    }
    packets.push_back({10,
                       {{"unit_id", std::to_string(id)},
                        {"by_sight", true},
                        {"nub",
                         {{"pos", wire_position(monster.position)},
                          {"yaw", monster.position.yaw},
                          {"npc", std::move(npc)}}}}});
  }
  Json removed = Json::array();
  for (const auto id : visible_world_mobs_) {
    if (!visible.contains(id)) {
      removed.push_back(std::to_string(id));
    }
  }
  if (!removed.empty()) {
    packets.push_back({11, {{"unit_ids", std::move(removed)}}});
  }
  visible_world_mobs_ = std::move(visible);
  for (const auto &event : world_mobs_->journal) {
    if (event.sequence > world_journal_cursor_ && event.source != player_id_) {
      // Map scope is enforced by the shared state. A packet for an unannounced NPC is withheld.
      const auto root_id = event.packet.message.find("unit_id");
      const auto id = root_id != event.packet.message.end() && root_id->is_string()
                          ? parse_unit_id_text(root_id->get_ref<const std::string &>())
                          : std::nullopt;
      if (!id || !monsters().contains(*id) || visible_world_mobs_.contains(*id)) {
        packets.push_back(event.packet);
      }
    }
  }
  world_journal_cursor_ = world_mobs_->sequence;
  return packets;
}

} // namespace chronicles
