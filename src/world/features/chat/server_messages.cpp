#include "world/features/chat/server_messages.hpp"

#include "world/features/chat/native_chat.hpp"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>

namespace chronicles {
namespace {

constexpr std::size_t kMaximumConfiguredMessages = 100;
constexpr std::size_t kMaximumWelcomeMessages = 8;
constexpr auto kMaximumMessageInterval = std::chrono::hours(24 * 7);

void validate_config(const ServerMessagesConfig &config) {
  validate_chat_text(config.sender_name);
  if (config.sender_name.size() > 32) {
    throw std::invalid_argument("Server message sender_name exceeds 32 bytes");
  }
  if (config.interval < std::chrono::seconds(1) || config.interval > kMaximumMessageInterval) {
    throw std::invalid_argument("Server message interval_seconds must be 1-604800");
  }
  if (config.messages.size() > kMaximumConfiguredMessages ||
      (config.enabled && config.messages.empty())) {
    throw std::invalid_argument("Enabled server messages require 1-100 messages");
  }
  for (const auto &message : config.messages) {
    validate_chat_text(message.text);
    if (message.type != ServerMessageType::chat &&
        message.type != ServerMessageType::announcement) {
      throw std::invalid_argument("Unknown server message type");
    }
  }
  if (config.welcome_messages.size() > kMaximumWelcomeMessages ||
      (config.welcome_enabled && config.welcome_messages.empty())) {
    throw std::invalid_argument("Enabled welcome requires 1-8 messages");
  }
  for (const auto &message : config.welcome_messages) {
    validate_chat_text(message);
  }
}

void require_fields(const nlohmann::json &document,
                    const std::vector<std::string> &allowed_fields) {
  if (!document.is_object()) {
    throw std::invalid_argument("Server message configuration must contain JSON objects");
  }
  for (const auto &[key, value] : document.items()) {
    if (std::ranges::find(allowed_fields, key) == allowed_fields.end()) {
      throw std::invalid_argument("Unknown server message configuration field: " + key);
    }
  }
}

} // namespace

ServerMessagesConfig parse_server_messages_config(const nlohmann::json &document) {
  require_fields(document, {"enabled", "sender_name", "interval_seconds", "messages", "welcome"});

  ServerMessagesConfig config;
  config.enabled = document.value("enabled", false);
  config.sender_name = document.value("sender_name", std::string("[Server]"));
  if (document.contains("interval_seconds")) {
    const auto &interval = document.at("interval_seconds");
    if (!interval.is_number_integer() || interval < 1 || interval > 604800) {
      throw std::invalid_argument("Server message interval_seconds must be an integer 1-604800");
    }
    config.interval = std::chrono::seconds(interval.get<int>());
  }
  if (document.contains("messages")) {
    const auto &messages = document.at("messages");
    if (!messages.is_array() || messages.size() > kMaximumConfiguredMessages) {
      throw std::invalid_argument("Server messages must be an array of at most 100 entries");
    }
    for (const auto &message : messages) {
      require_fields(message, {"type", "text"});
      const auto type = message.at("type").get<std::string>();
      if (type != "chat" && type != "announcement") {
        throw std::invalid_argument("Server message type must be chat or announcement");
      }
      config.messages.push_back(
          {type == "chat" ? ServerMessageType::chat : ServerMessageType::announcement,
           message.at("text").get<std::string>()});
    }
  }
  if (document.contains("welcome")) {
    const auto &welcome = document.at("welcome");
    require_fields(welcome, {"enabled", "messages"});
    config.welcome_enabled = welcome.value("enabled", false);
    if (welcome.contains("messages")) {
      const auto &messages = welcome.at("messages");
      if (!messages.is_array() || messages.size() > kMaximumWelcomeMessages) {
        throw std::invalid_argument("Welcome messages must be an array of at most 8 entries");
      }
      config.welcome_messages = messages.get<std::vector<std::string>>();
    }
  }
  validate_config(config);
  return config;
}

ServerMessagesConfig load_server_messages_config(const std::string &path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    throw std::runtime_error("Cannot open server message configuration: " + path);
  }
  constexpr std::streamoff kMaximumConfigBytes = 256 * 1024;
  const auto size = file.tellg();
  if (size < 0 || size > kMaximumConfigBytes) {
    throw std::invalid_argument("Server message configuration exceeds 256 KiB");
  }
  file.seekg(0);
  try {
    const auto document = nlohmann::json::parse(file);
    return parse_server_messages_config(document);
  } catch (const nlohmann::json::exception &error) {
    throw std::invalid_argument("Invalid server message configuration: " +
                                std::string(error.what()));
  }
}

ServerMessages::ServerMessages(ServerMessagesConfig config, Broadcast broadcast,
                               PrivateWelcome welcome, Clock::time_point started)
    : config_(std::move(config)), broadcast_(std::move(broadcast)), welcome_(std::move(welcome)),
      next_message_(started) {
  validate_config(config_);
  if ((config_.enabled && !broadcast_) || (config_.welcome_enabled && !welcome_)) {
    throw std::invalid_argument("Enabled server messages require delivery callbacks");
  }
  next_message_ = started + config_.interval;
}

void ServerMessages::tick(Clock::time_point now) {
  ServerMessage message;
  std::string sender;
  {
    std::lock_guard lock(mutex_);
    if (!config_.enabled || now < next_message_) {
      return;
    }
    message = config_.messages[next_index_];
    sender = config_.sender_name;
    next_index_ = (next_index_ + 1) % config_.messages.size();
    next_message_ = now + config_.interval;
  }
  broadcast_(sender, message);
}

void ServerMessages::world_entered(const ChatIdentity &recipient) const {
  std::vector<std::string> messages;
  std::string sender;
  {
    std::lock_guard lock(mutex_);
    if (!config_.welcome_enabled) {
      return;
    }
    messages = config_.welcome_messages;
    sender = config_.sender_name;
  }
  if (recipient.world_id <= 0 || recipient.character_id <= 0) {
    throw Unauthorized();
  }
  for (const auto &message : messages) {
    welcome_(recipient, sender, message);
  }
}

void ServerMessages::replace_configuration(const nlohmann::json &document, Clock::time_point now) {
  auto replacement = parse_server_messages_config(document);
  if ((replacement.enabled && !broadcast_) || (replacement.welcome_enabled && !welcome_)) {
    throw std::invalid_argument("Enabled server messages require delivery callbacks");
  }

  std::lock_guard lock(mutex_);
  config_ = std::move(replacement);
  next_index_ = 0;
  next_message_ = now + config_.interval;
}

nlohmann::json ServerMessages::configuration() const {
  std::lock_guard lock(mutex_);
  nlohmann::json messages = nlohmann::json::array();
  for (const auto &message : config_.messages) {
    messages.push_back({{"type", message.type == ServerMessageType::chat ? "chat" : "announcement"},
                        {"text", message.text}});
  }
  return {
      {"enabled", config_.enabled},
      {"sender_name", config_.sender_name},
      {"interval_seconds", config_.interval.count()},
      {"messages", std::move(messages)},
      {"welcome", {{"enabled", config_.welcome_enabled}, {"messages", config_.welcome_messages}}}};
}

} // namespace chronicles
