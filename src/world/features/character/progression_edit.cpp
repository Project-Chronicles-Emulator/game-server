#include "world/features/character/progression_edit.hpp"

namespace chronicles::character {

const char *progression_edit_name(ProgressionEditKind kind) {
  switch (kind) {
  case ProgressionEditKind::add_experience:
    return "exp_up";
  case ProgressionEditKind::set_level:
    return "set_level";
  case ProgressionEditKind::add_build_points:
    return "add_char_build_point";
  case ProgressionEditKind::set_build_points:
    return "set_char_build_point";
  case ProgressionEditKind::reset_stat_bonus:
    return "reset_stat_bonus";
  case ProgressionEditKind::reset_perks:
    return "force_reset_perk";
  case ProgressionEditKind::add_stat_bonus:
    return "add_stat_bonus";
  case ProgressionEditKind::set_stat_bonus:
    return "set_stat_bonus";
  case ProgressionEditKind::learn_all_perks:
    return "learn_all_perks";
  }
  return "unknown";
}

} // namespace chronicles::character
