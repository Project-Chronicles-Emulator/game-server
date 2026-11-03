#include "world/features/npc/npc_feature.hpp"

#include "world/core/world_request_opcodes.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <set>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace chronicles::npc {
namespace {

// Original native dispatch constants; the guarded source audit checks numeric identity.
constexpr std::uint16_t kPingRequest = 20003;
constexpr std::uint16_t kTimeSyncRequest = 20001;
constexpr std::uint16_t kMoveRequest = 20087;
using world_request::kMapLoadCompleteRequest;
// tools/build_stationary_npc_placements.py writes one map-<id>.json per map.
constexpr std::string_view kPlacementFilePrefix = "map-";
// Startup bound on placement files; the client catalog has fewer than 512 maps.
constexpr std::size_t kMaximumPopulationMaps = 512;

} // namespace

NpcFeature::NpcFeature(std::shared_ptr<const NpcPopulation> population, std::int64_t epoch_tick,
                       std::optional<NpcReactionCatalog> reactions)
    : population_(std::move(population)), epoch_tick_(epoch_tick),
      reactions_(std::move(reactions)) {
  if (population_) {
    populations_.emplace(population_->map_id(), population_);
  }
}

void NpcFeature::load_map_populations(const std::filesystem::path &catalog_root) {
  const auto directory = catalog_root.parent_path() / "npc" / "placements";
  if (!std::filesystem::is_directory(directory)) {
    return;
  }

  std::vector<std::filesystem::path> files;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    const auto name = entry.path().filename().string();
    if (entry.is_regular_file() && name.starts_with(kPlacementFilePrefix) &&
        entry.path().extension() == ".json") {
      files.push_back(entry.path());
    }
  }
  if (files.size() > kMaximumPopulationMaps) {
    throw std::runtime_error("Too many NPC placement maps");
  }
  std::ranges::sort(files);

  std::size_t loaded = 0;
  std::size_t units = 0;
  for (const auto &file : files) {
    auto population = load_npc_population_file(file, kMaxPopulatedNpcUnits);
    const int map_id = population->map_id();
    if (populations_.contains(map_id)) {
      continue;
    }
    units += population->units().size();
    populations_.emplace(map_id, std::move(population));
    ++loaded;
  }
  spdlog::info("NPC map populations loaded: maps={} additional_maps={} additional_units={}",
               populations_.size(), loaded, units);
}

std::shared_ptr<const NpcPopulation> NpcFeature::population_for(int map_id) const {
  const auto found = populations_.find(map_id);
  return found == populations_.end() ? nullptr : found->second;
}

std::size_t NpcFeature::population_map_count() const noexcept {
  return populations_.size();
}

void NpcFeature::load_static_npcs(const std::filesystem::path &catalog_root) {
  static_npcs_ = MapStaticNpcs::load(catalog_root);
  reactions_ = NpcReactionCatalog::load(catalog_root);
  if (reactions_) {
    spdlog::info("NPC threat reaction profile loaded: {} civilians, range {} mm",
                 reactions_->size(), reactions_->range_mm());
  } else {
    spdlog::warn("NPC threat reaction profile is absent; threatened civilians do not react. "
                 "Export and deploy npc-reactions/definitions.json beside the catalog.");
  }
  if (static_npcs_) {
    spdlog::info("Static NPC interaction index loaded: {} targets", static_npcs_->size());
  } else {
    spdlog::warn("Static NPC interaction index is absent; client-owned NPC interactions are "
                 "unavailable. Export and deploy static-npcs/definitions.json beside the catalog.");
  }
}

const std::shared_ptr<const NpcPopulation> &NpcFeature::population() const noexcept {
  return population_;
}

const std::optional<MapStaticNpcs> &NpcFeature::static_npcs() const noexcept {
  return static_npcs_;
}

std::int64_t NpcFeature::epoch_tick() const noexcept {
  return epoch_tick_;
}

