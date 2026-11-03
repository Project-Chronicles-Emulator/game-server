#include "world/features/gm/gm_feature.hpp"

#include "common/clock_ticks.hpp"
#include "data/client_catalog.hpp"
#include "protocol/protocol.hpp"
#include "world/features/combat/combat.hpp"
#include "world/features/company/company_world.hpp"
#include "world/features/mail/mail.hpp"
#include "world/features/npc/npc_population.hpp"

#include "cms.generated.pb.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>

#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <spdlog/spdlog.h>

namespace chronicles::gm {
namespace {

// Existing world-PC2S/PS2C values; front-WebSocket IDs use a separate namespace.
constexpr int kDiagnosticTutorialMapId = 8000;
constexpr std::uint16_t kUnitAddNotification = 10;
constexpr std::uint16_t kUnitRemoveNotification = 11;
constexpr std::uint16_t kUnitChangeAttributeNotification = 33;

} // namespace

GmFeature::GmFeature(world_metrics::TimedMutex &protocol_mutex) : protocol_mutex_(protocol_mutex) {
}

void GmFeature::set_reply_handler(ReplyHandler handler) {
  std::lock_guard lock(protocol_mutex_);
  reply_handler_ = std::move(handler);
}

void GmFeature::set_announce_handler(AnnounceHandler handler) {
  std::lock_guard lock(protocol_mutex_);
  announce_handler_ = std::move(handler);
}

GmFeature::ReplyHandler GmFeature::reply_handler() const {
  std::lock_guard lock(protocol_mutex_);
  return reply_handler_;
}

GmFeature::AnnounceHandler GmFeature::announce_handler() const {
  std::lock_guard lock(protocol_mutex_);
  return announce_handler_;
}

std::unique_ptr<GmSession> GmFeature::create_session(GmSessionPorts ports,
                                                     SessionEventBus &events) {
  return std::make_unique<GmSession>(*this, ports, events);
}

std::unique_ptr<GmSession> GmFeature::create_session(GmSessionContext &context,
                                                     SessionEventBus &events) {
  return std::make_unique<GmSession>(*this, context, events);
}

GmSession::GmSession(GmFeature &feature, GmSessionPorts ports, SessionEventBus &events)
    : feature_(feature), runtime_(ports.runtime), combat_(ports.combat),
      character_(ports.character), social_(ports.social), quests_(ports.quests),
      world_(ports.world), mail_(ports.mail), dispatch_(ports.dispatch), events_(events) {
}

GmSession::GmSession(GmFeature &feature, GmSessionContext &context, SessionEventBus &events)
    : GmSession(feature,
                GmSessionPorts{.runtime = context,
                               .combat = context,
                               .character = context,
                               .social = context,
                               .quests = context,
                               .world = context,
                               .mail = context,
                               .dispatch = context},
                events) {
}

bool GmSession::god_mode() const {
  return gm_god_;
}

bool GmSession::infinite_stamina() const {
  return gm_infinite_stamina_;
}

bool GmSession::free_movement() const {
  return gm_free_movement_;
}

bool GmSession::invisible() const {
  return gm_invisible_.load();
}

bool GmSession::has_spawned_units() const {
  return !gm_spawned_units_.empty();
}

void GmSession::forget_spawned_units() {
  gm_spawned_units_.clear();
}

std::optional<std::int64_t> GmSession::next_watch_tick() const {
  return gm_watches_.next_tick();
}

bool GmSession::watches_due(std::int64_t ticks) const {
  return gm_watches_.due(ticks);
}

void GmSession::mark_watches_emitted(std::int64_t ticks) {
  gm_watches_.mark_emitted(ticks);
}

void GmSession::post_frame(Frame frame) {
  runtime_.gm_post_frame(std::move(frame));
}

std::vector<GameplayPacket> GmSession::social_gm_command(const WorldGameplayRequest &request,
                                                         const SocialGmParse &parsed) {
  if (!runtime_.gm_identity() || !runtime_.gm_identity()->game_master ||
      !runtime_.gm_chat_identity()) {
    spdlog::warn("GM opcode {} refused: not a game master session", request.opcode);
    return {};
  }
  if (!parsed.error.empty()) {
    spdlog::info("GM opcode {} not executed for character {}: {}", request.opcode,
                 runtime_.gm_identity()->character_id, parsed.error);
    return {};
  }

  std::vector<GameplayPacket> packets;
  for (const auto &command : parsed.commands) {
    const auto error = execute_social_gm(command, packets);
    const char *name = social_gm_command_name(command.kind);
    if (error != x5::ec::Success) {
      spdlog::info("GM {} rejected for character {}: {}", name,
                   runtime_.gm_identity()->character_id, x5::ec::EErrorCode_Name(error));
      break;
    }
    spdlog::info("GM {} applied for character {}", name, runtime_.gm_identity()->character_id);
  }
  return packets;
}

x5::ec::EErrorCode GmSession::execute_social_gm(const SocialGmCommand &command,
                                                std::vector<GameplayPacket> &packets) {
  const auto char_id = runtime_.gm_chat_identity()->character_id;
  if (command.kind == SocialGmCommandKind::company) {
    return execute_company_gm(command, packets);
  }
  if (!social_.gm_has_guilds()) {
    return x5::ec::GmCmd_Fail;
  }

  const auto own_guild = social_.gm_own_guild_id(char_id);
  const auto guild_id = command.guild_id != 0 ? command.guild_id : own_guild;
  switch (command.kind) {
  case SocialGmCommandKind::guild_experience:
    return social_.gm_guild_add_experience(guild_id, static_cast<int>(command.amount));
  case SocialGmCommandKind::guild_level:
    return social_.gm_guild_set_level(guild_id, static_cast<int>(command.amount));
  case SocialGmCommandKind::guild_disband:
    return social_.gm_guild_disband(guild_id);
  case SocialGmCommandKind::guild_master:
    return social_.gm_guild_set_master(guild_id, command.char_id);
  case SocialGmCommandKind::guild_reset_cooldown:
    return social_.gm_guild_reset_cooldowns(command.char_id != 0 ? command.char_id : char_id);
  case SocialGmCommandKind::company:
    break;
  }
  return x5::ec::GmCmd_Fail;
}

x5::ec::EErrorCode GmSession::execute_company_gm(const SocialGmCommand &command,
                                                 std::vector<GameplayPacket> &packets) {
  if (!social_.gm_has_companies()) {
    return x5::ec::GmCmd_Fail;
  }

  const auto char_id = runtime_.gm_chat_identity()->character_id;
  auto record = social_.gm_company_record(char_id);
  const bool company_changed = command.company_type && *command.company_type != record.company_type;
  const auto now = runtime_.gm_epoch_ticks();
  if (company_changed) {
    record.company_type = *command.company_type;
    record.contribution = 0;
    record.join_epoch_ticks = now;
    record.unlocked_position_level = 0;
    record.permit_expiry_epoch_ticks = 0;
  }
  if (record.company_type == kCompanyNone) {
    return x5::ec::Company_IsNotJoined;
  }

  if (command.contribution_delta) {
    record.contribution =
        std::max<std::int64_t>(0, record.contribution + *command.contribution_delta);
  }
  if (command.position_level) {
    const auto threshold =
        social_.gm_company_threshold(record.company_type, *command.position_level);
    if (!threshold) {
      return x5::ec::Company_PositionLevelMismatch;
    }
    record.contribution = *threshold;
    record.unlocked_position_level =
        std::max(record.unlocked_position_level, *command.position_level);
  }
  if (command.unlock_level) {
    record.unlocked_position_level =
        std::max(record.unlocked_position_level, *command.unlock_level);
  }
  if (command.permit_minutes) {
    constexpr std::int64_t kTicksPerMinute = 60LL * kEpochTicksPerSecond;
    record.permit_expiry_epoch_ticks =
        *command.permit_minutes == 0 ? 0 : now + *command.permit_minutes * kTicksPerMinute;
  }
  record.activity_epoch_ticks = now;

  // A GM switch does not remove the character from a guild that belongs to another company.
  const auto result = social_.gm_commit_company_record(record);
  if (result.error != x5::ec::Success) {
    return result.error;
  }
  events_.publish(CompanyMembershipChanged{});
  const auto opcode = company_changed ? company_world::kJoinCompanyResponse
                                      : company_world::kCompanyUpdateNotification;
  packets.push_back(company_packet(opcode, result.status.record));
  return x5::ec::Success;
}

GameplayPacket GmSession::gm_invincible_attribute(bool enabled) const {
  // Invincible is attribute 2 in UnitChangeAttributeNoti, as used by PlayerEscape.
  const auto unit_id =
      runtime_.gm_identity() ? player_wire_unit_id(runtime_.gm_identity()->character_id) : 0;
  return {kUnitChangeAttributeNotification,
          {{"unit_id", std::to_string(unit_id)}, {"attributes", {{"2", enabled ? 1 : 0}}}}};
}

bool GmSession::allow_gm_request() {
  const auto now = runtime_.gm_monotonic_ticks();
  if (now - gm_window_start_ticks_ >= kGmRequestWindowTicks) {
    gm_window_start_ticks_ = now;
    gm_requests_in_window_ = 0;
  }
  ++gm_requests_in_window_;
  return gm_requests_in_window_ <= kMaximumGmRequestsPerWindow;
}

bool GmSession::allow_player_car() {
  const auto now = runtime_.gm_monotonic_ticks();
  if (last_player_car_ticks_ != 0 && now - last_player_car_ticks_ < kPlayerCarCooldownTicks) {
    return false;
  }
  last_player_car_ticks_ = now;
  return true;
}

std::vector<GameplayPacket> GmSession::gm_command(const WorldGameplayRequest &request) {
  if (!runtime_.gm_identity()) {
    return {};
  }
  if (!runtime_.gm_identity()->game_master) {
    spdlog::warn("GM opcode {} refused for character {}: account is not a game master",
                 request.opcode, runtime_.gm_identity()->character_id);
    gm_reply("GM commands need a game master account.");
    return {};
  }
  if (!allow_gm_request()) {
    spdlog::warn("GM opcode {} refused for character {}: request rate limit", request.opcode,
                 runtime_.gm_identity()->character_id);
    gm_reply("Too many GM commands. Wait a second and try again.");
    return {};
  }

  // Debug overlays and the world clock (src/world/features/gm/gm_diagnostics.hpp).
  const auto diagnostic = gm::parse_diagnostic_command(request.opcode, request.message);
  if (diagnostic.matched) {
    return execute_gm_diagnostic(diagnostic);
  }

  // Combat units, quest acceptance and mail (src/world/features/gm/gm_world_commands.hpp).
  const auto world_command = gm::parse_world_command(request.opcode, request.message);
  if (world_command.matched) {
    return execute_gm_world_command(world_command);
  }

  // Movement, protection, visibility and NPC spawning (src/world/features/gm/gm_tools.hpp).
  const auto session_command = gm::parse_session_command(request.opcode, request.message);
  if (session_command.matched) {
    return execute_gm_session_command(session_command);
  }

  if (auto event = dispatch_.gm_dispatch_event_request(request)) {
    return std::move(*event);
  }
  if (request.opcode == kGmCommandTextRequestOpcode) {
    if (auto expression = dispatch_.gm_dispatch_expression_request(request)) {
      return std::move(*expression);
    }
  }
  if (auto housing = dispatch_.gm_dispatch_housing_request(request)) {
    return std::move(*housing);
  }

  const auto parsed = parse_gm_request(request.opcode, request.message);
  if (!parsed.error.empty()) {
    spdlog::info("GM opcode {} not executed for character {}: {}", request.opcode,
                 runtime_.gm_identity()->character_id, parsed.error);
    const auto unsupported = gm::unsupported_command_reason(request.opcode, request.message);
    gm_reply(unsupported.value_or(parsed.error + ". Type /gm help for the command list."));
    return {};
  }

  // Commands commit one at a time; a failure stops the rest of the request.
  std::vector<GameplayPacket> packets;
  for (const auto &command : parsed.commands) {
    const std::string command_name = gm_command_name(command.kind);
    if (is_progression_gm_command(command.kind)) {
      auto progression_packets = character_.gm_execute_progression(command);
      if (progression_packets.empty()) {
        gm_reply(command_name + " was refused (check the value; see the server log).");
        break;
      }
      gm_reply(command_name + " applied.");
      packets.insert(packets.end(), std::make_move_iterator(progression_packets.begin()),
                     std::make_move_iterator(progression_packets.end()));
      continue;
    }

    if (!character_.gm_shop_catalog()) {
      spdlog::warn("GM opcode {} refused: the item catalog is not loaded", request.opcode);
      break;
    }
    auto packet = execute_gm_command(command);
    if (!packet) {
      break;
    }
    gm_reply(command_name + " applied.");
    packets.push_back(std::move(*packet));
  }
  return packets;
}

std::optional<GameplayPacket> GmSession::execute_gm_command(const GmCommand &command) {
  const char *command_name = gm_command_name(command.kind);
  const auto character = character_.gm_character();
  if (!character) {
    spdlog::warn("GM {} refused: character {} is unavailable", command_name,
                 runtime_.gm_identity()->character_id);
    return std::nullopt;
  }

  // A tutorial session owns its in-memory progress; other sessions read storage.
  const auto before = character_.gm_progress();
  const auto plan = plan_gm_command(*character_.gm_shop_catalog(), command, *character, before);
  if (plan.error != x5::ec::Success) {
    spdlog::info("GM {} rejected for character {}: {}", command_name,
                 runtime_.gm_identity()->character_id, x5::ec::EErrorCode_Name(plan.error));
    gm_reply(std::string(command_name) + " refused: " + x5::ec::EErrorCode_Name(plan.error));
    return std::nullopt;
  }

  std::optional<std::vector<CharacterItem>> committed;
  try {
    committed =
        character_.gm_commit_item_trade(before, plan.trade.next, plan.trade.storage_changes());
  } catch (const std::exception &error) {
    spdlog::warn("GM {} commit rejected for character {}: {}", command_name,
                 runtime_.gm_identity()->character_id, error.what());
    return std::nullopt;
  }
  if (!committed) {
    // Revision, ownership or slot conflict: nothing was written.
    spdlog::warn("GM {} commit conflicted for character {}", command_name,
                 runtime_.gm_identity()->character_id);
    return std::nullopt;
  }

  if (character_.gm_has_tutorial()) {
    events_.publish(PlayerProgressCommitted{plan.trade.next});
  }
  character_.gm_refresh_traded_items(*character, plan.trade, *committed);
  spdlog::info("GM {} applied for character {}", command_name,
               runtime_.gm_identity()->character_id);
  return gm_command_packet(plan.trade, *committed);
}

std::vector<std::uint64_t> GmSession::gm_command_targets(const gm::UnitTarget &target) const {
  if (target.kind == gm::UnitTargetKind::self) {
    return {player_wire_unit_id(runtime_.gm_identity()->character_id)};
  }
  if (target.kind == gm::UnitTargetKind::spawned) {
    return gm_spawned_units_;
  }
  const auto own_id = player_wire_unit_id(runtime_.gm_identity()->character_id);
  if (target.unit_id == own_id ||
      (combat_.gm_has_combat() && combat_.gm_combat_visible_monster(target.unit_id)) ||
      std::ranges::find(gm_spawned_units_, target.unit_id) != gm_spawned_units_.end()) {
    return {target.unit_id};
  }
  return {};
}

std::vector<GameplayPacket>
GmSession::execute_gm_world_command(const gm::WorldCommandParse &parse) {
  if (!parse.command) {
    gm_reply("Usage: " + parse.error);
    return {};
  }
  const auto &command = *parse.command;
  std::optional<WorldPosition> position;
  {
    std::lock_guard lock(runtime_.gm_protocol_mutex());
    position = runtime_.gm_position_locked();
  }
  if (!position) {
    gm_reply("Enter the world before using this command.");
    return {};
  }

  if (command.kind == gm::WorldCommandKind::mail_send) {
    gm_send_mail(command);
    return {};
  }
  if (command.kind == gm::WorldCommandKind::accept_quest) {
    return gm_accept_quest(command, *position);
  }
  if (!combat_.gm_has_combat()) {
    gm_reply("Combat state is unavailable on this map.");
    return {};
  }
  if (command.kind == gm::WorldCommandKind::set_gm_stat ||
      command.kind == gm::WorldCommandKind::add_gm_stat) {
    return gm_change_stat(command);
  }
  if (command.kind == gm::WorldCommandKind::reset_cooldowns) {
    const auto cleared = combat_.gm_combat_reset_cooldowns();
    gm_reply("Cleared " + std::to_string(cleared) +
             " server cooldowns. Client cooldown indicators may remain until they expire.");
    return {};
  }
  if (command.kind == gm::WorldCommandKind::execute_skill_effect) {
    auto packets = combat_.gm_combat_execute_self_effect(command.cms_id, *position,
                                                         runtime_.gm_monotonic_ticks());
    {
      std::lock_guard lock(runtime_.gm_protocol_mutex());
      events_.publish(PlayerCombatSnapshotChanged{});
    }
    gm_reply(packets.empty() ? "Effect refused or unchanged: only supported self effects run."
                             : "Self effect applied.");
    return packets;
  }
  if (command.kind == gm::WorldCommandKind::combat_formula_log ||
      command.kind == gm::WorldCommandKind::stat_sync_warning) {
    gm_reply("Combat hit logging is enabled in the server log. No per-session formula or "
             "stat-sync warning toggle is implemented; use regen_stat for a stat snapshot.");
    return {};
  }

  const auto targets = gm_command_targets(command.target);
  if (targets.empty()) {
    gm_reply("No matching private target. Use me or an NPC created by your spawn_npc command.");
    return {};
  }
  const auto ticks = runtime_.gm_monotonic_ticks();
  std::vector<GameplayPacket> packets;
  for (const auto target : targets) {
    auto updates = gm_apply_unit_command(command, target, *position, ticks);
    packets.insert(packets.end(), std::make_move_iterator(updates.begin()),
                   std::make_move_iterator(updates.end()));
  }
  const bool applied = !packets.empty();
  auto combat_updates = combat_.gm_advance_combat(ticks, *position);
  packets.insert(packets.end(), std::make_move_iterator(combat_updates.begin()),
                 std::make_move_iterator(combat_updates.end()));
  gm_reply(std::string(gm::world_command_name(command.kind)) +
           (applied ? " applied to private session targets."
                    : ": no change (unknown, inactive or unsupported target)."));
  {
    std::lock_guard lock(runtime_.gm_protocol_mutex());
    events_.publish(PlayerCombatSnapshotChanged{});
  }
  return packets;
}

std::vector<GameplayPacket> GmSession::gm_apply_unit_command(const gm::WorldCommand &command,
                                                             std::uint64_t target,
                                                             const WorldPosition &position,
                                                             std::int64_t ticks) {
  switch (command.kind) {
  case gm::WorldCommandKind::monster_kill:
  case gm::WorldCommandKind::damage_monster: {
    const auto health = combat_.gm_combat_monster_health(target);
    if (!health) {
      return {};
    }
    std::int64_t amount = command.amount;
    if (command.kind == gm::WorldCommandKind::monster_kill) {
      amount = *health;
    } else if (command.percent) {
      // Emulator policy: a percentage of the target's current HP, rounded upward.
      amount = static_cast<std::int64_t>(
          std::ceil(static_cast<double>(*health) * *command.percent / 100.0));
    }
    return combat_.gm_combat_damage_monster(target, static_cast<int>(amount), ticks);
  }
  case gm::WorldCommandKind::add_status_effect:
    return combat_.gm_combat_add_status_effect(target, command.cms_id, ticks);
  case gm::WorldCommandKind::remove_status_effect:
    return combat_.gm_combat_remove_status_effect(target, command.cms_id, ticks);
  case gm::WorldCommandKind::add_abnormal_gauge: {
    std::vector<GameplayPacket> packets;
    for (const auto &[type, amount] : command.gauges) {
      auto changed = combat_.gm_combat_add_abnormal_gauge(target, type, amount, ticks);
      packets.insert(packets.end(), std::make_move_iterator(changed.begin()),
                     std::make_move_iterator(changed.end()));
    }
    return packets;
  }
  case gm::WorldCommandKind::cancel_skill:
    return combat_.gm_combat_cancel_skill(target, position, ticks);
  case gm::WorldCommandKind::npc_ai:
    return combat_.gm_combat_toggle_monster_ai(target, command.enabled, ticks);
  case gm::WorldCommandKind::move_unit:
    return gm_move_unit(command, target, position, ticks);
  default:
    return {};
  }
}

std::vector<GameplayPacket> GmSession::gm_move_unit(const gm::WorldCommand &command,
                                                    std::uint64_t target,
                                                    const WorldPosition &position,
                                                    std::int64_t ticks) {
  const bool self = target == player_wire_unit_id(runtime_.gm_identity()->character_id);
  const auto monster = combat_.gm_combat_monster_debug_view(target, ticks);
  if (!self && !monster) {
    return {};
  }
  const auto &current = self ? position : monster->position;
  double x = command.x_meters;
  double y = command.y_meters;
  double z = command.z_meters;
  if (command.relative) {
    x += current.x_mm / 1000.0;
    y += current.y_mm / 1000.0;
    z += current.z_mm / 1000.0;
  }
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
      std::abs(x) > gm::kMaximumCoordinateMeters || std::abs(y) > gm::kMaximumCoordinateMeters ||
      std::abs(z) > gm::kMaximumCoordinateMeters) {
    return {};
  }
  const auto destination = gm::absolute_destination(current.map_id, x, y, z, current.yaw);
  if (self) {
    return gm_teleport(destination, position, true);
  }
  return combat_.gm_combat_place_monster(target, destination, ticks);
}

