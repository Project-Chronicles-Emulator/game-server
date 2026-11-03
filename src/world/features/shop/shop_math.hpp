#pragma once

#include "data/shop_catalog.hpp"

#include <cstdint>
#include <limits>
#include <optional>

namespace chronicles {

// Overflow-checked non-negative arithmetic for prices, refunds and weights. Every function
// returns nullopt for a negative operand or a result outside int64.
inline std::optional<std::int64_t> checked_multiply(std::int64_t left, std::int64_t right) {
  if (left < 0 || right < 0) {
    return std::nullopt;
  }
  if (left != 0 && right > std::numeric_limits<std::int64_t>::max() / left) {
    return std::nullopt;
  }
  return left * right;
}

inline std::optional<std::int64_t> checked_add(std::int64_t left, std::int64_t right) {
  if (left < 0 || right < 0 || right > std::numeric_limits<std::int64_t>::max() - left) {
    return std::nullopt;
  }
  return left + right;
}

// base x multiplier / 1000 x count. Captured purchases match this formula, but no fractional
// unit price was observed, so one is rejected rather than rounded.
inline std::optional<std::int64_t>
scaled_amount(std::int64_t base, std::int64_t multiplier_per_mille, std::int64_t count) {
  const auto scaled = checked_multiply(base, multiplier_per_mille);
  if (!scaled || *scaled % kPerMille != 0) {
    return std::nullopt;
  }
  return checked_multiply(*scaled / kPerMille, count);
}

// ceil(numerator / denominator) for a positive denominator.
inline std::optional<std::int64_t> checked_ceiling_divide(std::int64_t numerator,
                                                          std::int64_t denominator) {
  if (numerator < 0 || denominator <= 0) {
    return std::nullopt;
  }
  return numerator / denominator + (numerator % denominator != 0 ? 1 : 0);
}

} // namespace chronicles
