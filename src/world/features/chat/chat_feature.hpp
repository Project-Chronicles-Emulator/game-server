#pragma once

#include "auth/auth.hpp"
#include "world/core/frame_context.hpp"
#include "world/features/chat/chat.hpp"
#include "world/features/party/party_world.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace chronicles::chat {

class ChatSession;
class ChatSessionContext;

// Registry queries run under the composition's existing protocol mutex. Its implementation
// retains strong session references until after unlocking and preserves registry order.
class LocalChatView {
public:
  virtual ~LocalChatView() = default;
  virtual std::optional<std::uint64_t> speaker_connection(std::int64_t character_id) = 0;
  virtual std::vector<std::uint64_t> observers(std::uint64_t connection_id) const = 0;
  virtual std::optional<std::int64_t> observer_character(std::uint64_t connection_id) = 0;
};

class ChatFeature final {
public:
  using EntryHandler = std::function<void(const ChatIdentity &)>;
  using CommandHandler = std::function<bool(const ChatIdentity &, const std::string &)>;

  explicit ChatFeature(std::function<WorldChatClock::time_point()> now = WorldChatClock::now);
  std::unique_ptr<ChatSession> create_session(ChatSessionContext &context);
  std::vector<std::int64_t> local_audience_locked(std::int64_t speaker, LocalChatView &view) const;

  // The caller holds the protocol (codec) mutex while copying or replacing a handler.
  void set_entry_handler_locked(EntryHandler handler);
  void set_exit_handler_locked(EntryHandler handler);
  void set_command_handler_locked(CommandHandler handler);
  EntryHandler entry_handler_locked() const;
  EntryHandler exit_handler_locked() const;
  CommandHandler command_handler_locked() const;

private:
  friend class ChatSession;

  WorldChatService world_chat_;
  EntryHandler entry_handler_;
  EntryHandler exit_handler_;
  CommandHandler command_handler_;
};

class ChatSessionContext {
public:
  virtual ~ChatSessionContext() = default;
  virtual const party::MemberPresence *chat_party_identity() const = 0;
  virtual std::optional<int> chat_authenticated_world() const = 0;
  virtual bool chat_has_handoff() const = 0;
  virtual ChatIdentity chat_exit_identity() const = 0;
  virtual ChatIdentity chat_entry_identity() const = 0;
  virtual std::uint64_t chat_connection_id() const = 0;
  virtual bool chat_closed() const = 0;
  virtual std::vector<std::uint64_t> chat_party_connections(std::int64_t character_id) const = 0;
  virtual ChatFeature::EntryHandler chat_entry_handler() const = 0;
  virtual ChatFeature::EntryHandler chat_exit_handler() const = 0;
  virtual ChatFeature::CommandHandler chat_command_handler() const = 0;
};

class ChatSession final {
public:
  enum class DepartureStatus { complete, pending, uncertain };

  ChatSession(ChatFeature &feature, ChatSessionContext &context);
  std::vector<GameplayPacket> receive(const WorldGameplayRequest &request,
                                      party::PartyUpdate &update);
  void on_enter_world(bool player_services_entered);
  DepartureStatus on_leave_world();
  DepartureStatus departure_status() const noexcept;

private:
  enum class EntryState { absent, entered, exiting, complete, uncertain };
  ChatFeature &feature_;
  ChatSessionContext &context_;
  std::atomic<EntryState> entry_state_{EntryState::absent};
};

} // namespace chronicles::chat
