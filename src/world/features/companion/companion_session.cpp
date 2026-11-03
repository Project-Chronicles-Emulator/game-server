#include "world/features/companion/companion_session.hpp"

#include "cms.generated.pb.h"
#include "world/features/npc/npc_population.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <string>

namespace chronicles::companion {
namespace {

using Json = nlohmann::json;
using x5::ec::EErrorCode;

using world_request::kGmCommandRequest;
using world_request::kGmCommandTextRequest;
constexpr std::size_t kMaximumGmCommands = 8;

std::optional<std::uint64_t> decimal_id(const Json &message, const char *field) {
  const auto found = message.find(field);
  if (found == message.end()) {
    return std::nullopt;
  }
  if (found->is_number_unsigned()) {
    return found->get<std::uint64_t>();
  }
  if (!found->is_string()) {
    return std::nullopt;
  }

  return parse_unit_id_text(found->get_ref<const std::string &>());
}

std::optional<int> positive_int(const Json &message, const char *field) {
  const auto found = message.find(field);
  if (found == message.end() || !found->is_number_integer()) {
    return std::nullopt;
  }
  const auto value = found->get<std::int64_t>();
  if (value <= 0 || value > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  return static_cast<int>(value);
}

int non_negative_int(const Json &message, const char *field) {
  const auto found = message.find(field);
  if (found == message.end() || !found->is_number_integer()) {
    return 0;
  }
  const auto value = found->get<std::int64_t>();
  return value < 0 || value > std::numeric_limits<int>::max() ? -1 : static_cast<int>(value);
}

std::optional<WorldPosition> spawn_point(const Json &message, const char *field) {
  const auto point = message.find(field);
  if (point == message.end() || !point->is_object() || !point->contains("pos")) {
    return std::nullopt;
  }

  const auto &pos = point->at("pos");
  if (!pos.is_object()) {
    return std::nullopt;
  }
  WorldPosition position;
  position.x_mm = pos.value("millimeter_x", 0);
  position.y_mm = pos.value("millimeter_y", 0);
  position.z_mm = pos.value("millimeter_z", 0);
  const auto yaw = point->find("yaw");
  if (yaw != point->end() && yaw->is_number()) {
    position.yaw = yaw->get<float>();
  }
  return position;
}

bool is_companion_unit(std::uint64_t unit_id, std::uint64_t nibble) {
  return is_companion_unit_id(unit_id) && (unit_id & 0xF) == nibble;
}

Json packet_seq(const Json &message, std::uint32_t sequence) {
  const auto found = message.find("packet_seq");
  if (found != message.end() && found->is_object()) {
    return *found;
  }
  return {{"seq", std::to_string(sequence)}};
}

Json mate_state_message(int mate_npc_cms_id, std::uint64_t unit_id, MateState state) {
  return {{"mate_npc_cms_id", mate_npc_cms_id},
          {"mate_npc_unit_id", std::to_string(unit_id)},
          {"state_type", static_cast<int>(state)}};
}

std::string ascii_lowercase(std::string text) {
  for (auto &character : text) {
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  }
  return text;
}

// Text console grammar: "<command> [integer arguments]", named after the GmCmd fields.
std::optional<CompanionGmCommand> parse_text_command(const std::string &line) {
  std::istringstream stream(line);
  std::string name;
  stream >> name;
  name = ascii_lowercase(name);

  std::vector<std::int64_t> arguments;
  std::string token;
  while (stream >> token) {
    std::int64_t value{};
    std::istringstream number(token);
    if (!(number >> value) || !number.eof()) {
      return std::nullopt;
    }
    arguments.push_back(value);
  }

  const auto argument = [&arguments](std::size_t index) -> std::int64_t {
    return index < arguments.size() ? arguments[index] : 0;
  };
  CompanionGmCommand command;
  command.cms_id = static_cast<int>(std::clamp<std::int64_t>(argument(0), 0, 0x7FFFFFFF));
  command.value = static_cast<int>(std::clamp<std::int64_t>(argument(1), 0, 0x7FFFFFFF));
  if (name == "register_summon_npc") {
    command.kind = CompanionGmKind::register_summon;
  } else if (name == "spawn_summon_npc") {
    command.kind = CompanionGmKind::spawn_summon;
  } else if (name == "despawn_summon_npc") {
    command.kind = CompanionGmKind::despawn_summon;
  } else if (name == "register_mate_npc") {
    command.kind = CompanionGmKind::register_mate;
  } else if (name == "add_mate_intimate") {
    command.kind = CompanionGmKind::add_mate_intimacy;
  } else if (name == "spawn_mate_npc") {
    command.kind = CompanionGmKind::spawn_mate;
  } else if (name == "despawn_mate_npc") {
    command.kind = CompanionGmKind::despawn_mate;
  } else if (name == "invite_mate_npc") {
    command.kind = CompanionGmKind::invite_mate;
  } else if (name == "spawn_operable" || name == "spawn_vehicle" || name == "vehicle") {
    command.kind = CompanionGmKind::spawn_operable;
  } else if (name == "car") {
    // `/gm car` takes no argument: it spawns the catalog's one vehicle for any player.
    if (!arguments.empty()) {
      return std::nullopt;
    }
    command.kind = CompanionGmKind::spawn_operable;
    command.cms_id = kDefaultCarVehicleId;
    command.player_command = true;
  } else if (name == "damage_vehicle") {
    command.kind = CompanionGmKind::damage_vehicle;
    command.value = command.cms_id;
    command.cms_id = 0;
  } else if (name == "trade_voucher_add") {
    command.kind = CompanionGmKind::trade_voucher_add;
  } else {
    return std::nullopt;
  }
  return command;
}

std::optional<CompanionGmCommand> parse_structured_command(const Json &command) {
  if (!command.is_object() || command.size() != 1) {
    return std::nullopt;
  }

  const auto &[field, body] = *command.items().begin();
  if (!body.is_object()) {
    return std::nullopt;
  }
  CompanionGmCommand parsed;
  parsed.cms_id = positive_int(body, "cms_id").value_or(0);
  if (field == "register_summon_npc") {
    parsed.kind = CompanionGmKind::register_summon;
  } else if (field == "spawn_summon_npc") {
    parsed.kind = CompanionGmKind::spawn_summon;
  } else if (field == "despawn_summon_npc") {
    parsed.kind = CompanionGmKind::despawn_summon;
  } else if (field == "register_mate_npc") {
    parsed.kind = CompanionGmKind::register_mate;
  } else if (field == "add_mate_intimate") {
    parsed.kind = CompanionGmKind::add_mate_intimacy;
    parsed.value = non_negative_int(body, "value");
  } else if (field == "spawn_mate_npc") {
    parsed.kind = CompanionGmKind::spawn_mate;
  } else if (field == "despawn_mate_npc" || field == "invite_mate_npc") {
    parsed.kind = field == "despawn_mate_npc" ? CompanionGmKind::despawn_mate
                                              : CompanionGmKind::invite_mate;
    parsed.unit_id = decimal_id(body, "mate_npc_unit_id").value_or(0);
  } else if (field == "spawn_operable") {
    parsed.kind = CompanionGmKind::spawn_operable;
    parsed.cms_id = positive_int(body, "vehicle_cms_id").value_or(0);
  } else if (field == "damage_vehicle") {
    parsed.kind = CompanionGmKind::damage_vehicle;
    parsed.unit_id = decimal_id(body, "unit_id").value_or(0);
    parsed.value = non_negative_int(body, "damage_amount");
  } else if (field == "trade_voucher_add") {
    parsed.kind = CompanionGmKind::trade_voucher_add;
    parsed.value = non_negative_int(body, "valid_period");
  } else {
    return std::nullopt;
  }
  return parsed;
}

} // namespace

GameplayPacket common_error(std::uint32_t sequence, EErrorCode code) {
  return {kCommonErrorResponse,
          {{"ec", x5::ec::EErrorCode_Name(code)},
           {"packet_seq", {{"seq", std::to_string(sequence)}}}}};
}

GameplayPacket system_message(int key) {
  return {kSystemMessageNotification,
          {{"key", x5::cms::ESystemMessage_Name(static_cast<x5::cms::ESystemMessage>(key))}}};
}

std::optional<std::vector<CompanionGmCommand>>
parse_companion_gm_request(std::uint16_t opcode, const nlohmann::json &message) {
  if (!message.is_object()) {
    return std::nullopt;
  }

  std::vector<CompanionGmCommand> commands;
  if (opcode == kGmCommandTextRequest) {
    const auto text = message.find("text");
    if (text == message.end() || !text->is_string()) {
      return std::nullopt;
    }
    const auto command = parse_text_command(text->get<std::string>());
    if (!command) {
      return std::nullopt;
    }
    commands.push_back(*command);
    return commands;
  }

  if (opcode != kGmCommandRequest) {
    return std::nullopt;
  }
  const auto list = message.find("cmds");
  if (list == message.end() || !list->is_array() || list->empty() ||
      list->size() > kMaximumGmCommands) {
    return std::nullopt;
  }
  for (const auto &entry : *list) {
    const auto command = parse_structured_command(entry);
    if (!command) {
      return std::nullopt;
    }
    commands.push_back(*command);
  }
  return commands;
}

CompanionSession::CompanionSession(CompanionServices services,
                                   std::unique_ptr<ProgressAccess> progress,
                                   std::uint64_t connection_id, std::int64_t character_id)
    : services_(std::move(services)), progress_(std::move(progress)),
      connection_id_(connection_id), character_id_(character_id) {
  if (!services_.world || !services_.catalog || !progress_) {
    throw std::invalid_argument("CompanionSession requires its world, catalog and progress");
  }
}

bool CompanionSession::handles(std::uint16_t opcode, const nlohmann::json &message) const {
  switch (opcode) {
  case kSpawnMateNpcRequest:
  case kDespawnMateNpcRequest:
  case kInviteMateNpcRequest:
  case kSpawnSummonNpcRequest:
  case kRideUnitRequest:
  case kGetOffUnitRequest:
    return true;
  case kItemUseRequest: {
    // Registration consumables are Consumable stacks (Item.category), spent one per use.
    const auto item = positive_int(message, "item_cms_id");
    return item && services_.catalog->summon_registered_by_item(*item);
  }
  case kInteractUnitRequest: {
    const auto param = message.find("param");
    const auto target = decimal_id(message, "target_unit_id");
    if (param == message.end() || !param->is_object() || !target ||
        !is_companion_unit(*target, kOperableUnitTypeNibble)) {
      return false;
    }
    const auto interaction = positive_int(*param, "interaction_id");
    return interaction && services_.catalog->repair_task(*interaction) != nullptr;
  }
  default:
    return false;
  }
}

SessionResult CompanionSession::receive(std::uint16_t opcode, const nlohmann::json &message,
                                        std::uint32_t sequence, std::int64_t now_epoch_ticks) {
  if (!entered_) {
    return {{common_error(sequence, x5::ec::InvalidState)}, {}};
  }

  switch (opcode) {
  case kSpawnSummonNpcRequest:
    return spawn_summon(message, now_epoch_ticks);
  case kRideUnitRequest:
    return ride(message, now_epoch_ticks);
  case kGetOffUnitRequest:
    return get_off(message, now_epoch_ticks);
  case kItemUseRequest:
    // The pending registration animation owns its item consumption at the execute point. A
    // direct use request during the animation would skip it, so it is refused instead.
    if (registration_) {
      return {{common_error(sequence, x5::ec::InvalidState)}, {}};
    }
    return use_item(message, sequence);
  case kInteractUnitRequest:
    return repair(message, sequence, now_epoch_ticks);
  case kSpawnMateNpcRequest:
    return spawn_mate(message);
  case kDespawnMateNpcRequest:
    return despawn_mate(message, sequence);
  case kInviteMateNpcRequest:
    return invite_mate(message, sequence);
  default:
    return {};
  }
}

SessionResult CompanionSession::enter_world(const Participant &participant) {
  SessionResult result;
  if (entered_) {
    return result;
  }

  auto progress = progress_->load();
  // A crash can leave mates recorded as out; nothing of them exists after a restart.
  auto settled = progress;
  settled.revision = progress.revision + 1;
  bool changed = false;
  for (auto &[mate_id, mate] : settled.mates) {
    if (mate.state != static_cast<int>(MateState::be_away)) {
      mate.state = static_cast<int>(MateState::be_away);
      changed = true;
    }
  }
  if (changed && commit(progress, settled)) {
    progress = settled;
  }

  result.broadcast = services_.world->attach(participant);
  committed_dead_ = progress.dead;
  entered_ = true;
  result.own = snapshot_packets(progress);
  return result;
}

SessionResult CompanionSession::move(const WorldPosition &position,
                                     const nlohmann::json *movement) {
  SessionResult result;
  if (entered_) {
    result.broadcast = services_.world->move(connection_id_, position, movement);
  }
  return result;
}

SessionResult CompanionSession::reenter_region(const WorldPosition &position, std::int64_t tick,
                                               const CompanionViewToken &view) {
  SessionResult result;
  if (entered_) {
    result.broadcast = services_.world->reenter_region(connection_id_, position, tick, view);
  }
  return result;
}

SessionResult CompanionSession::suspend_region(const WorldPosition &destination) {
  SessionResult result;
  if (entered_) {
    result.broadcast = services_.world->suspend_region(connection_id_, destination);
  }
  return result;
}

SessionResult CompanionSession::set_channel(std::uint64_t channel_key) {
  SessionResult result;
  if (entered_) {
    result.broadcast = services_.world->set_channel(connection_id_, channel_key);
  }
  return result;
}

bool CompanionSession::entered() const {
  return entered_;
}

bool CompanionSession::riding() const {
  return entered_ && services_.world->ridden_unit(connection_id_).has_value();
}

void CompanionSession::observe_committed_life(bool dead) noexcept {
  committed_dead_ = dead;
}

SessionResult CompanionSession::dismount_if_dead(std::int64_t tick) {
  SessionResult result;
  if (committed_dead_ && riding()) {
    auto dismounted =
        services_.world->get_off(connection_id_, std::nullopt, tick, DismountReason::rider_killed);
    result.broadcast = std::move(dismounted.update);
  }
  return result;
}

DetachResult CompanionSession::leave(
    const std::function<void(const std::vector<VehicleSnapshot> &)> &before_commit) {
  if (!entered_) {
    registration_.reset();
    return {};
  }
  auto detached = services_.world->detach_staged(connection_id_, before_commit);
  return finish_leave(std::move(detached));
}

DetachResult CompanionSession::leave_retained(
    const std::function<void(const std::vector<VehicleSnapshot> &,
                             const std::shared_ptr<CompanionWorld::RetainedDetach> &)>
        &before_commit) {
  if (!entered_) {
    registration_.reset();
    return {};
  }
  auto detached = services_.world->detach_retained(connection_id_, before_commit);
  return finish_leave(std::move(detached));
}

DetachResult CompanionSession::finish_leave(DetachResult detached) {
  entered_ = false;
  registration_.reset();

  try {
    const auto before = progress_->load();
    auto after = before;
    after.revision = before.revision + 1;
    bool changed = false;
    for (auto &[mate_id, mate] : after.mates) {
      if (mate.state != static_cast<int>(MateState::be_away)) {
        mate.state = static_cast<int>(MateState::be_away);
        changed = true;
      }
    }
    if (changed && !commit(before, after)) {
      spdlog::warn("Mate states of character {} could not be settled at exit", character_id_);
    }
  } catch (const std::exception &error) {
    spdlog::warn("Mate state settlement failed for character {}: {}", character_id_,
                 error.what());
  }
  return detached;
}

std::vector<GameplayPacket> CompanionSession::snapshot_packets(const WorldProgress &progress) {
  auto summons = Json::array();
  for (const auto summon_id : progress.registered_summons) {
    summons.push_back(summon_id);
  }

  auto mates = Json::array();
  for (const auto &[mate_id, mate] : progress.mates) {
    mates.push_back({{"mate_npc_cms_id", mate_id},
                     {"state_type", x5::cms::EX5MateStateType_Name(
                                        static_cast<x5::cms::EX5MateStateType>(mate.state))},
                     {"housing_lot_number", mate.housing_lot_number},
                     {"intimate", mate.intimacy}});
  }

  return {{kWorldCharSummonNpcsNotification, {{"summon_npc_ids", std::move(summons)}}},
          {kWorldCharMatesNotification, {{"mate_infos", std::move(mates)}}}};
}

SessionResult CompanionSession::spawn_summon(const nlohmann::json &message, std::int64_t tick) {
  SessionResult result;
  const auto reply = [&result](EErrorCode code) {
    Json response = Json::object();
    if (code != x5::ec::Success) {
      response["ec"] = x5::ec::EErrorCode_Name(code);
    }
    result.own.insert(result.own.begin(), {kSpawnSummonNpcResponse, std::move(response)});
    return result;
  };

  const auto summon_id = positive_int(message, "summon_npc_cms_id");
  if (!summon_id || !services_.catalog->summon(*summon_id)) {
    return reply(x5::ec::Summon_InvalidSummonNpcCmsId);
  }
  const auto progress = progress_->load();
  if (progress.dead) {
    return reply(x5::ec::Summon_PlayerDead);
  }
  if (!progress.registered_summons.contains(*summon_id)) {
    return reply(x5::ec::Summon_NotRegistered);
  }

  auto spawned = services_.world->spawn_mount(connection_id_, *summon_id, tick);
  if (spawned.error != x5::ec::Success) {
    return reply(spawned.error);
  }
  result.broadcast = std::move(spawned.update);
  return reply(x5::ec::Success);
}

SessionResult CompanionSession::ride(const nlohmann::json &message, std::int64_t tick) {
  SessionResult result;
  const auto unit_id = decimal_id(message, "mount_unit_id");
  const auto slot = non_negative_int(message, "slot");
  EErrorCode error = x5::ec::Riding_TargetNotFound;
  if (progress_->load().dead) {
    error = x5::ec::InvalidState;
  } else if (unit_id && slot >= 0) {
    auto ridden = services_.world->ride(connection_id_, *unit_id, static_cast<std::uint32_t>(slot),
                                       tick);
    error = ridden.error;
    result.broadcast = std::move(ridden.update);
  }

  Json response = Json::object();
  if (error != x5::ec::Success) {
    response["ec"] = x5::ec::EErrorCode_Name(error);
  }
  result.own.push_back({kRideUnitResponse, std::move(response)});
  return result;
}

SessionResult CompanionSession::get_off(const nlohmann::json &message, std::int64_t tick) {
  SessionResult result;
  // No GetOffUnitRes exists: a refused dismount is answered with nothing.
  auto dismounted = services_.world->get_off(connection_id_, spawn_point(message, "dismount_point"),
                                             tick);
  result.broadcast = std::move(dismounted.update);
  return result;
}

SessionResult CompanionSession::use_item(const nlohmann::json &message, std::uint32_t sequence) {
  SessionResult result;
  const auto item = positive_int(message, "item_cms_id");
  const auto summon_id = item ? services_.catalog->summon_registered_by_item(*item) : std::nullopt;
  if (!item || !summon_id) {
    result.own.push_back(common_error(sequence, x5::ec::ItemUnusable));
    return result;
  }

  if (message.value("item_id", std::string("0")) != "0") {
    result.own.push_back(common_error(sequence, x5::ec::ItemIdInvalid));
    return result;
  }

  const auto before = progress_->load();
  const auto stack = before.stackables.find(*item);
  if (before.dead) {
    result.own.push_back(common_error(sequence, x5::ec::InvalidState));
    return result;
  }
  if (stack == before.stackables.end() || stack->second <= 0) {
    result.own.push_back(common_error(sequence, x5::ec::ItemCountError));
    return result;
  }
  // A summon is registered once; using another copy would waste the item.
  if (before.registered_summons.contains(*summon_id)) {
    result.own.push_back(common_error(sequence, x5::ec::ItemUnusable));
    return result;
  }

  if (before.revision == std::numeric_limits<std::int64_t>::max() ||
      before.registered_summons.size() >= kMaximumRegisteredSummons) {
    result.own.push_back(common_error(sequence, x5::ec::ItemError));
    return result;
  }

  auto after = before;
  after.revision = before.revision + 1;
  after.stackables[*item] -= 1;
  after.registered_summons.insert(*summon_id);
  if (!commit(before, after)) {
    result.own.push_back(common_error(sequence, x5::ec::ItemError));
    return result;
  }

  result.own.push_back(
      {kItemsUpdateNotification,
       {{"stackables", Json::array({{{"item_cms_id", *item},
                                     {"quantity", after.stackables.at(*item)}}})},
        {"use", Json::object()},
        {"packet_seq", packet_seq(message, sequence)}}});
  result.own.push_back({kRegisterSummonNpcResponse, {{"summon_npc_cms_id", *summon_id}}});
  return result;
}

SessionResult CompanionSession::repair(const nlohmann::json &message, std::uint32_t sequence,
                                       std::int64_t tick) {
  SessionResult result;
  const auto target = decimal_id(message, "target_unit_id").value_or(0);
  const auto &param = message.at("param");
  const auto interaction_id = positive_int(param, "interaction_id").value_or(0);
  const auto *task = services_.catalog->repair_task(interaction_id);
  const auto respond = [&](EErrorCode code) {
    Json response{{"target_unit_id", std::to_string(target)},
                  {"interaction_id", interaction_id},
                  {"packet_seq", packet_seq(message, sequence)}};
    if (code != x5::ec::Success) {
      response["ec"] = x5::ec::EErrorCode_Name(code);
    }
    result.own.push_back({kInteractUnitResponse, std::move(response)});
    return result;
  };

  const auto vehicle = services_.world->vehicle_of(character_id_);
  if (!task || !vehicle || vehicle->unit_id != target ||
      !services_.world->vehicle_in_reach(connection_id_, target)) {
    return respond(x5::ec::Interaction_TargetNotFound);
  }
  const auto *definition = services_.catalog->vehicle(vehicle->vehicle_cms_id);
  if (!definition || vehicle->health >= definition->max_health) {
    return respond(x5::ec::Interaction_ConditionsNotMatch);
  }

  // InteractionCondition ConsumeItem: the repair kit is spent with the repair.
  const auto before = progress_->load();
  auto after = before;
  after.revision = before.revision + 1;
  if (task->consumed_item_cms_id) {
    const auto stack = after.stackables.find(*task->consumed_item_cms_id);
    if (stack == after.stackables.end() || stack->second < task->consumed_item_count) {
      return respond(x5::ec::Interaction_ConditionsNotMatch);
    }
    stack->second -= task->consumed_item_count;
    if (!commit(before, after)) {
      return respond(x5::ec::ItemError);
    }
    result.own.push_back(
        {kItemsUpdateNotification,
         {{"stackables",
           Json::array({{{"item_cms_id", *task->consumed_item_cms_id},
                         {"quantity", after.stackables.at(*task->consumed_item_cms_id)}}})},
          {"use", Json::object()}}});
  }

  auto repaired = services_.world->repair_vehicle(connection_id_, target, task->add_durability,
                                                  tick);
  if (repaired.error != x5::ec::Success) {
    spdlog::warn("Wagon repair for character {} failed after the kit was spent: {}",
                 character_id_, x5::ec::EErrorCode_Name(repaired.error));
    return respond(repaired.error);
  }
  result.broadcast = std::move(repaired.update);
  return respond(x5::ec::Success);
}

SessionResult CompanionSession::spawn_mate(const nlohmann::json &message) {
  SessionResult result;
  const auto mate_id = positive_int(message, "mate_npc_cms_id");
  const auto lot = non_negative_int(message, "housing_lot_number");
  const auto progress = progress_->load();
  const auto record = mate_id ? progress.mates.find(*mate_id) : progress.mates.end();
  if (!mate_id || record == progress.mates.end() || !services_.catalog->mate(*mate_id)) {
    result.own.push_back(system_message(kMateSpawnNoMateMessage));
    return result;
  }
  // No live housing site ownership is wired into the world service, so a mate can only be
  // called outside a house (lot 0, the value the client sends when not in one). Candidate.
  if (lot != 0) {
    result.own.push_back(system_message(kMateSpawnSpaceMessage));
    return result;
  }
  std::size_t spawned = 0;
  for (const auto &[id, mate] : progress.mates) {
    if (services_.world->mate_unit(connection_id_, id)) {
      ++spawned;
    }
  }
  if (spawned >= kMaximumSpawnedMates) {
    result.own.push_back(system_message(kMateSpawnLimitMessage));
    return result;
  }

  auto spawned_state = record->second;
  spawned_state.state = static_cast<int>(MateState::spawned);
  spawned_state.housing_lot_number = lot;
  auto spawn = services_.world->spawn_mate(connection_id_, *mate_id, spawned_state);
  if (spawn.error != x5::ec::Success) {
    result.own.push_back(system_message(kMateSpawnSpaceMessage));
    return result;
  }
  if (!commit_mate_state(*mate_id, MateState::spawned, lot)) {
    CompanionUpdate removal;
    services_.world->despawn_mate(connection_id_, spawn.unit_id, removal);
    result.own.push_back(system_message(kMateSpawnSpaceMessage));
    return result;
  }

  result.broadcast = std::move(spawn.update);
  result.own.push_back({kChangedMateStateResponse,
                        mate_state_message(*mate_id, spawn.unit_id, MateState::spawned)});
  result.own.push_back(system_message(kMateSpawnSuccessMessage));
  return result;
}

SessionResult CompanionSession::despawn_mate(const nlohmann::json &message,
                                             std::uint32_t sequence) {
  SessionResult result;
  const auto unit_id = decimal_id(message, "mate_npc_unit_id");
  const auto mate_id = unit_id ? services_.world->owned_mate(connection_id_, *unit_id)
                               : std::nullopt;
  if (!mate_id) {
    result.own.push_back(common_error(sequence, x5::ec::Housing_NotInvitedMate));
    return result;
  }

  services_.world->despawn_mate(connection_id_, *unit_id, result.broadcast);
  if (!commit_mate_state(*mate_id, MateState::be_away, 0)) {
    spdlog::warn("Mate {} of character {} despawned but its state was not saved", *mate_id,
                 character_id_);
  }
  result.own.push_back(
      {kChangedMateStateResponse, mate_state_message(*mate_id, *unit_id, MateState::be_away)});
  return result;
}

SessionResult CompanionSession::invite_mate(const nlohmann::json &message,
                                            std::uint32_t sequence) {
  SessionResult result;
  const auto unit_id = decimal_id(message, "mate_npc_unit_id");
  const auto mate_id = unit_id ? services_.world->owned_mate(connection_id_, *unit_id)
                               : std::nullopt;
  if (!mate_id) {
    result.own.push_back(common_error(sequence, x5::ec::Housing_NotInvitedMate));
    return result;
  }

  const auto progress = progress_->load();
  const auto record = progress.mates.find(*mate_id);
  if (record == progress.mates.end()) {
    result.own.push_back(common_error(sequence, x5::ec::Housing_NotInvitedMate));
    return result;
  }
  if (record->second.state == static_cast<int>(MateState::invited)) {
    result.own.push_back(common_error(sequence, x5::ec::Housing_AlreadyInvitedMate));
    return result;
  }
  if (!commit_mate_state(*mate_id, MateState::invited, record->second.housing_lot_number)) {
    result.own.push_back(common_error(sequence, x5::ec::InvalidState));
    return result;
  }
  result.own.push_back(
      {kChangedMateStateResponse, mate_state_message(*mate_id, *unit_id, MateState::invited)});
  return result;
}

SessionResult CompanionSession::gm_command(const std::vector<CompanionGmCommand> &commands,
                                           const WorldPosition &position,
                                           std::int64_t now_epoch_ticks) {
  SessionResult result;
  for (const auto &command : commands) {
    SessionResult step;
    switch (command.kind) {
    case CompanionGmKind::register_summon:
      step = register_summon(command.cms_id);
      break;
    case CompanionGmKind::spawn_summon: {
      auto spawned = services_.world->spawn_mount(connection_id_, command.cms_id,
                                                  now_epoch_ticks);
      if (spawned.error == x5::ec::Success) {
        step.broadcast = std::move(spawned.update);
      }
      break;
    }
    case CompanionGmKind::despawn_summon:
      step.broadcast = services_.world->despawn_mount(connection_id_, now_epoch_ticks);
      break;
    case CompanionGmKind::register_mate:
      step = register_mate(command.cms_id);
      break;
    case CompanionGmKind::add_mate_intimacy:
      step = add_mate_intimacy(command.cms_id, command.value);
      break;
    case CompanionGmKind::spawn_mate:
      step = spawn_mate({{"mate_npc_cms_id", command.cms_id}, {"housing_lot_number", 0}});
      break;
    case CompanionGmKind::despawn_mate:
      step = despawn_mate({{"mate_npc_unit_id", std::to_string(command.unit_id)}}, 0);
      break;
    case CompanionGmKind::invite_mate:
      step = invite_mate({{"mate_npc_unit_id", std::to_string(command.unit_id)}}, 0);
      break;
    case CompanionGmKind::spawn_operable: {
      // A free vehicle at the GM's position, outside any trade run.
      const auto *vehicle = services_.catalog->vehicle(command.cms_id);
      if (vehicle) {
        auto spawned = services_.world->spawn_vehicle(connection_id_, vehicle->id, position,
                                                      vehicle->max_health, false,
                                                      now_epoch_ticks);
        if (spawned.error == x5::ec::Success) {
          step.broadcast = std::move(spawned.update);
        }
      }
      break;
    }
    case CompanionGmKind::damage_vehicle:
    case CompanionGmKind::trade_voucher_add:
      // Owned by the trade session, which also handles the wagon's run state.
      break;
    }
    result.own.insert(result.own.end(), std::make_move_iterator(step.own.begin()),
                      std::make_move_iterator(step.own.end()));
    result.broadcast.append(std::move(step.broadcast));
  }
  return result;
}

PlayerCompanionOutcome CompanionSession::player_mount(std::int64_t tick) {
  PlayerCompanionOutcome outcome;
  if (!entered_) {
    outcome.reply = "Enter the game world before using !mount.";
    return outcome;
  }
  if (riding()) {
    outcome.reply = "You are already riding. Type !dismount to get off.";
    return outcome;
  }

  const auto progress = progress_->load();
  if (committed_dead_ || progress.dead) {
    outcome.reply = "You cannot mount while dead.";
    return outcome;
  }
  const auto summon_id = player_mount_summon_id(progress);
  if (!summon_id) {
    outcome.reply = "No horse is available on this server.";
    return outcome;
  }

  auto spawned = services_.world->spawn_mount(connection_id_, *summon_id, tick);
  if (spawned.error != x5::ec::Success) {
    spdlog::info("Player mount refused for character {}: summon {} failed with {}", character_id_,
                 *summon_id, x5::ec::EErrorCode_Name(spawned.error));
    outcome.reply = "Your horse cannot be summoned right now.";
    return outcome;
  }

  outcome.result.broadcast = std::move(spawned.update);
  outcome.reply = "Your horse is ready. Type !dismount to get off.";
  return outcome;
}

PlayerCompanionOutcome CompanionSession::player_dismount(std::int64_t tick) {
  PlayerCompanionOutcome outcome;
  if (!entered_) {
    outcome.reply = "Enter the game world before using !dismount.";
    return outcome;
  }
  if (!riding()) {
    outcome.reply = "You are not riding anything.";
    return outcome;
  }

  auto dismounted = services_.world->get_off(connection_id_, std::nullopt, tick);
  if (dismounted.error != x5::ec::Success) {
    spdlog::info("Player dismount refused for character {}: {}", character_id_,
                 x5::ec::EErrorCode_Name(dismounted.error));
    outcome.reply = "You cannot get off right now.";
    return outcome;
  }

  outcome.result.broadcast = std::move(dismounted.update);
  outcome.reply = "You got off.";
  return outcome;
}

std::optional<int> CompanionSession::player_mount_summon_id(const WorldProgress &progress) const {
  // std::set iterates in ascending order, so the first rideable entry is the lowest id.
  for (const auto summon_id : progress.registered_summons) {
    const auto *definition = services_.catalog->summon(summon_id);
    if (definition && definition->rideable) {
      return summon_id;
    }
  }
  return services_.catalog->default_summon_id();
}

SessionResult CompanionSession::register_summon(int summon_npc_cms_id) {
  SessionResult result;
  if (!services_.catalog->summon(summon_npc_cms_id)) {
    return result;
  }
  const auto before = progress_->load();
  if (before.registered_summons.contains(summon_npc_cms_id)) {
    return result;
  }

  auto after = before;
  after.revision = before.revision + 1;
  after.registered_summons.insert(summon_npc_cms_id);
  if (commit(before, after)) {
    result.own.push_back({kRegisterSummonNpcResponse, {{"summon_npc_cms_id", summon_npc_cms_id}}});
  }
  return result;
}

SessionResult CompanionSession::register_mate(int mate_npc_cms_id) {
  SessionResult result;
  if (!services_.catalog->mate(mate_npc_cms_id)) {
    return result;
  }
  const auto before = progress_->load();
  if (before.mates.contains(mate_npc_cms_id)) {
    return result;
  }

  auto after = before;
  after.revision = before.revision + 1;
  after.mates[mate_npc_cms_id] = MateRecord{};
  if (commit(before, after)) {
    result.own.push_back({kRegisterMateNpcResponse, {{"mate_npc_cms_id", mate_npc_cms_id}}});
  }
  return result;
}

SessionResult CompanionSession::add_mate_intimacy(int mate_npc_cms_id, int value) {
  SessionResult result;
  const auto before = progress_->load();
  const auto record = before.mates.find(mate_npc_cms_id);
  if (record == before.mates.end() || value <= 0) {
    return result;
  }

  auto after = before;
  after.revision = before.revision + 1;
  const auto limit = services_.catalog->mate_intimacy_limit(mate_npc_cms_id);
  auto &intimacy = after.mates.at(mate_npc_cms_id).intimacy;
  intimacy = static_cast<int>(
      std::min<std::int64_t>(static_cast<std::int64_t>(intimacy) + value, limit));
  if (commit(before, after)) {
    result.own.push_back({kChangedMateIntimateResponse,
                          {{"mate_npc_cms_id", mate_npc_cms_id}, {"result_intimate", intimacy}}});
  }
  return result;
}

bool CompanionSession::commit_mate_state(int mate_npc_cms_id, MateState state,
                                         int housing_lot_number) {
  const auto before = progress_->load();
  if (!before.mates.contains(mate_npc_cms_id)) {
    return false;
  }

  auto after = before;
  after.revision = before.revision + 1;
  auto &mate = after.mates.at(mate_npc_cms_id);
  mate.state = static_cast<int>(state);
  mate.housing_lot_number = housing_lot_number;
  return commit(before, after).has_value();
}

std::optional<std::vector<CharacterItem>> CompanionSession::commit(const WorldProgress &before,
                                                                   const WorldProgress &after,
                                                                   const ItemTrade &trade) {
  try {
    return progress_->commit(before, after, trade);
  } catch (const std::exception &error) {
    spdlog::warn("Companion progress commit rejected for character {}: {}", character_id_,
                 error.what());
    return std::nullopt;
  }
}

} // namespace chronicles::companion
