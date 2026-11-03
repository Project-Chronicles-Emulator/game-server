#pragma once

// Company (in-game faction) membership, contribution, position levels and pioneer permits.
//
// CompanyService is transport-free and owns one realm's company records. Every change is
// written through a CompanyStore before it is applied, so a failed write changes nothing.
// World sessions call it for company dialogues, experience-driven contribution and GM
// commands; the guild and character-selection paths read positions from it.
//
// Evidence (docs/companies.md, dumps/ida_research/guild-expansion-20261026):
// - Catalog: Company (five companies, requireContributionToChangeCompany 4000),
//   CompanyPosition (per-company levels, nextLevelContribution, isMaxPositionLevel,
//   changePenaltyContribution, pioneer permit cost and duration), GameVariable
//   CompanyChangeLimitMin 1440 and RenewPioneerPermitDurationMinuteBeforeExpiry 1440.
// - Client dialogue checks before PC2S_DialogueFunctionReq: CompanyJoin requires no company;
//   CompanyChange requires a company and join time + CompanyChangeLimitMin <= now
//   (DialogueErrorCode 1104); pioneer permits require a company (1103) and the position's
//   money (1101); extension requires remaining time < the renewal window (1102).
// - Times are 100 ns Unix-epoch ticks, the unit of WorldCharCompany's *_time_since_epoch.
//
// Candidate interpretations are marked where they are used.

#include "common/clock_ticks.hpp"
#include "ec.generated.pb.h"
#include "p_character.pb.h"
#include "persistence/company_record.hpp"
#include "persistence/quest_service_effects.hpp"
#include "world/features/company/company_quest_settlement.hpp"
#include "persistence/memory_repositories.hpp"

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
#include <unordered_set>
#include <vector>

namespace chronicles {

struct MemoryQuestServiceCoordinator;

class ClientCatalog;

struct CompanyPositionRule {
  int level{};
  // Contribution needed to advance from this level to the next one.
  std::int64_t next_level_contribution{};
  // CompanyPosition.isMaxPositionLevel. Candidate: a flagged level is a cap. Earned
  // contribution stops just below this level's nextLevelContribution until a
  // CompanyLevelUnlock reward (or GM command) unlocks a level at least this high. Capping the
  // contribution, not the level, keeps the client's own level calculation (which reads only
  // contribution) in agreement. Every exported unlock reward (BlueSalt 2 and 3) lifts a cap.
  bool locks_advancement{};
  // Candidate: contribution at this level does not decay. Decay itself is not modeled.
  bool safe_contribution{};
  std::int64_t change_penalty_contribution{};
  std::chrono::minutes pioneer_permit_duration{};
  // EX5MoneyType of the permit cost (Gold = 0, HersanderToken = 1).
  int pioneer_permit_money_type{};
  std::int64_t pioneer_permit_money_value{};
};

struct CompanyDefinition {
  int type{};
  std::string name;
  // Candidate: contribution a character must hold to change into this company.
  std::int64_t required_contribution_to_change{};
  // Ordered by level and contiguous from 1.
  std::vector<CompanyPositionRule> positions;
};

struct CompanyRules {
  std::map<int, CompanyDefinition> companies;
  std::chrono::minutes change_cooldown{1440};
  std::chrono::minutes permit_renewal_window{1440};
  // Emulator policy (no official source is reachable): one contribution point per this much
  // character experience earned while in a company.
  std::int64_t character_experience_per_contribution{20};
};

// Reads Company, CompanyPosition and GameVariable rows. Invalid rows are skipped and logged;
// a company without valid positions is not joinable.
CompanyRules load_company_rules(ClientCatalog &catalog);

using CompanyQuestPlanner = std::function<CompanyRecord(std::int64_t)>;

// Persistence for company records. Every method is one transaction and throws on failure.
class CompanyStore {
public:
  virtual ~CompanyStore() = default;

  virtual std::vector<CompanyRecord> load(int world_id) = 0;

  virtual void save(const CompanyRecord &record) = 0;

  // Adopts the epoch acquired by the server; it never changes the global owner epoch.
  virtual void adopt_quest_owner_epoch(std::int64_t) {
    throw std::runtime_error("Company quest settlement epoch is unavailable");
  }

