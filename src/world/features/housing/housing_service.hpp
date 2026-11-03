#pragma once

// Authoritative, transport-free housing rules.
//
// HousingService owns every housing site, guild area, unlock record and move package of one
// world. It validates requests against HousingCatalog, charges rent items and Hersander Tokens
// through character progress, and persists each accepted change through HousingStore before it
// becomes visible. It never touches sockets or the protocol codec: each operation returns the
// error code, its response fields and a HousingOutcome that the transport turns into packets.
// Thread-safe; one internal lock serializes all mutations, including their storage commit.
//
// Evidence and Candidate decisions are recorded in docs/housing.md. In short:
// - Rent and unlock prices, durations, object limits and cooldowns come from the catalog.
// - The server picks the lot: HousingSiteContractReq names only site type, zone and contract
//   type, and HousingSiteContractRes returns the chosen lot and channel.
// - Candidate: Monthly and Periodic contracts both run HousingSiteRentDurationDays. A Periodic
//   contract renews itself at expiry when the owner still holds the rent items; a Close request
//   stops renewal and the site is packed when it expires.
// - Candidate: preset buildings and objects come with the lot and are not owned storage items.

#include "world/features/housing/housing_catalog.hpp"
#include "world/features/housing/housing_doodads.hpp"
#include "world/features/housing/housing_state.hpp"
#include "p_common.pb.h"
#include "p_system.pb.h"

#include <chrono>
#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace chronicles::housing {

using ErrorCode = x5::ec::EErrorCode;

// The personal channel every rentable housing zone exposes, even before anyone rents there.
// Channel lists, boards and visits always accept it.
inline constexpr int kPrimaryPersonalChannel = 1;

// The authenticated character a request acts for. Nothing here comes from a request payload.
struct HousingActor {
  AccountId account_id{};
  CharacterId character_id{};
  int world_id{};
  std::string name;
  // Where the character stands when the request is made, if known.
  std::optional<WorldPosition> position;
  // The character's current company position level, supplied by transports that serve
  // ordinary player requests. A personal contract then requires
  // HousingRules::general_position_level for the site size. Operator paths (GM commands)
  // leave it empty, so their test rentals are not gated by company progression.
  std::optional<int> company_position_level;
};

// Live progress of an online character. The world session supplies this so its cached
// tutorial revision stays equal to storage; offline characters read and write storage directly.
// Only transitions of `character_id` are read from or adopted into the session.
struct ProgressAccess {
  CharacterId character_id{};
  std::function<WorldProgress()> load;
  std::function<void(const WorldProgress &)> adopt;
};

// Guild membership is owned by the guild service. Housing only reads it. Values equal
// x5.cms.EX5GuildMemberGrade (src/world/features/guild/guild.hpp GuildGrade).
enum class GuildGrade : int { none = 0, master = 1, sub_master = 2, member = 3 };

struct GuildMembership {
  GuildId guild_id{};
  std::string guild_name;
  GuildGrade grade{GuildGrade::none};
  int guild_level{};
  int max_member_count{};
};

struct GuildMember {
  CharacterId character_id{};
  AccountId account_id{};
  std::string name;
  GuildGrade grade{GuildGrade::none};
  // Company position level; guild sites require HousingRules::guild_position_level.
  int company_position_level{};
};

class GuildDirectory {
public:
  virtual ~GuildDirectory() = default;

  virtual std::optional<GuildMembership> membership(CharacterId character_id) const = 0;

  // Online and offline members, used for assignments and guild housing notifications.
  virtual std::vector<GuildMember> members(GuildId guild_id) const = 0;

  // Whether the guild currently exists, answered from the authoritative guild registry.
  // nullopt means the directory cannot tell; guild housing reconciliation then never
  // releases that guild's housing, so an unavailable registry fails closed.
  virtual std::optional<bool> guild_exists(GuildId guild_id) const;
};

// The default directory: no character belongs to a guild.
class NoGuildDirectory final : public GuildDirectory {
public:
  std::optional<GuildMembership> membership(CharacterId character_id) const override;

  std::vector<GuildMember> members(GuildId guild_id) const override;
};

