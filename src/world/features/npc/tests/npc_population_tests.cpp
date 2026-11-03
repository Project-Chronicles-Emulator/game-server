#include "world/features/npc/npc_population.hpp"
#include "protocol/protocol.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>

namespace chronicles {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kBound = 64;
// The 2026-10-29 export adds three ArcheWorld Static-layer civilians inside map 9012's area
// (two Constellation Field villagers and one Shining Shore East NPC) to the 261 runtime ones.
constexpr std::size_t kExpectedMap9012ServerNpcs = 264;

// One placement in the stationary placement file format (schema.md, format_version 1).
Json placement_json(int cms_id, int x, const char *guid_x, const char *guid_y) {
  return {{"spawner_guid", "00000000-0000-0000-0000-000000000000"},
          {"spawner_guid_wire", {guid_x, guid_y}},
          {"npc_cms_id", cms_id},
          {"behavior", "Civilian"},
          {"faction", "TownNpc"},
          {"interaction_group_id", 219},
          {"interactions", {{{"interaction_id", 219}, {"task_case", "Shop"}, {"shop_id", 3002}}}},
          {"position_mm", {{"x", x}, {"y", -2000}, {"z", 3000}}},
          {"yaw_degrees", 90.5},
          {"active", true}};
}

Json document_json() {
  return {{"format_version", 1},
          {"map_id", 9012},
          {"layers_missing", {"layer_a", "layer_b"}},
          {"placements",
           {placement_json(1001121, 1000, "30", "40"), placement_json(5001015, 2000, "10", "20"),
            placement_json(1001671, 3000, "10", "5")}}};
}

NpcPopulation make_population(const Json &document = document_json()) {
  return NpcPopulation(NpcPlacementSet::from_json(document, kBound), kBound);
}

TEST(NpcPopulation, LoadsPlacementsForAMapWithTutorialDefaults) {
  auto document = document_json();
  document["placements"][1]["faction"] = "Bandit";
  document["placements"][0]["anim_stance_key"] = 7;
  const auto set = NpcPlacementSet::from_json(document, kBound);

  ASSERT_EQ(set.map_id, 9012);
  EXPECT_EQ(set.missing_layers, 2u);
  ASSERT_EQ(set.placements.size(), 3u);
  EXPECT_EQ(set.placements[0].npc_cms_id, 1001121);
  EXPECT_EQ(set.placements[0].x_mm, 1000);
  EXPECT_EQ(set.placements[0].y_mm, -2000);
  EXPECT_EQ(set.placements[0].z_mm, 3000);
  EXPECT_FLOAT_EQ(set.placements[0].yaw, 90.5F);
  EXPECT_EQ(set.placements[0].faction, "EX5Faction_TownNpc");
  EXPECT_EQ(set.placements[1].faction, "EX5Faction_Bandit");
  EXPECT_EQ(set.placements[0].anim_stance_key, 7);
  EXPECT_FALSE(set.placements[2].anim_stance_key.has_value());
  EXPECT_EQ(set.placements[0].behavior, "Civilian");
  EXPECT_EQ(set.placements[0].interaction_group_id, 219);
  ASSERT_EQ(set.placements[0].interactions.size(), 1u);
  EXPECT_EQ(set.placements[0].interactions[0].interaction_id, 219);
  EXPECT_EQ(set.placements[0].interactions[0].task_case, "Shop");
  EXPECT_EQ(set.placements[0].interactions[0].shop_id, 3002);
  EXPECT_TRUE(set.placements[0].active);
  ASSERT_TRUE(set.placements[0].spawner_guid.has_value());
  EXPECT_EQ(set.placements[0].spawner_guid->first, 30u);
  EXPECT_EQ(set.placements[0].spawner_guid->second, 40u);
}

TEST(NpcPopulation, AcceptsAnEmptyMapAndTheFullGuidRange) {
  Json document{{"format_version", 1}, {"map_id", 1}, {"placements", Json::array()}};
  EXPECT_EQ(make_population(document).units().size(), 0u);

  document["placements"].push_back(
      placement_json(1, 0, "18446744073709551615", "0"));
  const auto set = NpcPlacementSet::from_json(document, kBound);
  EXPECT_EQ(set.placements[0].spawner_guid->first, std::numeric_limits<std::uint64_t>::max());
}

TEST(NpcPopulation, RejectsMalformedPlacementData) {
  const auto rejects = [](const std::function<void(Json &)> &mutate, const char *what) {
    auto document = document_json();
    mutate(document);
    EXPECT_THROW(NpcPlacementSet::from_json(document, kBound), std::runtime_error) << what;
  };
  rejects([](Json &d) { d.erase("format_version"); }, "missing version");
  rejects([](Json &d) { d["format_version"] = 2; }, "unsupported version");
  rejects([](Json &d) { d["format_version"] = "1"; }, "version type");
  rejects([](Json &d) { d.erase("map_id"); }, "missing map");
  rejects([](Json &d) { d["map_id"] = 0; }, "zero map");
  rejects([](Json &d) { d["map_id"] = -3; }, "negative map");
  rejects([](Json &d) { d["map_id"] = 1.5; }, "fractional map");
  rejects([](Json &d) { d.erase("placements"); }, "missing placements");
  rejects([](Json &d) { d["placements"] = Json::object(); }, "placements type");
  rejects([](Json &d) { d["placements"][0] = 5; }, "placement type");
  rejects([](Json &d) { d["placements"][0]["npc_cms_id"] = 0; }, "zero template");
  rejects([](Json &d) { d["placements"][0]["npc_cms_id"] = "1001121"; }, "template type");
  rejects([](Json &d) { d["placements"][0].erase("npc_cms_id"); }, "missing template");
  rejects([](Json &d) { d["placements"][0]["position_mm"] = {{"x", 1}, {"y", 2}}; },
          "position without z");
  rejects([](Json &d) { d["placements"][0]["position_mm"] = {1, 2, 3}; }, "array position");
  rejects([](Json &d) { d["placements"][0]["position_mm"]["x"] = 1.5; }, "fractional position");
  rejects([](Json &d) { d["placements"][0]["position_mm"]["y"] = "1"; }, "text position");
  rejects([](Json &d) { d["placements"][0]["position_mm"]["x"] = 2147483648LL; },
          "position above int range");
  rejects([](Json &d) { d["placements"][0]["position_mm"]["z"] = -2147483649LL; },
          "position below int range");
  rejects([](Json &d) { d["placements"][0]["yaw_degrees"] = "north"; }, "yaw type");
  rejects([](Json &d) { d["placements"][0].erase("yaw_degrees"); }, "missing yaw");
  rejects([](Json &d) { d["placements"][0]["faction"] = "EX5Faction_TownNpc"; }, "prefixed faction");
  rejects([](Json &d) { d["placements"][0]["faction"] = ""; }, "empty faction");
  rejects([](Json &d) { d["placements"][0]["faction"] = 3; }, "faction type");
  rejects([](Json &d) { d["placements"][0]["spawner_guid_wire"] = {"1"}; }, "short guid");
  rejects([](Json &d) { d["placements"][0]["spawner_guid_wire"] = {"x", "2"}; }, "non decimal guid");
  rejects([](Json &d) { d["placements"][0]["spawner_guid_wire"] = {"-1", "2"}; }, "signed guid");
  rejects([](Json &d) { d["placements"][0]["spawner_guid_wire"] = {"", "2"}; }, "empty guid");
  rejects([](Json &d) { d["placements"][0]["spawner_guid_wire"] = {1, 2}; }, "numeric guid");
  rejects([](Json &d) { d["placements"][0]["spawner_guid_wire"] = {"18446744073709551616", "2"}; },
          "guid above uint64");
  rejects([](Json &d) { d["placements"][0]["spawner_guid_wire"] = {"10", "20"}; }, "duplicate guid");
  rejects([](Json &d) { d["placements"][0]["anim_stance_key"] = -1; }, "negative stance");
  rejects([](Json &d) { d["placements"][0]["active"] = "yes"; }, "active type");
  rejects([](Json &d) { d["placements"][0]["behavior"] = 4; }, "behavior type");
  rejects([](Json &d) { d["placements"][0]["interaction_group_id"] = -1; }, "negative group");
  rejects([](Json &d) { d["placements"][0]["interactions"] = 3; }, "interactions type");
  rejects([](Json &d) { d["placements"][0]["interactions"][0]["interaction_id"] = 0; },
          "zero interaction");
  rejects([](Json &d) { d["placements"][0]["interactions"][0]["task_case"] = ""; },
          "empty task case");
  rejects([](Json &d) { d["placements"][0]["interactions"][0]["shop_id"] = -2; }, "negative shop");
  rejects([](Json &d) { d["layers_missing"] = 1; }, "layers_missing type");
}

TEST(NpcPopulation, RejectsADocumentThatIsNotAnObjectOrExceedsTheBound) {
  EXPECT_THROW(NpcPlacementSet::from_json(Json::array(), kBound), std::runtime_error);
  EXPECT_THROW(NpcPlacementSet::from_json(document_json(), 2), std::runtime_error);
  EXPECT_NO_THROW(NpcPlacementSet::from_json(document_json(), 3));
}

TEST(NpcPopulation, UnitBoundsAreEnforcedBeforeAnyIdIsAllocated) {
  const auto set = NpcPlacementSet::from_json(document_json(), kBound);
  EXPECT_THROW(NpcPopulation(set, 0), std::invalid_argument);
  EXPECT_THROW(NpcPopulation(set, 2), std::invalid_argument);
  EXPECT_NO_THROW(NpcPopulation(set, 3));

  auto bad_map = set;
  bad_map.map_id = 0;
  EXPECT_THROW(NpcPopulation(bad_map, kBound), std::invalid_argument);
}

TEST(NpcPopulation, IdsAreUniqueStableAndOutsideTheCharacterRange) {
  const auto population = make_population();
  std::set<std::uint64_t> seen;
  for (const auto &unit : population.units()) {
    EXPECT_TRUE(is_server_npc_unit_id(unit.unit_id));
    EXPECT_GE(unit.unit_id, kFirstNpcUnitId);
    EXPECT_LE(unit.unit_id, kLastNpcUnitId);
    EXPECT_LT(unit.unit_id, 1ULL << 32);
    EXPECT_EQ(unit.unit_id & 0xFULL, kNpcUnitTypeNibble);
    EXPECT_TRUE(seen.insert(unit.unit_id).second);
  }
  EXPECT_EQ(seen.size(), 3u);
  // Character ids (the player's unit id) are positive 32-bit values; none below the NPC range
  // can equal an NPC id.
  EXPECT_FALSE(is_server_npc_unit_id(1));
  EXPECT_FALSE(is_server_npc_unit_id(kFirstNpcUnitId - 1));
  EXPECT_FALSE(is_server_npc_unit_id(kLastNpcUnitId + 1));
  EXPECT_EQ(kLastCharacterUnitId + 1, kFirstNpcUnitId);
  EXPECT_FALSE(is_server_npc_unit_id(kLastCharacterUnitId));
  EXPECT_TRUE(is_server_npc_unit_id(kFirstNpcUnitId));
  EXPECT_TRUE(is_server_npc_unit_id(kLastNpcUnitId));
  EXPECT_FALSE(is_server_npc_unit_id(0));

  // The id follows the placement, not the input order: the guid sort fixes it.
  auto reordered = document_json();
  std::swap(reordered["placements"][0], reordered["placements"][2]);
  const auto other = make_population(reordered);
  ASSERT_EQ(other.units().size(), population.units().size());
  for (std::size_t index = 0; index < population.units().size(); ++index) {
    EXPECT_EQ(other.units()[index].unit_id, population.units()[index].unit_id);
    EXPECT_EQ(other.units()[index].placement.npc_cms_id,
              population.units()[index].placement.npc_cms_id);
  }
  // guid (10,5) < (10,20) < (30,40)
  EXPECT_EQ(population.units()[0].placement.npc_cms_id, 1001671);
  EXPECT_EQ(population.units()[1].placement.npc_cms_id, 5001015);
  EXPECT_EQ(population.units()[2].placement.npc_cms_id, 1001121);
  EXPECT_EQ(population.units()[0].unit_id, kFirstTypedNpcUnitId);
  EXPECT_EQ(population.units()[2].unit_id, kFirstTypedNpcUnitId + 2 * kNpcUnitIdStride);
}

TEST(NpcPopulation, PlacementsWithoutAGuidSortAfterGuidPlacementsAndStayDistinct) {
  Json document{{"format_version", 1}, {"map_id", 5}, {"placements", Json::array()}};
  Json bare = {{"npc_cms_id", 77}, {"position_mm", {{"x", 1}, {"y", 2}, {"z", 3}}},
               {"yaw_degrees", 0}};
  document["placements"].push_back(bare);
  document["placements"].push_back(bare);
  document["placements"].push_back(placement_json(88, 5, "1", "1"));
  const auto population = make_population(document);
  ASSERT_EQ(population.units().size(), 3u);
  EXPECT_EQ(population.units()[0].placement.npc_cms_id, 88);
  EXPECT_NE(population.units()[1].unit_id, population.units()[2].unit_id);
}

TEST(NpcPopulation, UnitAddedPacketsCarryTheFieldsTheTutorialPathSends) {
  const auto population = make_population();
  const auto packets = population.unit_added_packets();
  ASSERT_EQ(packets.size(), 3u);
  const auto &first = packets[0];
  EXPECT_EQ(first.opcode, 10);
  const auto &unit = population.units()[0];
  EXPECT_EQ(first.message.at("unit_id"), std::to_string(unit.unit_id));
  EXPECT_EQ(first.message.at("by_sight"), true);
  const auto &nub = first.message.at("nub");
  EXPECT_EQ(nub.at("pos").at("millimeter_x"), 3000);
  EXPECT_EQ(nub.at("pos").at("millimeter_y"), -2000);
  EXPECT_EQ(nub.at("pos").at("millimeter_z"), 3000);
  EXPECT_FLOAT_EQ(nub.at("yaw").get<float>(), 90.5F);
  const auto &npc = nub.at("npc");
  EXPECT_EQ(npc.at("npc_cms_id"), 1001671);
  EXPECT_EQ(npc.at("base_unit_id"), std::to_string(unit.unit_id));
  EXPECT_EQ(npc.at("state"), "EX5UnitStateType_Idle");
  EXPECT_EQ(npc.at("faction_type"), "EX5Faction_TownNpc");
  EXPECT_EQ(npc.at("spawner_id_x"), "10");
  EXPECT_EQ(npc.at("spawner_id_y"), "5");
  EXPECT_FALSE(npc.contains("anim_stance_key"));

  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  for (const auto &packet : packets) {
    EXPECT_NO_THROW(protocol.encode(packet.opcode, packet.message)) << packet.message;
  }
}

TEST(NpcPopulation, AStanceKeyIsAnnouncedOnlyWhenTheDataSuppliesOne) {
  auto document = document_json();
  document["placements"][0]["anim_stance_key"] = 7;
  const auto packets = make_population(document).unit_added_packets();
  int with_stance = 0;
  for (const auto &packet : packets) {
    if (packet.message.at("nub").at("npc").contains("anim_stance_key")) {
      ++with_stance;
      EXPECT_EQ(packet.message.at("nub").at("npc").at("anim_stance_key"), 7);
    }
  }
  EXPECT_EQ(with_stance, 1);
}

TEST(NpcPopulation, InactivePlacementsAreNeverAllocatedOrAnnounced) {
  auto document = document_json();
  document["placements"][1]["active"] = false;
  const auto population = make_population(document);
  EXPECT_EQ(population.units().size(), 2u);
  EXPECT_EQ(population.skipped_inactive(), 1u);
  for (const auto &unit : population.units()) {
    EXPECT_NE(unit.placement.npc_cms_id, 5001015);
  }
  UnitRegistry registry(kBound);
  EXPECT_EQ(population.announce_to(registry, "instance-a").added, 2u);
  EXPECT_EQ(population.unit_added_packets().size(), 2u);
  // The bound applies to active placements only.
  EXPECT_NO_THROW(NpcPopulation(NpcPlacementSet::from_json(document, kBound), 2));
}

TEST(NpcPopulation, TheFullPlacementFileOfMap9012LoadsWhenPresent) {
  const char *path = std::getenv("CHRONICLES_NPC_PLACEMENTS_9012");
  if (!path) {
    GTEST_SKIP() << "Set CHRONICLES_NPC_PLACEMENTS_9012 to the generated full map-9012 file";
  }
  std::ifstream input(path);
  ASSERT_TRUE(input.good());
  const auto document = Json::parse(input);
  const auto set = NpcPlacementSet::from_json(document, 4096);
  EXPECT_EQ(set.map_id, 9012);
  EXPECT_EQ(set.missing_layers, 8u);
  EXPECT_EQ(set.placements.size(), kExpectedMap9012ServerNpcs);

  const NpcPopulation population(set, 4096);
  EXPECT_EQ(population.units().size(), kExpectedMap9012ServerNpcs);
  EXPECT_EQ(population.skipped_inactive(), 0u);
  UnitRegistry registry(4096);
  const auto result = population.announce_to(registry, "instance-a");
  EXPECT_EQ(result.added, kExpectedMap9012ServerNpcs);
  EXPECT_EQ(result.rejected, 0u);
  EXPECT_EQ(result.collisions, 0u);
  EXPECT_EQ(registry.size(), kExpectedMap9012ServerNpcs);
  for (const auto &unit : population.units()) {
    EXPECT_TRUE(is_server_npc_unit_id(unit.unit_id));
  }

  const auto merchant = std::ranges::find_if(population.units(), [](const PopulatedNpc &unit) {
    return unit.placement.npc_cms_id == 1001121;
  });
  ASSERT_NE(merchant, population.units().end());
  EXPECT_EQ(merchant->placement.x_mm, -1242227);
  EXPECT_EQ(merchant->placement.y_mm, 2993829);
  EXPECT_EQ(merchant->placement.z_mm, -647306);
  ASSERT_EQ(merchant->placement.interactions.size(), 1u);
  EXPECT_EQ(merchant->placement.interactions[0].interaction_id, 212);
  EXPECT_EQ(merchant->placement.interactions[0].shop_id, 1001);
  const auto *announced = registry.find_in_instance(merchant->unit_id, 9012, "instance-a");
  ASSERT_NE(announced, nullptr);
  EXPECT_EQ(announced->npc_template_id, 1001121);
}

TEST(NpcPopulation, EveryGeneratedMapRegistersAndEncodesItsActiveCivilianPopulation) {
  const char *directory = std::getenv("CHRONICLES_NPC_POPULATION_DIRECTORY");
  if (!directory) {
    GTEST_SKIP() << "Set CHRONICLES_NPC_POPULATION_DIRECTORY to generated npc/placements";
  }

  constexpr std::size_t maximum_units = 4096;
  Protocol protocol(CHRONICLES_PROTOCOL_DIR);
  std::size_t checked_maps = 0;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".json" ||
        !entry.path().filename().string().starts_with("map-")) {
      continue;
    }

