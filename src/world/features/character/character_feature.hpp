#pragma once

#include "common/world_metrics.hpp"
#include "world/core/frame_context.hpp"
#include "world/core/session_event_bus.hpp"
#include "world/features/character/achievements.hpp"
#include "world/features/character/character_progress_store.hpp"
#include "world/features/character/character_stats.hpp"
#include "world/features/character/graduation_grant.hpp"
#include "world/features/character/progression.hpp"
#include "world/features/character/progression_edit.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace chronicles::character {

class CharacterSession;
class CharacterSessionContext;

class CharacterFeature final {
public:
  CharacterFeature(std::shared_ptr<const CharacterStats> stats,
                   std::shared_ptr<ClientCatalog> catalog,
                   std::shared_ptr<const ShopCatalog> items);

  bool has_stats() const noexcept;
  nlohmann::json stats(const Character &character) const;
  const std::shared_ptr<const CharacterStats> &stats_model() const noexcept;
  const std::shared_ptr<const Progression> &progression() const noexcept;
  const std::shared_ptr<const AchievementCatalog> &achievements() const noexcept;
  bool has_graduation() const noexcept;
  std::optional<std::string> graduation_notice(bool game_master) const;
  void enable_graduation();
  GraduationGrantServices graduation_services() const;
  void reconcile_entry(CharacterProgressStore &store, Character &character) const;
  void apply_entry_graduation(GraduationGrantContext &context, Character &character) const;
  std::optional<nlohmann::json> entry_perks(const Character &character,
                                            const WorldProgress &progress) const;
  std::unique_ptr<CharacterSession> create_session(CharacterSessionContext &context,
                                                   SessionEventBus &events);

private:
  const std::shared_ptr<const CharacterStats> stats_;
  const std::shared_ptr<ClientCatalog> catalog_;
  const std::shared_ptr<const ShopCatalog> items_;
  const std::shared_ptr<const Progression> progression_;
  const std::shared_ptr<const AchievementCatalog> achievements_;
  std::shared_ptr<const GraduationLoadout> loadout_;
};

// Current live owner queries and result-bearing effects. No tutorial/combat/party instance
// pointer escapes its owner, and neither progress nor the stat cache is mirrored here.
class CharacterSessionContext : public CharacterProgressStore {
public:
  // Achievement XP earned inside an instance must be committed with its activity reward.
  virtual bool progression_save_activity(const WorldProgress &before,
                                         const WorldProgress &after) = 0;
  virtual bool character_has_handoff() const = 0;
  virtual std::uint64_t character_player_unit_id(std::int64_t character_id) const = 0;
  virtual const WorldProgress *character_tutorial_progress() const = 0;
  virtual bool character_has_combat() const = 0;
  virtual const nlohmann::json *character_combat_stats() const = 0;
  virtual void character_update_combat_stats(const nlohmann::json &stats) = 0;
  virtual bool character_refill_player_vitals() = 0;
  virtual std::optional<int> character_party_level() const = 0;
  virtual world_metrics::TimedMutex &character_protocol_mutex() const = 0;
  virtual GameplayPacket character_error(const WorldGameplayRequest &request,
                                         x5::ec::EErrorCode code) const = 0;
  virtual GameplayPacket character_stat_notification(std::int64_t unit_id,
                                                     const nlohmann::json &before,
                                                     const nlohmann::json &after,
                                                     const nlohmann::json &current) const = 0;
  virtual void character_deliver_observers(const std::vector<GameplayPacket> &packets) = 0;
};

class CharacterSession final {
public:
  CharacterSession(CharacterFeature &feature, CharacterSessionContext &context,
                   SessionEventBus &events);

  bool handle(FrameContext &frame);
  std::vector<GameplayPacket> apply_operator_edit(const ProgressionEdit &edit);
  // The packet list is an actual result; this is an explicit synchronous capability,
  // rather than a mutable event payload. Identity/cache effects publish at their old sites.
  void apply_level_change(const WorldProgress &progress, std::vector<GameplayPacket> &packets);

private:
  struct ProgressionChange {
    Character character;
    WorldProgress before;
    WorldProgress after;
    bool award_activity_experience{false};
  };

  std::optional<ProgressionChange> begin_progression_change();
  bool commit_progression_change(ProgressionChange &change, std::vector<GameplayPacket> &packets);
  std::vector<GameplayPacket> progression_request(const WorldGameplayRequest &request);
  std::vector<GameplayPacket> update_achievement(const WorldGameplayRequest &request);
  std::vector<GameplayPacket> update_stat_bonus(const WorldGameplayRequest &request);
  static GameplayPacket perk_response(int packet_index, x5::ec::EErrorCode status,
                                      const WorldProgress &progress);
  GameplayPacket unit_perk_packet(const std::vector<PerkUpdate> &perks) const;
  std::vector<GameplayPacket> change_perks(const WorldGameplayRequest &request);
  std::vector<GameplayPacket> reset_perks(const WorldGameplayRequest &request);
  CharacterFeature &feature_;
  CharacterSessionContext &context_;
  SessionEventBus &events_;
};

} // namespace chronicles::character
