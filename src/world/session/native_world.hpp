#pragma once

#include "world/features/character/character_bootstrap.hpp"
#include "world/features/chat/player_commands.hpp"
#include "world/features/chat/system_notice.hpp"

#include "auth/auth.hpp"
#include "protocol/protocol.hpp"
#include "world/core/world_gameplay_request.hpp"
#include "world/core/world_presence.hpp"
#include "world/features/character/character_stats.hpp"
#include "world/features/events/ranking_repository.hpp"
#include "world/features/fog/fog_repository.hpp"
#include "world/features/housing/housing_bootstrap.hpp"
#include "world/features/inventory/inventory_bootstrap.hpp"
#include "world/features/inventory/inventory_capacity.hpp"
#include "world/features/mail/mail_repository.hpp"
#include "world/features/market/market_repository.hpp"
#include "world/features/trade/trade_bootstrap.hpp"
#include "world/features/tutorial/tutorial.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/thread_pool.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace chronicles {

class ClientCatalog;
class CompanyDialogueIndex;
class CompanyService;
class GatheringData;
class GuildHousingCoordinator;
class GuildService;

namespace housing {
class HousingHub;
} // namespace housing
class LootData;
class ProcessingData;
class NpcPopulation;

inline constexpr auto kMaximumWorldHandshakeDelay = std::chrono::milliseconds(10000);

// Default cap on simultaneous accepted world TCP sessions. Server policy constant: the
// official game/lobby connection ceiling is not recovered from captures. The default
// admits the 500-player realm target (kDefaultWorldCapacity) plus a small margin for
// sockets still closing after a reconnect; that capacity has not been load tested.
// Upper bounds reuse the realm capacity ceiling defined in auth.hpp.
inline constexpr unsigned kDefaultMaximumWorldConnections = 512;

// Validates a --max-world-connections style value: an unsigned decimal in
// [1, kMaximumWorldCapacity]. Throws std::invalid_argument otherwise; the message names
// the bound, not the flag, so non-CLI callers stay neutral.
unsigned parse_world_connection_limit(std::string_view value);

// Realm services shared with the front WebSocket. Each one is optional.
struct WorldSocialServices {
  // Guild invitations, the entry WorldCharGuildNoti and guild unit labels.
  std::shared_ptr<GuildService> guilds;
  // Company membership, contribution and pioneer permits.
  std::shared_ptr<CompanyService> companies;
  // Enables company dialogue functions (PC2S_DialogueFunctionReq). Requires `companies`.
  std::shared_ptr<const CompanyDialogueIndex> company_dialogues;
  // Legacy coordinator retained for test compatibility. The listener uses HousingHub instead.
  std::shared_ptr<GuildHousingCoordinator> guild_housing;
};

struct WorldPacketMetadata {
  std::string family;
  std::string status;
  std::uint32_t sequence{};
  std::size_t protobuf_bytes{};
};

// Application entry/clock packets match the recovered native schemas and capture.
// The listener retains a 32-byte emulator credential fixture. A live client sent
// 100 initial bytes with the 64-character char_token at offset 36. The preceding
// 36 bytes remain opaque; their construction and certificate policy are unresolved.
class WorldHandshake {
public:
  explicit WorldHandshake(Protocol &protocol, bool bootstrap_diagnostic_enabled = false,
                          std::shared_ptr<const CharacterStats> character_stats = {});

  // Tutorial snapshots send their own inventory and money notifications;
  // only non-tutorial entry supplies persisted progress to this handshake.
  // overworld_map names the one non-tutorial map this server may place a session in (the loaded
  // NPC population's map). A character whose PERSISTED position is on that map enters it; a
  // character with a persisted position on any other non-tutorial map is still refused. Client
  // packets never choose the map. instance_map names a followup tutorial instance (map 10117)
  // whose persisted position resumes in that instance.
  bool
  configure_bootstrap_character(Character character,
                                std::optional<WorldProgress> non_tutorial_progress = std::nullopt,
                                std::optional<int> overworld_map = std::nullopt,
                                std::optional<int> instance_map = std::nullopt);

