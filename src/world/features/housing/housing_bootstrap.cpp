#include "world/features/housing/housing_bootstrap.hpp"

#include <utility>

namespace chronicles::housing {

void HousingBootstrap::set_items(std::map<int, HousingItemRecord> items) {
  items_ = std::move(items);
}

void HousingBootstrap::clear() {
  items_.clear();
}

std::optional<WorldPacket> HousingBootstrap::snapshot() const {
  if (items_.empty()) {
    return std::nullopt;
  }
  return housing_items_packet(items_);
}

} // namespace chronicles::housing