// x5.pb.GuildHousingAction.
enum class GuildHousingAction : int {
  none = 0,
  contract = 1,
  allow = 2,
  remove = 3,
  disband = 4,
  leave = 5,
  expired = 6,
  join = 7,
  renew = 8,
};

struct SiteUpdateNotice {
  SiteKey site;
  bool custom_ids_changed{};
  std::string house_part_custom_ids;
  std::vector<HouseRecord> houses;
  std::vector<DoodadRecord> doodads;
  std::vector<std::uint32_t> removed_house_ids;
  std::vector<std::uint64_t> removed_doodad_unit_ids;
  std::int64_t edit_save_cooldown_start_ticks{};
};

struct PartsUpdateNotice {
  SiteKey site;
  std::uint32_t house_id{};
  std::uint64_t parts_state{};
};

struct ProgressNotice {
  CharacterId character_id{};
  WorldProgress before;
  WorldProgress after;
};

struct PackageNotice {
  CharacterId character_id{};
  PackageReason reason{PackageReason::none};
};

struct GuildSiteNotice {
  CharacterId character_id{};
  SiteKey site;
  GuildHousingAction action{GuildHousingAction::none};
};

struct GuildAreaNotice {
  GuildId guild_id{};
  int zone_key{};
  int channel{};
  bool extension{};
};

// A placed doodad changed phase (a garden plot was planted, grew or was harvested). The
// transport announces it with ChangeDoodadPhaseNoti naming the housing doodad unit ID.
struct DoodadPhaseNotice {
  SiteKey site;
  std::uint32_t house_id{};
  std::uint32_t doodad_id{};
  int phase{};
  // 100 ns Unix ticks; converted to the client's monotonic wire clock when sent.
  std::int64_t phase_begin_epoch_ticks{};
};

// Changes an accepted operation made. The transport reads the current records through the
// service accessors when it builds packets.
struct HousingOutcome {
  std::vector<SiteKey> sites_added;
  std::vector<SiteKey> contracts_changed;
  std::vector<SiteKey> sites_removed;
  std::vector<SiteUpdateNotice> site_updates;
  std::vector<PartsUpdateNotice> parts_updates;
  std::vector<CharacterId> owner_sites_changed;
  std::vector<ProgressNotice> progress_changes;
  std::vector<PackageNotice> packages_created;
  std::vector<GuildSiteNotice> guild_sites;
  std::vector<GuildAreaNotice> guild_areas;
  std::vector<DoodadPhaseNotice> doodad_phases;

  void append(HousingOutcome other);

  bool empty() const;
};

// A furniture pose request (sitting on a chair).
struct FurniturePoseCheck {
  ErrorCode ec{x5::ec::Success};
  // The pose holds after its animation (HoldAfterEnd), so later viewers must see it too.
  bool holds{};
};

// StartInteractionAnimReq on an interactive housing doodad (garden plot or decoration).
struct DoodadUseCheck {
  ErrorCode ec{x5::ec::Success};
  // The phase the interaction starts from; the execute must still find it.
  int phase{};
  const DoodadAnimation *animation{};
  // Due timers that advanced while checking, to be announced like any phase change.
  HousingOutcome outcome;
};

// ExecuteInteractionReq of an accepted doodad interaction.
struct DoodadUseResult {
  ErrorCode ec{x5::ec::Success};
  int next_phase{};
  std::int64_t phase_begin_epoch_ticks{};
  // Items the interaction granted (item -> quantity) and how ItemsUpdateNoti reports them.
  std::map<int, int> rewards;
  bool looting_reason{};
  // Resulting quantity of every stack the interaction changed (spent seeds, granted crops).
  // The requester's session already adopted the committed progress.
  std::map<int, int> changed_stacks;
  // The doodad's phase change plus any other timers that elapsed on the same lot.
  HousingOutcome outcome;
};

struct UnlockInfo {
  ErrorCode ec{x5::ec::Success};
  std::vector<SiteType> unlocked_site_types;
  bool guild_unlocked{};
  std::vector<SiteType> rentable_site_types;
  bool guild_rentable{};
  int personal_channel_open_limit{};
  int guild_channel_open_limit{};
  bool region_rent_locked{};
  int personal_site_rent_limit_per_region{};
};

