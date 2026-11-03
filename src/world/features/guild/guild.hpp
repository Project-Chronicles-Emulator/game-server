#pragma once

// Guild membership, permissions and progression for one realm.
//
// GuildService is transport-free. Front WebSocket (native_guild) and world (guild_world)
// adapters translate requests into calls and its published GuildEvents into packets. The
// service owns the authoritative in-memory registry and writes every change through a
// GuildStore before applying it, so a failed write leaves both copies unchanged.
//
// Evidence (see docs/guilds.md and dumps/ida_research/guild-20261026):
// - Catalog GameVariable rows: GuildNameUnitLengthMin/Max 2/20, GuildDescUnitLengthMax 200,
//   MaxGuildSubMasterCount 3, GuildRejoinableTimeHours 24, GuildRecreatableTimeHours 168,
//   GuildDisbandableTimeHours 168, GuildInviteTimeoutMillis 20000, GuildJoinableListCount 10,
//   GuildCreatableCompanyPosition 3. GuildLevel rows give per-level member limits.
// - Client validation (AAC_Guild_UI_RequestManageMemberGrade and
//   AAC_Guild_UI_EvaluateMemberManagePermissions): only the Master changes grades, a Master or
//   SubMaster may kick only a Member, the Master's own grade cannot be managed, and
//   GuildMemberManageReq with grade None is the kick request.
// - Error codes are the recovered x5.ec.EErrorCode Guild_* values (23001-23053).
//
// Candidate interpretations are marked where they are used. Company membership comes from
// CompanyService through GuildActor: creation needs a company and GuildCreatableCompanyPosition,
// and admission checks the guild's company and join_company_position. Guild experience comes
// from the emulator's activity source (record_character_experience) and GM commands;
// award_experience is the entry point for RewardType.GuildExp quest rewards. See
// docs/guilds.md.

#include "ec.generated.pb.h"
#include "persistence/quest_effect_settlement_status.hpp"
#include "persistence/quest_service_effects.hpp"
#include "world/core/quest_effect_dispatcher.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace chronicles {

struct MemoryQuestServiceCoordinator;

class ClientCatalog;

// Values equal x5.cms.EX5GuildMemberGrade.
enum class GuildGrade : int {
  none = 0,
  master = 1,
  sub_master = 2,
  member = 3,
};

// Values equal x5.cms.EX5GuildJoinType. Auto lets a qualifying character join from the
// joinable list without an invitation; None accepts invitations only (Candidate).
enum class GuildJoinType : int {
  none = 0,
  automatic = 1,
};

struct GuildRecord {
  std::int64_t id{};
  int world_id{};
  std::string name;
  std::string description;
  std::string notice;
  std::int64_t notice_char_id{};
  // Wall-clock values are 100 ns Unix-epoch ticks, the client's *_epochtime unit.
  std::int64_t notice_epoch_ticks{};
  GuildJoinType join_type{GuildJoinType::none};
  int join_level{};
  // x5.cms.EX5CompanyType members must belong to: the founder's company (Candidate).
  int join_company_type{};
  int join_company_position{};
  // Experience gathered toward the next level. GuildLevel.requiredGuildExp is read per level,
  // like CharacterLevel.nextLevelExp (Candidate).
  int experience{};
  int level{1};
  std::int64_t created_epoch_ticks{};
};

struct GuildMemberRecord {
  std::int64_t guild_id{};
  std::int64_t char_id{};
  GuildGrade grade{GuildGrade::member};
  // Display fields mirror the character row; the service refreshes them when the member
  // enters the world. They are never taken from a client payload.
  std::string name;
  int level{1};
  int char_class{};
  // Company position level shown in the roster; refreshed with the other display fields.
  int company_position{};
  // Guild experience this member has earned for the guild.
  int contribution{};
  std::int64_t joined_epoch_ticks{};
  std::int64_t last_connect_epoch_ticks{};
  // Owning account, needed by guild housing to charge or pack an offline member's progress.
  // Not part of the guild tables; read from the character row or the acting session.
  std::int64_t account_id{};
};

// Cooldown anchors that outlive a membership. Zero means the event never happened.
struct GuildHistory {
  std::int64_t char_id{};
  std::int64_t last_leave_epoch_ticks{};
  std::int64_t last_disband_epoch_ticks{};
};

struct GuildSnapshot {
  std::vector<GuildRecord> guilds;
  std::vector<GuildMemberRecord> members;
  std::vector<GuildHistory> histories;
};

