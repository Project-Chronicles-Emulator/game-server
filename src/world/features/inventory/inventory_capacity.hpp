#pragma once

#include "persistence/character_repository.hpp"
#include "persistence/character_state_types.hpp"

#include <cstdint>
#include <unordered_map>

namespace chronicles {

class ClientCatalog;

struct CarriedWeight {
  std::int64_t total{};
  std::int64_t safe{};
};

// One hash-verified catalog load at listener startup; no table reads while
// measuring a character. A capacity rejection needs an observed grant rule.
class InventoryCapacity {
public:
  explicit InventoryCapacity(ClientCatalog &catalog);

  CarriedWeight measure(const Character &character, const WorldProgress &progress) const;

private:
  struct ItemWeight {
    std::int64_t per_unit{};
    std::int64_t bag_safe{};
    bool is_bag{};
  };

  std::unordered_map<int, ItemWeight> item_weights_;
  std::int64_t default_safe_weight_{};
};

} // namespace chronicles
