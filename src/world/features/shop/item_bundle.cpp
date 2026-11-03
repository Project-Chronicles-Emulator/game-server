#include "world/features/shop/item_bundle.hpp"

namespace chronicles {

bool ItemBundlePlan::empty() const {
  return trade.granted_items.empty() && trade.changed_stackables.empty() &&
         trade.money_changes.empty();
}

} // namespace chronicles