    SCOPED_TRACE(entry.path().filename().string());
    const auto population = load_npc_population_file(entry.path(), maximum_units);
    UnitRegistry registry(maximum_units);
    const auto result = population->announce_to(registry, "generated-map-check");
    EXPECT_EQ(result.added, population->units().size());
    EXPECT_EQ(result.rejected, 0U);
    EXPECT_EQ(result.collisions, 0U);
    const auto packets = population->unit_added_packets();
    EXPECT_EQ(packets.size(), population->units().size());
    for (const auto &packet : packets) {
      EXPECT_NO_THROW(protocol.encode(packet.opcode, packet.message));
    }
    ++checked_maps;
  }
  EXPECT_GT(checked_maps, 1U);
}

// Identity of one placement in the file: template, position and the placement guid wire halves.
using PlacementKey = std::tuple<int, int, int, int, std::uint64_t, std::uint64_t>;

std::uint64_t guid_half_from_json(const Json &value) {
  if (value.is_string()) {
    return std::stoull(value.get<std::string>());
  }
  return value.get<std::uint64_t>();
}

PlacementKey placement_key_from_json(const Json &placement) {
  const auto &position = placement.at("position_mm");
  const auto &guid = placement.at("spawner_guid_wire");
  return {placement.at("npc_cms_id").get<int>(), position.at("x").get<int>(),
          position.at("y").get<int>(), position.at("z").get<int>(), guid_half_from_json(guid.at(0)),
          guid_half_from_json(guid.at(1))};
}