struct ContractResult {
  ErrorCode ec{x5::ec::Success};
  int lot_number{};
  int channel{};
  std::int64_t expiry_epoch_ticks{};
  ContractType contract_type{ContractType::none};
  HousingOutcome outcome;
};

struct ExtensionResult {
  ErrorCode ec{x5::ec::Success};
  std::int64_t expiry_epoch_ticks{};
  ContractType contract_type{ContractType::none};
  HousingOutcome outcome;
};

struct AreaResident {
  int lot_number{};
  CharacterId owner_character_id{};
  std::string name;
};

struct BoardResult {
  ErrorCode ec{x5::ec::Success};
  std::vector<AreaResident> residents;
};

struct SiteAssignment {
  SiteType site_type{SiteType::none};
  int assigned{};
  int total{};
};

struct ChannelEntry {
  int channel{};
  std::string area_name;
  GuildId guild_id{};
  std::string guild_name;
  std::vector<SiteAssignment> assignments;
  bool is_residence{};
};

struct ChannelListResult {
  ErrorCode ec{x5::ec::Success};
  std::vector<ChannelEntry> entries;
};

struct ChannelSearchResult {
  ErrorCode ec{x5::ec::Success};
  std::optional<ChannelEntry> entry;
};

// Where a character should be placed when it next enters a housing map.
struct HousingDestination {
  int zone_key{};
  int map_id{};
  int channel{};
  WorldPosition spawn;
  // Position to return to when the character leaves the housing area.
  std::optional<WorldPosition> origin;
};

struct VisitResult {
  ErrorCode ec{x5::ec::Success};
  std::optional<HousingDestination> destination;
};

struct PackageSummary {
  std::int64_t package_id{};
  SiteType site_type{SiteType::none};
  std::int64_t created_epoch_ticks{};
  int original_lot_number{};
  int original_channel{};
};

struct PackageListResult {
  ErrorCode ec{x5::ec::Success};
  std::vector<PackageSummary> packages;
};

struct PackageResult {
  ErrorCode ec{x5::ec::Success};
  std::vector<std::int64_t> package_ids;
  HousingOutcome outcome;
};

struct EditModeResult {
  ErrorCode ec{x5::ec::Success};
  EditMode mode{EditMode::build};
  bool end{};
};

// One UpdateHousingReq after decoding. Positions are site-local millimetres.
struct UpdateRequest {
  int lot_number{};
  std::optional<std::string> house_part_custom_ids;
  std::vector<HouseRecord> houses;
  std::vector<DoodadRecord> doodads;
  std::vector<std::uint32_t> remove_house_ids;
  std::vector<std::uint64_t> remove_doodad_unit_ids;
};

struct UpdateResult {
  ErrorCode ec{x5::ec::Success};
  HousingOutcome outcome;
};

struct SimpleResult {
  ErrorCode ec{x5::ec::Success};
  HousingOutcome outcome;
};

struct GuildAreaResult {
  ErrorCode ec{x5::ec::Success};
  int channel{};
  std::int64_t expiry_epoch_ticks{};
  ContractType contract_type{ContractType::none};
  std::string area_name;
  int zone_key{};
  HousingOutcome outcome;
};

struct GuildResidentsResult {
  ErrorCode ec{x5::ec::Success};
  std::vector<GuildResident> residents;
  HousingOutcome outcome;
};

struct GuildAssignmentResult {
  ErrorCode ec{x5::ec::Success};
  std::vector<CharacterId> characters_with_sites;
};

struct GoHomeResult {
  ErrorCode ec{x5::ec::Success};
  std::optional<HousingDestination> destination;
};

// Emulator policies that the official server keeps private. Documented in docs/housing.md.
struct HousingPolicy {
  // Channels each housing zone may open for personal sites and for guild areas.
  int personal_channel_open_limit{10};
  int guild_channel_open_limit{10};
  int personal_site_rent_limit_per_region{1};
  // Upper bound of objects one UpdateHousingReq may carry, before any per-site limit.
  std::size_t maximum_update_objects{512};
  // How long after HousingEditModeReq{end} the trailing UpdateHousingReq is accepted, in 100 ns
  // ticks. The client sends it as soon as HousingEditModeNoti{end} arrives (about 0.2 s on the
  // live server on 2026-10-30); 30 s tolerates a slow link without leaving sessions open.
  std::int64_t edit_save_window_ticks{30LL * 10'000'000};
  // Guild member assignment cooldown (Housing_GuildSiteAssignCooldown). Candidate: equal to
  // HousingGuildHousingsiteEditCooldownMin.
  bool assignment_cooldown_uses_guild_edit_cooldown{true};
};

