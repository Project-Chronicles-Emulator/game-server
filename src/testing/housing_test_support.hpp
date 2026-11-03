#pragma once

// Synthetic housing catalog rows for unit tests. Values follow the exported catalog layout
// (all fields are strings) and reuse real IDs where the tests describe catalog behavior.

#include "world/features/housing/housing_catalog.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace chronicles::housing::testing_support {

inline constexpr int kZoneKey = 112;
inline constexpr int kHousingMapId = 20101;
inline constexpr int kUnmappedZoneKey = 702;
inline constexpr int kSmallLot = 101;
inline constexpr int kSecondSmallLot = 102;
inline constexpr int kMediumLot = 103;
inline constexpr int kUnmappedLot = 701;
inline constexpr int kPermitItem = 123;
inline constexpr int kMainBuilding = 1002;
inline constexpr int kUpgradedBuilding = 1003;
inline constexpr int kFacility = 20;
inline constexpr int kMainBuildingItem = 2000001002;
inline constexpr int kUpgradedBuildingItem = 2000001003;
inline constexpr int kFacilityItem = 2000000020;
inline constexpr int kChairDoodad = 160067;
inline constexpr int kChairItem = 1900000002;
inline constexpr int kFarmDoodad = 160066;
inline constexpr int kFarmItem = 1900000001;
inline constexpr int kStationDoodad = 85002;
inline constexpr int kPresetDoodad = 180005;
inline constexpr int kUpgradeMaterial = 70203001;
inline constexpr int kSitInteraction = 70;
inline constexpr int kSitAnimation = 70;
inline constexpr int kStandInteraction = 73;
inline constexpr int kStandAnimation = 71;
inline constexpr char kSmallPreset[] = "Ssite_Test";

inline nlohmann::json row(std::initializer_list<std::pair<const std::string, std::string>> fields) {
  nlohmann::json object = nlohmann::json::object();
  for (const auto &[name, value] : fields) {
    object[name] = value;
  }
  return object;
}