struct GuildQuestEffectResult {
  QuestEffectSettlementStatus status{QuestEffectSettlementStatus::retry};
  std::optional<GuildRecord> guild;
  std::optional<GuildMemberRecord> member;
  bool event_completed{};
  std::int64_t activity_remainder{};
};

struct GuildProgressionSnapshot {
  GuildRecord guild;
  std::optional<GuildMemberRecord> member;
};

// Persistence for one realm's guilds. Every method is one transaction and throws on a
// storage failure. Implementations: MemoryGuildStore and PostgresGuildStore.
class GuildStore {
public:
  virtual ~GuildStore() = default;

  // The live owner epoch is acquired once before guild services are constructed.
  virtual void adopt_quest_service_owner_epoch(std::int64_t) {
    throw std::runtime_error("Guild store cannot adopt a quest service owner epoch");
  }

  // Reconciliation reads are fenced against an earlier uncertain transaction. Settlement
  // compares the expected progression before atomically applying both deltas and the marker.
  virtual GuildQuestEffectResult read_quest_effect(const QuestServiceEffectEvent &) {
    throw std::runtime_error("Guild store cannot read quest effects");
  }
  virtual GuildQuestEffectResult settle_quest_effect(const QuestServiceEffectEvent &,
                                                     const GuildRecord &,
                                                     const GuildMemberRecord &,
                                                     const GuildRecord &,
                                                     const GuildMemberRecord &,
                                                     std::int64_t = 0,
                                                     std::int64_t = 0) {
    throw std::runtime_error("Guild store cannot settle quest effects");
  }

  // Activity progression is one guarded transaction for both the guild and member.
  virtual void commit_progression(const GuildRecord &, const GuildMemberRecord &,
                                  const GuildRecord &, const GuildMemberRecord &) {
    throw std::runtime_error("Guild store cannot commit progression atomically");
  }

  virtual GuildProgressionSnapshot read_progression(
      int, std::int64_t, const std::optional<std::int64_t> &) {
    throw std::runtime_error("Guild store cannot fence progression readback");
  }

  virtual GuildSnapshot load(int world_id) = 0;

  // Inserts the guild and its master. Returns the stored guild with its new ID, or nullopt
  // when the realm already has a guild with this name (ASCII case-insensitive).
  virtual std::optional<GuildRecord> create_guild(GuildRecord guild,
                                                  const GuildMemberRecord &master) = 0;

  // Replaces the mutable guild fields: description, notice, join settings and progression.
  virtual void update_guild(const GuildRecord &guild) = 0;

  // Inserts or updates memberships (grade, contribution, last connection) in list order.
  // A Master transfer must list the demoted master first.
  virtual void save_members(const std::vector<GuildMemberRecord> &members) = 0;

  virtual void remove_member(std::int64_t guild_id, std::int64_t char_id,
                             const GuildHistory &history) = 0;

  // Deletes the guild with its memberships and stores each former member's cooldown anchors.
  virtual void delete_guild(std::int64_t guild_id, const std::vector<GuildHistory> &histories) = 0;

  // Replaces one character's cooldown anchors (GM cooldown reset).
  virtual void save_history(const GuildHistory &history) = 0;
};

class MemoryGuildStore final : public GuildStore {
public:
  explicit MemoryGuildStore(std::shared_ptr<MemoryQuestServiceCoordinator> quest_services = {});

  void adopt_quest_service_owner_epoch(std::int64_t epoch) override;

  GuildQuestEffectResult read_quest_effect(const QuestServiceEffectEvent &event) override;
  GuildQuestEffectResult settle_quest_effect(const QuestServiceEffectEvent &event,
                                             const GuildRecord &before_guild,
                                             const GuildMemberRecord &before_member,
                                             const GuildRecord &after_guild,
                                             const GuildMemberRecord &after_member,
                                             std::int64_t before_remainder = 0,
                                             std::int64_t after_remainder = 0) override;

  void commit_progression(const GuildRecord &before_guild,
                          const GuildMemberRecord &before_member,
                          const GuildRecord &after_guild,
                          const GuildMemberRecord &after_member) override;

  GuildProgressionSnapshot read_progression(
      int world_id, std::int64_t guild_id,
      const std::optional<std::int64_t> &character_id) override;

  // Fault injection at the allocation/publication boundary for disposable store tests.
  void set_before_membership_publish_for_test(std::function<void()> hook);