class HousingService {
public:
  using Clock = std::function<std::int64_t()>;

  // `now_epoch_ticks` returns the current time in 100 ns Unix ticks. The snapshot is loaded
  // from `store` for `world_id` immediately.
  HousingService(std::shared_ptr<const HousingCatalog> catalog,
                 std::shared_ptr<HousingStore> store, int world_id, Clock now_epoch_ticks,
                 HousingPolicy policy = {}, std::shared_ptr<const GuildDirectory> guilds = {});

  const HousingCatalog &catalog() const;

  const GuildDirectory &guilds() const;

  int world_id() const;

  // WSC2F_HousingUnlockGetReq.
  UnlockInfo unlock_info(const HousingActor &actor, int zone_key);

  // WSC2F_HousingSiteContractReq: unlock the size if needed, pay rent and assign a free lot.
  ContractResult contract(const HousingActor &actor, SiteType type, int zone_key,
                          ContractType contract_type, const ProgressAccess *access);

  // PC2S_UpdateHousingSiteContractExtensionReq for the actor's personal site.
  ExtensionResult extend(const HousingActor &actor, ContractType contract_type,
                         const ProgressAccess *access);

  // Packs the actor's personal site into a move package and frees the lot (GM release and the
  // service's own expiry).
  SimpleResult release(const HousingActor &actor, PackageReason reason,
                       const ProgressAccess *access);

  // WSC2F_HousingSiteAreaBoardGetReq.
  BoardResult board(const HousingActor &actor, int zone_key, int channel);

  // WSC2F_HousingChannelListReq / SearchReq.
  ChannelListResult channel_list(const HousingActor &actor, int zone_key,
                                 AuthorityType authority);

  ChannelSearchResult channel_search(const HousingActor &actor, int zone_key,
                                     AuthorityType authority, const std::string &text);

  // WSC2F_HousingChannelVisitReq: records the destination the next housing-map entry uses.
  VisitResult visit(const HousingActor &actor, int zone_key, int channel);

  // PC2S_GoHomeReq: travel to the actor's own site.
  GoHomeResult go_home(const HousingActor &actor);

  // Destination a character's next housing-map entry uses; consumes a pending visit.
  std::optional<HousingDestination> take_destination(CharacterId character_id, int map_id);

  // A character walking into a housing area's cells: its own channel there (else the first),
  // arriving where it stands. Forgets any recorded return point, so walking out again steps
  // onto the parent map. Nullopt when `arrival` names no housing map.
  std::optional<HousingDestination> walk_in_destination(CharacterId character_id,
                                                        const WorldPosition &arrival);

  // Remembers where a character came from so leaving the housing area returns there.
  std::optional<WorldPosition> origin_of(CharacterId character_id) const;

  // WSC2F_HousingMovePackageListReq.
  PackageListResult packages(const HousingActor &actor);

  // PC2S_HousingMovePackageUnpackReq.
  PackageResult unpack(const HousingActor &actor, std::int64_t package_id, int lot_number,
                       const ProgressAccess *access);

  // PC2S_HousingMovePackageRecoverToStorageReq.
  PackageResult recover_to_storage(const HousingActor &actor,
                                   const std::vector<std::int64_t> &package_ids, bool all,
                                   const ProgressAccess *access);

  // PC2S_HousingEditModeReq. Edit mode needs the actor on its own site in the current channel.
  EditModeResult edit_mode(const HousingActor &actor, EditMode mode, bool end, int zone_key,
                           int channel);

  // PC2S_UpdateHousingReq.
  UpdateResult update(const HousingActor &actor, const UpdateRequest &request, int zone_key,
                      int channel, const ProgressAccess *access);

