#pragma once

#include "world/core/progress_access.hpp"
#include "world/core/world_gameplay_request.hpp"
#include "world/features/loot/loot.hpp"
#include "world/features/tutorial/tutorial_player_session.hpp"

#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace chronicles::adventure {

class Catalog {
public:
  static std::shared_ptr<const Catalog> load(const std::filesystem::path &path);
  explicit Catalog(const nlohmann::json &document);

  std::shared_ptr<const TutorialData> map(int map_id) const;
  std::size_t map_count() const noexcept;
  std::vector<int> map_ids() const;
  nlohmann::json roll_rewards(int pack_id, LootRandom &random, const WorldProgress &progress) const;

private:
  std::map<int, std::shared_ptr<const TutorialData>> maps_;
  LootData loot_;
};

// A shared placement has character-owned completion/rewards. It is never depleted globally.
// All calls run on the owning session worker, including the existing atomic item commit.
class Session {
public:
  // Answers whether the current character at `position` meets an object's exported interaction
  // conditions (catalog-shaped Quest/OwnItem rows, see build_adventure_objects.py).
  using RequirementCheck =
      std::function<bool(const nlohmann::json &conditions, const WorldPosition &position)>;

  Session(std::shared_ptr<const Catalog> catalog, std::int64_t wire_unit_id,
          ProgressAccess &progress, TutorialSession::Commit commit,
          tutorial::TutorialRewardPersistence items, std::shared_ptr<const Progression> progression,
          std::unique_ptr<LootRandom> random = {});

  // Without a check, objects with interaction conditions are refused (fail closed).
  void set_requirement_check(RequirementCheck check);

  bool handles(const WorldGameplayRequest &request, const WorldPosition &position) const;
  bool busy() const;
  std::optional<std::int64_t> next_action_tick() const;
  std::vector<GameplayPacket> receive(const WorldGameplayRequest &request,
                                      const WorldPosition &position, std::int64_t now,
                                      bool blocked);
  std::vector<GameplayPacket> advance(std::int64_t now,
                                      const std::optional<WorldPosition> &position, bool blocked);

  // Closes any open interaction and refuses `request` after an unexpected failure, so the
  // caller can keep the connection instead of dropping the player for one bad object.
  std::vector<GameplayPacket> abort(const WorldGameplayRequest &request);

private:
  void enter_map(int map_id);
  std::vector<GameplayPacket> cancel();
  nlohmann::json rewards(int cms_id, const WorldProgress &progress);
  // Interaction starts and instant uses of a not yet activated object must meet its exported
  // interaction conditions. Activated objects replay their committed result without them.
  bool interaction_conditions_met(const WorldGameplayRequest &request,
                                  const WorldPosition &position,
                                  const WorldProgress &current) const;

  std::shared_ptr<const Catalog> catalog_;
  std::int64_t wire_unit_id_;
  ProgressAccess &progress_;
  TutorialSession::Commit commit_;
  tutorial::TutorialRewardPersistence items_;
  std::shared_ptr<const Progression> progression_;
  std::unique_ptr<LootRandom> random_;
  RequirementCheck requirement_check_;
  std::unique_ptr<TutorialSession> interaction_;
  std::optional<WorldPosition> origin_;
  std::map<int, nlohmann::json> rolled_rewards_;
};

} // namespace chronicles::adventure
