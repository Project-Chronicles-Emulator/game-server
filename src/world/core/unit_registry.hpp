#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace chronicles {

// A position exactly as the server sent it in a unit announcement, in millimeters.
struct UnitPositionMm {
    int x_mm{};
    int y_mm{};
    int z_mm{};
};

// One unit the SERVER announced to this session. Every field is copied from the server's own
// announcement; nothing here is read from a client request.
struct AnnouncedUnit {
    std::uint64_t unit_id{};
    int npc_template_id{};
    int map_id{};
    // Identifies the instance the server placed this session's units in (for example a private
    // tutorial instance). The caller supplies it; the registry never derives or defaults it.
    std::string instance_key;
    UnitPositionMm position;
};

enum class UnitAnnounceResult {
    Added,
    InvalidUnit,
    AlreadyAnnounced,
    CapacityExceeded,
};

// Strict decimal parse of a client-supplied `target_unit_id`. Accepts digits only, no sign or
// whitespace, a non-zero value not above INT64_MAX (the same bounds the tutorial session uses
// for unit ids). The result says nothing about whether the server announced that unit.
std::optional<std::uint64_t> parse_unit_id_text(std::string_view text);

// Per-session record of announced units, so a later authorizer can resolve a client-supplied
// unit id to a server-owned unit. Not thread-safe by design: it is confined to the session
// worker that sends the announcements. There is no global state and no default capacity.
class UnitRegistry {
public:
    // `capacity` must be non-zero (std::invalid_argument otherwise).
    explicit UnitRegistry(std::size_t capacity);

    UnitAnnounceResult announce(AnnouncedUnit unit);

    // Returns the announced unit, or nullptr for an id the server never announced (or already
    // despawned). The pointer stays valid until that unit is despawned or the registry is
    // cleared or destroyed.
    const AnnouncedUnit *find(std::uint64_t unit_id) const;

    // Same, but only if the unit was announced on `map_id` in `instance_key`.
    const AnnouncedUnit *find_in_instance(std::uint64_t unit_id, int map_id,
                                          std::string_view instance_key) const;

    // Resolves the low 32 bits carried by ThreatenedNpcReq. Ambiguous identities fail closed.
    const AnnouncedUnit *find_low32_in_instance(std::uint32_t low_id, int map_id,
                                                std::string_view instance_key) const;

    // Advances a server-owned actor without changing its identity, template or ownership.
    // The map and instance must still match the original announcement.
    bool update_position(std::uint64_t unit_id, int map_id, std::string_view instance_key,
                         UnitPositionMm position);

    // Removes the unit. Returns false when no such unit was announced.
    bool despawn(std::uint64_t unit_id);

    void clear();

    std::size_t size() const;
    std::size_t capacity() const;

private:
    std::size_t capacity_;
    std::unordered_map<std::uint64_t, AnnouncedUnit> units_;
};

// Raw distances in millimeters only. No interaction range is defined here: choosing a
// threshold needs a verified source and belongs to the authorizer, not this module.
double distance_mm(const UnitPositionMm &first, const UnitPositionMm &second);
double horizontal_distance_mm(const UnitPositionMm &first, const UnitPositionMm &second);

} // namespace chronicles