  // Both methods fence any prior transaction and return the authoritative company row.
  virtual CompanyQuestSettlementResult read_quest_effect(const QuestServiceEffectEvent &) {
    throw std::runtime_error("Company quest settlement is unavailable");
  }
  virtual CompanyQuestSettlementResult settle_quest_effect(const QuestServiceEffectEvent &,
                                                           const CompanyRecord &,
                                                           const CompanyQuestPlanner &) {
    throw std::runtime_error("Company quest settlement is unavailable");
  }

  // Atomically commit the permit and the matching currency transition. False means a
  // stale company/progress revision or an unavailable atomic backend.
  virtual bool commit_permit(std::int64_t, int, const CompanyRecord &, const CompanyRecord &,
                             const WorldProgress &, const WorldProgress &) {
    return false;
  }
};

class MemoryCompanyStore final : public CompanyStore {
public:
  // The concrete memory repository: commit_permit uses commit_company_permit, which exists
  // only on the memory character-state repository.
  explicit MemoryCompanyStore(
      std::shared_ptr<MemoryCharacterStateRepository> progress_store = {},
      std::shared_ptr<MemoryQuestServiceCoordinator> quest_services = {});

  std::vector<CompanyRecord> load(int world_id) override;

  void save(const CompanyRecord &record) override;

  void adopt_quest_owner_epoch(std::int64_t epoch) override;
  CompanyQuestSettlementResult read_quest_effect(const QuestServiceEffectEvent &event) override;
  CompanyQuestSettlementResult settle_quest_effect(const QuestServiceEffectEvent &event,
                                                    const CompanyRecord &before,
                                                    const CompanyQuestPlanner &planner) override;

  bool commit_permit(std::int64_t account_id, int world_id,
                     const CompanyRecord &before_company, const CompanyRecord &after_company,
                     const WorldProgress &before_progress,
                     const WorldProgress &after_progress) override;

private:
  std::mutex mutex_;
  std::map<std::int64_t, CompanyRecord> records_;
  std::shared_ptr<MemoryCharacterStateRepository> progress_store_;
  std::shared_ptr<MemoryQuestServiceCoordinator> quest_services_;
  std::atomic<std::int64_t> quest_owner_epoch_{};
};

// Uses emulator_character_company (schema version 19). Rows cascade with their character.
class PostgresCompanyStore final : public CompanyStore {
public:
  explicit PostgresCompanyStore(std::string connection);

  std::vector<CompanyRecord> load(int world_id) override;

  void save(const CompanyRecord &record) override;

  void adopt_quest_owner_epoch(std::int64_t epoch) override;
  CompanyQuestSettlementResult read_quest_effect(const QuestServiceEffectEvent &event) override;
  CompanyQuestSettlementResult settle_quest_effect(const QuestServiceEffectEvent &event,
                                                    const CompanyRecord &before,
                                                    const CompanyQuestPlanner &planner) override;

  bool commit_permit(std::int64_t account_id, int world_id,
                     const CompanyRecord &before_company, const CompanyRecord &after_company,
                     const WorldProgress &before_progress,
                     const WorldProgress &after_progress) override;

private:
  std::string connection_;
  std::atomic<std::int64_t> quest_owner_epoch_{};
};

struct CompanyStatus {
  CompanyRecord record;
  // Zero when the character has no company.
  int position_level{};
  bool permit_valid{};
  // The contribution is held exactly at the ceiling of a level flagged isMaxPositionLevel
  // (BlueSalt level 2: 1500, level 3: 11500). Company requirements with is_max_contribution
  // (for example Donella's housing quests) test this.
  bool at_capped_level_ceiling{};
};

struct CompanyResult {
  x5::ec::EErrorCode error{x5::ec::Success};
  CompanyStatus status;
};

// Gold or token cost of a pioneer permit, paid by the caller before commit_permit().
struct PermitPlan {
  x5::ec::EErrorCode error{x5::ec::Success};
  int money_type{};
  std::int64_t money_value{};
  std::int64_t new_expiry_epoch_ticks{};
  CompanyRecord expected_company;
};

// Result of converting earned character experience into contribution.
struct ContributionGain {
  std::int64_t contribution{};
  bool position_changed{};
  CompanyStatus status;
};

// Thread-safe. Operations serialize on one lock, including their store write.
class CompanyService {
public:
  // `clock` returns the current time in 100 ns Unix-epoch ticks.
  CompanyService(std::shared_ptr<CompanyStore> store, CompanyRules rules, int world_id,
                 std::function<std::int64_t()> clock = unix_epoch_ticks_now);