std::vector<GameplayPacket> GmSession::gm_change_stat(const gm::WorldCommand &command) {
  const auto character = character_.gm_character();
  if (!character || !character_.gm_has_character_stats()) {
    gm_reply("Character stat definitions are unavailable.");
    return {};
  }
  if (command.clear) {
    const auto before = combat_.gm_combat_player_stats();
    combat_.gm_combat_update_player_stats(character_.gm_session_stats(*character));
    const auto after = combat_.gm_combat_player_stats();
    {
      std::lock_guard lock(runtime_.gm_protocol_mutex());
      events_.publish(PlayerCombatSnapshotChanged{});
    }
    gm_reply("GM stats restored from equipment and build; current vitals are preserved.");
    return {
        character_.gm_stat_notification(player_wire_unit_id(character->id), before, after, after)};
  }

  const auto key = std::to_string(command.cms_id);
  const auto before = combat_.gm_combat_player_stats();
  const int current = before.value(key, 0);
  std::int64_t value = command.amount;
  if (command.kind == gm::WorldCommandKind::add_gm_stat) {
    value += current;
    if (command.percent) {
      value += static_cast<std::int64_t>(static_cast<double>(current) * *command.percent / 100.0);
    }
  }
  if (value < 0 || value > gm::kMaximumGmStatValue ||
      ((command.cms_id == 18 || command.cms_id == 69) && value == 0)) {
    gm_reply("Stat result is outside the supported range; use suicide for a lethal change.");
    return {};
  }
  auto packets = combat_.gm_combat_set_player_stat(command.cms_id, static_cast<int>(value),
                                                   runtime_.gm_monotonic_ticks());
  {
    std::lock_guard lock(runtime_.gm_protocol_mutex());
    events_.publish(PlayerCombatSnapshotChanged{});
  }
  gm_reply(packets.empty()
               ? "Stat unchanged or refused; HP cannot bypass death/revival."
               : "Temporary stat change applied; equipment/build recalculation resets it.");
  return packets;
}

