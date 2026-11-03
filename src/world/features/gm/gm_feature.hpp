#pragma once

#include "world/core/session_event_bus.hpp"
#include "world/features/gm/gm_diagnostics.hpp"
#include "world/features/gm/gm_session_context.hpp"
#include "world/features/gm/gm_tools.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace chronicles::gm {

class GmSession;

class GmFeature final {
public:
  using ReplyHandler = std::function<void(const ChatIdentity &, const std::string &)>;
  using AnnounceHandler = std::function<std::size_t(const ChatIdentity &, const std::string &)>;

  explicit GmFeature(world_metrics::TimedMutex &protocol_mutex);

  void set_reply_handler(ReplyHandler handler);
  void set_announce_handler(AnnounceHandler handler);
  ReplyHandler reply_handler() const;
  AnnounceHandler announce_handler() const;
  std::unique_ptr<GmSession> create_session(GmSessionPorts ports, SessionEventBus &events);

  // Legacy factory; only reference binding differs from the focused production path.
  std::unique_ptr<GmSession> create_session(GmSessionContext &context, SessionEventBus &events);

private:
  world_metrics::TimedMutex &protocol_mutex_;
  ReplyHandler reply_handler_;
  AnnounceHandler announce_handler_;
};

class GmSession final : public GmPeer {
public:
  GmSession(GmFeature &feature, GmSessionPorts ports, SessionEventBus &events);

  // Keep existing callers while storing only the focused provider references.
  GmSession(GmFeature &feature, GmSessionContext &context, SessionEventBus &events);

  bool god_mode() const;
  bool infinite_stamina() const;
  bool free_movement() const;
  bool invisible() const;
  bool has_spawned_units() const;
  void forget_spawned_units();
  std::optional<std::int64_t> next_watch_tick() const;
  bool watches_due(std::int64_t ticks) const;
  void mark_watches_emitted(std::int64_t ticks);
  void post_frame(Frame frame) override;

  GameplayPacket gm_invincible_attribute(bool enabled) const;

  bool allow_gm_request();

  bool allow_player_car();

  void gm_reply(const std::string &text);

  void sync_gm_combat_protection();

  std::vector<GameplayPacket> debug_watch_packets(const std::optional<WorldPosition> &own_position,
                                                  std::int64_t ticks);

  void post_gm_summon(const WorldPosition &destination, std::string summoner) override;

  bool handle_social_gm(FrameContext &frame);

  bool handle_generic_gm(FrameContext &frame);

private:
  std::vector<GameplayPacket> social_gm_command(const WorldGameplayRequest &request,
                                                const SocialGmParse &parsed);

  x5::ec::EErrorCode execute_social_gm(const SocialGmCommand &command,
                                       std::vector<GameplayPacket> &packets);

  x5::ec::EErrorCode execute_company_gm(const SocialGmCommand &command,
                                        std::vector<GameplayPacket> &packets);

  std::vector<GameplayPacket> gm_command(const WorldGameplayRequest &request);

  std::optional<GameplayPacket> execute_gm_command(const GmCommand &command);

  std::vector<std::uint64_t> gm_command_targets(const gm::UnitTarget &target) const;

  std::vector<GameplayPacket> execute_gm_world_command(const gm::WorldCommandParse &parse);

  std::vector<GameplayPacket> gm_apply_unit_command(const gm::WorldCommand &command,
                                                    std::uint64_t target,
                                                    const WorldPosition &position,
                                                    std::int64_t ticks);

  std::vector<GameplayPacket> gm_move_unit(const gm::WorldCommand &command, std::uint64_t target,
                                           const WorldPosition &position, std::int64_t ticks);

  std::vector<GameplayPacket> gm_change_stat(const gm::WorldCommand &command);

  std::vector<GameplayPacket> gm_accept_quest(const gm::WorldCommand &command,
                                              const WorldPosition &position);

  void gm_send_mail(const gm::WorldCommand &command);

  static const char *on_off(bool enabled);

