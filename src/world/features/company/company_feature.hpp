#pragma once

#include "world/core/frame_context.hpp"
#include "world/features/company/company.hpp"
#include "world/features/company/company_dialogues.hpp"
#include "world/features/company/company_world.hpp"

#include <memory>
#include <optional>

namespace chronicles::company {

class CompanySession;
class CompanySessionContext;

// Realm services are also used by front/character selection; ownership remains shared.
class CompanyFeature final {
public:
  CompanyFeature(std::shared_ptr<CompanyService> service,
                 std::shared_ptr<const CompanyDialogueIndex> dialogues);

  std::unique_ptr<CompanySession> create_session(CompanySessionContext &context);
  const std::shared_ptr<CompanyService> &service() const noexcept;
  bool has_dialogues() const noexcept;

private:
  friend class CompanySession;

  std::shared_ptr<CompanyService> service_;
  std::shared_ptr<const CompanyDialogueIndex> dialogues_;
};

struct CompanyCharacter {
  std::int64_t account_id{};
  std::int64_t character_id{};
};

class CompanySessionContext {
public:
  virtual ~CompanySessionContext() = default;

  virtual std::optional<CompanyCharacter> company_character() const = 0;
  virtual std::optional<std::int64_t> company_selected_character_id() const = 0;
  // Activity EXP rewards only track the tutorial's cached copy.
  virtual const WorldProgress *company_tutorial_progress() const = 0;
  // Progress a permit charge commits from: the tutorial's cached copy, or stored progress on
  // shared-world maps, which have no tutorial instance. nullopt before world entry.
  virtual std::optional<WorldProgress> company_current_progress() const = 0;
  virtual std::optional<int> company_dialogue_npc() const = 0;
  virtual bool company_has_guild_membership(std::int64_t character_id) const = 0;

  // Synchronous typed bus publications. EXP publication completes before company
  // contribution; membership publication refreshes the guild roster.
  virtual void company_publish_activity_experience(std::int64_t character_id,
                                                   std::int64_t experience) = 0;
  virtual void company_publish_membership_changed() = 0;
  virtual void company_publish_progress_committed(const WorldProgress &progress) = 0;
};

class CompanySession final {
public:
  CompanySession(CompanyFeature &feature, CompanySessionContext &context);

  // Initialized at the old post-selection point, only when a tutorial session exists.
  void on_select_character(const WorldProgress &progress);
  void collect_activity_rewards(std::vector<GameplayPacket> &packets);
  std::vector<GameplayPacket> dialogue(const WorldGameplayRequest &request);

private:
  std::vector<GameplayPacket> pioneer_permit(std::uint32_t sequence, int company_type, bool extend);

  CompanyFeature &feature_;
  CompanySessionContext &context_;
  std::optional<std::int64_t> last_experience_;
};

} // namespace chronicles::company
