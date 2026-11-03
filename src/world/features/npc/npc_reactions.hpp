#pragma once

#include "protocol/gameplay_packet.hpp"
#include "world/core/unit_registry.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace chronicles::npc {

inline constexpr std::uint16_t kThreatenedNpcRequest = 20056;

// Reaction scopes name one shared channel ("shared:<map>:<channel>") or one tutorial
// instance. Private instance scopes start with this prefix and end with their session.
inline constexpr std::string_view kPrivateReactionScopePrefix = "private:";

struct ThreatReaction {
  int animation_id{};
  std::int64_t duration_ticks{};
};

class NpcReactionCatalog {
public:
  static std::optional<NpcReactionCatalog> load(const std::filesystem::path &catalog_root);
  explicit NpcReactionCatalog(const nlohmann::json &document);
  const ThreatReaction *find(int npc_cms_id) const;
  int range_mm() const noexcept;
  std::size_t size() const noexcept;

private:
  int range_mm_{};
  std::map<int, ThreatReaction> definitions_;
};

struct ReactionTimelineSample {
  std::int64_t route_elapsed{};
  std::int64_t started{};
  std::int64_t ends{};
  int animation_id{};
  float yaw{};
  bool active{};
};

using ReactionTimeline = std::function<ReactionTimelineSample(std::uint64_t, std::int64_t)>;

// Shared by viewers of one channel. A reaction pauses the authored route clock; it does
// not teleport a walking NPC to the route's future pose when the animation ends.
//
// Bounded state: a patrolling unit keeps its accumulated pause for as long as the process
// runs, because its route pose depends on it. A stationary unit's state carries nothing after
// its animation ends and is dropped. When the bound is reached, finished private-instance
// states are discarded next, since private scopes end with their session.
class NpcReactionTimeline {
public:
  static constexpr std::size_t kMaximumActors = 4096;

  bool begin(const std::string &scope, std::uint64_t unit_id, std::int64_t elapsed,
             const ThreatReaction &reaction, float yaw, bool route_bound);
  ReactionTimelineSample sample(const std::string &scope, std::uint64_t unit_id,
                                std::int64_t elapsed) const;
  std::size_t size() const;

private:
  struct State {
    std::int64_t delay{};
    std::int64_t started{};
    std::int64_t ends{};
    int animation_id{};
    float yaw{};
    bool route_bound{};
  };

  // Frees capacity at `elapsed`; requires mutex_ to be held.
  void prune_finished(std::int64_t elapsed);

  mutable std::mutex mutex_;
  std::map<std::pair<std::string, std::uint64_t>, State> states_;
};

GameplayPacket threat_reaction_packet(std::uint64_t unit_id, const UnitPositionMm &position,
                                      const ReactionTimelineSample &reaction,
                                      std::int64_t epoch_ticks);

} // namespace chronicles::npc
