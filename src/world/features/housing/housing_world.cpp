#include "world/features/housing/housing_world.hpp"

#include "world/core/progress_packets.hpp"
#include "world/core/world_request_opcodes.hpp"
#include <openssl/evp.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace chronicles::housing {
namespace {

using Json = nlohmann::json;

constexpr char kHousingErrorCategory[] = "EErrorCategory_Housing";
constexpr std::size_t kMaximumRequestedPackages = 64;

Json sequence_json(std::uint32_t sequence) {
  return {{"seq", std::to_string(sequence)}};
}

Json world_pos_json(const LocalPosition &position) {
  return {{"millimeter_x", position.x_mm},
          {"millimeter_y", position.y_mm},
          {"millimeter_z", position.z_mm}};
}

Json world_pos_json(const WorldPosition &position) {
  return {{"millimeter_x", position.x_mm},
          {"millimeter_y", position.y_mm},
          {"millimeter_z", position.z_mm}};
}

// Housing doodad phases start at a stored Unix time so they survive restarts. The client reads
// phase_begin_tick on its monotonic wire clock (nanoseconds of the server's steady clock, as in
// the captured gathering ChangeDoodadPhaseNoti), so the stored start is mapped onto that clock.
std::string phase_begin_wire_tick(std::int64_t begin_epoch_ticks) {
  if (begin_epoch_ticks <= 0) {
    return "0";
  }
  using Ticks = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
  constexpr std::int64_t kNanosecondsPerTick = 100;

  const auto epoch_now = std::chrono::duration_cast<Ticks>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
  const auto monotonic_now = std::chrono::duration_cast<Ticks>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
  const auto elapsed = std::max<std::int64_t>(0, epoch_now - begin_epoch_ticks);
  const auto begin = std::max<std::int64_t>(0, monotonic_now - elapsed);
  return std::to_string(begin * kNanosecondsPerTick);
}

std::string base64_encode(const std::string &bytes) {
  if (bytes.empty()) {
    return {};
  }
  std::string encoded(4 * ((bytes.size() + 2) / 3), '\0');
  const int written =
      EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()),
                      reinterpret_cast<const unsigned char *>(bytes.data()),
                      static_cast<int>(bytes.size()));
  encoded.resize(static_cast<std::size_t>(written));
  return encoded;
}

std::string base64_decode(const std::string &text) {
  if (text.empty()) {
    return {};
  }
  if (text.size() % 4 != 0 || text.size() > 4096) {
    throw std::invalid_argument("house_part_custom_ids is not valid base64");
  }

  std::string decoded(text.size() / 4 * 3, '\0');
  const int written =
      EVP_DecodeBlock(reinterpret_cast<unsigned char *>(decoded.data()),
                      reinterpret_cast<const unsigned char *>(text.data()),
                      static_cast<int>(text.size()));
  if (written < 0) {
    throw std::invalid_argument("house_part_custom_ids is not valid base64");
  }
  // EVP_DecodeBlock counts padding bytes as data; drop one byte per '='.
  std::size_t length = static_cast<std::size_t>(written);
  for (auto character = text.rbegin(); character != text.rend() && *character == '='; ++character) {
    --length;
  }
  decoded.resize(length);
  return decoded;
}

// Protobuf JSON leaves default scalars out; 64-bit integers arrive as strings.
template <typename Integer>
Integer integer_field(const Json &message, const char *field, Integer fallback = 0) {
  const auto found = message.find(field);
  if (found == message.end() || found->is_null()) {
    return fallback;
  }

  std::int64_t value{};
  if (found->is_string()) {
    std::size_t consumed = 0;
    const auto &text = found->get_ref<const std::string &>();
    try {
      value = std::stoll(text, &consumed);
    } catch (const std::exception &) {
      throw std::invalid_argument(std::string(field) + " is not an integer");
    }
    if (consumed != text.size()) {
      throw std::invalid_argument(std::string(field) + " is not an integer");
    }
  } else if (found->is_number_unsigned()) {
    const auto unsigned_value = found->get<std::uint64_t>();
    if (unsigned_value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      if constexpr (std::is_same_v<Integer, std::uint64_t>) {
        return unsigned_value;
      }
      throw std::invalid_argument(std::string(field) + " is out of range");
    }
    value = static_cast<std::int64_t>(unsigned_value);
  } else if (found->is_number_integer()) {
    value = found->get<std::int64_t>();
  } else {
    throw std::invalid_argument(std::string(field) + " is not an integer");
  }

  if constexpr (std::is_unsigned_v<Integer>) {
    if (value < 0 || static_cast<std::uint64_t>(value) > std::numeric_limits<Integer>::max()) {
      throw std::invalid_argument(std::string(field) + " is out of range");
    }
  } else {
    if (value < std::numeric_limits<Integer>::min() ||
        value > std::numeric_limits<Integer>::max()) {
      throw std::invalid_argument(std::string(field) + " is out of range");
    }
  }
  return static_cast<Integer>(value);
}

std::uint64_t unsigned_field(const Json &message, const char *field) {
  const auto found = message.find(field);
  if (found == message.end() || found->is_null()) {
    return 0;
  }
  if (found->is_string()) {
    const auto &text = found->get_ref<const std::string &>();
    std::size_t consumed = 0;
    std::uint64_t value{};
    try {
      value = std::stoull(text, &consumed);
    } catch (const std::exception &) {
      throw std::invalid_argument(std::string(field) + " is not an unsigned integer");
    }
    if (consumed != text.size() || text.starts_with('-')) {
      throw std::invalid_argument(std::string(field) + " is not an unsigned integer");
    }
    return value;
  }
  if (found->is_number_unsigned()) {
    return found->get<std::uint64_t>();
  }
  if (found->is_number_integer() && found->get<std::int64_t>() >= 0) {
    return static_cast<std::uint64_t>(found->get<std::int64_t>());
  }
  throw std::invalid_argument(std::string(field) + " is not an unsigned integer");
}

float float_field(const Json &message, const char *field) {
  const auto found = message.find(field);
  if (found == message.end() || found->is_null()) {
    return 0.0F;
  }
  if (!found->is_number()) {
    throw std::invalid_argument(std::string(field) + " is not a number");
  }
  const auto value = found->get<double>();
  if (!std::isfinite(value)) {
    throw std::invalid_argument(std::string(field) + " is not finite");
  }
  return static_cast<float>(value);
}

bool bool_field(const Json &message, const char *field) {
  const auto found = message.find(field);
  if (found == message.end() || found->is_null()) {
    return false;
  }
  if (!found->is_boolean()) {
    throw std::invalid_argument(std::string(field) + " is not a boolean");
  }
  return found->get<bool>();
}

// Decoded enums arrive by name (for example "EX5HousingEditMode_Customize"); numbers are also
// accepted. An absent field is the zero value.
int enum_field(const Json &message, const char *field, const char *prefix,
               std::initializer_list<const char *> names) {
  const auto found = message.find(field);
  if (found == message.end() || found->is_null()) {
    return 0;
  }
  if (found->is_number_integer()) {
    const auto value = found->get<std::int64_t>();
    if (value < 0 || value >= static_cast<std::int64_t>(names.size())) {
      throw std::invalid_argument(std::string(field) + " names an unknown value");
    }
    return static_cast<int>(value);
  }
  if (!found->is_string()) {
    throw std::invalid_argument(std::string(field) + " is not an enum value");
  }

  const auto &text = found->get_ref<const std::string &>();
  int index = 0;
  for (const auto *name : names) {
    if (text == std::string(prefix) + name) {
      return index;
    }
    ++index;
  }
  throw std::invalid_argument(std::string(field) + " names an unknown value");
}

EditMode edit_mode_field(const Json &message) {
  return static_cast<EditMode>(
      enum_field(message, "mode", "EX5HousingEditMode_", {"Build", "Customize", "Upgrade"}));
}

ContractType contract_type_field(const Json &message, const char *field) {
  return static_cast<ContractType>(enum_field(
      message, field, "EX5HousingContractType_",
      {"None", "MonthlyContract", "PeriodicContract", "CloseContract"}));
}

LocalPosition local_position_field(const Json &message, const char *field) {
  const auto found = message.find(field);
  if (found == message.end() || found->is_null()) {
    return {};
  }
  if (!found->is_object()) {
    throw std::invalid_argument(std::string(field) + " is not a position");
  }
  return LocalPosition{integer_field<int>(*found, "millimeter_x"),
                       integer_field<int>(*found, "millimeter_y"),
                       integer_field<int>(*found, "millimeter_z")};
}

const Json &array_field(const Json &message, const char *field) {
  static const Json empty = Json::array();
  const auto found = message.find(field);
  if (found == message.end() || found->is_null()) {
    return empty;
  }
  if (!found->is_array()) {
    throw std::invalid_argument(std::string(field) + " is not a list");
  }
  return *found;
}

std::uint32_t doodad_map_key(const DoodadRecord &doodad) {
  // Candidate: the map key repeats the doodad unit ID's object field (house << 14 | doodad).
  return (doodad.house_id << 14) | doodad.doodad_id;
}

Json stackable_changes(const WorldProgress &before, const WorldProgress &after) {
  auto stackables = Json::array();
  for (const auto &[cms_id, quantity] : after.stackables) {
    const auto previous = before.stackables.find(cms_id);
    if (previous == before.stackables.end() || previous->second != quantity) {
      stackables.push_back({{"item_cms_id", cms_id}, {"quantity", quantity}});
    }
  }
  return stackables;
}

Json housing_item_changes(const WorldProgress &before, const WorldProgress &after) {
  auto items = Json::array();
  for (const auto &[cms_id, record] : after.housing_items) {
    const auto previous = before.housing_items.find(cms_id);
    if (previous == before.housing_items.end() || previous->second != record) {
      items.push_back(housing_item_count_json(cms_id, record));
    }
  }
  return items;
}