  const CompanyRules &rules() const;

  CompanyStatus status(std::int64_t char_id) const;

  // Position level for a contribution total in a company: the first level whose
  // nextLevelContribution exceeds it, as the client computes it
  // (AAC_Company_Catalog_PositionLevelForContribution_Candidate); the last level beyond every
  // threshold. Zero for an unknown company.
  int position_level(int company_type, std::int64_t contribution) const;

  // Highest contribution earned contribution may reach before an unlock: one below the
  // nextLevelContribution of the first capped level the record has not unlocked.
  std::int64_t contribution_cap(const CompanyRecord &record) const;

  // Contribution total at which `level` is reached in a company; nullopt for an unknown
  // company or level.
  std::optional<std::int64_t> contribution_for_position(int company_type, int level) const;

  CompanyResult join(std::int64_t char_id, int company_type);

  // Changes company: requires the change cooldown and the target's required contribution,
  // deducts the current position's change penalty and clears the permit (Candidate).
  CompanyResult change(std::int64_t char_id, int company_type);

  // Adds contribution to a company member, stopping at contribution_cap(). Non-members gain
  // nothing (nullopt).
  std::optional<ContributionGain> add_contribution(std::int64_t char_id, std::int64_t amount);

  // Converts earned character experience into contribution with a per-character remainder.
  std::optional<ContributionGain> record_character_experience(std::int64_t char_id,
                                                              std::int64_t experience);

  // CompanyLevelUnlock reward: raises the unlocked level for a member of `company_type`.
  CompanyResult unlock_position(std::int64_t char_id, int company_type, int level);

  PermitPlan plan_permit(std::int64_t char_id, bool extend) const;

  CompanyResult commit_permit(std::int64_t char_id, std::int64_t expiry_epoch_ticks);

  CompanyResult commit_paid_permit(std::int64_t account_id, const PermitPlan &plan,
                                   const WorldProgress &before,
                                   const WorldProgress &after);

  // Recovery retains an uncertain event even after its durable applied marker disappears
  // from pending enumeration. Both methods hold the company cache mutex through readback.
  void adopt_quest_owner_epoch(std::int64_t epoch);
  std::optional<CompanyQuestSettlementResult> reconcile_uncertain_quest_effect();
  CompanyQuestSettlementResult settle_quest_effect(const QuestServiceEffectEvent &event);

  // Validates an operator record without changing storage or the in-memory company cache.
  void validate_record(const CompanyRecord &record) const;

  // Operator/GM override. Throws std::invalid_argument for an unknown company.
  CompanyResult set_record(const CompanyRecord &record);

private:
  void ensure_quest_cache_available_locked(std::int64_t character_id) const;
  CompanyRecord plan_quest_company_effect(const CompanyRecord &before,
                                         const QuestServiceEffects &effects,
                                         std::int64_t activity_contribution) const;
  const CompanyDefinition *definition(int company_type) const;
  CompanyRecord record_locked(std::int64_t char_id) const;
  CompanyStatus status_locked(const CompanyRecord &record) const;
  CompanyResult store_locked(const CompanyRecord &record);
  std::optional<ContributionGain> contribute_locked(std::int64_t char_id, std::int64_t amount);

  mutable std::mutex mutex_;
  std::shared_ptr<CompanyStore> store_;
  CompanyRules rules_;
  int world_id_{};
  std::function<std::int64_t()> clock_;
  std::unordered_map<std::int64_t, CompanyRecord> records_;
  std::optional<QuestServiceEffectEvent> uncertain_quest_effect_;
  std::unordered_set<std::int64_t> blocked_quest_characters_;
  // Character experience not yet converted into a whole contribution point.
  std::unordered_map<std::int64_t, std::int64_t> experience_remainders_;
};

// Protobuf message for JoinCompanyRes, CompanyUpdateNoti, RegisterPioneerPermitRes and
// WorldCharacter.company. A character without a company yields an empty message.
x5::pb::WorldCharCompany world_char_company(const CompanyRecord &record);

// "EX5CompanyType_<Name>" for a valid company type; empty otherwise.
std::string company_type_name(int company_type);

} // namespace chronicles