  // The map this handshake places the character on: the tutorial map unless
  // configure_bootstrap_character accepted an overworld map.
  int map_id() const;

  // Replaces the snapshot only after the candidate stats have been computed
  // and the equipment transaction has committed.
  void refresh_items_and_stats(const Character &character, const nlohmann::json *prepared_stats);

  // Aligns the cached character with committed progression (level, EXP and build), so later
  // presence snapshots and player nubs report it.
  void refresh_progression(int level, std::int64_t experience, const CharacterBuild &build);

  // Entry WorldCharPerkNoti body with earned perks and build points. Without it the handshake
  // sends the class's auto-registered perks and zero balances.
  void set_perk_snapshot(nlohmann::json snapshot);

  // Align a persisted death snapshot or completed server-owned revival with
  // the next bootstrap/presence snapshot, without trusting a client move.
  void set_authoritative_player_state(const nlohmann::json &stats, bool dead);
  void set_authoritative_position(const WorldPosition &position);

  // Starts a server-owned map change for a character on a loaded map (a cutscene end point).
  // Returns MapMoveStartNoti and re-arms the MapLoadComplete/StartPlaying handshake.
  // `cut_scene_id` > 0 names the MapMoveCutScene the client plays around the load.
  Frame begin_map_move(const WorldPosition &spawn, int cut_scene_id = 0,
                       bool restore_gliding = false);

  // Marks the bootstrap WorldCharBaseNoti with gm=true. The client then enables its Tilde
  // GM console (AAC_PS2C_WorldCharBaseNoti_Handle). Set only from the authenticated handoff.
  void set_game_master(bool game_master);

  Frame entry_response();

  std::optional<Frame> receive(const Frame &frame, std::int64_t monotonic_ticks,
                               std::int64_t epoch_ticks, WorldPacketMetadata *metadata = nullptr);

  std::optional<Frame> take_pending_notification();

  std::optional<WorldGameplayRequest> take_gameplay_request();

  std::optional<WorldPosition> last_position() const;

  std::optional<WorldPosition> current_position() const;

  std::optional<WorldPresenceSnapshot> presence_snapshot() const;

  std::optional<nlohmann::json> take_last_movement();

  bool exited() const;

  void set_learned_crafts(const std::set<int> &learned_crafts);
  void set_achievement_snapshot(nlohmann::json snapshot);

private:
  void append_shop_adjacent_snapshots();

  Protocol &protocol_;
  bool bootstrap_diagnostic_enabled_{};
  bool map_move_started_{};
  // Server-owned map of this session; the tutorial map unless an overworld map was accepted.
  int map_id_{8000};
  bool map_move_completed_{};
  bool start_playing_completed_{};
  bool restore_gliding_on_map_completion_{};
  character::CharacterBootstrap character_bootstrap_;
  inventory::InventoryBootstrap inventory_bootstrap_;
  housing::HousingBootstrap housing_bootstrap_;
  trade::TradeBootstrap trade_bootstrap_;
  std::set<int> learned_crafts_;
  std::optional<nlohmann::json> achievement_snapshot_;
  std::optional<WorldPosition> last_position_;
  std::optional<nlohmann::json> last_movement_;
  std::uint32_t diagnostic_unit_id_{};
  std::size_t logged_move_requests_{};
  std::optional<std::int64_t> last_move_received_tick_;
  std::deque<Frame> pending_notifications_;
  std::optional<WorldGameplayRequest> gameplay_request_;
  std::uint32_t next_sequence_{};
  bool exited_{};
};

// Operator console view of one world-service connection. Never sent to a game client and
// never contains credential bytes.
struct WorldConnectionSummary {
  std::uint64_t connection_id{};
  std::string remote_address;
  std::int64_t connected_at_unix{};
  // False until the world credential is accepted; identity fields are zero before that.
  bool authenticated{};
  std::int64_t account_id{};
  std::int64_t character_id{};
  bool game_master{};
  // Selected character's name, used by GM goto/summon; empty before map entry data exists.
  std::string character_name;
  std::optional<WorldPosition> position;
};