std::vector<GameplayPacket> GmSession::gm_accept_quest(const gm::WorldCommand &command,
                                                       const WorldPosition &position) {
  if (character_.gm_has_tutorial() || !quests_.gm_has_quest_book()) {
    gm_reply("Quest acceptance requires a shared map and a loaded quest catalog.");
    return {};
  }
  const auto character = character_.gm_character();
  if (!character) {
    return {};
  }
  QuestWorldView view;
  view.position = position;
  view.character = &*character;
  const auto before = character_.gm_stored_progress();
  auto result =
      quests_.gm_plan_quest_acceptance(command.cms_id, command.check_conditions, before, view);
  if (!result.next || before.revision == std::numeric_limits<std::int64_t>::max()) {
    gm_reply("Quest acceptance refused (unknown, already saved, conditions or reward limits).");
    return {};
  }
  ++result.next->revision;
  if (!character_.gm_save_progress(before, *result.next)) {
    gm_reply("Quest acceptance conflicted; reconnect and retry.");
    return {};
  }
  character_.gm_apply_level_change(*result.next, result.packets);
  gm_reply("Quest accepted.");
  return std::move(result.packets);
}

void GmSession::gm_send_mail(const gm::WorldCommand &command) {
  if (!mail_.gm_has_mail_storage() || !character_.gm_shop_catalog()) {
    gm_reply("Mail delivery requires storage and the item catalog.");
    return;
  }
  std::int64_t account_id = runtime_.gm_identity()->account_id;
  std::int64_t character_id = runtime_.gm_identity()->character_id;
  const bool other = !command.recipient_name.empty() || command.recipient_character_id != 0;
  if (other) {
    const auto target =
        world_.gm_find_target(command.recipient_name, command.recipient_character_id);
    if (!target) {
      gm_reply("Mail target must be an online character. Use the operator API for offline mail.");
      return;
    }
    account_id = target->summary.account_id;
    character_id = target->summary.character_id;
  }
  if (command.recipient_account_id != 0 && command.recipient_account_id != account_id) {
    gm_reply("Mail account does not own the selected recipient.");
    return;
  }
  nlohmann::json attachments = nlohmann::json::array();
  if (command.attachment == gm::MailAttachmentChoice::item) {
    attachments.push_back(
        {{"kind", "item"}, {"cms_id", command.cms_id}, {"count", command.amount}});
  } else if (command.attachment == gm::MailAttachmentChoice::gold) {
    attachments.push_back({{"kind", "money"}, {"cms_id", 0}, {"count", command.amount}});
  }
  const auto now = runtime_.gm_epoch_ticks();
  const nlohmann::json request{
      {"account_id", account_id},
      {"character_id", character_id},
      {"world_id", runtime_.gm_identity()->world_id},
      {"delivery_key", "gm-" + random_hex(16)},
      {"title", command.mail_title.empty() ? "Game master delivery" : command.mail_title},
      {"text", command.mail_text},
      {"retention_seconds", 30 * 24 * 60 * 60},
      {"attachments", std::move(attachments)}};
  MailRecord mail;
  try {
    mail = parse_mail_delivery(request, now);
    validate_mail_catalog(mail, *character_.gm_shop_catalog());
  } catch (const std::invalid_argument &) {
    gm_reply("Mail refused: invalid attachment, text or retention.");
    return;
  }

  std::optional<MailRecord> delivered;
  try {
    delivered = mail_.gm_deliver_mail_record(mail);
  } catch (const std::exception &) {
    spdlog::error("GM mail delivery failed; check the recipient mailbox before retrying");
    gm_reply("Delivery result is uncertain. Check the mailbox before sending another command.");
    return;
  }
  if (!delivered) {
    gm_reply("Mail refused: recipient unavailable or mailbox full.");
    return;
  }

  // Notification failure must never describe an already durable delivery as refused.
  try {
    const auto target = world_.gm_find_target({}, character_id);
    if (target) {
      mail_.gm_notify_mail_peer(*delivered, target->session);
    }
  } catch (const std::exception &) {
    spdlog::warn("GM mail was saved but its live notification could not be queued");
  }
  gm_reply("Mail delivered. Each command creates a new delivery; retry only if refused.");
}

