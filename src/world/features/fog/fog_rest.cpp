#include "world/features/fog/fog_rest.hpp"

#include "p_c2f.rest.pb.h"
#include "p_f2c.rest.pb.h"

#include <charconv>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace chronicles {
namespace {

constexpr std::string_view kFrontPrefix = "/api/front/";
constexpr std::string_view kLoadRoute = "CharFogAreaLoadReq";
constexpr std::string_view kSaveRoute = "CharFogAreaSaveReq";
constexpr std::string_view kBearerPrefix = "Bearer ";
constexpr std::string_view kProtobufContentType = "application/protobuf";
constexpr std::size_t kMaximumTokenBytes = 512;
constexpr std::size_t kMaximumFogBodyBytes = 32768;

struct FogRoute {
  std::string_view name;
  std::int64_t character_id{};
};

std::optional<FogRoute> parse_route(std::string_view path) {
  path = path.substr(0, path.find('?'));
  const auto front = path.find(kFrontPrefix);
  if (front == std::string_view::npos) {
    return std::nullopt;
  }

  const auto rest = path.substr(front + kFrontPrefix.size());
  const auto separator = rest.find('/');
  if (separator == std::string_view::npos) {
    return std::nullopt;
  }
  FogRoute route;
  route.name = rest.substr(0, separator);
  if (route.name != kLoadRoute && route.name != kSaveRoute) {
    return std::nullopt;
  }

  const auto number = rest.substr(separator + 1);
  const auto parsed =
      std::from_chars(number.data(), number.data() + number.size(), route.character_id);
  if (number.empty() || parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() ||
      route.character_id <= 0) {
    return std::nullopt;
  }
  return route;
}

NativeHttpResponse error_response(unsigned status, const char *error) {
  NativeHttpResponse response;
  response.status = status;
  response.content_type = "application/json";
  response.body = nlohmann::json{{"error", error}}.dump();
  return response;
}

bool protobuf_content_type(std::string_view content_type) {
  return content_type == kProtobufContentType || content_type.starts_with("application/protobuf;");
}

} // namespace

FogRest::FogRest(AuthService &auth, FogRepository &fog, ClientCatalog *catalog)
    : auth_(auth), fog_(fog) {
  if (!catalog) {
    return;
  }
  const auto areas = catalog->fog_area_ids();
  if (areas.size() > kMaximumFogAreas) {
    throw std::runtime_error("FogArea catalog exceeds supported exploration bounds");
  }
  known_areas_.insert(areas.begin(), areas.end());
  validate_catalog_ = true;
}

bool FogRest::handles(std::string_view path) {
  return parse_route(path).has_value();
}

NativeHttpResponse FogRest::handle(std::string_view method, std::string_view path,
                                   std::string_view authorization, std::string_view content_type,
                                   const std::string &body) {
  const auto route = parse_route(path);
  if (!route) {
    return error_response(404, "route_not_found");
  }
  if (!authorization.starts_with(kBearerPrefix) || authorization.size() <= kBearerPrefix.size() ||
      authorization.size() > kBearerPrefix.size() + kMaximumTokenBytes) {
    return error_response(401, "authentication_required");
  }
  const std::string token(authorization.substr(kBearerPrefix.size()));

  try {
    if (route->name == kLoadRoute) {
      if (method != "GET") {
        return error_response(405, "method_not_allowed");
      }
      const auto owner = auth_.character_owner(token, route->character_id, true);
      if (!owner) {
        return error_response(403, "character_not_selected");
      }
      const auto areas =
          fog_.character_fog_areas(owner->account_id, route->character_id, owner->world_id);

      x5::pb::rest::FogAreaLoadRes message;
      message.set_ec(x5::ec::Success);
      for (const int area_id : areas) {
        message.add_fog_area_ids(area_id);
      }
      NativeHttpResponse response;
      if (!message.SerializeToString(&response.body)) {
        return error_response(500, "serialization_failed");
      }
      return response;
    }

    if (method != "PUT") {
      return error_response(405, "method_not_allowed");
    }
    if (!protobuf_content_type(content_type)) {
      return error_response(415, "protobuf_required");
    }
    if (body.size() > kMaximumFogBodyBytes) {
      return error_response(413, "body_too_large");
    }

    x5::pb::rest::FogAreaSaveReq message;
    if (!message.ParseFromString(body) || message.fog_area_ids_size() > kMaximumFogAreas) {
      return error_response(400, "invalid_fog_areas");
    }
    std::vector<int> areas;
    areas.reserve(message.fog_area_ids_size());
    for (const int area_id : message.fog_area_ids()) {
      if (area_id <= 0 || (validate_catalog_ && !known_areas_.contains(area_id))) {
        return error_response(400, "invalid_fog_areas");
      }
      areas.push_back(area_id);
    }
    const auto owner = auth_.character_owner(token, route->character_id, true);
    if (!owner || !fog_.discover_fog_areas(owner->account_id, route->character_id,
                                           owner->world_id, areas)) {
      return error_response(403, "character_not_selected");
    }

    // The client's save completion reads status only; success follows the durable commit.
    return NativeHttpResponse{};
  } catch (const Unauthorized &) {
    return error_response(401, "authentication_required");
  } catch (const std::invalid_argument &) {
    return error_response(400, "invalid_fog_areas");
  } catch (const std::exception &) {
    // Database diagnostics can contain connection details; keep the public error bounded.
    spdlog::error("Front REST exploration storage failed");
    return error_response(500, "exploration_storage_failed");
  }
}

} // namespace chronicles