  std::vector<GameplayPacket> execute_gm_session_command(const gm::SessionParse &parse);

  std::vector<GameplayPacket> gm_suicide(const std::optional<WorldPosition> &position);

  void gm_kick_self(int delay_seconds);

  void gm_announce(const std::string &announcement);

  std::vector<GameplayPacket> execute_gm_diagnostic(const gm::DiagnosticParse &parse);

  static std::string clock_text(double hours);

  std::vector<GameplayPacket> gm_game_time(const gm::GameTimeSetting &setting);

  std::vector<GameplayPacket> gm_regen_stat();

  std::optional<GmWatchedUnit> watched_unit(std::uint64_t unit_id,
                                            const std::optional<WorldPosition> &own_position,
                                            std::int64_t ticks) const;

  std::vector<GameplayPacket> gm_watch(const gm::DiagnosticCommand &command,
                                       const std::optional<WorldPosition> &position);

  void gm_unwatch(const gm::DiagnosticCommand &command);

  std::vector<GameplayPacket> gm_restore_vitals();

  void gm_set_invisible(bool invisible);

  // How a teleport picks its channel. Coordinate teleports follow the public region that holds
  // the destination; `tp_map <id>` without coordinates keeps the named public channel. The
  // whole-world map is always routed to a region because no channel serves it directly.
  enum class TeleportChannel { destination_region, requested_map };

  // The map that `tp` and `up/down/fwd` coordinates refer to: the current map, or the field a
  // housing area overlays. Staying on the housing map would load an empty copy of the area.
  int coordinate_map_id(const WorldPosition &current) const;

  std::vector<GameplayPacket>
  gm_teleport(const WorldPosition &requested, const std::optional<WorldPosition> &current,
              bool announce, TeleportChannel channel = TeleportChannel::destination_region);

  std::vector<GameplayPacket> gm_teleport_to_map(const gm::SessionCommand &command,
                                                 const std::optional<WorldPosition> &position);

  void gm_change_map(const WorldPosition &destination, bool announce);

  void gm_list_players();

  std::vector<GameplayPacket> gm_go_to_player(const std::string &name,
                                              const std::optional<WorldPosition> &position,
                                              std::int64_t character_id = 0);

  void gm_summon_player(const std::string &name, const std::optional<WorldPosition> &position);

  void accept_gm_summon(const WorldPosition &destination, const std::string &summoner);

  std::string npc_faction(int npc_cms_id) const;

  std::vector<GameplayPacket> gm_spawn_npcs(int npc_cms_id, int count,
                                            const std::optional<WorldPosition> &position);

  void gm_reveal_map();

  std::vector<GameplayPacket> gm_complete_quests(const gm::SessionCommand &command,
                                                 const std::optional<WorldPosition> &position);

  std::vector<GameplayPacket> gm_despawn_npcs();

  GmFeature &feature_;
  GmSessionAccess &runtime_;
  GmCombatCommands &combat_;
  GmCharacterCommands &character_;
  GmSocialCommands &social_;
  GmQuestCommands &quests_;
  GmWorldCommands &world_;
  GmMailCommands &mail_;
  GmFeatureDispatch &dispatch_;
  SessionEventBus &events_;
  static constexpr std::int64_t kGmRequestWindowTicks = 10'000'000;
  static constexpr unsigned kMaximumGmRequestsPerWindow = 4;
  std::int64_t gm_window_start_ticks_{};
  unsigned gm_requests_in_window_{};
  static constexpr std::int64_t kPlayerCarCooldownTicks = 5 * 10'000'000;
  std::int64_t last_player_car_ticks_{};
  bool gm_god_{};
  bool gm_infinite_stamina_{};
  bool gm_free_movement_{};
  std::atomic<bool> gm_invisible_{};
  std::vector<std::uint64_t> gm_spawned_units_;
  std::uint32_t gm_spawn_sequence_{};
  WatchList gm_watches_;
};

} // namespace chronicles::gm
