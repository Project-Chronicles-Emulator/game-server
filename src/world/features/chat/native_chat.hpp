#pragma once

#include "auth/auth.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace chronicles {

// These are bounded emulator policies, not recovered official service limits.
inline constexpr std::size_t kMaximumChatTextBytes = 1024;
// Every in-world client keeps one front WebSocket registered here for the whole session,
// so this bounds concurrent players. It covers the 500-player realm target with headroom
// for reconnect overlap; that capacity has not been load tested.
inline constexpr std::size_t kMaximumChatConnections = 1024;
inline constexpr std::size_t kMaximumQueuedChatMessages = 64;
inline constexpr std::size_t kMaximumQueuedChatBytes = 64 * 1024;
inline constexpr unsigned kMaximumChatMessagesPerWindow = 10;
inline constexpr auto kChatRateWindow = std::chrono::seconds(10);
// Recent World-channel messages kept in memory for the operator console; never persisted.
inline constexpr std::size_t kWorldChatHistoryCapacity = 1000;
inline constexpr std::size_t kMaximumWorldChatHistoryPage = 500;

void validate_chat_text(const std::string &message);

// One delivered World-channel message, as the operator console shows it.
struct WorldChatRecord {
  // Increases by one per message for the process lifetime; the console polls after it.
  std::uint64_t sequence{};
  std::int64_t unix_milliseconds{};
  int world_id{};
  // Zero for an operator notice.
  std::int64_t character_id{};
  std::string sender_name;
  std::string message;
  bool operator_notice{};
};

struct WorldChatHistoryPage {
  std::vector<WorldChatRecord> records;
  std::uint64_t latest_sequence{};
  // True when messages after the requested sequence were evicted, or the cursor came from an
  // earlier server process.
  bool gap{};
};

class NativeChatService {
public:
  // Resolves the characters that should receive a group-channel message from `sender`.
  using ChatAudience = std::function<std::vector<std::int64_t>(const ChatIdentity &sender)>;

  explicit NativeChatService(AuthService &auth,
                             std::function<Clock::time_point()> now = Clock::now);

  std::uint64_t connect(const std::string &token);

  void disconnect(std::uint64_t connection_id);

  // Drain rechecks authentication and selected-character ownership. A revoked token or
  // an overflowing recipient closes only that recipient's front connection.
  std::vector<std::string> drain(std::uint64_t connection_id);

  void world_chat(const std::string &token, const x5::pb::ws::front::ChatReq &request);

  // Sends ChatNoti to the connected characters `audience` returns for the authenticated
  // sender, including the sender's own echo. The caller resolves the audience from
  // authoritative membership (for example the sender's guild). Returns false, sending
  // nothing, when the audience is empty.
  bool group_chat(const std::string &token, const x5::pb::ws::front::ChatReq &request,
                  const ChatAudience &audience);

  // Queues one encoded front frame for every front connection of the character. Returns
  // false when the character has no connected front socket on this realm.
  bool push_to_character(int world_id, std::int64_t character_id, const std::string &frame);

  x5::pb::frontwebsocket::DirectMessageRes
  direct_message(const std::string &token, const x5::pb::ws::front::DirectMessageReq &request);

  // Operator console notice: one World-channel ChatNoti carrying `sender_name` (for example
  // "[Server]") to every character on the realm's front connections. No player rate limit
  // applies; text uses the normal chat validation. Returns the number of queued recipients.
  std::size_t broadcast_notice(int world_id, const std::string &sender_name,
                               const std::string &message);

  // Server replies and welcome messages use the selected character's front connections only.
  // These notices never enter public chat history or consume the player message rate limit.
  bool private_notice(int world_id, std::int64_t character_id, const std::string &sender_name,
                      const std::string &message);

  // Operator console only: World messages with sequence > after_sequence, oldest first.
  WorldChatHistoryPage world_chat_history(std::uint64_t after_sequence, std::size_t limit) const;

private:
  void record_world_chat(int world_id, std::int64_t character_id, const std::string &sender_name,
                         const std::string &message, bool operator_notice);

  struct QueuedMessage {
    std::int64_t recipient_character_id{};
    std::string frame;
  };

  struct Connection {
    std::string token;
    std::deque<QueuedMessage> pending;
    std::size_t pending_bytes{};
    bool overflowed{};
  };

  struct Recipient {
    std::uint64_t connection_id{};
    ChatIdentity identity;
  };

  struct RateWindow {
    Clock::time_point started;
    unsigned messages{};
  };

  ChatIdentity sender(const std::string &token, const std::string &message);

  std::vector<Recipient> recipients(int world_id);

  bool enqueue(const Recipient &recipient, const std::string &frame);

  AuthService &auth_;
  std::function<Clock::time_point()> now_;
  std::mutex mutex_;
  std::uint64_t next_connection_id_{};
  std::map<std::uint64_t, Connection> connections_;
  std::map<std::pair<int, std::int64_t>, RateWindow> rate_windows_;
  // Leaf lock for the console history; never held while taking mutex_ or calling auth_.
  mutable std::mutex history_mutex_;
  std::deque<WorldChatRecord> world_history_;
  std::uint64_t next_history_sequence_{1};
};

} // namespace chronicles
