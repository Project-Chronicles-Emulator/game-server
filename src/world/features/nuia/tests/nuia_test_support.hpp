#pragma once

#include <nlohmann/json.hpp>

namespace chronicles::nuia_test {

using Json = nlohmann::json;

inline Json statue_fixture() {
  return {
      {"format_version", 1},
      {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
      {"source_executable_dump_sha256",
       "e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6"},
      {"interactions",
       {{"10", {{"execute_time_ms", 5000}, {"duration_ms", 6333}}},
        {"16", {{"execute_time_ms", 3408}, {"duration_ms", 4400}}}}},
      {"flame_only_maps", Json::array()},
      {"utility_capacity", {{"40101101", 5}}},
      {"revive_tasks",
       {{"1",
         {{"supplies", {{{"itemId", 40101101}, {"maxCount", 5}}}}, {"refillUtilityBag", true}}}}},
      {"blessing_levels",
       {{{"level", 1}, {"required_tears", 1}},
        {{"level", 2}, {"required_tears", 2}},
        {{"level", 3}, {"required_tears", 3}}}},
      {"placements",
       {
           {{"map_id", 9012},
            {"unit_id", "4102"},
            {"cms_id", 6010000},
            {"kind", "statue"},
            {"position", {0, 0, 0}},
            {"spawn", {500, 0, 0}},
            {"spawn_yaw", 90},
            {"restricted_combat_radius_mm", 10000},
            {"restricted_npc_spawn_radius_mm", 10000},
            {"revive_task_id", 0}},
           {{"map_id", 9012},
            {"unit_id", "8198"},
            {"cms_id", 0},
            {"kind", "flame"},
            {"position", {20000, 0, 0}},
            {"spawn", {20500, 0, 0}},
            {"spawn_yaw", 180},
            {"restricted_combat_radius_mm", 10000},
            {"restricted_npc_spawn_radius_mm", 10000},
            {"revive_task_id", 0}},
       }}};
}

inline Json start_request(int interaction_id = 10) {
  return {{"interaction_anim_id", interaction_id},
          {"target_unit_id", "4102"},
          {"param", {{"interaction", {{"interaction_id", interaction_id}}}}},
          {"pos", Json::object()},
          {"yaw", 0},
          {"tick", "1"}};
}

inline Json tear_fixture(int quantity = 1) {
  return {{"format_version", 1},
          {"build_id", "40367e24241c9481bfc07fbf9ef1919f25fb0eaa"},
          {"source_executable_dump_sha256",
           "e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6"},
          {"placements",
           {{{"map_id", 9012},
             {"unit_id", "123006"},
             {"cms_id", 5010058},
             {"position", {1000, 2000, 3000}},
             {"quantity", quantity}},
            {{"map_id", 9011},
             {"unit_id", "123006"},
             {"cms_id", 5010058},
             {"position", {1000, 2000, 3000}},
             {"quantity", quantity}}}}};
}

inline Json tear_request() {
  return {{"spot_object_id", "123006"},
          {"pos", {{"millimeter_x", 1000}, {"millimeter_y", 2000}, {"millimeter_z", 3000}}}};
}

} // namespace chronicles::nuia_test