  GuildSnapshot load(int world_id) override;

  std::optional<GuildRecord> create_guild(GuildRecord guild,
                                          const GuildMemberRecord &master) override;

  void update_guild(const GuildRecord &guild) override;

  void save_members(const std::vector<GuildMemberRecord> &members) override;

  void remove_member(std::int64_t guild_id, std::int64_t char_id,
                     const GuildHistory &history) override;

  void delete_guild(std::int64_t guild_id, const std::vector<GuildHistory> &histories) override;

  void save_history(const GuildHistory &history) override;

private:
  GuildQuestEffectResult quest_effect_locked(const QuestServiceEffectEvent &event);

  std::shared_ptr<MemoryQuestServiceCoordinator> quest_services_;
  std::atomic<std::int64_t> owner_epoch_{};
  std::mutex mutex_;
  std::int64_t next_guild_id_{1};
  std::map<std::int64_t, GuildRecord> guilds_;
  std::map<std::int64_t, GuildMemberRecord> members_;
  std::map<std::int64_t, GuildHistory> histories_;
  std::function<void()> before_membership_publish_for_test_;
};

// Uses the emulator_guilds, emulator_guild_members and emulator_character_guild_history
// tables from migrations/001_accounts_characters.sql (schema versions 17 and 19). Member display
// fields are read from emulator_characters.
class PostgresGuildStore final : public GuildStore {
public:
  explicit PostgresGuildStore(std::string connection);

  void adopt_quest_service_owner_epoch(std::int64_t epoch) override;

  GuildQuestEffectResult read_quest_effect(const QuestServiceEffectEvent &event) override;
  GuildQuestEffectResult settle_quest_effect(const QuestServiceEffectEvent &event,
                                             const GuildRecord &before_guild,
                                             const GuildMemberRecord &before_member,
                                             const GuildRecord &after_guild,
                                             const GuildMemberRecord &after_member,
                                             std::int64_t before_remainder = 0,
                                             std::int64_t after_remainder = 0) override;

  void commit_progression(const GuildRecord &before_guild,
                          const GuildMemberRecord &before_member,
                          const GuildRecord &after_guild,
                          const GuildMemberRecord &after_member) override;

  GuildProgressionSnapshot read_progression(
      int world_id, std::int64_t guild_id,
      const std::optional<std::int64_t> &character_id) override;

  GuildSnapshot load(int world_id) override;

  std::optional<GuildRecord> create_guild(GuildRecord guild,
                                          const GuildMemberRecord &master) override;

  void update_guild(const GuildRecord &guild) override;

  void save_members(const std::vector<GuildMemberRecord> &members) override;

  void remove_member(std::int64_t guild_id, std::int64_t char_id,
                     const GuildHistory &history) override;

  void delete_guild(std::int64_t guild_id, const std::vector<GuildHistory> &histories) override;

  void save_history(const GuildHistory &history) override;

private:
  GuildQuestEffectResult quest_effect(const QuestServiceEffectEvent &event,
                                      const GuildRecord *before_guild,
                                      const GuildMemberRecord *before_member,
                                      const GuildRecord *after_guild,
                                      const GuildMemberRecord *after_member,
                                      std::int64_t before_remainder,
                                      std::int64_t after_remainder);

  std::string connection_;
  std::atomic<std::int64_t> owner_epoch_{};
};

struct GuildLevelRule {
  int level{};
  // GuildLevel.requiredGuildExp: experience needed to advance past this level. Ignored for
  // the last level, which is the maximum.
  int required_experience{};
  std::size_t max_members{};
};

struct GuildRules {
  std::size_t name_min_length{2};
  std::size_t name_max_length{20};
  std::size_t description_max_length{200};
  std::size_t max_sub_masters{3};
  std::chrono::hours rejoin_cooldown{24};
  std::chrono::hours recreate_cooldown{168};
  // Candidate: GuildDisbandableTimeHours read as the minimum guild age before disbanding.
  std::chrono::hours disband_cooldown{168};
  std::chrono::milliseconds invite_timeout{20000};
  std::size_t joinable_list_page_size{10};
  // GuildLevel rows, ordered by level and contiguous from 1. The last row is the maximum.
  std::vector<GuildLevelRule> levels{
      {1, 10000, 10}, {2, 25000, 15}, {3, 45000, 20}, {4, 70000, 25}, {5, 100000, 30}};
  // GameVariable GuildCreatableCompanyPosition; the client dialogue refuses creation without
  // a company (DialogueErrorCode 1106) or below this position (1107).
  int creatable_company_position{3};
  // False when the realm has no company catalog (a server started without client data):
  // company requirements are then skipped so guilds stay usable.
  bool companies_enabled{true};
  // Emulator policy, because no official guild experience source is reachable in this build:
  // one guild experience point per this much character experience a member earns.
  std::int64_t character_experience_per_guild_experience{10};
};

