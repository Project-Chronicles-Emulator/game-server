#pragma once

// Session-scoped access to one character's committed world progress.
//
// Gameplay modules plan a transition from a loaded snapshot and commit it atomically; a
// revision, ownership or inventory-slot conflict leaves storage unchanged. The interface keeps
// those modules independent of AuthService so they can be tested against the narrow character
// repositories.

#include "auth/auth.hpp"
#include "persistence/character_repository.hpp"
#include "persistence/character_state_repository.hpp"

#include <optional>
#include <stdexcept>
#include <vector>

namespace chronicles {

class ProgressAccess {
public:
  virtual ~ProgressAccess() = default;

  virtual WorldProgress load() = 0;

  virtual std::optional<Character> character() = 0;

  // Commits progress and item changes together. Granted items receive storage ids in the
  // returned copy; nullopt means nothing was written.
  virtual std::optional<std::vector<CharacterItem>>
  commit(const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade) = 0;

  // Repeating the same immutable receipt and transition is safe after an unknown COMMIT.
  // A different fingerprint for an existing receipt is rejected by the repository.
  virtual LootClaimResult commit_loot_claim(const std::string &, const std::string &,
                                            const WorldProgress &, const WorldProgress &,
                                            const ItemTrade &) {
    throw std::logic_error("Durable loot claim commit is unavailable");
  }

  virtual LootClaimResult commit_gathering_quest_claim(
      const std::string &, const std::string &, const WorldProgress &, const WorldProgress &,
      const ItemTrade &, const QuestServiceEffects &,
      const std::optional<SharedGatheringSourceTransition> &) {
    throw std::logic_error("Durable gathering commit is unavailable");
  }

  virtual GatheringFinalizeResult finalize_gathering_quest_claim(
      const std::string &, const std::string &, const WorldProgress &, const WorldProgress &,
      const ItemTrade &, const QuestServiceEffects &,
      const std::optional<SharedGatheringSourceTransition> &, std::int64_t) {
    throw std::logic_error("Durable gathering finalization is unavailable");
  }

  // Implementations that cannot atomically store both values must fail closed.
  virtual bool commit_with_position(const WorldProgress &, const WorldProgress &,
                                    const WorldPosition &) {
    throw std::logic_error("Atomic world position commit is unavailable");
  }
};

// ProgressAccess over the character repositories, for one owned character.
class RepositoryProgressAccess final : public ProgressAccess {
public:
  RepositoryProgressAccess(CharacterRepository &characters,
                           CharacterStateRepository &character_state,
                           std::int64_t account_id, std::int64_t character_id, int world_id);

  WorldProgress load() override;

  std::optional<Character> character() override;

  std::optional<std::vector<CharacterItem>> commit(const WorldProgress &before,
                                                   const WorldProgress &after,
                                                   const ItemTrade &trade) override;
  LootClaimResult commit_loot_claim(const std::string &receipt_id, const std::string &fingerprint,
                                    const WorldProgress &before, const WorldProgress &after,
                                    const ItemTrade &trade) override;
  LootClaimResult commit_gathering_quest_claim(
      const std::string &receipt_id, const std::string &fingerprint,
      const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade,
      const QuestServiceEffects &effects,
      const std::optional<SharedGatheringSourceTransition> &source) override;
  GatheringFinalizeResult finalize_gathering_quest_claim(
      const std::string &receipt_id, const std::string &fingerprint,
      const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade,
      const QuestServiceEffects &effects,
      const std::optional<SharedGatheringSourceTransition> &source,
      std::int64_t confirmation_epoch_ticks) override;
  bool commit_with_position(const WorldProgress &before, const WorldProgress &after,
                            const WorldPosition &position) override;

private:
  CharacterRepository &characters_;
  CharacterStateRepository &character_state_;
  std::int64_t account_id_{};
  std::int64_t character_id_{};
  int world_id_{};
};

// ProgressAccess over the world-entry handoff of an authenticated session.
class HandoffProgressAccess final : public ProgressAccess {
public:
  HandoffProgressAccess(AuthService &auth, Handoff handoff);

  WorldProgress load() override;

  std::optional<Character> character() override;

  std::optional<std::vector<CharacterItem>> commit(const WorldProgress &before,
                                                   const WorldProgress &after,
                                                   const ItemTrade &trade) override;
  LootClaimResult commit_loot_claim(const std::string &receipt_id, const std::string &fingerprint,
                                    const WorldProgress &before, const WorldProgress &after,
                                    const ItemTrade &trade) override;
  LootClaimResult commit_gathering_quest_claim(
      const std::string &receipt_id, const std::string &fingerprint,
      const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade,
      const QuestServiceEffects &effects,
      const std::optional<SharedGatheringSourceTransition> &source) override;
  GatheringFinalizeResult finalize_gathering_quest_claim(
      const std::string &receipt_id, const std::string &fingerprint,
      const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade,
      const QuestServiceEffects &effects,
      const std::optional<SharedGatheringSourceTransition> &source,
      std::int64_t confirmation_epoch_ticks) override;
  bool commit_with_position(const WorldProgress &before, const WorldProgress &after,
                            const WorldPosition &position) override;

private:
  AuthService &auth_;
  Handoff handoff_;
};

} // namespace chronicles
