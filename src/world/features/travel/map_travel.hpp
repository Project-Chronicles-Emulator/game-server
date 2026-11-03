#pragma once

// Instances, dungeons, portals and map-move cutscenes of the shared world.
//
// Evidence (build 40367e24, static analysis saved in the IDA database):
// - Walking into a channelSession, channel or portal level-trigger volume makes the client
//   check the current map's exit condition and the destination's entrance condition
//   (MapCondition), send PC2S_LevelTriggerReq (20084) with the volume GUID and start a
//   pending map transition that only a server map move (MapMoveStartNoti 175) clears.
// - Map travel requested from UI flows (AAC_MapMove_Candidate_RequestMapTravel) runs the same
//   checks and sends PC2S_ChannelTriggerEntryReq (20082) {map_id}; map_id 0 asks for the
//   current map's parent.
// - MapMoveStartNoti carries cut_scene_id; MapMoveCutScene names it per (from, to) map pair.
// - LevelTriggerVolume rows attach cutscene, map move, doodad phase and client dialogue tasks
//   with their own conditions to a volume.
//
// The profile comes from tools/build_map_travel.py. Arrival positions are not in the client
// export (channelSession spawnPositionId and Portal.spawnId have no table). CUSTOM: a travel
// keeps the character's position when the destination's authored area contains it (every
// field dungeon entrance lies inside its dungeon's area, and instances share ArcheWorld
// coordinates), else it uses the destination level's origin. Same-map dungeon portals
// (GoBoss/GoExit) use the destination of the dungeon's own boss or exit doodad interaction
// (Candidate). Cutscene end points use the captured position, the doodad named by the end point
// id (Candidate), or the same arrival rule.

#include "persistence/character_state_types.hpp"
#include "protocol/gameplay_packet.hpp"
#include "world/core/world_gameplay_request.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace chronicles::travel {

inline constexpr std::uint16_t kLevelTriggerRequest = 20084;
inline constexpr std::uint16_t kCutSceneEndRequest = 20055;
inline constexpr std::uint16_t kVisibleChannelNotification = 67;
inline constexpr std::uint16_t kCutScenePlayNotification = 70;
inline constexpr std::uint16_t kDoodadPhaseChangedNotification = 194;

// x5 party state as MapCondition PartyState rows test it.
enum class PartyRole { none, leader, member };

// What the character's session can answer about itself for map conditions.
struct TravelContext {
  // Quest, OwnItem, AdventureObject, CheckUnitLevel and PlayRecord rows (the quest
  // condition evaluator). Without it those rows fail closed.
  std::function<bool(const nlohmann::json &row)> evaluate;
  PartyRole party_role{PartyRole::none};
};

struct MapInfo {
  int map_id{};
  std::string type;
  std::string world;
  int parent_map_id{};
  int origin_map_id{};
  std::optional<std::array<double, 3>> area_min_cm;
  std::optional<std::array<double, 3>> area_max_cm;
  std::optional<std::array<double, 3>> translation_cm;
  nlohmann::json entrance_conditions;
  nlohmann::json exit_conditions;
};

// A doodad phase a scripted volume changes (DoodadPhaseChangedNoti).
struct DoodadPhaseChange {
  std::string unit_id;
  int phase{};
};

// The server's answer to one travel request.
struct TravelPlan {
  // Set when the request reached a travel volume or rule; false lets other handlers see it.
  bool handled{};
  // A map move to perform, with the MapMoveStartNoti cutscene (0 for none).
  std::optional<WorldPosition> destination;
  int cut_scene_id{};
  // Cutscenes to play where the character stands (CutScenePlayNoti).
  std::vector<int> cut_scenes;
  // Doodad phases set by scripted volumes. The world records them in the map's (or instance
  // copy's) doodad state, so everyone there sees them and later arrivals receive them, and saves
  // them in the character's progress; TravelSession::play does not send them.
  std::vector<DoodadPhaseChange> doodad_phases;
  // Doodad phases shown to the acting character only (the captured Cliffs gate).
  std::vector<DoodadPhaseChange> actor_doodad_phases;
  // A captured cutscene gate moves the player only after its matching end request.
  std::optional<WorldPosition> cut_scene_destination;
  // The client started a pending transition for this request; a refusal must still send a
  // same-map handoff so the client releases movement.
  bool client_waits_for_move{};
  // Why nothing happened (log text only).
  std::string refusal;
};

// Where a cutscene's CutScene row ends: its map and, when recovered, the position.
struct CutSceneEndPoint {
  int map_id{};
  int end_point_id{};
  std::optional<WorldPosition> position;
};

class Catalog {
public:
  static std::shared_ptr<const Catalog> load(const std::filesystem::path &path);
  explicit Catalog(const nlohmann::json &document);

  const MapInfo *map(int map_id) const;
  std::size_t map_count() const noexcept;
  std::size_t volume_count() const noexcept;

  // Whether a character may be resumed on this map at login: shared-world fields, instances,
  // dungeons and points of interest that belong to the main world.
  bool resumable(int map_id) const;

  // The current map's exit conditions and the destination's entrance conditions hold.
  bool travel_allowed(int from_map_id, int to_map_id, const TravelContext &context) const;

  // Where a character standing at `current` arrives on its map's parent map, by the travel
  // arrival rule (housing areas and instances share their parent's coordinates). Nullopt for
  // maps without a known parent or without an arrival position.
  std::optional<WorldPosition> parent_arrival(const WorldPosition &current) const;

