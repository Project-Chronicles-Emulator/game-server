#pragma once

#include "persistence/character_repository.hpp"
#include "world/core/world_gameplay_request.hpp"
#include "world/features/mail/mail_types.hpp"
#include "world/features/tutorial/tutorial_feature.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace chronicles {

class AuthService;
struct Handoff;

namespace tutorial {

// These storage operations capture the bootstrap Handoff explicitly. At this creation point
// the containing session has not published its current authenticated identity yet.
struct TutorialRewardPersistence {
  std::function<std::optional<Character>()> load_character;
  std::function<std::optional<std::vector<CharacterItem>>(const WorldProgress &,
                                                          const WorldProgress &, const ItemTrade &)>
      commit;
  std::function<void(const Character &)> items_changed;
};

TutorialRewardPersistence
tutorial_reward_persistence(AuthService &auth, const Handoff &handoff,
                            std::function<void(const Character &)> items_changed);

TutorialSession::InstanceCommitResult
commit_tutorial_instances(TutorialRewardPersistence &persistence, const WorldProgress &before,
                          const WorldProgress &after, const std::vector<CharacterItem> &granted);

struct TutorialMapChange {
  WorldPosition spawn;
  std::vector<std::string> removed_unit_ids;
  std::optional<MailRecord> completion_mail;
};

// Gameplay is completed before the application samples its combined deadline and takes the
// codec lock. Encoded frames, handshake mutation and delivery remain in that application phase.
struct TutorialFrameResult {
  std::vector<GameplayPacket> packets;
  std::optional<int> completed_cut_scene;
  std::optional<WorldPosition> finished_teleport;
  std::optional<TutorialMapChange> map_change;
};

class TutorialSessionContext {
public:
  virtual ~TutorialSessionContext() = default;

  virtual std::int64_t tutorial_monotonic_ticks() const = 0;
  virtual bool tutorial_has_combat() const = 0;
  virtual bool tutorial_combat_handles(const WorldGameplayRequest &request) const = 0;
  virtual bool tutorial_combat_busy() const = 0;
  virtual bool tutorial_player_in_combat() const {
    return false;
  }
  virtual std::vector<GameplayPacket> tutorial_advance_combat(std::int64_t ticks,
                                                              const WorldPosition &position) = 0;
  virtual std::vector<GameplayPacket> tutorial_receive_combat(const WorldGameplayRequest &request,
                                                              const WorldPosition &position,
                                                              std::int64_t ticks) = 0;
  virtual std::vector<GameplayPacket>
  tutorial_revive_at_shrine(const WorldGameplayRequest &request) = 0;
  virtual bool tutorial_has_loot() const = 0;
  virtual bool tutorial_loot_handles(std::uint16_t opcode) const = 0;
  virtual std::vector<GameplayPacket> tutorial_receive_loot(const nlohmann::json &request,
                                                            const WorldPosition &position,
                                                            std::int64_t ticks) = 0;
  virtual std::vector<GameplayPacket> tutorial_advance_loot(std::int64_t ticks) = 0;

  virtual void tutorial_apply_level_change(const WorldProgress &progress,
                                           std::vector<GameplayPacket> &packets) = 0;
  virtual void tutorial_append_encumbrance_changes(std::vector<GameplayPacket> &packets,
                                                   bool initial) = 0;
  virtual void tutorial_persist_player_death(std::vector<GameplayPacket> &packets) = 0;
  virtual void tutorial_collect_activity_rewards(std::vector<GameplayPacket> &packets) = 0;

  virtual bool tutorial_authenticated() const = 0;
  virtual void tutorial_end_combat() = 0;
  virtual void tutorial_reset_loot() = 0;
  virtual std::optional<int> tutorial_population_map() const = 0;
  virtual bool tutorial_has_party_identity() const = 0;
  virtual void tutorial_party_graduation(const WorldProgress &progress) = 0;
  virtual bool tutorial_has_mail_repository() const = 0;
  virtual bool tutorial_has_shop_catalog() const = 0;
  virtual std::optional<MailRecord> tutorial_deliver_completion_mail(std::int64_t epoch_ticks) = 0;
  virtual std::optional<std::string> tutorial_graduation_notice() const = 0;
  virtual void tutorial_gm_reply(const std::string &text) = 0;
  virtual void tutorial_set_population_map(std::optional<int> map_id) = 0;
  virtual std::optional<Character> tutorial_stored_character() = 0;
  virtual void tutorial_create_gathering(const Character &character) = 0;
  virtual void tutorial_create_companions(const Character &character) = 0;
  virtual void tutorial_create_quests(std::int64_t character_id) = 0;
  virtual bool tutorial_save_world_position(const WorldPosition &position) = 0;
  virtual std::uint64_t tutorial_connection_id() const = 0;
};

class TutorialPlayerSession {
public:
  TutorialPlayerSession(const TutorialFeature &feature, TutorialSessionContext &context);

  void initialize(std::int64_t wire_unit_id, WorldProgress progress, TutorialSession::Commit commit,
                  std::shared_ptr<const Progression> progression,
                  std::shared_ptr<const TutorialData> followup_data);
  void bind_instance_rewards(TutorialRewardPersistence persistence);

  bool has_instance() const noexcept;
  // Borrowed only by the application's existing named capability bridges. Never retained by
  // another feature; this domain remains the single owner of the authoritative progress cache.
  TutorialSession *current_instance() noexcept;
  const TutorialSession *current_instance() const noexcept;

  void reset_instance();
  void reset_bootstrap_flags();
  void mark_bootstrap_sent();
  std::vector<GameplayPacket> bootstrap_packets();

  std::optional<TutorialFrameResult> handle_frame(const WorldGameplayRequest *request,
                                                  const std::optional<WorldPosition> &position);
  TutorialMapChange leave_map(const TutorialMapMove &move);
  std::optional<std::int64_t> next_action_tick() const;
  std::vector<GameplayPacket> advance(std::int64_t ticks, const WorldPosition &position);

private:
  std::optional<MailRecord> deliver_completion_mail();

  const TutorialFeature &feature_;
  TutorialSessionContext &context_;
  std::unique_ptr<TutorialSession> session_;
  bool snapshot_sent_{};
  bool sectors_sent_{};
};

} // namespace tutorial
} // namespace chronicles
