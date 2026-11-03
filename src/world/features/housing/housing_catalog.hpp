#pragma once

// Static housing definitions read from the verified client catalog export.
//
// Evidence (client build 40367e24241c9481bfc07fbf9ef1919f25fb0eaa, catalog source SHA-256
// e4002be7...b6, see dumps/ida_research/housing-20261026):
// - Housing, HousingSite, HousingSiteDesign, HousingRentCost, HousingUpgrade, HousingPreset,
//   HousingPresetDoodad, HousingModelPartAddOn, WorldHousingLotNumber, Map, Zone, Item,
//   Doodad, Interaction, InteractionAnim and GameVariable rows are the only inputs. Nothing
//   here is read from a client request.
// - A housing unit ID is (lot_number << 12) | 5: the client builds it with
//   AAC_Unit_MakeTypedUnitId(type 5, lot) before AAC_WorldActors_FindHousingByUnitId.
// - A housing doodad unit ID carries ((house_id << 14) | doodad_id) in its high 32 bits and the
//   housing unit ID in its low 32 bits (UpdateHousingRes removed_doodad_unit_ids decoding).
//   house_id 0 addresses a site-level doodad.
// - HouseNub and HousingDoodadNub positions are site-local millimetres: the client composes
//   them with the housing actor transform (AAC_Housing_ApplyHouseNub). Preset relative
//   positions are the same local frame in centimetres.
// - HousingSite.expiry_epochtime and related fields count 100 ns ticks since the Unix epoch:
//   the client adds them to FDateTime(1970-01-01).

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace chronicles {
class ClientCatalog;
} // namespace chronicles

namespace chronicles::housing {

// Wire enum values (x5.cms.EX5Housing*). Values match the recovered descriptors.
enum class SiteType : int { none = 0, small = 1, medium = 2, large = 3 };
enum class AuthorityType : int { none = 0, individual = 1, guild = 2 };
enum class ContractType : int { none = 0, monthly = 1, periodic = 2, close = 3 };
enum class EditMode : int { build = 0, customize = 1, upgrade = 2 };
enum class HousingCategory : int {
  main_building = 0,
  attached_building = 1,
  function_facility = 2,
  housing_object = 3,
  trophy = 4,
};
enum class MoneyKind { none, gold, hersander_token };

// x5.cms.EX5MapType_HousingArea. MapMoveCompleteNoti into a map of this type completes a
// pending channel visit in the client housing manager.
inline constexpr int kHousingAreaMapType = 3;

inline constexpr std::uint64_t kHousingUnitTypeNibble = 5;

// House and doodad IDs share 14-bit fields inside a doodad unit ID.
inline constexpr std::uint32_t kMaximumObjectId = 0x3FFF;

// One sector is 5100 cm on each axis; the ID packs x | (y << 14) (tools/build_gathering_data.py).
inline constexpr double kSectorSizeCentimetres = 5100.0;
inline constexpr std::uint32_t kSectorAxisLimit = 16384;

// 100 ns ticks per second and per day, the unit of every housing epoch field.
inline constexpr std::int64_t kEpochTicksPerSecond = 10'000'000;
inline constexpr std::int64_t kEpochTicksPerDay = kEpochTicksPerSecond * 86'400;

std::uint64_t housing_unit_id(int lot_number);

std::uint64_t housing_doodad_unit_id(int lot_number, std::uint32_t house_id,
                                     std::uint32_t doodad_id);

struct DoodadUnitKey {
  int lot_number{};
  std::uint32_t house_id{};
  std::uint32_t doodad_id{};
};

// Decodes a client-supplied housing doodad unit ID. Returns nothing when the type nibble, the
// reserved bits or either 14-bit object field is outside the recovered layout.
std::optional<DoodadUnitKey> parse_housing_doodad_unit_id(std::uint64_t unit_id);

// The client computes a sector index per axis as floor(position_cm / 5100) + 1
// (AAC_Sector_Candidate_IndexFromPositionCm, RVA 0x6163D50) and packs x | (y << 14) with
// 14-bit fields. Negative indices therefore wrap inside their field. The client keys housing
// actors by lot number and uses HousingAddNoti.sector_id only to bucket the actor.
std::uint32_t sector_id_for_position(double x_centimetres, double y_centimetres);

// Site-local position in millimetres (HouseNub.pos / HousingDoodadNub.pos).
struct LocalPosition {
  int x_mm{};
  int y_mm{};
  int z_mm{};