  // StartInteractionAnimReq.param.change_house_part_state on a site the actor may enter.
  SimpleResult set_house_part(const HousingActor &actor, int zone_key, int channel,
                              int lot_number, std::uint32_t house_id,
                              std::uint32_t part_attach_index, bool open);

  // StartInteractionAnimReq aimed at a placed object: Success when the doodad stands on an
  // occupied lot of this channel and offers `animation_id` as a pose (and `interaction_id`
  // when the client named one). Like doors, anyone in the channel may use furniture.
  FurniturePoseCheck furniture_pose(int zone_key, int channel, const DoodadUnitKey &target,
                                    int animation_id, std::optional<int> interaction_id) const;

  // Installs the garden/decoration phase profile (tools/build_housing_data.py) and the seed
  // of the reward and next-phase rolls. Without a profile every phase interaction is refused.
  void set_doodad_profile(std::shared_ptr<const HousingDoodadProfile> profile,
                          std::uint32_t random_seed);

  const HousingDoodadProfile *doodad_profile() const;

  // StartInteractionAnimReq on an interactive housing doodad. Only the lot's owner may use it
  // (Candidate: garden plots consume seeds and grant crops into the owner's progress). Due
  // timers advance first; the interaction must exist in the resulting phase, and every item
  // condition must already hold. Nothing is spent here.
  DoodadUseCheck check_doodad_use(const HousingActor &actor, int zone_key, int channel,
                                  const DoodadUnitKey &target, int interaction_id,
                                  int animation_id, const ProgressAccess *access);

  // ExecuteInteractionReq of a checked interaction: the doodad must still be in
  // `expected_phase`. Consumes ConsumeItem conditions, grants the rolled reward, moves the
  // doodad to its next phase and commits site and progress together.
  DoodadUseResult complete_doodad_use(const HousingActor &actor, int zone_key, int channel,
                                      const DoodadUnitKey &target, int interaction_id,
                                      int expected_phase, const ProgressAccess *access);

  // Advances every placed doodad whose timed phase has elapsed (growth, rest, decay), chaining
  // phases that elapsed back to back, and commits each changed site. Call periodically.
  HousingOutcome advance_due_doodads();

  // PC2S_HousingItemDiscardReq: removes uninstalled housing storage items.
  SimpleResult discard_item(const HousingActor &actor, int item_cms_id, int quantity,
                            const ProgressAccess *access);

  // Guild housing (WSC2F_GuildHousing*). The guild directory decides membership and grade.
  GuildAreaResult guild_contract(const HousingActor &actor, GuildId guild_id, int zone_key,
                                 ContractType contract_type, const std::string &area_name,
                                 const ProgressAccess *access);

  GuildAreaResult guild_extend(const HousingActor &actor, GuildId guild_id,
                               ContractType contract_type, const ProgressAccess *access);

  GuildResidentsResult guild_board(const HousingActor &actor, GuildId guild_id);

  GuildResidentsResult guild_allow(const HousingActor &actor, GuildId guild_id,
                                   const std::vector<GuildResident> &allow,
                                   const std::vector<GuildResident> &remove);

  GuildAssignmentResult guild_assignment(const HousingActor &actor, GuildId guild_id);

  // Guild departures release the assigned lot, preserving the reassignment cooldown, and
  // pack a guild site of that guild still owned by the character even without a resident
  // entry. A character who belongs to the guild again is left alone, so a late or repeated
  // call never releases a current member's site. Idempotent.
  SimpleResult guild_member_left(GuildId guild_id, CharacterId character_id,
                                 const ProgressAccess *access, bool kicked = false);

  // Removes the guild's area and packs its sites. Does nothing while the directory reports
  // that the guild still exists. Idempotent.
  SimpleResult guild_disbanded(GuildId guild_id);

  // Guild housing whose guild membership has already ended. Guild changes commit before
  // housing cleanup runs, so a failed, timed-out or interrupted cleanup (including a server
  // stop) leaves these behind; the guild registry is the durable record of what is owed.
  struct DepartedGuildResident {
    GuildId guild_id{};
    CharacterId character_id{};

    auto operator<=>(const DepartedGuildResident &) const = default;
  };

  struct StaleGuildHousing {
    std::vector<GuildId> disbanded_guilds;
    std::vector<DepartedGuildResident> departed_residents;

