#include "world/features/nuia/nuia_tears.hpp"

#include "world/core/world_request_opcodes.hpp"
#include "world/features/quests/quest_rules.hpp"

#include <charconv>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace chronicles {
namespace {

using Json = nlohmann::json;
using quest_rules::require;

constexpr std::uint16_t kActivateRequest = world_request::kActivateAdventureObjectRequest;
constexpr std::uint16_t kDiscoverRequest = world_request::kDiscoverAdventureObjectRequest;
constexpr std::uint16_t kActivateResponse = 91;
constexpr std::uint16_t kObjectsUpdated = 90;
constexpr int kTearItemId = 2000;
constexpr std::size_t kMaximumObjectRecords = 4096;
// Official successful samples reach 8001 mm from the authored point. Leave bounded
// movement/placement tolerance rather than using the tutorial's smaller use radius.
constexpr double kCollectionRangeMm = 10000;

int definition_integer(const Json &value, int minimum) {
  if (!value.is_number_integer() ||
      (value.is_number_unsigned() &&
       value.get<std::uint64_t>() > std::numeric_limits<int>::max())) {
    throw std::runtime_error("Invalid Nuia Tear definition integer");
  }

  const auto number = value.get<std::int64_t>();
  if (number < minimum || number > std::numeric_limits<int>::max()) {
    throw std::runtime_error("Nuia Tear definition integer is outside its range");
  }
  return static_cast<int>(number);
}

std::uint64_t unit_id(const Json &value) {
  if (!value.is_string()) {
    throw std::invalid_argument("Expected a decimal Nuia Tear spot ID");
  }
  const auto text = value.get<std::string>();
  std::uint64_t result{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
  if (error != std::errc{} || end != text.data() + text.size() || result == 0) {
    throw std::invalid_argument("Invalid Nuia Tear spot ID");
  }
  return result;
}

Json object_state(int cms_id, int state) {
  return {{"cms_id", cms_id},
          {"state", state == 2 ? "EX5AdventureObjectState_Activated"
                               : "EX5AdventureObjectState_Discovered"}};
}

GameplayPacket failure(std::uint16_t opcode, std::uint32_t sequence, const char *code) {
  if (opcode == kActivateRequest) {
    return {kActivateResponse, {{"ec", code}}};
  }
  return {2, {{"ec", code}, {"packet_seq", {{"seq", std::to_string(sequence)}}}}};
}

} // namespace

NuiaTears NuiaTears::load(const std::filesystem::path &path) {
  if (std::filesystem::file_size(path) > 8 * 1024 * 1024) {
    throw std::runtime_error("Nuia Tear definitions exceed the size limit");
  }
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Cannot open Nuia Tear definitions");
  }
  Json document;
  input >> document;
  return NuiaTears(document);
}

NuiaTears::NuiaTears(const Json &document) {
  if (document.at("format_version") != 1 ||
      document.at("build_id") != "40367e24241c9481bfc07fbf9ef1919f25fb0eaa" ||
      document.at("source_executable_dump_sha256") !=
          "e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6" ||
      !document.at("placements").is_array() || document.at("placements").size() > 100000) {
    throw std::runtime_error("Unsupported Nuia Tear definitions");
  }

  for (const auto &row : document.at("placements")) {
    const int map_id = definition_integer(row.at("map_id"), 1);
    const int cms_id = definition_integer(row.at("cms_id"), 1);
    const int quantity = definition_integer(row.at("quantity"), 1);
    const auto &point = row.at("position");
    if (map_id <= 0 || cms_id <= 0 || (quantity != 1 && quantity != 5 && quantity != 10) ||
        !point.is_array() || point.size() != 3) {
      throw std::runtime_error("Invalid Nuia Tear placement");
    }
    for (const auto &coordinate : point) {
      definition_integer(coordinate, std::numeric_limits<int>::min());
    }
    if (!placements_
             .emplace(std::pair{map_id, unit_id(row.at("unit_id"))},
                      Placement{cms_id, point, quantity})
             .second) {
      throw std::runtime_error("Duplicate Nuia Tear placement");
    }
  }
}