// Reads the GameVariable and GuildLevel rows. Missing or implausible rows keep the
// defaults above and are logged; a catalog is never required.
GuildRules load_guild_rules(ClientCatalog &catalog);

// Authoritative identity of the character performing a request, built by the caller from
// its authenticated session and the stored character row.
struct GuildActor {
  int world_id{};
  std::int64_t char_id{};
  std::string name;
  int level{1};
  int char_class{};
  std::int64_t account_id{};
  // From CompanyService for the same character; kCompanyNone (0) without a company.
  int company_type{};
  int company_position{};
};

struct GuildSummary {
  GuildRecord guild;
  std::string master_name;
  std::size_t member_count{};
};

struct GuildListResult {
  x5::ec::EErrorCode error{x5::ec::Success};
  std::vector<GuildSummary> guilds;
  // Index to request next when more joinable guilds remain; zero otherwise (Candidate).
  int next_start_index{};
};

struct GuildSearchResult {
  x5::ec::EErrorCode error{x5::ec::Success};
  std::optional<GuildSummary> guild;
};

struct GuildDetailResult {
  x5::ec::EErrorCode error{x5::ec::Success};
  GuildRecord guild;
  std::vector<GuildMemberRecord> members;
};

struct GuildCreateRequest {
  std::string name;
  std::string description;
  int join_type{};
  int join_level{};
  int join_company_position{};
};

struct GuildCreateResult {
  x5::ec::EErrorCode error{x5::ec::Success};
  GuildRecord guild;
  GuildMemberRecord owner;
};

struct GuildManageRequest {
  std::int64_t guild_id{};
  std::string description;
  std::string notice;
  int join_type{};
  int join_level{};
  int join_company_position{};
};

struct GuildManageResult {
  x5::ec::EErrorCode error{x5::ec::Success};
  GuildRecord guild;
};

struct GuildMemberManageResult {
  x5::ec::EErrorCode error{x5::ec::Success};
  std::int64_t char_id{};
  GuildGrade grade{GuildGrade::none};
};

struct GuildJoinResult {
  x5::ec::EErrorCode error{x5::ec::Success};
  GuildRecord guild;
  GuildMemberRecord member;
  std::vector<GuildMemberRecord> members;
};

struct GuildLeaveResult {
  x5::ec::EErrorCode error{x5::ec::Success};
  std::int64_t left_epoch_ticks{};
};

struct GuildDisbandResult {
  x5::ec::EErrorCode error{x5::ec::Success};
  std::int64_t disbanded_epoch_ticks{};
};

struct GuildInviteResult {
  x5::ec::EErrorCode error{x5::ec::Success};
  std::string target_name;
};

// A character's guild state for WorldCharGuildNoti and unit decoration.
struct GuildStatus {
  std::int64_t guild_id{};
  std::string guild_name;
  GuildGrade grade{GuildGrade::none};
  int guild_level{};
  // Member limit of the guild's current level; zero without a guild.
  std::size_t max_members{};
  std::int64_t last_leave_epoch_ticks{};
  std::int64_t last_disband_epoch_ticks{};
};

// Published events. `recipients` lists the characters that should receive the front
// notification; the acting character already has its response and is excluded.

// A character joined (created, auto-joined or accepted an invitation).
struct GuildMemberJoined {
  GuildRecord guild;
  GuildMemberRecord member;
  std::vector<GuildMemberRecord> members;
  std::vector<std::int64_t> recipients;
  // True when the joiner accepted a world invitation and has no front response yet.
  bool via_invitation{};
};

struct GuildMemberLeft {
  std::int64_t guild_id{};
  std::int64_t char_id{};
  bool kicked{};
  std::int64_t left_epoch_ticks{};
  std::vector<std::int64_t> recipients;
};

struct GuildMemberGradeChanged {
  std::int64_t guild_id{};
  std::int64_t char_id{};
  GuildGrade grade{GuildGrade::none};
  std::vector<std::int64_t> recipients;
};