// Repositories the world session uses directly; character data still goes through
// AuthService. Set once at startup before the listener accepts connections.
struct WorldRepositories {
  std::shared_ptr<MailRepository> mail;
  std::shared_ptr<MarketRepository> market;
  std::shared_ptr<RankingRepository> ranking;
  std::shared_ptr<FogRepository> fog;
};

// Holds exclusive ownership of an offline character while an operator mutation runs.
// An unknown write outcome keeps the character fenced until operator recovery.
class OfflineCharacterReservation {
public:
  virtual ~OfflineCharacterReservation() = default;
  virtual void begin_write() = 0;
  virtual void finish_write() = 0;
};

class NativeWorldListener {
public:
  virtual ~NativeWorldListener() = default;

  virtual unsigned short port() const = 0;

  virtual void stop() = 0;

  // Keep the I/O and worker executors running until retained world owners have drained.
  virtual void shutdown_when_drained(std::function<void()>) {
    throw std::runtime_error("Drained world shutdown is unavailable");
  }

  // Test observation only: this does not run or cancel the queued completion.
  virtual bool shutdown_completion_queued() const noexcept {
    return false;
  }

  virtual bool shutdown_started() const noexcept {
    return false;
  }

  virtual bool shutdown_finished() const noexcept {
    return false;
  }

  // Test observation only: counts close-post attempts during drained shutdown.
  virtual std::size_t shutdown_close_post_attempts() const noexcept {
    return 0;
  }

  // Schedules an authoritative refresh for an online character after effect settlement.
  // An absent character needs no notification; world entry loads the current state.
  virtual void resynchronize_quest_company(std::int64_t) {
    throw std::runtime_error("Quest company refresh is unavailable");
  }

  // Null means admission, close settlement or another operator owns the character.
  // Default implementations fail closed if the listener does not support this fence.
  virtual std::unique_ptr<OfflineCharacterReservation>
  reserve_offline_character(std::int64_t character_id) {
    return {};
  }

  // Operator console: thread-safe snapshot of every live world connection.
  virtual std::vector<WorldConnectionSummary> connections() = 0;

  // Operator console: closes one connection, saving its position as on a normal exit.
  // Returns false when the connection no longer exists.
  virtual bool disconnect(std::uint64_t connection_id) = 0;

  // Operator console: closes every connection of the account; returns how many.
  virtual std::size_t disconnect_account(std::int64_t account_id) = 0;

  // Operator console: sends PS2C_SystemMessageNoti with free text to every character that is
  // in a map. Returns the recipient count. Original-client rendering of free text is unverified.
  virtual std::size_t broadcast_system_notice(const std::string &text, SystemNoticeStyle style) = 0;

  // Installed before the event loop starts. Called once per connection after StartPlaying
  // succeeds, rather than on selection, map loading, or later map transitions.
  virtual void set_world_entry_handler(std::function<void(const ChatIdentity &)> handler) {
  }

  // Installed before the event loop starts. Called once when a connection whose entry handler
  // ran closes, so presence trackers can pair every entry with one departure. The identity's
  // name may be empty.
  virtual void set_world_exit_handler(std::function<void(const ChatIdentity &)> handler) {
  }

  virtual void set_player_command_handler(
      std::function<bool(const ChatIdentity &, const std::string &)> handler) {
  }

  // Characters that should hear `speaker` on the Local chat channel: the speaker (for the
  // sender echo) plus every in-world character that currently sees the speaker's unit.
  // Empty when the speaker has no in-world connection on this realm. Thread-safe.
  virtual std::vector<std::int64_t> local_chat_audience(const ChatIdentity &speaker) {
    return {};
  }

  // Installed before the event loop starts. Delivers a GM command's reply text privately to
  // the game master (the shipping client discards PS2C_DebugText, so chat carries replies).
  virtual void
  set_gm_reply_handler(std::function<void(const ChatIdentity &, const std::string &)> handler) {
  }

  // Installed before the event loop starts. Broadcasts a GM announcement (GmCmd.announce) as a
  // World chat notice to every character of the realm and returns the recipient count.
  virtual void set_gm_announce_handler(
      std::function<std::size_t(const ChatIdentity &, const std::string &)> handler) {
  }

