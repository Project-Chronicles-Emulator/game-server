#include "world/features/trade/trade_session.hpp"

#include "world/features/npc/npc_population.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <limits>

namespace chronicles::trade {
namespace {

using Json = nlohmann::json;
using companion::SessionResult;
using x5::ec::EErrorCode;

// Voucher ids pack the character id above this many bits of per-character sequence, the
// scheme the NPC voucher counter uses, so GM-issued ids never collide with crafted ones.
constexpr int kVoucherSequenceBits = 20;
constexpr std::int64_t kTicksPerMinute = 60LL * 1000 * 10000;
constexpr std::int64_t kTicksPerMillisecond = 10000;
constexpr int kWagonCooldownMessage = 230;

std::optional<TradeRunIdentity> wagon_run_identity(const TradeRunRecord &run) {
  if (run.phase != TradeRunPhase::running && run.phase != TradeRunPhase::parked) {
    return std::nullopt;
  }
  return TradeRunIdentity{run.start_post_id, run.vehicle_cms_id, run.started_epoch_ticks};
}

bool within(const WorldPosition &first, const WorldPosition &second, std::int64_t radius_mm) {
  if (first.map_id != second.map_id) {
    return false;
  }
  const auto delta_x = static_cast<std::int64_t>(first.x_mm) - second.x_mm;
  const auto delta_y = static_cast<std::int64_t>(first.y_mm) - second.y_mm;
  const auto delta_z = static_cast<std::int64_t>(first.z_mm) - second.z_mm;
  return delta_x * delta_x + delta_y * delta_y + delta_z * delta_z <= radius_mm * radius_mm;
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

bool is_trading_escape(const Json &message) {
  const auto trading = message.find("is_trading");
  return trading != message.end() && trading->is_boolean() && trading->get<bool>();
}

WorldPosition departure_of(const TradingPost &post, int map_id) {
  return {map_id, post.departure.x_mm, post.departure.y_mm, post.departure.z_mm,
          post.departure_yaw};
}

void append(SessionResult &target, SessionResult source) {
  target.own.insert(target.own.end(), std::make_move_iterator(source.own.begin()),
                    std::make_move_iterator(source.own.end()));
  target.broadcast.append(std::move(source.broadcast));
  target.trailing.insert(target.trailing.end(), std::make_move_iterator(source.trailing.begin()),
                         std::make_move_iterator(source.trailing.end()));
}

} // namespace

WagonSettlementResult reconcile_wagon_settlement(ProgressAccess &progress,
                                                 const PendingWagonSettlement &pending) {
  if (!pending.run_identity ||
      pending.wagon.vehicle_cms_id != pending.run_identity->vehicle_cms_id) {
    return WagonSettlementResult::retry;
  }
  try {
    const auto before = progress.load();
    const auto identity = wagon_run_identity(before.trade_run);
    if (!identity || *identity != *pending.run_identity) {
      return WagonSettlementResult::superseded;
    }

    const auto health = std::max(pending.wagon.health, 0);
    bool player_position_matches = true;
    if (pending.player_position) {
      const auto character = progress.character();
      if (!character) {
        return WagonSettlementResult::retry;
      }
      player_position_matches = character->world_position == pending.player_position;
    }
    if (before.trade_run.wagon_position == pending.wagon.position &&
        before.trade_run.wagon_health == health && player_position_matches) {
      return WagonSettlementResult::already_applied;
    }
    if (before.revision == std::numeric_limits<std::int64_t>::max()) {
      return WagonSettlementResult::retry;
    }

    auto after = before;
    after.revision = before.revision + 1;
    after.trade_run.wagon_position = pending.wagon.position;
    after.trade_run.wagon_health = health;
    bool committed = false;
    if (pending.player_position) {
      committed = progress.commit_with_position(before, after, *pending.player_position);
    } else {
      committed = progress.commit(before, after, ItemTrade{}).has_value();
    }
    return committed ? WagonSettlementResult::applied : WagonSettlementResult::retry;
  } catch (const std::exception &error) {
    spdlog::warn("Detached trade wagon settlement needs retry: {}", error.what());
    return WagonSettlementResult::retry;
  }
}

TradeSession::TradeSession(TradeServices services, std::unique_ptr<ProgressAccess> progress,
                           std::uint64_t connection_id, std::int64_t character_id)
    : services_(std::move(services)), progress_(std::move(progress)),
      connection_id_(connection_id), character_id_(character_id) {
  if (!services_.world || !services_.vehicles || !services_.catalog || !progress_) {
    throw std::invalid_argument("TradeSession requires its world, catalogs and progress");
  }
}

bool TradeSession::handles(std::uint16_t opcode, const nlohmann::json &message) const {
  switch (opcode) {
  case kTradeCancelRequest:
  case kTradeMarketPriceRequest:
  case kTradeGoodLoadingRequest:
  case kTradeGoodSellRequest:
  case kTradingPostWagonParkingRequest:
  case kTradingPostWagonRemoveRequest:
    return true;
  case kEscapeRequest:
    return is_trading_escape(message);
  case kDialogueFunctionRequest: {
    const auto dialogue = positive_int(message, "dialogue_cms_id");
    return dialogue && services_.catalog->is_trade_management_dialogue(*dialogue);
  }
  case kDialogueEndRequest:
    return dialogue_npc_position_.has_value();
  default:
    return false;
  }
}

SessionResult TradeSession::receive(std::uint16_t opcode, const nlohmann::json &message,
                                    std::uint32_t sequence, const WorldPosition &position,
                                    std::int64_t now_epoch_ticks) {
  switch (opcode) {
  case kDialogueFunctionRequest:
    return dialogue_function(message, sequence);
  case kDialogueEndRequest:
    return end_dialogue();
  case kTradeMarketPriceRequest:
    return {{{kTradeMarketPriceResponse,
              market_price_message(*services_.catalog, now_epoch_ticks)}},
            {},
            {}};
  case kTradeGoodLoadingRequest:
    return load(message, sequence, position, now_epoch_ticks);
  case kTradeGoodSellRequest:
    return sell(message, sequence, position, now_epoch_ticks);
  case kTradingPostWagonParkingRequest:
    return park(message, sequence, position, now_epoch_ticks);
  case kTradingPostWagonRemoveRequest:
    return stow(sequence, now_epoch_ticks);
  case kTradeCancelRequest:
    return cancel(sequence, now_epoch_ticks);
  case kEscapeRequest:
    return escape(sequence, position, now_epoch_ticks);
  default:
    return {};
  }
}

bool TradeSession::is_trade_manager(int npc_cms_id) const {
  return services_.trade_managers && services_.trade_managers->contains(npc_cms_id);
}

QuestEvents TradeSession::take_quest_events() {
  auto events = std::move(quest_events_);
  quest_events_.clear();
  return events;
}

SessionResult TradeSession::start_dialogue(std::uint64_t target_unit_id,
                                           const WorldPosition &npc_position,
                                           const WorldPosition &player_position) {
  SessionResult result;
  Json response{{"target_unit_id", std::to_string(target_unit_id)}};
  if (!within(npc_position, player_position, kTradeManagerReachMm)) {
    response["ec"] = x5::ec::EErrorCode_Name(x5::ec::Dialogue_InvalidPos);
    result.own.push_back({kDialogueStartResponse, std::move(response)});
    return result;
  }

  dialogue_npc_position_ = npc_position;
  response["target_unit_pos"] = {{"millimeter_x", npc_position.x_mm},
                                 {"millimeter_y", npc_position.y_mm},
                                 {"millimeter_z", npc_position.z_mm}};
  result.own.push_back({kDialogueStartResponse, std::move(response)});
  return result;
}

SessionResult TradeSession::dialogue_function(const nlohmann::json &message,
                                              std::uint32_t sequence) {
  SessionResult result;
  if (!dialogue_npc_position_) {
    result.own.push_back(error_packet(sequence, x5::ec::Dialogue_InvalidTarget));
    return result;
  }

  // The client derives the current trading post from the trade manager's position; the
  // emulator picks the placed post whose departure point is nearest that NPC.
  const auto post = post_at(*services_.catalog, *dialogue_npc_position_);
  if (!post) {
    result.own.push_back(error_packet(sequence, x5::ec::Trade_PostUnstartable));
    return result;
  }
  current_post_id_ = post;
  // Ending the dialogue lets the client run its queued "open trade UI" action (Candidate).
  return end_dialogue();
}

SessionResult TradeSession::end_dialogue() {
  dialogue_npc_position_.reset();
  return {{{kDialogueEndNotification,
            {{"player_unit_id", player_wire_unit_id(character_id_)}}}},
          {},
          {}};
}

std::optional<int> TradeSession::current_post(const WorldPosition &position) const {
  if (!current_post_id_) {
    return std::nullopt;
  }
  const auto *post = services_.catalog->post(*current_post_id_);
  if (!post || !player_near_post(*post, position)) {
    return std::nullopt;
  }
  return current_post_id_;
}

TradeContext TradeSession::context(const Character &character, const WorldProgress &before,
                                   std::int64_t now) const {
  return {*services_.catalog, *services_.vehicles, services_.items.get(), character, before, now};
}

std::optional<std::vector<CharacterItem>> TradeSession::commit(const WorldProgress &before,
                                                               const TradePlan &plan) {
  auto after = plan.next;
  after.revision = before.revision + 1;
  ItemTrade trade;
  trade.granted_items = plan.granted_items;
  try {
    auto result = progress_->commit(before, after, trade);
    if (result) {
      const auto identity = wagon_run_identity(after.trade_run);
      if (identity != active_run_identity_) {
        trade_wagon_unit_id_.reset();
      }
      active_run_identity_ = identity;
    }
    return result;
  } catch (const std::exception &error) {
    spdlog::warn("Trade commit rejected for character {}: {}", character_id_, error.what());
    return std::nullopt;
  }
}

SessionResult TradeSession::load(const nlohmann::json &message, std::uint32_t sequence,
                                 const WorldPosition &position, std::int64_t now) {
  SessionResult result;
  const auto fail = [&](EErrorCode code) {
    result.own.push_back(error_packet(sequence, code));
    return result;
  };

  LoadRequest request;
  const auto parsed = parse_load_request(message, request);
  if (parsed != x5::ec::Success) {
    return fail(parsed);
  }
  const auto post_id = current_post(position);
  if (!post_id) {
    return fail(x5::ec::Trade_PostTooFar);
  }
  const auto character = progress_->character();
  if (!character) {
    return fail(x5::ec::InvalidChar);
  }

  const auto before = progress_->load();
  auto plan = plan_load(context(*character, before, now), *post_id, request);
  if (plan.error != x5::ec::Success) {
    return fail(plan.error);
  }

  std::optional<companion::SpawnResult> spawned;
  if (plan.departure_post_id) {
    // A summoned horse is sent away so the owner can take the wagon's driver seat.
    if (services_.world->ridden_unit(connection_id_)) {
      result.broadcast.append(services_.world->get_off(connection_id_, position, now).update);
    }
    const auto *post = services_.catalog->post(*plan.departure_post_id);
    const auto departure = departure_of(*post, position.map_id);
    spawned = services_.world->spawn_vehicle(connection_id_, plan.next.trade_run.vehicle_cms_id,
                                             departure, plan.next.trade_run.wagon_health, true,
                                             now);
    if (spawned->error != x5::ec::Success) {
      result.own.push_back({kSpawnTradeWagonResponse,
                            {{"ec", x5::ec::EErrorCode_Name(x5::ec::Trade_WagonSpawnFailed)}}});
      return result;
    }
    plan.next.trade_run.wagon_position = departure;
  }

  if (!commit(before, plan)) {
    if (spawned) {
      companion::CompanionUpdate removal;
      services_.world->remove_vehicle(character_id_, companion::DismountReason::clear_trade, now,
                                      removal);
      result.broadcast.append(std::move(removal));
    }
    return fail(x5::ec::ItemError);
  }

  result.own.push_back(loading_packet(plan));
  if (spawned) {
    trade_wagon_unit_id_ = spawned->unit_id;
    trade_start_pending_ = false;
    result.own.push_back({kSpawnTradeWagonResponse, Json::object()});
    result.broadcast.append(std::move(spawned->update));
    result.trailing.push_back(trade_start_packet(plan.next));
  }
  return result;
}

SessionResult TradeSession::sell(const nlohmann::json &message, std::uint32_t sequence,
                                 const WorldPosition &position, std::int64_t now) {
  SessionResult result;
  const auto fail = [&](EErrorCode code) {
    result.own.push_back(error_packet(sequence, code));
    return result;
  };

  SellRequest request;
  const auto parsed = parse_sell_request(message, request);
  if (parsed != x5::ec::Success) {
    return fail(parsed);
  }
  const auto post_id = current_post(position);
  if (!post_id) {
    return fail(x5::ec::Trade_PostTooFar);
  }
  const auto character = progress_->character();
  if (!character) {
    return fail(x5::ec::InvalidChar);
  }

  const auto before = progress_->load();
  const auto seed = static_cast<std::uint32_t>(now ^ (character_id_ << 16));
  const auto plan = plan_sell(context(*character, before, now), *post_id, request, seed);
  if (plan.error != x5::ec::Success) {
    if (plan.error == x5::ec::ItemError) {
      // SM_INVENTORY_ERROR_TRADE_FAIL: the payment would exceed the currency limit.
      result.own.push_back(companion::system_message(163));
    }
    return fail(plan.error);
  }

  const auto committed = commit(before, plan);
  if (!committed) {
    return fail(x5::ec::ItemError);
  }

  // Each sold unit counts toward TradePayment objectives naming this post and good.
  for (const auto &sale : plan.sale_results) {
    quest_events_.push_back({QuestEventKind::TradeGoodSold, sale.good_cms_id, 0, 1, *post_id});
  }
  result.own.push_back(selling_packet(plan, *committed));
  if (plan.run_ended) {
    services_.world->remove_vehicle(character_id_, companion::DismountReason::clear_trade, now,
                                    result.broadcast);
    result.trailing.push_back({kTradeEndNotification, Json::object()});
  }
  return result;
}

SessionResult TradeSession::park(const nlohmann::json &message, std::uint32_t sequence,
                                 const WorldPosition &position, std::int64_t now) {
  SessionResult result;
  const auto post_id = positive_int(message, "trading_post_cms_id").value_or(0);
  const auto reply = [&](EErrorCode code) {
    Json response{{"trading_post_cms_id", post_id}};
    if (code != x5::ec::Success) {
      response["ec"] = x5::ec::EErrorCode_Name(code);
    }
    result.own.push_back({kTradingPostWagonParkingResponse, std::move(response)});
    return result;
  };

  const auto *post = services_.catalog->post(post_id);
  if (!post || !post->placed || !post->map_ids.contains(position.map_id)) {
    return reply(x5::ec::Trade_InvalidPost);
  }
  const auto wagon = services_.world->vehicle_of(character_id_);
  if (!wagon) {
    return reply(x5::ec::Trade_NotInProgress);
  }
  if (!wagon_inside_post(*post, wagon->position)) {
    return reply(x5::ec::Trade_PostTooFar);
  }

  const auto character = progress_->character();
  if (!character) {
    return reply(x5::ec::InvalidChar);
  }
  const auto before = progress_->load();
  auto plan = plan_park(context(*character, before, now), post_id, wagon->position);
  plan.next.trade_run.wagon_health = wagon->health;
  if (plan.error != x5::ec::Success) {
    return reply(plan.error);
  }
  if (!commit(before, plan)) {
    return reply(x5::ec::InvalidState);
  }
  current_post_id_ = post_id;
  return reply(x5::ec::Success);
}

SessionResult TradeSession::stow(std::uint32_t sequence, std::int64_t now) {
  SessionResult result;
  const auto character = progress_->character();
  if (!character) {
    result.own.push_back(error_packet(sequence, x5::ec::InvalidChar));
    return result;
  }

  const auto before = progress_->load();
  const auto plan = plan_stow(context(*character, before, now));
  if (plan.error != x5::ec::Success || !commit(before, plan)) {
    result.own.push_back(
        error_packet(sequence, plan.error != x5::ec::Success ? plan.error : x5::ec::InvalidState));
    return result;
  }
  // No response packet exists; the wagon leaves and the client opens the sell UI.
  services_.world->remove_vehicle(character_id_, companion::DismountReason::clear_trade, now,
                                  result.broadcast);
  return result;
}

SessionResult TradeSession::cancel(std::uint32_t sequence, std::int64_t now) {
  SessionResult result;
  const auto character = progress_->character();
  if (!character) {
    result.own.push_back(error_packet(sequence, x5::ec::InvalidChar));
    return result;
  }

  const auto before = progress_->load();
  const auto plan = plan_cancel(context(*character, before, now));
  if (plan.error != x5::ec::Success || !commit(before, plan)) {
    result.own.push_back(
        error_packet(sequence, plan.error != x5::ec::Success ? plan.error : x5::ec::InvalidState));
    return result;
  }
  // Message 173: certificates and fees are not refunded.
  services_.world->remove_vehicle(character_id_, companion::DismountReason::clear_trade, now,
                                  result.broadcast);
  result.trailing.push_back({kTradeEndNotification, Json::object()});
  return result;
}

SessionResult TradeSession::escape(std::uint32_t sequence, const WorldPosition &position,
                                   std::int64_t now) {
  SessionResult result;
  const auto character = progress_->character();
  const auto before = progress_->load();
  const auto &run = before.trade_run;
  if (!character ||
      (run.phase != TradeRunPhase::running && run.phase != TradeRunPhase::parked)) {
    result.own.push_back(
        {kSpawnTradeWagonResponse, {{"ec", x5::ec::EErrorCode_Name(x5::ec::Trade_NotInProgress)}}});
    return result;
  }
  active_run_identity_ = wagon_run_identity(run);

  const auto cooldown_ticks =
      services_.catalog->settings().wagon_respawn_cooldown_ms * kTicksPerMillisecond;
  if (now - run.last_wagon_spawn_epoch_ticks < cooldown_ticks) {
    result.own.push_back(companion::system_message(kWagonCooldownMessage));
    return result;
  }

  // Candidate recovery: the wagon returns beside its owner with its health, the owner aboard.
  int health = run.wagon_health;
  if (const auto removed = services_.world->remove_vehicle(
          character_id_, companion::DismountReason::mount_removed, now, result.broadcast)) {
    health = removed->health;
  }
  trade_wagon_unit_id_.reset();
  if (services_.world->ridden_unit(connection_id_)) {
    result.broadcast.append(services_.world->get_off(connection_id_, position, now).update);
  }
  auto spawned = services_.world->spawn_vehicle(connection_id_, run.vehicle_cms_id, position,
                                                health, true, now);
  if (spawned.error != x5::ec::Success) {
    result.own.push_back(
        {kSpawnTradeWagonResponse,
         {{"ec", x5::ec::EErrorCode_Name(x5::ec::Trade_WagonSpawnFailed)}}});
    return result;
  }
  trade_wagon_unit_id_ = spawned.unit_id;

  auto plan = plan_wagon_state(context(*character, before, now), position, health, true);
  plan.next.trade_run.phase = TradeRunPhase::running;
  if (!commit(before, plan)) {
    spdlog::warn("Escaped wagon of character {} was respawned but not saved", character_id_);
  }
  result.own.push_back({kSpawnTradeWagonResponse, Json::object()});
  result.broadcast.append(std::move(spawned.update));

  // The owner is seated again, so a run restored on foot can announce itself now.
  auto boarded = wagon_boarded();
  result.trailing.insert(result.trailing.end(), std::make_move_iterator(boarded.trailing.begin()),
                         std::make_move_iterator(boarded.trailing.end()));
  return result;
}

SessionResult TradeSession::enter_world(const WorldPosition &,
                                        std::int64_t now_epoch_ticks) {
  SessionResult result;
  const auto progress = progress_->load();
  const auto &run = progress.trade_run;
  const auto identity = wagon_run_identity(run);
  if (identity != active_run_identity_) {
    trade_wagon_unit_id_.reset();
  }
  active_run_identity_ = identity;
  if (run.phase == TradeRunPhase::idle) {
    trade_start_pending_ = false;
    return result;
  }

  // Loading and stowed runs have no wagon out; the goods container is restored as is.
  bool wagon_out = false;
  bool owner_driving = false;
  if ((run.phase == TradeRunPhase::running || run.phase == TradeRunPhase::parked) &&
      run.wagon_position) {
    if (const auto existing = services_.world->vehicle_of(character_id_)) {
      if (trade_wagon_unit_id_ && existing->unit_id == *trade_wagon_unit_id_) {
        wagon_out = true;
        owner_driving = existing->owner_riding;
        result.trailing.push_back(wagon_position_packet(existing->position));
      } else {
        spdlog::warn("Existing vehicle is not this session's trade wagon character={}",
                     character_id_);
      }
    } else {
      auto wagon_position = *run.wagon_position;
      auto spawned = services_.world->spawn_vehicle(connection_id_, run.vehicle_cms_id,
                                                    wagon_position, run.wagon_health, false,
                                                    now_epoch_ticks);
      if (spawned.error == x5::ec::Success) {
        trade_wagon_unit_id_ = spawned.unit_id;
        wagon_out = true;
        result.broadcast = std::move(spawned.update);
        result.trailing.push_back(wagon_position_packet(wagon_position));
      }
    }
  }
  if (run.phase == TradeRunPhase::parked || run.phase == TradeRunPhase::stowed) {
    current_post_id_ = run.parked_post_id;
  } else if (run.phase == TradeRunPhase::loading) {
    current_post_id_ = run.start_post_id;
  }

  // TradeStartNoti would leave an owner on foot in front of a black screen (see
  // trade_start_pending_), so it is sent now only to a driving owner.
  trade_start_pending_ = wagon_out && !owner_driving;
  if (owner_driving) {
    result.trailing.insert(result.trailing.begin(), trade_start_packet(progress));
  }
  return result;
}

SessionResult TradeSession::wagon_boarded() {
  SessionResult result;
  if (!trade_start_pending_) {
    return result;
  }

  const auto wagon = services_.world->vehicle_of(character_id_);
  if (!wagon || !wagon->owner_riding || !trade_wagon_unit_id_ ||
      wagon->unit_id != *trade_wagon_unit_id_) {
    return result;
  }

  trade_start_pending_ = false;
  const auto progress = progress_->load();
  const auto phase = progress.trade_run.phase;
  if (phase == TradeRunPhase::running || phase == TradeRunPhase::parked) {
    // Delivered after MountUnitNoti, so the client's next trade tick sees its rider at once.
    result.trailing.push_back(trade_start_packet(progress));
  }
  return result;
}

RegionCheckpointResult TradeSession::checkpoint_region(const WorldPosition &destination,
                                                        std::int64_t now_epoch_ticks) {
  const auto wagon = services_.world->vehicle_of(character_id_);
  if (!wagon || !wagon->owner_riding || !trade_wagon_unit_id_ ||
      wagon->unit_id != *trade_wagon_unit_id_) {
    return RegionCheckpointResult::no_driven_trade_wagon;
  }

  try {
    const auto character = progress_->character();
    const auto before = progress_->load();
    if (before.trade_run.phase != TradeRunPhase::running &&
        before.trade_run.phase != TradeRunPhase::parked) {
      return RegionCheckpointResult::no_driven_trade_wagon;
    }
    if (!active_run_identity_ || wagon_run_identity(before.trade_run) != active_run_identity_) {
      return RegionCheckpointResult::conflict;
    }
    if (!character) {
      return RegionCheckpointResult::conflict;
    }

    const auto plan = plan_wagon_state(context(*character, before, now_epoch_ticks), destination,
                                       wagon->health, false);
    if (plan.error != x5::ec::Success ||
        before.revision == std::numeric_limits<std::int64_t>::max()) {
      return RegionCheckpointResult::conflict;
    }

    auto after = plan.next;
    after.revision = before.revision + 1;
    if (!progress_->commit_with_position(before, after, destination)) {
      return RegionCheckpointResult::conflict;
    }
    return RegionCheckpointResult::committed;
  } catch (const std::exception &error) {
    // A lost acknowledgement may follow a successful database COMMIT. Do not retry or send
    // a conflicting map transition until a new session reads the durable state.
    spdlog::error("Trade wagon region checkpoint outcome unknown character={}: {}",
                  character_id_, error.what());
    return RegionCheckpointResult::unknown;
  }
}

std::optional<PendingWagonSettlement> TradeSession::capture_wagon_settlement(
    const std::vector<companion::VehicleSnapshot> &vehicles,
    std::optional<WorldPosition> player_position) const {
  if (vehicles.empty() || !trade_wagon_unit_id_) {
    return std::nullopt;
  }
  const auto wagon = std::find_if(vehicles.begin(), vehicles.end(), [&](const auto &vehicle) {
    return vehicle.unit_id == *trade_wagon_unit_id_;
  });
  if (wagon == vehicles.end()) {
    return std::nullopt;
  }
  return PendingWagonSettlement{*wagon, active_run_identity_,
                                std::move(player_position)};
}

SessionResult TradeSession::wagon_dismounted(std::int64_t now_epoch_ticks) {
  SessionResult result;
  const auto wagon = services_.world->vehicle_of(character_id_);
  if (!wagon || wagon->owner_riding || !trade_wagon_unit_id_ ||
      wagon->unit_id != *trade_wagon_unit_id_) {
    return result;
  }

  const auto character = progress_->character();
  const auto before = progress_->load();
  if (character && (before.trade_run.phase == TradeRunPhase::running ||
                    before.trade_run.phase == TradeRunPhase::parked)) {
    const auto plan = plan_wagon_state(context(*character, before, now_epoch_ticks),
                                       wagon->position, wagon->health, false);
    if (plan.error == x5::ec::Success && !commit(before, plan)) {
      spdlog::warn("Wagon transform of character {} was not saved", character_id_);
    }
  }
  result.own.push_back(wagon_position_packet(wagon->position));
  return result;
}

bool TradeSession::wagons_left(const std::vector<companion::VehicleSnapshot> &vehicles,
                               std::int64_t now_epoch_ticks) {
  if (vehicles.empty()) {
    return true;
  }
  try {
    const auto character = progress_->character();
    const auto before = progress_->load();
    if (!character) {
      return false;
    }
    if (before.trade_run.phase != TradeRunPhase::running &&
        before.trade_run.phase != TradeRunPhase::parked) {
      return true;
    }
    const auto &wagon = vehicles.front();
    const auto plan = plan_wagon_state(context(*character, before, now_epoch_ticks),
                                       wagon.position, wagon.health, false);
    if (plan.error != x5::ec::Success || !commit(before, plan)) {
      spdlog::warn("Wagon of character {} was not saved at exit", character_id_);
      return false;
    }
    return true;
  } catch (const std::exception &error) {
    spdlog::warn("Wagon persistence failed for character {}: {}", character_id_, error.what());
    return false;
  }
}

SessionResult TradeSession::gm_command(
    const std::vector<companion::CompanionGmCommand> &commands, std::int64_t now_epoch_ticks) {
  SessionResult result;
  for (const auto &command : commands) {
    if (command.kind == companion::CompanionGmKind::damage_vehicle) {
      append(result, damage_wagon(command.unit_id, command.value, now_epoch_ticks));
    } else if (command.kind == companion::CompanionGmKind::trade_voucher_add) {
      append(result, add_voucher(command.cms_id, command.value, now_epoch_ticks));
    }
  }
  return result;
}

SessionResult TradeSession::damage_wagon(std::uint64_t unit_id, int amount, std::int64_t now) {
  SessionResult result;
  const auto wagon = services_.world->vehicle_of(character_id_);
  const auto target = unit_id != 0 ? unit_id : (wagon ? wagon->unit_id : 0);
  if (target == 0 || amount <= 0) {
    return result;
  }

  auto damaged = services_.world->damage_vehicle(target, amount, now);
  if (damaged.error != x5::ec::Success) {
    return result;
  }
  result.broadcast = std::move(damaged.update);

  // A destroyed trade wagon loses its cargo: the run ends without payment (Candidate; the
  // client ignores TradeEventResultNoti, so only TradeEndNoti resets its trade state).
  if (damaged.destroyed && damaged.owner_character_id == character_id_ && wagon &&
      wagon->unit_id == target) {
    const auto character = progress_->character();
    const auto before = progress_->load();
    if (character) {
      const auto plan = plan_cancel(context(*character, before, now));
      if (plan.error == x5::ec::Success && commit(before, plan)) {
        result.trailing.push_back({kTradeEndNotification, Json::object()});
      }
    }
  }
  return result;
}

SessionResult TradeSession::add_voucher(int good_cms_id, int valid_minutes, std::int64_t now) {
  SessionResult result;
  const auto *good = services_.catalog->good(good_cms_id);
  if (!good) {
    return result;
  }

  const auto before = progress_->load();
  if (before.trade_vouchers.size() >= kMaximumTradeVouchers) {
    return result;
  }
  const auto minutes = valid_minutes > 0 ? valid_minutes : good->valid_period_minutes;
  const auto sequence = static_cast<std::int64_t>(before.trade_vouchers.size()) + 1;
  const auto voucher_id = (character_id_ << kVoucherSequenceBits) + sequence;

  TradePlan plan;
  plan.next = before;
  plan.next.trade_vouchers[voucher_id] = {good->id, now, now + minutes * kTicksPerMinute, false};
  if (!commit(before, plan)) {
    return result;
  }

  const auto &record = plan.next.trade_vouchers.at(voucher_id);
  const Json voucher{{"voucher_id", std::to_string(voucher_id)},
                     {"good_cms_id", record.good_cms_id},
                     {"created_epochtime", std::to_string(record.created_epoch_ticks)},
                     {"expiration_epochtime", std::to_string(record.expiration_epoch_ticks)}};
  result.own.push_back(
      {kItemsUpdateNotification,
       {{"trade_voucher", {{"vouchers", Json::array({voucher})}}}}});
  return result;
}

} // namespace chronicles::trade