PlacementKey placement_key_from_unit(const NpcPlacement &placement) {
  const auto guid = placement.spawner_guid.value_or(std::pair<std::uint64_t, std::uint64_t>{});
  return {placement.npc_cms_id, placement.x_mm,       placement.y_mm,
          placement.z_mm,       guid.first,           guid.second};
}

// The real population invariants the integration relies on, checked against the raw file rather
// than against the parser's own view of it.
TEST(NpcPopulation, TheFullMap9012FileYieldsExactlyItsActivePlacementsWithOwnedDeterministicIds) {
  const char *path = std::getenv("CHRONICLES_NPC_PLACEMENTS_9012");
  if (!path) {
    GTEST_SKIP() << "Set CHRONICLES_NPC_PLACEMENTS_9012 to the generated full map-9012 file";
  }
  std::ifstream input(path);
  ASSERT_TRUE(input.good());
  const auto document = Json::parse(input);

  std::multiset<PlacementKey> active_keys;
  std::size_t inactive_count = 0;
  for (const auto &placement : document.at("placements")) {
    if (placement.at("active").get<bool>()) {
      active_keys.insert(placement_key_from_json(placement));
    } else {
      ++inactive_count;
    }
  }
  // Static-NPC actors belong to the client's sector lifecycle and must never acquire a
  // second server-owned UnitAddNoti identity, including Gus (CMS 1001799).
  ASSERT_EQ(active_keys.size(), kExpectedMap9012ServerNpcs);
  for (const auto &placement : document.at("placements")) {
    EXPECT_NE(placement.at("stationary_basis"), "static_npc_data");
    EXPECT_NE(placement.at("npc_cms_id"), 1001799);
  }

  const auto first_load = load_npc_population_file(path, kMaxPopulatedNpcUnits);
  const auto second_load = load_npc_population_file(path, kMaxPopulatedNpcUnits);
  ASSERT_EQ(first_load->units().size(), active_keys.size());
  EXPECT_EQ(first_load->skipped_inactive(), inactive_count);

  // Same multiset of placements as the file's active entries: every inactive placement is
  // excluded and no active one is lost or duplicated.
  std::multiset<PlacementKey> unit_keys;
  std::set<std::uint64_t> unique_ids;
  for (const auto &unit : first_load->units()) {
    EXPECT_TRUE(unit.placement.active);
    unit_keys.insert(placement_key_from_unit(unit.placement));
    unique_ids.insert(unit.unit_id);
    EXPECT_GT(unit.unit_id, kLastCharacterUnitId);
    EXPECT_LE(unit.unit_id, kLastNpcUnitId);
    EXPECT_TRUE(is_server_npc_unit_id(unit.unit_id));
  }
  EXPECT_EQ(unit_keys, active_keys);
  EXPECT_EQ(unique_ids.size(), first_load->units().size());

  // Every deterministic allocation retains the NPC type nibble across loads.
  ASSERT_EQ(second_load->units().size(), first_load->units().size());
  for (std::size_t index = 0; index < first_load->units().size(); ++index) {
    const auto &first = first_load->units()[index];
    const auto &second = second_load->units()[index];
    EXPECT_EQ(first.unit_id, kFirstTypedNpcUnitId + index * kNpcUnitIdStride);
    EXPECT_EQ(first.unit_id & 0xFULL, kNpcUnitTypeNibble);
    EXPECT_EQ(second.unit_id, first.unit_id);
    EXPECT_EQ(placement_key_from_unit(second.placement), placement_key_from_unit(first.placement));
  }
}

