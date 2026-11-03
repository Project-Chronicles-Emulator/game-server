#pragma once

#include "world/features/character/character_progress_store.hpp"
#include "world/features/quests/world_quests.hpp"

namespace chronicles {

class ShopCatalog;

namespace quests {

inline constexpr int kMaximumQuestCompletionRounds = 50;

int complete_graduation_quests(character::CharacterProgressStore &store,
                               const std::shared_ptr<const QuestCatalog> &catalog,
                               const std::shared_ptr<const Progression> &progression,
                               const ShopCatalog &items);

} // namespace quests
} // namespace chronicles
