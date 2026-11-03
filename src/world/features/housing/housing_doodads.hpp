#pragma once

// Phase graphs of interactive housing doodads: garden plots and decorations that change state
// when used. Built by tools/build_housing_data.py into data/generated/housing/doodads.json from
// DoodadPhaseFlow, Interaction, InteractionCondition, InteractionAnim, ExpertiseReward and loot
// rows of client build 40367e24241c9481bfc07fbf9ef1919f25fb0eaa.
//
// Evidence:
// - The client starts these interactions like doodad gathering: StartInteractionAnimReq with
//   param.interaction.interaction_id and the housing doodad unit ID, then
//   ExecuteInteractionReq at the montage's execute event.
// - AAC_PS2C_ChangeDoodadPhaseNoti_Handle (0x1461D3910) applies ChangeDoodadPhaseNoti to a
//   placed housing doodad named by its housing doodad unit ID
//   (dumps/ida_research/housing-furniture-20261030).

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace chronicles::housing {

// InteractionCondition rows on the caster: ConsumeItem spends the items when the interaction
// executes (seeds); OwnItem only requires them (quest trophies on decoration 161124).
struct DoodadCondition {
  enum class Kind { consume, own };

  Kind kind{Kind::own};
  int item_cms_id{};
  int count{};
};

struct DoodadLootItem {
  int item_cms_id{};
  int min_quantity{};
  int max_quantity{};
  int weight{};
};

struct DoodadLootGroup {
  double rate_percent{};
  std::vector<DoodadLootItem> items;
};

// An ExpertiseReward or LootingPack task. Captured gathering doodads report LootingPack rewards
// with the looting reason naming the doodad and ExpertiseReward rewards with the system reason.
struct DoodadReward {
  double pack_rate_percent{};
  std::vector<DoodadLootGroup> groups;
  bool looting_reason{};
};

struct DoodadPhaseChoice {
  int phase_id{};
  int weight{};
};

// One DoodadPhaseFlow row. A phase may hold several interaction transitions (one per seed of a
// garden plot) and at most one timer transition.
struct DoodadTransition {
  enum class Trigger { interaction, timer };

  Trigger trigger{Trigger::timer};
  int interaction_id{};
  int animation_id{};
  std::vector<DoodadCondition> conditions;
  std::optional<DoodadReward> reward;
  std::int64_t duration_ms{};
  std::vector<DoodadPhaseChoice> next;
};

struct HousingDoodadDefinition {
  int doodad_id{};
  std::map<int, std::vector<DoodadTransition>> phases;
};

struct DoodadAnimation {
  int id{};
  int sp_cost{};
  // CMS EX5EquipmentType name, or "None".
  std::string tool_equipment_type;
  int execute_time_ms{};
  int duration_ms{};
};

// Immutable after load; safe to share between threads.
class HousingDoodadProfile {
public:
  static std::shared_ptr<const HousingDoodadProfile> load(const std::filesystem::path &path);

  // Throws std::runtime_error naming the first invalid entry.
  explicit HousingDoodadProfile(const nlohmann::json &document);

  const HousingDoodadDefinition *doodad(int doodad_cms_id) const;

  const DoodadAnimation *animation(int animation_id) const;

  // The transition `interaction_id` triggers in `phase_id`; null when that phase has none.
  const DoodadTransition *interaction(int doodad_cms_id, int phase_id, int interaction_id) const;

  // The timer of `phase_id`; null when the phase waits for an interaction only.
  const DoodadTransition *timer(int doodad_cms_id, int phase_id) const;

  // GameVariable SPStatRegenDelayMilli: SP regeneration resumes this long after the animation.
  int sp_regen_delay_ms() const;

  std::size_t doodad_count() const;

  std::size_t animation_count() const;

private:
  std::unordered_map<int, HousingDoodadDefinition> doodads_;
  std::unordered_map<int, DoodadAnimation> animations_;
  int sp_regen_delay_ms_{};
};

// Picks a next phase by weight. `choices` must not be empty.
int choose_doodad_phase(const std::vector<DoodadPhaseChoice> &choices, std::mt19937 &random);

// Rolls a reward like gathering: the pack passes its rate, then each passing group grants one
// item chosen by weight with a uniform quantity. Returns item -> quantity.
std::map<int, int> roll_doodad_reward(const DoodadReward &reward, std::mt19937 &random);

} // namespace chronicles::housing