Json money_changes(const WorldProgress &before, const WorldProgress &after) {
  auto changes = Json::array();
  if (after.gold != before.gold) {
    changes.push_back({{"change", std::to_string(after.gold - before.gold)},
                       {"result", std::to_string(after.gold)}});
  }
  if (after.hersander_token != before.hersander_token) {
    changes.push_back({{"change", std::to_string(after.hersander_token - before.hersander_token)},
                       {"result", std::to_string(after.hersander_token)},
                       {"money_type", "EX5MoneyType_HersanderToken"}});
  }
  return changes;
}

// Summarizes a refused UpdateHousingReq for the server log: object IDs, catalog IDs and
// site-local positions only, bounded so a large save cannot flood the log.
std::string describe_update_entries(const UpdateRequest &request) {
  constexpr std::size_t kMaximumDescribedEntries = 24;

  std::string text = "houses=[";
  std::size_t described = 0;
  for (const auto &house : request.houses) {
    if (described++ == kMaximumDescribedEntries) {
      text += " ...";
      break;
    }
    text += fmt::format(" {}:{}@{},{},{}/attach{}", house.house_id, house.housing_id,
                        house.position.x_mm, house.position.y_mm, house.position.z_mm,
                        house.attach_index);
  }

  text += " ] doodads=[";
  described = 0;
  for (const auto &doodad : request.doodads) {
    if (described++ == kMaximumDescribedEntries) {
      text += " ...";
      break;
    }
    text += fmt::format(" {}/{}:{}@{},{},{}^{}", doodad.house_id, doodad.doodad_id,
                        doodad.cms_id, doodad.position.x_mm, doodad.position.y_mm,
                        doodad.position.z_mm, doodad.parent_doodad_id);
  }
  text += " ]";
  return text;
}

} // namespace

Json house_nub_json(const HouseRecord &house) {
  return {{"house_id", house.house_id},
          {"attach_index", house.attach_index},
          {"cms_id", house.housing_id},
          {"pos", world_pos_json(house.position)},
          {"yaw", house.yaw},
          {"parts_state", std::to_string(house.parts_state)}};
}

Json doodad_nub_json(const DoodadRecord &doodad) {
  return {{"doodad_id", doodad.doodad_id},
          {"house_id", doodad.house_id},
          {"cms_id", doodad.cms_id},
          {"cur_phase", doodad.phase},
          {"phase_begin_tick", phase_begin_wire_tick(doodad.phase_begin_tick)},
          {"pos", world_pos_json(doodad.position)},
          {"pitch", doodad.pitch},
          {"yaw", doodad.yaw},
          {"roll", doodad.roll},
          {"interaction_count", doodad.interaction_count},
          {"parent_doodad_id", doodad.parent_doodad_id}};
}

Json housing_nub_json(const SiteRecord &site) {
  Json nub{{"lot_number", site.key.lot_number},
           {"owner_char_id", std::to_string(site.owner_character_id)},
           {"owner_name", site.owner_name},
           {"expiry_epochtime", std::to_string(site.expiry_epoch_ticks)},
           {"contract_type", static_cast<int>(site.contract_type)}};
  if (site.return_position) {
    nub["return_pos"] = world_pos_json(*site.return_position);
  }
  if (!site.contents.house_part_custom_ids.empty()) {
    nub["house_part_custom_ids"] = base64_encode(site.contents.house_part_custom_ids);
  }

  Json houses = Json::object();
  for (const auto &[house_id, house] : site.contents.houses) {
    houses[std::to_string(house_id)] = house_nub_json(house);
  }
  Json doodads = Json::object();
  for (const auto &[key, doodad] : site.contents.doodads) {
    doodads[std::to_string(doodad_map_key(doodad))] = doodad_nub_json(doodad);
  }
  nub["houses"] = std::move(houses);
  nub["housing_doodads"] = std::move(doodads);
  return nub;
}

Json housing_site_json(const SiteRecord &site) {
  Json housing_site{{"lot_number", site.key.lot_number},
                    {"channel", site.key.channel},
                    {"expiry_epochtime", std::to_string(site.expiry_epoch_ticks)},
                    {"contract_type", static_cast<int>(site.contract_type)},
                    {"authority_type", static_cast<int>(site.authority)}};
  if (site.return_position) {
    housing_site["return_pos"] = world_pos_json(*site.return_position);
  }
  if (site.authority == AuthorityType::guild) {
    housing_site["guild_housing"] = {{"guild_channel", site.key.channel},
                                     {"guild_zone_key", site.key.zone_key}};
  }
  return housing_site;
}

WorldPacket housing_add_packet(const SiteRecord &site, const HousingCatalog &catalog) {
  const auto *definition = catalog.site(site.key.lot_number);
  const std::uint32_t sector_id = definition ? definition->sector_id : 0;
  return {kHousingAddNotification, {{"sector_id", sector_id}, {"nub", housing_nub_json(site)}}};
}

WorldPacket site_contract_packet(const SiteRecord &site, const HousingCatalog &catalog) {
  const auto *definition = catalog.site(site.key.lot_number);
  const std::uint32_t sector_id = definition ? definition->sector_id : 0;
  return {kSiteContractNotification,
          {{"sector_id", sector_id},
           {"lot_number", site.key.lot_number},
           {"char_id", std::to_string(site.owner_character_id)},
           {"expiry_epochtime", std::to_string(site.expiry_epoch_ticks)},
           {"contract_type", static_cast<int>(site.contract_type)}}};
}

WorldPacket site_removed_packet(const SiteKey &site) {
  // Candidate: a released lot leaves the client's sight like any unit.
  return {kUnitRemoveNotification,
          {{"unit_ids", Json::array({std::to_string(housing_unit_id(site.lot_number))})}}};
}

WorldPacket site_update_packet(const SiteUpdateNotice &notice) {
  Json message{{"lot_number", notice.site.lot_number},
               {"edit_save_cooldown_start_epoch_time",
                std::to_string(notice.edit_save_cooldown_start_ticks)}};
  if (notice.custom_ids_changed) {
    message["house_part_custom_ids"] = base64_encode(notice.house_part_custom_ids);
    return {kUpdateHousingResponse, std::move(message)};
  }

  auto houses = Json::array();
  for (const auto &house : notice.houses) {
    houses.push_back(house_nub_json(house));
  }
  auto doodads = Json::array();
  for (const auto &doodad : notice.doodads) {
    doodads.push_back(doodad_nub_json(doodad));
  }
  auto removed_doodads = Json::array();
  for (const auto unit_id : notice.removed_doodad_unit_ids) {
    removed_doodads.push_back(std::to_string(unit_id));
  }
  message["house_nubs"] = std::move(houses);
  message["housing_doodad_nubs"] = std::move(doodads);
  message["removed_house_ids"] = notice.removed_house_ids;
  message["removed_doodad_unit_ids"] = std::move(removed_doodads);
  return {kUpdateHousingResponse, std::move(message)};
}

WorldPacket parts_update_packet(const PartsUpdateNotice &notice) {
  return {kHousePartsUpdateResponse,
          {{"housing_lot_number", notice.site.lot_number},
           {"house_id", notice.house_id},
           {"parts_state", std::to_string(notice.parts_state)}}};
}

WorldPacket owner_site_packet(const std::optional<SiteRecord> &site) {
  if (!site) {
    return {kCharSiteUpdateNotification,
            {{"housing_site", Json::object()},
             {"authority_type", static_cast<int>(AuthorityType::none)}}};
  }
  return {kCharSiteUpdateNotification,
          {{"housing_site", housing_site_json(*site)},
           {"authority_type", static_cast<int>(site->authority)}}};
}

WorldPacket guild_site_packet(const SiteRecord &site, GuildHousingAction action) {
  return {kGuildSiteUpdateNotification,
          {{"housing_site", housing_site_json(site)}, {"guild_action", static_cast<int>(action)}}};
}

WorldPacket package_created_packet(PackageReason reason) {
  return {kPackageCreatedNotification, {{"source_reason", static_cast<int>(reason)}}};
}

std::optional<WorldPacket> progress_update_packet(const ProgressNotice &notice) {
  const auto stackables = stackable_changes(notice.before, notice.after);
  const auto housing_items = housing_item_changes(notice.before, notice.after);
  const auto money = money_changes(notice.before, notice.after);
  if (stackables.empty() && housing_items.empty() && money.empty()) {
    return std::nullopt;
  }

  // Candidate: housing charges use ItemsUpdateNoti.Reason_Housing; no capture exists.
  Json message{{"housing", Json::object()}};
  if (!stackables.empty()) {
    message["stackables"] = stackables;
  }
  if (!housing_items.empty()) {
    message["housing_items"] = housing_items;
  }
  if (!money.empty()) {
    message["money_changes"] = money;
  }
  return WorldPacket{kItemsUpdateNotification, std::move(message)};
}

std::optional<WorldPacket> housing_items_packet(const WorldProgress &progress) {
  return housing_items_packet(progress.housing_items);
}

std::optional<WorldPacket>
housing_items_packet(const std::map<int, HousingItemRecord> &owned_items) {
  auto items = Json::array();
  for (const auto &[cms_id, record] : owned_items) {
    // Every owned row is listed, also when all its copies are placed, so the client learns
    // the installed count.
    if (record.quantity > 0) {
      items.push_back(housing_item_count_json(cms_id, record));
    }
  }
  if (items.empty()) {
    return std::nullopt;
  }
  return WorldPacket{kWorldCharHousingItemsNotification, {{"housing_items", std::move(items)}}};
}

