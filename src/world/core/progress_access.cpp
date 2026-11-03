#include "world/core/progress_access.hpp"

#include <algorithm>

namespace chronicles {

RepositoryProgressAccess::RepositoryProgressAccess(CharacterRepository &characters,
                                                   CharacterStateRepository &character_state,
                                                   std::int64_t account_id,
                                                   std::int64_t character_id, int world_id)
    : characters_(characters), character_state_(character_state), account_id_(account_id),
      character_id_(character_id), world_id_(world_id) {}

WorldProgress RepositoryProgressAccess::load() {
  return character_state_.load_world_progress(account_id_, character_id_, world_id_);
}

std::optional<Character> RepositoryProgressAccess::character() {
  const auto characters = characters_.characters(account_id_, world_id_);
  const auto owned = std::ranges::find_if(characters, [this](const Character &character) {
    return character.id == character_id_;
  });
  if (owned == characters.end()) {
    return std::nullopt;
  }
  return *owned;
}

std::optional<std::vector<CharacterItem>>
RepositoryProgressAccess::commit(const WorldProgress &before, const WorldProgress &after,
                                 const ItemTrade &trade) {
  return character_state_.commit_item_trade(account_id_, character_id_, world_id_, before,
                                            after, trade);
}

LootClaimResult RepositoryProgressAccess::commit_loot_claim(
    const std::string &receipt_id, const std::string &fingerprint, const WorldProgress &before,
    const WorldProgress &after, const ItemTrade &trade) {
  return character_state_.commit_loot_claim(account_id_, character_id_, world_id_, receipt_id,
                                             fingerprint, before, after, trade);
}

LootClaimResult RepositoryProgressAccess::commit_gathering_quest_claim(
    const std::string &receipt_id, const std::string &fingerprint,
    const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade,
    const QuestServiceEffects &effects,
    const std::optional<SharedGatheringSourceTransition> &source) {
  return character_state_.commit_gathering_quest_claim(account_id_, character_id_, world_id_,
                                                       receipt_id, fingerprint, before, after,
                                                       trade, effects, source);
}

GatheringFinalizeResult RepositoryProgressAccess::finalize_gathering_quest_claim(
    const std::string &receipt_id, const std::string &fingerprint,
    const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade,
    const QuestServiceEffects &effects,
    const std::optional<SharedGatheringSourceTransition> &source,
    std::int64_t confirmation_epoch_ticks) {
  return character_state_.finalize_gathering_quest_claim(
      account_id_, character_id_, world_id_, receipt_id, fingerprint, before, after, trade,
      effects, source, confirmation_epoch_ticks);
}

bool RepositoryProgressAccess::commit_with_position(const WorldProgress &before,
                                                     const WorldProgress &after,
                                                     const WorldPosition &position) {
  return character_state_.commit_world_progress_and_position(
      account_id_, character_id_, world_id_, before, after, position);
}

HandoffProgressAccess::HandoffProgressAccess(AuthService &auth, Handoff handoff)
    : auth_(auth), handoff_(std::move(handoff)) {}

WorldProgress HandoffProgressAccess::load() {
  return auth_.world_progress(handoff_);
}

std::optional<Character> HandoffProgressAccess::character() {
  return auth_.world_character(handoff_);
}

std::optional<std::vector<CharacterItem>>
HandoffProgressAccess::commit(const WorldProgress &before, const WorldProgress &after,
                              const ItemTrade &trade) {
  return auth_.commit_world_item_trade(handoff_, before, after, trade);
}

LootClaimResult HandoffProgressAccess::commit_loot_claim(
    const std::string &receipt_id, const std::string &fingerprint, const WorldProgress &before,
    const WorldProgress &after, const ItemTrade &trade) {
  return auth_.commit_world_loot_claim(handoff_, receipt_id, fingerprint, before, after, trade);
}

LootClaimResult HandoffProgressAccess::commit_gathering_quest_claim(
    const std::string &receipt_id, const std::string &fingerprint,
    const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade,
    const QuestServiceEffects &effects,
    const std::optional<SharedGatheringSourceTransition> &source) {
  return auth_.commit_gathering_quest_claim(handoff_, receipt_id, fingerprint, before, after,
                                             trade, effects, source);
}

GatheringFinalizeResult HandoffProgressAccess::finalize_gathering_quest_claim(
    const std::string &receipt_id, const std::string &fingerprint,
    const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade,
    const QuestServiceEffects &effects,
    const std::optional<SharedGatheringSourceTransition> &source,
    std::int64_t confirmation_epoch_ticks) {
  return auth_.finalize_gathering_quest_claim(handoff_, receipt_id, fingerprint, before, after,
                                               trade, effects, source, confirmation_epoch_ticks);
}

bool HandoffProgressAccess::commit_with_position(const WorldProgress &before,
                                                  const WorldProgress &after,
                                                  const WorldPosition &position) {
  return auth_.commit_world_progress_and_position(handoff_, before, after, position);
}

} // namespace chronicles