  // Queues the operation on the authenticated character's world worker. Completion carries
  // the actual result; accepting the task does not mean the flag has changed yet.
  virtual bool player_pvp(const ChatIdentity &identity, std::optional<bool> enabled,
                          std::function<void(std::string)> completion) {
    return false;
  }

  // `!mount` / `!dismount`: queues the action on the authenticated character's world worker.
  // Completion carries the reply line; false when the character has no in-world session.
  virtual bool player_mount(const ChatIdentity &identity, PlayerMountAction action,
                            std::function<void(std::string)> completion) {
    return false;
  }

  // `!unstuck`: the world worker chooses and validates the destination.
  virtual bool player_unstuck(const ChatIdentity &identity,
                              std::function<void(std::string)> completion) {
    return false;
  }

  virtual void set_repositories(WorldRepositories repositories) = 0;

  // Installed before the event loop starts. Characters that then enter the overworld below the
  // level cap receive the test kit once (docs/graduation-grant.md). Off until called.
  virtual void enable_graduation_grant() {
  }

  // How long a world entry waits for the same character's previous session to release it
  // (still closing, saving or settling) before the entry is refused. Zero, the default,
  // refuses at once; the hosted server enables a bounded wait because the stock client
  // cannot reconnect after a refused entry.
  virtual void set_entry_admission_wait([[maybe_unused]] std::chrono::milliseconds wait) {
  }

  virtual bool claim_mail(const Character &character, std::int64_t mail_id,
                          std::function<void(MailClaimStatus)> completion) {
    return false;
  }

  virtual std::size_t notify_mail(const MailRecord &mail) {
    return 0;
  }
};

// The caller must keep io_context, workers, and auth alive until the listener and
// its sessions have stopped. Certificate/key loading errors are reported at startup.
// The optional diagnostic delay precedes TLS negotiation and is bounded to 10 seconds.
// tls12_only compares the original client's startup paths after distinct TLS versions.
// credential_diagnostic_enabled compares a rejected initial read with pending handoffs
// and logs only match metadata, never credential contents.
// bootstrap_diagnostic_enabled emits a local tutorial map move after time sync.
// gathering_data enables shared-world gathering for sessions whose current map
// has gathering placements; private tutorial sessions never route to it.
// loot_data enables monster corpse loot for private tutorial combat; without it, defeated
// monsters stay unlootable.
// processing_data enables crafting stations (processing and campfire cooking) for every
// session whose current map contains the targeted station.
// guilds enables guild invitations, the entry WorldCharGuildNoti and guild unit labels; it
// is the same registry the front WebSocket guild requests use.
// housing enables the PC2S housing requests, housing-area travel and site announcements; it is
// the same hub the front WebSocket housing requests use.
// social supplies company rules and dialogue data.
// Enable explicitly; this listener does not yet prove original-client compatibility.
std::shared_ptr<NativeWorldListener> start_native_world(
    boost::asio::io_context &io_context, boost::asio::thread_pool &workers, AuthService &auth,
    const boost::asio::ip::tcp::endpoint &endpoint, const std::filesystem::path &protocol_directory,
    const std::filesystem::path &certificate_file, const std::filesystem::path &private_key_file,
    std::chrono::milliseconds handshake_delay = std::chrono::milliseconds::zero(),
    bool tls12_only = false, bool credential_diagnostic_enabled = false,
    bool bootstrap_diagnostic_enabled = false,
    std::shared_ptr<const TutorialData> tutorial_data = {},
    std::shared_ptr<const CharacterStats> character_stats = {},
    std::shared_ptr<ClientCatalog> client_catalog = {},
    std::shared_ptr<const GatheringData> gathering_data = {},
    std::shared_ptr<const LootData> loot_data = {},
    std::shared_ptr<const ProcessingData> processing_data = {},
    const std::filesystem::path &catalog_root = {},
    std::shared_ptr<const NpcPopulation> npc_population = {},
    std::shared_ptr<GuildService> guilds = {}, std::shared_ptr<housing::HousingHub> housing = {},
    unsigned maximum_connections = kDefaultMaximumWorldConnections,
    WorldSocialServices social = {});

} // namespace chronicles
