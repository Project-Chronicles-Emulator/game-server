#pragma once

#include "world/core/corpse_handling.hpp"

#include "auth/auth.hpp"
#include "world/features/combat/combat.hpp"
#include "ec.generated.pb.h"
#include "world/features/loot/loot_rules.hpp"
#include "persistence/character_repository.hpp"
#include "persistence/character_state_types.hpp"
#include "world/features/tutorial/tutorial.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace chronicles {

// Recovered world opcodes used by corpse looting (build 40367e24...).
inline constexpr std::uint16_t kLootItemRequestOpcode = 20079;
inline constexpr std::uint16_t kLootItemResponseOpcode = 26;
inline constexpr std::uint16_t kLootUnitRemoveOpcode = 11;
inline constexpr std::uint16_t kLootItemsUpdateOpcode = 169;
inline constexpr std::uint16_t kUnitLootItemInfoOpcode = 173;

// Emulator bounds. One corpse never grants more item rows than a single storage trade accepts
// (storage allows 64), and a session never tracks an unbounded number of corpses.
inline constexpr std::size_t kMaximumLootInstancesPerCorpse = 32;
inline constexpr std::size_t kMaximumInstancesPerLootEntry = 16;
inline constexpr std::size_t kMaximumTrackedCorpses = 1024;
// Corpses shared between party members, across every session of the process.
inline constexpr std::size_t kMaximumSharedCorpses = 4096;

enum class LootItemStorage {
  // One WorldProgress stack per CMS ID.
  stackable,
  // One CharacterItem row per unit, with the catalog grade and full durability.
  instance,
};

struct LootItemDefinition {
  LootItemStorage storage{LootItemStorage::stackable};
  // x5.cms.EX5ItemGrade value from the Item table.
  int grade{};
  int max_durability{};
  // CUSTOM (guessed, not official): the item drops only while the character has one of these
  // quests active. Empty means no gate.
  std::vector<int> required_quests;
};

// One weighted alternative inside a loot group: an item or authored gold.
struct LootEntry {
  bool is_gold{};
  int item_cms_id{};
  int min_quantity{};
  int max_quantity{};
  int weight{};
  // CUSTOM (guessed, not official): polish step of each dropped instance; zero for none.
  int polish_step{};
};

// LootCondition row: an independent chance (percent) to pick one weighted entry.
struct LootGroup {
  double rate_percent{};
  // CUSTOM (guessed, not official): drops only on the character's first looted kill of the NPC.
  bool first_kill_only{};
  std::vector<LootEntry> entries;
};

// LootPack row: a chance (percent) that the pack is rolled at all, then its groups.
struct LootPack {
  int id{};
  double rate_percent{};
  std::vector<LootGroup> groups;
  bool has_first_kill_group{};
};

// Loot facts about one monster NPC.
struct LootNpc {
  int pack_id{};
  // CUSTOM (guessed, not official): a second pack rolled with probability handling_rate_percent.
  int handling_pack_id{};
  double handling_rate_percent{};
  // CUSTOM (guessed, not official): gold fitted to the captures; see tools/build_loot_data.py.
  std::int64_t kill_gold{};
};

// Immutable corpse-loot indexes built by tools/build_loot_data.py. Load and validate once at
// startup; lookups never parse JSON. Groups and NPC facts that depend on a switched-off custom
// rule are withheld here, so the rest of the loot code only sees what is enabled.
class LootData {
public:
  static std::shared_ptr<const LootData>
  load(const std::filesystem::path &path,
       const LootCustomRules &rules = LootCustomRules::all_enabled());

  // Throws std::runtime_error for a document that is malformed, internally inconsistent or
  // built for another client build.
  explicit LootData(const nlohmann::json &document,
                    const LootCustomRules &rules = LootCustomRules::all_enabled());