  // The field a housing area overlays (map 20101 -> 9012). Housing areas share their parent's
  // coordinates, so coordinates read inside one name a spot on that field. Nullopt for every
  // other map type and for housing areas without a parent.
  std::optional<int> housing_area_parent(int map_id) const;

  // PC2S_LevelTriggerReq: travel volumes and LevelTriggerVolume rules.
  TravelPlan level_trigger(const WorldPosition &current, const nlohmann::json &request,
                           const TravelContext &context) const;

  // PC2S_ChannelTriggerEntryReq {map_id}: a child instance whose entry volume or authored
  // area holds the character, or the parent map (map_id 0) when leaving an instance.
  TravelPlan channel_entry(const WorldPosition &current, const nlohmann::json &request,
                           const TravelContext &context) const;

  // The captured Cliffs entrance is a doodad interaction followed by a cutscene.
  TravelPlan cliffs_gate(const WorldPosition &current, const nlohmann::json &request,
                         const TravelContext &context) const;

  // MapMoveCutScene for a travel from one map to another; 0 when none applies.
  int map_move_cut_scene(int from_map_id, int to_map_id, const TravelContext &context) const;

  // Cutscenes the server announces (client-only cutscenes are played by the client itself).
  bool server_cut_scene(int cut_scene_id) const;
  bool invisible_cut_scene(int cut_scene_id) const;

  // Instance and dungeon maps: each party (or solo character) gets its own copy of them.
  std::set<int> copy_maps() const;

  // Wire unit ids of the doodads whose phase a scripted volume placed on `map_id` changes.
  std::vector<std::uint64_t> volume_doodads(int map_id) const;

  // The CutScene row's end point, when it names one.
  std::optional<CutSceneEndPoint> cut_scene_end_point(int cut_scene_id) const;

  // Where the character goes when a cutscene it watched at `current` ends: the end point's
  // recovered position, else the travel arrival rule on the end map. Nullopt when the cutscene
  // names no end point, when the end point would leave the character where it stands, when it
  // names a raid map (raid channels are admitted by the event service) or when no arrival
  // exists.
  std::optional<WorldPosition> cut_scene_end_destination(int cut_scene_id,
                                                         const WorldPosition &current) const;

private:
  struct Volume {
    std::array<std::string, 2> wire;
    std::string world;
    std::vector<int> maps;
    std::string kind;
    bool sphere{};
    std::array<double, 3> center_cm{};
    std::array<double, 3> extent_cm{};
    std::array<std::array<double, 3>, 3> axes{};
    double radius_cm{};
    int destination_map_id{};
    int portal_id{};
    nlohmann::json rules;

    bool contains(const WorldPosition &position, double tolerance_cm) const;
  };

  struct Portal {
    int portal_id{};
    int from_map_id{};
    int to_map_id{};
    // Same-map dungeon portals: the recovered arrival point (millimetres).
    std::optional<std::array<int, 3>> arrival_mm;
  };

  struct MapMoveCutScene {
    int from_map_id{};
    int to_map_id{};
    int cut_scene_id{};
    nlohmann::json conditions;
  };

  struct CutScene {
    bool invisible{};
    bool client_only{};
    std::optional<CutSceneEndPoint> end_point;
  };

  struct CutSceneGate {
    std::string unit_id;
    int interaction_id{};
    int source_map_id{};
    WorldPosition position;
    int required_quest_id{};
    int cut_scene_id{};
    WorldPosition destination;
  };

  bool conditions_pass(const nlohmann::json &conditions, int from_map_id, int to_map_id,
                       const TravelContext &context) const;
  bool volume_on_map(const Volume &volume, int map_id) const;
  bool area_contains(const MapInfo &map, const WorldPosition &position) const;
  std::optional<WorldPosition> arrival(const WorldPosition &current, int to_map_id) const;
  TravelPlan move_to(const WorldPosition &current, int to_map_id, const TravelContext &context,
                     bool client_waits) const;
  TravelPlan run_rules(const Volume &volume, const WorldPosition &current, bool leave,
                       const TravelContext &context) const;

  std::map<int, MapInfo> maps_;
  std::vector<Volume> volumes_;
  std::map<std::array<std::string, 2>, std::size_t> volumes_by_wire_;
  std::vector<Portal> portals_;
  std::vector<MapMoveCutScene> map_move_cut_scenes_;
  std::map<int, CutScene> cut_scenes_;
  std::optional<CutSceneGate> cliffs_gate_;
};

// Per-character cutscene state for travel cutscenes played in the shared world.
class TravelSession {
public:
  TravelSession(std::shared_ptr<const Catalog> catalog, std::uint64_t player_unit_id);

  const Catalog &catalog() const;
  bool cut_scene_active() const noexcept;

  // CutScenePlayNoti (and VisibleChannelNoti for invisible cutscenes) for a plan's in-place
  // cutscenes, plus its scripted doodad phases. Only one server cutscene plays at a time,
  // like in the tutorial.
  std::vector<GameplayPacket> play(const TravelPlan &plan, const WorldPosition &position,
                                   std::int64_t monotonic_ticks);

  // PC2S_CutSceneEndReq for the active travel cutscene: restores visibility. Returns nullopt
  // when the request names no active travel cutscene (another handler may own it).
  std::optional<std::vector<GameplayPacket>> end(const WorldGameplayRequest &request);

  std::optional<WorldPosition> take_cut_scene_destination();

private:
  std::shared_ptr<const Catalog> catalog_;
  std::uint64_t player_unit_id_;
  std::optional<int> active_cut_scene_;
  std::optional<WorldPosition> pending_cut_scene_destination_;
  std::optional<WorldPosition> completed_cut_scene_destination_;
};

} // namespace chronicles::travel
