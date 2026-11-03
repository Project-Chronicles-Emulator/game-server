#include "world/features/chat/chat.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>

namespace chronicles {
namespace {

// x5.cms.EX5ChatMessageType values from cms.generated.proto, anchored by the
// AAC_Chat_SendByChannelTransport switch at client 0x145E79A40.
constexpr int kChatTypeLocal = 1;
constexpr int kChatTypeWorld = 2;
constexpr int kChatTypeCompany = 3;
constexpr int kChatTypeGuild = 4;
constexpr int kChatTypeParty = 5;
constexpr int kChatTypeWhisper = 6;

constexpr std::string_view kChatTypePrefix = "EX5ChatMessageType_";

// Result of interpreting the request "type" field. Unknown integer values and
// unparseable fields are malformed; recognized channels outside this module's
// scope (Company, Guild, Whisper) are separately reported as unsupported so the
// integrator can distinguish a wrong transport from corrupt input.
struct ParsedChannel {
  int value{};
  bool malformed{};
};

ParsedChannel parse_chat_type(const nlohmann::json &request) {
  const auto field = request.find("type");
  if (field == request.end()) {
    return {0, true};
  }

  if (field->is_number_integer()) {
    const auto value = field->get<int>();
    switch (value) {
    case kChatTypeLocal:
    case kChatTypeWorld:
    case kChatTypeCompany:
    case kChatTypeGuild:
    case kChatTypeParty:
    case kChatTypeWhisper:
      return {value, false};
    default:
      // Enum values outside the recovered range are malformed, not merely
      // unsupported.
      return {0, true};
    }
  }

  if (field->is_string()) {
    std::string_view name = field->get_ref<const std::string &>();
    if (name.starts_with(kChatTypePrefix)) {
      name.remove_prefix(kChatTypePrefix.size());
    }
    if (name == "Local") {
      return {kChatTypeLocal, false};
    }
    if (name == "World") {
      return {kChatTypeWorld, false};
    }
    if (name == "Company") {
      return {kChatTypeCompany, false};
    }
    if (name == "Guild") {
      return {kChatTypeGuild, false};
    }
    if (name == "Party") {
      return {kChatTypeParty, false};
    }
    if (name == "Whisper") {
      return {kChatTypeWhisper, false};
    }
  }

  return {0, true};
}

// Strict RFC 3629 UTF-8. The front service delegates this to the protobuf wire
// format verifier; this module has no protobuf dependency, so it validates the
// byte stream directly, rejecting overlong encodings, surrogates and values
// above U+10FFFF.
bool valid_utf8(const std::string &text) {
  const auto *bytes = reinterpret_cast<const unsigned char *>(text.data());
  const auto size = text.size();
  std::size_t index = 0;

  while (index < size) {
    const unsigned char first = bytes[index];
    if (first < 0x80) {
      ++index;
      continue;
    }

    unsigned expected;
    std::uint32_t code_point;
    if (first >= 0xc2 && first <= 0xdf) {
      expected = 1;
      code_point = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
      expected = 2;
      code_point = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      expected = 3;
      code_point = first & 0x07;
    } else {
      return false;
    }

    if (index + expected >= size) {
      return false;
    }

    for (unsigned continuation = 1; continuation <= expected; ++continuation) {
      const unsigned char next = bytes[index + continuation];
      if ((next & 0xc0) != 0x80) {
        return false;
      }
      code_point = (code_point << 6) | (next & 0x3f);
    }

    // Overlong encodings, UTF-16 surrogates and values beyond U+10FFFF.
    const std::uint32_t minimum = expected == 1 ? 0x80 : expected == 2 ? 0x800 : 0x10000;
    if (code_point < minimum || code_point > 0x10ffff ||
        (code_point >= 0xd800 && code_point <= 0xdfff)) {
      return false;
    }

    index += expected + 1;
  }

  return true;
}

bool has_control_character(const std::string &text) {
  return std::any_of(text.begin(), text.end(),
                     [](char character) {
                       const auto byte = static_cast<unsigned char>(character);
                       return byte < 0x20 || byte == 0x7f;
                     });
}

const char *chat_type_enum_name(int chat_type) {
  switch (chat_type) {
  case kChatTypeLocal:
    return "EX5ChatMessageType_Local";
  case kChatTypeWorld:
    return "EX5ChatMessageType_World";
  case kChatTypeParty:
    return "EX5ChatMessageType_Party";
  default:
    return "EX5ChatMessageType_None";
  }
}

} // namespace

WorldChatService::WorldChatService(std::function<WorldChatClock::time_point()> now)
    : now_(std::move(now)) {}

bool WorldChatService::consume_rate_allowance(std::int64_t character_id) {
  std::lock_guard lock(mutex_);
  const auto now = now_();
  std::erase_if(rate_windows_,
                [&](const auto &entry) { return now - entry.second.started >= kWorldChatRateWindow; });

  if (!rate_windows_.contains(character_id) &&
      rate_windows_.size() >= kWorldChatMaximumTrackedSenders) {
    // Sender table is full: evict the oldest-started window rather than
    // starving new senders outright. This is an approximation of idle-sender
    // eviction; an evicted active sender simply starts a fresh window.
    const auto oldest = std::min_element(
        rate_windows_.begin(), rate_windows_.end(),
        [](const auto &left, const auto &right) { return left.second.started < right.second.started; });
    rate_windows_.erase(oldest);
  }

  auto [entry, inserted] = rate_windows_.try_emplace(character_id, RateWindow{now});
  if (entry->second.messages >= kWorldChatMaximumMessagesPerWindow) {
    return false;
  }

  ++entry->second.messages;
  return true;
}

WorldChatResult WorldChatService::receive(const WorldChatIdentity &sender,
                                          const nlohmann::json &request_message,
                                          const WorldChatAudience &audience) {
  if (!audience) {
    return {WorldChatStatus::malformed, {}};
  }

  // Every request consumes one rate allowance, including requests that are
  // later rejected as malformed or unsupported. Otherwise a flood of
  // unparseable frames would bypass the limiter entirely.
  if (!consume_rate_allowance(sender.character_id)) {
    return {WorldChatStatus::rate_limited, {}};
  }

  if (!request_message.is_object()) {
    return {WorldChatStatus::malformed, {}};
  }

  const auto channel = parse_chat_type(request_message);
  if (channel.malformed) {
    return {WorldChatStatus::malformed, {}};
  }
  if (channel.value != kChatTypeLocal && channel.value != kChatTypeWorld &&
      channel.value != kChatTypeParty) {
    // Company, Guild and Whisper need membership/whisper services that do not
    // exist yet; refuse instead of broadcasting to the wrong audience.
    return {WorldChatStatus::unsupported_channel, {}};
  }

  const auto message_field = request_message.find("message");
  if (message_field == request_message.end() || !message_field->is_string()) {
    return {WorldChatStatus::malformed, {}};
  }
  const std::string &text = message_field->get_ref<const std::string &>();
  if (text.empty()) {
    return {WorldChatStatus::empty_message, {}};
  }
  if (text.size() > kWorldChatMaximumMessageBytes) {
    return {WorldChatStatus::message_too_long, {}};
  }
  if (!valid_utf8(text)) {
    return {WorldChatStatus::invalid_utf8, {}};
  }
  if (has_control_character(text)) {
    return {WorldChatStatus::control_character, {}};
  }

  // Explicit interest rule; invoked outside the lock. The rule decides whether
  // the sender's own connection receives an echo.
  auto recipients = audience(sender, channel.value);
  std::sort(recipients.begin(), recipients.end());
  recipients.erase(std::unique(recipients.begin(), recipients.end()), recipients.end());
  if (recipients.empty()) {
    return {WorldChatStatus::no_recipients, {}};
  }

  // x5.pb.SendChatNoti. sender_name/sender_unit_id come only from the session
  // identity; display_secs stays unset because no official usage is captured.
  nlohmann::json notification{{"type", chat_type_enum_name(channel.value)},
                              {"message", text},
                              {"sender_name", sender.character_name},
                              {"sender_unit_id", std::to_string(sender.sender_unit_id)}};

  WorldChatResult result{WorldChatStatus::delivered, {}};
  result.deliveries.reserve(recipients.size());
  for (const auto connection_id : recipients) {
    result.deliveries.push_back({connection_id, notification});
  }
  return result;
}

} // namespace chronicles