bool NuiaTears::handles(std::uint16_t opcode) {
  return opcode == kActivateRequest || opcode == kDiscoverRequest;
}

bool NuiaTears::handles(std::uint16_t opcode, const Json &request,
                        const WorldPosition &position) const {
  if (opcode != kDiscoverRequest) {
    return handles(opcode);
  }

  // Proximity reports cover every adventure-object type. Only indexed tears belong
  // here; unrelated objects must reach their own handler without a target-error toast.
  // Explicit activation keeps its existing refusal when no handler owns the target.
  try {
    return placements_.contains({position.map_id, unit_id(request.at("spot_object_id"))});
  } catch (const Json::exception &) {
    // Keep malformed requests on the rate-limited BadRequest path.
    return true;
  } catch (const std::invalid_argument &) {
    return true;
  }
}

GameplayPacket NuiaTears::snapshot(const WorldProgress &progress) {
  auto objects = Json::array();
  for (const auto &[cms_id, state] : progress.adventure_objects) {
    objects.push_back(object_state(cms_id, state));
  }
  return {127, {{"adventure_objects", std::move(objects)}}};
}

NuiaTearResult NuiaTears::receive(std::uint16_t opcode, const Json &request, std::uint32_t sequence,
                                  const WorldPosition &position,
                                  const WorldProgress &current) const {
  try {
    require(handles(opcode), "BadRequest");
    // Proximity discovery is automatic even while the client restores its death UI.
    // It cannot collect an item or change discovery state while dead.
    if (opcode == kDiscoverRequest && current.dead) {
      return {};
    }
    require(!current.dead, "BadRequest");
    const auto found = placements_.find({position.map_id, unit_id(request.at("spot_object_id"))});
    require(found != placements_.end(), "Interaction_TargetNotFound");
    const auto &placement = found->second;
    require(quest_rules::request_position_matches(request, position), "TooFar");
    if (opcode == kActivateRequest) {
      require(quest_rules::distance_squared(position, placement.position) <=
                  kCollectionRangeMm * kCollectionRangeMm,
              "TooFar");
    } else {
      require(quest_rules::within_discovery_range(position, placement.position), "TooFar");
    }
    const auto saved = current.adventure_objects.find(placement.cms_id);
    const int previous_state = saved == current.adventure_objects.end() ? 0 : saved->second;
    NuiaTearResult result;
    const int next_state = opcode == kActivateRequest || previous_state == 2 ? 2 : 1;
    const auto state = object_state(placement.cms_id, next_state);
    result.packets.push_back(
        opcode == kActivateRequest
            ? GameplayPacket{kActivateResponse, {{"adventure_object", state}}}
            : GameplayPacket{kObjectsUpdated, {{"adventure_objects", {state}}}});
    if (previous_state == next_state) {
      return result;
    }

    require(current.adventure_objects.size() < kMaximumObjectRecords || previous_state != 0,
            "BadRequest");
    require(current.revision < std::numeric_limits<std::int64_t>::max(), "BadRequest");
    auto next = current;
    next.adventure_objects[placement.cms_id] = next_state;
    if (opcode == kActivateRequest) {
      quest_rules::grant_rewards(next,
                                 {{{"item_cms_id", kTearItemId}, {"quantity", placement.quantity}}},
                                 result.packets, "adventure_object_activate");
    }
    ++next.revision;
    result.next = std::move(next);
    return result;
  } catch (const quest_rules::RuleError &error) {
    return {{failure(opcode, sequence, error.code.c_str())}, std::nullopt};
  } catch (const Json::exception &) {
    return {{failure(opcode, sequence, "BadRequest")}, std::nullopt};
  } catch (const std::invalid_argument &) {
    return {{failure(opcode, sequence, "BadRequest")}, std::nullopt};
  }
}

} // namespace chronicles