WorldPacket error_packet(ErrorCode code, std::uint32_t sequence) {
  return {kCommonErrorResponse,
          {{"ec", x5::ec::EErrorCode_Name(code)},
           {"category", kHousingErrorCategory},
           {"packet_seq", sequence_json(sequence)}}};
}

std::vector<WorldPacket> channel_snapshot(const HousingService &service, int zone_key,
                                          int channel) {
  std::vector<WorldPacket> packets;
  for (const auto &site : service.sites_in_channel(zone_key, channel)) {
    packets.push_back(housing_add_packet(site, service.catalog()));
  }
  return packets;
}

UpdateRequest parse_update_request(const Json &message) {
  if (!message.is_object()) {
    throw std::invalid_argument("UpdateHousingReq is not an object");
  }

  UpdateRequest request;
  request.lot_number = integer_field<int>(message, "housing_lot_number");
  const auto custom = message.find("house_part_custom_ids");
  if (custom != message.end() && !custom->is_null()) {
    if (!custom->is_string()) {
      throw std::invalid_argument("house_part_custom_ids is not bytes");
    }
    auto decoded = base64_decode(custom->get<std::string>());
    if (!decoded.empty()) {
      request.house_part_custom_ids = std::move(decoded);
    }
  }

  for (const auto &entry : array_field(message, "house_nubs")) {
    HouseRecord house;
    house.house_id = integer_field<std::uint32_t>(entry, "house_id");
    // The client sets attach_index only for attached buildings (see
    // AAC_Housing_BuildEditor_FillHouseNub); an absent field means a main building.
    house.attach_index = integer_field<int>(entry, "attach_index", kUnattachedHouseIndex);
    house.housing_id = integer_field<int>(entry, "cms_id");
    house.position = local_position_field(entry, "pos");
    house.yaw = float_field(entry, "yaw");
    request.houses.push_back(house);
  }
  for (const auto &entry : array_field(message, "housing_doodad_nubs")) {
    DoodadRecord doodad;
    doodad.doodad_id = integer_field<std::uint32_t>(entry, "doodad_id");
    doodad.house_id = integer_field<std::uint32_t>(entry, "house_id");
    doodad.cms_id = integer_field<int>(entry, "cms_id");
    doodad.phase = integer_field<int>(entry, "cur_phase");
    doodad.position = local_position_field(entry, "pos");
    doodad.pitch = float_field(entry, "pitch");
    doodad.yaw = float_field(entry, "yaw");
    doodad.roll = float_field(entry, "roll");
    doodad.parent_doodad_id = integer_field<std::uint32_t>(entry, "parent_doodad_id");
    request.doodads.push_back(doodad);
  }
  for (const auto &entry : array_field(message, "remove_house_ids")) {
    request.remove_house_ids.push_back(integer_field<std::uint32_t>(Json{{"id", entry}}, "id"));
  }
  for (const auto &entry : array_field(message, "remove_doodad_unit_ids")) {
    request.remove_doodad_unit_ids.push_back(unsigned_field(Json{{"id", entry}}, "id"));
  }
  return request;
}

HousingHub::HousingHub(std::shared_ptr<HousingService> service,
                       std::chrono::milliseconds session_task_timeout)
    : service_(std::move(service)), session_task_timeout_(session_task_timeout) {
  if (!service_) {
    throw std::invalid_argument("Housing hub needs a housing service");
  }
}

HousingService &HousingHub::service() {
  return *service_;
}

const HousingCatalog &HousingHub::catalog() const {
  return service_->catalog();
}

void HousingHub::set_guild_area_listener(GuildAreaListener listener) {
  std::lock_guard lock(mutex_);
  guild_area_listener_ = std::move(listener);
}

void HousingHub::attach(const HousingActor &actor, std::uint64_t connection_id,
                        std::uint64_t unit_id, std::weak_ptr<SessionPort> port) {
  std::lock_guard lock(mutex_);
  auto &member = members_[actor.character_id];
  member.actor = actor;
  member.connection_id = connection_id;
  member.unit_id = unit_id;
  member.port = std::move(port);
  member.pose_animation_id.reset();
  member.held_pose.reset();
  member.doodad_use.reset();
}

void HousingHub::detach(CharacterId character_id, std::uint64_t connection_id) {
  std::lock_guard lock(mutex_);
  const auto found = members_.find(character_id);
  if (found != members_.end() && found->second.connection_id == connection_id) {
    members_.erase(found);
  }
}

void HousingHub::set_location(CharacterId character_id, std::uint64_t connection_id,
                              std::optional<HousingLocation> location) {
  std::lock_guard lock(mutex_);
  const auto found = members_.find(character_id);
  if (found != members_.end() && found->second.connection_id == connection_id) {
    if (found->second.location != location) {
      // A pose or garden action ends with the scene it was started in.
      found->second.pose_animation_id.reset();
      found->second.held_pose.reset();
      found->second.doodad_use.reset();
    }
    found->second.location = location;
  }
}

std::optional<HousingLocation> HousingHub::location_of(CharacterId character_id) const {
  std::lock_guard lock(mutex_);
  const auto found = members_.find(character_id);
  if (found == members_.end()) {
    return std::nullopt;
  }
  return found->second.location;
}

std::optional<HousingPlacedDoodad>
HousingHub::placed_doodad(CharacterId character_id, std::uint64_t unit_id, int map_id) const {
  constexpr double kMillimetresPerCentimetre = 10.0;

  const auto target = parse_housing_doodad_unit_id(unit_id);
  const auto location = location_of(character_id);
  if (!target || !location) {
    return std::nullopt;
  }

  const auto &housing_catalog = catalog();
  const auto *zone = housing_catalog.zone(location->zone_key);
  const auto *lot = housing_catalog.site(target->lot_number);
  if (!zone || zone->map_id != map_id || !lot || lot->zone_key != location->zone_key) {
    return std::nullopt;
  }

  const auto site =
      service_->site(SiteKey{location->zone_key, location->channel, target->lot_number});
  if (!site) {
    return std::nullopt;
  }
  const auto doodad = site->contents.doodads.find(DoodadKey{target->house_id, target->doodad_id});
  if (doodad == site->contents.doodads.end()) {
    return std::nullopt;
  }

  // Object positions are lot-local and compose with the lot's actor transform (and a parent
  // building's), so the lot centre and its corner distance bound every object of the lot.
  HousingPlacedDoodad placed;
  placed.doodad_cms_id = doodad->second.cms_id;
  placed.lot_centre_x_mm = static_cast<int>(std::lround(lot->x_cm * kMillimetresPerCentimetre));
  placed.lot_centre_y_mm = static_cast<int>(std::lround(lot->y_cm * kMillimetresPerCentimetre));
  placed.lot_centre_z_mm = static_cast<int>(std::lround(lot->z_cm * kMillimetresPerCentimetre));
  const double half_diagonal_cm = std::hypot(lot->size_x_cm, lot->size_y_cm) / 2.0;
  placed.lot_radius_mm =
      static_cast<int>(std::lround(half_diagonal_cm * kMillimetresPerCentimetre));
  return placed;
}

bool HousingHub::handles(std::uint16_t opcode) {
  switch (opcode) {
  case kEditModeRequest:
  case kContractExtensionRequest:
  case kPackageUnpackRequest:
  case kPackageRecoverRequest:
  case kUpdateHousingRequest:
  case kItemDiscardRequest:
  case kGoHomeRequest:
    return true;
  default:
    return false;
  }
}

std::shared_ptr<SessionPort> HousingHub::port_of(CharacterId character_id) const {
  std::lock_guard lock(mutex_);
  const auto found = members_.find(character_id);
  if (found == members_.end()) {
    return {};
  }
  return found->second.port.lock();
}

std::vector<CharacterId> HousingHub::viewers(const SiteKey &site) const {
  std::lock_guard lock(mutex_);
  std::vector<CharacterId> result;
  const HousingLocation location{site.zone_key, site.channel};
  for (const auto &[character_id, member] : members_) {
    if (member.location == location) {
      result.push_back(character_id);
    }
  }
  return result;
}

void HousingHub::send(CharacterId character_id, std::vector<WorldPacket> packets,
                      CharacterId local_character, std::vector<WorldPacket> *local) {
  if (packets.empty()) {
    return;
  }
  if (local && character_id == local_character) {
    local->insert(local->end(), std::make_move_iterator(packets.begin()),
                  std::make_move_iterator(packets.end()));
    return;
  }
  if (const auto port = port_of(character_id)) {
    port->deliver(std::move(packets));
  }
}

std::vector<WorldPacket> HousingHub::receive(const HousingActor &actor, std::uint64_t unit_id,
                                             std::uint16_t opcode, const Json &message,
                                             std::uint32_t sequence,
                                             const ProgressAccess &access) {
  try {
    return dispatch(actor, unit_id, opcode, message, sequence, access);
  } catch (const std::invalid_argument &error) {
    spdlog::warn("Housing request opcode={} rejected: {}", opcode, error.what());
    return {error_packet(x5::ec::BadRequest, sequence)};
  }
}

