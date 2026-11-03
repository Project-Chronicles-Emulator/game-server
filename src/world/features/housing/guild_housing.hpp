#pragma once

// Guild housing: the link between GuildService and HousingService.
//
// HousingService already owns guild areas, member sites, rent and expiry. This module supplies
// what it needs from the guild side and turns its results into packets:
// - LegacyGuildServiceDirectory answers housing's membership questions from GuildService, mapping
//   EX5GuildMemberGrade (Master 1, SubMaster 2, Member 3) to housing::GuildGrade explicitly.
// - GuildHousingCoordinator executes the front WSC2F_GuildHousing* requests (20034-20038),
//   releases a leaving member's site and a disbanded guild's area, renews or ends due areas,
//   and builds the WSF2C responses and notifications.
//
// Rent is paid from the guild master's live progress. A HousingWorldBridge (the world listener)
// runs such work on the payer's world-session strand so the session's cached progress stays
// current; without a bridge, or for a character not in the world, storage is used directly.
//
// Evidence: packet IDs from protocol/native-ws-opcodes.json; the client's
// guildHousingAreaAuthorize dialogue check allows only the guild Master (DialogueErrorCode
// 1207, dumps/ida_research/guild-expansion-20261026). Which members receive each area
// notification is the emulator's Candidate mapping of the recovered message names.

#include "world/features/guild/guild.hpp"
#include "world/features/housing/housing_service.hpp"
#include "world/features/guild/native_guild.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace chronicles {

class AuthService;

namespace guild_housing {

// x5.pb.HousingSite for a guild site notice. `site` is null after the site was removed.
x5::pb::HousingSite guild_housing_site(const housing::SiteKey &key,
                                       const std::optional<housing::SiteRecord> &site,
                                       const std::optional<housing::ChannelRecord> &area);

} // namespace guild_housing

housing::GuildGrade housing_guild_grade(GuildGrade grade);

GuildGrade guild_grade_from_housing(housing::GuildGrade grade);

class LegacyGuildServiceDirectory final : public housing::GuildDirectory {
public:
  explicit LegacyGuildServiceDirectory(std::shared_ptr<GuildService> guilds);

  std::optional<housing::GuildMembership>
  membership(housing::CharacterId character_id) const override;

  std::vector<housing::GuildMember> members(housing::GuildId guild_id) const override;

  std::optional<bool> guild_exists(housing::GuildId guild_id) const override;

private:
  std::shared_ptr<GuildService> guilds_;
};

// Implemented by the world listener. Both methods may be called from any thread.
class HousingWorldBridge {
public:
  virtual ~HousingWorldBridge() = default;

  // Runs `work` on the character's world-session strand with its live progress, or on the
  // calling thread with nullptr when the character is not connected to the world.
  virtual void with_live_progress(std::int64_t char_id,
                                  std::function<void(const housing::ProgressAccess *)> work) = 0;

  // Lets online sessions adopt their progress transitions and sends each affected character
  // PS2C_GuildHousingSiteUpdateNoti.
  virtual void deliver(const housing::HousingOutcome &outcome) = 0;
};

class GuildHousingCoordinator final : public GuildHousingRequests {
public:
  GuildHousingCoordinator(std::shared_ptr<housing::HousingService> housing,
                          std::shared_ptr<GuildService> guilds);

  // Wiring performed once during startup, before requests are served.
  void set_world_bridge(std::shared_ptr<HousingWorldBridge> bridge);

  void set_front_delivery(FrontDelivery delivery) override;

  // Handles one front guild housing request. Contract and extension charge the master's
  // progress and answer asynchronously through the front delivery, returning an empty string;
  // the other requests return their encoded response.
  std::string handle(const GuildHousingRequester &requester,
                     native_ws::FrameView frame) override;

  std::optional<x5::pb::guild::GuildHousingArea>
  housing_area(std::int64_t guild_id) const override;

  // GuildService subscriber: a leaving member loses its site; a disbanded guild its area.
  void on_guild_events(const std::vector<GuildEvent> &events);

  // Renews or ends every guild area whose contract has expired. Called periodically.
  void expire_due_areas();

  housing::HousingService &housing();

  const housing::HousingService &housing() const;

private:
  enum class AreaNotice { contract, extension, update };

  std::string contract(const housing::HousingActor &actor, native_ws::FrameView frame);
  std::string extend(const housing::HousingActor &actor, native_ws::FrameView frame);
  std::string allow(const housing::HousingActor &actor, native_ws::FrameView frame);
  std::string board(const housing::HousingActor &actor, native_ws::FrameView frame);
  std::string assignment(const housing::HousingActor &actor, native_ws::FrameView frame);

  // Delivers an accepted outcome: world packets and progress adoption through the bridge,
  // area notifications to the guild's front sockets. `excluded` already has its response.
  void publish(const housing::HousingOutcome &outcome, AreaNotice kind,
               std::int64_t excluded);

  void send(std::int64_t char_id, const std::string &frame) const;

  std::shared_ptr<HousingWorldBridge> bridge() const;

  std::shared_ptr<housing::HousingService> housing_;
  std::shared_ptr<GuildService> guilds_;
  mutable std::mutex wiring_mutex_;
  std::shared_ptr<HousingWorldBridge> bridge_;
  FrontDelivery delivery_;
};

} // namespace chronicles
