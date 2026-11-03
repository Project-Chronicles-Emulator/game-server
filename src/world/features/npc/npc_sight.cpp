#include "world/features/npc/npc_sight.hpp"

#include "world/features/npc/npc_population.hpp"

#include <string>
#include <utility>

namespace chronicles {
namespace {

bool within_horizontal_radius(const UnitPositionMm &first, const UnitPositionMm &second,
                              std::int64_t radius_mm) {
  // Decoded coordinates span int32; squaring their full-range difference can overflow int64.
  const double delta_x = static_cast<double>(first.x_mm) - second.x_mm;
  const double delta_y = static_cast<double>(first.y_mm) - second.y_mm;
  const double radius = static_cast<double>(radius_mm);
  return delta_x * delta_x + delta_y * delta_y <= radius * radius;
}

} // namespace

std::vector<GameplayPacket> NpcSightTracker::update(const NpcPopulation &population,
                                                    const UnitPositionMm &viewer,
                                                    std::int64_t elapsed_ticks,
                                                    const npc::ReactionTimeline &timeline,
                                                    std::int64_t epoch_ticks) {
  std::vector<GameplayPacket> packets;
  nlohmann::json leaving = nlohmann::json::array();

  for (const auto &unit : population.units()) {
    const bool known = visible_.contains(unit.unit_id);
    const auto radius = known ? kNpcSightLeaveRadiusMm : kNpcSightEnterRadiusMm;
    const auto reaction = timeline ? timeline(unit.unit_id, elapsed_ticks)
                                   : npc::ReactionTimelineSample{elapsed_ticks};
    const auto position = NpcPopulation::unit_position(unit, reaction.route_elapsed);
    const bool in_range = within_horizontal_radius(viewer, position, radius);

    if (!known && in_range) {
      visible_.insert(unit.unit_id);
      auto added = NpcPopulation::unit_added_packet(unit, reaction.route_elapsed);
      if (reaction.active) {
        added.message["nub"]["npc"]["move_detail"] = "MoveDetail_Stop";
        added.message["nub"]["yaw"] = reaction.yaw;
      }
      packets.push_back(std::move(added));
      if (reaction.active) {
        packets.push_back(
            npc::threat_reaction_packet(unit.unit_id, position, reaction, epoch_ticks));
      }
    } else if (known && !in_range) {
      visible_.erase(unit.unit_id);
      leaving.push_back(std::to_string(unit.unit_id));
    }
  }

  if (!leaving.empty()) {
    packets.push_back(
        {kNpcSightUnitRemovedOpcode, {{"unit_ids", std::move(leaving)}, {"by_sight", true}}});
  }
  return packets;
}

bool NpcSightTracker::visible(std::uint64_t unit_id) const {
  return visible_.contains(unit_id);
}

std::size_t NpcSightTracker::visible_count() const {
  return visible_.size();
}

std::vector<GameplayPacket> NpcSightTracker::visible_only(
    std::vector<GameplayPacket> packets) const {
  std::erase_if(packets, [this](const GameplayPacket &packet) {
    const auto unit_id = packet.message.find("unit_id");
    if (unit_id == packet.message.end() || !unit_id->is_string()) {
      return false;
    }
    const auto parsed = parse_unit_id_text(unit_id->get<std::string>());
    return parsed && is_server_npc_unit_id(*parsed) && !visible_.contains(*parsed);
  });
  return packets;
}

void NpcSightTracker::clear() {
  visible_.clear();
}

} // namespace chronicles
