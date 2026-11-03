#pragma once

#include "persistence/character_repository.hpp"
#include "protocol/gameplay_packet.hpp"
#include "world/features/quests/quest_rules.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace chronicles::guides {

// The guide text and control icons remain in the matching client build. The server only
// selects catalog IDs when supported, authored conditions become true.
class Catalog {
public:
  static std::shared_ptr<const Catalog> load(const std::filesystem::path &path);
  explicit Catalog(nlohmann::json document);

  const nlohmann::json &guides() const;
  const nlohmann::json *volumes(int map_id) const;

private:
  nlohmann::json document_;
  std::map<int, nlohmann::json> maps_;
};

struct ItemDescription {
  std::string weapon_type;
  std::string grade;
};

struct Snapshot {
  WorldPosition position;
  WorldProgress progress;
  Character character;
};

// Per-character session state is transient. One-shot guides are not re-sent until a new
// session, while area-bound guides are removed when their authored end condition matches.
class Session {
public:
  Session(std::shared_ptr<const Catalog> catalog, quest_rules::QuestLookup quests,
          std::shared_ptr<const std::map<int, ItemDescription>> items);

  std::vector<GameplayPacket> observe(const WorldPosition &position,
                                      std::optional<WorldProgress> progress,
                                      std::optional<Character> character,
                                      const std::vector<GameplayPacket> &action_packets,
                                      std::optional<int> completed_cut_scene = {});

private:
  std::shared_ptr<const Catalog> catalog_;
  quest_rules::QuestLookup quests_;
  std::shared_ptr<const std::map<int, ItemDescription>> items_;
  std::optional<Snapshot> previous_;
  std::set<std::string> inside_;
  std::set<int> active_;
  std::set<int> shown_;
};

} // namespace chronicles::guides
