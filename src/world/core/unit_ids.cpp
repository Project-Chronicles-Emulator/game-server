#include "world/core/unit_ids.hpp"

namespace chronicles {

bool is_server_npc_unit_id(std::uint64_t unit_id) {
  return unit_id >= kFirstNpcUnitId && unit_id <= kLastNpcUnitId;
}

} // namespace chronicles