void GmSession::gm_reply(const std::string &text) {
  const auto identity = runtime_.gm_identity();
  if (!identity) {
    return;
  }
  const auto chat = runtime_.gm_chat_identity();
  if (!chat) {
    return;
  }
  const auto handler = feature_.reply_handler();
  if (!handler) {
    return;
  }
  try {
    handler(*chat, text);
  } catch (const std::exception &error) {
    spdlog::warn("GM reply for character {} was not delivered: {}", identity->character_id,
                 error.what());
  }
}

const char *GmSession::on_off(bool enabled) {
  return enabled ? "ON" : "OFF";
}

void GmSession::sync_gm_combat_protection() {
  if (!combat_.gm_has_combat()) {
    return;
  }
  events_.publish(PlayerProtectionChanged{gm_god_ || gm_free_movement_, gm_invisible_.load(),
                                          gm_infinite_stamina_ || gm_free_movement_});
}

std::vector<GameplayPacket> GmSession::execute_gm_session_command(const gm::SessionParse &parse) {
  if (!parse.command) {
    gm_reply("Usage: " + parse.error);
    return {};
  }

  const auto &command = *parse.command;
  std::optional<WorldPosition> position;
  {
    std::lock_guard lock(runtime_.gm_protocol_mutex());
    position = runtime_.gm_position_locked();
  }
  spdlog::info("GM {} requested by character {}", gm::session_command_name(command.kind),
               runtime_.gm_identity()->character_id);

  switch (command.kind) {
  case gm::SessionCommandKind::help:
    for (const auto &line : gm::help_lines(command.help_page)) {
      gm_reply(line);
    }
    // The event commands have their own parser (src/world/features/events/event_gm_commands.hpp).
    if (command.help_page == gm::kHelpPageCount) {
      for (const auto &line : dispatch_.gm_event_help_lines()) {
        gm_reply(line);
      }
    }
    return {};
  case gm::SessionCommandKind::position:
    gm_reply(position ? gm::describe_position(*position)
                      : std::string("Your position is not known yet."));
    return {};
  case gm::SessionCommandKind::status:
    gm_reply(std::string("god ") + on_off(gm_god_) + ", invisible " + on_off(gm_invisible_.load()) +
             ", stamina " + on_off(gm_infinite_stamina_) + ", fly " + on_off(gm_free_movement_) +
             ", spawned NPCs " + std::to_string(gm_spawned_units_.size()));
    return {};
  case gm::SessionCommandKind::list_players:
    gm_list_players();
    return {};
  case gm::SessionCommandKind::teleport:
    if (!position) {
      gm_reply("Enter the world before teleporting.");
      return {};
    }
    return gm_teleport(gm::absolute_destination(coordinate_map_id(*position), command.x_meters,
                                                command.y_meters, command.z_meters,
                                                command.yaw.value_or(position->yaw)),
                       position, true);
  case gm::SessionCommandKind::teleport_relative: {
    if (!position) {
      gm_reply("Enter the world before teleporting.");
      return {};
    }
    auto destination =
        gm::relative_destination(*position, command.direction, command.z_meters);
    if (!destination) {
      gm_reply("That destination is outside the map coordinate range.");
      return {};
    }
    destination->map_id = coordinate_map_id(*position);
    return gm_teleport(*destination, position, true);
  }
  case gm::SessionCommandKind::teleport_map:
    return gm_teleport_to_map(command, position);
  case gm::SessionCommandKind::go_to_player:
    return gm_go_to_player(command.player_name, position, command.character_id);
  case gm::SessionCommandKind::summon_player:
    gm_summon_player(command.player_name, position);
    return {};
  case gm::SessionCommandKind::god_mode:
    gm_god_ = command.enabled.value_or(!gm_god_);
    sync_gm_combat_protection();
    gm_reply(std::string("God mode ") + on_off(gm_god_) + ".");
    return {gm_invincible_attribute(gm_god_ || gm_free_movement_)};
  case gm::SessionCommandKind::invisible:
    gm_set_invisible(command.enabled.value_or(!gm_invisible_.load()));
    return {};
  case gm::SessionCommandKind::infinite_stamina:
    gm_infinite_stamina_ = command.enabled.value_or(!gm_infinite_stamina_);
    sync_gm_combat_protection();
    gm_reply(std::string("Infinite stamina ") + on_off(gm_infinite_stamina_) + ".");
    return gm_infinite_stamina_ ? gm_restore_vitals() : std::vector<GameplayPacket>{};
  case gm::SessionCommandKind::free_movement:
    gm_free_movement_ = command.enabled.value_or(!gm_free_movement_);
    sync_gm_combat_protection();
    if (gm_free_movement_) {
      gm_reply("Fly ON: no damage, no fall damage, infinite stamina. The game client keeps its "
               "own walls and gravity; use up/down/fwd <meters> or tp to pass through them.");
    } else {
      gm_reply("Fly OFF.");
    }
    return {gm_invincible_attribute(gm_god_ || gm_free_movement_)};
  case gm::SessionCommandKind::heal: {
    auto packets = gm_restore_vitals();
    gm_reply(combat_.gm_has_combat() ? "HP and stamina restored." : "Healing is unavailable here.");
    return packets;
  }
  case gm::SessionCommandKind::revive: {
    auto packets = combat_.gm_revive_player();
    gm_reply(packets.empty() ? "You are not dead." : "Revived.");
    return packets;
  }
  case gm::SessionCommandKind::spawn_npc:
    return gm_spawn_npcs(command.npc_cms_id, command.count, position);
  case gm::SessionCommandKind::despawn_npcs:
    return gm_despawn_npcs();
  case gm::SessionCommandKind::reveal_map:
    gm_reveal_map();
    return {};
  case gm::SessionCommandKind::complete_quests:
  case gm::SessionCommandKind::complete_quest:
    return gm_complete_quests(command, position);
  case gm::SessionCommandKind::suicide:
    return gm_suicide(position);
  case gm::SessionCommandKind::kick:
    gm_kick_self(command.delay_seconds);
    return {};
  case gm::SessionCommandKind::announce:
    gm_announce(command.announcement);
    return {};
  }
  return {};
}

