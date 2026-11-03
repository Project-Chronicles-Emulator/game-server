#pragma once

#include <cstdint>
#include <vector>

namespace chronicles {

// Server-established player actions that can advance quest objectives. A producer records an
// event only after its own storage commit succeeded; the quest service then credits matching
// active objectives in a separate revisioned commit.
enum class QuestEventKind : std::uint8_t {
  // A crafting-station recipe completed (QuestProgress CounterSuccessCrafting and, because a
  // success is also an attempt, CounterTryCrafting).
  CraftSucceeded,
  // An interaction ran on a doodad in a given phase (QuestProgress InteractDoodad and
  // EtherealInteractDoodad).
  DoodadInteracted,
  // An item use committed (CounterUseItem; UseItemInTriggerVolume with the player position).
  ItemUsed,
  // Trade goods were sold at a trading post (TradePayment). `target_id` is the TradeGood id,
  // `context_id` the TradingPost id.
  TradeGoodSold,
  // Server combat accepted a skill the player started (CounterUseSkill).
  SkillUsed,
  // Server combat executed a skill effect of the player's skill (CounterUseSkillEffect).
  SkillEffectExecuted,
  // A status effect was applied to the player (AddStatusEffect).
  StatusEffectApplied,
  // The player destroyed a destructible object (DestroyObject); `target_id` is its
  // DestructibleObject id.
  ObjectDestroyed,
};

struct QuestEvent {
  QuestEventKind kind{QuestEventKind::CraftSucceeded};
  // ItemCraft.craftId for crafts, Doodad.id for doodad interactions, Item.id for item uses,
  // TradeGood.id, Skill.id, SkillEffect.id, StatusEffect.id or DestructibleObject.id.
  int target_id{};
  // Doodad phase the interaction ran in; unused for the other kinds.
  int phase_id{};
  // Completed actions this event stands for (the craft request's count, goods sold).
  int count{1};
  // TradingPost.id of a trade-good sale; zero for the other kinds.
  int context_id{};

  bool operator==(const QuestEvent &) const = default;
};

using QuestEvents = std::vector<QuestEvent>;

} // namespace chronicles