    bool empty() const {
      return disbanded_guilds.empty() && departed_residents.empty();
    }
  };

  // Lists stale guild housing. Guilds the directory cannot confirm are skipped.
  StaleGuildHousing stale_guild_housing() const;

  // GM commands (GmCmd 56, 96, 103, 104, 115, 116, 123, 126).
  SimpleResult gm_release(int lot_number, int channel);

  SimpleResult gm_set_unlock(const HousingActor &actor, SiteType type, bool guild, bool grant);

  SimpleResult gm_set_remaining_minutes(const HousingActor &actor, int minutes,
                                        ContractType contract_type);

  SimpleResult gm_reset_account(const HousingActor &actor, const ProgressAccess *access);

  SimpleResult gm_contract_guild(const HousingActor &actor, int lot_number);

  SimpleResult gm_release_guild(int lot_number, int channel);

  // Expiry runs in two phases so no session strand is awaited while the service lock is held:
  // due_*() lists what expired, then the transport calls expire_*() for each entry on the
  // owner's session strand when the owner is online.
  struct DueSite {
    SiteKey site;
    CharacterId owner_character_id{};
    AccountId owner_account_id{};
  };

  struct DueArea {
    int zone_key{};
    int channel{};
    GuildId guild_id{};
    // The guild master pays a periodic renewal; zero when the directory names no master.
    CharacterId master_character_id{};
    AccountId master_account_id{};
  };

  std::vector<DueSite> due_sites() const;

  std::vector<DueArea> due_areas() const;

  // Renews a periodic personal site from its owner's rent items, or packs it.
  SimpleResult expire_site(const SiteKey &site, const ProgressAccess *access);

  // Renews a periodic guild area from its master's rent items, or packs every guild site in
  // it and removes the area.
  SimpleResult expire_area(int zone_key, int channel, const ProgressAccess *access);

  // Read accessors used to build packets. Copies, so callers never hold the lock.
  std::optional<SiteRecord> site(const SiteKey &key) const;

  std::optional<SiteRecord> personal_site(CharacterId character_id) const;

  std::optional<SiteRecord> guild_site_of(CharacterId character_id) const;

  std::vector<SiteRecord> sites_in_channel(int zone_key, int channel) const;

  std::optional<ChannelRecord> channel_record(int zone_key, int channel) const;

  std::optional<ChannelRecord> guild_area(GuildId guild_id) const;

private:
  ErrorCode charge_rent(const RentCost &cost, WorldProgress &progress) const;

  ErrorCode charge_unlock(const RentCost &cost, WorldProgress &progress) const;

  WorldProgress load_progress_locked(const HousingActor &actor,
                                     const ProgressAccess *access) const;

  // Persists the commit, then applies it to the cache and lets the caller's session adopt its
  // own progress transition.
  bool commit_locked(HousingCommit commit, const ProgressAccess *access);

  std::optional<SiteKey> find_free_lot_locked(int zone_key, SiteType type) const;

  int open_channel_count_locked(int zone_key, bool guild) const;

  SiteRecord new_site_locked(const HousingActor &actor, const SiteKey &key,
                             const SiteDefinition &definition, ContractType contract_type,
                             std::int64_t now) const;

  SiteContents preset_contents(const SiteDefinition &definition) const;

  std::int64_t rent_duration_ticks(AuthorityType authority) const;

  // Adds the site's removal to `commit` and, when it held owned objects, a move package. The
  // owned objects leave `owner_progress` storage when `owner_progress_loaded`.
  bool pack_site_locked(const SiteRecord &site, PackageReason reason, HousingCommit &commit,
                        WorldProgress &owner_progress, bool owner_progress_loaded,
                        HousingOutcome &outcome);

  // Packs one site in its own commit, charging nothing.
  SimpleResult pack_and_commit_locked(const SiteRecord &site, PackageReason reason,
                                      const ProgressAccess *access);

  // Packs the guild site of `guild_id` owned by `character_id` when no resident entry of the
  // guild's area lists it. Succeeds without change when there is no such site.
  SimpleResult release_unlisted_guild_site_locked(GuildId guild_id, CharacterId character_id,
                                                  GuildHousingAction action,
                                                  const ProgressAccess *access);