// The lean runtime document is no longer accepted.
TEST(NpcPopulation, TheLeanRuntimeDocumentFormIsRejected) {
  const Json runtime{{"format_version", 1},
                     {"map_id", 9012},
                     {"placements",
                      {{{"npc_cms_id", 1001121},
                        {"position", {-1242227, 2993829, -647306}},
                        {"yaw", -12.5},
                        {"spawner_guid", {"13286539544437461146", "6402777569292472999"}},
                        {"anim_stance_key", 3},
                        {"faction", "EX5Faction_TownNpc"}}}}};
  EXPECT_THROW(NpcPlacementSet::from_json(runtime, kBound), std::runtime_error);
}

// Writes `text` to a unique temporary file and removes it on destruction.
class TemporaryFile {
public:
  explicit TemporaryFile(const std::string &text)
      : path_(std::filesystem::temp_directory_path() /
              ("npc-population-" + std::to_string(next_++) + "-" +
               std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".json")) {
    std::ofstream(path_) << text;
  }
  ~TemporaryFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }
  const std::filesystem::path &path() const { return path_; }

private:
  static inline int next_ = 0;
  std::filesystem::path path_;
};

TEST(NpcPopulation, LoadsAPopulationFromAPlacementFile) {
  const TemporaryFile file(document_json().dump());
  const auto population = load_npc_population_file(file.path(), kBound);
  ASSERT_NE(population, nullptr);
  EXPECT_EQ(population->map_id(), 9012);
  EXPECT_EQ(population->units().size(), 3u);
}

