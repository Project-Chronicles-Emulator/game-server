#pragma once

#include "auth/auth.hpp"
#include "common/world_metrics.hpp"
#include "persistence/company_record.hpp"
#include "world/core/frame_context.hpp"
#include "world/core/unit_registry.hpp"
#include "world/features/combat/combat.hpp"
#include "world/features/company/company.hpp"
#include "world/features/gm/gm_commands.hpp"
#include "world/features/gm/gm_diagnostics.hpp"
#include "world/features/gm/gm_world_commands.hpp"
#include "world/features/gm/social_gm_commands.hpp"
#include "world/features/mail/mail_types.hpp"
#include "world/features/quests/world_quests.hpp"

#include <boost/asio/any_io_executor.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace chronicles {

class ClientCatalog;
class CombatSession;
struct Frame;

namespace gm {

struct GmIdentity {
  std::int64_t account_id{};
  std::int64_t character_id{};
  int world_id{};
  bool game_master{};
};

struct GmPeerSnapshot {
  std::uint64_t connection_id{};
  std::int64_t account_id{};
  std::int64_t character_id{};
  bool authenticated{};
  bool game_master{};
  std::string character_name;
  std::optional<WorldPosition> position;
};

// The only operations a GM may perform on an online peer. Returned handles alias the
// containing session's shared owner, retaining its identity/worker/connection lifetime.
class GmPeer {
public:
  virtual ~GmPeer() = default;

  virtual void post_gm_summon(const WorldPosition &destination, std::string summoner) = 0;
  virtual void post_frame(Frame frame) = 0;
};

struct GmTarget {
  std::shared_ptr<GmPeer> session;
  GmPeerSnapshot summary;
};

struct GmOnlinePlayers {
  std::vector<std::string> lines;
  // Keep all peers locked from the directory alive until every private list reply finishes.
  // These handles grant no peer operation; they preserve the original final-release timing.
  std::vector<std::shared_ptr<void>> retained_peers;
};

struct GmWatchedUnit {
  WorldPosition position;
  std::optional<NpcAiState> ai_state;
  std::optional<WorldPosition> route_end;
};

// Existing GM runtime commands/queries; implementations stay in composition.
class GmSessionAccess {
public:
  virtual ~GmSessionAccess() = default;

  virtual std::optional<GmIdentity> gm_identity() const = 0;

  virtual std::optional<ChatIdentity> gm_chat_identity() const = 0;

  virtual std::uint64_t gm_connection_id() const = 0;

  virtual bool gm_closed() const = 0;

  virtual std::int64_t gm_monotonic_ticks() const = 0;

  virtual std::int64_t gm_epoch_ticks() const = 0;

  virtual world_metrics::TimedMutex &gm_protocol_mutex() const = 0;

  virtual std::optional<WorldPosition> gm_position_locked() const = 0;

  virtual Frame gm_encode_packet_locked(std::uint16_t opcode, const nlohmann::json &message) = 0;

  virtual void gm_post_frame(Frame frame) = 0;

  virtual void gm_close_on_io() = 0;

  virtual boost::asio::any_io_executor gm_io_executor() = 0;

  virtual boost::asio::any_io_executor gm_worker_executor() = 0;

  virtual std::shared_ptr<void> retain_gm_owner() = 0;

  virtual std::optional<std::int64_t> gm_next_gameplay_tick() const = 0;
};

// Existing GM combat commands/queries; implementations stay in composition.
class GmCombatCommands {
public:
  virtual ~GmCombatCommands() = default;

  virtual bool gm_combat_add_monster(std::uint64_t unit_id, int npc_cms_id,
                                     const WorldPosition &position) = 0;

  virtual bool gm_combat_decorate_spawn(GameplayPacket &packet) const = 0;

  virtual std::vector<GameplayPacket> gm_combat_add_abnormal_gauge(std::uint64_t unit_id,
                                                                   combat_rules::AbnormalType type,
                                                                   int amount,
                                                                   std::int64_t ticks) = 0;

  virtual std::vector<GameplayPacket>
  gm_combat_add_status_effect(std::uint64_t unit_id, int status_effect_id, std::int64_t ticks) = 0;

  virtual std::vector<GameplayPacket> gm_combat_cancel_skill(std::uint64_t unit_id,
                                                             const WorldPosition &position,
                                                             std::int64_t ticks) = 0;

  virtual std::vector<GameplayPacket> gm_combat_damage_monster(std::uint64_t unit_id, int amount,
                                                               std::int64_t ticks) = 0;

  virtual std::vector<GameplayPacket> gm_combat_execute_self_effect(int effect_id,
                                                                    const WorldPosition &position,
                                                                    std::int64_t ticks) = 0;

