#pragma once

#include "world/features/inventory/inventory_encumbrance.hpp"

namespace chronicles::inventory_testing {

using Json = nlohmann::json;

inline EncumbranceCatalogRows catalog_rows() {
  EncumbranceCatalogRows rows;
  rows.game_variables = {
      {{"name", "BagDefaultSafeWeight"}, {"value", "50000"}},
      {{"name", "BagMaxWeightMultiplier"}, {"value", "200"}},
      {{"name", "InvWeightMoveSpeedStep2RatioFrom"}, {"value", "0.5"}},
      {{"name", "StatusEffectIdForInvWeightHeavy"}, {"value", "50030004"}},
      {{"name", "StatusEffectIdForInvWeightVeryHeavy"}, {"value", "50030005"}},
  };
  rows.status_effects = {
      {{"id", "50030004"}, {"ui_type", "Debuff"}, {"FK_maintainEffects", "(50030004)"}},
      {{"id", "50030005"}, {"ui_type", "Debuff"}, {"FK_maintainEffects", "(50030005)"}},
  };
  rows.maintain_effects = {
      {{"id", "50030004"}, {"maintainEffectType", "Stat"}, {"maintainEffectValue", "50030004"}},
      {{"id", "50030005"}, {"maintainEffectType", "Stat"}, {"maintainEffectValue", "50030005"}},
  };
  rows.maintain_stats = {
      {{"id", "50030004"}, {"statType", "MoveSpeedRate"}, {"statAdd", "-500"}, {"statMul", ""}},
      {{"id", "50030005"}, {"statType", "MoveSpeedRate"}, {"statAdd", "-1500"}, {"statMul", ""}},
  };
  rows.stats = {
      {{"id", "MaxHP"}, {"min", "1"}, {"max", "9999999"}},
      {{"id", "MoveSpeedRate"}, {"min", "200"}, {"max", "1500"}},
  };
  return rows;
}

} // namespace chronicles::inventory_testing
