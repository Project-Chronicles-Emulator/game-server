#include "world/features/npc/npc_reactions.hpp"

#include "common/clock_ticks.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace chronicles::npc {
namespace {

constexpr std::int64_t kTicksPerMillisecond = 10000;
constexpr auto kBuildId = "40367e24241c9481bfc07fbf9ef1919f25fb0eaa";
// The researched client dump the profile was exported from (build_tutorial_data.SOURCE_SHA256).
constexpr auto kSourceExecutableSha256 =
    "e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6";

int bounded_integer(const nlohmann::json &value, int minimum, int maximum) {
  if (!value.is_number_integer() || value < minimum || value > maximum) {
    throw std::runtime_error("Invalid NPC reaction profile integer");
  }
  return value.get<int>();
}

} // namespace

std::optional<NpcReactionCatalog>
NpcReactionCatalog::load(const std::filesystem::path &catalog_root) {
  const auto path = catalog_root.parent_path() / "npc-reactions" / "definitions.json";
  if (!std::filesystem::exists(path)) {
    return std::nullopt;
  }
  if (std::filesystem::file_size(path) > 4 * 1024 * 1024) {
    throw std::runtime_error("NPC reaction profile exceeds size limit");
  }
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Cannot read NPC reaction profile");
  }
  return NpcReactionCatalog(nlohmann::json::parse(input));
}

NpcReactionCatalog::NpcReactionCatalog(const nlohmann::json &document) {
  if (document.at("format_version") != 1 || document.at("build_id") != kBuildId ||
      document.at("source_executable_dump_sha256") != kSourceExecutableSha256 ||
      !document.at("npcs").is_array() || document.at("npcs").size() > 10000) {
    throw std::runtime_error("Unsupported NPC reaction profile");
  }
  range_mm_ = bounded_integer(document.at("range_mm"), 1, 100000);
  for (const auto &row : document.at("npcs")) {
    const int id = bounded_integer(row.at("npc_cms_id"), 1, std::numeric_limits<int>::max());
    ThreatReaction reaction;
    reaction.animation_id = bounded_integer(row.at("game_anim_mapping_id"), 0, 1000000);
    reaction.duration_ticks =
        bounded_integer(row.at("duration_ms"), 1, 60000) * kTicksPerMillisecond;
    if (!definitions_.emplace(id, reaction).second) {
      throw std::runtime_error("Duplicate NPC reaction definition");
    }
  }
}

const ThreatReaction *NpcReactionCatalog::find(int npc_cms_id) const {
  const auto found = definitions_.find(npc_cms_id);
  return found == definitions_.end() ? nullptr : &found->second;
}

int NpcReactionCatalog::range_mm() const noexcept {
  return range_mm_;
}

std::size_t NpcReactionCatalog::size() const noexcept {
  return definitions_.size();
}

bool NpcReactionTimeline::begin(const std::string &scope, std::uint64_t unit_id,
                                std::int64_t elapsed, const ThreatReaction &reaction, float yaw,
                                bool route_bound) {
  if (scope.empty() || unit_id == 0 || elapsed < 0 || reaction.duration_ticks <= 0 ||
      reaction.duration_ticks > 60000 * kTicksPerMillisecond ||
      elapsed > std::numeric_limits<std::int64_t>::max() - reaction.duration_ticks ||
      !std::isfinite(yaw)) {
    return false;
  }

  std::lock_guard lock(mutex_);
  const auto key = std::make_pair(scope, unit_id);
  auto found = states_.find(key);
  if (found == states_.end() && states_.size() >= kMaximumActors) {
    prune_finished(elapsed);
    if (states_.size() >= kMaximumActors) {
      return false;
    }
  }

  auto &state = states_[key];
  if (elapsed < state.ends) {
    return false;
  }
  state.delay += state.ends - state.started;
  state.started = elapsed;
  state.ends = elapsed + reaction.duration_ticks;
  state.animation_id = reaction.animation_id;
  state.yaw = yaw;
  state.route_bound = state.route_bound || route_bound;
  return true;
}

void NpcReactionTimeline::prune_finished(std::int64_t elapsed) {
  // A stationary unit's finished reaction leaves no observable state behind.
  std::erase_if(states_, [elapsed](const auto &entry) {
    return !entry.second.route_bound && entry.second.ends <= elapsed;
  });
  if (states_.size() < kMaximumActors) {
    return;
  }

  // Private instances end with their session; a finished reaction there is the least valuable
  // remaining state. Shared patrol offsets are kept so live channels never jump along routes.
  std::erase_if(states_, [elapsed](const auto &entry) {
    return entry.first.first.starts_with(kPrivateReactionScopePrefix) &&
           entry.second.ends <= elapsed;
  });
}

std::size_t NpcReactionTimeline::size() const {
  std::lock_guard lock(mutex_);
  return states_.size();
}

ReactionTimelineSample NpcReactionTimeline::sample(const std::string &scope, std::uint64_t unit_id,
                                                   std::int64_t elapsed) const {
  std::lock_guard lock(mutex_);
  const auto found = states_.find({scope, unit_id});
  if (found == states_.end()) {
    return {std::max<std::int64_t>(0, elapsed)};
  }
  const auto &state = found->second;
  const auto paused =
      std::clamp(elapsed - state.started, std::int64_t{0}, state.ends - state.started);
  return {std::max<std::int64_t>(0, elapsed - state.delay - paused),
          state.started,
          state.ends,
          state.animation_id,
          state.yaw,
          elapsed >= state.started && elapsed < state.ends};
}

GameplayPacket threat_reaction_packet(std::uint64_t unit_id, const UnitPositionMm &position,
                                      const ReactionTimelineSample &reaction,
                                      std::int64_t epoch_ticks) {
  return {
      69,
      {{"unit_id", std::to_string(unit_id)},
       {"tick", wire_monotonic_tick(epoch_ticks + reaction.started)},
       {"pos",
        {{"millimeter_x", position.x_mm},
         {"millimeter_y", position.y_mm},
         {"millimeter_z", position.z_mm}}},
       {"body_yaw", reaction.yaw},
       {"x5_motion_data", {{"game_anim_mapping_id", reaction.animation_id}, {"loop_count", 1}}}}};
}

} // namespace chronicles::npc