  // Adds one site's packing to a larger commit, moving the owner's installed objects out of the
  // owner's stored progress. The owner's online session adopts the change via the outcome.
  void pack_owned_site_locked(const SiteRecord &site, PackageReason reason,
                              HousingCommit &commit, HousingOutcome &outcome);

  ChannelEntry channel_entry_locked(int zone_key, int channel, CharacterId viewer) const;

  const SiteRecord *personal_site_locked(CharacterId character_id) const;

  // The personal or guild site a character owns in one channel.
  const SiteRecord *owned_site_in_channel_locked(CharacterId character_id, int zone_key,
                                                 int channel) const;

  std::optional<ChannelRecord> guild_area_locked(GuildId guild_id) const;

  ErrorCode validate_update_locked(const SiteRecord &site, const UpdateRequest &request,
                                   WorldProgress &progress, SiteRecord &updated,
                                   SiteUpdateNotice &notice) const;

  // Validates and commits one UpdateHousingReq for the actor's site after the session check.
  UpdateResult apply_update_locked(const HousingActor &actor, const UpdateRequest &request,
                                   const SiteRecord &current, const ProgressAccess *access);

  // Moves every profiled doodad of `site` through its elapsed timers up to `now`. Returns the
  // resulting phase notices (one per changed doodad); `site` is updated in place.
  std::vector<DoodadPhaseNotice> advance_site_doodads_locked(SiteRecord &site, std::int64_t now);

  // Commits `site` (already advanced) when `notices` is not empty. Returns false when storage
  // refused the write.
  bool commit_doodad_advance_locked(const SiteRecord &before, SiteRecord site,
                                    const std::vector<DoodadPhaseNotice> &notices);

  // Checks the conditions of `transition` against `progress`.
  static ErrorCode doodad_conditions_hold(const DoodadTransition &transition,
                                          const WorldProgress &progress);

  // Resolves the actor's guild and checks the requested guild ID and minimum grade.
  ErrorCode guild_permission(const HousingActor &actor, GuildId guild_id,
                             GuildGrade lowest_allowed, GuildMembership &membership) const;

  std::shared_ptr<const HousingCatalog> catalog_;
  std::shared_ptr<HousingStore> store_;
  int world_id_{};
  Clock now_;
  HousingPolicy policy_;
  std::shared_ptr<const GuildDirectory> guilds_;

  mutable std::mutex mutex_;
  std::map<SiteKey, SiteRecord> sites_;
  std::map<std::pair<int, int>, ChannelRecord> channels_;
  std::map<CharacterId, CharacterHousingRecord> characters_;
  std::map<std::int64_t, MovePackageRecord> packages_;
  std::int64_t next_package_id_{1};
  // Transient travel state: pending visits and where each visitor came from.
  std::unordered_map<CharacterId, HousingDestination> pending_destinations_;
  std::unordered_map<CharacterId, WorldPosition> origins_;
  std::unordered_map<CharacterId, std::int64_t> last_go_home_ticks_;
  // Transient edit sessions: character -> mode on its own site.
  //
  // The stock client saves after leaving edit mode: it sends HousingEditModeReq{end}, waits for
  // HousingEditModeNoti{end} and only then sends UpdateHousingReq with the changes
  // (AAC_PS2C_HousingEditModeNoti_Handle -> AAC_Housing_ExitBuildMode_SaveOrDiscard). An ended
  // session therefore stays open for one save inside HousingPolicy::edit_save_window_ticks.
  struct EditSession {
    EditMode mode{EditMode::build};
    SiteKey site;
    // Zero while editing; the time edit mode ended while the trailing save is pending.
    std::int64_t ended_epoch_ticks{};
  };

  // Returns the actor's session when it may still save to `site`; drops an expired one.
  const EditSession *save_session_locked(CharacterId character_id, const SiteKey &site);

  std::unordered_map<CharacterId, EditSession> edit_sessions_;

  // Garden and decoration phase graphs; null leaves phase interactions unsupported.
  std::shared_ptr<const HousingDoodadProfile> doodad_profile_;
  std::mt19937 doodad_random_;
};

} // namespace chronicles::housing