  bool operator==(const LocalPosition &) const = default;
};

std::string site_type_name(SiteType type);
std::optional<SiteType> parse_site_type(const std::string &text);

struct ZoneDefinition {
  // x5.cms.EX5ZoneKey value; equals Zone.id in the catalog.
  int zone_key{};
  // Zone.zoneKey text, for example "Zone_01_Housing_001".
  std::string zone_name;
  std::string display_name;
  // Map.id of the HousingArea map whose FK_zoneKey is this zone. Zones without such a map have
  // no loadable area in this client build and cannot be rented.
  std::optional<int> map_id;
  int min_lot_number{};
  int max_lot_number{};
};

struct SiteDefinition {
  int lot_number{};
  int zone_key{};
  SiteType type{SiteType::none};
  double x_cm{};
  double y_cm{};
  double z_cm{};
  double size_x_cm{};
  double size_y_cm{};
  double yaw_degrees{};
  double sign_x_cm{};
  double sign_y_cm{};
  std::string preset_id;
  std::uint32_t sector_id{};
};

struct SiteDesign {
  SiteType type{SiteType::none};
  double size_x_cm{};
  double size_y_cm{};
  double max_height_cm{};
};

struct HousingDefinition {
  int id{};
  HousingCategory category{HousingCategory::main_building};
  int housing_unit_model_id{};
  // Item whose FK_housingId names this building; placing the building consumes one.
  std::optional<int> item_cms_id;
};

struct RentCost {
  AuthorityType authority{AuthorityType::none};
  SiteType site_type{SiteType::none};
  MoneyKind unlock_money{MoneyKind::none};
  std::int64_t unlock_value{};
  int rent_item_cms_id{};
  int rent_item_count{};
};

struct ItemCount {
  int item_cms_id{};
  int count{};

  bool operator==(const ItemCount &) const = default;
};

struct UpgradeDefinition {
  int from_housing_id{};
  int to_housing_id{};
  std::vector<ItemCount> materials;
};

struct PresetHouse {
  std::uint32_t house_id{};
  int attach_index{};
  int housing_id{};
  LocalPosition position;
  float yaw{};
};

struct PresetDoodad {
  std::uint32_t doodad_id{};
  std::uint32_t house_id{};
  int doodad_cms_id{};
  int phase{};
  LocalPosition position;
  float yaw{};
};

struct Preset {
  std::string id;
  SiteType site_type{SiteType::none};
  std::vector<int> part_custom_ids;
  std::vector<PresetHouse> houses;
  std::vector<PresetDoodad> doodads;
};

// A doodad the building model carries (crafting stations of function facilities). Positions
// are relative to the house in the same local frame as preset doodads.
struct ModelAddOnDoodad {
  std::uint32_t doodad_id{};
  int doodad_cms_id{};
  LocalPosition position;
  float yaw{};
};

// A pose a placed piece of furniture offers, such as sitting on a chair (Interaction 70,
// InteractionAnim 70 Chair_Sit_01_INT, attachPolicy HoldAfterEnd) and standing up again
// (Interaction 73, InteractionAnim 71, ReleaseHeld). Only unconditional Use interactions whose
// animation costs nothing and sends no ExecuteInteractionReq (noExecuteNotify) qualify: they
// change the character's pose and nothing else, so the server needs no further rules.
struct FurnitureInteraction {
  int interaction_id{};
  int animation_id{};
  // InteractionAnim.attachPolicy HoldAfterEnd: the character stays in the pose after the
  // animation (sitting); ReleaseHeld and other policies end it.
  bool holds_pose{};