  // Null when the NPC has no loot facts the server models and the rules allow.
  const LootNpc *npc(int npc_cms_id) const;
  const LootPack *pack(int pack_id) const;
  const LootPack *handling_pack(int pack_id) const;
  const LootItemDefinition *item(int cms_id) const;

  const LootCustomRules &rules() const;
  double looting_range_mm() const;

  // How long a corpse that still has loot remains, and how long an emptied one does. The custom
  // rules may override the catalog's NpcCorpse*DestroyMillis.
  std::int64_t corpse_lootable_ms() const;
  std::int64_t corpse_empty_ms() const;

  std::int64_t gold_limit() const;

  std::size_t npc_count() const;
  std::size_t pack_count() const;
  // Groups that the profile carries but the active rules withhold.
  std::size_t withheld_group_count() const;

private:
  LootCustomRules rules_;
  std::unordered_map<int, LootPack> packs_;
  std::unordered_map<int, LootPack> handling_packs_;
  std::unordered_map<int, LootNpc> npcs_;
  std::unordered_map<int, LootItemDefinition> items_;
  double looting_range_mm_{};
  std::int64_t corpse_lootable_ms_{};
  std::int64_t corpse_empty_ms_{};
  std::int64_t gold_limit_{};
  std::size_t withheld_groups_{};
};

// Source of every random decision in a loot roll, so tests can script exact outcomes.
class LootRandom {
public:
  virtual ~LootRandom() = default;

  // True when a chance of rate_percent succeeds. A rate of 100 always succeeds.
  virtual bool passes(double rate_percent) = 0;

  // Index into weights, proportionally to each positive weight.
  virtual std::size_t pick_index(const std::vector<int> &weights) = 0;

  // Uniform integer in [minimum, maximum].
  virtual int pick_quantity(int minimum, int maximum) = 0;
};

class SeededLootRandom final : public LootRandom {
public:
  explicit SeededLootRandom(std::uint32_t seed);

  bool passes(double rate_percent) override;
  std::size_t pick_index(const std::vector<int> &weights) override;
  int pick_quantity(int minimum, int maximum) override;

private:
  std::mt19937 engine_;
};

// One item instance a corpse will create.
struct LootInstance {
  int item_cms_id{};
  int polish_step{};

  bool operator==(const LootInstance &) const = default;
};

// What one corpse holds after the death roll.
struct LootContents {
  std::int64_t gold{};
  // CMS ID to dropped quantity (not the owner's resulting total).
  std::map<int, std::int64_t> stackables;
  std::vector<LootInstance> instances;

  bool empty() const;
};

// Character state a roll may consult. A null progress means no quests and no first kills.
struct LootRollContext {
  const WorldProgress *progress{};
  // CUSTOM (guessed, not official): false once the character already received this NPC's
  // first-kill groups, or while another corpse of this kind is waiting to be looted.
  bool first_kill_available{};
};

// Rolls pack, then each group, then one weighted entry and its quantity, in that order. Entries
// that need an inactive quest are not alternatives; first-kill groups are skipped unless the
// context allows them.
LootContents roll_loot(const LootData &data, const LootPack &pack, LootRandom &random,
                       const LootRollContext &context = {});

// The complete corpse roll for one monster.
struct RolledCorpse {
  LootContents contents;
  // CUSTOM (guessed, not official): the Npc.FK_handlingLootPackId pack applied and its contents
  // joined the ordinary corpse take.
  bool handling_loot{};
  // The NPC's first-kill groups were available and are consumed by taking this corpse.
  bool consumes_first_kill{};
};

// Kill gold, then the loot pack, then the handling pack (when its rate passes).
RolledCorpse roll_corpse(const LootData &data, const LootNpc &npc, LootRandom &random,
                         const LootRollContext &context);

// Everything the session needs to plan and commit one take, loaded from storage or from the
// tutorial session's cached progress. Callbacks run on the calling session's worker strand.
struct LootCommit {
  // Inventory snapshot used to choose free slots; granted rows are not in it yet.
  Character character;
  WorldProgress before;
  // Already advances before.revision by one.
  WorldProgress after;
  std::vector<CharacterItem> granted_items;
  std::string receipt_id;
  std::string corpse_fingerprint;
};

