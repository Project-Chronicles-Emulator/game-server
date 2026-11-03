#include "world/features/character/character_feature.hpp"

#include "cms.generated.pb.h"
#include "common/clock_ticks.hpp"

#include <spdlog/spdlog.h>

#include <limits>
#include <mutex>
#include <utility>

namespace chronicles::character {
namespace {

constexpr std::uint16_t kCombatWeaponGaugeNotification = 78;
constexpr std::uint16_t kWorldCharPerkNotification = 135;

bool is_progression_request(std::uint16_t opcode) {
  return opcode == kUpdateStatBonusRequest || opcode == kChangePerkRequest ||
         opcode == kResetPerkRequest || opcode == kUpdateAchievementRequest;
}

} // namespace

CharacterSession::CharacterSession(CharacterFeature &feature, CharacterSessionContext &context,
                                   SessionEventBus &events)
    : feature_(feature), context_(context), events_(events) {
}

std::optional<CharacterSession::ProgressionChange> CharacterSession::begin_progression_change() {
  if (!context_.character_has_handoff() || !feature_.progression()) {
    return std::nullopt;
  }
  auto character = context_.progression_character();
  if (!character) {
    return std::nullopt;
  }

  ProgressionChange change;
  change.before = context_.character_tutorial_progress() ? *context_.character_tutorial_progress()
                                                         : context_.progression_stored_progress();
  change.after = change.before;
  // The progress copy is the authority for every value that feeds the stat calculation.
  character->level = change.before.level;
  character->experience = change.before.experience;
  character->build = change.before.build;
  change.character = std::move(*character);
  return change;
}

bool CharacterSession::commit_progression_change(ProgressionChange &change,
                                                 std::vector<GameplayPacket> &packets) {
  Character updated = change.character;
  updated.level = change.after.level;
  updated.experience = change.after.experience;
  updated.build = change.after.build;

  std::optional<nlohmann::json> stats_before;
  std::optional<nlohmann::json> stats_after;
  if (feature_.has_stats()) {
    try {
      stats_before = feature_.stats(change.character);
      stats_after = feature_.stats(updated);
    } catch (const std::exception &error) {
      spdlog::warn("Cannot calculate build stats for character {}: {}", updated.id, error.what());
      return false;
    }
  }

  if (change.after != change.before) {
    if (change.before.revision == std::numeric_limits<std::int64_t>::max()) {
      return false;
    }
    ++change.after.revision;

    bool committed = false;
    try {
      if (change.award_activity_experience && context_.character_tutorial_progress() &&
          change.after.experience > change.before.experience) {
        committed = context_.progression_save_activity(change.before, change.after);
      } else {
        committed = context_.progression_save(change.before, change.after);
      }
    } catch (const std::exception &error) {
      spdlog::warn("Progression commit rejected for character {}: {}", updated.id, error.what());
    }
    if (!committed) {
      return false;
    }
    if (context_.character_tutorial_progress()) {
      events_.publish(PlayerProgressCommitted{change.after});
    }
  }

  const bool stats_changed = stats_before && stats_after && *stats_before != *stats_after;
  if (stats_changed && context_.character_has_combat()) {
    context_.character_update_combat_stats(*stats_after);
  }
  {
    std::lock_guard lock(context_.character_protocol_mutex());
    events_.publish(
        PlayerProgressionSnapshotChanged{updated.level, updated.experience, updated.build});
    if (stats_changed) {
      events_.publish(PlayerInventorySnapshotChanged{
          updated,
          context_.character_has_combat() ? context_.character_combat_stats() : &*stats_after});
    }
  }
  if (stats_changed) {
    const auto &current =
        context_.character_has_combat() ? *context_.character_combat_stats() : *stats_after;
    packets.push_back({kCombatWeaponGaugeNotification, nlohmann::json::object()});
    packets.push_back(context_.character_stat_notification(
        context_.character_player_unit_id(updated.id), *stats_before, *stats_after, current));
  }
  return true;
}

void CharacterSession::apply_level_change(const WorldProgress &progress,
                                          std::vector<GameplayPacket> &packets) {
  if (!context_.character_has_handoff() || !context_.character_party_level() ||
      progress.level == *context_.character_party_level()) {
    return;
  }

  const bool gained = progress.level > *context_.character_party_level();
  events_.publish(PlayerLevelChanged{progress.level});
  {
    std::lock_guard lock(context_.character_protocol_mutex());
    events_.publish(
        PlayerProgressionSnapshotChanged{progress.level, progress.experience, progress.build});
  }

  const auto unit_id = context_.character_player_unit_id(context_.progression_character_id());
  if (gained && context_.character_has_combat() && context_.character_refill_player_vitals()) {
    const auto &current = *context_.character_combat_stats();
    packets.push_back({kCombatWeaponGaugeNotification, nlohmann::json::object()});
    packets.push_back(context_.character_stat_notification(unit_id, current, current, current));
  }

  const std::vector<GameplayPacket> public_change{
      {kChangeCharacterPublicNotification,
       {{"unit_id", std::to_string(unit_id)}, {"level", progress.level}}}};
  context_.character_deliver_observers(public_change);
}

std::vector<GameplayPacket>
CharacterSession::progression_request(const WorldGameplayRequest &request) {
  switch (request.opcode) {
  case kUpdateStatBonusRequest:
    return update_stat_bonus(request);
  case kUpdateAchievementRequest:
    return update_achievement(request);
  case kChangePerkRequest:
    return change_perks(request);
  case kResetPerkRequest:
    return reset_perks(request);
  default:
    return {};
  }
}

std::vector<GameplayPacket>
CharacterSession::update_achievement(const WorldGameplayRequest &request) {
  auto change = begin_progression_change();
  if (!change || !feature_.achievements()) {
    return {context_.character_error(request, x5::ec::InvalidState)};
  }
  const int id = request.message.value("cms_id", 0);
  x5::cms::EX5AchievementState state{};
  if (!x5::cms::EX5AchievementState_Parse(request.message.value("achievement_state", std::string{}),
                                          &state)) {
    return {context_.character_error(request, x5::ec::Achievement_InvalidState)};
  }
  const auto facts = [&](const AchievementGoal &goal) {
    return stored_achievement_goal_value(goal, change->before);
  };
  auto plan =
      feature_.achievements()->plan(change->before, id, static_cast<int>(state),
                                    unix_epoch_ticks_now(), facts, feature_.progression().get());
  if (plan.error != x5::ec::Success) {
    return {context_.character_error(request, plan.error)};
  }
  change->after = std::move(plan.after);
  change->award_activity_experience = true;
  if (!commit_progression_change(*change, plan.packets)) {
    return {context_.character_error(request, x5::ec::BadRequest)};
  }
  apply_level_change(change->after, plan.packets);
  return plan.packets;
}

std::vector<GameplayPacket>
CharacterSession::update_stat_bonus(const WorldGameplayRequest &request) {
  const auto entries = parse_stat_bonus_request(request.message);
  if (!entries) {
    return {context_.character_error(request, x5::ec::StatBonus_InvalidRequest)};
  }
  auto change = begin_progression_change();
  if (!change) {
    return {context_.character_error(request, x5::ec::BadRequest)};
  }

  const auto status = feature_.progression()->allocate_stat_bonus(change->after, *entries);
  if (status != x5::ec::Success) {
    return {context_.character_error(request, status)};
  }

  std::vector<GameplayPacket> packets{
      {kChangeStatBonusNotification, level_up_points_message(change->after)}};
  if (!commit_progression_change(*change, packets)) {
    return {context_.character_error(request, x5::ec::BadRequest)};
  }
  return packets;
}

GameplayPacket CharacterSession::perk_response(int packet_index, x5::ec::EErrorCode status,
                                               const WorldProgress &progress) {
  nlohmann::json message{{"packet_index", packet_index},
                         {"remaining_char_build_points", remaining_build_points_json(progress)}};
  if (status != x5::ec::Success) {
    message["ec"] = x5::ec::EErrorCode_Name(status);
  }
  return {kChangePerkResponse, std::move(message)};
}

GameplayPacket CharacterSession::unit_perk_packet(const std::vector<PerkUpdate> &perks) const {
  nlohmann::json entries = nlohmann::json::array();
  for (const auto &perk : perks) {
    entries.push_back({{"cms_id", perk.cms_id}, {"level", perk.level}});
  }
  return {kUnitPerkNotification,
          {{"unit_id",
            std::to_string(context_.character_player_unit_id(context_.progression_character_id()))},
           {"perks", std::move(entries)}}};
}

std::vector<GameplayPacket> CharacterSession::change_perks(const WorldGameplayRequest &request) {
  auto change = begin_progression_change();
  if (!change) {
    return {context_.character_error(request, x5::ec::BadRequest)};
  }
  const auto parsed = parse_perk_change_request(request.message);
  if (!parsed) {
    return {perk_response(0, x5::ec::BadRequest, change->before)};
  }

  const auto status = feature_.progression()->change_perks(
      change->after, change->character.class_id, parsed->updates);
  if (status != x5::ec::Success) {
    return {perk_response(parsed->packet_index, status, change->before)};
  }

  std::vector<GameplayPacket> packets{
      perk_response(parsed->packet_index, x5::ec::Success, change->after)};
  if (!commit_progression_change(*change, packets)) {
    return {perk_response(parsed->packet_index, x5::ec::BadRequest, change->before)};
  }
  context_.character_deliver_observers({unit_perk_packet(parsed->updates)});
  return packets;
}

std::vector<GameplayPacket> CharacterSession::reset_perks(const WorldGameplayRequest &request) {
  const auto theme = parse_perk_reset_request(request.message);
  if (!theme) {
    return {context_.character_error(request, x5::ec::PerkNode_InvalidTheme)};
  }
  auto change = begin_progression_change();
  if (!change) {
    return {context_.character_error(request, x5::ec::BadRequest)};
  }

  std::vector<int> reset_nodes;
  const auto status = feature_.progression()->reset_perks(change->after, change->character.class_id,
                                                          *theme, &reset_nodes);
  if (status != x5::ec::Success) {
    return {context_.character_error(request, status)};
  }

  std::vector<GameplayPacket> packets{
      {kResetPerkNotification,
       {{"remaining_char_build_points", remaining_build_points_json(change->after)}}}};
  if (!commit_progression_change(*change, packets)) {
    return {context_.character_error(request, x5::ec::BadRequest)};
  }

  std::vector<PerkUpdate> deactivated;
  for (const int node : reset_nodes) {
    deactivated.push_back({node, 0});
  }
  if (!deactivated.empty()) {
    context_.character_deliver_observers({unit_perk_packet(deactivated)});
  }
  return packets;
}

std::vector<GameplayPacket> CharacterSession::apply_operator_edit(const ProgressionEdit &edit) {
  const char *command_name = progression_edit_name(edit.kind);
  auto change = begin_progression_change();
  if (!change) {
    spdlog::warn("GM {} refused for character {}: progression is unavailable", command_name,
                 context_.progression_character_id());
    return {};
  }

  const auto &progression = *feature_.progression();
  auto &after = change->after;
  std::vector<GameplayPacket> packets;
  // Perk nodes other players must learn about once the change is committed.
  std::vector<PerkUpdate> observer_perks;
  switch (edit.kind) {
  case ProgressionEditKind::add_experience: {
    const auto result = progression.award_experience(after, edit.value);
    packets.push_back({kChangeCharacterBaseNotification, experience_change_message(after, result)});
    break;
  }
  case ProgressionEditKind::set_level: {
    if (edit.value > progression.max_level()) {
      spdlog::info("GM set_level refused for character {}: the level cap is {}",
                   context_.progression_character_id(), progression.max_level());
      return {};
    }
    const auto level = static_cast<int>(edit.value);
    const auto result = progression.set_level(after, level);
    packets.push_back({kChangeCharacterBaseNotification, experience_change_message(after, result)});
    break;
  }
  case ProgressionEditKind::add_build_points:
  case ProgressionEditKind::set_build_points: {
    const bool weapon = edit.build_point_type == kWeaponPointType;
    const int current = weapon ? after.weapon_points : after.level_up_points;
    std::int64_t delta = edit.value;
    if (edit.kind == ProgressionEditKind::set_build_points) {
      delta = edit.value - current;
    }
    const auto status =
        progression.grant_build_points(after, edit.build_point_type, static_cast<int>(delta));
    if (status != x5::ec::Success) {
      return {};
    }
    const auto message = weapon ? weapon_points_message(after) : level_up_points_message(after);
    packets.push_back({kChangeCharacterBaseNotification, message});
    break;
  }
  case ProgressionEditKind::reset_stat_bonus:
    progression.reset_stat_bonus(after);
    packets.push_back({kChangeCharacterBaseNotification, level_up_points_message(after)});
    break;
  case ProgressionEditKind::reset_perks:
    progression.reset_perks(after, change->character.class_id, std::nullopt);
    packets.push_back({kResetPerkNotification,
                       {{"remaining_char_build_points", remaining_build_points_json(after)}}});
    break;
  case ProgressionEditKind::add_stat_bonus:
  case ProgressionEditKind::set_stat_bonus: {
    std::int64_t value = edit.value;
    if (edit.kind == ProgressionEditKind::add_stat_bonus) {
      value += Progression::stat_bonus(after, edit.stat_type);
    }
    if (value > kMaximumStatBonus) {
      return {};
    }
    const auto status =
        progression.gm_set_stat_bonus(after, edit.stat_type, static_cast<int>(value));
    if (status != x5::ec::Success) {
      return {};
    }
    // Observed for player allocations: ChangeStatBonusNoti, then the stat follow-ups.
    packets.push_back({kChangeStatBonusNotification, level_up_points_message(after)});
    break;
  }
  case ProgressionEditKind::learn_all_perks: {
    const auto learned = progression.gm_learn_all_perks(after, change->character.class_id);
    if (learned.empty()) {
      return {};
    }
    // Candidate: the entry snapshot (WorldCharPerkNoti) carries the whole active tree.
    packets.push_back({kWorldCharPerkNotification,
                       perk_snapshot_message(progression, change->character.class_id, after)});
    observer_perks = learned;
    break;
  }
  default:
    return {};
  }

  if (!commit_progression_change(*change, packets)) {
    spdlog::warn("GM {} commit failed for character {}", command_name,
                 context_.progression_character_id());
    return {};
  }
  if (!observer_perks.empty()) {
    context_.character_deliver_observers({unit_perk_packet(observer_perks)});
  }
  apply_level_change(change->after, packets);
  spdlog::info("GM {} applied for character {}", command_name, context_.progression_character_id());
  return packets;
}

bool CharacterSession::handle(FrameContext &frame) {
  const auto *gameplay_request = frame.request();
  auto &session_packets = frame.session_packets;

  // Character build requests (attribute points, perk activation and theme reset) commit
  // through the session's current progress revision, in tutorial and shared maps alike.
  if (gameplay_request && is_progression_request(gameplay_request->opcode)) {
    session_packets = progression_request(*gameplay_request);
    return true;
  }

  return false;
}

} // namespace chronicles::character
