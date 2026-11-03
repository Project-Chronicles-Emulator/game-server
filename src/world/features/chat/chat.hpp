#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <vector>

namespace chronicles {

// Server-authorized world chat delivery for the native world transport.
//
// Evidence (client build 40367e24241c9481bfc07fbf9ef1919f25fb0eaa, static
// analysis recorded in protocol/native-chat-evidence.json and docs/native-chat.md):
// - Candidate: PC2S_SendChatReq (opcode 20038), schema x5.pb.SendChatReq
//   {type: x5.cms.EX5ChatMessageType, message: string}. Observed statically in
//   AAC_Chat_SendByChannelTransport (0x145E79A40) only for type 5 Party on the
//   world transport; never captured on the wire (protocol/demo-capture-observations.json
//   contains zero chat frames).
// - Candidate: PS2C_SendChatNoti (opcode 75), schema x5.pb.SendChatNoti
//   {type, message, optional sender_name, optional sender_unit_id,
//   optional display_secs}. Never captured on the wire.
// - Observed statically: EX5ChatMessageType Local=1 World=2 Company=3 Guild=4
//   Party=5 Whisper=6; types 1/2/3/4/6 route over the front WebSocket instead
//   (WSC2F_ChatReq 20008 / WSC2F_DirectMessageReq 20009).
// This module implements server-authoritative delivery for the local, world
// and party channels. Whether the official server accepts Local/World chat on
// the world transport is unverified; treating the world request as authorized
// for those channels is an emulator policy choice made because per-character
// instanced tutorial maps are the only live audiences today.

// Bounded emulator policies, not recovered official limits. They mirror the
// front WebSocket chat service in src/world/features/chat/native_chat.cpp so the world path is not
// weaker than the front path.
inline constexpr std::size_t kWorldChatMaximumMessageBytes = 1024;
inline constexpr unsigned kWorldChatMaximumMessagesPerWindow = 10;
inline constexpr auto kWorldChatRateWindow = std::chrono::seconds(10);
inline constexpr std::size_t kWorldChatMaximumTrackedSenders = 128;

// PS2C_SendChatNoti packet ID; integrators encode deliveries under this opcode.
inline constexpr std::uint16_t kWorldChatSendChatNotificationOpcode = 75;

using WorldChatClock = std::chrono::steady_clock;

// Identity of the sender as established by the authenticated world session and
// its selected character. Chat request payloads have no sender fields; callers
// must fill this from server-side session state, never from client input.
struct WorldChatIdentity {
  // World connection the request arrived on.
  std::uint64_t connection_id{};
  // Persistent character ID for sender_name and rate tracking.
  std::int64_t character_id{};
  // Session unit ID for the SendChatNoti sender_unit_id field.
  std::uint64_t sender_unit_id{};
  // Selected character name for the SendChatNoti sender_name field.
  std::string character_name;
};

enum class WorldChatStatus {
  delivered,
  no_recipients,
  malformed,
  unsupported_channel,
  empty_message,
  message_too_long,
  invalid_utf8,
  control_character,
  rate_limited,
};

// One decoded PS2C_SendChatNoti payload addressed to a world connection.
// The integrator encodes `message` with Protocol::encode under
// kWorldChatSendChatNotificationOpcode and writes it on the recipient session.
struct WorldChatDelivery {
  std::uint64_t recipient_connection_id{};
  nlohmann::json message;
};

struct WorldChatResult {
  WorldChatStatus status{WorldChatStatus::malformed};
  std::vector<WorldChatDelivery> deliveries;
};

// Explicit interest rule supplied by the integrator per request. It returns the
// world connection IDs that should receive the notification for `chat_type`
// (an EX5ChatMessageType value). It must include sender.connection_id when a
// sender echo is wanted. Because today's maps are per-character instances, this
// rule is the only audience authority; the service does not assume a shared
// open world. The rule runs outside the service lock and must not re-enter it.
using WorldChatAudience =
    std::function<std::vector<std::uint64_t>(const WorldChatIdentity &sender, int chat_type)>;

// Validates, rate-limits and fans out one PC2S_SendChatReq payload. Thread-safe.
// The service never logs or persists chat text, sender names or payloads.
class WorldChatService {
public:
  explicit WorldChatService(std::function<WorldChatClock::time_point()> now = WorldChatClock::now);

  // `request_message` is the decoded x5.pb.SendChatReq JSON body (fields
  // "type" and "message"); the integrator's Protocol::decode output. Enum
  // fields arrive as integer or enum-name string depending on the decoder.
  // Rate limiting runs before any field validation: every call consumes one
  // allowance, so malformed floods are throttled too. A throttled call
  // reports rate_limited regardless of its fields.
  WorldChatResult receive(const WorldChatIdentity &sender,
                          const nlohmann::json &request_message,
                          const WorldChatAudience &audience);

private:
  struct RateWindow {
    WorldChatClock::time_point started;
    unsigned messages{};
  };

  // Rate windows are keyed by persistent character ID, not connection: a
  // character owns at most one world session, so per-character keying is the
  // intended scope and survives reconnects. Every receive() call consumes one
  // allowance before field validation, so unparseable floods cannot bypass it.
  // When the bounded sender table is full, the oldest-started window is
  // evicted so new senders are not starved permanently.

  bool consume_rate_allowance(std::int64_t character_id);

  std::function<WorldChatClock::time_point()> now_;
  std::mutex mutex_;
  std::map<std::int64_t, RateWindow> rate_windows_;
};

} // namespace chronicles
