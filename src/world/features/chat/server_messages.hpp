#pragma once

#include "auth/auth.hpp"

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace chronicles {

enum class ServerMessageType { chat, announcement };

struct ServerMessage {
  ServerMessageType type{ServerMessageType::chat};
  std::string text;
};

struct ServerMessagesConfig {
  bool enabled{};
  std::string sender_name{"[Server]"};
  std::chrono::seconds interval{300};
  std::vector<ServerMessage> messages;
  bool welcome_enabled{};
  std::vector<std::string> welcome_messages;
};

ServerMessagesConfig parse_server_messages_config(const nlohmann::json &document);

ServerMessagesConfig load_server_messages_config(const std::string &path);

class ServerMessages {
public:
  using Broadcast = std::function<void(const std::string &sender, const ServerMessage &message)>;
  using PrivateWelcome = std::function<void(const ChatIdentity &recipient,
                                            const std::string &sender, const std::string &message)>;

  ServerMessages(ServerMessagesConfig config, Broadcast broadcast, PrivateWelcome welcome,
                 Clock::time_point started = Clock::now());

  // Call from the server timer. Missed intervals produce one message, never a catch-up burst.
  void tick(Clock::time_point now);

  // The world session owns entry de-duplication. Call once after map-load completion, rather
  // than at character selection or for every map change.
  void world_entered(const ChatIdentity &recipient) const;

  // Validates before taking effect. Runtime replacement restarts the interval and rotation.
  void replace_configuration(const nlohmann::json &document, Clock::time_point now = Clock::now());

  nlohmann::json configuration() const;

private:
  ServerMessagesConfig config_;
  Broadcast broadcast_;
  PrivateWelcome welcome_;
  mutable std::mutex mutex_;
  Clock::time_point next_message_;
  std::size_t next_index_{};
};

} // namespace chronicles
