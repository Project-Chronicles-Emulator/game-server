#pragma once

#include "persistence/character_repository.hpp"
#include "persistence/character_state_types.hpp"
#include "protocol/gameplay_packet.hpp"
#include "world/core/quest_events.hpp"
#include "world/core/world_request_opcodes.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace chronicles {

// Recovered world opcodes used by crafting stations (build 40367e24...). Processing
// (Food, Wildcraft, Cloth, Refining, Smelting) and campfire cooking share one flow:
// DialogueStartReq on the station, then StartInteractionAnimReq with
// param.interaction.execute_craft, ExecuteInteractionReq and EndInteractionAnimReq.
inline constexpr std::uint16_t kProcessingDialogueStartRequest = 20065;
inline constexpr std::uint16_t kProcessingDialogueEndRequest = 20066;
inline constexpr std::uint16_t kProcessingStartInteractionRequest =
    world_request::kInteractionStartRequest;
inline constexpr std::uint16_t kProcessingExecuteInteractionRequest = 20070;
inline constexpr std::uint16_t kProcessingEndInteractionRequest = 20071;
inline constexpr std::uint16_t kProcessingCraftResultNotification = 147;

// World-entry crafting snapshots. Every official entry sends both, ExpertiseSkills before
// WorldCharItemsNoti and ItemCrafts after it.
inline constexpr std::uint16_t kWorldCharExpertiseSkillsNotification = 128;
inline constexpr std::uint16_t kWorldCharItemCraftsNotification = 132;

// WorldCharExpertiseSkillsNoti payload with every EX5ExpertiseType at Lv1, as the official
// server sent for each captured character. The client opens a station's craft window only
// when the station's expertise has a non-zero level; otherwise it reports Craft_SkillNotLearn
// and ends the dialogue. Expertise experience and level-ups are not persisted yet.
nlohmann::json expertise_skills_entry_snapshot();

struct ProcessingMaterial {
  int item_cms_id{};
  int count{};
};

// Where a recipe's product lives in emulator storage.
enum class ProcessingProductStorage { stackable, instance, housing };

// One supported ItemCraft row. Costs and counts are per single craft.
struct ProcessingRecipe {
  int id{};
  bool requires_learning{};
  int product_item_cms_id{};
  int product_count{};
  ProcessingProductStorage product_storage{ProcessingProductStorage::stackable};
  // Only meaningful for instance products (Item.maxDurability and EX5ItemGrade).
  int product_max_durability{};
  int product_grade{};
  std::int64_t gold_cost{};
  std::vector<ProcessingMaterial> materials;
};

// A Craft interaction row that a station's interaction group offers.
struct ProcessingInteraction {
  int id{};
  int group_id{};
  int animation_id{};
};

struct ProcessingAnimation {
  int id{};
  int execute_time_ms{};
  int duration_ms{};
};

// A crafting doodad definition: its Craft interaction group and ItemCraftPack recipes.
struct ProcessingStation {
  int doodad_cms_id{};
  int interaction_group_id{};
  std::set<int> recipe_ids;
};

// How close a character must stand to a map station (the tutorial interaction allowance;
// captured crafts started 1.25-1.27 m from the station origin).
inline constexpr int kProcessingStationRangeMm = 7000;

// One placed station. unit_id is the ID the client targets: the spot unit ID of a map
// station, or the housing doodad unit ID of a station placed on a housing lot.
struct ProcessingPlacement {
  std::uint64_t unit_id{};
  int doodad_cms_id{};
  int x_mm{};
  int y_mm{};
  int z_mm{};
  // Largest accepted distance between the character and (x_mm, y_mm, z_mm).
  int range_mm{kProcessingStationRangeMm};
};

// Resolves a station placed at runtime, such as a crafting station on a housing lot, that the
// character may use while on map_id. Returns nothing for any other unit ID.
using ProcessingStationLocator =
    std::function<std::optional<ProcessingPlacement>(std::uint64_t unit_id, int map_id)>;

struct ProcessingMapArea {
  int map_id{};
  std::array<int, 3> min_mm{};
  std::array<int, 3> max_mm{};
};

// Immutable recipe, station and placement indexes built by tools/build_processing_data.py.
// Load and validate once at startup; lookups never parse JSON.
class ProcessingData {
public:
  static std::shared_ptr<const ProcessingData> load(const std::filesystem::path &path);

  explicit ProcessingData(const nlohmann::json &document);

  const ProcessingPlacement *placement(std::uint64_t unit_id) const;

