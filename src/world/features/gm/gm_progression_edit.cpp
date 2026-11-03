#include "world/features/gm/gm_commands.hpp"

namespace chronicles {

std::optional<character::ProgressionEdit> character_progression_edit(const GmCommand &command) {
  character::ProgressionEdit edit;
  edit.value = command.progression_value;
  edit.build_point_type = command.build_point_type;
  edit.stat_type = command.stat_type;
  switch (command.kind) {
  case GmCommandKind::exp_up:
    edit.kind = character::ProgressionEditKind::add_experience;
    break;
  case GmCommandKind::set_level:
    edit.kind = character::ProgressionEditKind::set_level;
    break;
  case GmCommandKind::add_char_build_point:
    edit.kind = character::ProgressionEditKind::add_build_points;
    break;
  case GmCommandKind::set_char_build_point:
    edit.kind = character::ProgressionEditKind::set_build_points;
    break;
  case GmCommandKind::reset_stat_bonus:
    edit.kind = character::ProgressionEditKind::reset_stat_bonus;
    break;
  case GmCommandKind::force_reset_perk:
    edit.kind = character::ProgressionEditKind::reset_perks;
    break;
  case GmCommandKind::add_stat_bonus:
    edit.kind = character::ProgressionEditKind::add_stat_bonus;
    break;
  case GmCommandKind::set_stat_bonus:
    edit.kind = character::ProgressionEditKind::set_stat_bonus;
    break;
  case GmCommandKind::learn_all_perks:
    edit.kind = character::ProgressionEditKind::learn_all_perks;
    break;
  default:
    return std::nullopt;
  }
  return edit;
}

} // namespace chronicles
