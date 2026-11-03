#pragma once

#include "world/core/frame_context.hpp"
#include "world/core/item_grant_commit.hpp"
#include "world/features/item_use/item_use.hpp"

namespace chronicles::item_use {

class ItemUseSessionContext {
public:
  virtual ~ItemUseSessionContext() = default;

  virtual const WorldProgress *item_use_tutorial_progress() const = 0;
  virtual const nlohmann::json *item_use_player_stats() const = 0;
  virtual bool item_use_has_combat() const = 0;
  virtual const std::vector<int> *item_use_effect_list(int item_cms_id) const = 0;
  virtual bool item_use_effect_ready() const = 0;
  virtual std::vector<GameplayPacket>
  item_use_apply_effects(const std::vector<int> &effect_ids) = 0;
  virtual std::optional<std::int64_t> item_use_next_gameplay_tick() const = 0;

  // True for items a quest objective asks the player to use (CounterUseItem,
  // UseItemInTriggerVolume). Realms without a quest catalog have none.
  virtual bool item_use_is_quest_item(int item_cms_id) const {
    static_cast<void>(item_cms_id);
    return false;
  }

  // Hands committed item uses to the quest service; its packets follow the use's own reply.
  virtual void item_use_record_quest_events(QuestEvents events, FrameContext &frame) {
    static_cast<void>(events);
    static_cast<void>(frame);
  }
};

// The existing ItemUseSession remains the player action owner. This server feature owns
// catalog startup and its real persistence/effect/dispatch bindings, without another shell.
class ItemUseFeature final {
public:
  ItemUseFeature(ClientCatalog *catalog, std::shared_ptr<const ShopCatalog> items);
  ItemUseFeature(std::shared_ptr<const ItemUseCatalog> catalog,
                 std::shared_ptr<const ShopCatalog> items);

  bool enabled() const noexcept;
  std::unique_ptr<ItemUseSession> create_session(std::shared_ptr<ProgressAccess> progress,
                                                 ItemUseSessionContext &context,
                                                 SessionEventBus &events,
                                                 std::unique_ptr<LootRandom> random = {});
  bool handle(ItemUseSession *session, FrameContext &frame, ItemUseSessionContext &context);

private:
  const std::shared_ptr<const ItemUseCatalog> catalog_;
  const std::shared_ptr<const ShopCatalog> items_;
};

} // namespace chronicles::item_use