struct LootPersistence {
  std::function<std::optional<Character>()> load_character;
  // Must not touch storage when it also serves the defeat observer (tutorial sessions return
  // cached progress).
  std::function<WorldProgress()> load_progress;
  // Atomically commits progress and new item instances under the revision in commit.before.
  // Returns the stored rows (with IDs), or nullopt when the revision or a slot conflicts and
  // nothing was written.
  std::function<std::optional<std::vector<CharacterItem>>(const LootCommit &)> commit;
  std::function<LootClaimResult(const std::string &, const std::string &)> lookup_claim;
  // Production uses a durable receipt; the legacy callback remains for disposable fixtures.
  std::function<LootClaimResult(const LootCommit &)> commit_once;
  // A lost acknowledgement or replay requires a fresh world-entry inventory snapshot.
  std::function<void()> force_reconnect;
  // Optional carry-capacity policy; an empty function accepts every take. The emulator has no
  // evidence-backed weight rule yet, so production wiring leaves it empty.
  std::function<bool(const LootCommit &)> within_weight_limit;
  // Optional clock (100 ns Unix-epoch ticks) stamped on created items as created_epochtime.
  std::function<std::int64_t()> epoch_ticks;
};

// Storage wiring for a character in a private tutorial instance. Progress comes from the
// tutorial session's cached revision, and a successful commit is adopted by that session so its
// later writes compare against stored state. items_changed receives the character with the
// newly stored item rows appended, so cached world snapshots can follow the inventory.
// auth and tutorial must outlive the returned persistence.
LootPersistence tutorial_loot_persistence(AuthService &auth, const Handoff &handoff,
                                          TutorialSession &tutorial,
                                          std::function<void(const Character &)> items_changed);

// CUSTOM (guessed, not official): party loot. The loot service asks the audience who may share a
// kill and tells those characters about it. Implementations must not block.
class LootAudience {
public:
  virtual ~LootAudience() = default;

  // Character IDs of the killer's party members who may share this kill (same instance and
  // within range_mm of it), excluding the killer.
  virtual std::vector<std::int64_t> party_members_near(const MonsterDefeat &defeat,
                                                       double range_mm) = 0;

  // Queues packets for another character's connection.
  virtual void notify(std::int64_t character_id, std::vector<GameplayPacket> packets) = 0;
};

struct SharedCorpseKey {
  std::int64_t owner_character_id{};
  std::uint64_t unit_id{};

  auto operator<=>(const SharedCorpseKey &) const = default;
};

// A corpse that several party members may take from. The first eligible member to claim it
// receives everything; the others find it taken.
struct SharedCorpse {
  // Every character that may take the corpse, including the owner (the killer).
  std::set<std::int64_t> eligible;
  WorldPosition position;
  LootContents contents;
  int npc_cms_id{};
  bool records_first_kill{};
  std::string receipt_id;
  std::string corpse_fingerprint;
  bool taken{};
  bool published{true};
  std::int64_t expire_tick{};
};

enum class ClaimStatus {
  claimed,
  unknown,
  expired,
  not_eligible,
  already_taken,
};

struct SharedClaim {
  ClaimStatus status{ClaimStatus::unknown};
  // The corpse as claimed; present only for ClaimStatus::claimed.
  std::optional<SharedCorpse> corpse;
};

// Process-wide, thread-safe claim state for shared corpses. Contents live here, not in any one
// session, so exactly one claim can succeed.
class SharedLootBoard {
public:
  // False when the board is full or the key is already open.
  bool open(const SharedCorpseKey &key, SharedCorpse corpse);

  // Atomically takes the corpse for claimant. The claimant must commit and then leave the claim
  // in place, or call release() when the commit fails.
  SharedClaim claim(const SharedCorpseKey &key, std::int64_t claimant, std::int64_t now_ticks);