std::vector<GameplayPacket> GmSession::gm_suicide(const std::optional<WorldPosition> &position) {
  if (!combat_.gm_has_combat() || !position) {
    gm_reply("Suicide is unavailable here (no combat state or position).");
    return {};
  }
  if (combat_.gm_combat_player_dead()) {
    gm_reply("You are already dead.");
    return {};
  }

  auto packets = combat_.gm_combat_kill_player(*position, runtime_.gm_monotonic_ticks());
  // Commits the death and refreshes the authoritative stats before anything is published.
  combat_.gm_persist_player_death(packets);
  gm_reply("You died. Use revive or the resurrection window.");
  return packets;
}

void GmSession::gm_kick_self(int delay_seconds) {
  gm_reply("Disconnecting in " + std::to_string(delay_seconds) + " seconds.");
  auto timer = std::make_shared<boost::asio::steady_timer>(runtime_.gm_io_executor(),
                                                           std::chrono::seconds(delay_seconds));
  timer->async_wait([this, owner = runtime_.retain_gm_owner(),
                     timer](const boost::system::error_code &error) {
    if (error) {
      return;
    }
    spdlog::info("Native world connection={} closed by GM test_kick", runtime_.gm_connection_id());
    runtime_.gm_close_on_io();
  });
}

void GmSession::gm_announce(const std::string &announcement) {
  const auto identity = runtime_.gm_identity();
  if (!identity) {
    return;
  }
  const auto chat = runtime_.gm_chat_identity();
  if (!chat) {
    return;
  }
  const auto handler = feature_.announce_handler();
  if (!handler) {
    gm_reply("Announcements are unavailable on this server.");
    return;
  }
  try {
    const auto recipients = handler(*chat, announcement);
    gm_reply("Announcement sent to " + std::to_string(recipients) + " characters.");
  } catch (const std::exception &error) {
    spdlog::warn("GM announcement from character {} was refused: {}", identity->character_id,
                 error.what());
    gm_reply("The announcement was refused (check its characters and length).");
  }
}

std::vector<GameplayPacket> GmSession::execute_gm_diagnostic(const gm::DiagnosticParse &parse) {
  if (!parse.command) {
    gm_reply("Usage: " + parse.error);
    return {};
  }

  const auto &command = *parse.command;
  std::optional<WorldPosition> position;
  {
    std::lock_guard lock(runtime_.gm_protocol_mutex());
    position = runtime_.gm_position_locked();
  }
  spdlog::info("GM {} requested by character {}", gm::diagnostic_name(command.kind),
               runtime_.gm_identity()->character_id);

  switch (command.kind) {
  case gm::DiagnosticKind::game_time:
    return gm_game_time(command.game_time);
  case gm::DiagnosticKind::regen_stat:
    return gm_regen_stat();
  case gm::DiagnosticKind::channel_event_info:
    return dispatch_.gm_channel_event_info(position);
  case gm::DiagnosticKind::npc_field:
    gm_reply("debug_npc_field is unavailable: this server keeps no NPC navigation field.");
    return {};
  case gm::DiagnosticKind::watch:
    return gm_watch(command, position);
  case gm::DiagnosticKind::unwatch:
    gm_unwatch(command);
    return {};
  }
  return {};
}

std::string GmSession::clock_text(double hours) {
  constexpr int kMinutesPerHour = 60;
  const auto total_minutes = static_cast<int>(std::lround(hours * kMinutesPerHour));
  const int hour = total_minutes / kMinutesPerHour;
  const int minute = total_minutes % kMinutesPerHour;
  return (hour < 10 ? "0" : "") + std::to_string(hour) + ":" + (minute < 10 ? "0" : "") +
         std::to_string(minute);
}

