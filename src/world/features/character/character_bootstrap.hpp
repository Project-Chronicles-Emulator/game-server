#pragma once

#include "persistence/character_repository.hpp"
#include "protocol/gameplay_packet.hpp"

#include <memory>
#include <optional>

namespace chronicles {

class CharacterStats;

namespace character {

// Actual character presentation state for entry and subsequent refreshes. Map/transport
// sequencing stays with WorldHandshake; it borrows this one selected character and snapshot.
class CharacterBootstrap final {
public:
  explicit CharacterBootstrap(std::shared_ptr<const CharacterStats> stats);

  // The old configure path computes stats before moving non-character bootstrap fields,
  // then adopts the character last. Keep those two operations separately ordered.
  void prepare_stats(const Character &character);
  void set_entry_dead(bool dead);
  void select_character(Character character);
  const Character *selected_character() const noexcept;
  const nlohmann::json &stats() const noexcept;
  bool dead() const noexcept;
  void relocate(const WorldPosition &position);

  void refresh_items_and_stats(const Character &character, const nlohmann::json *prepared_stats);
  void refresh_progression(int level, std::int64_t experience, const CharacterBuild &build);
  void set_perk_snapshot(nlohmann::json snapshot);
  void set_game_master(bool game_master);
  void set_authoritative_player_state(const nlohmann::json &stats, bool dead);
  std::optional<GameplayPacket> base_snapshot() const;
  std::optional<GameplayPacket> perk_snapshot() const;

private:
  std::optional<Character> character_;
  std::shared_ptr<const CharacterStats> stats_model_;
  nlohmann::json stats_ = nlohmann::json::object();
  std::optional<nlohmann::json> perk_snapshot_;
  bool game_master_{};
  bool dead_{};
};

} // namespace character
} // namespace chronicles