  virtual std::vector<GameplayPacket> gm_combat_kill_player(const WorldPosition &position,
                                                            std::int64_t ticks) = 0;

  virtual std::vector<GameplayPacket> gm_combat_place_monster(std::uint64_t unit_id,
                                                              const WorldPosition &destination,
                                                              std::int64_t ticks) = 0;

  virtual std::vector<GameplayPacket> gm_combat_remove_status_effect(std::uint64_t unit_id,
                                                                     int status_effect_id,
                                                                     std::int64_t ticks) = 0;

  virtual std::size_t gm_combat_reset_cooldowns() = 0;

  virtual std::vector<GameplayPacket> gm_combat_restore_vitals(std::int64_t ticks) = 0;

  virtual std::vector<GameplayPacket> gm_combat_set_player_stat(int stat_type, int value,
                                                                std::int64_t ticks) = 0;

  virtual std::vector<GameplayPacket>
  gm_combat_toggle_monster_ai(std::uint64_t unit_id, bool enabled, std::int64_t ticks) = 0;

  virtual bool gm_combat_visible_monster(std::uint64_t unit_id) const = 0;

  virtual std::optional<int> gm_combat_monster_health(std::uint64_t unit_id) const = 0;

  virtual std::optional<CombatSession::MonsterDebugView>
  gm_combat_monster_debug_view(std::uint64_t unit_id, std::int64_t ticks) const = 0;

  virtual bool gm_combat_player_dead() const = 0;

  virtual const nlohmann::json &gm_combat_player_stats() const = 0;

  virtual std::vector<GameplayPacket> gm_combat_remove_monster(std::uint64_t unit_id,
                                                               std::int64_t ticks) = 0;

  virtual void gm_combat_update_player_stats(nlohmann::json stats) = 0;

  virtual bool gm_has_combat() const = 0;

  virtual std::vector<GameplayPacket> gm_advance_combat(std::int64_t ticks,
                                                        const WorldPosition &position) = 0;

  virtual void gm_persist_player_death(std::vector<GameplayPacket> &packets) = 0;

  virtual std::vector<GameplayPacket> gm_revive_player() = 0;
};

// Existing GM character commands/queries; implementations stay in composition.
class GmCharacterCommands {
public:
  virtual ~GmCharacterCommands() = default;

  virtual const ShopCatalog *gm_shop_catalog() const = 0;

  virtual bool gm_has_character_stats() const = 0;

  virtual bool gm_has_tutorial() const = 0;

  virtual std::optional<Character> gm_character() const = 0;

  virtual WorldProgress gm_progress() const = 0;

  virtual WorldProgress gm_stored_progress() const = 0;

  virtual bool gm_save_progress(const WorldProgress &before, const WorldProgress &after) = 0;

  virtual std::optional<std::vector<CharacterItem>>
  gm_commit_item_trade(const WorldProgress &before, const WorldProgress &after,
                       const ItemTrade &trade) = 0;

  virtual void gm_refresh_traded_items(const Character &before, const ShopTrade &trade,
                                       const std::vector<CharacterItem> &committed) = 0;

  virtual nlohmann::json gm_session_stats(const Character &character) const = 0;

  virtual GameplayPacket gm_stat_notification(std::uint64_t unit_id, const nlohmann::json &before,
                                              const nlohmann::json &after,
                                              const nlohmann::json &current) const = 0;

  virtual std::vector<GameplayPacket> gm_execute_progression(const GmCommand &command) = 0;

  virtual void gm_apply_level_change(const WorldProgress &progress,
                                     std::vector<GameplayPacket> &packets) = 0;
};

// Existing GM social commands/queries; implementations stay in composition.
class GmSocialCommands {
public:
  virtual ~GmSocialCommands() = default;

  virtual bool gm_has_guilds() const = 0;

  virtual std::int64_t gm_own_guild_id(std::int64_t character_id) const = 0;

  virtual x5::ec::EErrorCode gm_guild_add_experience(std::int64_t guild_id, int amount) = 0;

  virtual x5::ec::EErrorCode gm_guild_set_level(std::int64_t guild_id, int level) = 0;

  virtual x5::ec::EErrorCode gm_guild_disband(std::int64_t guild_id) = 0;

  virtual x5::ec::EErrorCode gm_guild_set_master(std::int64_t guild_id,
                                                 std::int64_t character_id) = 0;

  virtual x5::ec::EErrorCode gm_guild_reset_cooldowns(std::int64_t character_id) = 0;

  virtual bool gm_has_companies() const = 0;

  virtual CompanyRecord gm_company_record(std::int64_t character_id) const = 0;

  virtual std::optional<std::int64_t> gm_company_threshold(int company_type, int level) const = 0;