const std::optional<NpcReactionCatalog> &NpcFeature::reactions() const noexcept {
  return reactions_;
}

NpcReactionTimeline &NpcFeature::reaction_timeline() const noexcept {
  return reaction_timeline_;
}

ReactionTimeline NpcPlayerSession::timeline() const {
  const auto scope = context_.npc_reaction_scope();
  return [this, scope](std::uint64_t unit_id, std::int64_t elapsed) {
    return feature_.reaction_timeline().sample(scope, unit_id, elapsed);
  };
}

NpcPlayerSession::NpcPlayerSession(const NpcFeature &feature, NpcSessionContext &context)
    : feature_(feature), context_(context) {
}

const AnnouncedUnit *NpcPlayerSession::interaction_npc(std::uint64_t unit_id) const {
  const auto *announced = context_.npc_registry().find_in_instance(
      unit_id, context_.npc_session_map_id(), context_.npc_instance_key());
  if (announced) {
    return announced;
  }
  return feature_.static_npcs()
             ? feature_.static_npcs()->find(context_.npc_session_map_id(), unit_id)
             : nullptr;
}

std::optional<int> NpcPlayerSession::session_owned_population_map() const {
  return entered_population_map_;
}

void NpcPlayerSession::set_population_map(std::optional<int> map_id) {
  if (entered_population_map_ == map_id) {
    return;
  }
  context_.npc_registry().clear();
  npc_patrol_.reset();
  npc_sight_.clear();
  entered_population_map_ = map_id;
  population_announced_ = false;
}

void NpcPlayerSession::announce_population() {
  const auto owned_map = session_owned_population_map();
  const auto population = current_population();
  if (population_announced_ || !owned_map || !population) {
    return;
  }
  const auto result = population->announce_to(context_.npc_registry(), context_.npc_instance_key(),
                                              population_elapsed_ticks());
  if (result.rejected != 0 || result.collisions != 0) {
    throw std::runtime_error("NPC population announcement was refused by the registry");
  }
  // One announcement per map entry: a repeated map-load-complete must not collide with the
  // ids this session already holds.
  population_announced_ = true;
  npc_sight_.clear();
  if (population->has_patrols()) {
    npc_patrol_ =
        std::make_unique<NpcPatrolSession>(*population, feature_.epoch_tick(), timeline());
  }
}

std::shared_ptr<const NpcPopulation> NpcPlayerSession::current_population() const {
  return entered_population_map_ ? feature_.population_for(*entered_population_map_) : nullptr;
}

std::int64_t NpcPlayerSession::population_elapsed_ticks() const {
  return std::max<std::int64_t>(0, context_.npc_monotonic_ticks() - feature_.epoch_tick());
}

std::vector<GameplayPacket> NpcPlayerSession::refresh_npc_sight(const WorldPosition &position) {
  const auto population = current_population();
  if (!population_announced_ || !population || position.map_id != population->map_id()) {
    return {};
  }
  return npc_sight_.update(*population, {position.x_mm, position.y_mm, position.z_mm},
                           population_elapsed_ticks(), timeline(), feature_.epoch_tick());
}