inline std::map<std::string, std::vector<nlohmann::json>> housing_tables() {
  std::map<std::string, std::vector<nlohmann::json>> tables;
  tables["GameVariable"] = {
      row({{"name", "HousingSiteRentDurationDays"}, {"value", "30"}}),
      row({{"name", "HousingSiteRentImminentDurationDays"}, {"value", "7"}}),
      row({{"name", "HousingEditModeSaveCooldown"}, {"value", "5000"}}),
      row({{"name", "HousingSiteSObjectLimitCount"}, {"value", "8"}}),
      row({{"name", "HousingFarmDoodadSmall"}, {"value", "160066"}}),
      row({{"name", "HousingFarmDoodadSmallLimitInHousingSiteS"}, {"value", "1"}}),
      row({{"name", "GoHomeCooltimeSec"}, {"value", "180"}}),
  };
  tables["Zone"] = {
      row({{"id", "112"}, {"zoneKey", "Zone_01_Housing_001"}, {"name", "Seaglass Harbor"}}),
      row({{"id", "702"}, {"zoneKey", "Zone_07_Housing_001"}, {"name", "Centrium"}}),
      row({{"id", "100"}, {"zoneKey", "Zone_01"}, {"name", "Diamond Shores"}}),
  };
  tables["WorldHousingLotNumber"] = {
      row({{"zoneKey", "Zone_01_Housing_001"},
           {"minHousingLotNumber", "101"},
           {"maxHousingLotNumber", "141"}}),
      row({{"zoneKey", "Zone_07_Housing_001"},
           {"minHousingLotNumber", "701"},
           {"maxHousingLotNumber", "730"}}),
  };
  tables["Map"] = {
      row({{"id", "20101"}, {"type", "HousingArea"}, {"FK_zoneKey", "Zone_01_Housing_001"}}),
      row({{"id", "9012"}, {"type", "Field"}, {"FK_zoneKey", "Zone_01"}}),
  };
  tables["HousingSiteDesign"] = {
      row({{"siteType", "Small"}, {"sizeX", "2100"}, {"sizeY", "2100"}, {"maxHeight", "4000"}}),
      row({{"siteType", "Medium"}, {"sizeX", "3000"}, {"sizeY", "3000"}, {"maxHeight", "4000"}}),
  };
  tables["HousingSite"] = {
      row({{"lotNumber", "101"}, {"zoneKey", "Zone_01_Housing_001"}, {"siteType", "Small"},
           {"x", "-53530.11"}, {"y", "343941.5"}, {"z", "-62525.2"}, {"sizeX", "2100"},
           {"sizeY", "2100"}, {"yaw", "-163.1"}, {"signX", "-54379.9"}, {"signY", "343025.3"},
           {"fk_PresetId", kSmallPreset}}),
      row({{"lotNumber", "102"}, {"zoneKey", "Zone_01_Housing_001"}, {"siteType", "Small"},
           {"x", "-50000"}, {"y", "343000"}, {"z", "-62500"}, {"sizeX", "2100"}, {"sizeY", "2100"},
           {"yaw", "0"}, {"signX", "-50500"}, {"signY", "342500"}, {"fk_PresetId", ""}}),
      row({{"lotNumber", "103"}, {"zoneKey", "Zone_01_Housing_001"}, {"siteType", "Medium"},
           {"x", "-45199.38"}, {"y", "343615.7"}, {"z", "-62990.8"}, {"sizeX", "3000"},
           {"sizeY", "3000"}, {"yaw", "-80.4"}, {"signX", "-44058.0"}, {"signY", "342257.1"},
           {"fk_PresetId", ""}}),
      row({{"lotNumber", "701"}, {"zoneKey", "Zone_07_Housing_001"}, {"siteType", "Small"},
           {"x", "1000"}, {"y", "1000"}, {"z", "0"}, {"sizeX", "2100"}, {"sizeY", "2100"},
           {"yaw", "0"}, {"signX", "1000"}, {"signY", "1000"}, {"fk_PresetId", ""}}),
  };
  tables["Housing"] = {
      row({{"id", "1002"}, {"category", "MainBuilding"}, {"housingUnitModelId", "1002"}}),
      row({{"id", "1003"}, {"category", "MainBuilding"}, {"housingUnitModelId", "1003"}}),
      row({{"id", "20"}, {"category", "FunctionFacility"}, {"housingUnitModelId", "20"}}),
  };
  tables["Item"] = {
      row({{"id", "2000001002"}, {"category", "HousingBuilding"}, {"FK_housingId", "1002"},
           {"discardable", "True"}}),
      row({{"id", "2000001003"}, {"category", "HousingBuilding"}, {"FK_housingId", "1003"},
           {"discardable", "True"}}),
      row({{"id", "2000000020"}, {"category", "HousingBuilding"}, {"FK_housingId", "20"},
           {"FK_doodadId", "85002"}, {"discardable", "True"}}),
      row({{"id", "1900000001"}, {"category", "HousingObject"}, {"FK_doodadId", "160066"},
           {"discardable", "True"}}),
      row({{"id", "1900000002"}, {"category", "HousingObject"}, {"FK_doodadId", "160067"},
           {"discardable", "False"}}),
      row({{"id", "123"}, {"category", "HousingPermit"}}),
  };
  tables["HousingRentCost"] = {
      row({{"authorityType", "Individual"}, {"siteType", "Small"}, {"unlockMoneyType", ""},
           {"unlockValue", "0"}, {"rentItemId", "123"}, {"rentItemCount", "60"}}),
      row({{"authorityType", "Individual"}, {"siteType", "Medium"},
           {"unlockMoneyType", "HersanderToken"}, {"unlockValue", "1000"}, {"rentItemId", "123"},
           {"rentItemCount", "240"}}),
      row({{"authorityType", "Guild"}, {"siteType", "None"},
           {"unlockMoneyType", "HersanderToken"}, {"unlockValue", "5000"}, {"rentItemId", "123"},
           {"rentItemCount", "3000"}}),
  };
  tables["HousingUpgrade"] = {
      row({{"FK_fromHousingId", "1002"}, {"FK_toHousingId", "1003"},
           {"materialId01", "70203001"}, {"materialCnt01", "20"}}),
  };
  tables["HousingPreset"] = {
      row({{"id", kSmallPreset},
           {"siteType", "Small"},
           {"partCustomIds", "[0,0,0,0,0,0]"},
           {"houses", R"([{"houseId":1,"attachIndex":-1,"housingCmsId":1002,)"
                      R"("relativePosition":{"x":-480,"y":-30,"z":0},"relativeYaw":0}])"}}),
  };
  tables["HousingPresetDoodad"] = {
      row({{"id", kSmallPreset}, {"housingDoodadId", "1"}, {"houseId", "0"},
           {"FK_doodad", "180005"}, {"phase", "1"},
           {"relativePosition", R"("x":742.5,"y":-367.5,"z":0)"}, {"relativeYaw", "180"}}),
  };
  tables["HousingModelPartAddOn"] = {
      row({{"housingUnitModelId", "20"}, {"housingDoodadId", "1"}, {"doodadCmsId", "85002"},
           {"relativeX", "-88"}, {"relativeY", "0"}, {"relativeZ", "0"},
           {"relativeYaw", "-90"}}),
  };
  // The synthetic chair offers the real sit/stand pair (Interaction group 70). The farm's
  // group mixes a conditional Use and a crafting-style animation, neither of which is a pose.
  tables["Doodad"] = {
      row({{"id", "160067"}, {"propertyTypeCase", "Functional"}, {"interactionGroupId", "70"}}),
      row({{"id", "160066"}, {"propertyTypeCase", "Farm"}, {"interactionGroupId", "160005"}}),
      row({{"id", "180005"}, {"propertyTypeCase", "Decorate"}, {"interactionGroupId", ""}}),
  };
  tables["Interaction"] = {
      row({{"id", "70"}, {"groupId", "70"}, {"FK_conditionId", ""}, {"playerTaskCase", "Use"},
           {"FK_interactionAnimId", "70"}}),
      row({{"id", "73"}, {"groupId", "70"}, {"FK_conditionId", ""}, {"playerTaskCase", "Use"},
           {"FK_interactionAnimId", "71"}}),
      row({{"id", "160005"}, {"groupId", "160005"}, {"FK_conditionId", "160001"},
           {"playerTaskCase", "Use"}, {"FK_interactionAnimId", "70"}}),
      row({{"id", "160006"}, {"groupId", "160005"}, {"FK_conditionId", ""},
           {"playerTaskCase", "Use"}, {"FK_interactionAnimId", "160001"}}),
  };
  tables["InteractionAnim"] = {
      row({{"id", "70"}, {"costType", "None"}, {"noExecuteNotify", "True"},
           {"attachPolicy", "HoldAfterEnd"}}),
      row({{"id", "71"}, {"costType", "None"}, {"noExecuteNotify", "True"},
           {"attachPolicy", "ReleaseHeld"}}),
      row({{"id", "160001"}, {"costType", "None"}, {"noExecuteNotify", ""}}),
  };
  return tables;
}