  // True when the placement lies inside the authored area of map_id.
  bool map_contains(int map_id, const ProcessingPlacement &placement) const;

  const ProcessingStation *station(int doodad_cms_id) const;

  const ProcessingInteraction *interaction(int interaction_id) const;

  const ProcessingAnimation *animation(int animation_id) const;

  const ProcessingRecipe *recipe(int recipe_id) const;

  // ItemCraft rows the emulator cannot safely execute (unsupported costs
  // or company-position requirements). Housing products use their existing persistent store.
  bool is_unsupported_recipe(int recipe_id) const;

  // GameVariable.MaxNumOfCraftAmount: the largest count accepted in one request.
  int maximum_craft_amount() const;

  std::size_t placement_count() const;

  std::size_t recipe_count() const;

private:
  std::vector<ProcessingMapArea> maps_;
  std::unordered_map<std::uint64_t, ProcessingPlacement> placements_;
  std::unordered_map<int, ProcessingStation> stations_;
  std::unordered_map<int, ProcessingInteraction> interactions_;
  std::unordered_map<int, ProcessingAnimation> animations_;
  std::unordered_map<int, ProcessingRecipe> recipes_;
  std::unordered_set<int> unsupported_recipes_;
  int maximum_craft_amount_{};
};

// Storage callbacks for the authenticated character. They run on the calling session's
// worker strand and must not hold the shared protocol codec lock.
struct ProcessingPersistence {
  std::function<std::optional<Character>()> load_character;
  std::function<WorldProgress()> load_progress;
  // Commits the revisioned progress transition and new item instances atomically. Returns
  // the granted items with storage IDs, or nullopt when nothing was written.
  std::function<std::optional<std::vector<CharacterItem>>(
      const WorldProgress &, const WorldProgress &, const std::vector<CharacterItem> &)>
      commit;
};

// One character's crafting-station actions. Calls are serialized by the world session.
// Materials, gold and products change together at the server-side execute deadline,
// or nothing changes.
class ProcessingSession {
public:
  // placed_stations is optional; without it only the profile's map stations are served.
  ProcessingSession(std::shared_ptr<const ProcessingData> data, std::uint64_t player_unit_id,
                    ProcessingPersistence persistence,
                    ProcessingStationLocator placed_stations = {});

  // True for station dialogue starts, craft starts, and dialogue end, execute or end
  // requests that belong to this session's open station dialogue or craft.
  bool handles(std::uint16_t opcode, const nlohmann::json &request,
               const std::optional<WorldPosition> &position) const;

  // other_action_busy refuses a new craft while another session service runs an action.
  std::vector<GameplayPacket> receive(std::uint16_t opcode, const nlohmann::json &request,
                                      std::uint32_t sequence, const WorldPosition &position,
                                      std::int64_t server_ticks, bool other_action_busy);

  // True while a craft is started but not yet executed.
  bool busy() const;

  // Crafts committed since the last call, for quest objectives.
  QuestEvents take_quest_events();

private:
  struct PendingCraft {
    std::uint64_t station_unit_id{};
    int recipe_id{};
    int count{};
    int animation_id{};
    std::int64_t started_ticks{};
    bool executed{};
  };

  std::vector<GameplayPacket> open_station(const nlohmann::json &request,
                                           const WorldPosition &position);

  std::vector<GameplayPacket> close_station();

  std::vector<GameplayPacket> start(const nlohmann::json &request, const WorldPosition &position,
                                    std::int64_t server_ticks, bool other_action_busy);

  std::vector<GameplayPacket> execute(const nlohmann::json &request,
                                      const WorldPosition &position, std::int64_t server_ticks);

  std::vector<GameplayPacket> end(const nlohmann::json &request);

  // A profile placement inside map_id's area, else a runtime placement whose doodad is a
  // profiled station.
  std::optional<ProcessingPlacement> station_on_map(std::uint64_t unit_id, int map_id) const;

  bool pending_expired(std::int64_t server_ticks) const;

  bool within_request_limit(std::int64_t server_ticks);

  std::shared_ptr<const ProcessingData> data_;
  std::uint64_t player_unit_id_{};
  ProcessingPersistence persistence_;
  ProcessingStationLocator placed_stations_;
  // Station whose dialogue is open; crafts are accepted only at this station.
  std::optional<std::uint64_t> dialogue_station_;
  std::optional<PendingCraft> pending_;
  QuestEvents quest_events_;
  std::int64_t request_window_ticks_{};
  unsigned requests_in_window_{};
};

} // namespace chronicles