TEST(NpcPopulation, AFileThatIsMissingInvalidOrTooLargeIsRefused) {
  EXPECT_THROW(load_npc_population_file("definitely-missing-npc-placements.json", kBound),
               std::runtime_error);
  const TemporaryFile broken("{ not json");
  EXPECT_THROW(load_npc_population_file(broken.path(), kBound), std::runtime_error);
  const TemporaryFile malformed(Json{{"format_version", 2}}.dump());
  EXPECT_THROW(load_npc_population_file(malformed.path(), kBound), std::runtime_error);
  const TemporaryFile three(document_json().dump());
  EXPECT_THROW(load_npc_population_file(three.path(), 2), std::runtime_error);
  EXPECT_GE(kMaxPopulatedNpcUnits, kExpectedMap9012ServerNpcs);
}

TEST(NpcPopulation, AGuidlessPlacementOmitsTheSpawnerFields) {
  Json document{{"format_version", 1}, {"map_id", 5}, {"placements", Json::array()}};
  document["placements"].push_back({{"npc_cms_id", 77},
                                    {"position_mm", {{"x", 1}, {"y", 2}, {"z", 3}}},
                                    {"yaw_degrees", 0}});
  const auto packets = make_population(document).unit_added_packets();
  const auto &npc = packets.at(0).message.at("nub").at("npc");
  EXPECT_FALSE(npc.contains("spawner_id_x"));
  EXPECT_FALSE(npc.contains("spawner_id_y"));
  EXPECT_FALSE(npc.contains("anim_stance_key"));
}

