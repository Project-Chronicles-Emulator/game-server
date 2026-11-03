#pragma once

#include <cstdint>

namespace chronicles::character {

// The bounded progression operation already selected and authorized by its caller. This
// carries only the domain values needed by the existing progression transaction.
enum class ProgressionEditKind {
  add_experience,
  set_level,
  add_build_points,
  set_build_points,
  reset_stat_bonus,
  reset_perks,
  add_stat_bonus,
  set_stat_bonus,
  learn_all_perks,
};

struct ProgressionEdit {
  ProgressionEditKind kind{};
  std::int64_t value{};
  int build_point_type{};
  int stat_type{};
};

const char *progression_edit_name(ProgressionEditKind kind);

} // namespace chronicles::character