  // Undoes a claim whose storage commit failed, so another attempt can succeed.
  void release(const SharedCorpseKey &key);

  struct MemberView {
    SharedCorpseKey key;
    WorldPosition position;
  };

  // A corpse this member (not its owner) may share, found by unit ID. Expired corpses are not
  // returned; taken ones are, so the member learns the corpse is gone for them.
  std::optional<MemberView> find_for_member(std::uint64_t unit_id, std::int64_t member,
                                            std::int64_t now_ticks) const;

  std::set<std::int64_t> eligible(const SharedCorpseKey &key) const;

  // Makes a staged corpse claimable and extends its lifetime after public death.
  bool publish(const SharedCorpseKey &key, std::int64_t expire_tick);

  // Pending registration may be retried after its original corpse deadline.
  void extend_expiry(const SharedCorpseKey &key, std::int64_t expire_tick);

  // The owner forgot the corpse (it expired).
  void close(const SharedCorpseKey &key);

  std::size_t size() const;

private:
  mutable std::mutex mutex_;
  std::map<SharedCorpseKey, SharedCorpse> corpses_;
};

// A corpse its killer may butcher with a knife (PC2S_StartInteractionAnimReq param.handling_loot).
struct ButcherableCorpse {
  int npc_cms_id{};
  WorldPosition position;
  std::shared_ptr<CorpseHandlingState> handling;
};

// One character's corpse loot. Calls are serialized by the world session. A kill rolls the
// corpse contents once; the owner takes them with one atomic commit, after which the corpse is
// empty and cannot be taken again. A corpse with loot stays for its own lifetime and the session
// removes it from the client; combat removes every other corpse on its default timer.
//
// A butcherable corpse also stays for the lootable lifetime until its killer butchers it; the
// butchering itself (animation, tool, reward) belongs to the gathering service.
//
// With party_shared_loot and an audience, the contents move to a SharedLootBoard and every
// eligible member may take them, first come first served, as a whole.
class LootSession {
public:
  LootSession(std::shared_ptr<const LootData> data, std::int64_t character_id,
              LootPersistence persistence, std::unique_ptr<LootRandom> random,
              std::shared_ptr<LootAudience> audience = nullptr,
              std::shared_ptr<SharedLootBoard> board = nullptr);

  bool handles(std::uint16_t opcode) const;

  // Monsters whose corpse the killer may butcher. Unset, no corpse can be butchered.
  void set_butchering(std::function<bool(int npc_cms_id)> butcherable);

  // Registers the corpse. When its NPC has loot facts and this character is the killer, the
  // reaction announces the loot and takes over the corpse's removal from combat.
  MonsterDefeatReaction monster_defeated(const MonsterDefeat &defeat);

  // The unexpired corpse this character killed and has not butchered yet.
  std::optional<ButcherableCorpse> butcherable_corpse(std::uint64_t unit_id,
                                                      std::int64_t ticks) const;

  // Records a committed butchering and returns the UnitLootItemInfoNoti with the remaining
  // flags. An already taken corpse then expires NpcCorpseNoneDestroyMillis later.
  std::vector<GameplayPacket> corpse_butchered(std::uint64_t unit_id, std::int64_t ticks);

  // Handles PC2S_LootItemReq. Every outcome is a LootItemRes, so the client always gets an
  // answer; a rejected request leaves the corpse lootable.
  std::vector<GameplayPacket> receive(const nlohmann::json &request,
                                      const WorldPosition &player_position, std::int64_t ticks);

  // Forgets corpses whose deadline has passed and returns PS2C_UnitRemoveNoti for the ones whose
  // removal this session manages.
  std::vector<GameplayPacket> advance(std::int64_t ticks);

  std::optional<std::int64_t> next_action_tick() const;

