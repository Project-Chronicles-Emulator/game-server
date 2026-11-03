#include "world/session/graduation_grant.hpp"

#include "auth/auth.hpp"
#include "world/features/gm/gm_commands.hpp"
#include "world/features/quests/graduation_quests.hpp"

#include <utility>

namespace chronicles {

CharacterEntryContext::CharacterEntryContext(AuthService &auth, const Handoff &handoff,
                                             GraduationServices services)
    : auth_(auth), handoff_(handoff), services_(std::move(services)) {
}

std::int64_t CharacterEntryContext::progression_character_id() const {
  return handoff_.character_id;
}

std::optional<Character> CharacterEntryContext::progression_character() {
  return auth_.world_character(handoff_);
}

WorldProgress CharacterEntryContext::progression_stored_progress() {
  return auth_.world_progress(handoff_);
}

bool CharacterEntryContext::progression_save(const WorldProgress &before,
                                             const WorldProgress &after) {
  return auth_.save_world_progress(handoff_, before, after);
}

bool CharacterEntryContext::graduation_game_master() const {
  return handoff_.game_master;
}

int CharacterEntryContext::graduation_complete_quests() {
  return quests::complete_graduation_quests(*this, services_.quest_catalog, services_.progression,
                                            *services_.shop_catalog);
}

ItemBundlePlan CharacterEntryContext::graduation_plan_items(
    const ShopCatalog &catalog, const std::vector<ItemBundleEntry> &items,
    std::optional<std::int64_t> gold_target, const Character &character,
    const WorldProgress &before, int maximum_instances) {
  return plan_gm_grant_bundle(catalog, items, gold_target, character, before, maximum_instances);
}

std::optional<std::vector<CharacterItem>>
CharacterEntryContext::graduation_commit_items(const WorldProgress &before,
                                               const WorldProgress &after, const ItemTrade &trade) {
  return auth_.commit_world_item_trade(handoff_, before, after, trade);
}

GraduationResult apply_graduation_grant(AuthService &auth, const Handoff &handoff,
                                        const GraduationServices &services) {
  CharacterEntryContext context(auth, handoff, services);
  return character::apply_graduation_grant(
      context, {services.shop_catalog, services.progression, services.loadout});
}

} // namespace chronicles
