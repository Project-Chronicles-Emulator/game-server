#include "world/features/quests/graduation_quests.hpp"

#include "data/shop_catalog.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace chronicles::quests {
namespace {

// Equipment the quest rules credit as stacks is dropped: the graduation kit grants real items.
// Storage never deletes a stored progress row, so a misfiled stack that already existed
// before this round keeps its row; deleting it rejected the whole graduation commit (live:
// "World progress records cannot be removed").
void drop_misfiled_equipment_stacks(WorldProgress &progress, const WorldProgress &before,
                                    const ShopCatalog &catalog) {
  std::erase_if(progress.stackables, [&catalog, &before](const auto &stack) {
    const auto *item = catalog.item(stack.first);
    return item && item->storage == ShopItemStorage::instance &&
           !before.stackables.contains(stack.first);
  });
  for (auto &[cms_id, quantity] : progress.stackables) {
    const auto *item = catalog.item(cms_id);
    const auto previous = before.stackables.find(cms_id);
    if (item && item->storage == ShopItemStorage::instance &&
        previous != before.stackables.end()) {
      quantity = previous->second;
    }
  }
}

} // namespace

int complete_graduation_quests(character::CharacterProgressStore &store,
                               const std::shared_ptr<const QuestCatalog> &catalog,
                               const std::shared_ptr<const Progression> &progression,
                               const ShopCatalog &items) {
  if (!catalog) {
    return 0;
  }

  // Only the quest rules matter here; the packets a quest book returns are dropped because the
  // client has not entered the world yet.
  const WorldQuestBook quest_book(catalog, store.progression_character_id(), progression);
  auto quest_ids = quest_book.catalog_quest_ids();
  int completed = 0;
  for (int round = 0; round < kMaximumQuestCompletionRounds && !quest_ids.empty(); ++round) {
    const auto character = store.progression_character();
    const auto current = store.progression_stored_progress();
    if (!character || current.revision == std::numeric_limits<std::int64_t>::max()) {
      break;
    }

    // No NPC, range or trigger applies: this is a grant, not a turn-in.
    QuestWorldView view;
    view.position = character->world_position.value_or(WorldPosition{});
    view.character = &*character;
    view.find_unit = [](std::uint64_t) -> const AnnouncedUnit * {
      return nullptr;
    };

    auto completion = quest_book.gm_complete(quest_ids, current, view);
    if (!completion.result.next || completion.completed == 0) {
      break;
    }

    auto next = std::move(*completion.result.next);
    ++next.revision;
    drop_misfiled_equipment_stacks(next, current, items);
    if (!store.progression_save(current, next)) {
      spdlog::warn("Graduation quests for character {} conflicted with another change",
                   store.progression_character_id());
      break;
    }

    completed += completion.completed;
    quest_ids = WorldQuestBook::active_quest_ids(next);
  }
  return completed;
}

} // namespace chronicles::quests
