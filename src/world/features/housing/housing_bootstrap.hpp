#pragma once

#include "world/features/housing/housing_world.hpp"

#include <map>
#include <optional>

namespace chronicles::housing {

// The non-tutorial entry cache travels with the handshake. Tutorial entry already takes its
// live storage snapshot through HousingSession; neither path duplicates the saved records.
class HousingBootstrap final {
public:
  void set_items(std::map<int, HousingItemRecord> items);
  void clear();
  std::optional<WorldPacket> snapshot() const;

private:
  std::map<int, HousingItemRecord> items_;
};

} // namespace chronicles::housing