std::vector<WorldPacket> HousingHub::dispatch(const HousingActor &actor, std::uint64_t unit_id,
                                              std::uint16_t opcode, const Json &message,
                                              std::uint32_t sequence,
                                              const ProgressAccess &access) {
  std::vector<WorldPacket> local;
  const auto location = location_of(actor.character_id);

  switch (opcode) {
  case kEditModeRequest: {
    const auto mode = edit_mode_field(message);
    const auto end = bool_field(message, "end");
    EditModeResult result{x5::ec::Housing_NotOwner, mode, end};
    if (location) {
      result = service_->edit_mode(actor, mode, end, location->zone_key, location->channel);
    }
    spdlog::info("Housing edit mode character={} mode={} end={} ec={}", actor.character_id,
                 static_cast<int>(mode), end, x5::ec::EErrorCode_Name(result.ec));

    WorldPacket packet{kEditModeNotification,
                       {{"unit_id", std::to_string(unit_id)},
                        {"ec", x5::ec::EErrorCode_Name(result.ec)},
                        {"mode", static_cast<int>(result.mode)},
                        {"end", result.end}}};
    local.push_back(packet);
    // Visitors in the channel see the owner enter or leave edit mode.
    if (result.ec == x5::ec::Success && location) {
      for (const auto viewer : viewers(SiteKey{location->zone_key, location->channel, 0})) {
        if (viewer != actor.character_id) {
          send(viewer, {packet}, 0, nullptr);
        }
      }
    }
    return local;
  }
  case kContractExtensionRequest: {
    auto result =
        service_->extend(actor, contract_type_field(message, "contract_type"), &access);
    local.push_back({kContractExtensionResponse,
                     {{"ec", x5::ec::EErrorCode_Name(result.ec)},
                      {"expiry_epochtime", std::to_string(result.expiry_epoch_ticks)},
                      {"contract_type", static_cast<int>(result.contract_type)}}});
    publish(result.outcome, actor.character_id, actor.character_id, &local);
    return local;
  }
  case kPackageUnpackRequest: {
    const auto package_id = integer_field<std::int64_t>(message, "package_id");
    const auto lot_number = integer_field<int>(message, "housing_lot_number");
    auto result = service_->unpack(actor, package_id, lot_number, &access);
    local.push_back({kPackageUnpackResponse,
                     {{"ec", x5::ec::EErrorCode_Name(result.ec)},
                      {"package_id", std::to_string(package_id)}}});
    publish(result.outcome, actor.character_id, actor.character_id, &local);
    return local;
  }
  case kPackageRecoverRequest: {
    std::vector<std::int64_t> package_ids;
    const auto &requested = array_field(message, "package_ids");
    if (requested.size() > kMaximumRequestedPackages) {
      throw std::invalid_argument("package_ids exceeds the request limit");
    }
    for (const auto &entry : requested) {
      package_ids.push_back(integer_field<std::int64_t>(Json{{"id", entry}}, "id"));
    }
    auto result =
        service_->recover_to_storage(actor, package_ids, bool_field(message, "all"), &access);
    auto recovered = Json::array();
    for (const auto package_id : result.package_ids) {
      recovered.push_back(std::to_string(package_id));
    }
    local.push_back({kPackageRecoverResponse,
                     {{"ec", x5::ec::EErrorCode_Name(result.ec)},
                      {"package_ids", std::move(recovered)}}});
    publish(result.outcome, actor.character_id, actor.character_id, &local);
    return local;
  }
  case kUpdateHousingRequest: {
    const auto request = parse_update_request(message);
    UpdateResult result;
    result.ec = x5::ec::Housing_NotOwner;
    if (location) {
      result = service_->update(actor, request, location->zone_key, location->channel, &access);
    }
    // IDs and counts only: enough to diagnose a refused save without logging placements.
    spdlog::info("Housing update character={} lot={} houses={} doodads={} removed_houses={} "
                 "removed_doodads={} customization={} ec={}",
                 actor.character_id, request.lot_number, request.houses.size(),
                 request.doodads.size(), request.remove_house_ids.size(),
                 request.remove_doodad_unit_ids.size(),
                 request.house_part_custom_ids.has_value(), x5::ec::EErrorCode_Name(result.ec));
    if (result.ec != x5::ec::Success) {
      spdlog::warn("Housing update refused character={} ec={} {}", actor.character_id,
                   x5::ec::EErrorCode_Name(result.ec), describe_update_entries(request));
      return {error_packet(result.ec, sequence)};
    }
    publish(result.outcome, actor.character_id, actor.character_id, &local);
    return local;
  }
  case kItemDiscardRequest: {
    auto result = service_->discard_item(actor, integer_field<int>(message, "item_cms_id"),
                                         integer_field<int>(message, "quantity"), &access);
    if (result.ec != x5::ec::Success) {
      return {error_packet(result.ec, sequence)};
    }
    publish(result.outcome, actor.character_id, actor.character_id, &local);
    return local;
  }
  case kGoHomeRequest: {
    const auto result = service_->go_home(actor);
    if (result.ec != x5::ec::Success || !result.destination) {
      return {error_packet(result.ec, sequence)};
    }
    if (const auto port = port_of(actor.character_id)) {
      port->travel(*result.destination);
    }
    return local;
  }
  default:
    return local;
  }
}