struct GuildMemberUpdated {
  GuildMemberRecord member;
  std::vector<std::int64_t> recipients;
};

struct GuildInfoChanged {
  GuildRecord guild;
  std::vector<std::int64_t> recipients;
};

struct GuildDisbanded {
  std::int64_t guild_id{};
  // Every former member, including the master who disbanded it.
  std::vector<std::int64_t> former_members;
  std::vector<std::int64_t> recipients;
};

struct GuildInviteIssued {
  std::int64_t guild_id{};
  std::string guild_name;
  std::int64_t inviter_char_id{};
  std::int64_t target_char_id{};
};

// Values equal x5.pb.guild.EGuildInviteRejectReason.
enum class GuildInviteRejectReason : int {
  user_rejected = 0,
  aborted_by_party = 1,
};

struct GuildInviteRejected {
  std::int64_t inviter_char_id{};
  std::string target_name;
  GuildInviteRejectReason reason{GuildInviteRejectReason::user_rejected};
};

// Guild experience or level changed. Every member is a recipient: experience changes have no
// acting request to answer.
struct GuildExperienceChanged {
  std::int64_t guild_id{};
  int experience{};
  int level{};
  bool level_changed{};
  std::vector<std::int64_t> recipients;
};

using GuildEvent = std::variant<GuildMemberJoined, GuildMemberLeft, GuildMemberGradeChanged,
                                GuildMemberUpdated, GuildInfoChanged, GuildDisbanded,
                                GuildInviteIssued, GuildInviteRejected, GuildExperienceChanged>;

using GuildEventSubscriber = std::function<void(const std::vector<GuildEvent> &)>;

struct GuildClock {
  // Wall clock for persisted *_epochtime values (100 ns Unix-epoch ticks).
  std::function<std::int64_t()> epoch_ticks;
  // Monotonic clock for invitation expiry.
  std::function<std::chrono::steady_clock::time_point()> steady;
};

GuildClock system_guild_clock();

// Thread-safe. Operations serialize on one lock, including their store write. Events are
// published after the lock is released, so subscribers may call read-only methods.
class GuildService {
public:
  // Loads the realm's guilds and repairs memberships left inconsistent by character
  // deletion (a guild without a master promotes its longest-standing member; an empty guild
  // is deleted).
  GuildService(std::shared_ptr<GuildStore> store, GuildRules rules, int world_id,
               GuildClock clock = system_guild_clock());

  // Register during startup, before requests are served. Subscribers run on the thread
  // that completed the operation and must not block.
  void subscribe(GuildEventSubscriber subscriber);

  const GuildRules &rules() const;

  GuildListResult joinable(const GuildActor &actor, int start_index);

  GuildSearchResult search(const GuildActor &actor, const std::string &name);

  GuildDetailResult detail(const GuildActor &actor, std::int64_t guild_id);

  GuildCreateResult create(const GuildActor &actor, const GuildCreateRequest &request);

  GuildManageResult manage(const GuildActor &actor, const GuildManageRequest &request);

  // Grade None kicks the member; Master transfers mastership and demotes the former master
  // to Member (Candidate: the demoted grade is not evidenced).
  GuildMemberManageResult manage_member(const GuildActor &actor, std::int64_t guild_id,
                                        std::int64_t target_char_id, int grade);

  GuildJoinResult join(const GuildActor &actor, std::int64_t guild_id);

  GuildLeaveResult leave(const GuildActor &actor, std::int64_t guild_id);

  GuildDisbandResult disband(const GuildActor &actor, std::int64_t guild_id);

  // World invitation. `target` must be the authoritative identity of an online character.
  GuildInviteResult invite(const GuildActor &inviter, std::int64_t guild_id,
                           const GuildActor &target);

  x5::ec::EErrorCode accept_invite(const GuildActor &invitee, std::int64_t guild_id,
                                   std::int64_t inviter_char_id);

  x5::ec::EErrorCode reject_invite(const GuildActor &invitee, std::int64_t guild_id,
                                   std::int64_t inviter_char_id);

  // Records the member's current display fields and connection time. Publishes
  // GuildMemberUpdated when name, level or class changed.
  void refresh_member(const GuildActor &actor);

  GuildStatus status(std::int64_t char_id);

  // Character IDs of the guild of `char_id`, including itself. Empty when not a member.
  std::vector<std::int64_t> guild_mates(std::int64_t char_id);