  std::size_t corpse_count() const;

private:
  struct Corpse {
    std::int64_t owner_character_id{};
    int npc_cms_id{};
    WorldPosition position;
    std::int64_t death_tick{};
    std::string reward_event_id;
    std::string receipt_id;
    std::string corpse_fingerprint;
    LootContents contents;
    // True once the NPC announced loot and until it has been taken.
    bool lootable{};
    // True while the killer may still butcher the corpse with a knife.
    bool butcherable{};
    std::shared_ptr<CorpseHandlingState> handling;
    // True when this session, not combat, sends the unit removal at expire_tick.
    bool announces_removal{};
    // The contents live in the shared board.
    bool shared{};
    // Taking the corpse records the NPC as first-killed (CUSTOM).
    bool records_first_kill{};
    std::int64_t expire_tick{};
    // Registration may resume after a party callback throws; the rolled contents and
    // recipient order stay attached to the same corpse until registration finishes.
    std::vector<std::int64_t> pending_party_members;
    std::size_t next_party_notification{};
    bool registration_complete{};
    bool publication_pending{};
    bool sharing_decided{};
    bool party_query_complete{};
    bool has_chance_to_loot{};
  };

  // What a take consumes: the contents and, for a first kill, the NPC to record.
  struct TakeSource {
    LootContents contents;
    int npc_cms_id{};
    bool records_first_kill{};
    std::string receipt_id;
    std::string corpse_fingerprint;
  };

  x5::ec::EErrorCode check_range(const WorldPosition &corpse_position,
                                 const WorldPosition &player_position) const;

  // Fills commit with the progress and item rows that taking the source would write.
  x5::ec::EErrorCode plan_take(const TakeSource &source, LootCommit &commit) const;

  // PS2C_ItemsUpdateNoti (reason looting) for committed contents. Empty when nothing dropped.
  std::optional<GameplayPacket> items_update_packet(std::uint64_t unit_id,
                                                    const LootContents &contents,
                                                    const LootCommit &commit,
                                                    const std::vector<CharacterItem> &stored) const;

  struct TakeOutcome {
    x5::ec::EErrorCode error{x5::ec::Success};
    std::optional<GameplayPacket> items_update;
    bool claimed_by_other{};
    bool receipt_mismatch{};
  };

  // Plans and atomically commits one take. Nothing is written unless error is Success.
  TakeOutcome commit_take(std::uint64_t unit_id, const TakeSource &source);

  // The corpse a request names: this session's own, or a party member's shared one.
  struct Target {
    Corpse *own{};
    std::optional<SharedCorpseKey> shared_key;
    WorldPosition position;
  };
  std::optional<Target> find_target(std::uint64_t unit_id, std::int64_t ticks);

  // Claims or reads the contents of a target, or names the refusal.
  x5::ec::EErrorCode acquire(const Target &target, std::int64_t ticks, TakeSource &source);

  MonsterDefeatReaction finish_corpse_registration(const MonsterDefeat &defeat, Corpse &corpse);
  void publish_corpse(std::uint64_t unit_id, const std::string &event_id, std::int64_t ticks);
  void notify_pending_party_members(std::uint64_t unit_id, Corpse &corpse);
  void tell_party_taken(const SharedCorpseKey &key, std::uint64_t unit_id);
  void mark_corpse_emptied(Corpse &corpse, std::int64_t ticks);
  void shorten_to_emptied_lifetime(Corpse &corpse, std::int64_t ticks);

  std::shared_ptr<const LootData> data_;
  std::int64_t character_id_{};
  LootPersistence persistence_;
  std::unique_ptr<LootRandom> random_;
  std::shared_ptr<LootAudience> audience_;
  std::shared_ptr<SharedLootBoard> board_;
  std::function<bool(int npc_cms_id)> butcherable_;
  std::map<std::uint64_t, Corpse> corpses_;
  // NPCs whose first-kill groups a waiting corpse of this session will consume.
  std::set<int> first_kills_reserved_;
};

} // namespace chronicles