  virtual CompanyResult gm_commit_company_record(const CompanyRecord &record) = 0;
};

// Existing GM quests commands/queries; implementations stay in composition.
class GmQuestCommands {
public:
  virtual ~GmQuestCommands() = default;

  virtual bool gm_has_quest_book() const = 0;

  virtual QuestResult gm_plan_quest_acceptance(int quest_id, bool check_conditions,
                                               const WorldProgress &progress,
                                               const QuestWorldView &view) = 0;

  virtual WorldQuestBook::GmCompletion gm_plan_quest_completion(const std::vector<int> &quest_ids,
                                                                const WorldProgress &progress,
                                                                const QuestWorldView &view) = 0;

  virtual const AnnouncedUnit *gm_interaction_npc(std::uint64_t unit_id) const = 0;
};

// Existing GM world commands/queries; implementations stay in composition.
class GmWorldCommands {
public:
  virtual ~GmWorldCommands() = default;

  virtual std::string gm_instance_key() const = 0;

  virtual int gm_session_map_id() const = 0;

  // ClientCatalog's logical reads fill its existing lazy table cache.
  virtual ClientCatalog *gm_client_catalog() const = 0;

  virtual bool gm_tutorial_data_available() const = 0;

  virtual bool gm_in_housing() const = 0;

  // The public field channel that owns an open-world destination (see
  // MapChannels::region_map_for), so a teleport loads that region's channel. nullopt keeps the
  // requested map. `keep_public_channel` keeps a named public field even outside its area.
  virtual std::optional<int>
  gm_region_map([[maybe_unused]] const WorldPosition &destination,
                [[maybe_unused]] bool keep_public_channel) const {
    return std::nullopt;
  }

  // The field a housing area overlays (map 20101 -> 9012); nullopt on every other map.
  // Coordinates read inside a housing area name a spot on that field.
  virtual std::optional<int> gm_housing_area_parent([[maybe_unused]] int map_id) const {
    return std::nullopt;
  }

  virtual bool gm_begin_map_move(const WorldPosition &destination) = 0;

  // Original housing/origin reset and tutorial/shared-map staging after the GM state clears.
  virtual void gm_stage_map_move(const WorldPosition &destination, bool to_tutorial) = 0;

  virtual std::optional<std::vector<GameplayPacket>>
  gm_request_server_teleport(const WorldPosition &destination, std::int64_t ticks) = 0;

  virtual std::optional<GmTarget> gm_find_target(const std::string &name,
                                                 std::int64_t character_id = 0) = 0;

  virtual GmOnlinePlayers gm_online_players() = 0;

  virtual const AnnouncedUnit *gm_find_announced_unit(std::uint64_t unit_id) const = 0;

  virtual UnitAnnounceResult gm_announce_npc(AnnouncedUnit unit) = 0;

  virtual void gm_despawn_unit(std::uint64_t unit_id) = 0;

  virtual bool gm_has_fog_storage() const = 0;

  virtual std::optional<std::size_t> gm_reveal_fog() = 0;
};

// Existing GM mail commands/queries; implementations stay in composition.
class GmMailCommands {
public:
  virtual ~GmMailCommands() = default;

  virtual bool gm_has_mail_storage() const = 0;

  virtual std::optional<MailRecord> gm_deliver_mail_record(const MailRecord &mail) = 0;

  virtual void gm_notify_mail_peer(const MailRecord &mail, const std::shared_ptr<GmPeer> &peer) = 0;
};

// Existing GM dispatch commands/queries; implementations stay in composition.
class GmFeatureDispatch {
public:
  virtual ~GmFeatureDispatch() = default;

  virtual std::optional<std::vector<GameplayPacket>>
  gm_dispatch_event_request(const WorldGameplayRequest &request) = 0;

  virtual std::vector<std::string> gm_event_help_lines() const = 0;

  virtual std::vector<GameplayPacket>
  gm_channel_event_info(const std::optional<WorldPosition> &position) = 0;

  virtual std::optional<std::vector<GameplayPacket>>
  gm_dispatch_expression_request(const WorldGameplayRequest &request) = 0;

  virtual std::optional<std::vector<GameplayPacket>>
  gm_dispatch_housing_request(const WorldGameplayRequest &request) = 0;
};

// Constructor-only bindings. Providers must outlive the GM session and retained work.
// This value owns references only; it performs no lookup, virtual call or scheduling.
struct GmSessionPorts {
  GmSessionAccess &runtime;
  GmCombatCommands &combat;
  GmCharacterCommands &character;
  GmSocialCommands &social;
  GmQuestCommands &quests;
  GmWorldCommands &world;
  GmMailCommands &mail;
  GmFeatureDispatch &dispatch;
};

} // namespace gm
} // namespace chronicles