  bool operator==(const FurnitureInteraction &) const = default;
};

// GameVariable rows that govern housing. Defaults equal the exported values of this build and
// only apply when a row is absent from a partial test catalog.
struct HousingRules {
  int site_rent_duration_days{30};
  int site_rent_imminent_days{7};
  int area_rent_duration_days{30};
  int area_rent_imminent_days{7};
  std::chrono::milliseconds edit_save_cooldown{5000};
  std::chrono::minutes guild_site_edit_cooldown{60};
  std::chrono::seconds go_home_cooldown{180};
  std::map<SiteType, int> object_limit{
      {SiteType::small, 160}, {SiteType::medium, 180}, {SiteType::large, 200}};
  int farm_doodad_cms_id{160066};
  std::map<SiteType, int> farm_doodad_limit{
      {SiteType::small, 5}, {SiteType::medium, 10}, {SiteType::large, 15}};
  // Company position level a character needs to hold each site size
  // (Housing_InsufficientCompanyPosition). HousingService::contract enforces the general levels
  // when the transport supplies the actor's company position; guild_allow enforces the guild
  // levels from the guild directory.
  std::map<SiteType, int> general_position_level{
      {SiteType::small, 3}, {SiteType::medium, 4}, {SiteType::large, 5}};
  std::map<SiteType, int> guild_position_level{
      {SiteType::small, 3}, {SiteType::medium, 4}, {SiteType::large, 5}};
};

// Immutable after load; safe to share between threads.
class HousingCatalog {
public:
  // Tables load_from() reads. Missing tables leave housing disabled.
  static const std::vector<std::string> &required_tables();

  static std::shared_ptr<const HousingCatalog> load(ClientCatalog &catalog);

  // Builds the catalog from already-read rows keyed by table name. Throws std::runtime_error
  // naming the table and field of the first invalid row.
  static std::shared_ptr<const HousingCatalog>
  from_rows(const std::map<std::string, std::vector<nlohmann::json>> &tables);

  const HousingRules &rules() const;

  const ZoneDefinition *zone(int zone_key) const;

  const ZoneDefinition *zone_for_map(int map_id) const;

  // Zones that have a loadable HousingArea map, in ascending zone key order.
  std::vector<const ZoneDefinition *> rentable_zones() const;

  const SiteDefinition *site(int lot_number) const;

  // Lots of a zone in ascending lot-number order.
  std::vector<const SiteDefinition *> sites_in_zone(int zone_key) const;

  const SiteDesign *design(SiteType type) const;

  const HousingDefinition *housing(int housing_id) const;

  const RentCost *rent_cost(AuthorityType authority, SiteType type) const;

  const UpgradeDefinition *upgrade(int from_housing_id, int to_housing_id) const;

  const Preset *preset(const std::string &preset_id) const;

  const std::vector<ModelAddOnDoodad> *model_add_ons(int housing_unit_model_id) const;

  // Item that places this doodad (Item.FK_doodadId of a HousingObject or HousingBuilding row).
  std::optional<int> item_for_doodad(int doodad_cms_id) const;

  // Pose interactions of a doodad (Doodad.interactionGroupId -> Interaction rows); nothing for
  // doodads without one.
  const std::vector<FurnitureInteraction> *furniture_interactions(int doodad_cms_id) const;

  // True for HousingObject and HousingBuilding items, the kinds kept in housing storage.
  bool is_storage_item(int item_cms_id) const;

  bool is_discardable_storage_item(int item_cms_id) const;

private:
  HousingRules rules_;
  std::map<int, ZoneDefinition> zones_;
  std::unordered_map<int, int> zone_by_map_;
  std::map<int, SiteDefinition> sites_;
  std::map<SiteType, SiteDesign> designs_;
  std::unordered_map<int, HousingDefinition> housings_;
  std::map<std::pair<AuthorityType, SiteType>, RentCost> rent_costs_;
  std::map<std::pair<int, int>, UpgradeDefinition> upgrades_;
  std::unordered_map<std::string, Preset> presets_;
  std::unordered_map<int, std::vector<ModelAddOnDoodad>> model_add_ons_;
  std::unordered_map<int, int> item_by_doodad_;
  std::unordered_map<int, bool> storage_items_;
  std::unordered_map<int, std::vector<FurnitureInteraction>> furniture_interactions_;
};

} // namespace chronicles::housing