  // Roster of a guild in display order; empty for an unknown guild.
  std::vector<GuildMemberRecord> members(std::int64_t guild_id);

  int world_id() const;

  // Adds guild experience earned by member `char_id` and credits the member's contribution.
  // Entry point for RewardType.GuildExp quest rewards; the activity source below uses it too.
  // Returns false when the character belongs to no guild or nothing was stored.
  bool award_experience(std::int64_t char_id, int amount);

  QuestEffectSettlementStatus settle_quest_effect(const QuestServiceEffectEvent &event);
  std::optional<QuestEffectReconciliation> reconcile_uncertain_quest_effect();
  bool reconcile_uncertain_progression();
  void resynchronize_quest_effect(std::int64_t character_id);

  // Injects a preparation failure before a GM progression transaction in tests.
  void set_before_progression_notification_for_test(std::function<void()> hook);

  // Activity source (emulator policy): converts character experience a member earned into
  // guild experience at GuildRules::character_experience_per_guild_experience, keeping the
  // remainder per character in memory.
  bool record_character_experience(std::int64_t char_id, std::int64_t experience);

  // Game master operations (GmCmd guild_exp, guild_level, guild_disband,
  // guild_master_authorization and guild_reset_cooldown). They skip grade and cooldown
  // checks but keep every storage invariant.
  x5::ec::EErrorCode gm_add_experience(std::int64_t guild_id, int delta);

  x5::ec::EErrorCode gm_set_level(std::int64_t guild_id, int level);

  x5::ec::EErrorCode gm_disband(std::int64_t guild_id);

  x5::ec::EErrorCode gm_set_master(std::int64_t guild_id, std::int64_t char_id);

  x5::ec::EErrorCode gm_reset_cooldowns(std::int64_t char_id);

private:
  struct GuildEntry {
    GuildRecord record;
    std::map<std::int64_t, GuildMemberRecord> members;
  };

  struct PendingInvite {
    std::int64_t guild_id{};
    std::int64_t inviter_char_id{};
    std::chrono::steady_clock::time_point expires_at;
  };

  struct UncertainProgression {
    std::int64_t guild_id{};
    std::optional<std::int64_t> character_id;
    bool settings_changed{};
  };

  // Runs `operation(events)` under mutex_, then publishes the collected events unlocked.
  template <typename Operation> auto transact(Operation &&operation);

  // The *_locked operations and every helper below require mutex_.
  GuildListResult joinable_locked(const GuildActor &actor, int start_index) const;
  GuildSearchResult search_locked(const GuildActor &actor, const std::string &name) const;
  GuildDetailResult detail_locked(const GuildActor &actor, std::int64_t guild_id) const;
  GuildCreateResult create_locked(const GuildActor &actor, const GuildCreateRequest &request,
                                  std::vector<GuildEvent> &events);
  GuildManageResult manage_locked(const GuildActor &actor, const GuildManageRequest &request,
                                  std::vector<GuildEvent> &events);
  GuildMemberManageResult manage_member_locked(const GuildActor &actor, std::int64_t guild_id,
                                               std::int64_t target_char_id, int grade,
                                               std::vector<GuildEvent> &events);
  GuildJoinResult join_locked(const GuildActor &actor, std::int64_t guild_id,
                              std::vector<GuildEvent> &events);
  GuildLeaveResult leave_locked(const GuildActor &actor, std::int64_t guild_id,
                                std::vector<GuildEvent> &events);
  GuildDisbandResult disband_locked(const GuildActor &actor, std::int64_t guild_id,
                                    std::vector<GuildEvent> &events);
  GuildInviteResult invite_locked(const GuildActor &inviter, std::int64_t guild_id,
                                  const GuildActor &target, std::vector<GuildEvent> &events);
  x5::ec::EErrorCode accept_invite_locked(const GuildActor &invitee, std::int64_t guild_id,
                                          std::int64_t inviter_char_id,
                                          std::vector<GuildEvent> &events);
  x5::ec::EErrorCode reject_invite_locked(const GuildActor &invitee, std::int64_t guild_id,
                                          std::int64_t inviter_char_id,
                                          std::vector<GuildEvent> &events);
  void refresh_member_locked(const GuildActor &actor, std::vector<GuildEvent> &events);
  bool award_experience_locked(std::int64_t char_id, int amount,
                               std::vector<GuildEvent> &events);
  // Applies an experience delta to a guild record: advances through GuildLevel thresholds
  // and drops experience past the maximum level.
  GuildRecord with_experience(GuildRecord guild, std::int64_t delta) const;
  void install_quest_effect_snapshot_locked(const GuildQuestEffectResult &result);
  void require_settled_quest_cache_locked() const;
  void require_authoritative_guild_cache_locked() const;
  bool store_progression_locked(GuildEntry &entry, const GuildRecord &updated,
                                std::vector<GuildEvent> &events);
  x5::ec::EErrorCode check_company(const GuildActor &actor, const GuildEntry &entry) const;
  void remove_guild_locked(GuildEntry &entry, const GuildHistory *master_history,
                           std::vector<GuildEvent> &events);