std::optional<std::vector<WorldPacket>>
HousingHub::receive_part_toggle(const HousingActor &actor, const Json &message,
                                std::uint32_t sequence) {
  const auto param = message.find("param");
  if (param == message.end() || !param->is_object() ||
      !param->contains("change_house_part_state")) {
    return std::nullopt;
  }

  const auto location = location_of(actor.character_id);
  const auto &toggle = param->at("change_house_part_state");
  const auto target = unsigned_field(message, "target_unit_id");
  // The target is the housing unit: (lot << 12) | 5.
  if (!location || (target & 0xFF) != kHousingUnitTypeNibble) {
    return std::vector<WorldPacket>{
        {165, {{"ec", "Housing_HouseNotFound"}, {"packet_seq", sequence_json(sequence)}}}};
  }

  const int lot_number = static_cast<int>((target & 0xFFFF'FFFFu) >> 12);
  auto result = service_->set_house_part(
      actor, location->zone_key, location->channel, lot_number,
      integer_field<std::uint32_t>(toggle, "house_id"),
      integer_field<std::uint32_t>(toggle, "part_attach_index"), bool_field(toggle, "state"));
  std::vector<WorldPacket> local{
      {165,
       {{"ec", x5::ec::EErrorCode_Name(result.ec)}, {"packet_seq", sequence_json(sequence)}}}};
  publish(result.outcome, actor.character_id, actor.character_id, &local);
  return local;
}

namespace {

constexpr char kSheathedWeaponState[] = "EX5WeaponStateType_Sheathed";

// InteractionAnimParam.interaction.interaction_id when the client named the catalog
// interaction it chose (the generic doodad interaction shape used by gathering).
std::optional<int> requested_interaction_id(const Json &message) {
  const auto param = message.find("param");
  if (param == message.end() || !param->is_object()) {
    return std::nullopt;
  }
  const auto interaction = param->find("interaction");
  if (interaction == param->end() || !interaction->is_object() ||
      !interaction->contains("interaction_id")) {
    return std::nullopt;
  }
  return integer_field<int>(*interaction, "interaction_id");
}

// InteractionAnimParam.interaction.execute_craft: a crafting-station craft, which the
// processing service owns even when the station is a placed housing object.
bool requests_craft(const Json &message) {
  const auto param = message.find("param");
  if (param == message.end() || !param->is_object()) {
    return false;
  }
  const auto interaction = param->find("interaction");
  return interaction != param->end() && interaction->is_object() &&
         interaction->contains("execute_craft");
}

WorldPacket start_refusal(ErrorCode code, std::uint32_t sequence) {
  // The captured rejected interaction starts use StartInteractionAnimRes with an error code.
  return {kStartInteractionAnimResponse,
          {{"ec", x5::ec::EErrorCode_Name(code)}, {"packet_seq", sequence_json(sequence)}}};
}

} // namespace

std::optional<std::vector<WorldPacket>>
HousingHub::receive_furniture_pose(const HousingActor &actor, std::uint16_t opcode,
                                   const Json &message, std::uint32_t sequence) {
  try {
    if (opcode == world_request::kInteractionStartRequest) {
      return start_furniture_pose(actor, message, sequence);
    }
    if (opcode == world_request::kInteractionEndRequest) {
      return end_furniture_pose(actor, message);
    }
  } catch (const std::invalid_argument &error) {
    spdlog::warn("Housing furniture request opcode={} rejected: {}", opcode, error.what());
    if (opcode == world_request::kInteractionStartRequest) {
      return std::vector<WorldPacket>{start_refusal(x5::ec::BadRequest, sequence)};
    }
  }
  return std::nullopt;
}

std::optional<std::vector<WorldPacket>>
HousingHub::start_furniture_pose(const HousingActor &actor, const Json &message,
                                 std::uint32_t sequence) {
  const auto target_unit_id = unsigned_field(message, "target_unit_id");
  const auto target = parse_housing_doodad_unit_id(target_unit_id);
  if (!target || requests_craft(message)) {
    // Not a housing object: the housing unit itself (doors), NPCs and world doodads belong to
    // other handlers. Crafts at a placed station belong to processing.
    return std::nullopt;
  }

  const auto animation_id = integer_field<int>(message, "interaction_anim_id");
  const auto location = location_of(actor.character_id);
  FurniturePoseCheck check;
  check.ec = x5::ec::Housing_DoodadNotFound;
  if (location) {
    check = service_->furniture_pose(location->zone_key, location->channel, *target,
                                     animation_id, requested_interaction_id(message));
  }
  spdlog::info("Housing furniture pose character={} lot={} house={} doodad={} animation={} ec={}",
               actor.character_id, target->lot_number, target->house_id, target->doodad_id,
               animation_id, x5::ec::EErrorCode_Name(check.ec));
  if (check.ec != x5::ec::Success) {
    return std::vector<WorldPacket>{start_refusal(check.ec, sequence)};
  }

  WorldPacket packet;
  {
    std::lock_guard lock(mutex_);
    const auto member = members_.find(actor.character_id);
    if (member == members_.end()) {
      return std::vector<WorldPacket>{start_refusal(x5::ec::Interaction_Invalid, sequence)};
    }

    Json notification{{"unit_id", std::to_string(member->second.unit_id)},
                      {"interaction_anim_id", animation_id},
                      {"target_unit_id", std::to_string(target_unit_id)},
                      {"weapon_state_type", kSheathedWeaponState},
                      {"packet_seq", Json::object()}};
    for (const auto *field : {"param", "pos", "yaw", "tick"}) {
      const auto echoed = message.find(field);
      if (echoed != message.end() && !echoed->is_null()) {
        notification[field] = *echoed;
      }
    }
    packet = WorldPacket{kStartInteractionAnimNotification, std::move(notification)};

    member->second.pose_animation_id = animation_id;
    // Sitting holds after its animation, so later viewers are shown it; standing up (and any
    // other pose) ends what was held.
    if (check.holds) {
      member->second.held_pose = packet;
    } else {
      member->second.held_pose.reset();
    }
  }

  send_to_channel(*location, actor.character_id, packet);
  return std::vector<WorldPacket>{packet};
}

std::optional<std::vector<WorldPacket>>
HousingHub::end_furniture_pose(const HousingActor &actor, const Json &message) {
  const auto ended = message.find("end_interaction_anim");
  if (ended == message.end() || !ended->is_object()) {
    return std::nullopt;
  }
  const auto animation_id = integer_field<int>(*ended, "interaction_anim_id");

  std::uint64_t player_unit_id{};
  std::optional<HousingLocation> location;
  {
    std::lock_guard lock(mutex_);
    const auto member = members_.find(actor.character_id);
    if (member == members_.end() || member->second.pose_animation_id != animation_id) {
      // Not this character's furniture pose: gathering or crafting owns the request.
      return std::nullopt;
    }
    member->second.pose_animation_id.reset();
    player_unit_id = member->second.unit_id;
    location = member->second.location;
  }

  // Captured gathering ends answer InteractionAnimRes{unit_id}; the ended pose is echoed so
  // viewers finish it at the reported spot (Candidate).
  const WorldPacket packet{
      kInteractionAnimResponse,
      {{"unit_id", std::to_string(player_unit_id)}, {"end_interaction_anim", *ended}}};
  if (location) {
    send_to_channel(*location, actor.character_id, packet);
  }
  return std::vector<WorldPacket>{packet};
}

void HousingHub::send_to_channel(const HousingLocation &location, CharacterId except,
                                 const WorldPacket &packet) {
  for (const auto viewer : viewers(SiteKey{location.zone_key, location.channel, 0})) {
    if (viewer != except) {
      send(viewer, {packet}, 0, nullptr);
    }
  }
}

std::optional<WorldPacket> HousingHub::late_viewer_pose(const Json &unit_add_message) const {
  const auto unit = unit_add_message.find("unit_id");
  if (unit == unit_add_message.end()) {
    return std::nullopt;
  }
  std::uint64_t unit_id{};
  try {
    unit_id = unsigned_field(unit_add_message, "unit_id");
  } catch (const std::invalid_argument &) {
    return std::nullopt;
  }

  std::lock_guard lock(mutex_);
  for (const auto &[character_id, member] : members_) {
    if (member.unit_id == unit_id && member.held_pose) {
      return member.held_pose;
    }
  }
  return std::nullopt;
}

void HousingHub::observe_movement(CharacterId character_id, const WorldPosition &position) {
  // A seated character does not move; a position this far from where it sat down means the
  // client left the seat without a stand-up request reaching us (Candidate threshold).
  constexpr double kLeftSeatDistanceMm = 1500.0;

  std::lock_guard lock(mutex_);
  const auto found = members_.find(character_id);
  if (found == members_.end() || !found->second.held_pose) {
    return;
  }
  const auto &message = found->second.held_pose->message;
  const auto seat = message.find("pos");
  if (seat == message.end() || !seat->is_object()) {
    found->second.held_pose.reset();
    return;
  }
  const double dx = position.x_mm - seat->value("millimeter_x", 0.0);
  const double dy = position.y_mm - seat->value("millimeter_y", 0.0);
  const double dz = position.z_mm - seat->value("millimeter_z", 0.0);
  if (std::sqrt(dx * dx + dy * dy + dz * dz) > kLeftSeatDistanceMm) {
    found->second.held_pose.reset();
  }
}

WorldPacket HousingHub::interaction_result(std::uint64_t player_unit_id) const {
  // Captured gathering ends and executes answer InteractionAnimRes{unit_id}.
  return {kInteractionAnimResponse, {{"unit_id", std::to_string(player_unit_id)}}};
}

void HousingHub::publish_doodad_phases(const std::vector<DoodadPhaseNotice> &notices,
                                       CharacterId local_character,
                                       std::vector<WorldPacket> *local) {
  for (const auto &notice : notices) {
    const WorldPacket packet{
        kChangeDoodadPhaseNotification,
        {{"unit_id", std::to_string(housing_doodad_unit_id(notice.site.lot_number,
                                                           notice.house_id, notice.doodad_id))},
         {"phase_id", notice.phase},
         {"phase_begin_tick", phase_begin_wire_tick(notice.phase_begin_epoch_ticks)}}};
    for (const auto viewer : viewers(notice.site)) {
      send(viewer, {packet}, local_character, local);
    }
  }
}

namespace {

// An unfinished garden action can be replaced after this long, as for gathering.
constexpr std::int64_t kDoodadUseTimeoutTicks = 30LL * 10'000'000;
// Gathering accepts an execute up to 100 ms before the montage's execute event.
constexpr int kExecuteToleranceMs = 100;
constexpr std::int64_t kTicksPerMillisecond = 10'000;
constexpr std::int64_t kWireNanosecondsPerTick = 100;

// The wire tick of a start: the request's own tick when it is a plausible positive number,
// otherwise the server's monotonic clock in nanoseconds (as gathering does).
std::int64_t request_wire_tick(const Json &message, std::int64_t monotonic_ticks) {
  const auto tick = message.find("tick");
  if (tick != message.end() && tick->is_string()) {
    try {
      const auto value = std::stoll(tick->get<std::string>());
      if (value > 0) {
        return value;
      }
    } catch (const std::exception &) {
      // Fall back to the server clock below.
    }
  }
  return monotonic_ticks * kWireNanosecondsPerTick;
}

WorldPacket common_refusal(ErrorCode code, std::uint32_t sequence) {
  return {kCommonErrorResponse,
          {{"ec", x5::ec::EErrorCode_Name(code)}, {"packet_seq", sequence_json(sequence)}}};
}

} // namespace

std::optional<std::vector<WorldPacket>>
HousingHub::receive_doodad_use(const HousingActor &actor, std::uint16_t opcode,
                               const Json &message, std::uint32_t sequence,
                               const ProgressAccess &access, const HousingVitals &vitals,
                               std::int64_t monotonic_ticks) {
  try {
    if (opcode == world_request::kInteractionStartRequest) {
      const auto target_unit_id = unsigned_field(message, "target_unit_id");
      const auto target = parse_housing_doodad_unit_id(target_unit_id);
      const auto *profile = service_->doodad_profile();
      const auto location = location_of(actor.character_id);
      if (!target || !profile || !location || !requested_interaction_id(message)) {
        return std::nullopt;
      }
      // Only doodads with a phase graph are garden/decoration targets; chairs stay poses.
      const auto site =
          service_->site(SiteKey{location->zone_key, location->channel, target->lot_number});
      if (!site) {
        return std::nullopt;
      }
      const auto doodad = site->contents.doodads.find(DoodadKey{target->house_id,
                                                                target->doodad_id});
      if (doodad == site->contents.doodads.end() || !profile->doodad(doodad->second.cms_id)) {
        return std::nullopt;
      }
      return start_doodad_use(actor, *target, target_unit_id, message, sequence, access,
                              vitals, monotonic_ticks);
    }
    if (opcode == world_request::kInteractionExecuteRequest) {
      return execute_doodad_use(actor, message, sequence, access, vitals, monotonic_ticks);
    }
    if (opcode == world_request::kInteractionEndRequest) {
      return end_doodad_use(actor, message, vitals, monotonic_ticks);
    }
  } catch (const std::invalid_argument &error) {
    spdlog::warn("Housing doodad request opcode={} rejected: {}", opcode, error.what());
    if (opcode == world_request::kInteractionStartRequest) {
      return std::vector<WorldPacket>{start_refusal(x5::ec::BadRequest, sequence)};
    }
  }
  return std::nullopt;
}

std::vector<WorldPacket> HousingHub::start_doodad_use(const HousingActor &actor,
                                                      const DoodadUnitKey &target,
                                                      std::uint64_t target_unit_id,
                                                      const Json &message, std::uint32_t sequence,
                                                      const ProgressAccess &access,
                                                      const HousingVitals &vitals,
                                                      std::int64_t monotonic_ticks) {
  const auto interaction_id = *requested_interaction_id(message);
  const auto animation_id = integer_field<int>(message, "interaction_anim_id");
  const auto location = location_of(actor.character_id);
  std::vector<WorldPacket> local;
  if (!location) {
    local.push_back(start_refusal(x5::ec::Housing_DoodadNotFound, sequence));
    return local;
  }
  if (vitals.dead && vitals.dead()) {
    local.push_back(start_refusal(x5::ec::Interaction_Invalid, sequence));
    return local;
  }

  {
    std::lock_guard lock(mutex_);
    const auto member = members_.find(actor.character_id);
    if (member == members_.end()) {
      local.push_back(start_refusal(x5::ec::Interaction_Invalid, sequence));
      return local;
    }
    auto &open = member->second.doodad_use;
    if (open && monotonic_ticks <= open->started_ticks + kDoodadUseTimeoutTicks &&
        !open->executed) {
      local.push_back(start_refusal(x5::ec::Interaction_Invalid, sequence));
      return local;
    }
    // An expired or finished action is replaced; an expired drain is stopped first.
    if (open && !open->executed && open->stamina_flow && vitals.cancel_stamina) {
      if (auto stopped = vitals.cancel_stamina(monotonic_ticks)) {
        local.push_back(std::move(*stopped));
      }
    }
    open.reset();
  }

  auto check = service_->check_doodad_use(actor, location->zone_key, location->channel, target,
                                          interaction_id, animation_id, &access);
  publish_doodad_phases(check.outcome.doodad_phases, actor.character_id, &local);
  spdlog::info("Housing doodad use character={} lot={} house={} doodad={} interaction={} "
               "animation={} ec={}",
               actor.character_id, target.lot_number, target.house_id, target.doodad_id,
               interaction_id, animation_id, x5::ec::EErrorCode_Name(check.ec));
  if (check.ec != x5::ec::Success || !check.animation) {
    local.push_back(start_refusal(check.ec != x5::ec::Success ? check.ec
                                                              : x5::ec::Interaction_Invalid,
                                  sequence));
    return local;
  }

  const auto wire_tick = request_wire_tick(message, monotonic_ticks);
  bool stamina_flow = false;
  if (vitals.begin_stamina && check.animation->sp_cost > 0) {
    const HousingStaminaFlow flow{
        check.animation->sp_cost, wire_tick, check.animation->execute_time_ms,
        check.animation->duration_ms + service_->doodad_profile()->sp_regen_delay_ms(),
        monotonic_ticks};
    auto announcement = vitals.begin_stamina(flow);
    if (!announcement) {
      local.push_back(start_refusal(x5::ec::Interaction_ConditionsNotMatch, sequence));
      return local;
    }
    // Official gathering order: the SP flow StatSyncNoti precedes StartInteractionAnimNoti.
    local.insert(local.end(), std::make_move_iterator(announcement->begin()),
                 std::make_move_iterator(announcement->end()));
    stamina_flow = true;
  }

  WorldPacket notification;
  {
    std::lock_guard lock(mutex_);
    const auto member = members_.find(actor.character_id);
    if (member == members_.end()) {
      local.push_back(start_refusal(x5::ec::Interaction_Invalid, sequence));
      return local;
    }
    DoodadUse use;
    use.target = target;
    use.target_unit_id = target_unit_id;
    use.interaction_id = interaction_id;
    use.animation_id = animation_id;
    use.phase = check.phase;
    use.started_ticks = monotonic_ticks;
    use.stamina_flow = stamina_flow;
    member->second.doodad_use = use;
    member->second.held_pose.reset();

    // Official gathering StartInteractionAnimNoti echoes the request's param, position, yaw and
    // tick with an empty packet_seq and no target_unit_id.
    Json body{{"unit_id", std::to_string(member->second.unit_id)},
              {"interaction_anim_id", animation_id},
              {"param", message.at("param")},
              {"tick", std::to_string(wire_tick)},
              {"weapon_state_type", kSheathedWeaponState},
              {"packet_seq", Json::object()}};
    for (const auto *field : {"pos", "yaw"}) {
      const auto echoed = message.find(field);
      if (echoed != message.end() && !echoed->is_null()) {
        body[field] = *echoed;
      }
    }
    notification = WorldPacket{kStartInteractionAnimNotification, std::move(body)};
  }

  send_to_channel(*location, actor.character_id, notification);
  local.push_back(std::move(notification));
  return local;
}

std::optional<std::vector<WorldPacket>>
HousingHub::execute_doodad_use(const HousingActor &actor, const Json &message,
                               std::uint32_t sequence, const ProgressAccess &access,
                               const HousingVitals &vitals, std::int64_t monotonic_ticks) {
  const auto animation_id = integer_field<int>(message, "interaction_anim_id");
  DoodadUse use;
  std::uint64_t player_unit_id{};
  std::optional<HousingLocation> location;
  {
    std::lock_guard lock(mutex_);
    const auto member = members_.find(actor.character_id);
    if (member == members_.end() || !member->second.doodad_use ||
        member->second.doodad_use->animation_id != animation_id) {
      return std::nullopt;
    }
    use = *member->second.doodad_use;
    player_unit_id = member->second.unit_id;
    location = member->second.location;
  }

  std::vector<WorldPacket> local;
  if (use.executed) {
    // A retried execute never grants a second reward.
    local.push_back(interaction_result(player_unit_id));
    return local;
  }

  const auto *animation = service_->doodad_profile()->animation(animation_id);
  const auto earliest =
      use.started_ticks + (animation->execute_time_ms - kExecuteToleranceMs) * kTicksPerMillisecond;
  const bool expired = monotonic_ticks > use.started_ticks + kDoodadUseTimeoutTicks;
  if (monotonic_ticks < earliest || expired || !location) {
    // Too early: the client cannot have reached its execute event. Expired: the action was
    // abandoned. Either way the action ends without a reward.
    {
      std::lock_guard lock(mutex_);
      const auto member = members_.find(actor.character_id);
      if (member != members_.end()) {
        member->second.doodad_use.reset();
      }
    }
    if (use.stamina_flow && vitals.cancel_stamina) {
      if (auto stopped = vitals.cancel_stamina(monotonic_ticks)) {
        local.push_back(std::move(*stopped));
      }
    }
    local.push_back(common_refusal(x5::ec::Interaction_Invalid, sequence));
    return local;
  }

  auto result = service_->complete_doodad_use(actor, location->zone_key, location->channel,
                                              use.target, use.interaction_id, use.phase,
                                              &access);
  spdlog::info("Housing doodad execute character={} lot={} house={} doodad={} interaction={} "
               "next_phase={} rewards={} ec={}",
               actor.character_id, use.target.lot_number, use.target.house_id,
               use.target.doodad_id, use.interaction_id, result.next_phase,
               result.rewards.size(), x5::ec::EErrorCode_Name(result.ec));
  if (result.ec != x5::ec::Success) {
    {
      std::lock_guard lock(mutex_);
      const auto member = members_.find(actor.character_id);
      if (member != members_.end()) {
        member->second.doodad_use.reset();
      }
    }
    if (use.stamina_flow && vitals.cancel_stamina) {
      if (auto stopped = vitals.cancel_stamina(monotonic_ticks)) {
        local.push_back(std::move(*stopped));
      }
    }
    local.push_back(common_refusal(result.ec, sequence));
    return local;
  }

  {
    std::lock_guard lock(mutex_);
    const auto member = members_.find(actor.character_id);
    if (member != members_.end() && member->second.doodad_use) {
      member->second.doodad_use->executed = true;
    }
  }
  if (use.stamina_flow && vitals.spend_stamina) {
    vitals.spend_stamina(monotonic_ticks);
  }

  // Captured gathering order: ItemsUpdateNoti, ChangeDoodadPhaseNoti, InteractionAnimRes.
  if (!result.changed_stacks.empty()) {
    Json stackables = Json::array();
    for (const auto &[item_cms_id, quantity] : result.changed_stacks) {
      stackables.push_back({{"item_cms_id", item_cms_id}, {"quantity", quantity}});
    }
    Json update{{"stackables", std::move(stackables)}, {"packet_seq", Json::object()}};
    if (result.looting_reason) {
      update["looting"] = {{"target_unit_id", std::to_string(use.target_unit_id)}};
    } else {
      update["system"] = Json::object();
    }
    local.push_back({kItemsUpdateNotification, std::move(update)});
  }
  publish_doodad_phases(result.outcome.doodad_phases, actor.character_id, &local);
  const auto finished = interaction_result(player_unit_id);
  send_to_channel(*location, actor.character_id, finished);
  local.push_back(finished);
  return local;
}

std::optional<std::vector<WorldPacket>> HousingHub::end_doodad_use(const HousingActor &actor,
                                                                   const Json &message,
                                                                   const HousingVitals &vitals,
                                                                   std::int64_t monotonic_ticks) {
  const auto ended = message.find("end_interaction_anim");
  if (ended == message.end() || !ended->is_object()) {
    return std::nullopt;
  }
  const auto animation_id = integer_field<int>(*ended, "interaction_anim_id");

  DoodadUse use;
  std::uint64_t player_unit_id{};
  std::optional<HousingLocation> location;
  {
    std::lock_guard lock(mutex_);
    const auto member = members_.find(actor.character_id);
    if (member == members_.end() || !member->second.doodad_use ||
        member->second.doodad_use->animation_id != animation_id) {
      return std::nullopt;
    }
    use = *member->second.doodad_use;
    member->second.doodad_use.reset();
    player_unit_id = member->second.unit_id;
    location = member->second.location;
  }

  // Ending before the execute cancels the action; nothing was spent or granted.
  std::vector<WorldPacket> local;
  if (!use.executed && use.stamina_flow && vitals.cancel_stamina) {
    if (auto stopped = vitals.cancel_stamina(monotonic_ticks)) {
      local.push_back(std::move(*stopped));
    }
  }
  const auto finished = interaction_result(player_unit_id);
  if (location) {
    send_to_channel(*location, actor.character_id, finished);
  }
  local.push_back(finished);
  return local;
}

namespace {

// GmCmd oneof fields the housing GM commands use (gmcmd.proto field numbers in comments).
const std::vector<std::string> kHousingGmCommands = {
    "release_housing",                  // 56
    "teleport_to_home",                 // 74
    "housing_remained_expiry_minutes",  // 96
    "contract_housing_guild",           // 103
    "release_housing_guild",            // 104
    "housing_move_package_unpack",      // 115
    "housing_move_package_recover",     // 116
    "housing_unlock",                   // 123
    "housing_account_reset",            // 126
};

bool is_housing_gm_command(const std::string &name) {
  return std::ranges::find(kHousingGmCommands, name) != kHousingGmCommands.end();
}

// Emulator-defined text command (no GmCmd field exists for a personal contract). It rents a lot
// through the same service path as WSC2F_HousingSiteContractReq, so a GM can test housing
// without the client's contract window.
constexpr char kContractGmCommand[] = "housing_contract";
constexpr char kContractGmUsage[] = "Usage: housing_contract <small|medium|large> "
                                    "[monthly|periodic]";

bool is_housing_gm_text_command(const std::string &name) {
  return is_housing_gm_command(name) || name == kContractGmCommand;
}

std::vector<std::string> split_words(const std::string &text) {
  std::vector<std::string> words;
  std::string current;
  for (const auto character : text) {
    if (std::isspace(static_cast<unsigned char>(character))) {
      if (!current.empty()) {
        words.push_back(std::move(current));
        current.clear();
      }
      continue;
    }
    current.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
  }
  if (!current.empty()) {
    words.push_back(std::move(current));
  }
  return words;
}

int word_integer(const std::vector<std::string> &words, std::size_t index) {
  if (index >= words.size()) {
    throw std::invalid_argument("a number argument is missing");
  }
  return integer_field<int>(Json{{"value", words[index]}}, "value");
}

int site_type_word(const std::string &word) {
  if (word == "small") {
    return static_cast<int>(SiteType::small);
  }
  if (word == "medium") {
    return static_cast<int>(SiteType::medium);
  }
  if (word == "large") {
    return static_cast<int>(SiteType::large);
  }
  throw std::invalid_argument("unknown site type; use small, medium or large");
}

int contract_word(const std::string &word) {
  if (word == "monthly") {
    return static_cast<int>(ContractType::monthly);
  }
  if (word == "periodic") {
    return static_cast<int>(ContractType::periodic);
  }
  if (word == "close") {
    return static_cast<int>(ContractType::close);
  }
  throw std::invalid_argument("unknown contract type; use monthly, periodic or close");
}

// Translates one text line into the GmCmd field it names and that field's arguments.
std::optional<std::pair<std::string, Json>> parse_housing_gm_text(const std::string &text) {
  const auto words = split_words(text);
  if (words.empty() || !is_housing_gm_text_command(words.front())) {
    return std::nullopt;
  }

  const auto &command = words.front();
  Json arguments = Json::object();
  if (command == kContractGmCommand) {
    if (words.size() < 2) {
      throw std::invalid_argument(kContractGmUsage);
    }
    arguments["site_type"] = site_type_word(words[1]);
    if (words.size() > 2) {
      arguments["contract"] = contract_word(words[2]);
    }
  } else if (command == "release_housing" && words.size() > 1) {
    arguments["lot_number"] = word_integer(words, 1);
  } else if (command == "release_housing_guild" || command == "contract_housing_guild") {
    arguments["lot_number"] = word_integer(words, 1);
  } else if (command == "housing_unlock") {
    if (words.size() < 2) {
      throw std::invalid_argument("Usage: housing_unlock <small|medium|large|guild> [revoke]");
    }
    const bool guild = words[1] == "guild";
    if (!guild) {
      arguments["site_type"] = site_type_word(words[1]);
    }
    arguments["guild"] = guild;
    arguments["grant"] = words.size() < 3 || words[2] != "revoke";
  } else if (command == "housing_remained_expiry_minutes") {
    arguments["minutes"] = word_integer(words, 1);
    if (words.size() > 2) {
      arguments["contract"] = contract_word(words[2]);
    }
  } else if (command == "housing_move_package_unpack") {
    arguments["lot_number"] = word_integer(words, 1);
    arguments["package_id"] = std::to_string(word_integer(words, 2));
  } else if (command == "housing_move_package_recover") {
    auto ids = Json::array();
    for (std::size_t index = 1; index < words.size(); ++index) {
      ids.push_back(std::to_string(word_integer(words, index)));
    }
    arguments["package_ids"] = std::move(ids);
  }
  return std::pair{command, std::move(arguments)};
}

// The chat line for a refused GM housing command, with a next step for the refusals a GM
// testing housing meets first.
std::string gm_refusal_message(const std::string &command, ErrorCode code) {
  std::string message = command + " refused: " + x5::ec::EErrorCode_Name(code);
  if (code == x5::ec::Housing_NotRegisteredHousingSite) {
    message += ". You have no lot; rent one with housing_contract small.";
  } else if (code == x5::ec::Housing_RentItemNotEnough) {
    message += ". Get housing permits with item_add 123 <count>.";
  } else if (code == x5::ec::Housing_CanHaveOnlyOneHousingSite) {
    message += ". You already rent a lot; free it with release_housing.";
  }
  return message;
}

} // namespace

std::optional<GmHousingReply> HousingHub::receive_gm(const HousingActor &actor,
                                                     std::uint16_t opcode, const Json &message,
                                                     std::uint32_t sequence,
                                                     const ProgressAccess &access) {
  using world_request::kGmCommandRequest;
  using world_request::kGmCommandTextRequest;

  std::vector<std::pair<std::string, Json>> commands;
  try {
    if (opcode == kGmCommandTextRequest) {
      auto parsed = parse_housing_gm_text(message.value("text", std::string()));
      if (!parsed) {
        return std::nullopt;
      }
      commands.push_back(std::move(*parsed));
    } else if (opcode == kGmCommandRequest) {
      for (const auto &command : array_field(message, "cmds")) {
        if (!command.is_object() || command.size() != 1 ||
            !is_housing_gm_command(command.begin().key())) {
          return std::nullopt;
        }
        commands.emplace_back(command.begin().key(), command.begin().value());
      }
      if (commands.empty()) {
        return std::nullopt;
      }
    } else {
      return std::nullopt;
    }
  } catch (const std::invalid_argument &error) {
    spdlog::info("GM housing command not executed for character {}: {}", actor.character_id,
                 error.what());
    GmHousingReply refused;
    refused.packets.push_back(error_packet(x5::ec::BadRequest, sequence));
    refused.messages.push_back(std::string("Housing command not executed: ") + error.what());
    return refused;
  }

  GmHousingReply reply;
  for (const auto &[name, arguments] : commands) {
    SimpleResult result;
    std::string rejection;
    try {
      result = execute_gm(actor, name, arguments, access);
    } catch (const std::invalid_argument &error) {
      spdlog::info("GM housing command {} rejected: {}", name, error.what());
      result.ec = x5::ec::BadRequest;
      rejection = error.what();
    }
    spdlog::info("GM housing command {} character={} ec={}", name, actor.character_id,
                 x5::ec::EErrorCode_Name(result.ec));

    if (result.ec != x5::ec::Success) {
      reply.packets.push_back(error_packet(result.ec, sequence));
      if (rejection.empty()) {
        reply.messages.push_back(gm_refusal_message(name, result.ec));
      } else {
        reply.messages.push_back(name + " refused: " + rejection);
      }
      break;
    }

    publish(result.outcome, actor.character_id, actor.character_id, &reply.packets);
    reply.messages.push_back(gm_success_message(actor, name));
  }
  return reply;
}

std::string HousingHub::gm_success_message(const HousingActor &actor,
                                           const std::string &command) const {
  if (command == kContractGmCommand) {
    const auto site = service_->personal_site(actor.character_id);
    if (site) {
      const auto *definition = catalog().site(site->key.lot_number);
      const auto size = definition ? site_type_name(definition->type) : std::string("personal");
      return "Rented " + size + " lot " + std::to_string(site->key.lot_number) +
             " in housing channel " + std::to_string(site->key.channel) +
             ". Use teleport_to_home to go there.";
    }
  }
  if (command == "teleport_to_home") {
    return "Moving to your housing lot.";
  }
  return command + " applied.";
}

SimpleResult HousingHub::execute_gm(const HousingActor &actor, const std::string &command,
                                    const Json &arguments, const ProgressAccess &access) {
  auto &service = *service_;
  const auto location = location_of(actor.character_id);
  const int channel = location ? location->channel : 1;

  if (command == kContractGmCommand) {
    const auto zones = service.catalog().rentable_zones();
    if (zones.empty()) {
      return SimpleResult{x5::ec::Housing_InvalidZoneKey, {}};
    }

    const auto site_type = static_cast<SiteType>(integer_field<int>(arguments, "site_type"));
    // Monthly is the client's plain contract; periodic renews itself from permits at expiry.
    const auto contract_type = arguments.contains("contract")
                                   ? contract_type_field(arguments, "contract")
                                   : ContractType::monthly;
    auto contracted =
        service.contract(actor, site_type, zones.front()->zone_key, contract_type, &access);
    return SimpleResult{contracted.ec, std::move(contracted.outcome)};
  }

  if (command == "release_housing") {
    const int lot_number = integer_field<int>(arguments, "lot_number");
    if (lot_number == 0) {
      return service.release(actor, PackageReason::cheat, &access);
    }
    return service.gm_release(lot_number, channel);
  }
  if (command == "release_housing_guild") {
    return service.gm_release_guild(integer_field<int>(arguments, "lot_number"), channel);
  }
  if (command == "contract_housing_guild") {
    return service.gm_contract_guild(actor, integer_field<int>(arguments, "lot_number"));
  }
  if (command == "housing_unlock") {
    const auto type = static_cast<SiteType>(
        enum_field(arguments, "site_type", "EX5HousingSiteType_", {"None", "Small", "Medium",
                                                                   "Large"}));
    return service.gm_set_unlock(actor, type, bool_field(arguments, "guild"),
                                 bool_field(arguments, "grant"));
  }
  if (command == "housing_remained_expiry_minutes") {
    return service.gm_set_remaining_minutes(actor, integer_field<int>(arguments, "minutes"),
                                            contract_type_field(arguments, "contract"));
  }
  if (command == "housing_account_reset") {
    return service.gm_reset_account(actor, &access);
  }
  if (command == "housing_move_package_unpack") {
    auto result = service.unpack(actor, integer_field<std::int64_t>(arguments, "package_id"),
                                 integer_field<int>(arguments, "lot_number"), &access);
    return SimpleResult{result.ec, std::move(result.outcome)};
  }
  if (command == "housing_move_package_recover") {
    std::vector<std::int64_t> package_ids;
    for (const auto &entry : array_field(arguments, "package_ids")) {
      package_ids.push_back(integer_field<std::int64_t>(Json{{"id", entry}}, "id"));
    }
    auto result =
        service.recover_to_storage(actor, package_ids, package_ids.empty(), &access);
    return SimpleResult{result.ec, std::move(result.outcome)};
  }
  if (command == "teleport_to_home") {
    // A GM teleport skips the GoHome cooldown: it visits the owner's channel directly.
    const auto site = service.personal_site(actor.character_id);
    if (!site) {
      return SimpleResult{x5::ec::Housing_NotRegisteredHousingSite, {}};
    }
    const auto visit = service.visit(actor, site->key.zone_key, site->key.channel);
    if (visit.ec == x5::ec::Success) {
      if (const auto port = port_of(actor.character_id)) {
        port->travel(*visit.destination);
      }
    }
    return SimpleResult{visit.ec, {}};
  }
  throw std::invalid_argument("Unknown GM housing command");
}

bool HousingHub::run_for(CharacterId character_id,
                         const std::function<void(const ProgressAccess *)> &operation) {
  const auto port = port_of(character_id);
  if (!port) {
    operation(nullptr);
    return true;
  }

  // A timed-out caller may own the operation's captured references. The posted task
  // must claim the operation before the caller can abandon it; once claimed, the
  // caller waits for completion so those references remain alive.
  enum class TaskState { pending, running, abandoned };
  auto completed = std::make_shared<std::promise<void>>();
  auto future = completed->get_future();
  auto state = std::make_shared<std::atomic<TaskState>>(TaskState::pending);
  const bool posted = port->post([operation, completed, state](const ProgressAccess &access) {
    auto expected = TaskState::pending;
    if (!state->compare_exchange_strong(expected, TaskState::running)) {
      return;
    }
    try {
      operation(&access);
      completed->set_value();
    } catch (...) {
      completed->set_exception(std::current_exception());
    }
  });
  if (!posted) {
    operation(nullptr);
    return true;
  }

  if (future.wait_for(session_task_timeout_) != std::future_status::ready) {
    auto expected = TaskState::pending;
    if (state->compare_exchange_strong(expected, TaskState::abandoned)) {
      spdlog::warn("Housing operation for character={} timed out before it started",
                   character_id);
      return false;
    }
    // A running operation can commit and still uses the caller's captures.
    // Wait for its actual outcome instead of reporting a misleading timeout.
  }

  future.get();
  return true;
}

bool HousingHub::travel(CharacterId character_id, const HousingDestination &destination) {
  const auto port = port_of(character_id);
  if (!port) {
    return false;
  }
  return port->post([port, destination](const ProgressAccess &) { port->travel(destination); });
}

void HousingHub::publish(const HousingOutcome &outcome, CharacterId adopted_character,
                         CharacterId local_character, std::vector<WorldPacket> *local) {
  const auto &catalog = service_->catalog();
  const auto to_viewers = [&](const SiteKey &key, const WorldPacket &packet) {
    for (const auto viewer : viewers(key)) {
      send(viewer, {packet}, local_character, local);
    }
  };

  for (const auto &key : outcome.sites_added) {
    if (const auto site = service_->site(key)) {
      to_viewers(key, housing_add_packet(*site, catalog));
    }
  }
  for (const auto &key : outcome.contracts_changed) {
    if (const auto site = service_->site(key)) {
      to_viewers(key, site_contract_packet(*site, catalog));
    }
  }
  for (const auto &key : outcome.sites_removed) {
    to_viewers(key, site_removed_packet(key));
  }
  for (const auto &notice : outcome.site_updates) {
    to_viewers(notice.site, site_update_packet(notice));
  }
  for (const auto &notice : outcome.parts_updates) {
    to_viewers(notice.site, parts_update_packet(notice));
  }
  publish_doodad_phases(outcome.doodad_phases, local_character, local);
  for (const auto character_id : outcome.owner_sites_changed) {
    auto site = service_->personal_site(character_id);
    if (!site) {
      site = service_->guild_site_of(character_id);
    }
    send(character_id, {owner_site_packet(site)}, local_character, local);
  }
  for (const auto &notice : outcome.progress_changes) {
    if (const auto packet = progress_update_packet(notice)) {
      send(notice.character_id, {*packet}, local_character, local);
    }
    // Another character's session caches its progress revision; let it adopt the commit.
    if (notice.character_id != adopted_character) {
      if (const auto port = port_of(notice.character_id)) {
        const auto committed = notice.after;
        port->post([committed](const ProgressAccess &access) {
          if (access.adopt) {
            access.adopt(committed);
          }
        });
      }
    }
  }
  for (const auto &notice : outcome.packages_created) {
    send(notice.character_id, {package_created_packet(notice.reason)}, local_character, local);
  }
  for (const auto &notice : outcome.guild_sites) {
    SiteRecord site;
    if (const auto current = service_->site(notice.site)) {
      site = *current;
    } else {
      site.key = notice.site;
      site.authority = AuthorityType::guild;
      site.owner_character_id = notice.character_id;
    }
    send(notice.character_id, {guild_site_packet(site, notice.action)}, local_character, local);
  }

  GuildAreaListener listener;
  {
    std::lock_guard lock(mutex_);
    listener = guild_area_listener_;
  }
  if (listener) {
    for (const auto &notice : outcome.guild_areas) {
      listener(notice);
    }
  }
}

void HousingHub::expire_due() {
  reconcile_guild_housing();

  // Garden growth, rest and decay timers: phases that elapsed since the last sweep advance and
  // are announced to everyone standing in the lot's channel.
  publish(service_->advance_due_doodads(), 0, 0, nullptr);

  for (const auto &due : service_->due_sites()) {
    SimpleResult result;
    const bool ran = run_for(due.owner_character_id, [&](const ProgressAccess *access) {
      result = service_->expire_site(due.site, access);
    });
    if (ran) {
      publish(result.outcome, due.owner_character_id, 0, nullptr);
    }
  }
  for (const auto &due : service_->due_areas()) {
    SimpleResult result;
    const auto payer = due.master_character_id;
    const bool ran = run_for(payer, [&](const ProgressAccess *access) {
      result = service_->expire_area(due.zone_key, due.channel, access);
    });
    if (ran) {
      publish(result.outcome, payer, 0, nullptr);
    }
  }
}

bool HousingHub::guild_member_left(GuildId guild_id, CharacterId character_id, bool kicked) {
  try {
    SimpleResult result;
    const bool ran = run_for(character_id, [&](const ProgressAccess *access) {
      result = service_->guild_member_left(guild_id, character_id, access, kicked);
    });
    if (!ran) {
      spdlog::warn("Guild site cleanup guild={} character={} timed out; it will be retried",
                   guild_id, character_id);
      return false;
    }
    if (result.ec != x5::ec::Success) {
      spdlog::warn("Guild site cleanup guild={} character={} failed: {}; it will be retried",
                   guild_id, character_id, x5::ec::EErrorCode_Name(result.ec));
      return false;
    }

    publish(result.outcome, character_id, 0, nullptr);
    return true;
  } catch (const std::exception &failure) {
    spdlog::error("Guild site cleanup guild={} character={} failed: {}; it will be retried",
                  guild_id, character_id, failure.what());
  } catch (...) {
    spdlog::error("Guild site cleanup guild={} character={} failed with an unknown error; "
                  "it will be retried",
                  guild_id, character_id);
  }
  return false;
}

bool HousingHub::guild_disbanded(GuildId guild_id) {
  try {
    const auto result = service_->guild_disbanded(guild_id);
    if (result.ec != x5::ec::Success) {
      spdlog::warn("Guild area cleanup guild={} failed: {}; it will be retried", guild_id,
                   x5::ec::EErrorCode_Name(result.ec));
      return false;
    }

    publish(result.outcome);
    return true;
  } catch (const std::exception &failure) {
    spdlog::error("Guild area cleanup guild={} failed: {}; it will be retried", guild_id,
                  failure.what());
  } catch (...) {
    spdlog::error("Guild area cleanup guild={} failed with an unknown error; it will be retried",
                  guild_id);
  }
  return false;
}

std::size_t HousingHub::reconcile_guild_housing() {
  HousingService::StaleGuildHousing stale;
  try {
    stale = service_->stale_guild_housing();
  } catch (const std::exception &failure) {
    spdlog::error("Guild housing reconciliation could not read guild state: {}",
                  failure.what());
    return 1;
  }
  if (stale.empty()) {
    return 0;
  }

  // Departed residents first: a disbanded guild's area then packs only what remains in it.
  std::size_t failures = 0;
  for (const auto &departed : stale.departed_residents) {
    spdlog::info("Releasing the guild site of departed character={} guild={}",
                 departed.character_id, departed.guild_id);
    if (!guild_member_left(departed.guild_id, departed.character_id)) {
      ++failures;
    }
  }
  for (const auto guild_id : stale.disbanded_guilds) {
    spdlog::info("Releasing the guild housing area of disbanded guild={}", guild_id);
    if (!guild_disbanded(guild_id)) {
      ++failures;
    }
  }
  return failures;
}

} // namespace chronicles::housing