std::vector<GameplayPacket> GmSession::gm_game_time(const gm::GameTimeSetting &setting) {
  const auto now_seconds = runtime_.gm_epoch_ticks() / kEpochTicksPerSecond;
  const std::string mode = setting.stopped ? "stopped" : "running";
  gm_reply("Game clock " + mode + " at " + clock_text(setting.hour) + " (one game day = " +
           std::to_string(setting.day_seconds) + " s). Only your client is changed.");
  return {{gm::kWorldGameTimeNotification, gm::world_game_time_message(setting, now_seconds)}};
}

std::vector<GameplayPacket> GmSession::gm_regen_stat() {
  if (!combat_.gm_has_combat()) {
    gm_reply("Regen stats are unavailable here (no combat state).");
    return {};
  }
  gm_reply("Regen stats sent to the debug panel.");
  return {{gm::kDebugPlayerRegenStatNotification,
           gm::player_regen_stat_message(combat_.gm_combat_player_stats(),
                                         runtime_.gm_monotonic_ticks())}};
}

std::optional<GmWatchedUnit>
GmSession::watched_unit(std::uint64_t unit_id, const std::optional<WorldPosition> &own_position,
                        std::int64_t ticks) const {
  if (runtime_.gm_identity() &&
      unit_id == player_wire_unit_id(runtime_.gm_identity()->character_id)) {
    if (!own_position) {
      return std::nullopt;
    }
    return GmWatchedUnit{*own_position, std::nullopt, std::nullopt};
  }

  if (combat_.gm_has_combat()) {
    if (const auto view = combat_.gm_combat_monster_debug_view(unit_id, ticks)) {
      return GmWatchedUnit{view->position, view->ai_state, view->route_end};
    }
  }

  const auto *announced = world_.gm_find_announced_unit(unit_id);
  if (!announced) {
    return std::nullopt;
  }
  // Announcements carry no yaw for this purpose; the overlay draws the point only.
  const WorldPosition position{announced->map_id, announced->position.x_mm,
                               announced->position.y_mm, announced->position.z_mm, 0.0F};
  return GmWatchedUnit{position, std::nullopt, std::nullopt};
}

std::vector<GameplayPacket>
GmSession::debug_watch_packets(const std::optional<WorldPosition> &own_position,
                               std::int64_t ticks) {
  std::vector<GameplayPacket> packets;
  std::vector<std::uint64_t> vanished_units;
  for (const auto &watch : gm_watches_.entries()) {
    const auto unit = watched_unit(watch.unit_id, own_position, ticks);
    if (!unit) {
      vanished_units.push_back(watch.unit_id);
      continue;
    }

    switch (watch.type) {
    case gm::WatchType::position:
      packets.push_back({gm::kDebugPositionNotification,
                         gm::debug_position_message(watch.unit_id, unit->position)});
      break;
    case gm::WatchType::path:
      packets.push_back({gm::kDebugPathNotification,
                         gm::debug_path_message(watch.unit_id, unit->position, unit->route_end)});
      break;
    case gm::WatchType::ai:
      if (unit->ai_state) {
        packets.push_back(
            {gm::kDebugAiNotification, gm::debug_ai_message(watch.unit_id, *unit->ai_state)});
      }
      break;
    case gm::WatchType::abnormal_status:
    case gm::WatchType::skill_collision:
      break;
    }
  }

  for (const auto unit_id : vanished_units) {
    gm_watches_.remove_unit(unit_id);
  }
  return packets;
}

std::vector<GameplayPacket> GmSession::gm_watch(const gm::DiagnosticCommand &command,
                                                const std::optional<WorldPosition> &position) {
  std::vector<std::uint64_t> units;
  switch (command.target) {
  case gm::WatchTarget::unit:
    units.push_back(command.unit_id);
    break;
  case gm::WatchTarget::self:
    units.push_back(player_wire_unit_id(runtime_.gm_identity()->character_id));
    break;
  case gm::WatchTarget::spawned:
    units = gm_spawned_units_;
    break;
  }
  if (units.empty()) {
    gm_reply("You have no spawned NPCs to watch.");
    return {};
  }

  const auto ticks = runtime_.gm_monotonic_ticks();
  int added = 0;
  int unknown_units = 0;
  bool unsupported_type = false;
  bool limit_reached = false;
  for (const auto unit_id : units) {
    if (!watched_unit(unit_id, position, ticks)) {
      ++unknown_units;
      continue;
    }
    for (const auto type : command.watch_types) {
      switch (gm_watches_.add(unit_id, type)) {
      case gm::WatchAddResult::added:
        ++added;
        break;
      case gm::WatchAddResult::already_watched:
        break;
      case gm::WatchAddResult::unsupported_type:
        unsupported_type = true;
        break;
      case gm::WatchAddResult::limit_reached:
        limit_reached = true;
        break;
      }
    }
  }

  std::string reply = "Watching " + std::to_string(gm_watches_.entries().size()) + " overlays (" +
                      std::to_string(added) + " added).";
  if (unknown_units > 0) {
    reply += " " + std::to_string(unknown_units) + " unit(s) are not on your map.";
  }
  if (unsupported_type) {
    reply += " Abnormal-status and skill-collision overlays are not simulated.";
  }
  if (limit_reached) {
    reply += " Limit of " + std::to_string(gm::kMaximumWatchesPerSession) + " reached.";
  }
  gm_reply(reply);
  if (added == 0) {
    return {};
  }

  // An immediate snapshot; the gameplay timer refreshes it while combat state exists.
  auto packets = debug_watch_packets(position, ticks);
  gm_watches_.mark_emitted(ticks);
  return packets;
}

void GmSession::gm_unwatch(const gm::DiagnosticCommand &command) {
  std::size_t removed = 0;
  if (command.unit_id == 0) {
    removed = gm_watches_.entries().size();
    gm_watches_.clear();
  } else if (command.watch_types.empty()) {
    removed = gm_watches_.remove_unit(command.unit_id);
  } else {
    for (const auto type : command.watch_types) {
      if (gm_watches_.remove(command.unit_id, type)) {
        ++removed;
      }
    }
  }
  gm_reply("Stopped " + std::to_string(removed) + " overlays.");
}

std::vector<GameplayPacket> GmSession::gm_restore_vitals() {
  if (!combat_.gm_has_combat()) {
    return {};
  }
  auto packets = combat_.gm_combat_restore_vitals(runtime_.gm_monotonic_ticks());
  std::lock_guard lock(runtime_.gm_protocol_mutex());
  events_.publish(PlayerCombatSnapshotChanged{});
  return packets;
}

void GmSession::gm_set_invisible(bool invisible) {
  gm_invisible_.store(invisible);
  events_.publish(PlayerVisibilityChanged{runtime_.gm_connection_id(), invisible});
  sync_gm_combat_protection();
  gm_reply(std::string("Invisible ") + on_off(invisible) +
           (invisible ? ". Other players and monsters cannot see you." : "."));
}

int GmSession::coordinate_map_id(const WorldPosition &current) const {
  // Housing areas share their field's coordinates (live 2026-11-02: `tp` typed inside map
  // 20101 loaded an empty housing copy at the harbor instead of the harbor on map 9012).
  return world_.gm_housing_area_parent(current.map_id).value_or(current.map_id);
}