TEST(NpcPopulation, AnnouncingFeedsTheRegistryWithTheServersOwnValues) {
  const auto population = make_population();
  UnitRegistry registry(kBound);
  const auto result = population.announce_to(registry, "instance-a");
  EXPECT_EQ(result.added, 3u);
  EXPECT_EQ(result.rejected, 0u);
  EXPECT_EQ(registry.size(), 3u);

  for (const auto &unit : population.units()) {
    const auto *found = registry.find_in_instance(unit.unit_id, 9012, "instance-a");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->npc_template_id, unit.placement.npc_cms_id);
    EXPECT_EQ(found->position.x_mm, unit.placement.x_mm);
    EXPECT_EQ(found->position.y_mm, unit.placement.y_mm);
    EXPECT_EQ(found->position.z_mm, unit.placement.z_mm);
    EXPECT_EQ(registry.find_in_instance(unit.unit_id, 9013, "instance-a"), nullptr);
    EXPECT_EQ(registry.find_in_instance(unit.unit_id, 9012, "instance-b"), nullptr);
  }
  // A unit id the server never allocated does not resolve.
  EXPECT_EQ(registry.find(kFirstNpcUnitId + 3), nullptr);
  EXPECT_EQ(registry.find(1), nullptr);
}

TEST(NpcPopulation, AnnouncingTwiceOrOverCapacityOrWithoutAnInstanceIsReported) {
  const auto population = make_population();
  UnitRegistry registry(kBound);
  ASSERT_EQ(population.announce_to(registry, "instance-a").added, 3u);
  const auto again = population.announce_to(registry, "instance-a");
  EXPECT_EQ(again.added, 0u);
  EXPECT_EQ(again.rejected, 3u);

  UnitRegistry small(2);
  const auto limited = population.announce_to(small, "instance-a");
  EXPECT_EQ(limited.added, 2u);
  EXPECT_EQ(limited.rejected, 1u);

  UnitRegistry unnamed(kBound);
  const auto none = population.announce_to(unnamed, "");
  EXPECT_EQ(none.added, 0u);
  EXPECT_EQ(none.rejected, 3u);
  EXPECT_EQ(unnamed.size(), 0u);
}

