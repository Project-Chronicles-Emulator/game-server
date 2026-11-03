#include "world/features/housing/native_housing.hpp"

#include "auth/auth.hpp"
#include "world/features/chat/native_chat.hpp"
#include "world/features/company/company.hpp"
#include "p_c2f.ws.guild.pb.h"
#include "p_c2f.ws.pb.h"
#include "p_f2c.ws.guild.pb.h"
#include "p_f2c.ws.pb.h"
#include "p_system.pb.h"
#include "persistence/character_repository.hpp"

#include <spdlog/spdlog.h>

#include <stdexcept>

namespace chronicles {
namespace {

using native_ws::parse_payload;
namespace front = x5::pb::ws::front;
namespace response = x5::pb::frontwebsocket;
namespace guild_wire = x5::pb::guild;

housing::SiteType site_type(x5::cms::EX5HousingSiteType type) {
  return static_cast<housing::SiteType>(static_cast<int>(type));
}

housing::ContractType contract_type(x5::cms::EX5HousingContractType type) {
  return static_cast<housing::ContractType>(static_cast<int>(type));
}

housing::AuthorityType authority_type(x5::cms::EX5HousingAuthorityType type) {
  return static_cast<housing::AuthorityType>(static_cast<int>(type));
}

x5::cms::EX5HousingContractType wire_contract(housing::ContractType type) {
  return static_cast<x5::cms::EX5HousingContractType>(static_cast<int>(type));
}

x5::cms::EX5HousingSiteType wire_site_type(housing::SiteType type) {
  return static_cast<x5::cms::EX5HousingSiteType>(static_cast<int>(type));
}

x5::cms::EX5ZoneKey wire_zone(int zone_key) {
  return static_cast<x5::cms::EX5ZoneKey>(zone_key);
}

void fill_channel_entry(const housing::ChannelEntry &entry, x5::pb::HousingChannelEntry &wire) {
  wire.set_channel(entry.channel);
  wire.set_area_name(entry.area_name);
  wire.set_guild_id(entry.guild_id);
  wire.set_guild_name(entry.guild_name);
  wire.set_is_residence(entry.is_residence);
  for (const auto &assignment : entry.assignments) {
    auto *wire_assignment = wire.add_assignments();
    wire_assignment->set_site_type(wire_site_type(assignment.site_type));
    wire_assignment->set_assigned(assignment.assigned);
    wire_assignment->set_total(assignment.total);
  }
}

void fill_guild_resident(const housing::GuildResident &resident,
                         x5::pb::GuildHousingAreaBoardResidents &wire) {
  wire.set_lot_number(resident.lot_number);
  wire.set_char_id(resident.character_id);
  wire.set_name(resident.name);
  wire.set_allow_cooldown_start_epochtime(resident.allow_cooldown_start_ticks);
}

std::vector<housing::GuildResident> guild_residents(
    const google::protobuf::RepeatedPtrField<x5::pb::GuildHousingAreaBoardResidents> &wire) {
  std::vector<housing::GuildResident> residents;
  for (const auto &entry : wire) {
    housing::GuildResident resident;
    resident.lot_number = entry.lot_number();
    resident.character_id = entry.char_id();
    resident.name = entry.name();
    residents.push_back(resident);
  }
  return residents;
}

} // namespace

GuildServiceDirectory::GuildServiceDirectory(std::shared_ptr<GuildService> guilds,
                                             std::shared_ptr<CharacterRepository> characters)
    : guilds_(std::move(guilds)), characters_(std::move(characters)) {
  if (!guilds_ || !characters_) {
    throw std::invalid_argument("Guild housing directory needs guilds and characters");
  }
}

std::optional<housing::GuildMembership>
GuildServiceDirectory::membership(housing::CharacterId character_id) const {
  const auto status = guilds_->status(character_id);
  if (status.guild_id == 0) {
    return std::nullopt;
  }

  housing::GuildMembership membership;
  membership.guild_id = status.guild_id;
  membership.guild_name = status.guild_name;
  membership.grade = static_cast<housing::GuildGrade>(static_cast<int>(status.grade));
  membership.guild_level = status.guild_level;
  return membership;
}

std::vector<housing::GuildMember>
GuildServiceDirectory::members(housing::GuildId guild_id) const {
  // GuildService::detail needs a valid actor only to check the realm; membership is not
  // required to read a roster.
  GuildActor reader;
  reader.world_id = guilds_->world_id();
  reader.char_id = 1;
  const auto detail = guilds_->detail(reader, guild_id);
  if (detail.error != x5::ec::Success) {
    return {};
  }

  std::vector<housing::GuildMember> members;
  for (const auto &record : detail.members) {
    const auto character = characters_->character_by_id(record.char_id);
    if (!character) {
      continue;
    }
    housing::GuildMember member;
    member.character_id = record.char_id;
    member.account_id = character->account_id;
    member.name = record.name;
    member.grade = static_cast<housing::GuildGrade>(static_cast<int>(record.grade));
    // guild_allow requires GuildHousingSite{S,M,L}positionLv. Without the roster's company
    // position every resident looked like position 0 and every assignment was refused.
    member.company_position_level = record.company_position;
    members.push_back(member);
  }
  return members;
}

std::optional<bool> GuildServiceDirectory::guild_exists(housing::GuildId guild_id) const {
  // Every live guild has at least its Master; a disbanded guild has no roster.
  return !guilds_->members(guild_id).empty();
}

NativeHousingFront::NativeHousingFront(AuthService &auth,
                                       std::shared_ptr<housing::HousingHub> hub,
                                       NativeChatService &delivery,
                                       std::shared_ptr<const CompanyService> companies)
    : auth_(auth), hub_(std::move(hub)), delivery_(delivery), companies_(std::move(companies)) {
  if (!hub_) {
    throw std::invalid_argument("Housing front needs a housing hub");
  }
}

bool NativeHousingFront::handles(std::uint16_t packet_id) {
  switch (packet_id) {
  case native_housing::kSiteContractRequest:
  case native_housing::kAreaBoardRequest:
  case native_housing::kChannelListRequest:
  case native_housing::kChannelSearchRequest:
  case native_housing::kChannelVisitRequest:
  case native_housing::kPackageListRequest:
  case native_housing::kUnlockGetRequest:
  case native_housing::kGuildAreaContractRequest:
  case native_housing::kGuildAllowMemberRequest:
  case native_housing::kGuildAreaBoardRequest:
  case native_housing::kGuildAssignmentRequest:
  case native_housing::kGuildAreaExtensionRequest:
    return true;
  default:
    return false;
  }
}

std::optional<housing::HousingActor> NativeHousingFront::actor(const std::string &token) {
  const auto character = auth_.selected_character(token);
  if (!character) {
    return std::nullopt;
  }

  housing::HousingActor actor;
  actor.account_id = character->account_id;
  actor.character_id = character->id;
  actor.world_id = character->world_id;
  actor.name = character->name;
  actor.position = character->world_position;
  if (companies_) {
    actor.company_position_level = companies_->status(character->id).position_level;
  }
  return actor;
}

void NativeHousingFront::populate_character_entry(const std::string &token,
                                                  x5::pb::WorldCharacter &wire) {
  const auto character = auth_.selected_character(token);
  if (!character || character->id != wire.char_id()) {
    return;
  }

  const auto site = hub_->service().personal_site(character->id);
  if (site) {
    auto *owned = wire.mutable_housing_site();
    owned->set_lot_number(site->key.lot_number);
    owned->set_channel(site->key.channel);
    owned->set_expiry_epochtime(site->expiry_epoch_ticks);
    owned->set_contract_type(wire_contract(site->contract_type));
    owned->set_authority_type(static_cast<x5::cms::EX5HousingAuthorityType>(site->authority));
    if (site->return_position) {
      auto *position = owned->mutable_return_pos();
      position->set_millimeter_x(site->return_position->x_mm);
      position->set_millimeter_y(site->return_position->y_mm);
      position->set_millimeter_z(site->return_position->z_mm);
    }
  }

  if (!character->world_position) {
    return;
  }
  const auto *zone = hub_->catalog().zone_for_map(character->world_position->map_id);
  if (!zone) {
    return;
  }

  // Match HousingService::take_destination's reconnect policy without consuming a pending
  // destination. The live directory, when present, retains the exact visited channel.
  const auto location = hub_->location_of(character->id);
  const bool owns_here = site && site->key.zone_key == zone->zone_key;
  int channel = owns_here ? site->key.channel : 1;
  if (location && location->zone_key == zone->zone_key) {
    channel = location->channel;
  }
  wire.set_map_id(character->world_position->map_id);
  wire.set_zone_key(wire_zone(zone->zone_key));
  auto *entry = wire.mutable_entry();
  entry->set_housing_channel_id(channel);
  entry->set_housing_version(owns_here ? site->version : 0);
  entry->set_housing_visit(!owns_here || site->key.channel != channel);
}

int NativeHousingFront::request_zone(const housing::HousingActor &actor) const {
  if (const auto location = hub_->location_of(actor.character_id)) {
    return location->zone_key;
  }
  const auto zones = hub_->catalog().rentable_zones();
  return zones.empty() ? 0 : zones.front()->zone_key;
}

int NativeHousingFront::board_channel(const housing::HousingActor &actor, int zone_key,
                                      int requested_channel) const {
  // An explicit channel is validated by HousingService::board, including invalid values.
  if (requested_channel != 0) {
    return requested_channel;
  }

  // Proto3 omits channel zero. The stock client sends that empty request when the housing NPC
  // opens its board in the public world, and the reply carries no channel field. Candidate
  // emulator policy: the live housing channel when the character stands in this zone,
  // otherwise the primary personal channel, which exists before anyone rents there.
  const auto location = hub_->location_of(actor.character_id);
  if (location && location->zone_key == zone_key) {
    return location->channel;
  }
  return housing::kPrimaryPersonalChannel;
}

std::string NativeHousingFront::handle(const std::string &token, native_ws::FrameView frame) {
  const auto requester = actor(token);
  const bool guild_request = frame.packet_id >= native_housing::kGuildAreaContractRequest &&
                             frame.packet_id <= native_housing::kGuildAreaExtensionRequest;
  if (!requester) {
    // WSF2C_CommonErrorRes ID 1 is verified in native-ws-opcodes.json.
    x5::pb::CommonErrorRes refusal;
    refusal.set_ec(x5::ec::InvalidChar);
    return native_ws::encode_frame(1, refusal);
  }
  return guild_request ? handle_guild(*requester, frame) : handle_personal(*requester, frame);
}

std::string NativeHousingFront::handle_personal(const housing::HousingActor &actor,
                                                native_ws::FrameView frame) {
  auto &service = hub_->service();
  switch (frame.packet_id) {
  case native_housing::kSiteContractRequest: {
    const auto request = parse_payload<front::HousingSiteContractReq>(frame.payload);
    housing::ContractResult result;
    result.ec = x5::ec::Housing_RetryContract;
    hub_->run_for(actor.character_id, [&](const housing::ProgressAccess *access) {
      result = service.contract(actor, site_type(request.type()),
                                static_cast<int>(request.zone_key()),
                                contract_type(request.contract_type()), access);
    });
    hub_->publish(result.outcome, actor.character_id);

    response::HousingSiteContractRes wire;
    wire.set_ec(result.ec);
    wire.set_lot_number(result.lot_number);
    wire.set_channel(result.channel);
    wire.set_expiry_epochtime(result.expiry_epoch_ticks);
    wire.set_contract_type(wire_contract(result.contract_type));
    return native_ws::encode_frame(native_housing::kSiteContractResponse, wire);
  }
  case native_housing::kAreaBoardRequest: {
    const auto request = parse_payload<front::HousingSiteAreaBoardGetReq>(frame.payload);
    const int zone_key = request_zone(actor);
    const int channel = board_channel(actor, zone_key, request.channel());
    const auto result = service.board(actor, zone_key, channel);
    spdlog::debug("Housing resident board requested_channel={} channel={} residents={} ec={}",
                  request.channel(), channel, result.residents.size(),
                  x5::ec::EErrorCode_Name(result.ec));
    response::HousingSiteAreaBoardGetRes wire;
    wire.set_ec(result.ec);
    for (const auto &resident : result.residents) {
      auto *entry = wire.add_residents();
      entry->set_lot_number(resident.lot_number);
      entry->set_owner_char_id(resident.owner_character_id);
      entry->set_name(resident.name);
    }
    return native_ws::encode_frame(native_housing::kAreaBoardResponse, wire);
  }
  case native_housing::kChannelListRequest: {
    const auto request = parse_payload<front::HousingChannelListReq>(frame.payload);
    const auto authority = authority_type(request.authority_type());
    const auto result = service.channel_list(actor, request_zone(actor), authority);
    response::HousingChannelListRes wire;
    wire.set_ec(result.ec);
    wire.set_authority_type(request.authority_type());
    for (const auto &entry : result.entries) {
      fill_channel_entry(entry, *wire.add_entries());
    }
    return native_ws::encode_frame(native_housing::kChannelListResponse, wire);
  }
  case native_housing::kChannelSearchRequest: {
    const auto request = parse_payload<front::HousingChannelSearchReq>(frame.payload);
    const auto result = service.channel_search(actor, request_zone(actor),
                                               authority_type(request.authority_type()),
                                               request.text());
    response::HousingChannelSearchRes wire;
    wire.set_ec(result.ec);
    if (result.entry) {
      fill_channel_entry(*result.entry, *wire.mutable_entry());
    }
    return native_ws::encode_frame(native_housing::kChannelSearchResponse, wire);
  }
  case native_housing::kChannelVisitRequest: {
    const auto request = parse_payload<front::HousingChannelVisitReq>(frame.payload);
    auto result = service.visit(actor, request_zone(actor), request.channel());
    // The client completes the visit when this response and the MapMoveCompleteNoti of the
    // housing area have both arrived, in either order.
    if (result.ec == x5::ec::Success &&
        !hub_->travel(actor.character_id, *result.destination)) {
      result.ec = x5::ec::MapEntryUnavailable;
    }
    response::HousingChannelVisitRes wire;
    wire.set_ec(result.ec);
    return native_ws::encode_frame(native_housing::kChannelVisitResponse, wire);
  }
  case native_housing::kPackageListRequest: {
    parse_payload<front::HousingMovePackageListReq>(frame.payload);
    const auto result = service.packages(actor);
    response::HousingMovePackageListRes wire;
    wire.set_ec(result.ec);
    for (const auto &package : result.packages) {
      auto *summary = wire.add_packages()->mutable_summary();
      summary->set_package_id(package.package_id);
      summary->set_site_type(wire_site_type(package.site_type));
      summary->set_created_epochtime(package.created_epoch_ticks);
      summary->set_original_lot_number(package.original_lot_number);
      summary->set_original_channel(package.original_channel);
    }
    return native_ws::encode_frame(native_housing::kPackageListResponse, wire);
  }
  case native_housing::kUnlockGetRequest: {
    const auto request = parse_payload<front::HousingUnlockGetReq>(frame.payload);
    const auto result = service.unlock_info(actor, static_cast<int>(request.zone_key()));
    response::HousingUnlockGetRes wire;
    wire.set_ec(result.ec);
    for (const auto type : result.unlocked_site_types) {
      wire.add_unlocked_site_types(wire_site_type(type));
    }
    wire.set_guild_unlocked(result.guild_unlocked);
    for (const auto type : result.rentable_site_types) {
      wire.add_rentable_site_types(wire_site_type(type));
    }
    wire.set_guild_rentable(result.guild_rentable);
    wire.set_personal_channel_open_limit(result.personal_channel_open_limit);
    wire.set_guild_channel_open_limit(result.guild_channel_open_limit);
    wire.set_region_rent_locked(result.region_rent_locked);
    wire.set_personal_site_rent_limit_per_region(result.personal_site_rent_limit_per_region);
    return native_ws::encode_frame(native_housing::kUnlockGetResponse, wire);
  }
  default:
    throw std::invalid_argument("Unsupported housing front packet ID " +
                                std::to_string(frame.packet_id));
  }
}

std::string NativeHousingFront::handle_guild(const housing::HousingActor &actor,
                                             native_ws::FrameView frame) {
  auto &service = hub_->service();
  switch (frame.packet_id) {
  case native_housing::kGuildAreaContractRequest: {
    const auto request = parse_payload<guild_wire::GuildHousingAreaContractReq>(frame.payload);
    housing::GuildAreaResult result;
    result.ec = x5::ec::Housing_RetryContract;
    hub_->run_for(actor.character_id, [&](const housing::ProgressAccess *access) {
      result = service.guild_contract(actor, request.guild_id(),
                                      static_cast<int>(request.zone_key()),
                                      contract_type(request.contract_type()), request.area_name(),
                                      access);
    });
    hub_->publish(result.outcome, actor.character_id);

    guild_wire::GuildHousingAreaContractRes wire;
    wire.set_ec(result.ec);
    wire.set_guild_channel(result.channel);
    wire.set_expiry_epochtime(result.expiry_epoch_ticks);
    wire.set_contract_type(wire_contract(result.contract_type));
    wire.set_area_name(result.area_name);
    wire.set_zone_key(wire_zone(result.zone_key));
    return native_ws::encode_frame(native_housing::kGuildAreaContractResponse, wire);
  }
  case native_housing::kGuildAreaExtensionRequest: {
    const auto request =
        parse_payload<guild_wire::GuildHousingAreaContractExtensionReq>(frame.payload);
    housing::GuildAreaResult result;
    result.ec = x5::ec::Housing_FailedContractExtension;
    hub_->run_for(actor.character_id, [&](const housing::ProgressAccess *access) {
      result = service.guild_extend(actor, request.guild_id(),
                                    contract_type(request.contract_type()), access);
    });
    hub_->publish(result.outcome, actor.character_id);

    guild_wire::GuildHousingAreaContractExtensionRes wire;
    wire.set_ec(result.ec);
    wire.set_expiry_epochtime(result.expiry_epoch_ticks);
    wire.set_contract_type(wire_contract(result.contract_type));
    return native_ws::encode_frame(native_housing::kGuildAreaExtensionResponse, wire);
  }
  case native_housing::kGuildAllowMemberRequest: {
    const auto request =
        parse_payload<guild_wire::GuildHousingSiteAllowMemberReq>(frame.payload);
    auto result = service.guild_allow(actor, request.guild_id(),
                                      guild_residents(request.allow_lots()),
                                      guild_residents(request.remove_lots()));
    hub_->publish(result.outcome, actor.character_id);

    guild_wire::GuildHousingSiteAllowMemberRes wire;
    wire.set_ec(result.ec);
    for (const auto &resident : result.residents) {
      fill_guild_resident(resident, *wire.add_residents());
    }
    return native_ws::encode_frame(native_housing::kGuildAllowMemberResponse, wire);
  }
  case native_housing::kGuildAreaBoardRequest: {
    const auto request =
        parse_payload<guild_wire::GuildHousingSiteAreaBoardGetReq>(frame.payload);
    const auto result = service.guild_board(actor, request.guild_id());
    guild_wire::GuildHousingSiteAreaBoardGetRes wire;
    wire.set_ec(result.ec);
    for (const auto &resident : result.residents) {
      fill_guild_resident(resident, *wire.add_residents());
    }
    return native_ws::encode_frame(native_housing::kGuildAreaBoardResponse, wire);
  }
  case native_housing::kGuildAssignmentRequest: {
    const auto request =
        parse_payload<guild_wire::GuildHousingSiteAssignmentReq>(frame.payload);
    const auto result = service.guild_assignment(actor, request.guild_id());
    guild_wire::GuildHousingSiteAssignmentRes wire;
    wire.set_ec(result.ec);
    for (const auto character_id : result.characters_with_sites) {
      wire.add_exist_housing_site_char_ids(character_id);
    }
    return native_ws::encode_frame(native_housing::kGuildAssignmentResponse, wire);
  }
  default:
    throw std::invalid_argument("Unsupported guild housing front packet ID " +
                                std::to_string(frame.packet_id));
  }
}

void NativeHousingFront::deliver_guild_area(const housing::GuildAreaNotice &notice) {
  auto &service = hub_->service();
  const auto members = service.guilds().members(notice.guild_id);
  if (members.empty()) {
    return;
  }

  std::string frame;
  const auto channel = service.channel_record(notice.zone_key, notice.channel);
  if (!channel || channel->guild_id != notice.guild_id) {
    // The area ended or its residents changed in a way only a refresh can show.
    guild_wire::GuildHousingAreaUpdateNoti update;
    frame = native_ws::encode_frame(native_housing::kGuildAreaUpdateNotification, update);
  } else if (notice.extension) {
    guild_wire::GuildHousingAreaContractExtensionNoti extension;
    extension.set_guild_id(notice.guild_id);
    extension.set_guild_channel(channel->channel);
    extension.set_expiry_epochtime(channel->expiry_epoch_ticks);
    extension.set_contract_type(wire_contract(channel->contract_type));
    extension.set_area_name(channel->area_name);
    extension.set_zone_key(wire_zone(channel->zone_key));
    frame = native_ws::encode_frame(native_housing::kGuildAreaExtensionNotification, extension);
  } else {
    guild_wire::GuildHousingAreaContractNoti contract;
    contract.set_guild_id(notice.guild_id);
    contract.set_guild_channel(channel->channel);
    contract.set_expiry_epochtime(channel->expiry_epoch_ticks);
    contract.set_contract_type(wire_contract(channel->contract_type));
    contract.set_area_name(channel->area_name);
    contract.set_zone_key(wire_zone(channel->zone_key));
    frame = native_ws::encode_frame(native_housing::kGuildAreaContractNotification, contract);
  }

  for (const auto &member : members) {
    // An offline member misses the notification; GuildReq returns the area later.
    delivery_.push_to_character(service.world_id(), member.character_id, frame);
  }
}

} // namespace chronicles
