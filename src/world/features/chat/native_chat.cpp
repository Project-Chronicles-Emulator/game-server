#include "world/features/chat/native_chat.hpp"

#include "net/native_ws_frames.hpp"

#include <algorithm>
#include <chrono>
#include <google/protobuf/wire_format_lite.h>
#include <stdexcept>
#include <utility>

namespace chronicles {

void validate_chat_text(const std::string &message) {
  if (message.empty() || message.size() > kMaximumChatTextBytes) {
    throw std::invalid_argument("Chat text is outside the accepted byte length");
  }

  // Protobuf string fields enforce UTF-8 on the wire. Direct callers must pass the same check.
  using WireFormat = google::protobuf::internal::WireFormatLite;
  if (!WireFormat::VerifyUtf8String(message.data(), static_cast<int>(message.size()),
                                    WireFormat::PARSE, "chat.message")) {
    throw std::invalid_argument("Chat text must be valid UTF-8");
  }
  if (std::ranges::any_of(
          message, [](unsigned char character) { return character < 0x20 || character == 0x7f; })) {
    throw std::invalid_argument("Chat text contains a control character");
  }
}

NativeChatService::NativeChatService(AuthService &auth, std::function<Clock::time_point()> now)
    : auth_(auth), now_(std::move(now)) {}

std::uint64_t NativeChatService::connect(const std::string &token) {
  if (!auth_.authenticated(token)) {
    throw Unauthorized();
  }

  std::lock_guard lock(mutex_);
  if (connections_.size() >= kMaximumChatConnections) {
    throw std::length_error("Front chat connection limit reached");
  }
  const auto connection_id = ++next_connection_id_;
  connections_.emplace(connection_id, Connection{token});
  return connection_id;
}

void NativeChatService::disconnect(std::uint64_t connection_id) {
  std::lock_guard lock(mutex_);
  connections_.erase(connection_id);
}

ChatIdentity NativeChatService::sender(const std::string &token, const std::string &message) {
  validate_chat_text(message);
  const auto identity = auth_.chat_identity(token);
  if (!identity) {
    throw Unauthorized();
  }

  std::lock_guard lock(mutex_);
  const bool connected = std::ranges::any_of(
      connections_, [&](const auto &entry) { return entry.second.token == token; });
  if (!connected) {
    throw Unauthorized();
  }

  const auto now = now_();
  std::erase_if(rate_windows_,
                [&](const auto &entry) { return now - entry.second.started >= kChatRateWindow; });
  const auto key = std::make_pair(identity->world_id, identity->character_id);
  if (!rate_windows_.contains(key) && rate_windows_.size() >= kMaximumChatConnections) {
    throw std::length_error("Chat rate tracking limit reached");
  }
  auto [window, inserted] = rate_windows_.try_emplace(key, RateWindow{now});
  if (window->second.messages >= kMaximumChatMessagesPerWindow) {
    throw std::length_error("Chat message rate exceeded");
  }
  ++window->second.messages;
  return *identity;
}

std::vector<NativeChatService::Recipient> NativeChatService::recipients(int world_id) {
  std::vector<std::pair<std::uint64_t, std::string>> sessions;
  {
    std::lock_guard lock(mutex_);
    for (const auto &[connection_id, connection] : connections_) {
      sessions.emplace_back(connection_id, connection.token);
    }
  }

  std::vector<Recipient> result;
  for (const auto &[connection_id, token] : sessions) {
    try {
      const auto identity = auth_.chat_identity(token);
      if (identity && identity->world_id == world_id) {
        result.push_back({connection_id, *identity});
      }
    } catch (const Unauthorized &) {
      // Expiry/revocation is rechecked by the recipient's next drain as well.
    }
  }
  return result;
}

bool NativeChatService::enqueue(const Recipient &recipient, const std::string &frame) {
  std::lock_guard lock(mutex_);
  const auto found = connections_.find(recipient.connection_id);
  if (found == connections_.end()) {
    return false;
  }

  auto &connection = found->second;
  if (connection.overflowed) {
    return false;
  }
  if (connection.pending.size() >= kMaximumQueuedChatMessages ||
      frame.size() > kMaximumQueuedChatBytes - connection.pending_bytes) {
    connection.overflowed = true;
    return false;
  }
  connection.pending.push_back({recipient.identity.character_id, frame});
  connection.pending_bytes += frame.size();
  return true;
}

std::vector<std::string> NativeChatService::drain(std::uint64_t connection_id) {
  std::string token;
  {
    std::lock_guard lock(mutex_);
    const auto found = connections_.find(connection_id);
    if (found == connections_.end()) {
      return {};
    }
    token = found->second.token;
  }
  if (!auth_.authenticated(token)) {
    throw Unauthorized();
  }
  std::optional<ChatIdentity> identity;
  try {
    identity = auth_.chat_identity(token);
  } catch (const Unauthorized &) {
    // A valid front socket can exist before realm entry and character selection.
  }

  std::lock_guard lock(mutex_);
  const auto found = connections_.find(connection_id);
  if (found == connections_.end()) {
    return {};
  }
  auto &connection = found->second;
  if (connection.overflowed) {
    throw std::length_error("Chat recipient queue exceeded its limit");
  }

  std::vector<std::string> result;
  for (auto &message : connection.pending) {
    if (identity && identity->character_id == message.recipient_character_id) {
      result.push_back(std::move(message.frame));
    }
  }
  connection.pending.clear();
  connection.pending_bytes = 0;
  return result;
}

void NativeChatService::world_chat(const std::string &token,
                                   const x5::pb::ws::front::ChatReq &request) {
  // Local/company/guild audiences require authoritative spatial/group membership.
  // They must never fall through to a global broadcast.
  if (request.type() != x5::cms::EX5ChatMessageType_World) {
    throw std::invalid_argument("Chat channel requires an unavailable audience service");
  }
  const auto identity = sender(token, request.message());
  x5::pb::frontwebsocket::ChatNoti notification;
  notification.set_type(request.type());
  notification.set_message(request.message());
  notification.set_sender_name(identity.name);
  const auto frame = native_ws::encode_frame(native_ws::kChatNotification, notification);
  for (const auto &recipient : recipients(identity.world_id)) {
    enqueue(recipient, frame);
  }
  record_world_chat(identity.world_id, identity.character_id, identity.name, request.message(),
                    false);
}

void NativeChatService::record_world_chat(int world_id, std::int64_t character_id,
                                          const std::string &sender_name,
                                          const std::string &message, bool operator_notice) {
  const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();

  WorldChatRecord record;
  record.unix_milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count();
  record.world_id = world_id;
  record.character_id = character_id;
  record.sender_name = sender_name;
  record.message = message;
  record.operator_notice = operator_notice;

  std::lock_guard lock(history_mutex_);
  record.sequence = next_history_sequence_++;
  world_history_.push_back(std::move(record));
  if (world_history_.size() > kWorldChatHistoryCapacity) {
    world_history_.pop_front();
  }
}

WorldChatHistoryPage NativeChatService::world_chat_history(std::uint64_t after_sequence,
                                                           std::size_t limit) const {
  const auto page_limit = std::clamp<std::size_t>(limit, 1, kMaximumWorldChatHistoryPage);

  std::lock_guard lock(history_mutex_);
  WorldChatHistoryPage page;
  if (world_history_.empty()) {
    return page;
  }

  page.latest_sequence = world_history_.back().sequence;
  const auto oldest_sequence = world_history_.front().sequence;
  // A cursor beyond the newest record came from an earlier server process.
  const bool stale_cursor = after_sequence > page.latest_sequence;
  if (stale_cursor) {
    after_sequence = 0;
  }
  page.gap = stale_cursor || after_sequence + 1 < oldest_sequence;

  // Sequences are contiguous inside the ring, so the first wanted record is found by offset.
  const auto first_wanted = std::max(after_sequence + 1, oldest_sequence);
  for (auto index = static_cast<std::size_t>(first_wanted - oldest_sequence);
       index < world_history_.size() && page.records.size() < page_limit; ++index) {
    page.records.push_back(world_history_[index]);
  }
  return page;
}

std::size_t NativeChatService::broadcast_notice(int world_id, const std::string &sender_name,
                                                const std::string &message) {
  validate_chat_text(message);
  // The sender label is displayed where a character name normally appears.
  constexpr std::size_t kMaximumSenderNameBytes = 32;
  if (sender_name.empty() || sender_name.size() > kMaximumSenderNameBytes) {
    throw std::invalid_argument("Notice sender name must be 1-32 bytes");
  }
  validate_chat_text(sender_name);

  x5::pb::frontwebsocket::ChatNoti notification;
  notification.set_type(x5::cms::EX5ChatMessageType_World);
  notification.set_message(message);
  notification.set_sender_name(sender_name);
  const auto frame = native_ws::encode_frame(native_ws::kChatNotification, notification);

  std::size_t delivered = 0;
  for (const auto &recipient : recipients(world_id)) {
    if (enqueue(recipient, frame)) {
      ++delivered;
    }
  }
  record_world_chat(world_id, 0, sender_name, message, true);
  return delivered;
}

bool NativeChatService::private_notice(int world_id, std::int64_t character_id,
                                       const std::string &sender_name,
                                       const std::string &message) {
  validate_chat_text(message);
  validate_chat_text(sender_name);
  if (world_id <= 0 || character_id <= 0 || sender_name.size() > 32) {
    throw std::invalid_argument("Invalid private server notice destination or sender");
  }

  x5::pb::frontwebsocket::ChatNoti notification;
  notification.set_type(x5::cms::EX5ChatMessageType_World);
  notification.set_message(message);
  notification.set_sender_name(sender_name);
  return push_to_character(world_id, character_id,
                           native_ws::encode_frame(native_ws::kChatNotification, notification));
}

bool NativeChatService::group_chat(const std::string &token,
                                   const x5::pb::ws::front::ChatReq &request,
                                   const ChatAudience &audience) {
  const auto identity = sender(token, request.message());
  const auto members = audience(identity);
  if (members.empty()) {
    return false;
  }

  x5::pb::frontwebsocket::ChatNoti notification;
  notification.set_type(request.type());
  notification.set_message(request.message());
  notification.set_sender_name(identity.name);
  const auto frame = native_ws::encode_frame(native_ws::kChatNotification, notification);
  for (const auto &recipient : recipients(identity.world_id)) {
    if (std::ranges::find(members, recipient.identity.character_id) != members.end()) {
      enqueue(recipient, frame);
    }
  }
  return true;
}

bool NativeChatService::push_to_character(int world_id, std::int64_t character_id,
                                          const std::string &frame) {
  bool queued = false;
  for (const auto &recipient : recipients(world_id)) {
    if (recipient.identity.character_id == character_id && enqueue(recipient, frame)) {
      queued = true;
    }
  }
  return queued;
}

x5::pb::frontwebsocket::DirectMessageRes
NativeChatService::direct_message(const std::string &token,
                                  const x5::pb::ws::front::DirectMessageReq &request) {
  const auto identity = sender(token, request.message());
  if ((!request.has_char_id() && !request.has_char_name()) ||
      (request.has_char_id() && request.char_id() <= 0) ||
      (request.has_char_name() &&
       (request.char_name().empty() || request.char_name().size() > 16))) {
    throw std::invalid_argument("Invalid direct-message destination");
  }

  x5::pb::frontwebsocket::DirectMessageRes response;
  response.set_ec(x5::ec::DM_CharNotFound);
  response.set_message(request.message());
  if (request.has_char_name()) {
    response.set_char_name(request.char_name());
  } else {
    response.set_char_id(request.char_id());
  }

  x5::pb::frontwebsocket::DirectMessageNoti notification;
  notification.set_char_id(identity.character_id);
  notification.set_char_name(identity.name);
  notification.set_message(request.message());
  const auto frame = native_ws::encode_frame(native_ws::kDirectMessageNotification, notification);
  for (const auto &recipient : recipients(identity.world_id)) {
    const bool matches = request.has_char_id()
                             ? recipient.identity.character_id == request.char_id()
                             : recipient.identity.name == request.char_name();
    if (matches && enqueue(recipient, frame)) {
      response.set_ec(x5::ec::Success);
      response.set_char_id(recipient.identity.character_id);
      response.set_char_name(recipient.identity.name);
    }
  }
  return response;
}

} // namespace chronicles
