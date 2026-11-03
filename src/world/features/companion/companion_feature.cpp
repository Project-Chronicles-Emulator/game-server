#include "world/features/companion/companion_feature.hpp"

#include "data/catalog_requirements.hpp"
#include "world/features/npc/npc_population.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <iterator>
#include <utility>

namespace chronicles::companion {
namespace {

std::shared_ptr<const CompanionCatalog> load_catalog(ClientCatalog *catalog) {
  if (!catalog ||
      !has_catalog_tables(*catalog, CompanionCatalog::required_tables(), "Companions")) {
    return {};
  }

  auto companions = CompanionCatalog::load(*catalog);
  spdlog::info("Companion catalog loaded: {} summons, {} vehicles, {} mates",
               companions->summon_count(), companions->vehicle_count(), companions->mate_count());
  return companions;
}

} // namespace

void append_session_result(SessionResult &target, SessionResult source) {
  target.own.insert(target.own.end(), std::make_move_iterator(source.own.begin()),
                    std::make_move_iterator(source.own.end()));
  target.broadcast.append(std::move(source.broadcast));
  target.trailing.insert(target.trailing.end(), std::make_move_iterator(source.trailing.begin()),
                         std::make_move_iterator(source.trailing.end()));
}

CompanionFeature::CompanionFeature(ClientCatalog *catalog, std::shared_ptr<const ShopCatalog> items,
                                   std::shared_ptr<CompanionDetachBudget> detach_budget)
    : CompanionFeature(load_catalog(catalog), std::move(items), std::move(detach_budget)) {
}

CompanionFeature::CompanionFeature(std::shared_ptr<const CompanionCatalog> catalog,
                                   std::shared_ptr<const ShopCatalog> items,
                                   std::shared_ptr<CompanionDetachBudget> detach_budget)
    : catalog_(std::move(catalog)),
      world_(catalog_ ? std::make_shared<CompanionWorld>(catalog_, CompanionWorldConfig{},
                                                        std::move(detach_budget))
                      : nullptr),
      items_(std::move(items)) {
}

bool CompanionFeature::enabled() const noexcept {
  return world_ != nullptr;
}

const std::shared_ptr<const CompanionCatalog> &CompanionFeature::catalog() const noexcept {
  return catalog_;
}

const std::shared_ptr<CompanionWorld> &CompanionFeature::world() const noexcept {
  return world_;
}

std::unique_ptr<CompanionSession>
CompanionFeature::create_session(std::unique_ptr<ProgressAccess> progress,
                                 std::uint64_t connection_id, std::int64_t character_id) const {
  if (!world_ || !is_supported_character_id(character_id)) {
    return {};
  }

  CompanionServices services{world_, catalog_, items_};
  return std::make_unique<CompanionSession>(std::move(services), std::move(progress), connection_id,
                                            character_id);
}

bool CompanionFeature::handle_command(CompanionSession &session,
                                      const WorldGameplayRequest &request,
                                      const std::optional<WorldPosition> &position,
                                      CompanionSessionContext &context,
                                      SessionResult &result) const {
  // The established parser checks its own two GM opcodes before parsing command bodies.
  const auto commands = parse_companion_gm_request(request.opcode, request.message);
  if (!commands) {
    return false;
  }

  const bool is_game_master = context.companion_is_game_master();
  const bool player_may_run = std::ranges::all_of(*commands, [](const CompanionGmCommand &command) {
    return command.player_command;
  });
  if (!context.companion_has_handoff() || (!is_game_master && !player_may_run) || !position ||
      !context.companion_allow_gm_request()) {
    spdlog::warn("GM companion command refused for connection {}",
                 context.companion_connection_id());
    context.companion_reply(is_game_master
                                ? "Vehicle/mount command refused (enter the world or slow down)."
                                : "GM commands need a game master account.");
    return true;
  }
  if (!is_game_master && !context.companion_allow_player_car()) {
    context.companion_reply("Wait a few seconds before calling another car.");
    return true;
  }

  append_session_result(result,
                        session.gm_command(*commands, *position, context.companion_epoch_ticks()));
  context.companion_trade_commands(*commands, result);
  context.companion_reply(
      is_game_master
          ? "Vehicle/mount command processed. If nothing appeared, check the id (/gm help 3)."
          : "Your car is ready.");
  return true;
}

bool CompanionFeature::handle_request(CompanionSession &session,
                                      const WorldGameplayRequest &request,
                                      CompanionSessionContext &context,
                                      SessionResult &result) const {
  if (!session.handles(request.opcode, request.message)) {
    return false;
  }

  append_session_result(result, session.receive(request.opcode, request.message, request.sequence,
                                                context.companion_epoch_ticks()));
  if (request.opcode == kGetOffUnitRequest) {
    context.companion_trade_dismounted(result);
  } else if (request.opcode == kRideUnitRequest) {
    context.companion_trade_boarded(result);
  }
  return true;
}

void CompanionFeature::enter_world(CompanionSession &session, const WorldPosition &position,
                                   CompanionSessionContext &context, SessionResult &result) const {
  if (!context.companion_has_handoff()) {
    return;
  }

  try {
    const Participant participant{context.companion_connection_id(),
                                  context.companion_character_id(),
                                  context.companion_channel_key(), position,
                                  context.companion_view_token()};
    append_session_result(result, session.enter_world(participant));
    context.companion_enter_trade(position, result);
  } catch (const std::exception &error) {
    context.companion_entry_failed();
    spdlog::error("Companion world entry failed connection={}: {}",
                  context.companion_connection_id(), error.what());
  } catch (...) {
    context.companion_entry_failed();
    spdlog::error("Companion world entry failed connection={}: unknown error",
                  context.companion_connection_id());
  }
}

bool CompanionFeature::leave_world(CompanionSession &session,
                                   CompanionSessionContext &context) const {
  try {
    auto detached = session.leave([&context](const std::vector<VehicleSnapshot> &vehicles) {
      context.companion_capture_wagons(vehicles);
    });
    // Preserve and settle wagon state before notification delivery can fail.
    const bool settled = context.companion_settle_wagons(detached.vehicles);
    try {
      context.companion_deliver_update(detached.update);
    } catch (const std::exception &error) {
      spdlog::error("Companion exit notification failed connection={}: {}",
                    context.companion_connection_id(), error.what());
    }
    return settled;
  } catch (const std::exception &error) {
    spdlog::error("Companion world exit failed connection={}: {}",
                  context.companion_connection_id(), error.what());
    return false;
  }
}

PlayerCompanionOutcome CompanionFeature::player_mount(CompanionSession &session,
                                                      CompanionSessionContext &context) const {
  if (!context.companion_has_handoff()) {
    return {{}, "Enter the game world before using !mount."};
  }

  return session.player_mount(context.companion_epoch_ticks());
}

PlayerCompanionOutcome CompanionFeature::player_dismount(CompanionSession &session,
                                                         CompanionSessionContext &context) const {
  if (!context.companion_has_handoff()) {
    return {{}, "Enter the game world before using !dismount."};
  }

  auto outcome = session.player_dismount(context.companion_epoch_ticks());
  // Same follow-up as a client GetOffUnitReq: leaving a trade wagon updates the trade run.
  context.companion_trade_dismounted(outcome.result);
  return outcome;
}

} // namespace chronicles::companion