inline std::shared_ptr<const HousingCatalog> housing_catalog() {
  return HousingCatalog::from_rows(housing_tables());
}

// A garden plot graph shaped like the exported 160066 profile: planting spends five seeds,
// growth is timed, the ripe plot is harvested for crops and rests before it can be replanted.
inline constexpr int kSeedItem = 50000;
inline constexpr int kCropItem = 60000;
inline constexpr int kPlantInteraction = 160005;
inline constexpr int kPlantAnimation = 160199;
inline constexpr int kHarvestInteraction = 160002;
inline constexpr int kHarvestAnimation = 160002;
inline constexpr int kEmptyPlotPhase = 1;
inline constexpr int kSownPhase = 101;
inline constexpr int kRipePhase = 102;
inline constexpr int kRestingPhase = 9;
inline constexpr int kGrowthMilliseconds = 1000;
inline constexpr int kRestMilliseconds = 2000;
inline constexpr int kPlantExecuteMilliseconds = 3912;

inline nlohmann::json garden_profile_json() {
  using Json = nlohmann::json;
  const Json crop_reward{{"pack_rate_percent", 100.0},
                         {"reason", "system"},
                         {"groups",
                          Json::array({{{"rate_percent", 100.0},
                                        {"items", Json::array({{{"item_cms_id", kCropItem},
                                                                {"min_quantity", 2},
                                                                {"max_quantity", 2},
                                                                {"weight", 1}}})}}})}};
  const auto phase = [](int phase_id, Json transitions) {
    return Json{{"phase_id", phase_id}, {"transitions", std::move(transitions)}};
  };
  const auto timer = [](int duration_ms, int next) {
    return Json{{"trigger", "timer"},
                {"duration_ms", duration_ms},
                {"next", Json::array({Json::array({next, 1})})}};
  };
  return Json{
      {"format_version", 1},
      {"sp_regen_delay_ms", 300},
      {"animations",
       Json::array({{{"interaction_anim_id", kPlantAnimation},
                     {"sp_cost", 30},
                     {"tool_equipment_type", "None"},
                     {"execute_time_ms", kPlantExecuteMilliseconds},
                     {"duration_ms", 6333}},
                    {{"interaction_anim_id", kHarvestAnimation},
                     {"sp_cost", 50},
                     {"tool_equipment_type", "Sickle"},
                     {"execute_time_ms", 6777},
                     {"duration_ms", 8666}}})},
      {"doodads",
       Json::array(
           {{{"doodad_id", kFarmDoodad},
             {"phases",
              Json::array(
                  {phase(kEmptyPlotPhase,
                         Json::array({{{"trigger", "interaction"},
                                       {"interaction_id", kPlantInteraction},
                                       {"interaction_anim_id", kPlantAnimation},
                                       {"conditions",
                                        Json::array({{{"kind", "consume"},
                                                      {"item_cms_id", kSeedItem},
                                                      {"count", 5}}})},
                                       {"next", Json::array({Json::array({kSownPhase, 1})})}}})),
                   phase(kSownPhase, Json::array({timer(kGrowthMilliseconds, kRipePhase)})),
                   phase(kRipePhase,
                         Json::array({{{"trigger", "interaction"},
                                       {"interaction_id", kHarvestInteraction},
                                       {"interaction_anim_id", kHarvestAnimation},
                                       {"conditions", Json::array()},
                                       {"reward", crop_reward},
                                       {"next",
                                        Json::array({Json::array({kRestingPhase, 1})})}}})),
                   phase(kRestingPhase,
                         Json::array({timer(kRestMilliseconds, kEmptyPlotPhase)}))})}}})}};
}

} // namespace chronicles::housing::testing_support
