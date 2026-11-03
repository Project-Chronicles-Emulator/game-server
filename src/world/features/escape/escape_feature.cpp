#include "world/features/escape/escape_feature.hpp"

#include "protocol/protocol.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

#include <boost/asio/post.hpp>
#include <spdlog/spdlog.h>

namespace chronicles::escape {

EscapeFeature::EscapeFeature(EscapeRules rules) : rules_(rules) {
}

void EscapeFeature::load_catalog(ClientCatalog &catalog) {
  rules_ = EscapeRules::load(catalog);
}

std::unique_ptr<EscapeSession>
EscapeFeature::create_session(EscapeSessionContext &context,
                              boost::asio::any_io_executor timer_executor) {
  return std::make_unique<EscapeSession>(*this, context, std::move(timer_executor));
}

EscapeSession::EscapeSession(EscapeFeature &feature, EscapeSessionContext &context,
                             boost::asio::any_io_executor timer_executor)
    : feature_(feature), context_(context), timer_(std::move(timer_executor)) {
}

PlayerEscape &EscapeSession::player() {
  if (!player_) {
    player_.emplace(feature_.rules_, context_.escape_player_unit_id());
  }
  return *player_;
}

bool EscapeSession::handles(const WorldGameplayRequest &request) {
  return context_.escape_has_character() && player().handles(request.opcode, request.message);
}

bool EscapeSession::handle(FrameContext &frame) {
  const auto *request = frame.request();
  if (request && handles(*request)) {
    frame.session_packets = receive(*request, frame.party_position);
    return true;
  }

  return false;
}

bool EscapeSession::teleport_pending() const {
  return player_ && player_->teleport_pending();
}

bool EscapeSession::server_teleport_pending() {
  return player().teleport_pending();
}

EscapeOutcome EscapeSession::request_server_teleport(const WorldPosition &destination,
                                                     std::int64_t now) {
  return player().request_server_teleport(destination, now);
}

std::vector<GameplayPacket> EscapeSession::receive(const WorldGameplayRequest &request,
                                                   const std::optional<WorldPosition> &position) {
  const auto blockers = context_.escape_blockers();
  EscapeContext conditions;
  conditions.position = position;
  conditions.dead = blockers.dead;
  conditions.busy = blockers.busy;
  conditions.riding = blockers.riding;

  auto &escape = player();
  const bool was_pending = escape.teleport_pending();
  auto outcome =
      escape.receive(request.opcode, request.message, conditions, context_.escape_now_ticks());
  if (outcome.refusal != EscapeRefusal::none) {
    spdlog::info("Native world escape refused connection={} reason={}",
                 context_.escape_connection_id(), escape_refusal_name(outcome.refusal));
  } else if (!was_pending && escape.teleport_pending()) {
    spdlog::info("Native world escape teleport started connection={}",
                 context_.escape_connection_id());
  }

  if (outcome.relocated_to && !context_.finish_escape_revival(outcome.packets)) {
    return {{24, {{"ec", "BadRequest"}}},
            context_.escape_invincibility_packet(context_.escape_god_mode())};
  }
  if (outcome.relocated_to) {
    context_.adopt_escape_position(*outcome.relocated_to);
  }
  if (outcome.relocated_to && context_.escape_god_mode()) {
    outcome.packets.push_back(context_.escape_invincibility_packet(true));
  }

  arm_expiry(escape.next_expiry_tick());
  return std::move(outcome.packets);
}

void EscapeSession::arm_expiry(std::optional<std::int64_t> deadline) {
  boost::asio::post(context_.escape_io_executor(), [this, owner = context_.retain_escape_owner(),
                                                    deadline] {
    if (context_.escape_closed() || deadline == expiry_tick_) {
      return;
    }

    timer_.cancel();
    expiry_tick_ = deadline;
    if (!deadline) {
      return;
    }

    const auto remaining_ticks = std::max<std::int64_t>(0, *deadline - context_.escape_now_ticks());
    timer_.expires_after(std::chrono::nanoseconds(remaining_ticks * 100));
    timer_.async_wait([this, owner](boost::system::error_code error) {
      if (error || context_.escape_closed()) {
        return;
      }
      expiry_tick_.reset();
      resolve_expiry(owner);
    });
  });
}

void EscapeSession::resolve_expiry(std::shared_ptr<void> owner) {
  boost::asio::post(context_.escape_worker_executor(), [this, owner = std::move(owner)] {
    if (context_.escape_closed() || !player_) {
      return;
    }

    std::vector<Frame> frames;
    try {
      const auto packets = player_->expire(context_.escape_now_ticks());
      context_.append_encoded_escape_packets(packets, frames);
    } catch (const std::exception &error) {
      spdlog::error("Native world escape cooldown expiry failed connection={}: {}",
                    context_.escape_connection_id(), error.what());
    }

    // The worker may observe an early I/O firing. Preserve rearm before frame delivery.
    arm_expiry(player_->next_expiry_tick());
    boost::asio::post(context_.escape_io_executor(),
                      [this, owner, frames = std::move(frames)]() mutable {
                        if (context_.escape_closed()) {
                          return;
                        }
                        for (auto &frame : frames) {
                          context_.enqueue_escape_frame(std::move(frame));
                        }
                      });
  });
}

void EscapeSession::on_leave_world() {
  timer_.cancel();
}

} // namespace chronicles::escape