void NpcPlayerSession::track_announced_unit(const GameplayPacket &packet) {
  if (packet.opcode == kNpcSightUnitRemovedOpcode) {
    // Leaving sight removes the client actor, not the server-owned NPC. The registry
    // retains its current patrol pose for dialogue/shop range checks on return.
    if (packet.message.value("by_sight", false)) {
      return;
    }
    const auto ids = packet.message.find("unit_ids");
    if (ids == packet.message.end() || !ids->is_array()) {
      return;
    }
    for (const auto &id : *ids) {
      const auto parsed = id.is_string() ? parse_unit_id_text(id.get<std::string>())
                                         : std::optional<std::uint64_t>();
      if (parsed) {
        context_.npc_registry().despawn(*parsed);
      }
    }
    return;
  }
  if (packet.opcode != kUnitAddedOpcode || !context_.npc_has_tutorial() ||
      !context_.npc_authenticated() || !packet.message.contains("nub") ||
      !packet.message.at("nub").is_object()) {
    return;
  }
  const auto &nub = packet.message.at("nub");
  if (!nub.contains("npc") || !nub.at("npc").is_object() || !nub.contains("pos") ||
      !nub.at("pos").is_object() || !packet.message.contains("unit_id") ||
      !packet.message.at("unit_id").is_string()) {
    return;
  }
  const auto &npc = nub.at("npc");
  const auto &pos = nub.at("pos");
  const auto unit_id = parse_unit_id_text(packet.message.at("unit_id").get<std::string>());
  if (!unit_id || !npc.contains("npc_cms_id") || !npc.at("npc_cms_id").is_number_integer() ||
      !pos.contains("millimeter_x") || !pos.at("millimeter_x").is_number_integer() ||
      !pos.contains("millimeter_y") || !pos.at("millimeter_y").is_number_integer() ||
      !pos.contains("millimeter_z") || !pos.at("millimeter_z").is_number_integer()) {
    spdlog::warn("Native world NPC announcement lacks a unit id, template or position");
    return;
  }
  const auto result = context_.npc_registry().announce(
      {*unit_id,
       npc.at("npc_cms_id").get<int>(),
       context_.npc_session_map_id(),
       context_.npc_instance_key(),
       {pos.at("millimeter_x").get<int>(), pos.at("millimeter_y").get<int>(),
        pos.at("millimeter_z").get<int>()}});
  if (result != UnitAnnounceResult::Added) {
    spdlog::warn("Native world NPC announcement was not recorded (result {})",
                 static_cast<int>(result));
  }
}

void NpcPlayerSession::observe_patrol_position(std::uint16_t request_opcode) {
  // Interaction and shop range checks need the current server-owned NPC pose. Ping,
  // clock and movement have no NPC target; sight calculates patrol poses from the shared
  // route itself, and the next targeted request will synchronize before range checks.
  const bool needs_patrol_position = request_opcode != kPingRequest &&
                                     request_opcode != kTimeSyncRequest &&
                                     request_opcode != kMoveRequest;
  if (needs_patrol_position && npc_patrol_ && session_owned_population_map()) {
    npc_patrol_->synchronize(context_.npc_monotonic_ticks(), context_.npc_registry(),
                             context_.npc_instance_key());
  }
}

bool NpcPlayerSession::observe_shared_population(const FrameContext &frame) {
  const auto *gameplay_request = frame.request();
  if (gameplay_request && !context_.npc_has_tutorial() &&
      gameplay_request->opcode == kMapLoadCompleteRequest) {
    announce_population();
    return true;
  }
  return false;
}

void NpcPlayerSession::observe_npc_sight(FrameContext &frame) {
  auto &party_position = frame.party_position;
  auto &session_packets = frame.session_packets;

  if (!context_.npc_has_tutorial() && party_position) {
    auto sight_packets = refresh_npc_sight(*party_position);
    session_packets.insert(session_packets.end(), std::make_move_iterator(sight_packets.begin()),
                           std::make_move_iterator(sight_packets.end()));
  }
}

void NpcPlayerSession::observe_start_playing_patrol(bool entering_world,
                                                    std::vector<GameplayPacket> &session_packets) {
  if (entering_world && npc_patrol_) {
    auto patrol_packets = npc_sight_.visible_only(
        npc_patrol_->start(context_.npc_monotonic_ticks(), context_.npc_registry(),
                           context_.npc_instance_key(), &npc_sight_));
    session_packets.insert(session_packets.end(), std::make_move_iterator(patrol_packets.begin()),
                           std::make_move_iterator(patrol_packets.end()));
  }
}

