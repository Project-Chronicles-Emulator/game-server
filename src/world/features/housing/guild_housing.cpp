#include "world/features/housing/guild_housing.hpp"

#include "p_c2f.ws.guild.pb.h"
#include "p_f2c.ws.guild.pb.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <variant>

namespace chronicles {
namespace {

using native_ws::parse_payload;
namespace wire = x5::pb::guild;

bool valid_contract_type(int value) {
  return x5::cms::EX5HousingContractType_IsValid(value);
}

bool valid_zone_key(int value) {
  return x5::cms::EX5ZoneKey_IsValid(value);
}

x5::cms::EX5GuildMemberGrade wire_grade(GuildGrade grade) {
  return static_cast<x5::cms::EX5GuildMemberGrade>(grade);
}

std::vector<housing::GuildResident>
residents_from(const google::protobuf::RepeatedPtrField<x5::pb::GuildHousingAreaBoardResidents>
                   &entries) {
  std::vector<housing::GuildResident> residents;
  residents.reserve(static_cast<std::size_t>(entries.size()));
  for (const auto &entry : entries) {
    housing::GuildResident resident;
    resident.lot_number = entry.lot_number();
    resident.character_id = entry.char_id();
    residents.push_back(resident);
  }
  return residents;
}

} // namespace

namespace guild_housing {

x5::pb::HousingSite guild_housing_site(const housing::SiteKey &key,
                                       const std::optional<housing::SiteRecord> &site,
                                       const std::optional<housing::ChannelRecord> &area) {
  x5::pb::HousingSite message;
  message.set_lot_number(key.lot_number);
  message.set_channel(key.channel);
  message.set_authority_type(x5::cms::EX5HousingAuthorityType_Guild);
  auto *guild_housing = message.mutable_guild_housing();
  guild_housing->set_guild_channel(key.channel);
  if (valid_zone_key(key.zone_key)) {
    guild_housing->set_guild_zone_key(static_cast<x5::cms::EX5ZoneKey>(key.zone_key));
  }

  if (site) {
    message.set_expiry_epochtime(site->expiry_epoch_ticks);
    message.set_contract_type(static_cast<x5::cms::EX5HousingContractType>(site->contract_type));
    if (site->return_position) {
      auto *position = message.mutable_return_pos();
      position->set_millimeter_x(site->return_position->x_mm);
      position->set_millimeter_y(site->return_position->y_mm);
      position->set_millimeter_z(site->return_position->z_mm);
    }
  } else if (area) {
    message.set_expiry_epochtime(area->expiry_epoch_ticks);
    message.set_contract_type(static_cast<x5::cms::EX5HousingContractType>(area->contract_type));
  }
  return message;
}

} // namespace guild_housing

housing::GuildGrade housing_guild_grade(GuildGrade grade) {
  switch (grade) {
  case GuildGrade::master:
    return housing::GuildGrade::master;
  case GuildGrade::sub_master:
    return housing::GuildGrade::sub_master;
  case GuildGrade::member:
    return housing::GuildGrade::member;
  case GuildGrade::none:
    break;
  }
  return housing::GuildGrade::none;
}

GuildGrade guild_grade_from_housing(housing::GuildGrade grade) {
  switch (grade) {
  case housing::GuildGrade::master:
    return GuildGrade::master;
  case housing::GuildGrade::sub_master:
    return GuildGrade::sub_master;
  case housing::GuildGrade::member:
    return GuildGrade::member;
  case housing::GuildGrade::none:
    break;
  }
  return GuildGrade::none;
}

LegacyGuildServiceDirectory::LegacyGuildServiceDirectory(std::shared_ptr<GuildService> guilds)
    : guilds_(std::move(guilds)) {
  if (!guilds_) {
    throw std::invalid_argument("Guild housing directory requires a guild service");
  }
}

std::optional<housing::GuildMembership>
LegacyGuildServiceDirectory::membership(housing::CharacterId character_id) const {
  const auto status = guilds_->status(character_id);
  if (status.guild_id == 0) {
    return std::nullopt;
  }

  housing::GuildMembership membership;
  membership.guild_id = status.guild_id;
  membership.guild_name = status.guild_name;
  membership.grade = housing_guild_grade(status.grade);
  membership.guild_level = status.guild_level;
  membership.max_member_count = static_cast<int>(status.max_members);
  return membership;
}

std::vector<housing::GuildMember>
LegacyGuildServiceDirectory::members(housing::GuildId guild_id) const {
  std::vector<housing::GuildMember> members;
  for (const auto &record : guilds_->members(guild_id)) {
    housing::GuildMember member;
    member.character_id = record.char_id;
    member.account_id = record.account_id;
    member.name = record.name;
    member.grade = housing_guild_grade(record.grade);
    member.company_position_level = record.company_position;
    members.push_back(std::move(member));
  }
  return members;
}

std::optional<bool> LegacyGuildServiceDirectory::guild_exists(housing::GuildId guild_id) const {
  // Every live guild has at least its Master; a disbanded guild has no roster.
  return !guilds_->members(guild_id).empty();
}

GuildHousingCoordinator::GuildHousingCoordinator(
    std::shared_ptr<housing::HousingService> housing, std::shared_ptr<GuildService> guilds)
    : housing_(std::move(housing)), guilds_(std::move(guilds)) {
  if (!housing_ || !guilds_) {
    throw std::invalid_argument("Guild housing requires housing and guild services");
  }
}

void GuildHousingCoordinator::set_world_bridge(std::shared_ptr<HousingWorldBridge> bridge) {
  std::lock_guard lock(wiring_mutex_);
  bridge_ = std::move(bridge);
}

void GuildHousingCoordinator::set_front_delivery(FrontDelivery delivery) {
  std::lock_guard lock(wiring_mutex_);
  delivery_ = std::move(delivery);
}

housing::HousingService &GuildHousingCoordinator::housing() {
  return *housing_;
}

const housing::HousingService &GuildHousingCoordinator::housing() const {
  return *housing_;
}

std::optional<x5::pb::guild::GuildHousingArea>
GuildHousingCoordinator::housing_area(std::int64_t guild_id) const {
  const auto area = housing_->guild_area(guild_id);
  if (!area || area->expiry_epoch_ticks <= 0) {
    return std::nullopt;
  }

  x5::pb::guild::GuildHousingArea message;
  message.set_housing_channel(area->channel);
  message.set_housing_expiry_epochtime(area->expiry_epoch_ticks);
  message.set_area_name(area->area_name);
  message.set_contract_type(static_cast<x5::cms::EX5HousingContractType>(area->contract_type));
  if (valid_zone_key(area->zone_key)) {
    message.set_zone_key(static_cast<x5::cms::EX5ZoneKey>(area->zone_key));
  }
  return message;
}

std::string GuildHousingCoordinator::handle(const GuildHousingRequester &requester,
                                            native_ws::FrameView frame) {
  housing::HousingActor actor;
  actor.account_id = requester.account_id;
  actor.character_id = requester.char_id;
  actor.world_id = requester.world_id;
  actor.name = requester.name;

  switch (frame.packet_id) {
  case guild_housing::kAreaContractRequest:
    return contract(actor, frame);
  case guild_housing::kSiteAllowMemberRequest:
    return allow(actor, frame);
  case guild_housing::kSiteAreaBoardGetRequest:
    return board(actor, frame);
  case guild_housing::kSiteAssignmentRequest:
    return assignment(actor, frame);
  case guild_housing::kAreaContractExtensionRequest:
    return extend(actor, frame);
  default:
    throw std::invalid_argument("Unsupported guild housing packet ID " +
                                std::to_string(frame.packet_id));
  }
}

std::string GuildHousingCoordinator::contract(const housing::HousingActor &actor,
                                              native_ws::FrameView frame) {
  const auto request = parse_payload<wire::GuildHousingAreaContractReq>(frame.payload);
  const auto respond = [this, character_id = actor.character_id](
                           const housing::GuildAreaResult &result) {
    wire::GuildHousingAreaContractRes response;
    response.set_ec(result.ec);
    if (result.ec == x5::ec::Success) {
      response.set_guild_channel(result.channel);
      response.set_expiry_epochtime(result.expiry_epoch_ticks);
      response.set_contract_type(
          static_cast<x5::cms::EX5HousingContractType>(result.contract_type));
      response.set_area_name(result.area_name);
      if (valid_zone_key(result.zone_key)) {
        response.set_zone_key(static_cast<x5::cms::EX5ZoneKey>(result.zone_key));
      }
    }
    send(character_id,
         native_ws::encode_frame(guild_housing::kAreaContractResponse, response));
  };

  const auto contract_type = static_cast<int>(request.contract_type());
  if (!valid_contract_type(contract_type) || !valid_zone_key(request.zone_key())) {
    housing::GuildAreaResult refused;
    refused.ec = x5::ec::Housing_InvalidContractType;
    respond(refused);
    return {};
  }

  // The master pays rent from live progress, so the work runs on the master's session.
  const auto guild_id = request.guild_id();
  const auto zone_key = static_cast<int>(request.zone_key());
  const auto area_name = request.area_name();
  const auto run = [this, actor, guild_id, zone_key, contract_type, area_name,
                    respond](const housing::ProgressAccess *access) {
    const auto result =
        housing_->guild_contract(actor, guild_id, zone_key,
                                 static_cast<housing::ContractType>(contract_type), area_name,
                                 access);
    respond(result);
    if (result.ec == x5::ec::Success) {
      publish(result.outcome, AreaNotice::contract, actor.character_id);
    }
  };

  const auto world = bridge();
  if (world) {
    world->with_live_progress(actor.character_id, run);
  } else {
    run(nullptr);
  }
  return {};
}

std::string GuildHousingCoordinator::extend(const housing::HousingActor &actor,
                                            native_ws::FrameView frame) {
  const auto request = parse_payload<wire::GuildHousingAreaContractExtensionReq>(frame.payload);
  const auto respond = [this, character_id = actor.character_id](
                           const housing::GuildAreaResult &result) {
    wire::GuildHousingAreaContractExtensionRes response;
    response.set_ec(result.ec);
    if (result.ec == x5::ec::Success) {
      response.set_expiry_epochtime(result.expiry_epoch_ticks);
      response.set_contract_type(
          static_cast<x5::cms::EX5HousingContractType>(result.contract_type));
    }
    send(character_id,
         native_ws::encode_frame(guild_housing::kAreaContractExtensionResponse, response));
  };

  const auto contract_type = static_cast<int>(request.contract_type());
  if (!valid_contract_type(contract_type)) {
    housing::GuildAreaResult refused;
    refused.ec = x5::ec::Housing_InvalidContractType;
    respond(refused);
    return {};
  }

  const auto guild_id = request.guild_id();
  const auto run = [this, actor, guild_id, contract_type,
                    respond](const housing::ProgressAccess *access) {
    const auto result = housing_->guild_extend(
        actor, guild_id, static_cast<housing::ContractType>(contract_type), access);
    respond(result);
    if (result.ec == x5::ec::Success) {
      publish(result.outcome, AreaNotice::extension, actor.character_id);
    }
  };

  const auto world = bridge();
  if (world) {
    world->with_live_progress(actor.character_id, run);
  } else {
    run(nullptr);
  }
  return {};
}

std::string GuildHousingCoordinator::allow(const housing::HousingActor &actor,
                                           native_ws::FrameView frame) {
  const auto request = parse_payload<wire::GuildHousingSiteAllowMemberReq>(frame.payload);
  const auto result = housing_->guild_allow(actor, request.guild_id(),
                                            residents_from(request.allow_lots()),
                                            residents_from(request.remove_lots()));

  wire::GuildHousingSiteAllowMemberRes response;
  response.set_ec(result.ec);
  if (result.ec == x5::ec::Success) {
    const auto roster = guilds_->members(request.guild_id());
    for (const auto &resident : result.residents) {
      auto *entry = response.add_residents();
      entry->set_lot_number(resident.lot_number);
      entry->set_char_id(resident.character_id);
      entry->set_name(resident.name);
      entry->set_allow_cooldown_start_epochtime(resident.allow_cooldown_start_ticks);
      const auto member = std::ranges::find(roster, resident.character_id,
                                            &GuildMemberRecord::char_id);
      if (member != roster.end()) {
        entry->set_grade(wire_grade(member->grade));
      }
    }
    publish(result.outcome, AreaNotice::update, actor.character_id);
  }
  return native_ws::encode_frame(guild_housing::kSiteAllowMemberResponse, response);
}

std::string GuildHousingCoordinator::board(const housing::HousingActor &actor,
                                           native_ws::FrameView frame) {
  const auto request = parse_payload<wire::GuildHousingSiteAreaBoardGetReq>(frame.payload);
  const auto result = housing_->guild_board(actor, request.guild_id());

  wire::GuildHousingSiteAreaBoardGetRes response;
  response.set_ec(result.ec);
  if (result.ec == x5::ec::Success) {
    const auto roster = guilds_->members(request.guild_id());
    for (const auto &resident : result.residents) {
      auto *entry = response.add_residents();
      entry->set_lot_number(resident.lot_number);
      entry->set_char_id(resident.character_id);
      entry->set_name(resident.name);
      entry->set_allow_cooldown_start_epochtime(resident.allow_cooldown_start_ticks);
      const auto member = std::ranges::find(roster, resident.character_id,
                                            &GuildMemberRecord::char_id);
      if (member != roster.end()) {
        entry->set_grade(wire_grade(member->grade));
      }
    }
  }
  return native_ws::encode_frame(guild_housing::kSiteAreaBoardGetResponse, response);
}

std::string GuildHousingCoordinator::assignment(const housing::HousingActor &actor,
                                                native_ws::FrameView frame) {
  const auto request = parse_payload<wire::GuildHousingSiteAssignmentReq>(frame.payload);
  const auto result = housing_->guild_assignment(actor, request.guild_id());

  wire::GuildHousingSiteAssignmentRes response;
  response.set_ec(result.ec);
  for (const auto character_id : result.characters_with_sites) {
    response.add_exist_housing_site_char_ids(character_id);
  }
  return native_ws::encode_frame(guild_housing::kSiteAssignmentResponse, response);
}

void GuildHousingCoordinator::on_guild_events(const std::vector<GuildEvent> &events) {
  for (const auto &event : events) {
    if (const auto *left = std::get_if<GuildMemberLeft>(&event)) {
      const auto result = housing_->guild_member_left(left->guild_id, left->char_id,
                                                      nullptr, left->kicked);
      if (result.ec != x5::ec::Success) {
        spdlog::error("Guild site of departed character {} was not released: {}",
                      left->char_id, x5::ec::EErrorCode_Name(result.ec));
      }
      publish(result.outcome, AreaNotice::update, 0);
    } else if (const auto *disbanded = std::get_if<GuildDisbanded>(&event)) {
      const auto result = housing_->guild_disbanded(disbanded->guild_id);
      if (result.ec != x5::ec::Success) {
        spdlog::error("Guild housing of disbanded guild {} was not released: {}",
                      disbanded->guild_id, x5::ec::EErrorCode_Name(result.ec));
      }
      publish(result.outcome, AreaNotice::update, 0);
    }
  }
}

void GuildHousingCoordinator::expire_due_areas() {
  for (const auto &due : housing_->due_areas()) {
    const auto run = [this, due](const housing::ProgressAccess *access) {
      const auto result = housing_->expire_area(due.zone_key, due.channel, access);
      if (result.ec != x5::ec::Success) {
        spdlog::warn("Guild housing area zone={} channel={} expiry failed: {}", due.zone_key,
                     due.channel, x5::ec::EErrorCode_Name(result.ec));
        return;
      }
      const bool renewed = std::ranges::any_of(
          result.outcome.guild_areas, [](const auto &notice) { return notice.extension; });
      publish(result.outcome, renewed ? AreaNotice::extension : AreaNotice::update, 0);
    };

    const auto world = bridge();
    if (world && due.master_character_id != 0) {
      world->with_live_progress(due.master_character_id, run);
    } else {
      run(nullptr);
    }
  }
}

void GuildHousingCoordinator::publish(const housing::HousingOutcome &outcome, AreaNotice kind,
                                      std::int64_t excluded) {
  if (outcome.empty()) {
    return;
  }

  const auto world = bridge();
  if (world) {
    world->deliver(outcome);
  }

  for (const auto &notice : outcome.guild_areas) {
    const auto area = housing_->channel_record(notice.zone_key, notice.channel);
    std::string frame;
    if (kind == AreaNotice::contract && area) {
      wire::GuildHousingAreaContractNoti message;
      message.set_guild_id(notice.guild_id);
      message.set_guild_channel(area->channel);
      message.set_expiry_epochtime(area->expiry_epoch_ticks);
      message.set_contract_type(static_cast<x5::cms::EX5HousingContractType>(area->contract_type));
      message.set_area_name(area->area_name);
      if (valid_zone_key(area->zone_key)) {
        message.set_zone_key(static_cast<x5::cms::EX5ZoneKey>(area->zone_key));
      }
      frame = native_ws::encode_frame(guild_housing::kAreaContractNotification, message);
    } else if (kind == AreaNotice::extension && area) {
      wire::GuildHousingAreaContractExtensionNoti message;
      message.set_guild_id(notice.guild_id);
      message.set_guild_channel(area->channel);
      message.set_expiry_epochtime(area->expiry_epoch_ticks);
      message.set_contract_type(static_cast<x5::cms::EX5HousingContractType>(area->contract_type));
      message.set_area_name(area->area_name);
      if (valid_zone_key(area->zone_key)) {
        message.set_zone_key(static_cast<x5::cms::EX5ZoneKey>(area->zone_key));
      }
      frame =
          native_ws::encode_frame(guild_housing::kAreaContractExtensionNotification, message);
    } else {
      // Residents, expiry and release changes: members refresh the area through their
      // board and guild requests.
      frame = native_ws::encode_frame(guild_housing::kAreaUpdateNotification,
                                      wire::GuildHousingAreaUpdateNoti{});
    }

    for (const auto &member : guilds_->members(notice.guild_id)) {
      if (member.char_id != excluded) {
        send(member.char_id, frame);
      }
    }
  }
}

void GuildHousingCoordinator::send(std::int64_t char_id, const std::string &frame) const {
  FrontDelivery delivery;
  {
    std::lock_guard lock(wiring_mutex_);
    delivery = delivery_;
  }
  if (delivery) {
    delivery(char_id, frame);
  }
}

std::shared_ptr<HousingWorldBridge> GuildHousingCoordinator::bridge() const {
  std::lock_guard lock(wiring_mutex_);
  return bridge_;
}

} // namespace chronicles
