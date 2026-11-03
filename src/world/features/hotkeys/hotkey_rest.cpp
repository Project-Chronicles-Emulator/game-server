#include "world/features/hotkeys/hotkey_rest.hpp"

#include "p_c2f.rest.pb.h"

#include <spdlog/spdlog.h>

#include <charconv>
#include <limits>
#include <nlohmann/json.hpp>

namespace chronicles {
namespace {

constexpr std::string_view kFrontPrefix = "/api/front/";
constexpr std::string_view kLoadRoute = "CharHotKeyLoadReq";
constexpr std::string_view kSaveRoute = "CharHotKeySaveReq";
constexpr std::string_view kBearerPrefix = "Bearer ";
constexpr std::string_view kProtobufContentType = "application/protobuf";
// A CharHotKey is a few bytes; the bound keeps a hostile body away from the parser.
constexpr std::size_t kMaximumHotKeyBodyBytes = 256;
constexpr std::size_t kMaximumTokenBytes = 512;

struct HotKeyRoute {
  std::string_view name;
  std::int64_t character_id{};
};

NativeHttpResponse json_error(unsigned status, const char *error) {
  NativeHttpResponse response;
  response.status = status;
  response.content_type = "application/json";
  response.body = nlohmann::json{{"error", error}}.dump();
  return response;
}

std::optional<std::int64_t> positive_number(std::string_view text) {
  std::int64_t value{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      value <= 0) {
    return std::nullopt;
  }
  return value;
}

// "{prefix}/api/front/{route}/{char_id}/{n}" with an optional query string.
std::optional<HotKeyRoute> parse_route(std::string_view path) {
  path = path.substr(0, path.find('?'));
  const auto front = path.find(kFrontPrefix);
  if (front == std::string_view::npos) {
    return std::nullopt;
  }

  auto rest = path.substr(front + kFrontPrefix.size());
  const auto first = rest.find('/');
  if (first == std::string_view::npos) {
    return std::nullopt;
  }
  HotKeyRoute route;
  route.name = rest.substr(0, first);
  if (route.name != kLoadRoute && route.name != kSaveRoute) {
    return std::nullopt;
  }

  rest = rest.substr(first + 1);
  const auto second = rest.find('/');
  const auto character = positive_number(rest.substr(0, second));
  if (!character) {
    return std::nullopt;
  }
  // The trailing number (always 1 in this client) is validated but has no known meaning.
  if (second != std::string_view::npos && !positive_number(rest.substr(second + 1))) {
    return std::nullopt;
  }
  route.character_id = *character;
  return route;
}

std::optional<CharacterHotKey> hotkey_from(const x5::pb::rest::CharHotKey &message) {
  CharacterHotKey hotkey;
  hotkey.slot = static_cast<int>(message.slot());
  switch (message.value_case()) {
  case x5::pb::rest::CharHotKey::kAction:
    hotkey.kind = HotKeyKind::action;
    hotkey.value = static_cast<int>(message.action());
    break;
  case x5::pb::rest::CharHotKey::kFlightToolCmsId:
    hotkey.kind = HotKeyKind::flight_tool;
    hotkey.value = message.flight_tool_cms_id();
    break;
  case x5::pb::rest::CharHotKey::kItemCmsId:
    hotkey.kind = HotKeyKind::item;
    hotkey.value = message.item_cms_id();
    break;
  case x5::pb::rest::CharHotKey::kSummonNpcCmsId:
    hotkey.kind = HotKeyKind::summon_npc;
    hotkey.value = message.summon_npc_cms_id();
    break;
  case x5::pb::rest::CharHotKey::VALUE_NOT_SET:
    return std::nullopt;
  }
  return hotkey;
}

void write_hotkey(const CharacterHotKey &hotkey, x5::pb::rest::CharHotKey &message) {
  message.set_slot(static_cast<std::uint32_t>(hotkey.slot));
  switch (hotkey.kind) {
  case HotKeyKind::action:
    message.set_action(static_cast<x5::cms::EX5HotKeyAction>(hotkey.value));
    break;
  case HotKeyKind::flight_tool:
    message.set_flight_tool_cms_id(hotkey.value);
    break;
  case HotKeyKind::item:
    message.set_item_cms_id(hotkey.value);
    break;
  case HotKeyKind::summon_npc:
    message.set_summon_npc_cms_id(hotkey.value);
    break;
  }
}

} // namespace

HotKeyRest::HotKeyRest(AuthService &auth, HotKeyRepository &hotkeys)
    : auth_(auth), hotkeys_(hotkeys) {}

bool HotKeyRest::handles(std::string_view path) {
  return parse_route(path).has_value();
}

NativeHttpResponse HotKeyRest::handle(std::string_view method, std::string_view path,
                                      std::string_view authorization,
                                      std::string_view content_type, const std::string &body) {
  const auto route = parse_route(path);
  if (!route) {
    return json_error(404, "route_not_found");
  }
  if (!authorization.starts_with(kBearerPrefix) ||
      authorization.size() > kBearerPrefix.size() + kMaximumTokenBytes) {
    return json_error(401, "authentication_required");
  }
  const std::string token(authorization.substr(kBearerPrefix.size()));

  try {
    if (route->name == kLoadRoute) {
      if (method != "GET") {
        return json_error(405, "method_not_allowed");
      }
      return load(token, route->character_id);
    }
    if (method != "PUT") {
      return json_error(405, "method_not_allowed");
    }
    return save(token, route->character_id, content_type, body);
  } catch (const Unauthorized &) {
    return json_error(401, "authentication_required");
  } catch (const std::invalid_argument &) {
    return json_error(400, "invalid_hot_key");
  }
}

NativeHttpResponse HotKeyRest::load(const std::string &token, std::int64_t character_id) {
  // Lobby loads arrive before CharSelectReq, so any owned character is accepted.
  const auto owner = auth_.character_owner(token, character_id, false);
  if (!owner) {
    return json_error(403, "character_not_owned");
  }

  x5::pb::rest::CharHotKeys message;
  for (const auto &hotkey :
       hotkeys_.character_hotkeys(owner->account_id, character_id, owner->world_id)) {
    write_hotkey(hotkey, *message.add_hot_keys());
  }

  NativeHttpResponse response;
  response.content_type = std::string(kProtobufContentType);
  if (!message.SerializeToString(&response.body)) {
    return json_error(500, "serialization_failed");
  }
  return response;
}

NativeHttpResponse HotKeyRest::save(const std::string &token, std::int64_t character_id,
                                    std::string_view content_type, const std::string &body) {
  if (!content_type.starts_with(kProtobufContentType)) {
    return json_error(415, "protobuf_required");
  }
  if (body.size() > kMaximumHotKeyBodyBytes) {
    return json_error(413, "body_too_large");
  }

  x5::pb::rest::CharHotKey message;
  if (!message.ParseFromString(body) || message.slot() > kMaximumHotKeySlot) {
    return json_error(400, "invalid_hot_key");
  }

  // A slot without a value is the client's "clear slot" request.
  const auto hotkey = hotkey_from(message);
  const auto owner = auth_.character_owner(token, character_id, true);
  if (!owner || !hotkeys_.save_character_hotkey(owner->account_id, character_id, owner->world_id,
                                              static_cast<int>(message.slot()), hotkey)) {
    return json_error(403, "character_not_selected");
  }

  NativeHttpResponse response;
  response.content_type = std::string(kProtobufContentType);
  return response;
}

} // namespace chronicles