  void load_snapshot(GuildSnapshot snapshot);
  void repair_loaded_guilds();
  const GuildEntry *find_guild(std::int64_t guild_id) const;
  GuildEntry *find_guild(std::int64_t guild_id);
  const GuildEntry *guild_of(std::int64_t char_id) const;
  GuildHistory history_of(std::int64_t char_id) const;
  bool cooldown_active(std::int64_t anchor_ticks, std::chrono::hours cooldown,
                       std::int64_t now_ticks) const;
  std::size_t member_limit(const GuildRecord &guild) const;
  std::size_t sub_master_count(const GuildEntry &entry) const;
  GuildSummary summarize(const GuildEntry &entry) const;
  std::vector<GuildMemberRecord> roster(const GuildEntry &entry) const;
  std::vector<std::int64_t> recipients(const GuildEntry &entry, std::int64_t excluded) const;
  x5::ec::EErrorCode validate_actor(const GuildActor &actor) const;
  x5::ec::EErrorCode validate_join_settings(int join_type, int join_level,
                                            int join_company_position) const;
  x5::ec::EErrorCode admit(const GuildActor &actor, const GuildEntry &entry,
                           bool check_join_level, std::int64_t now_ticks) const;
  // Stores the new member and appends GuildMemberJoined. `result` may be null.
  x5::ec::EErrorCode add_member(GuildEntry &entry, const GuildActor &actor, bool via_invitation,
                                std::vector<GuildEvent> &events, GuildJoinResult *result);
  x5::ec::EErrorCode kick(GuildEntry &entry, const GuildMemberRecord &actor_member,
                          const GuildMemberRecord &target, std::vector<GuildEvent> &events);
  x5::ec::EErrorCode change_grade(GuildEntry &entry, const GuildMemberRecord &actor_member,
                                  const GuildMemberRecord &target, GuildGrade grade,
                                  std::vector<GuildEvent> &events);
  void prune_expired_invites();
  void forget_invites_for_guild(std::int64_t guild_id);

  void publish(const std::vector<GuildEvent> &events, bool require_delivery = false) const;

  mutable std::mutex mutex_;
  std::mutex progression_reconciliation_mutex_;
  std::shared_ptr<GuildStore> store_;
  GuildRules rules_;
  int world_id_{};
  GuildClock clock_;
  std::map<std::int64_t, GuildEntry> guilds_;
  std::unordered_map<std::int64_t, std::int64_t> guild_by_member_;
  // Keys are ASCII-folded names; the realm forbids case-insensitive duplicates.
  std::map<std::string, std::int64_t> guild_by_folded_name_;
  std::unordered_map<std::int64_t, GuildHistory> histories_;
  // One pending invitation per target character.
  std::unordered_map<std::int64_t, PendingInvite> invites_;
  // Character experience not yet converted into a whole guild experience point.
  std::unordered_map<std::int64_t, std::int64_t> experience_remainders_;
  std::optional<QuestServiceEffectEvent> uncertain_quest_effect_;
  std::optional<UncertainProgression> uncertain_progression_;
  bool progression_readback_complete_{};
  std::function<void()> before_progression_notification_for_test_;
  std::vector<GuildEventSubscriber> subscribers_;
};

// Text rules from GameVariable GuildNameRegex/GuildDescRegex, counted in Unicode code points
// (Candidate: "UnitLength" read as code points). Exposed for tests.
bool valid_guild_name_text(std::string_view name);

bool valid_guild_description_text(std::string_view description);

// Number of UTF-8 code points, or nullopt for malformed UTF-8.
std::optional<std::size_t> utf8_code_points(std::string_view text);

} // namespace chronicles
