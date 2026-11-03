#include "world/core/unit_registry.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace chronicles {
namespace {

constexpr std::uint64_t kMaxUnitId =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

bool is_valid_announcement(const AnnouncedUnit &unit) {
    return unit.unit_id != 0 && unit.unit_id <= kMaxUnitId && unit.npc_template_id > 0 &&
           unit.map_id > 0 && !unit.instance_key.empty();
}

double axis_difference(int first, int second) {
    return static_cast<double>(static_cast<std::int64_t>(first) -
                               static_cast<std::int64_t>(second));
}

} // namespace

std::optional<std::uint64_t> parse_unit_id_text(std::string_view text) {
    std::uint64_t value{};
    const char *const begin = text.data();
    const char *const end = text.data() + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != end || value == 0 ||
        value > kMaxUnitId) {
        return std::nullopt;
    }
    return value;
}

UnitRegistry::UnitRegistry(std::size_t capacity) : capacity_(capacity) {
    if (capacity == 0) {
        throw std::invalid_argument("UnitRegistry capacity must be non-zero");
    }
}

UnitAnnounceResult UnitRegistry::announce(AnnouncedUnit unit) {
    if (!is_valid_announcement(unit)) {
        return UnitAnnounceResult::InvalidUnit;
    }
    if (units_.contains(unit.unit_id)) {
        return UnitAnnounceResult::AlreadyAnnounced;
    }
    if (units_.size() >= capacity_) {
        return UnitAnnounceResult::CapacityExceeded;
    }

    const auto unit_id = unit.unit_id;
    units_.emplace(unit_id, std::move(unit));
    return UnitAnnounceResult::Added;
}

const AnnouncedUnit *UnitRegistry::find(std::uint64_t unit_id) const {
    const auto found = units_.find(unit_id);
    return found == units_.end() ? nullptr : &found->second;
}

const AnnouncedUnit *UnitRegistry::find_in_instance(std::uint64_t unit_id, int map_id,
                                                    std::string_view instance_key) const {
    const auto *unit = find(unit_id);
    if (unit == nullptr || unit->map_id != map_id || unit->instance_key != instance_key) {
        return nullptr;
    }
    return unit;
}

const AnnouncedUnit *UnitRegistry::find_low32_in_instance(std::uint32_t low_id, int map_id,
                                                          std::string_view instance_key) const {
  const AnnouncedUnit *result = nullptr;
  for (const auto &[id, unit] : units_) {
    if (static_cast<std::uint32_t>(id) != low_id || unit.map_id != map_id ||
        unit.instance_key != instance_key) {
      continue;
    }
    if (result) {
      return nullptr;
    }
    result = &unit;
  }
  return result;
}

bool UnitRegistry::update_position(std::uint64_t unit_id, int map_id,
                                   std::string_view instance_key, UnitPositionMm position) {
    const auto found = units_.find(unit_id);
    if (found == units_.end() || found->second.map_id != map_id ||
        found->second.instance_key != instance_key) {
        return false;
    }
    found->second.position = position;
    return true;
}

bool UnitRegistry::despawn(std::uint64_t unit_id) {
    return units_.erase(unit_id) != 0;
}

void UnitRegistry::clear() {
    units_.clear();
}

std::size_t UnitRegistry::size() const {
    return units_.size();
}

std::size_t UnitRegistry::capacity() const {
    return capacity_;
}

double distance_mm(const UnitPositionMm &first, const UnitPositionMm &second) {
    return std::hypot(axis_difference(first.x_mm, second.x_mm),
                      axis_difference(first.y_mm, second.y_mm),
                      axis_difference(first.z_mm, second.z_mm));
}

double horizontal_distance_mm(const UnitPositionMm &first, const UnitPositionMm &second) {
    return std::hypot(axis_difference(first.x_mm, second.x_mm),
                      axis_difference(first.y_mm, second.y_mm));
}

} // namespace chronicles
