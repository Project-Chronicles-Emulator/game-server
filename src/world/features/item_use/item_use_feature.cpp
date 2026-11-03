#include "world/features/item_use/item_use_feature.hpp"

#include "data/catalog_requirements.hpp"

#include <spdlog/spdlog.h>

#include <iterator>
#include <random>
#include <utility>

namespace chronicles::item_use {
namespace {

std::shared_ptr<const ItemUseCatalog> load_catalog(ClientCatalog *catalog) {
  if (!catalog || !has_catalog_tables(*catalog, ItemUseCatalog::required_tables(), "Item use")) {
    return {};
  }
  auto item_use = ItemUseCatalog::load(*catalog);
  spdlog::info("Item use catalog loaded: {} draw-box items", item_use->draw_box_count());
  return item_use;
}

} // namespace

ItemUseFeature::ItemUseFeature(ClientCatalog *catalog, std::shared_ptr<const ShopCatalog> items)
    : ItemUseFeature(load_catalog(catalog), std::move(items)) {
}

ItemUseFeature::ItemUseFeature(std::shared_ptr<const ItemUseCatalog> catalog,
                               std::shared_ptr<const ShopCatalog> items)
    : catalog_(std::move(catalog)), items_(std::move(items)) {
}

bool ItemUseFeature::enabled() const noexcept {
  return catalog_ && items_;
}

std::unique_ptr<ItemUseSession>
ItemUseFeature::create_session(std::shared_ptr<ProgressAccess> progress,
                               ItemUseSessionContext &context, SessionEventBus &events,
                               std::unique_ptr<LootRandom> random) {
  if (!enabled()) {
    return {};
  }

  ItemUsePersistence persistence;
  persistence.load_character = [progress] {
    return progress->character();
  };
  persistence.load_progress = [progress, &context] {
    if (const auto *cached = context.item_use_tutorial_progress()) {
      return *cached;
    }
    return progress->load();
  };
  persistence.commit = make_item_grant_commit(
      progress,
      ItemGrantCacheAccess{[&context] {
                             return context.item_use_tutorial_progress() != nullptr;
                           },
                           [&context] {
                             return context.item_use_player_stats();
                           }},
      events);

  ItemUseServices services{catalog_, items_, {}, {}};
  services.quest_use_item = [&context](int item_cms_id) {
    return context.item_use_is_quest_item(item_cms_id);
  };
  if (context.item_use_has_combat()) {
    // Install the same capabilities as before, resolving the current combat each time.
    // Map changes can rebuild combat; retaining its creation-time pointer is unsafe.
    services.effects.effect_list = [&context](int item_cms_id) {
      return context.item_use_effect_list(item_cms_id);
    };
    services.effects.ready = [&context] {
      return context.item_use_effect_ready();
    };
    services.effects.apply = [&context](const std::vector<int> &effect_ids) {
      return context.item_use_apply_effects(effect_ids);
    };
  }

  if (!random) {
    std::random_device entropy;
    random = std::make_unique<SeededLootRandom>(entropy());
  }
  return std::make_unique<ItemUseSession>(std::move(services), std::move(persistence),
                                          std::move(random));
}

bool ItemUseFeature::handle(ItemUseSession *session, FrameContext &frame,
                            ItemUseSessionContext &context) {
  const auto *request = frame.request();
  if (!request || request->opcode != kItemUseRequestOpcode || !session ||
      !session->handles(request->message)) {
    return false;
  }

  auto used = session->receive(request->message, request->sequence);
  frame.session_packets.insert(frame.session_packets.end(), std::make_move_iterator(used.begin()),
                               std::make_move_iterator(used.end()));
  context.item_use_record_quest_events(session->take_quest_events(), frame);
  frame.action_deadline = context.item_use_next_gameplay_tick();
  return true;
}

} // namespace chronicles::item_use
