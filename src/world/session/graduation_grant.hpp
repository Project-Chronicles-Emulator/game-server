#pragma once

#include "world/features/character/graduation_grant.hpp"

namespace chronicles {

class AuthService;
class QuestCatalog;
struct Handoff;

// Composition inputs for the existing application entry point. The Character owner
// keeps the grant policy; this adapter joins its result-bearing Quest and GM services.
struct GraduationServices {
  std::shared_ptr<const ShopCatalog> shop_catalog;
  std::shared_ptr<const Progression> progression;
  std::shared_ptr<const QuestCatalog> quest_catalog;
  std::shared_ptr<const GraduationLoadout> loadout;
};

class CharacterEntryContext final : public character::GraduationGrantContext {
public:
  CharacterEntryContext(AuthService &auth, const Handoff &handoff, GraduationServices services);

  std::int64_t progression_character_id() const override;
  std::optional<Character> progression_character() override;
  WorldProgress progression_stored_progress() override;
  bool progression_save(const WorldProgress &before, const WorldProgress &after) override;
  bool graduation_game_master() const override;
  int graduation_complete_quests() override;
  ItemBundlePlan graduation_plan_items(const ShopCatalog &catalog,
                                       const std::vector<ItemBundleEntry> &items,
                                       std::optional<std::int64_t> gold_target,
                                       const Character &character, const WorldProgress &before,
                                       int maximum_instances) override;
  std::optional<std::vector<CharacterItem>>
  graduation_commit_items(const WorldProgress &before, const WorldProgress &after,
                          const ItemTrade &trade) override;

private:
  AuthService &auth_;
  const Handoff &handoff_;
  const GraduationServices services_;
};

GraduationResult apply_graduation_grant(AuthService &auth, const Handoff &handoff,
                                        const GraduationServices &services);

} // namespace chronicles