TEST(NpcPopulation, AnIdAlreadyInTheRegistryIsRefusedAndLeftUntouched) {
  const auto population = make_population();
  UnitRegistry registry(kBound);
  // A unit of another owner already holds the second NPC id.
  AnnouncedUnit squatter;
  squatter.unit_id = population.units()[1].unit_id;
  squatter.npc_template_id = 424242;
  squatter.map_id = 1;
  squatter.instance_key = "someone-else";
  squatter.position = {7, 8, 9};
  ASSERT_EQ(registry.announce(squatter), UnitAnnounceResult::Added);

  const auto result = population.announce_to(registry, "instance-a");
  EXPECT_EQ(result.added, 2u);
  EXPECT_EQ(result.rejected, 1u);
  EXPECT_EQ(result.collisions, 1u);
  const auto *kept = registry.find(squatter.unit_id);
  ASSERT_NE(kept, nullptr);
  EXPECT_EQ(kept->npc_template_id, 424242);
  EXPECT_EQ(kept->map_id, 1);
  EXPECT_EQ(kept->instance_key, "someone-else");
  EXPECT_EQ(kept->position.x_mm, 7);

  // A repeated announcement collides on every id and adds nothing.
  UnitRegistry full(kBound);
  ASSERT_EQ(population.announce_to(full, "instance-a").collisions, 0u);
  const auto again = population.announce_to(full, "instance-a");
  EXPECT_EQ(again.added, 0u);
  EXPECT_EQ(again.collisions, 3u);
  EXPECT_EQ(again.rejected, 3u);

  // Other rejection reasons are not collisions.
  UnitRegistry small(1);
  const auto limited = population.announce_to(small, "instance-a");
  EXPECT_EQ(limited.rejected, 2u);
  EXPECT_EQ(limited.collisions, 0u);
}

TEST(NpcPopulation, DespawnRemovesOnlyThePopulationsUnits) {
  const auto population = make_population();
  UnitRegistry registry(kBound);
  AnnouncedUnit other;
  other.unit_id = 4242;
  other.npc_template_id = 9;
  other.map_id = 9012;
  other.instance_key = "instance-a";
  ASSERT_EQ(registry.announce(other), UnitAnnounceResult::Added);
  ASSERT_EQ(population.announce_to(registry, "instance-a").added, 3u);

  EXPECT_EQ(population.despawn_from(registry), 3u);
  EXPECT_EQ(registry.size(), 1u);
  EXPECT_NE(registry.find(4242), nullptr);
  EXPECT_EQ(population.despawn_from(registry), 0u);
  for (const auto &unit : population.units()) {
    EXPECT_EQ(registry.find(unit.unit_id), nullptr);
  }
}



} // namespace
} // namespace chronicles