std::vector<GameplayPacket> GmSession::gm_teleport(const WorldPosition &requested,
                                                   const std::optional<WorldPosition> &current,
                                                   bool announce, TeleportChannel channel) {
  if (!current) {
    if (announce) {
      gm_reply("Enter the world before teleporting.");
    }
    return {};
  }

  // An open-world destination belongs to its region's public channel, so the client loads
  // that region's NPCs, monsters and services instead of staying on the departure channel.
  auto destination = requested;
  const bool keep_named_channel = channel == TeleportChannel::requested_map;
  if (const auto region_map = world_.gm_region_map(requested, keep_named_channel)) {
    destination.map_id = *region_map;
  }

  if (current->map_id != destination.map_id || world_.gm_in_housing()) {
    gm_change_map(destination, announce);
    return {};
  }

  auto outcome = world_.gm_request_server_teleport(destination, runtime_.gm_monotonic_ticks());
  if (!outcome) {
    if (announce) {
      gm_reply("A teleport is already in progress.");
    }
    return {};
  }
  if (announce) {
    gm_reply("Teleporting to " + gm::describe_position(destination));
  }
  return std::move(*outcome);
}

std::vector<GameplayPacket>
GmSession::gm_teleport_to_map(const gm::SessionCommand &command,
                              const std::optional<WorldPosition> &position) {
  if (!position) {
    gm_reply("Enter the world before teleporting.");
    return {};
  }

  const auto &catalog = world_.gm_client_catalog();
  if (catalog && catalog->has_table("Map") &&
      !catalog->find_row("Map", "id", std::to_string(command.map_id))) {
    gm_reply("Map " + std::to_string(command.map_id) + " is not in the client Map table.");
    return {};
  }

  // Without coordinates the current position is kept: the ArcheWorld channels share them.
  // The named map is kept as well, even when another region's area holds that position.
  auto destination = *position;
  destination.map_id = command.map_id;
  if (!command.has_coordinates) {
    return gm_teleport(destination, position, true, TeleportChannel::requested_map);
  }
  destination = gm::absolute_destination(command.map_id, command.x_meters, command.y_meters,
                                         command.z_meters, position->yaw);
  return gm_teleport(destination, position, true);
}

void GmSession::gm_change_map(const WorldPosition &destination, bool announce) {
  const bool to_tutorial = destination.map_id == kDiagnosticTutorialMapId;
  if (to_tutorial && !world_.gm_tutorial_data_available()) {
    if (announce) {
      gm_reply("The tutorial map is not loaded on this server.");
    }
    return;
  }
  if (!world_.gm_begin_map_move(destination)) {
    if (announce) {
      gm_reply("The map change was refused (see the server log).");
    }
    return;
  }

  gm_spawned_units_.clear();
  gm_watches_.clear();
  world_.gm_stage_map_move(destination, to_tutorial);
  if (announce) {
    gm_reply("Loading " + gm::describe_position(destination));
  }
}

void GmSession::gm_list_players() {
  const auto players = world_.gm_online_players();
  gm_reply("Online characters: " + std::to_string(players.lines.size()));
  for (const auto &line : players.lines) {
    gm_reply(line);
  }
}

std::vector<GameplayPacket> GmSession::gm_go_to_player(const std::string &name,
                                                       const std::optional<WorldPosition> &position,
                                                       std::int64_t character_id) {
  const auto target = world_.gm_find_target(name, character_id);
  if (!target) {
    gm_reply("No online character named " + name + ".");
    return {};
  }
  if (target->summary.character_id == runtime_.gm_identity()->character_id) {
    gm_reply("That is you.");
    return {};
  }
  if (target->summary.position->map_id == kDiagnosticTutorialMapId) {
    gm_reply(name + " is in a private tutorial instance.");
    return {};
  }
  return gm_teleport(gm::beside(*target->summary.position), position, true);
}

void GmSession::gm_summon_player(const std::string &name,
                                 const std::optional<WorldPosition> &position) {
  if (!position || !runtime_.gm_chat_identity()) {
    gm_reply("Enter the world before summoning.");
    return;
  }
  if (position->map_id == kDiagnosticTutorialMapId) {
    gm_reply("Players cannot be summoned into your private tutorial instance.");
    return;
  }

  const auto target = world_.gm_find_target(name);
  if (!target) {
    gm_reply("No online character named " + name + ".");
    return;
  }
  if (target->summary.character_id == runtime_.gm_identity()->character_id) {
    gm_reply("That is you.");
    return;
  }

  target->session->post_gm_summon(gm::beside(*position), runtime_.gm_chat_identity()->name);
  gm_reply("Summoning " + target->summary.character_name + ".");
}

void GmSession::post_gm_summon(const WorldPosition &destination, std::string summoner) {
  boost::asio::post(runtime_.gm_worker_executor(), [this, owner = runtime_.retain_gm_owner(),
                                                    destination, summoner = std::move(summoner)] {
    accept_gm_summon(destination, summoner);
  });
}

void GmSession::accept_gm_summon(const WorldPosition &destination, const std::string &summoner) {
  if (runtime_.gm_closed() || !runtime_.gm_identity()) {
    return;
  }

  std::optional<WorldPosition> current;
  {
    std::lock_guard lock(runtime_.gm_protocol_mutex());
    current = runtime_.gm_position_locked();
  }
  const auto packets = gm_teleport(destination, current, false);

  std::vector<Frame> frames;
  {
    std::lock_guard lock(runtime_.gm_protocol_mutex());
    for (const auto &packet : packets) {
      frames.push_back(runtime_.gm_encode_packet_locked(packet.opcode, packet.message));
    }
  }
  for (auto &frame : frames) {
    post_frame(std::move(frame));
  }
  gm_reply("You were summoned by " + summoner + ".");
}

std::string GmSession::npc_faction(int npc_cms_id) const {
  // Hostile monsters use Bandit; a catalog value is accepted only if it is a known enum name.
  constexpr const char *kDefaultSpawnFaction = "EX5Faction_Bandit";
  const auto &catalog = world_.gm_client_catalog();
  if (!catalog || !catalog->has_table("Npc")) {
    return kDefaultSpawnFaction;
  }
  const auto row = catalog->find_row("Npc", "id", std::to_string(npc_cms_id));
  if (!row) {
    return kDefaultSpawnFaction;
  }
  const auto faction = "EX5Faction_" + row->value("faction", std::string());
  x5::cms::EX5Faction parsed{};
  if (!x5::cms::EX5Faction_Parse(faction, &parsed)) {
    return kDefaultSpawnFaction;
  }
  return faction;
}

