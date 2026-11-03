#include "world/features/chat/chat_feature.hpp"

#include <spdlog/spdlog.h>

#include <utility>

namespace chronicles::chat {
namespace {

// EX5ChatMessageType_Party; world transport's original native receive audience.
constexpr int kPartyChatType = 5;

} // namespace

ChatFeature::ChatFeature(std::function<WorldChatClock::time_point()> now)
    : world_chat_(std::move(now)) {
}

std::unique_ptr<ChatSession> ChatFeature::create_session(ChatSessionContext &context) {
  return std::make_unique<ChatSession>(*this, context);
}

std::vector<std::int64_t> ChatFeature::local_audience_locked(std::int64_t speaker,
                                                             LocalChatView &view) const {
  const auto connection = view.speaker_connection(speaker);
  if (!connection) {
    return {};
  }

  std::vector<std::int64_t> audience{speaker};
  for (const auto observer : view.observers(*connection)) {
    const auto character = view.observer_character(observer);
    if (character && *character != speaker) {
      audience.push_back(*character);
    }
  }
  return audience;
}

void ChatFeature::set_entry_handler_locked(EntryHandler handler) {
  entry_handler_ = std::move(handler);
}

void ChatFeature::set_exit_handler_locked(EntryHandler handler) {
  exit_handler_ = std::move(handler);
}

void ChatFeature::set_command_handler_locked(CommandHandler handler) {
  command_handler_ = std::move(handler);
}

ChatFeature::EntryHandler ChatFeature::entry_handler_locked() const {
  return entry_handler_;
}

ChatFeature::EntryHandler ChatFeature::exit_handler_locked() const {
  return exit_handler_;
}

ChatFeature::CommandHandler ChatFeature::command_handler_locked() const {
  return command_handler_;
}

ChatSession::ChatSession(ChatFeature &feature, ChatSessionContext &context)
    : feature_(feature), context_(context) {
}

std::vector<GameplayPacket> ChatSession::receive(const WorldGameplayRequest &request,
                                                 party::PartyUpdate &update) {
  const auto *identity = context_.chat_party_identity();
  if (!identity) {
    spdlog::warn("World chat request connection={} has no selected character",
                 context_.chat_connection_id());
    return {};
  }

  const auto command_handler = context_.chat_command_handler();
  const auto message = request.message.value("message", std::string{});
  if (command_handler && context_.chat_authenticated_world() &&
      command_handler({*context_.chat_authenticated_world(), identity->char_id, identity->name},
                      message)) {
    return {};
  }

  const WorldChatIdentity sender{context_.chat_connection_id(), identity->char_id,
                                 static_cast<std::uint64_t>(identity->char_id), identity->name};
  bool party_requested = false;
  const auto audience = [&](const WorldChatIdentity &selected, int chat_type) {
    if (chat_type != kPartyChatType) {
      return std::vector<std::uint64_t>{};
    }
    party_requested = true;
    return context_.chat_party_connections(selected.character_id);
  };

  const auto result = feature_.world_chat_.receive(sender, request.message, audience);
  if (result.status == WorldChatStatus::delivered) {
    for (const auto &delivery : result.deliveries) {
      update.packets.push_back({delivery.recipient_connection_id,
                                kWorldChatSendChatNotificationOpcode, delivery.message});
    }
    return {};
  }

  spdlog::info("World chat request connection={} not delivered: status={}",
               context_.chat_connection_id(), static_cast<int>(result.status));
  if (result.status == WorldChatStatus::no_recipients && party_requested) {
    return {{party::kCommonErrorResponse,
             party::PartyWorld::error_message(party::PartyError::kNotJoined, request.sequence)}};
  }
  return {};
}

void ChatSession::on_enter_world(bool player_services_entered) {
  const auto on_entry = context_.chat_entry_handler();
  if (on_entry && player_services_entered) {
    auto previous = entry_state_.load();
    if (previous != EntryState::absent && previous != EntryState::complete) {
      return;
    }
    try {
      on_entry(context_.chat_entry_identity());
    } catch (...) {
      // The external handler may have observed entry before it failed.
      entry_state_.store(EntryState::uncertain);
      throw;
    }
    if (!entry_state_.compare_exchange_strong(previous, EntryState::entered)) {
      // A concurrent entry or departure has made the external pairing ambiguous.
      entry_state_.store(EntryState::uncertain);
      return;
    }
    if (context_.chat_closed()) {
      on_leave_world();
    }
  }
}

// Reports one paired departure, on I/O close or after a worker's late entry callback.
ChatSession::DepartureStatus ChatSession::departure_status() const noexcept {
  switch (entry_state_.load()) {
  case EntryState::absent:
  case EntryState::complete:
    return DepartureStatus::complete;
  case EntryState::entered:
  case EntryState::exiting:
    return DepartureStatus::pending;
  case EntryState::uncertain:
    return DepartureStatus::uncertain;
  }
  return DepartureStatus::uncertain;
}

ChatSession::DepartureStatus ChatSession::on_leave_world() {
  auto expected = EntryState::entered;
  if (!entry_state_.compare_exchange_strong(expected, EntryState::exiting)) {
    return departure_status();
  }

  try {
    if (!context_.chat_has_handoff()) {
      entry_state_.store(EntryState::entered);
      return DepartureStatus::pending;
    }
    const auto on_exit = context_.chat_exit_handler();
    if (on_exit) {
      on_exit(context_.chat_exit_identity());
    }
    entry_state_.store(EntryState::complete);
    return DepartureStatus::complete;
  } catch (...) {
    entry_state_.store(EntryState::uncertain);
    try {
      spdlog::error("World exit handler failed connection={}; departure is uncertain",
                    context_.chat_connection_id());
    } catch (...) {
    }
    return DepartureStatus::uncertain;
  }
}

} // namespace chronicles::chat