bool NpcPlayerSession::follows_patrol_route(std::uint64_t unit_id) const {
  const auto population = current_population();
  if (!population) {
    return false;
  }

  const auto &units = population->units();
  const auto found = std::ranges::find(units, unit_id, &PopulatedNpc::unit_id);
  return found != units.end() && found->placement.patrol != nullptr;
}

bool NpcPlayerSession::handle_threat(FrameContext &frame) {
  const auto *request = frame.request();
  if (!request || request->opcode != kThreatenedNpcRequest) {
    return false;
  }
  // This report has no dedicated acknowledgement. Valid nearby targets receive their
  // authored reaction; invalid or repeated entries cannot restart another actor's clock.
  if (!context_.npc_authenticated() || !context_.npc_player_alive() || !frame.gameplay_position ||
      !feature_.reactions()) {
    return true;
  }
  const auto ids = request->message.find("target_npc_unit_ids");
  if (ids == request->message.end() || !ids->is_array() || ids->size() > 64) {
    return true;
  }
  const auto &position = *frame.gameplay_position;
  const UnitPositionMm player{position.x_mm, position.y_mm, position.z_mm};
  const auto elapsed = population_elapsed_ticks();
  std::set<std::uint32_t> unique;
  for (const auto &value : *ids) {
    if (!value.is_number_integer() || value < 0 ||
        value > std::numeric_limits<std::uint32_t>::max()) {
      continue;
    }
    const auto id = value.get<std::uint32_t>();
    if (!unique.insert(id).second) {
      continue;
    }
    const auto *target = context_.npc_registry().find_low32_in_instance(
        id, context_.npc_session_map_id(), context_.npc_instance_key());
    if (!target || position.map_id != target->map_id ||
        distance_mm(player, target->position) > feature_.reactions()->range_mm()) {
      continue;
    }
    const auto *reaction = feature_.reactions()->find(target->npc_template_id);
    const auto yaw =
        static_cast<float>(std::atan2(static_cast<double>(player.y_mm) - target->position.y_mm,
                                      static_cast<double>(player.x_mm) - target->position.x_mm) *
                           180.0 / 3.141592653589793);
    const bool route_bound = follows_patrol_route(target->unit_id);
    if (!reaction ||
        !feature_.reaction_timeline().begin(context_.npc_reaction_scope(), target->unit_id, elapsed,
                                            *reaction, yaw, route_bound)) {
      continue;
    }
    const auto sample = timeline()(target->unit_id, elapsed);
    auto packet =
        threat_reaction_packet(target->unit_id, target->position, sample, feature_.epoch_tick());
    context_.npc_deliver_observers({packet});
    if (route_bound && npc_patrol_ && npc_patrol_->next_action_tick()) {
      // The walking unit's own patrol wakes now and sends its stop together with this same
      // reaction (identical start tick), instead of a reaction followed later by a second one.
      npc_patrol_->wake_at(context_.npc_monotonic_ticks());
      continue;
    }
    frame.session_packets.push_back(std::move(packet));
  }
  return true;
}

bool NpcPlayerSession::has_patrol() const noexcept {
  return static_cast<bool>(npc_patrol_);
}

std::optional<std::int64_t> NpcPlayerSession::next_action_tick() const {
  return npc_patrol_ ? npc_patrol_->next_action_tick() : std::nullopt;
}

std::vector<GameplayPacket> NpcPlayerSession::advance_patrol(std::int64_t ticks,
                                                             const WorldPosition &position) {
  // Refresh visibility before advance filters patrol packets, and emit the new sight
  // announcements before movement. A returning distant actor must be announced first.
  auto packets = refresh_npc_sight(position);
  auto visible_patrol = npc_patrol_->advance(ticks, context_.npc_registry(),
                                             context_.npc_instance_key(), &npc_sight_);
  packets.insert(packets.end(), std::make_move_iterator(visible_patrol.begin()),
                 std::make_move_iterator(visible_patrol.end()));
  return packets;
}

} // namespace chronicles::npc