std::vector<GameplayPacket> GmSession::gm_spawn_npcs(int npc_cms_id, int count,
                                                     const std::optional<WorldPosition> &position) {
  if (!position) {
    gm_reply("Enter the world before spawning.");
    return {};
  }
  const auto &catalog = world_.gm_client_catalog();
  if (catalog && catalog->has_table("Npc") &&
      !catalog->find_row("Npc", "id", std::to_string(npc_cms_id))) {
    gm_reply("NPC " + std::to_string(npc_cms_id) + " is not in the client Npc table.");
    return {};
  }
  if (gm_spawned_units_.size() + static_cast<std::size_t>(count) >
      gm::kMaximumSpawnedNpcsPerSession) {
    gm_reply("Spawn limit reached (" + std::to_string(gm::kMaximumSpawnedNpcsPerSession) +
             "). Use despawn_npcs first.");
    return {};
  }

  const auto faction = npc_faction(npc_cms_id);
  std::vector<GameplayPacket> packets;
  int fightable = 0;
  for (const auto &spawn : gm::spawn_positions(*position, count)) {
    const auto unit_id =
        gm::spawned_npc_unit_id(runtime_.gm_identity()->character_id, gm_spawn_sequence_++);
    AnnouncedUnit announced;
    announced.unit_id = unit_id;
    announced.npc_template_id = npc_cms_id;
    announced.map_id = position->map_id;
    announced.instance_key = world_.gm_instance_key();
    announced.position = {spawn.x_mm, spawn.y_mm, spawn.z_mm};
    if (world_.gm_announce_npc(std::move(announced)) != UnitAnnounceResult::Added) {
      continue;
    }

    GameplayPacket packet{kUnitAddNotification,
                          gm::spawned_npc_message(unit_id, npc_cms_id, spawn, faction)};
    if (combat_.gm_has_combat() && combat_.gm_combat_add_monster(unit_id, npc_cms_id, spawn)) {
      combat_.gm_combat_decorate_spawn(packet);
      ++fightable;
    }
    gm_spawned_units_.push_back(unit_id);
    packets.push_back(std::move(packet));
  }

  std::string reply =
      "Spawned " + std::to_string(packets.size()) + " x NPC " + std::to_string(npc_cms_id) + ".";
  if (fightable > 0) {
    reply += " " + std::to_string(fightable) + " can be fought (they fight back when hit).";
  } else {
    reply += " Visual only: no complete attack/locomotion profile is loaded for this NPC.";
  }
  gm_reply(reply);
  if (fightable > 0) {
    // The UnitAdd must precede aggro/movement. First advance also arms the AI deadline.
    auto started = combat_.gm_advance_combat(runtime_.gm_monotonic_ticks(), *position);
    packets.insert(packets.end(), std::make_move_iterator(started.begin()),
                   std::make_move_iterator(started.end()));
  }
  return packets;
}

void GmSession::gm_reveal_map() {
  if (!world_.gm_client_catalog() || !world_.gm_client_catalog()->has_table("FogArea")) {
    gm_reply("Map reveal unavailable: the verified FogArea catalog is required.");
    return;
  }
  if (!world_.gm_has_fog_storage()) {
    gm_reply("Map reveal unavailable: exploration storage is not configured.");
    return;
  }

  try {
    const auto count = world_.gm_reveal_fog();
    if (!count) {
      gm_reply("Map reveal refused: GM character ownership could not be verified.");
      return;
    }
    gm_reply("Saved all " + std::to_string(*count) +
             " map fog areas. Return to character selection and select this character "
             "again to reload the map. Adventure Progress and zone access are unchanged.");
  } catch (const std::exception &) {
    // Storage exceptions may contain private connection details; keep diagnostics bounded.
    spdlog::error("GM map reveal failed while validating or saving exploration");
    gm_reply("Map reveal failed. Check the server catalog/storage configuration and retry.");
  }
}

std::vector<GameplayPacket>
GmSession::gm_complete_quests(const gm::SessionCommand &command,
                              const std::optional<WorldPosition> &position) {
  if (character_.gm_has_tutorial()) {
    gm_reply("Tutorial quests follow the tutorial's own story and cutscenes. Leave the "
             "tutorial first (for example /gm tp_map 9012), then complete quests.");
    return {};
  }
  if (!quests_.gm_has_quest_book() || !position) {
    gm_reply("Quest completion is unavailable here (no quest catalog or position).");
    return {};
  }
  const auto character = character_.gm_character();
  if (!character) {
    gm_reply("Your character could not be loaded.");
    return {};
  }

  QuestWorldView view;
  view.position = *position;
  view.character = &*character;
  view.find_unit = [this](std::uint64_t unit_id) {
    return quests_.gm_interaction_npc(unit_id);
  };

  const bool single = command.kind == gm::SessionCommandKind::complete_quest;
  const int rounds = command.chain ? gm::kMaximumQuestCompletionRounds : 1;
  std::vector<GameplayPacket> packets;
  int completed = 0;
  for (int round = 0; round < rounds; ++round) {
    const auto current = character_.gm_stored_progress();
    const auto quest_ids =
        single ? std::vector<int>{command.quest_id} : WorldQuestBook::active_quest_ids(current);
    if (quest_ids.empty()) {
      break;
    }

    auto completion = quests_.gm_plan_quest_completion(quest_ids, current, view);
    if (!completion.result.next ||
        completion.result.next->revision == std::numeric_limits<std::int64_t>::max()) {
      break;
    }
    auto next = std::move(*completion.result.next);
    ++next.revision;
    if (!character_.gm_save_progress(current, next)) {
      gm_reply("Quest completion conflicted with another change; try again.");
      break;
    }

    completed += completion.completed;
    packets.insert(packets.end(), std::make_move_iterator(completion.result.packets.begin()),
                   std::make_move_iterator(completion.result.packets.end()));
    if (single || completion.completed == 0) {
      break;
    }
  }

  const auto remaining = WorldQuestBook::active_quest_ids(character_.gm_stored_progress());
  if (single) {
    gm_reply(completed > 0 ? "Quest " + std::to_string(command.quest_id) + " completed."
                           : "Quest " + std::to_string(command.quest_id) +
                                 " was not completed (unknown, already complete, or its "
                                 "rewards could not be granted).");
  } else {
    std::string reply = "Completed " + std::to_string(completed) + " quests.";
    if (!remaining.empty()) {
      reply += " " + std::to_string(remaining.size()) + " still in progress" +
               (command.chain ? "." : " (use complete_quests chain for follow-up quests).");
    }
    gm_reply(reply);
  }
  spdlog::info("GM complete_quests for character {}: completed={} remaining={}",
               runtime_.gm_identity()->character_id, completed, remaining.size());
  return packets;
}

std::vector<GameplayPacket> GmSession::gm_despawn_npcs() {
  if (gm_spawned_units_.empty()) {
    gm_reply("You have no spawned NPCs.");
    return {};
  }

  std::vector<GameplayPacket> packets;
  const auto ticks = runtime_.gm_monotonic_ticks();
  for (const auto unit_id : gm_spawned_units_) {
    world_.gm_despawn_unit(unit_id);
    if (combat_.gm_has_combat()) {
      auto cleanup = combat_.gm_combat_remove_monster(unit_id, ticks);
      packets.insert(packets.end(), std::make_move_iterator(cleanup.begin()),
                     std::make_move_iterator(cleanup.end()));
    }
  }
  packets.push_back({kUnitRemoveNotification, gm::removed_units_message(gm_spawned_units_)});
  gm_reply("Removed " + std::to_string(gm_spawned_units_.size()) + " spawned NPCs.");
  gm_spawned_units_.clear();
  return packets;
}

bool GmSession::handle_social_gm(FrameContext &frame) {
  const auto *gameplay_request = frame.request();
  auto &session_packets = frame.session_packets;

  if (gameplay_request && is_gm_command_opcode(gameplay_request->opcode)) {
    const auto social =
        parse_social_gm_request(gameplay_request->opcode, gameplay_request->message);
    if (social) {
      session_packets = social_gm_command(*gameplay_request, *social);
      return true;
    }
  }

  return false;
}

bool GmSession::handle_generic_gm(FrameContext &frame) {
  const auto *gameplay_request = frame.request();
  auto &session_packets = frame.session_packets;
  auto &action_deadline = frame.action_deadline;

  if (gameplay_request && is_gm_command_opcode(gameplay_request->opcode)) {
    session_packets = gm_command(*gameplay_request);
    action_deadline = runtime_.gm_next_gameplay_tick();
    return true;
  }

  return false;
}

} // namespace chronicles::gm
