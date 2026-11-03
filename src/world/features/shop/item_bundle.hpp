#pragma once

#include "world/features/shop/npc_shop.hpp"

#include <vector>

namespace chronicles {

// Shared grant input used by GM bundles and the character graduation kit. Instances are
// added once; stackable stock is raised to count rather than increased by count.
struct ItemBundleEntry {
  int item_cms_id{};
  int count{1};
};

// A real atomic item/progress plan. Planning remains with the existing planner; this value
// belongs beside ShopTrade so character rules do not depend on the GM protocol owner.
struct ItemBundlePlan {
  x5::ec::EErrorCode error{x5::ec::Success};
  ShopTrade trade;
  std::vector<int> skipped_item_ids;

  bool empty() const;
};

} // namespace chronicles
