#include "world/session/native_world.hpp"
#include "cms.generated.pb.h"
#include "common/catalog_fields.hpp"
#include "common/clock_ticks.hpp"
#include "common/currency.hpp"
#include "common/world_metrics.hpp"
#include "data/catalog_requirements.hpp"
#include "data/client_catalog.hpp"
#include "data/starter_gear.hpp"
#include "ec.generated.pb.h"
#include "net/world_connection.hpp"
#include "net/world_tls_listener.hpp"
#include "p_character.pb.h"
#include "persistence/item_wire.hpp"
#include "world/core/game_rates.hpp"
#include "world/core/instance_copies.hpp"
#include "world/core/map_channels.hpp"
#include "world/core/player_snapshots.hpp"
#include "world/core/progress_packets.hpp"
#include "world/core/retained_owner_supervisor.hpp"
#include "world/core/unit_registry.hpp"
#include "world/core/world_request_opcodes.hpp"
#include "world/core/world_sectors.hpp"
#include "world/features/adventure/adventure.hpp"
#include "world/features/character/character_feature.hpp"
#include "world/features/chat/chat.hpp"
#include "world/features/chat/chat_feature.hpp"
#include "world/features/combat/combat.hpp"
#include "world/features/combat/combat_bootstrap.hpp"
#include "world/features/combat/combat_feature.hpp"
#include "world/features/combat/combat_player_session.hpp"
#include "world/features/companion/companion_feature.hpp"
#include "world/features/company/company.hpp"
#include "world/features/company/company_dialogues.hpp"
#include "world/features/company/company_feature.hpp"
#include "world/features/company/company_world.hpp"
#include "world/features/escape/escape_feature.hpp"
#include "world/features/events/event_gm_commands.hpp"
#include "world/features/events/events_session.hpp"
#include "world/features/events/raid_channel.hpp"
#include "world/features/expression/expression_feature.hpp"
#include "world/features/fog/fog_reveal.hpp"
#include "world/features/gathering/construction_budget.hpp"
#include "world/features/gathering/gathering_feature.hpp"
#include "world/features/gathering/gathering_recovery.hpp"
#include "world/features/gm/gm_feature.hpp"
#include "world/features/guides/contents_guides.hpp"
#include "world/features/guild/guild_feature.hpp"
#include "world/features/guild/guild_replication.hpp"
#include "world/features/guild/guild_world.hpp"
#include "world/features/housing/housing_feature.hpp"
#include "world/features/inventory/inventory_encumbrance.hpp"
#include "world/features/inventory/inventory_feature.hpp"
#include "world/features/inventory/inventory_items.hpp"
#include "world/features/inventory/inventory_rules.hpp"
#include "world/features/inventory/item_polish_catalog.hpp"
#include "world/features/inventory/utility_bag.hpp"
#include "world/features/item_use/item_use_feature.hpp"
#include "world/features/loot/loot_feature.hpp"
#include "world/features/mail/mail_session.hpp"
#include "world/features/market/market_session.hpp"
#include "world/features/npc/npc_feature.hpp"
#include "world/features/npc/npc_patrol.hpp"
#include "world/features/npc/npc_population.hpp"
#include "world/features/npc/npc_sight.hpp"
#include "world/features/nuia/nuia_feature.hpp"
#include "world/features/party/party_feature.hpp"
#include "world/features/party/party_replication.hpp"
#include "world/features/party/party_world.hpp"
#include "world/features/processing/processing_feature.hpp"
#include "world/features/quests/quest_rules.hpp"
#include "world/features/quests/quests_feature.hpp"
#include "world/features/shop/shop_session.hpp"
#include "world/features/trade/trade_feature.hpp"
#include "world/features/travel/map_travel.hpp"
#include "world/features/tutorial/tutorial_player_session.hpp"
#include "world/session/boss_recipient_page.hpp"
#include "world/session/boss_supervised_dispatch.hpp"
#include "world/session/close_party_step.hpp"
#include "world/session/combat_work_schedule.hpp"
#include "world/session/frame_pipeline.hpp"
#include "world/session/gameplay_tick_gate.hpp"
#include "world/session/gathering_view_identity.hpp"
#include "world/session/graduation_grant.hpp"
#include "world/session/local_packet_binding.hpp"
#include "world/session/local_packet_memory.hpp"
#include "world/session/retained_frame_delivery.hpp"
#include "world/session/retained_retry_gate.hpp"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/system/error_code.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace chronicles {
namespace {

namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

#ifdef CHRONICLES_TEST_SHUTDOWN_START_FAULT
std::atomic<bool> shutdown_start_fault_for_testing{};
#endif

#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
std::atomic<bool> companion_remove_fault_armed{};
std::atomic<bool> companion_dismount_inserted{};
std::atomic<bool> companion_remove_fault_observed{};
std::atomic<std::uint64_t> companion_entry_unit_for_testing{};
std::atomic<bool> companion_entry_binding_observed{};
std::atomic<bool> companion_first_frame_fault_armed{};
std::atomic<bool> companion_first_frame_fault_observed{};
std::atomic<bool> companion_entry_fault_armed{};
std::atomic<bool> companion_close_post_fault_armed{};
std::atomic<bool> companion_close_post_fault_observed{};
std::mutex companion_terminal_pause_mutex;
std::condition_variable companion_terminal_pause_condition;
bool companion_terminal_pause_armed{};
bool companion_terminal_pause_entered{};
bool companion_terminal_pause_released{};
std::atomic<std::uint64_t> companion_terminal_pause_recipient{};
std::atomic<bool> companion_terminal_stale_observed{};
std::mutex companion_test_budget_mutex;
std::shared_ptr<companion::CompanionDetachBudget> companion_test_budget;
std::atomic<bool> companion_budget_exhaustion_observed{};
std::atomic<bool> quest_supervisor_post_fault_armed{};
std::atomic<bool> quest_supervisor_post_fault_observed{};
std::atomic<bool> quest_retirement_fault_armed{};
std::atomic<bool> quest_retirement_fault_observed{};
std::atomic<bool> quest_success_forget_fault_armed{};
std::atomic<bool> quest_success_forget_fault_observed{};
std::atomic<bool> recovered_reward_encoding_fault_armed{};
std::atomic<bool> recovered_reward_encoding_fault_observed{};
std::atomic<bool> gameplay_completion_fault_armed{};
std::atomic<bool> gameplay_completion_fault_observed{};
std::atomic<bool> gameplay_completion_fault_unknown{};
std::atomic<bool> frame_worker_post_fault_armed{};
std::atomic<bool> frame_worker_post_fault_observed{};
std::atomic<bool> tick_worker_post_fault_armed{};
std::atomic<bool> tick_worker_post_fault_observed{};
std::atomic<bool> tick_worker_post_timer_observed{};
std::atomic<bool> worker_post_fault_unknown{};
std::atomic<bool> map_transition_log_fault_armed{};
std::atomic<bool> map_transition_log_fault_observed{};
std::atomic<bool> map_transition_io_fault_armed{};
std::atomic<bool> map_transition_io_fault_observed{};
std::atomic<bool> frame_response_io_fault_armed{};
std::atomic<bool> frame_response_io_fault_observed{};
std::atomic<bool> tick_response_io_fault_armed{};
std::atomic<bool> tick_response_io_fault_observed{};
std::atomic<bool> close_hook_retry_fault_armed{};
std::atomic<bool> close_hook_retry_fault_observed{};
std::atomic<bool> close_hook_retry_fault_blocked{};
std::atomic<std::uint64_t> close_hook_retry_fault_connection{};
std::atomic<unsigned> close_hook_party_stage_calls{};
std::atomic<bool> companion_fanout_allocation_fault_armed{};
std::atomic<bool> companion_fanout_allocation_fault_observed{};
std::atomic<bool> companion_fanout_old_view_skipped{};
std::mutex companion_fanout_pause_mutex;
std::condition_variable companion_fanout_pause_condition;
bool companion_fanout_pause_armed{};
bool companion_fanout_pause_entered{};
bool companion_fanout_pause_released{};
std::atomic<bool> companion_transition_fault_armed{};
std::atomic<bool> companion_transition_fault_observed{};
std::atomic<bool> general_transition_fault_armed{};
std::atomic<bool> general_transition_fault_observed{};
std::atomic<bool> general_transition_staged_observed{};

std::shared_ptr<companion::CompanionDetachBudget> current_companion_test_budget() {
  std::lock_guard lock(companion_test_budget_mutex);
  return companion_test_budget;
}
#endif

constexpr std::uint16_t kTimeSyncRequest = 20001;
constexpr std::uint16_t kCharacterExitRequest = 20002;
constexpr std::uint16_t kPingRequest = 20003;
constexpr std::uint16_t kTimeSyncResponse = 1;
constexpr std::uint16_t kPongResponse = 4;
constexpr std::uint16_t kCharacterEntryResponse = 5;
constexpr std::uint16_t kCharacterExitResponse = 6;
constexpr std::uint16_t kStartPlayingRequest = 20086;
constexpr std::uint16_t kMoveRequest = 20087;
constexpr std::uint16_t kMapMoveStartNotification = 175;
constexpr std::uint16_t kMapMoveCompleteNotification = 176;
constexpr std::uint16_t kStartPlayingResponse = 183;
constexpr std::uint16_t kUnitAddNotification = 10;
constexpr std::uint16_t kUnitRemoveNotification = 11;
// A travel the client waits for while the character is busy is retried on gameplay ticks at
// this interval (100 ns ticks: 250 ms) until the blocking action ends, for at most two minutes.
constexpr std::int64_t kDeferredTravelRetryTicks = 2'500'000;
constexpr std::int64_t kDeferredTravelLifetimeTicks = 1'200'000'000;
constexpr std::uint16_t kMoveNotification = 177;

// Shared request IDs come from the one header the frame pipeline and the features also use.
using world_request::kInteractionStartRequest;
using world_request::kMapLoadCompleteRequest;
using world_request::kWorldChatRequest;
using world_request::kZoneChannelMoveRequest;

// Upper bounds for NPC units announced to one session. The tutorial profile caps its NPC spawns
// at 128. A populated map announces at most kMaxPopulatedNpcUnits at once (map 9012 has 752
// active placements); that number is a server memory/announce policy, not a game rule. The
// session registry holds both. kMaxPopulatedNpcUnits lives in npc_population.hpp.
constexpr std::size_t kMaxTutorialAnnouncedUnits = 128;
constexpr std::size_t kMaxAnnouncedUnits = kMaxTutorialAnnouncedUnits + kMaxPopulatedNpcUnits;
// Fight, Wild, Adamant, and Magic share this start in init_character_start.json.
// These Cliffs bosses activate encounter doodads that must survive an ambiguous defeat.
constexpr int kCliffsEncounterMapId = 30104;
constexpr int kCliffsEliteBossNpcId = 5001510;
constexpr int kCliffsCaptainBossNpcId = 5001104;
constexpr int kDiagnosticTutorialMapId = 8000;
constexpr int kDiagnosticSpawnX = -795146;
constexpr int kDiagnosticSpawnY = 712194;
constexpr int kDiagnosticSpawnZ = -698778;
constexpr float kDiagnosticSpawnYaw = 158.56723F;
constexpr std::size_t kMaximumDiagnosticMoveLogs = 8;
// Per-session outbound bound. The map-loaded reply of a populated map queues one opcode-10 frame
// per announced NPC (752 on map 9012) in one burst, so the bound is the ordinary 256 frames plus
// the population cap. A server policy, not an observed official limit.
constexpr std::size_t kMaximumQueuedWorldNotifications = 256 + kMaxPopulatedNpcUnits;
constexpr std::size_t kMaximumQueuedWorldBytes = 1024 * 1024;
// The first 100 client application bytes contain an opaque 36-byte prefix and
// the 64-character char_token at offset 36. The prefix remains unresolved.
constexpr std::size_t kPacketKeyBytes = 32;
constexpr std::size_t kNativeCredentialPrefixBytes = 36;
constexpr std::size_t kNativeCredentialTokenBytes = 64;
constexpr std::size_t kNativeCredentialBytes =
    kNativeCredentialPrefixBytes + kNativeCredentialTokenBytes;


} // namespace

// Parses and range-checks a world connection limit. Shared by the --max-world-connections
// option parser and reachable for direct validation tests.
unsigned parse_world_connection_limit(std::string_view value) {
  unsigned limit{};
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), limit);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || limit == 0 ||
      limit > kMaximumWorldCapacity) {
    throw std::invalid_argument("maximum world connections must be between 1 and " +
                                std::to_string(kMaximumWorldCapacity));
  }
  return limit;
}

namespace {

constexpr auto kWorldProgressCheckpointInterval = std::chrono::seconds(2);
// A character returning from character selection reconnects within a few seconds, while its
// previous world session may still be saving and releasing the character. When the listener
// enables an admission wait (set_entry_admission_wait), the new connection retries at this
// interval instead of being refused: the stock client cannot reconnect after a refused world
// entry (repeated TLS handshake failures until the game restarts).
constexpr auto kAdmissionRetryInterval = std::chrono::milliseconds(100);
using NativeTicks = std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>;

std::int64_t monotonic_ticks() {
  return std::chrono::duration_cast<NativeTicks>(Clock::now().time_since_epoch()).count();
}

std::int64_t epoch_ticks() {
  return std::chrono::duration_cast<NativeTicks>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

constexpr std::uint16_t kQuestUpdateNotification = 27;
constexpr std::uint16_t kQuestAcceptResponse = 28;
constexpr std::uint16_t kQuestAbandonResponse = 29;
constexpr std::uint16_t kGuideInteractUnitResponse = 163;
constexpr std::uint16_t kEquipmentsUpdateNotification = 168;
constexpr std::uint16_t kItemsUpdateNotification = 169;
constexpr std::uint16_t kWeaponSlotActivationNotification = 172;

bool affects_contents_guides(const GameplayPacket &packet) {
  return packet.opcode == kQuestUpdateNotification || packet.opcode == kQuestAcceptResponse ||
         packet.opcode == kQuestAbandonResponse || packet.opcode == kGuideInteractUnitResponse ||
         packet.opcode == kEquipmentsUpdateNotification || packet.opcode == kItemsUpdateNotification ||
         packet.opcode == kWeaponSlotActivationNotification;
}

void copy_present(const nlohmann::json &source, nlohmann::json &target, const char *source_name,
                  const char *target_name) {
  const auto field = source.find(source_name);
  if (field != source.end()) {
    target[target_name] = *field;
  }
}

nlohmann::json diagnostic_spawn_position(const Character &character) {
  const auto &saved = character.world_position;
  return {{"millimeter_x", saved ? saved->x_mm : kDiagnosticSpawnX},
          {"millimeter_y", saved ? saved->y_mm : kDiagnosticSpawnY},
          {"millimeter_z", saved ? saved->z_mm : kDiagnosticSpawnZ}};
}

class WorldSession;

struct QuestReconciliationRegistry {
  // Access is serialized by WorldState::character_admission_mutex.
  std::unordered_map<std::int64_t, std::shared_ptr<WorldSession>> owners;
  std::unordered_map<std::int64_t, std::shared_ptr<WorldSession>> death_owners;
};

struct ClosePresenceBatch {
  std::shared_ptr<RetainedFrameDelivery> delivery;
  std::vector<std::weak_ptr<WorldSession>> recipients;
  std::vector<GatheringViewIdentity> views;
  std::shared_ptr<gathering::ConstructionBudget::Lease> budget;
};

struct CloseCompanionBatch {
  std::shared_ptr<RetainedFrameDelivery> delivery;
  BossDeliveryCompletion completion;
  std::vector<std::weak_ptr<WorldSession>> recipients;
  std::vector<GatheringViewIdentity> views;
  std::vector<Clock::time_point> posted_at;
  std::shared_ptr<gathering::ConstructionBudget::Lease> budget;
  companion::CompanionDetachCursor next;
  bool last{};
};

enum class BossOrigin { frame, tick, retry };

struct LiveBossPublication {
  BossScriptIntent intent;
  std::shared_ptr<InstanceCopy> copy;
  std::shared_ptr<gathering::BossScriptSourceLease> lease;
  std::optional<std::vector<GatheringStateChange>> changes;
  std::shared_ptr<RetainedFrameDelivery> delivery;
  BossDeliveryCompletion completion;
  std::vector<std::weak_ptr<WorldSession>> recipients;
  std::vector<GatheringViewIdentity> views;
  std::vector<Clock::time_point> posted_at;
  std::shared_ptr<gathering::ConstructionBudget::Lease> budget;
  std::uint64_t cursor{};
  std::uint64_t high_water{};
  std::uint64_t page_end{};
  bool sealed{};
  bool last_page{};
  BossOrigin origin{BossOrigin::frame};
  bool origin_bound{};
  bool origin_queued{};
  std::atomic<bool> origin_wake_failed{};
  Clock::time_point created_at{Clock::now()};
  bool oversize{};
  bool published{};
};

struct ClosePresenceTarget {
  WorldPresenceEvent event;
  std::weak_ptr<WorldSession> recipient;
  GatheringViewIdentity view;
};

struct PendingWorldSettlement {
  std::optional<trade::PendingWagonSettlement> wagon;
  std::optional<WorldPosition> position;
};

struct PendingGatheringRecovery {
  std::shared_ptr<GatheringRecoveryOperation> operation;
  bool retry_armed{};
};

struct WorldState {
  WorldState(AuthService &service, const std::filesystem::path &protocol_directory,
             std::chrono::milliseconds diagnostic_handshake_delay,
             bool credential_diagnostic_enabled, bool bootstrap_diagnostic_enabled,
             std::shared_ptr<const TutorialData> tutorial_definitions,
             std::shared_ptr<const CharacterStats> character_definitions,
             std::shared_ptr<ClientCatalog> item_catalog,
             std::shared_ptr<const GatheringData> gathering_definitions,
             std::shared_ptr<const LootData> loot_definitions,
             std::shared_ptr<const ProcessingData> processing_definitions,
             const std::filesystem::path &catalog_root,
             std::shared_ptr<const NpcPopulation> population, asio::io_context &io_context)
      : auth(service), protocol(protocol_directory), handshake_delay(diagnostic_handshake_delay),
        credential_diagnostic_enabled(credential_diagnostic_enabled),
        bootstrap_diagnostic_enabled(bootstrap_diagnostic_enabled),
        tutorial_feature(std::move(tutorial_definitions)), client_catalog(std::move(item_catalog)),
        npc_feature(std::move(population), monotonic_ticks()),
        loot_feature(std::move(loot_definitions)),
        processing_feature(std::move(processing_definitions)),
        character_feature(std::move(character_definitions), client_catalog, shop_feature.catalog()),
        gathering_feature(std::make_shared<gathering::GatheringFeature>(
            io_context, std::move(gathering_definitions))),
        party_feature(std::make_shared<party::PartyFeature>(io_context, client_catalog.get())) {
    combat_feature.load_encounters(tutorial_feature.data().get(), catalog_root);
    if (client_catalog) {
      inventory_feature->load_capacity_and_encumbrance();
      combat_feature.load_movement_rules(*client_catalog);
      inventory_feature->load_polish_catalog();
      escape_feature.load_catalog(*client_catalog);
      expression_feature.load_catalog(*client_catalog);
      if (!catalog_root.empty()) {
        npc_feature.load_static_npcs(catalog_root);
        npc_feature.load_map_populations(catalog_root);
      }
      if (!catalog_root.empty() && client_catalog->has_table("Map")) {
        map_channels = MapChannels::load(catalog_root, client_catalog->rows("Map"));
        spdlog::info("Authored map channel routing: {}", map_channels ? "loaded" : "unavailable");
      }
      sector_grid = WorldSectorGrid::load(*client_catalog);
      if (sector_grid) {
        spdlog::info("Shared-world sector grid loaded: {} maps", sector_grid->map_count());
      }
    }
    combat_feature.load_revival_spawns(static_cast<bool>(tutorial_feature.data()),
                                       client_catalog.get(), catalog_root);
    trade_feature.load(client_catalog.get(), catalog_root);
    quests_feature.load_catalog(catalog_root);
    load_contents_guides(catalog_root);
    nuia_feature.load(catalog_root);
    if (!catalog_root.empty()) {
      const auto adventure_path = catalog_root.parent_path() / "adventure" / "definitions.json";
      if (std::filesystem::is_regular_file(adventure_path)) {
        adventure_catalog = adventure::Catalog::load(adventure_path);
        spdlog::info("Shared adventure profile loaded: {} maps", adventure_catalog->map_count());
      } else {
        spdlog::warn("Shared adventure profile is absent; open-world chests and records are "
                     "unavailable. Export and deploy adventure/definitions.json beside the "
                     "catalog.");
      }
    }
    events_feature->load_channel_event_data(catalog_root);
    load_travel_catalog(catalog_root);
  }

  void load_contents_guides(const std::filesystem::path &catalog_root) {
    if (catalog_root.empty() || !client_catalog || !client_catalog->has_table("Item")) {
      return;
    }
    const auto path = catalog_root.parent_path() / "contents-guides" / "definitions.json";
    if (!std::filesystem::is_regular_file(path)) {
      spdlog::warn("Contents guide profile is absent; contextual guide notifications are disabled");
      return;
    }
    contents_guides = guides::Catalog::load(path);
    auto items = std::make_shared<std::map<int, guides::ItemDescription>>();
    for (const auto &row : client_catalog->rows("Item")) {
      const auto id = catalog_integer(row, "id");
      if (!id || *id <= 0 || *id > std::numeric_limits<int>::max()) {
        continue;
      }
      items->emplace(static_cast<int>(*id),
                     guides::ItemDescription{catalog_text_field(row, "equipmentType"),
                                             catalog_text_field(row, "grade")});
    }
    guide_items = std::move(items);
    spdlog::info("Contents guide profile loaded: {} guides", contents_guides->guides().size());
  }

  // Instances, dungeons and portals (tools/build_map_travel.py). Without the profile, shared
  // world level triggers stay unanswered and only ungated channel boundaries travel.
  void load_travel_catalog(const std::filesystem::path &catalog_root) {
    if (catalog_root.empty()) {
      return;
    }
    const auto travel_path = catalog_root.parent_path() / "travel" / "definitions.json";
    if (!std::filesystem::is_regular_file(travel_path)) {
      spdlog::warn("Map travel profile is absent; instances, dungeons and portals are "
                   "unavailable. Export and deploy travel/definitions.json beside the catalog.");
      return;
    }
    travel_catalog = travel::Catalog::load(travel_path);
    instance_copies.emplace(travel_catalog->copy_maps());
    spdlog::info("Map travel profile loaded: {} maps, {} volumes", travel_catalog->map_count(),
                 travel_catalog->volume_count());
  }

  AuthService &auth;
  Protocol protocol;
  const std::chrono::milliseconds handshake_delay;
  const bool credential_diagnostic_enabled;
  const bool bootstrap_diagnostic_enabled;
  // Admission retries (kAdmissionRetryInterval apart) a world entry may spend waiting for the
  // same character's previous session; zero refuses a busy entry at once.
  std::atomic<int> maximum_admission_retries{};
  tutorial::TutorialFeature tutorial_feature;
  const std::shared_ptr<ClientCatalog> client_catalog;
  std::optional<MapChannels> map_channels;
  // Shared-world sector geometry from OriginMap/Map; without it no SectorUpdateNoti is sent.
  std::optional<WorldSectorGrid> sector_grid;
  nuia::NuiaFeature nuia_feature;
  std::shared_ptr<const adventure::Catalog> adventure_catalog;
  std::shared_ptr<const travel::Catalog> travel_catalog;
  // Separate copies of instance and dungeon maps per party or solo character (loaded with the
  // travel profile, which knows the map types). Absent without the profile: maps stay shared.
  std::optional<InstanceCopies> instance_copies;
  std::shared_ptr<const guides::Catalog> contents_guides;
  std::shared_ptr<const std::map<int, guides::ItemDescription>> guide_items;
  // Stationary NPC population of one map, loaded at startup from --npc-placements (null when the
  // option is absent). Immutable and shared; a session announces it only through
  // NpcPlayerSession::announce_population once it owns that map's identity.
  npc::NpcFeature npc_feature;
  // Monster corpse loot for private tutorial combat; null leaves corpses unlootable.
  loot::LootFeature loot_feature;
  // Loaded crafting-station profile; a missing profile leaves station requests unhandled.
  processing::ProcessingFeature processing_feature;
  quests::QuestsFeature quests_feature;
  // Shared node state, map/sector snapshots and its strand-bound respawn timer.
  const std::shared_ptr<gathering::GatheringFeature> gathering_feature;
  escape::EscapeFeature escape_feature;
  expression::ExpressionFeature expression_feature;
  combat::CombatFeature combat_feature{client_catalog};
  // Merchant indexes are immutable after startup. Members initialize in declaration order,
  // after client_catalog and tutorial_feature.
  shop::ShopFeature shop_feature{client_catalog.get()};
  market::MarketFeature market_feature{client_catalog.get(), shop_feature.catalog()};
  mail::MailFeature mail_feature{shop_feature.catalog()};
  // Progression loads at the same declaration-order point as before.
  character::CharacterFeature character_feature;
  // Shared companion units retain their own lock and never call back into sessions.
  // Missing catalog tables disable the feature at its original startup position.
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
  companion::CompanionFeature companion_feature{client_catalog.get(), shop_feature.catalog(),
                                                current_companion_test_budget()};
#else
  companion::CompanionFeature companion_feature{client_catalog.get(), shop_feature.catalog()};
#endif
  // Draw boxes/effect consumables; disabled without their required catalog tables.
  item_use::ItemUseFeature item_use_feature{client_catalog.get(), shop_feature.catalog()};
  // Utility definitions load here; capacity/weight/polish retain their later startup positions.
  const std::shared_ptr<inventory::InventoryFeature> inventory_feature =
      std::make_shared<inventory::InventoryFeature>(client_catalog);
  // Trade-run rules and trading-post placements (set in the constructor body because they read
  // the raw export beside the catalog). Null disables trade requests.
  trade::TradeFeature trade_feature{client_catalog.get(), shop_feature.catalog()};
  // Party membership spans every connection. PartyWorld has its own lock and never calls
  // back into sessions or the codec. Members initialize after client_catalog.
  const std::shared_ptr<party::PartyFeature> party_feature;
  // Event catalogs initialize at their old declaration-order position. The late optional
  // channel data load remains at the end of this constructor body.
  const std::shared_ptr<events::EventsFeature> events_feature =
      std::make_shared<events::EventsFeature>(party_feature->executor(), client_catalog.get(),
                                              party_feature->world());
  // Rate-limits and validates world-transport chat (party chat is the only audience).
  chat::ChatFeature chat_feature;
  // Feature repositories live in their owners; composition retains only Fog access.
  std::shared_ptr<FogRepository> fog_repository;
  // Guild invitations, entry snapshots and unit labels. Null without a guild service. Set
  // once by the listener before it accepts connections; GuildWorld has its own lock.
  std::shared_ptr<guild::GuildFeature> guild_feature;
  // Housing owner, installed before accepting connections. Its optional Hub is shared with
  // the front adapter and keeps its existing lock.
  std::shared_ptr<housing::HousingFeature> housing_feature;
  std::unique_ptr<company::CompanyFeature> company_feature;
  // DynamicMessageFactory mutates its prototype cache during decode/encode.
  world_metrics::TimedMutex protocol_mutex;
  // Orders brief quest-intent reservations with all members' lethal-event captures. Database
  // transactions and pending-intent resolution never run while this mutex is held.
  std::mutex quest_intent_order_mutex;
  // Wake identities include the population address and its owner generation. Two copies
  // can issue the same generation and must never wake or retire each other's sessions.
  std::mutex combat_order_mutex;
  using CombatWakeKey = std::tuple<std::uintptr_t, std::uint64_t, std::uint64_t>;
  std::map<CombatWakeKey, std::weak_ptr<WorldSession>> combat_order_owners;
  // The callback owner borrows this same mutex, including metric accounting.
  gm::GmFeature gm_feature{protocol_mutex};
  WorldPresence presence;
  std::unordered_map<std::uint64_t, std::weak_ptr<WorldSession>> sessions;
  // Entry reads and close settlement must never overlap for the same character.
  std::mutex character_admission_mutex;
  std::unordered_map<std::int64_t, std::uint64_t> character_admissions;
  std::unordered_map<std::int64_t, std::uint64_t> offline_character_reservations;
  std::unordered_set<std::int64_t> offline_admin_quarantines;
  std::uint64_t next_offline_reservation{1};
  // The listener owns this registry; a weak reference avoids a state/session ownership cycle.
  std::weak_ptr<QuestReconciliationRegistry> quest_reconciliation_registry;
  std::weak_ptr<RetainedOwnerSupervisor> boss_wake_supervisor;
  std::weak_ptr<RetainedOwnerSupervisor> close_wake_supervisor;
  std::atomic<bool> shutting_down{};
  std::atomic<std::size_t> shutdown_close_post_attempts{};
  // In-memory recovery is retained until the same character next passes the durable fence.
  std::unordered_map<std::int64_t, PendingWorldSettlement> pending_world_settlements;
  // Entries own their source reservations and exact immutable plans across disconnects.
  std::unordered_map<std::int64_t, PendingGatheringRecovery> pending_gathering_recoveries;
  // Copy serials are process-local; a fresh random prefix prevents restart aliasing.
  const std::string gathering_process_nonce{random_hex(16)};
  // Effective accept cap; validated to [1, kMaximumWorldCapacity] by start_native_world.
  unsigned maximum_connections{kDefaultMaximumWorldConnections};
  std::atomic<unsigned> active_connections{};
  std::atomic<std::uint64_t> next_connection_id{1};
};

// No callback below captures a WorldSession. The listener-owned state and operation survive
// a disconnected session; the admission mutex is never held during database work.
void schedule_gathering_recovery(std::weak_ptr<WorldState> weak_state,
                                 asio::any_io_executor io_executor,
                                 asio::any_io_executor worker_executor,
                                 std::int64_t character_id,
                                 const std::shared_ptr<GatheringRecoveryOperation> &operation) {
  auto state = weak_state.lock();
  if (!state) {
    return;
  }
  {
    std::lock_guard lock(state->character_admission_mutex);
    const auto entry = state->pending_gathering_recoveries.find(character_id);
    if (entry == state->pending_gathering_recoveries.end() ||
        entry->second.operation != operation || entry->second.retry_armed) {
      return;
    }
    entry->second.retry_armed = true;
  }
  try {
    auto timer = std::make_shared<asio::steady_timer>(io_executor);
    timer->expires_after(std::chrono::seconds(1));
    timer->async_wait([weak_state, io_executor, worker_executor, character_id, operation, timer](
                          const boost::system::error_code &error) {
      const auto current = weak_state.lock();
      if (!current) {
        return;
      }
      {
        std::lock_guard lock(current->character_admission_mutex);
        const auto entry = current->pending_gathering_recoveries.find(character_id);
        if (entry == current->pending_gathering_recoveries.end() ||
            entry->second.operation != operation) {
          return;
        }
        entry->second.retry_armed = false;
      }
      if (error) {
        if (error == asio::error::operation_aborted) {
          // Timer cancellation occurs during shutdown; startup recovery owns committed claims.
          return;
        }
        spdlog::critical("Gathering recovery timer failed for character {}: {}",
                         character_id, error.message());
        std::terminate();
      }
      try {
        asio::post(worker_executor, [weak_state, io_executor, worker_executor, character_id,
                                     operation] {
          try {
            operation->attempt();
          } catch (const std::exception &error) {
            spdlog::warn("Gathering recovery for character {} needs retry: {}", character_id,
                         error.what());
          } catch (...) {
            spdlog::error("Gathering recovery for character {} raised an unknown exception",
                          character_id);
          }
          const auto current = weak_state.lock();
          if (!current) {
            return;
          }
          if (operation->status() == GatheringRecoveryStatus::pending) {
            schedule_gathering_recovery(weak_state, io_executor, worker_executor, character_id,
                                        operation);
            return;
          }
          std::lock_guard lock(current->character_admission_mutex);
          const auto entry = current->pending_gathering_recoveries.find(character_id);
          if (entry != current->pending_gathering_recoveries.end() &&
              entry->second.operation == operation) {
            current->pending_gathering_recoveries.erase(entry);
          }
        });
      } catch (const std::exception &failure) {
        spdlog::critical("Gathering recovery worker dispatch failed for character {}: {}",
                         character_id, failure.what());
        std::terminate();
      }
    });
  } catch (...) {
    // The operation may already have committed. Never keep serving without a retry owner.
    spdlog::critical("Failed to schedule gathering recovery for character {}", character_id);
    std::terminate();
  }
}

// Sends node state changes to every other connection currently on the same map and in the same
// instance copy (`channel`, InstanceCopy::serial; 0 on shared maps).
// Locks the protocol mutex; never call it while holding that lock.
void deliver_gathering_changes(WorldState &state, const std::vector<GatheringStateChange> &changes,
                               std::uint64_t channel, std::uint64_t excluded_connection_id);

// Encodes party packets and fans ChangePartyUnitNoti out to each subject and its
// observers. Frames for `local_connection_id` are appended to `local_frames` so the
// calling session keeps them ordered after its own response; all others are posted.
// Locks the protocol mutex; never call it while holding that lock.
void deliver_party_update(WorldState &state, const party::PartyUpdate &update,
                          std::uint64_t local_connection_id = 0,
                          std::vector<Frame> *local_frames = nullptr);

// Encodes raid channel packets for their addressed connections, like deliver_party_update, and
// asks each evicted member's session to leave the closed raid map on its own worker strand.
// Locks the protocol mutex; never call it while holding that lock.
void deliver_raid_update(WorldState &state, const raid::RaidUpdate &update,
                         std::uint64_t local_connection_id = 0,
                         std::vector<Frame> *local_frames = nullptr);

// Sends packets describing one connection's unit (level or perk changes) to every connection
// that currently sees it. Private tutorial sessions have no observers.
// Locks the protocol mutex; never call it while holding that lock.
void deliver_observer_packets(WorldState &state, std::uint64_t subject_connection_id,
                              const std::vector<GameplayPacket> &packets);

// Encodes companion unit packets (mounts, vehicles, mates) for their addressed connections.
// Frames for `local_connection_id` are appended to `local_frames`; all others are posted.
// Locks the protocol mutex; never call it while holding that lock.
void deliver_companion_update(WorldState &state, const companion::CompanionUpdate &update,
                              std::uint64_t local_connection_id = 0,
                              std::vector<Frame> *local_frames = nullptr);

// Encodes guild packets for the addressed characters' connections and fans
// ChangeGuildUnitNoti out to each subject and its observers, like deliver_party_update.
// Locks the protocol mutex; never call it while holding that lock.
void deliver_guild_update(WorldState &state, const guild_world::GuildWorldUpdate &update,
                          std::uint64_t local_connection_id = 0,
                          std::vector<Frame> *local_frames = nullptr);

class WorldSession : public WorldConnection,
                     public housing::SessionPort,
                     private SessionFrameOperations,
                     private companion::CompanionSessionContext,
                     private trade::TradeSessionContext,
                     private events::EventsSessionContext,
                     private gm::GmSessionAccess,
                     private gm::GmCombatCommands,
                     private gm::GmCharacterCommands,
                     private gm::GmSocialCommands,
                     private gm::GmQuestCommands,
                     private gm::GmWorldCommands,
                     private gm::GmMailCommands,
                     private gm::GmFeatureDispatch,
                     private inventory::InventorySessionContext,
                     private nuia::NuiaSessionContext,
                     private housing::HousingSessionContext,
                     private company::CompanySessionContext,
                     private guild::GuildSessionContext,
                     private party::PartySessionContext,
                     private chat::ChatSessionContext,
                     private expression::ExpressionSessionContext,
                     private escape::EscapeSessionContext,
                     private ProgressAccess,
                     private shop::ShopSessionContext,
                     private market::MarketSessionContext,
                     private mail::MailSessionContext,
                     private item_use::ItemUseSessionContext,
                     private gathering::GatheringSessionContext,
                     private processing::ProcessingSessionContext,
                     private character::CharacterSessionContext,
                     private quests::QuestsSessionContext,
                     private combat::CombatPlayerContext,
                     private loot::LootSessionContext,
                     private tutorial::TutorialSessionContext,
                     private npc::NpcSessionContext {
public:
  WorldSession(tcp::socket socket, std::shared_ptr<ssl::context> tls_context,
               std::shared_ptr<WorldState> state, asio::thread_pool &workers)
      : WorldConnection(std::move(socket), std::move(tls_context),
                        WorldConnectionOptions{
                            state->next_connection_id.fetch_add(1, std::memory_order_relaxed),
                            state->handshake_delay, state->credential_diagnostic_enabled,
                            kMaximumQueuedWorldNotifications, kMaximumQueuedWorldBytes}),
        gameplay_timer_(io_executor()), monster_reward_retry_timer_(io_executor()),
        death_reconciliation_timer_(io_executor()),
        close_presence_retry_timer_(io_executor()),
        state_(std::move(state)),
        worker_strand_(asio::make_strand(workers)),
        handshake_(state_->protocol, state_->bootstrap_diagnostic_enabled,
                   state_->character_feature.stats_model()),
        session_events_(compose_session_events(session_event_observers())),
        tutorial_player_(std::make_unique<tutorial::TutorialPlayerSession>(
            state_->tutorial_feature, static_cast<tutorial::TutorialSessionContext &>(*this))),
        nuia_session_(state_->nuia_feature.create_session(*this, *this)),
        combat_player_(state_->combat_feature.create_session(*this)),
        loot_player_(state_->loot_feature.create_session(*this)),
        inventory_session_(state_->inventory_feature->create_session(*this, *this)),
        npc_player_(std::make_unique<npc::NpcPlayerSession>(
            state_->npc_feature, static_cast<npc::NpcSessionContext &>(*this))),
        housing_session_(state_->housing_feature->create_session(*this, *this)),
        chat_session_(state_->chat_feature.create_session(*this)),
        party_session_(state_->party_feature->create_session(*this)),
        guild_session_(state_->guild_feature->create_session(*this)),
        company_session_(state_->company_feature->create_session(*this)),
        events_session_(state_->events_feature->create_session(*this)),
        character_session_(state_->character_feature.create_session(*this, session_events_)),
        quests_session_(state_->quests_feature.create_session(
            *this, *this, state_->character_feature.progression(), session_events_)),
        gm_session_(state_->gm_feature.create_session(
            gm::GmSessionPorts{.runtime = static_cast<gm::GmSessionAccess &>(*this),
                               .combat = static_cast<gm::GmCombatCommands &>(*this),
                               .character = static_cast<gm::GmCharacterCommands &>(*this),
                               .social = static_cast<gm::GmSocialCommands &>(*this),
                               .quests = static_cast<gm::GmQuestCommands &>(*this),
                               .world = static_cast<gm::GmWorldCommands &>(*this),
                               .mail = static_cast<gm::GmMailCommands &>(*this),
                               .dispatch = static_cast<gm::GmFeatureDispatch &>(*this)},
            session_events_)),
        expression_feature_(state_->expression_feature.create_session(*this)),
        escape_feature_(state_->escape_feature.create_session(*this, io_executor())),
        shop_session_(state_->shop_feature.create_session(*this, *this, session_events_)),
        market_session_(state_->market_feature.create_session(*this, *this, session_events_)),
        mail_session_(state_->mail_feature.create_session(*this, *this, session_events_)),
        frame_pipeline_(*this) {
    ++state_->active_connections;
    inventory_session_->on_connection_open();

    connected_at_unix_ = current_unix_seconds();
  }

  ~WorldSession() {
    --state_->active_connections;
  }

  // housing::SessionPort: packets are encoded here and written after the frames already queued.
  void deliver(std::vector<housing::WorldPacket> packets) override {
    std::vector<Frame> frames;
    try {
      std::lock_guard lock(state_->protocol_mutex);
      for (const auto &packet : packets) {
        frames.push_back(state_->protocol.encode(packet.opcode, packet.message));
      }
    } catch (const std::exception &error) {
      spdlog::warn("Housing notification encoding failed connection={}: {}", connection_id(),
                   error.what());
      return;
    }
    for (auto &frame : frames) {
      post_frame(std::move(frame));
    }
  }

  // housing::SessionPort: runs on the worker strand, which owns the tutorial's progress cache.
  // A closed session still runs the task so a waiting front request completes.
  bool post(std::function<void(const housing::ProgressAccess &)> task) override {
    auto self = shared_session();
    asio::post(worker_strand_, [self, task = std::move(task)] {
      self->housing_session_->run_task(task);
    });
    return true;
  }

  // housing::SessionPort: called on the worker strand.
  void travel(const housing::HousingDestination &destination) override {
    housing_session_->travel(destination);
  }

  // Operator console view of this connection. Requires the caller to hold
  // state_->protocol_mutex (for the handshake position); takes admin_mutex_ after it.
  WorldConnectionSummary admin_summary_locked() const {
    WorldConnectionSummary summary;
    summary.connection_id = connection_id();
    summary.remote_address = remote_address();
    summary.connected_at_unix = connected_at_unix_;
    summary.position = handshake_.current_position();

    std::lock_guard lock(admin_mutex_);
    if (admin_identity_) {
      summary.authenticated = true;
      summary.account_id = admin_identity_->account_id;
      summary.character_id = admin_identity_->character_id;
      summary.game_master = admin_identity_->game_master;
      summary.character_name = admin_identity_->character_name;
    }
    return summary;
  }

  // An authenticated character whose map position is known, i.e. one that can receive
  // world notifications. Requires the caller to hold state_->protocol_mutex.
  bool admin_in_world_locked() const {
    if (!handshake_.current_position()) {
      return false;
    }

    std::lock_guard lock(admin_mutex_);
    return admin_identity_.has_value();
  }

  // Operator kick. close_on_io() saves the position and releases the population lease as usual.
  void admin_disconnect() {
    // A world admission completion may already be queued ahead of the posted close.
    admin_disconnect_requested_.store(true);
    asio::post(io_executor(), [self = shared_session()] {
      spdlog::info("Native world connection={} closed by operator", self->connection_id());
      self->close_on_io();
    });
  }

  void request_shutdown_close() {
    if (closed() || shutdown_close_in_flight_.exchange(true)) {
      return;
    }
    ++state_->shutdown_close_post_attempts;
    admin_disconnect_requested_.store(true);
    try {
      auto self = shared_session();
      const auto result = dispatch_boss_wake(
          [this](auto handler) {
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
            if (companion_close_post_fault_armed.exchange(false)) {
              companion_close_post_fault_observed.store(true);
              throw std::runtime_error("Injected companion close-post failure");
            }
#endif
            asio::post(io_executor(), std::move(handler));
          },
          [self] {
            self->close_on_io();
            self->shutdown_close_in_flight_.store(false);
          },
          [self] {
            self->shutdown_close_in_flight_.store(false);
            spdlog::critical("World shutdown close failed connection={}; retrying",
                             self->connection_id());
          },
          [self] { self->shutdown_close_in_flight_.store(false); });
      if (result == RetainedOwnerSubmit::definitely_not_posted) {
        shutdown_close_in_flight_.store(false);
      }
    } catch (...) {
      shutdown_close_in_flight_.store(false);
    }
  }

  // Keep a retry owner before posting a close that protects a partially changed view.
  void request_supervised_close() noexcept {
    admin_disconnect_requested_.store(true);
    if (!activate_close_supervisor()) {
      try {
        spdlog::critical("Could not activate close recovery connection={}; admission remains fenced",
                         connection_id());
      } catch (...) {
      }
    }
    request_shutdown_close();
  }

  void request_supervised_close_for_view(const GatheringViewIdentity &view) noexcept {
    try {
      if (gathering_view_.seal_for_close(view)) {
        request_supervised_close();
      } else {
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
        if (connection_id() == 2 && companion_fanout_allocation_fault_observed.load()) {
          companion_fanout_old_view_skipped.store(true);
        }
#endif
      }
    } catch (...) {
      try {
        spdlog::critical("Could not seal companion recipient view connection={}", connection_id());
      } catch (...) {
      }
    }
  }

  void resynchronize_quest_company(std::int64_t character_id) {
    asio::post(worker_strand_, [self = shared_session(), character_id] {
      try {
        if (self->closed() || !self->handoff_ ||
            self->handoff_->character_id != character_id) {
          return;
        }
        const auto &service = self->state_->company_feature->service();
        if (!service) {
          return;
        }
        const auto packet = company_packet(company_world::kCompanyUpdateNotification,
                                           service->status(character_id).record);
        Frame frame;
        {
          std::lock_guard lock(self->state_->protocol_mutex);
          frame = self->state_->protocol.encode(packet.opcode, packet.message);
        }
        self->post_frame(std::move(frame));
        self->session_events_.publish(CompanyMembershipChanged{});
      } catch (const std::exception &error) {
        spdlog::error("Company refresh failed connection={}: {}", self->connection_id(),
                      error.what());
        self->admin_disconnect();
      }
    });
  }

  void player_pvp_command(std::optional<bool> requested,
                          std::function<void(std::string)> completion) {
    asio::post(worker_strand_,
               [self = shared_session(), requested, completion = std::move(completion)] {
                 self->combat_player_->player_pvp_command(requested, completion);
               });
  }

  void player_mount_command(PlayerMountAction action, std::function<void(std::string)> completion) {
    asio::post(worker_strand_,
               [self = shared_session(), action, completion = std::move(completion)] {
                 self->run_player_mount_command(action, completion);
               });
  }

  void player_unstuck_command(std::function<void(std::string)> completion) {
    asio::post(worker_strand_, [self = shared_session(), completion = std::move(completion)] {
      self->run_player_unstuck_command(completion);
    });
  }

  void claim_mail(std::int64_t mail_id, std::function<void(MailClaimStatus)> completion) {
    asio::post(worker_strand_,
               [self = shared_session(), mail_id, completion = std::move(completion)] {
                 self->mail_session_->claim(mail_id, completion);
               });
  }

  void run() {
    start();
  }

  RetainedOwnerSubmit submit_supervised_boss_wake(
      RetainedOwnerSupervisor::Completion completion) noexcept {
    try {
      auto shared_completion =
          std::make_shared<RetainedOwnerSupervisor::Completion>(std::move(completion));
      auto self = shared_session();
      const auto result = dispatch_boss_wake(
          [this](auto handler) { asio::post(worker_strand_, std::move(handler)); },
          [self, shared_completion] {
            self->run_monster_reward_retry_body();
            if (self->combat_player_->has_pending_world_defeats() ||
                self->live_boss_publication_) {
              (*shared_completion)(RetainedOwnerOutcome::retry);
              return;
            }
            (*shared_completion)(RetainedOwnerOutcome::settled);
            self->boss_supervisor_registered_ = false;
            if (self->closed()) {
              self->release_settled_character_admission();
            }
          },
          [shared_completion] { (*shared_completion)(RetainedOwnerOutcome::retry); },
          [shared_completion] { (*shared_completion)(RetainedOwnerOutcome::retry); });
      return result;
    } catch (...) {
      return RetainedOwnerSubmit::definitely_not_posted;
    }
  }

  RetainedOwnerSubmit submit_supervised_close_wake(
      RetainedOwnerSupervisor::Completion completion) noexcept {
    try {
      auto shared_completion =
          std::make_shared<RetainedOwnerSupervisor::Completion>(std::move(completion));
      auto self = shared_session();
      const auto result = dispatch_boss_wake(
          [this](auto handler) {
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
            if (quest_supervisor_post_fault_armed.exchange(false)) {
              quest_supervisor_post_fault_observed.store(true);
              throw std::runtime_error("Injected quest supervisor worker-post failure");
            }
#endif
            asio::post(worker_strand_, std::move(handler));
          },
          [self, shared_completion] {
            if (!self->drive_supervised_close()) {
              (*shared_completion)(RetainedOwnerOutcome::retry);
              return;
            }
            (*shared_completion)(RetainedOwnerOutcome::settled);
            self->close_supervisor_completed_ = true;
            self->release_settled_character_admission();
          },
          [shared_completion] { (*shared_completion)(RetainedOwnerOutcome::retry); },
          [shared_completion] { (*shared_completion)(RetainedOwnerOutcome::retry); });
      return result;
    } catch (...) {
      return RetainedOwnerSubmit::definitely_not_posted;
    }
  }

private:
  std::shared_ptr<WorldSession> shared_session() {
    return std::static_pointer_cast<WorldSession>(WorldConnection::shared_from_this());
  }

  bool acquire_character_admission(const Handoff &handoff) {
    const auto registry = state_->quest_reconciliation_registry.lock();
    if (!registry) {
      return false;
    }
    auto owner = shared_session();
    std::lock_guard lock(state_->character_admission_mutex);
    if (closed() || state_->shutting_down.load() ||
        state_->pending_gathering_recoveries.contains(handoff.character_id) ||
        state_->offline_admin_quarantines.contains(handoff.character_id) ||
        state_->offline_character_reservations.contains(handoff.character_id)) {
      return false;
    }
    const auto [entry, inserted] = state_->character_admissions.emplace(handoff.character_id,
                                                                        connection_id());
    if (!inserted) {
      return false;
    }
    try {
      if (!registry->death_owners.emplace(handoff.character_id, std::move(owner)).second) {
        state_->character_admissions.erase(entry);
        return false;
      }
      const auto supervisor = state_->close_wake_supervisor.lock();
      if (!supervisor || !supervisor->reserve_owner({handoff.character_id, connection_id()})) {
        registry->death_owners.erase(handoff.character_id);
        state_->character_admissions.erase(entry);
        return false;
      }
      admitted_character_ = handoff.character_id;
      return true;
    } catch (...) {
      registry->death_owners.erase(handoff.character_id);
      state_->character_admissions.erase(entry);
      return false;
    }
  }

  bool retain_close_presence_owner() noexcept {
    try {
      std::lock_guard lock(state_->character_admission_mutex);
      if (!admitted_character_) {
        return true;
      }
      const auto registry = state_->quest_reconciliation_registry.lock();
      if (!registry) {
        return false;
      }
      const auto admission = state_->character_admissions.find(*admitted_character_);
      if (admission == state_->character_admissions.end() ||
          admission->second != connection_id()) {
        return false;
      }
      const auto owner = registry->death_owners.find(*admitted_character_);
      return owner != registry->death_owners.end() && owner->second.get() == this;
    } catch (...) {
      return false;
    }
  }

  bool activate_close_supervisor() noexcept {
    try {
      std::lock_guard lock(state_->character_admission_mutex);
      if (!admitted_character_) {
        return true;
      }
      const auto supervisor = state_->close_wake_supervisor.lock();
      const auto registry = state_->quest_reconciliation_registry.lock();
      if (!supervisor || !registry) {
        return false;
      }
      const auto owner = registry->death_owners.find(*admitted_character_);
      if (owner == registry->death_owners.end() || owner->second.get() != this) {
        return false;
      }
      return supervisor->activate_owner({*admitted_character_, connection_id()});
    } catch (...) {
      return false;
    }
  }

  bool drive_supervised_close() {
    const ScopedCombatTick close_tick(
        admitted_combat_tick_, ordered_close_ready_.load()
                                   ? std::optional<std::int64_t>(ordered_dispatch_tick_.load())
                                   : std::nullopt);
    if (ordered_close_pending_.load() && !ordered_close_submitted_.load()) {
      if (!ordered_close_posted_.exchange(true)) {
        try {
          asio::post(io_executor(), [self = shared_session()] {
            self->try_submit_ordered_close();
            self->ordered_close_posted_.store(false);
          });
        } catch (...) {
          ordered_close_posted_.store(false);
        }
      }
      return false;
    }
    if (ordered_close_pending_.load() && !ordered_close_ready_.load()) {
      if (!ordered_worker_failed_.load()) {
        return false;
      }
      // An exception never replays the claimed producer. Settle its durable outcomes,
      // then detach at that same logical tick before releasing the failed claim.
      const ScopedCombatTick combat_tick(admitted_combat_tick_, ordered_dispatch_tick_.load());
      if (quests_session_->has_pending_combat_events()) {
        reconcile_ambiguous_quest_mutation();
      }
      if (has_unsettled_combat_quest_events()) {
        return false;
      }
      std::vector<GameplayPacket> departed_packets;
      combat_player_->retry_pending_world_monster_rewards(departed_packets);
      if (combat_player_->has_pending_world_defeats() || boss_supervisor_registered_) {
        return false;
      }
      persist_player_death(departed_packets);
      if (combat_player_->has_pending_death()) {
        reconcile_ambiguous_death();
        if (!death_recovery_confirmed_) {
          return false;
        }
      }
      if (!ordered_failed_combat_detached_) {
        combat_player_->detach_world_mobs();
        ordered_failed_combat_detached_ = true;
      }
      if (!ordered_close_posted_.exchange(true)) {
        try {
          asio::post(io_executor(), [self = shared_session()] {
            try {
              if (self->combat_schedule_ && self->combat_schedule_->active()) {
                self->combat_schedule_->complete(std::nullopt);
              }
              self->ordered_worker_failed_.store(false);
              self->ordered_close_posted_.store(false);
              self->pump_ordered_combat();
            } catch (...) {
              self->ordered_close_posted_.store(false);
            }
          });
        } catch (...) {
          ordered_close_posted_.store(false);
        }
      }
      return false;
    }
    if (!closed()) {
      request_shutdown_close();
      return false;
    }
    if (!close_hook_finished_.load()) {
      return false;
    }
    if (!close_hook_succeeded_.load()) {
      if (close_hook_uncertain_.load()) {
        if (!close_hook_failure_reported_) {
          close_hook_failure_reported_ = true;
          try {
            spdlog::critical("Close hook outcome is uncertain connection={}; "
                             "admission remains fenced for operator recovery", connection_id());
          } catch (...) {
          }
        }
        return false;
      }
      if (!close_hook_retry_in_flight_.exchange(true)) {
        try {
          asio::post(io_executor(), [self = shared_session()] {
            self->run_close_hook_stages();
            self->close_hook_retry_in_flight_.store(false);
          });
        } catch (...) {
          close_hook_retry_in_flight_.store(false);
        }
      }
      return false;
    }
    const auto chat_departure = chat_session_->departure_status();
    if (chat_departure != chat::ChatSession::DepartureStatus::complete) {
      if (chat_departure == chat::ChatSession::DepartureStatus::uncertain &&
          !close_hook_failure_reported_) {
        close_hook_failure_reported_ = true;
        try {
          spdlog::critical("Chat departure is uncertain connection={}; "
                           "admission remains fenced for operator recovery", connection_id());
        } catch (...) {
        }
      }
      return false;
    }
    if (quests_session_->has_pending_combat_events()) {
      reconcile_ambiguous_quest_mutation();
      if (quests_session_->has_pending_combat_events()) {
        return false;
      }
    }
    if (combat_player_->has_pending_world_defeats() || boss_supervisor_registered_) {
      combat_schedule_monster_reward_retry();
      return false;
    }
    if (combat_player_->has_pending_death()) {
      if (!death_recovery_confirmed_) {
        reconcile_ambiguous_death();
      }
      if (!death_recovery_confirmed_) {
        return false;
      }
    } else if (pending_death_owner_) {
      return false;
    }
    if (pending_quest_reconciliation_) {
      reconcile_ambiguous_quest_mutation();
      if (pending_quest_reconciliation_) {
        return false;
      }
    }
    if (has_unsettled_combat_quest_events()) {
      // Capture failure before a frozen plan exists is unresolved, not a safe no-op.
      // Keep the actual producer and admission fence available for operator recovery.
      return false;
    }
    if (!close_combat_detached_) {
      combat_player_->detach_world_mobs();
      close_combat_detached_ = true;
    }
    if (!close_settlement_ready_) {
      settle_close_world_once();
    }
    if (pending_quest_reconciliation_ || has_unsettled_combat_quest_events() ||
        !close_settlement_ready_) {
      return false;
    }
    if (!drive_close_companion_delivery()) {
      return false;
    }
    bool presence_complete = false;
    {
      std::lock_guard lock(close_presence_mutex_);
      presence_complete = close_presence_complete_;
    }
    if (!presence_complete) {
      drive_close_presence_batch();
      std::lock_guard lock(close_presence_mutex_);
      presence_complete = close_presence_complete_;
    }
    if (!presence_complete) {
      return false;
    }
    // The old view has departed and the population is detached. A new admission
    // loads the fenced durable death state after the ordered close is retired.
    if (combat_player_->has_pending_death() &&
        !combat_player_->acknowledge_reconciled_death()) {
      return false;
    }
    if (ordered_close_pending_.load()) {
      if (!ordered_close_posted_.exchange(true)) {
        try {
          asio::post(io_executor(), [self = shared_session()] {
            try {
              self->combat_schedule_->complete(std::nullopt);
              self->retire_combat_schedule();
              self->ordered_close_pending_.store(false);
            } catch (...) {
              self->ordered_close_posted_.store(false);
            }
          });
        } catch (...) {
          ordered_close_posted_.store(false);
        }
      }
      return false;
    }
    return true;
  }

  void release_character_admission() {
    std::lock_guard lock(state_->character_admission_mutex);
    if (!admitted_character_) {
      return;
    }
    const auto registry = state_->quest_reconciliation_registry.lock();
    if (!registry) {
      return;
    }
    const auto retained = registry->death_owners.find(*admitted_character_);
    if (retained == registry->death_owners.end() || retained->second.get() != this) {
      return;
    }
    const auto entry = state_->character_admissions.find(*admitted_character_);
    if (entry != state_->character_admissions.end() && entry->second == connection_id()) {
      state_->character_admissions.erase(entry);
    }
    registry->death_owners.erase(retained);
    admitted_character_.reset();
  }

  // Close settlement and pending shared defeats both belong to the old admission.
  // Letting a new session enter while either is unresolved creates two owners of the
  // same character and can consume rewards before the old encounter is finalized.
  void release_settled_character_admission() {
    if (!close_settlement_ready_ || !close_supervisor_completed_ || !close_combat_detached_ ||
        boss_supervisor_registered_ || combat_player_->has_pending_world_defeats() ||
        combat_player_->has_pending_death() || pending_quest_reconciliation_ ||
        has_unsettled_combat_quest_events() || pending_death_owner_) {
      return;
    }
    {
      std::lock_guard lock(close_presence_mutex_);
      if (!close_presence_complete_) {
        return;
      }
    }

    release_character_admission();
  }

  bool reconcile_pending_settlement(const Handoff &handoff) {
    std::optional<PendingWorldSettlement> pending;
    {
      std::lock_guard lock(state_->character_admission_mutex);
      const auto entry = state_->pending_world_settlements.find(handoff.character_id);
      if (entry == state_->pending_world_settlements.end()) {
        return true;
      }
      pending = entry->second;
    }

    try {
      HandoffProgressAccess progress(state_->auth, handoff);
      if (pending->wagon) {
        const auto result = trade::reconcile_wagon_settlement(progress, *pending->wagon);
        if (result == trade::WagonSettlementResult::retry) {
          return false;
        }
      } else if (pending->position) {
        const auto before = progress.load();
        if (before.revision == std::numeric_limits<std::int64_t>::max()) {
          return false;
        }
        auto after = before;
        ++after.revision;
        if (!progress.commit_with_position(before, after, *pending->position)) {
          return false;
        }
      }
    } catch (const std::exception &error) {
      spdlog::warn("World close settlement needs retry connection={}: {}", connection_id(),
                   error.what());
      return false;
    }

    std::lock_guard lock(state_->character_admission_mutex);
    state_->pending_world_settlements.erase(handoff.character_id);
    return true;
  }

  void on_transport_ready() override {
    std::lock_guard lock(state_->protocol_mutex);
    state_->sessions.emplace(connection_id(), shared_session());
  }

  void on_initial_credential() override {
    authenticate();
  }

  void on_native_credential_tail() override {
    if (state_->credential_diagnostic_enabled) {
      log_native_credential_structure();
    }
    authenticate_world_token();
  }

  void on_frame(Frame frame) override {
    incoming_ = std::move(frame);
    try {
      if (combat_schedule_) {
        combat_schedule_->submit_input(monotonic_ticks());
        pump_ordered_combat();
      } else {
        process_frame();
      }
    } catch (...) {
      if (combat_schedule_ && combat_schedule_->active()) {
        ordered_worker_failed_.store(true);
      }
      request_supervised_close();
    }
  }

  void on_frame_started(std::uint16_t opcode) override {
    if (state_->bootstrap_diagnostic_enabled && opcode == kMapMoveStartNotification) {
      spdlog::info("World bootstrap diagnostic connection={} map_move_start map_id={}",
                   connection_id(), kDiagnosticTutorialMapId);
    }
  }

  void on_frame_written(std::uint16_t opcode) override {
    if (handshake_.exited()) {
      close_on_io();
      return;
    }
    if (opcode == kMapMoveCompleteNotification && !presence_published_) {
      publish_presence();
    }
  }

  void authenticate() {
    auto self = shared_session();
    asio::post(worker_strand_, [self] {
      std::optional<Handoff> handoff;
      Frame entry_response;
      bool authentication_failed{};
      bool admission_busy{};
      try {
        const auto packet_key =
            std::string_view(self->credential_prefix().data(), self->credential_prefix().size());
        handoff = self->state_->auth.consume_packet_key(
            packet_key, [self](const Handoff &candidate) {
              return self->acquire_character_admission(candidate);
            }, &admission_busy);
        if (!handoff && !admission_busy && self->state_->credential_diagnostic_enabled) {
          self->log_credential_diagnostic();
        }
        if (handoff) {
          self->log_admission_after_wait();
          if (!self->state_->auth.fence_world_entry(*handoff)) {
            throw std::runtime_error("World entry persistence fence was refused");
          }
          if (!self->reconcile_pending_settlement(*handoff)) {
            throw std::runtime_error("World entry has an unresolved previous settlement");
          }
          entry_response = self->prepare_entry_response(*handoff);
          self->publish_admin_identity(*handoff);
          if (!self->state_->auth.prepare_world_admission(*handoff)) {
            throw std::runtime_error("Login authority refused the prepared world admission");
          }
        }
      } catch (const std::exception &error) {
        spdlog::error("Native world authentication failed: {}", error.what());
        authentication_failed = true;
        admission_busy = false;
      }

      // The credential stays intact while the retry owns it; no read reuses the buffers.
      if (admission_busy && self->schedule_admission_retry([self] { self->authenticate(); })) {
        return;
      }
      if (admission_busy) {
        authentication_failed = true;
      }
      self->clear_initial_credential();

      asio::post(self->io_executor(),
                 [self, handoff = std::move(handoff), response = std::move(entry_response),
                  authentication_failed]() mutable {
                   if (authentication_failed) {
                     self->close_on_io();
                     return;
                   }
                   if (!handoff) {
                     self->read_native_credential_tail();
                     return;
                   }

                   self->finish_authentication(std::move(handoff), std::move(response));
                 });
    });
  }

  // Worker strand. Retries a world entry whose character is still held by its previous
  // session (closing, saving or settling) after kAdmissionRetryInterval. Returns false when
  // the connection closed, the server is stopping or the wait is exhausted; the caller then
  // refuses the entry as before.
  bool schedule_admission_retry(std::function<void()> retry) {
    if (closed() || state_->shutting_down.load()) {
      return false;
    }
    if (admission_retries_ >= state_->maximum_admission_retries.load()) {
      if (admission_retries_ > 0) {
        spdlog::warn("World entry refused connection={}: the character's previous session "
                     "did not release it within {} ms",
                     connection_id(), admission_wait_ms());
      }
      return false;
    }
    if (admission_retries_ == 0) {
      spdlog::info("World entry connection={} waits for the character's previous session to "
                   "close",
                   connection_id());
    }
    ++admission_retries_;

    try {
      auto timer = std::make_shared<asio::steady_timer>(io_executor());
      timer->expires_after(kAdmissionRetryInterval);
      timer->async_wait([self = shared_session(), timer, retry = std::move(retry)](
                            const boost::system::error_code &error) {
        if (error || self->closed()) {
          return;
        }
        retry();
      });
    } catch (const std::exception &error) {
      spdlog::error("World entry retry could not be scheduled connection={}: {}",
                    connection_id(), error.what());
      return false;
    }
    return true;
  }

  long long admission_wait_ms() const {
    return static_cast<long long>(admission_retries_) *
           std::chrono::duration_cast<std::chrono::milliseconds>(kAdmissionRetryInterval).count();
  }

  void log_admission_after_wait() const {
    if (admission_retries_ > 0) {
      spdlog::info("World entry connection={} admitted after waiting about {} ms for the "
                   "previous session",
                   connection_id(), admission_wait_ms());
    }
  }

  // The transport reads the credential in two parts and this session slices them: the first
  // 32 bytes are the packet key and the rest of the 100-byte sample holds the 64-byte token
  // after the unresolved prefix. A one-sided change would silently slice the wrong bytes.
  static_assert(kInitialCredentialBytes == kPacketKeyBytes);
  static_assert(kNativeCredentialTailBytes == kNativeCredentialBytes - kPacketKeyBytes);
  static_assert(kMaximumCredentialSampleBytes == kNativeCredentialBytes);
  static_assert(kNativeCredentialTailBytes - (kNativeCredentialPrefixBytes - kPacketKeyBytes) ==
                kNativeCredentialTokenBytes);

  void log_native_credential_structure() const {
    const auto field_at_offset_32 =
        static_cast<std::uint32_t>(static_cast<unsigned char>(credential_tail()[0])) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(credential_tail()[1])) << 8) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(credential_tail()[2])) << 16) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(credential_tail()[3])) << 24);

    spdlog::info("World credential structure connection={} bytes={} "
                 "field_at_32_is_token_length={}",
                 connection_id(), kNativeCredentialBytes,
                 field_at_offset_32 == kNativeCredentialTokenBytes);
  }

  void authenticate_world_token() {
    auto self = shared_session();
    asio::post(worker_strand_, [self] {
      std::optional<Handoff> handoff;
      Frame entry_response;
      bool admission_busy{};
      try {
        const auto token = std::string_view(self->credential_tail().data() +
                                                (kNativeCredentialPrefixBytes - kPacketKeyBytes),
                                            kNativeCredentialTokenBytes);
        handoff = self->state_->auth.consume_world_token(
            token, [self](const Handoff &candidate) {
              return self->acquire_character_admission(candidate);
            }, &admission_busy);
        if (handoff) {
          self->log_admission_after_wait();
          if (!self->state_->auth.fence_world_entry(*handoff)) {
            throw std::runtime_error("World entry persistence fence was refused");
          }
          if (!self->reconcile_pending_settlement(*handoff)) {
            throw std::runtime_error("World entry has an unresolved previous settlement");
          }
          entry_response = self->prepare_entry_response(*handoff);
          self->publish_admin_identity(*handoff);
          if (!self->state_->auth.prepare_world_admission(*handoff)) {
            throw std::runtime_error("Login authority refused the prepared world admission");
          }
        }
      } catch (const std::exception &error) {
        spdlog::error("Native world token authentication failed: {}", error.what());
        // Never admit a handoff whose entry preparation failed; the completion closes instead.
        handoff.reset();
        admission_busy = false;
      }

      // The token stays intact while the retry owns it; no read reuses the buffer.
      if (admission_busy &&
          self->schedule_admission_retry([self] { self->authenticate_world_token(); })) {
        return;
      }
      self->clear_native_credential_tail();

      asio::post(self->io_executor(), [self, handoff = std::move(handoff),
                                       response = std::move(entry_response)]() mutable {
        self->finish_authentication(std::move(handoff), std::move(response));
      });
    });
  }

  Frame prepare_entry_response(const Handoff &handoff) {
    std::optional<Character> character;
    if (state_->bootstrap_diagnostic_enabled) {
      character = state_->auth.world_character(handoff);
    }
    if (character && state_->character_feature.progression()) {
      reconcile_entry_progression(handoff, *character);
    }

    // A character saved inside the followup instance (map 10117) resumes there.
    std::shared_ptr<const TutorialData> followup_data;
    if (character && state_->tutorial_feature.data() && character->world_position) {
      followup_data = state_->tutorial_feature.data()->followup(character->world_position->map_id);
    }

    if (character && state_->tutorial_feature.data() &&
        (!character->world_position ||
         character->world_position->map_id == kDiagnosticTutorialMapId || followup_data)) {
      create_tutorial_sessions(*character, handoff, followup_data);
    }
    std::optional<WorldProgress> non_tutorial_progress;
    if (character && !tutorial_player_->current_instance()) {
      // A character that is already in the overworld has finished the tutorial.
      apply_entry_graduation(handoff, *character);
      non_tutorial_progress = state_->auth.world_progress(handoff);
      quest_intent_ledger_ = std::make_shared<quests::QuestIntentLedger>(*non_tutorial_progress);
      create_gathering_session(*character, handoff);
      create_companion_sessions(*character, handoff);
      create_world_quests(character->id);
    }
    if (character) {
      const auto progress = tutorial_player_->current_instance()
                                ? tutorial_player_->current_instance()->progress()
                                : *non_tutorial_progress;
      ensure_player_vitals(*character, progress);
    }
    if (character) {
      create_processing_session(*character, handoff);
      create_item_use_session(handoff);
      party_session_->select_character(*character, non_tutorial_progress);
      party_session_->set_quest_ledger(quest_intent_ledger_);
      if (tutorial_player_->current_instance()) {
        company_session_->on_select_character(tutorial_player_->current_instance()->progress());
      }
    }

    // WorldCharPerkNoti carries the earned perks and both build point balances.
    std::optional<nlohmann::json> perk_snapshot;
    if (character && state_->character_feature.progression()) {
      const auto &entry_progress = tutorial_player_->current_instance()
                                       ? tutorial_player_->current_instance()->progress()
                                       : *non_tutorial_progress;
      perk_snapshot = state_->character_feature.entry_perks(*character, entry_progress);
      if (state_->character_feature.achievements()) {
        handshake_.set_achievement_snapshot(character::achievement_snapshot(entry_progress));
      }
    }

    // Resolved before the codec lock: it reads the housing service.
    const auto housing_entry =
        character ? housing_session_->entry_destination(*character) : std::nullopt;
    // Saved volume doodad phases return with an instance, dungeon or point of interest the
    // character resumes in; configure_bootstrap_character consumes the progress below.
    const auto saved_doodad_phases = non_tutorial_progress
                                         ? non_tutorial_progress->doodad_phases
                                         : std::map<std::uint64_t, int>{};

    Frame response;
    std::optional<int> resumed_map;
    {
      std::lock_guard lock(state_->protocol_mutex);
      configure_entry(handoff, character, non_tutorial_progress, followup_data, housing_entry,
                      perk_snapshot);
      if (character && !housing_entry && !tutorial_player_->current_instance() &&
          !followup_data && handshake_.map_id() != kDiagnosticTutorialMapId) {
        resumed_map = handshake_.map_id();
      }
      response = handshake_.entry_response();
    }

    // The resumed map's copy and saved doodads are in place before the client loads the map.
    if (resumed_map) {
      select_instance_copy(handoff.character_id, *resumed_map);
      try {
        restore_volume_doodads(*resumed_map, saved_doodad_phases);
      } catch (const std::exception &error) {
        spdlog::error("Volume doodad restore failed connection={}: {}", connection_id(),
                      error.what());
      }
    }
    return response;
  }

  // The bootstrap character, its entry map and the map-bound services of the entry response.
  // Requires state_->protocol_mutex.
  void configure_entry(const Handoff &handoff, std::optional<Character> &character,
                       std::optional<WorldProgress> &non_tutorial_progress,
                       const std::shared_ptr<const TutorialData> &followup_data,
                       const std::optional<housing::HousingDestination> &housing_entry,
                       std::optional<nlohmann::json> &perk_snapshot) {
    handshake_.set_game_master(handoff.game_master);
    if (perk_snapshot) {
      handshake_.set_perk_snapshot(std::move(*perk_snapshot));
    }
    if (character) {
      const int selected_class = character->class_id;
      const std::string saved_map =
          character->world_position ? std::to_string(character->world_position->map_id) : "none";
      // Reconnects may resume a catalog-backed channel reached through an authored
      // boundary. A channel's eligibility does not imply that it has an NPC population.
      auto overworld_map = state_->npc_feature.population()
                               ? std::optional<int>(state_->npc_feature.population()->map_id())
                               : std::nullopt;
      if (housing_entry) {
        overworld_map = housing_entry->map_id;
      } else if (character->world_position &&
                 ((state_->map_channels &&
                   state_->map_channels->supports_map(character->world_position->map_id)) ||
                  (state_->travel_catalog &&
                   state_->travel_catalog->resumable(character->world_position->map_id)))) {
        // Instances, dungeons and points of interest retain the character on disconnect
        // (Map.retainOnDisconnect), so a reconnect resumes inside them with their content.
        overworld_map = character->world_position->map_id;
      }
      const bool configured = handshake_.configure_bootstrap_character(
          std::move(*character), std::move(non_tutorial_progress), overworld_map,
          followup_data ? std::optional<int>(followup_data->map_id()) : std::nullopt);
      if (configured && tutorial_player_->current_instance()) {
        handshake_.set_learned_crafts(
            tutorial_player_->current_instance()->progress().learned_crafts);
      }
      if (configured && housing_entry) {
        housing_session_->stage_entry(*housing_entry);
      } else if (configured && handshake_.map_id() != kDiagnosticTutorialMapId && !followup_data) {
        const bool populated = state_->npc_feature.population_for(handshake_.map_id()) != nullptr;
        npc_player_->set_population_map(populated ? std::optional<int>(handshake_.map_id())
                                                  : std::nullopt);
      }
      if (!configured) {
        spdlog::warn("World bootstrap diagnostic skipped: class={} saved_map={} is not "
                     "supported by the map 8000 tutorial start",
                     selected_class, saved_map);
      } else if (combat_player_->has_simulation()) {
        synchronize_player_state();
      }
    }
  }

  void reconcile_entry_progression(const Handoff &handoff, Character &character) {
    CharacterEntryContext context(state_->auth, handoff, {});
    state_->character_feature.reconcile_entry(context, character);
  }

  // Test-kit grant for a character that enters the overworld below the level cap
  // (docs/graduation-grant.md). Runs before the entry snapshots are built, so the client
  // receives the finished state like any other login; nothing is sent from here.
  void apply_entry_graduation(const Handoff &handoff, Character &character) {
    if (!state_->character_feature.has_graduation() || !state_->character_feature.progression() ||
        !state_->shop_feature.catalog()) {
      return;
    }
    const auto services = state_->character_feature.graduation_services();
    CharacterEntryContext context(state_->auth, handoff,
                                  {services.shop_catalog, services.progression,
                                   state_->quests_feature.catalog(), services.loadout});
    state_->character_feature.apply_entry_graduation(context, character);
  }
  // Item skills spend stackables from the tutorial session's committed progress, so a spent
  // item and later tutorial commits share one revision chain.
  // Creates the private tutorial instance for a character on map 8000 or a followup tutorial
  // map: at entry, and again when the character returns from a housing area. Tutorial combat
  // (and its loot) owns the map 8000 monster spawns; a followup instance has none.
  void reset_movement_vitals() {
    combat_player_->reset_movement_vitals();
  }

  void ensure_player_vitals(const Character &character, const WorldProgress &progress) {
    combat_player_->ensure_player_vitals(character, progress);
  }

  void ensure_world_mobs(const WorldPosition &position) {
    combat_player_->ensure_world_mobs(position);
  }

  std::vector<GameplayPacket> advance_session_combat(std::int64_t ticks,
                                                    const WorldPosition &position) {
    return combat_player_->advance_session_combat(ticks, position);
  }

  // Keep the same operation and admission owner until an ambiguous commit is fenced.
  std::optional<std::vector<CharacterItem>> commit_instance_activity(
      const Handoff &handoff, const WorldProgress &before, const WorldProgress &after,
      const ItemTrade &trade) {
    if (!handoff_ || handoff_->character_id != handoff.character_id ||
        handoff_->account_id != handoff.account_id || handoff_->world_id != handoff.world_id ||
        !quest_intent_ledger_ || before.experience < 0 ||
        after.experience <= before.experience) {
      throw std::logic_error("Invalid instance activity commit");
    }
    const auto membership = state_->auth.read_quest_service_membership(handoff);
    if (!membership) {
      return std::nullopt;
    }

    QuestServiceEffects effects;
    effects.activity_experience = after.experience - before.experience;
    effects.expected_company_type = membership->company_type;
    effects.expected_guild_id = membership->guild_id;
    effects.expected_company_generation = membership->company_generation;
    effects.expected_guild_generation = membership->guild_generation;
    if (membership->company_type != 0) {
      const auto &service = state_->company_feature->service();
      if (!service) {
        throw std::runtime_error("Company activity service is unavailable");
      }
      effects.activity_company_rate = service->rules().character_experience_per_contribution;
    }
    if (membership->guild_id != 0) {
      const auto &service = state_->guild_feature->service();
      if (!service) {
        throw std::runtime_error("Guild activity service is unavailable");
      }
      effects.activity_guild_rate = service->rules().character_experience_per_guild_experience;
    }
    const auto effect_id = random_hex(16);
    const auto intent = quests_begin_mutation(quest_intent_ledger_, after);
    if (!intent) {
      return std::nullopt;
    }
    try {
      quests_watch_mutation(quest_intent_ledger_, *intent, std::nullopt);
    } catch (...) {
      quest_intent_ledger_->finish_prepared(*intent, false);
      throw;
    }

    try {
      QuestMutationResult result;
      if (membership->company_type == 0 && membership->guild_id == 0) {
        auto stored = state_->auth.commit_unaffiliated_activity(handoff, *membership, before,
                                                                after, trade);
        if (stored) {
          result.status = QuestMutationStatus::committed;
          result.granted_items = std::move(*stored);
        }
      } else {
        result = state_->auth.commit_quest_mutation(handoff, effect_id, before, after,
                                                    trade, effects);
      }
      const bool committed = result.status != QuestMutationStatus::conflict;
      quest_intent_ledger_->finish_prepared(*intent, committed);
      if (!quests_mark_mutation_resolved(*intent, false)) {
        throw std::logic_error("Instance activity reconciliation owner changed");
      }
      quests_forget_mutation(*intent);
      if (!committed) {
        return std::nullopt;
      }
      return std::move(result.granted_items);
    } catch (...) {
      close_without_location_checkpoint();
      quests_retry_ambiguous_mutation(*intent);
      throw;
    }
  }

  void create_tutorial_sessions(const Character &character, const Handoff &handoff,
                                std::shared_ptr<const TutorialData> followup_data) {
    auto progress = state_->auth.world_progress(handoff);
    if (!quest_intent_ledger_) {
      quest_intent_ledger_ = std::make_shared<quests::QuestIntentLedger>(progress);
    }
    // Bootstrap has not published handoff_ yet. Both storage adapters bind this explicit
    // entry identity; the live Tutorial domain remains the sole progress cache.
    const auto wire_unit_id = static_cast<std::int64_t>(player_wire_unit_id(character.id));
    tutorial_player_->initialize(
        wire_unit_id, std::move(progress),
        [this, handoff](const auto &before, const auto &after) {
          if (after.experience > before.experience) {
            return commit_instance_activity(handoff, before, after, {}).has_value();
          }
          if (quest_intent_ledger_) {
            if (!handoff_ || handoff_->character_id != handoff.character_id ||
                handoff_->account_id != handoff.account_id ||
                handoff_->world_id != handoff.world_id) {
              throw std::runtime_error("Tutorial quest ledger does not match its character");
            }
            return gm_save_progress(before, after);
          }
          return state_->auth.save_world_progress(handoff, before, after);
        },
        state_->character_feature.progression(), followup_data);
    auto reward_persistence = tutorial::tutorial_reward_persistence(
        state_->auth, handoff, [this](const Character &live) {
          const auto *stats = combat_player_->has_simulation()
                                  ? &combat_player_->current_simulation()->player_stats()
                                  : nullptr;
          session_events_.publish(PlayerInventorySnapshotChanged{live, stats});
        });
    reward_persistence.commit = [this, handoff](const WorldProgress &before,
                                                const WorldProgress &after,
                                                const ItemTrade &trade) {
      if (after.experience > before.experience) {
        return commit_instance_activity(handoff, before, after, trade);
      }
      if (quest_intent_ledger_) {
        if (!handoff_ || handoff_->character_id != handoff.character_id ||
            handoff_->account_id != handoff.account_id ||
            handoff_->world_id != handoff.world_id) {
          throw std::runtime_error("Tutorial quest ledger does not match its character");
        }
        return gm_commit_item_trade(before, after, trade);
      }
      return state_->auth.commit_world_item_trade(handoff, before, after, trade);
    };
    tutorial_player_->bind_instance_rewards(std::move(reward_persistence));
    if (combat_player_->start_tutorial_combat(character, wire_unit_id,
                                              static_cast<bool>(followup_data))) {
      loot_player_->create_tutorial(character.id, handoff);
    }
  }

  // Shared-world quests read and commit stored progress directly, like gathering.
  void create_world_quests(std::int64_t character_id) {
    quests_session_->select_character(character_id);
  }

  // A private tutorial session owns its cached progress revision, so gathering is
  // only created for characters whose progress is read directly from storage.
  void create_gathering_session(const Character &character, const Handoff &handoff) {
    if (!state_->gathering_feature->enabled()) {
      return;
    }
    gathering_ = state_->gathering_feature->create_session(
        player_wire_unit_id(character.id),
        std::make_shared<HandoffProgressAccess>(state_->auth, handoff), *this, session_events_);
    gathering_->set_nodes(state_->gathering_feature->nodes_for(instance_copy_.get()));
    connect_butchering();
  }

  // Butchering joins two owners: the loot service holds the killer's corpse rights and its
  // lifetime, and the gathering service runs the knife interaction and its reward.
  void connect_butchering() {
    const auto feature = state_->gathering_feature;
    loot_player_->set_butchering([feature](int npc_cms_id) {
      return feature->butcherable(npc_cms_id);
    });

    GatheringCorpses corpses;
    corpses.find = [this](std::uint64_t unit_id,
                          std::int64_t ticks) -> std::optional<ButcheringTarget> {
      const auto corpse = loot_player_->butcherable_corpse(unit_id, ticks);
      if (!corpse) {
        return std::nullopt;
      }
      const auto &position = corpse->position;
      return ButcheringTarget{corpse->npc_cms_id, position.map_id,
                              {position.x_mm, position.y_mm, position.z_mm}, corpse->handling};
    };
    corpses.butchered = [this](std::uint64_t unit_id, std::int64_t ticks) {
      return loot_player_->corpse_butchered(unit_id, ticks);
    };
    gathering_->set_corpses(std::move(corpses));
  }

  // Crafting commits through the tutorial session's cached revision when one exists, so
  // later tutorial writes still compare against stored state; otherwise it reads storage.
  void create_processing_session(const Character &character, const Handoff &handoff) {
    if (!state_->processing_feature.enabled()) {
      return;
    }
    processing_ = state_->processing_feature.create_session(
        player_wire_unit_id(character.id),
        std::make_shared<HandoffProgressAccess>(state_->auth, handoff), *this, session_events_);
  }

  // Draw boxes and effect consumables (PC2S_ItemUseReq). Like crafting, the session commits
  // through the tutorial session's cached revision when one exists; effect consumables need the
  // combat session, so outside the private tutorial instance only draw boxes are usable.
  void create_item_use_session(const Handoff &handoff) {
    if (!state_->item_use_feature.enabled()) {
      return;
    }
    item_use_ = state_->item_use_feature.create_session(
        std::make_shared<HandoffProgressAccess>(state_->auth, handoff), *this, session_events_);
  }

  // Mounts, vehicles, mates and the trade run of a non-tutorial character. Each reads and
  // commits storage through the handoff, like gathering.
  void create_companion_sessions(const Character &character, const Handoff &handoff) {
    if (!state_->companion_feature.enabled() || !is_supported_character_id(character.id)) {
      return;
    }

    companions_ = state_->companion_feature.create_session(
        std::make_unique<HandoffProgressAccess>(state_->auth, handoff), connection_id(), character.id);
    if (!state_->trade_feature.catalog()) {
      return;
    }
    trade_ = state_->trade_feature.create_session(
        state_->companion_feature, std::make_unique<HandoffProgressAccess>(state_->auth, handoff),
        connection_id(), character.id);
  }

  // Routes a request owned by the companion or trade session. Returns false when neither owns
  // it. GM companion commands are authorized here by the handoff's role, like other GM requests.
  bool route_companion_request(const WorldGameplayRequest &request,
                                const std::optional<WorldPosition> &position,
                                companion::SessionResult &result) {
    if (state_->companion_feature.handle_command(*companions_, request, position, *this, result)) {
      return true;
    }
    if (state_->trade_feature.handle_dialogue(trade_.get(), request, position, *this, result)) {
      return true;
    }
    if (state_->companion_feature.handle_request(*companions_, request, *this, result)) {
      return true;
    }
    return state_->trade_feature.handle_request(trade_.get(), request, position, *this, result);
  }

  // Joins the shared companion world when the client starts playing: owned summon and mate
  // lists, visible companion units, and an unfinished trade run with its wagon.
  void enter_companion_world(const WorldPosition &position, companion::SessionResult &result) {
    state_->companion_feature.enter_world(*companions_, position, *this, result);
  }

  void encode_companion_packets(const std::vector<GameplayPacket> &packets,
                                std::vector<Frame> &frames) {
    if (packets.empty()) {
      return;
    }
    std::lock_guard lock(state_->protocol_mutex);
    for (const auto &packet : packets) {
      try {
        frames.push_back(state_->protocol.encode(packet.opcode, packet.message));
      } catch (const std::exception &error) {
        spdlog::warn("Companion packet opcode={} encoding failed: {}", packet.opcode, error.what());
      }
    }
  }

  // Runs on the worker strand after close_on_io(): owned units leave the shared world and the wagon
  // transform is saved for the next entry.
  bool leave_companions() {
    const auto detached = companions_->leave_retained(
        [this](const std::vector<companion::VehicleSnapshot> &vehicles,
               const std::shared_ptr<companion::CompanionWorld::RetainedDetach> &operation) {
          if (close_companion_detach_) {
            throw std::logic_error("Companion detach is already retained");
          }
          companion_capture_wagons(vehicles);
          close_companion_detach_ = operation;
        });
    return companion_settle_wagons(detached.vehicles);
  }

  // A new craft must not overlap another service's open action.
  bool other_action_busy(bool include_statue = true, bool include_registration = true,
                         bool include_adventure = true) const {
    return (tutorial_player_->current_instance() && tutorial_player_->current_instance()->busy()) ||
           (combat_player_->has_simulation() && combat_player_->busy()) ||
           (gathering_ && gathering_->busy()) || (include_statue && nuia_session_->busy()) ||
           (include_registration && companions_ && companions_->registration_busy()) ||
           (include_adventure && adventure_ && adventure_->busy());
  }

  bool adventure_blocked() const {
    return other_action_busy(true, true, false) || (processing_ && processing_->busy()) ||
           (companions_ && companions_->riding()) ||
           (combat_player_->has_simulation() &&
            combat_player_->current_simulation()->player_dead());
  }

  bool registration_blocked() const {
    return other_action_busy(true, false) || (processing_ && processing_->busy()) ||
           (combat_player_->has_simulation() &&
            combat_player_->current_simulation()->player_dead());
  }

  void deliver_interaction_observers(const std::vector<GameplayPacket> &packets) {
    std::vector<GameplayPacket> visual;
    for (const auto &packet : packets) {
      if (packet.opcode == 166 || packet.opcode == 167) {
        visual.push_back(packet);
      }
    }
    if (!visual.empty()) {
      deliver_observer_packets(*state_, connection_id(), visual);
    }
  }

  // Captures the party roster identity of the selected character. Non-tutorial sessions
  // read the unlock quest from stored progress here; tutorial sessions use live progress.

  // Live presence for the party directory. Private combat owns the current vitals.

  // Attaches once the client has entered the world (StartPlaying), then refreshes
  // position and vitals on later packets. Runs on the worker strand.

  // The requester is the authenticated character, never a value from the payload.

  // Guild identity of the selected character. Name, level and class come from the stored
  // character row captured at entry (party_session_->identity()), never from a payload.

  // Registers the character in the guild directory once its client entered the world
  // (StartPlaying) and returns its guild entry snapshot. Runs on the worker strand.

  // The requester is the authenticated character, never a value from the payload.

  // Progress the session commits from: the tutorial's cached copy, or storage otherwise.
  WorldProgress current_progress() const {
    return tutorial_player_->current_instance() ? tutorial_player_->current_instance()->progress()
                                                : state_->auth.world_progress(*handoff_);
  }

  // Keeps the character's guild roster entry (company position) current.

  // A lost database acknowledgement may mean the region transaction committed. Reconnect
  // must read that durable result instead of having close-time checkpoints overwrite it.
  void close_without_location_checkpoint() noexcept {
    preserve_durable_location_on_close_.store(true);
    request_supervised_close();
  }

  // Starts an in-connection map move: the session leaves shared presence, drops map-bound
  // tutorial state, and the client loads the new map. Worker strand only.
  enum class MapMoveResult { refused, started, closing };

  MapMoveResult begin_server_map_move(const WorldPosition &spawn,
                                      bool preserve_player_combat = false,
                                      int cut_scene_id = 0,
                                      bool preserve_region_movement = false,
                                      bool position_already_committed = false) {
    if (combat_player_->has_pending_world_defeats() || pending_quest_reconciliation_ ||
        has_unsettled_combat_quest_events() || pending_death_owner_) {
      return MapMoveResult::refused;
    }
    if (preserve_durable_location_on_close_.load() && !position_already_committed) {
      return MapMoveResult::refused;
    }
    if (!events_session_->authorize_server_map_move(spawn)) {
      return MapMoveResult::refused;
    }

    std::shared_ptr<InstanceCopy> destination_copy;
    try {
      if (handoff_) {
        destination_copy = prepare_instance_copy(handoff_->character_id, spawn.map_id);
        if (gathering_) {
          state_->gathering_feature->nodes_for(destination_copy.get());
        }
      }
    } catch (const std::exception &error) {
      spdlog::warn("Map move refused connection={}: instance copy unavailable: {}",
                   connection_id(), error.what());
      return MapMoveResult::refused;
    }

    Frame start;
    std::vector<WorldPresenceEvent> leave_events;
    int handshake_map_before_move{};
    bool transition_started = false;
    try {
      std::lock_guard lock(state_->protocol_mutex);
      if (const auto current = handshake_.current_position()) {
        handshake_map_before_move = current->map_id;
      }
      const bool restore_gliding = preserve_region_movement && combat_player_->gliding();
      transition_started = true;
      map_staging_checkpoint_fence_.store(true);
      gathering_view_.invalidate();
      start = handshake_.begin_map_move(spawn, cut_scene_id, restore_gliding);
      if (preserve_player_combat && combat_player_->has_simulation()) {
        synchronize_player_state();
      }
      leave_events = state_->presence.leave(connection_id());
    } catch (const std::exception &error) {
      if (transition_started) {
        close_without_location_checkpoint();
      }
      try {
        spdlog::warn("Map move refused connection={}: {}", connection_id(), error.what());
      } catch (...) {
      }
      return transition_started ? MapMoveResult::closing : MapMoveResult::refused;
    } catch (...) {
      if (transition_started) {
        close_without_location_checkpoint();
        return MapMoveResult::closing;
      }
      throw;
    }
    try {
      deliver_presence_events(std::move(leave_events));

      if (preserve_region_movement && companions_ && companions_->entered()) {
        auto suspended = companions_->suspend_region(spawn);
        deliver_companion_update(*state_, suspended.broadcast);
      }
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
      if (preserve_region_movement && companion_transition_fault_armed.exchange(false)) {
        companion_transition_fault_observed.store(true);
        throw std::runtime_error("Injected partial regional transition failure");
      }
      if (!preserve_region_movement && general_transition_fault_armed.exchange(false)) {
        general_transition_fault_observed.store(true);
        throw std::runtime_error("Injected partial map transition failure");
      }
#endif

      // Tutorial, combat and loot belong to map 8000's private instance and its announced units.
      loot_player_->reset();
      combat_player_->on_map_departure(preserve_player_combat, preserve_region_movement);
      tutorial_player_->reset_instance();
      npc_player_->set_population_map(std::nullopt);
      unit_registry_.clear();
      tutorial_player_->reset_bootstrap_flags();
      housing_session_->on_map_departure();

      // The destination's copy is chosen before the client loads it, so presence, monsters and
      // doodads of the right copy are in place when it arrives.
      if (handoff_) {
        const auto previous_copy = copy_channel_.load();
        adopt_instance_copy(std::move(destination_copy));
        if (copy_channel_.load() != previous_copy) {
          refresh_companion_channel();
        }
        if (spawn.map_id != handshake_map_before_move) {
          restore_volume_doodads(spawn.map_id,
                                 state_->auth.world_progress(*handoff_).doodad_phases);
        }
      }

      if (!position_already_committed) {
        try {
          if (!state_->auth.save_world_position(*handoff_, events_session_->persisted_position(spawn))) {
            throw std::runtime_error("Map move position save was refused");
          }
        } catch (const std::exception &error) {
          close_without_location_checkpoint();
          try {
            spdlog::error("Map move position save failed connection={}: {}", connection_id(),
                          error.what());
          } catch (...) {
          }
          return MapMoveResult::closing;
        }
      }

      events_session_->on_map_departure(spawn);

      asio::post(io_executor(), [self = shared_session(), start = std::move(start)]() mutable {
        try {
          if (self->closed()) {
            return;
          }
          self->schedule_gameplay_tick(std::nullopt);
          self->presence_published_ = false;
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
          if (map_transition_io_fault_armed.exchange(false)) {
            map_transition_io_fault_observed.store(true);
            throw std::bad_alloc();
          }
#endif
          const auto status = self->enqueue_frame(std::move(start));
          if (status != FrameEnqueueStatus::inserted) {
            self->close_without_location_checkpoint();
            try {
              self->close_on_io();
            } catch (...) {
            }
            return;
          }
          self->map_staging_checkpoint_fence_.store(false);
        } catch (...) {
          self->close_without_location_checkpoint();
          try {
            self->close_on_io();
          } catch (...) {
          }
          try {
            spdlog::error("Map move staging failed connection={}", self->connection_id());
          } catch (...) {
          }
        }
      });
      // Any started move answers the client's pending transition.
      deferred_travel_.reset();
      return MapMoveResult::started;
    } catch (const std::exception &error) {
      close_without_location_checkpoint();
      try {
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
        if (map_transition_log_fault_armed.exchange(false)) {
          map_transition_log_fault_observed.store(true);
          throw std::bad_alloc();
        }
#endif
        spdlog::error("Map move failed after transition began connection={}: {}",
                      connection_id(), error.what());
      } catch (...) {
      }
      return MapMoveResult::closing;
    } catch (...) {
      close_without_location_checkpoint();
      return MapMoveResult::closing;
    }
  }

  // Runs when MapLoadCompleteReq finishes a server-owned map move. Returns the packets that
  // repopulate the new map for this client. Worker strand only.
  std::vector<GameplayPacket> complete_server_map_move() {
    std::vector<GameplayPacket> packets;
    if (auto housing_packets = housing_session_->complete_entry()) {
      return std::move(*housing_packets);
    }

    if (pending_exit_) {
      const auto target = *pending_exit_;
      pending_exit_.reset();
      housing_session_->forget_origin();
      if (target.map_id == kDiagnosticTutorialMapId && state_->tutorial_feature.data()) {
        // A fresh private instance re-announces the tutorial map's objects and monsters.
        const auto character = state_->auth.world_character(*handoff_);
        if (character) {
          create_tutorial_sessions(*character, *handoff_, nullptr);
          packets = tutorial_player_->bootstrap_packets();
        }
      } else {
        const bool populated = state_->npc_feature.population_for(target.map_id) != nullptr;
        npc_player_->set_population_map(populated ? std::optional<int>(target.map_id)
                                                  : std::nullopt);
        ensure_shared_world_services();
      }
    }
    return packets;
  }

  bool receive_zone_channel_move(const WorldGameplayRequest &request,
                                 const std::optional<WorldPosition> &position) {
    if (!handoff_ || !position || !state_->map_channels || tutorial_player_->current_instance() ||
        housing_session_->in_housing()) {
      return false;
    }
    if (combat_player_->has_pending_world_defeats() || pending_quest_reconciliation_ ||
        has_unsettled_combat_quest_events() || pending_death_owner_) {
      // No zone-failure response is recovered. Close before either character or wagon is
      // checkpointed, so close settlement preserves their origin and the old admission remains
      // fenced until its outstanding defeat has been resolved.
      asio::post(io_executor(), [self = shared_session()] {
        self->close_on_io();
      });
      return true;
    }
    // Natural boundaries retain HP, stamina, statuses and cooldowns. Private encounters
    // have map-owned actors/effects and must end before this bounded travel path is used.
    // The client keeps waiting for this move, so a gameplay tick retries it afterwards.
    if (travel_blocked()) {
      spdlog::warn("Zone channel move deferred connection={}: character has an active action "
                   "or private encounter", connection_id());
      defer_zone_channel_move(request, *position);
      return false;
    }

    auto destination = state_->map_channels->resolve(*position, request.message);
    if (destination && destination->map_id != position->map_id &&
        !zone_travel_allowed(*position, destination->map_id)) {
      spdlog::warn("Zone channel move refused connection={} from_map={} to_map={}: entrance or "
                   "exit conditions are not met",
                   connection_id(), position->map_id, destination->map_id);
      destination.reset();
    }
    if (!destination) {
      spdlog::warn("Zone channel move resynchronized connection={} map={}: no eligible boundary",
                   connection_id(), position->map_id);
      // No zone-failure packet is recovered. The normal same-map handoff resets the
      // client's pending transition, rather than leaving movement and UI locked.
      destination = position;
    }
    // Close settlement runs on this strand after this checkpoint and transition.
    if (closed()) {
      return true;
    }
    if (destination->map_id != position->map_id && housing_area(destination->map_id)) {
      if (walk_into_housing(*position, *destination)) {
        return true;
      }
      spdlog::warn("Zone channel move into housing refused connection={} map={}",
                   connection_id(), destination->map_id);
      destination = position;
    }
    bool wagon_checkpointed = false;
    if (trade_ && destination->map_id != position->map_id) {
      // I/O close can race the database transaction. Until its outcome is known and
      // in-memory positions agree with it, close must not overwrite either durable value.
      map_staging_checkpoint_fence_.store(true);
      const auto checkpoint = trade_->checkpoint_region(*destination, epoch_ticks());
      wagon_checkpointed = checkpoint == trade::RegionCheckpointResult::committed;
      if (checkpoint != trade::RegionCheckpointResult::unknown && !wagon_checkpointed) {
        map_staging_checkpoint_fence_.store(false);
      }
      if (closed()) {
        return true;
      }
      if (checkpoint == trade::RegionCheckpointResult::unknown) {
        close_without_location_checkpoint();
        return true;
      }
      if (checkpoint == trade::RegionCheckpointResult::conflict) {
        // Release the client's pending transition while keeping the player and wagon together.
        destination = position;
      }
    }
    const auto move_result = begin_server_map_move(*destination, true, 0, true,
                                                   wagon_checkpointed);
    if (move_result == MapMoveResult::closing) {
      return true;
    }
    if (move_result == MapMoveResult::refused) {
      if (wagon_checkpointed && trade_) {
        const auto restored = trade_->checkpoint_region(*position, epoch_ticks());
        if (restored != trade::RegionCheckpointResult::committed) {
          close_without_location_checkpoint();
          try {
            spdlog::error("Trade wagon checkpoint restoration uncertain connection={}",
                          connection_id());
          } catch (...) {
          }
          return true;
        }
        // A failed map move may already have changed the in-memory handshake. Reconnect
        // from the durable checkpoint even when restoring the origin was successful.
        close_without_location_checkpoint();
        return true;
      }
      return false;
    }

    if (admin_disconnect_requested_.load()) {
      return true;
    }
    pending_region_companion_handoff_ = true;
    housing_session_->cancel_pending_entry();
    pending_exit_ = destination;
    gm_session_->forget_spawned_units();
    spdlog::info("Zone channel move connection={} from_map={} to_map={}", connection_id(),
                 position->map_id, destination->map_id);
    return true;
  }

  // A used portal doodad (Interaction.playerTask MapMove) moves the player like a zone channel
  // move: vitals, statuses and cooldowns travel along and the destination map repopulates.
  void gathering_travel(const WorldPosition &destination) override {
    if (!handoff_ || tutorial_player_->current_instance() || housing_session_->in_housing()) {
      return;
    }
    const auto move_result = begin_server_map_move(destination, true);
    if (move_result != MapMoveResult::started) {
      if (move_result == MapMoveResult::refused) {
        spdlog::warn("Portal travel refused connection={} to_map={}", connection_id(),
                     destination.map_id);
      }
      return;
    }
    housing_session_->cancel_pending_entry();
    pending_exit_ = destination;
    gm_session_->forget_spawned_units();
    spdlog::info("Portal travel connection={} to_map={}", connection_id(), destination.map_id);
  }

  std::vector<GameplayPacket> gathering_play_cut_scene(int cut_scene_id,
                                                      const WorldPosition &position) override {
    if (!state_->travel_catalog || !handoff_ ||
        !state_->travel_catalog->server_cut_scene(cut_scene_id)) {
      return {};
    }
    if (!travel_session_) {
      travel_session_ = std::make_unique<travel::TravelSession>(
          state_->travel_catalog, player_wire_unit_id(handoff_->character_id));
    }
    travel::TravelPlan plan;
    plan.cut_scenes.push_back(cut_scene_id);
    return travel_session_->play(plan, position, combat_monotonic_ticks());
  }

  // Map entrance/exit conditions read the character's quests and items through the quest
  // condition evaluator and its party membership through the party world.
  travel::TravelContext travel_context(const WorldPosition &position) {
    travel::TravelContext context;
    context.evaluate = [this, position](const nlohmann::json &row) {
      // One row on its own: the travel catalog already applied grouping and negation.
      auto single = row;
      single["group"] = 1;
      single["operator"] = "AND";
      single["negated"] = false;
      return quests_session_->requirements_met(nlohmann::json::array({single}), position);
    };
    switch (state_->party_feature->world()->membership(handoff_->character_id)) {
    case party::PartyWorld::Membership::leader:
      context.party_role = travel::PartyRole::leader;
      break;
    case party::PartyWorld::Membership::member:
      context.party_role = travel::PartyRole::member;
      break;
    case party::PartyWorld::Membership::none:
      context.party_role = travel::PartyRole::none;
      break;
    }
    return context;
  }

  // The presence map that keeps this character apart from other housing channels and other
  // copies of its instance or dungeon; 0 on shared maps. Safe on any executor.
  int presence_map_override() const {
    if (const int housing_map = housing_session_->presence_map(); housing_map != 0) {
      return housing_map;
    }
    return copy_presence_map_.load();
  }

  // Acquire the destination before changing the handshake or departing the current map.
  // Allocation errors propagate so admission never falls back to a shared population.
  std::shared_ptr<InstanceCopy> prepare_instance_copy(std::int64_t character_id, int map_id) {
    if (instance_copy_ && instance_copy_->map_id() == map_id) {
      return instance_copy_;
    }

    if (state_->instance_copies && state_->instance_copies->copies(map_id)) {
      const auto party_id = state_->party_feature->world()->established_party(character_id);
      return state_->instance_copies->enter(map_id, character_id, party_id);
    }

    return {};
  }

  // Publish the previously acquired copy to map-bound services on the worker strand.
  void adopt_instance_copy(std::shared_ptr<InstanceCopy> copy) {
    if (instance_copy_ == copy) {
      return;
    }

    {
      std::lock_guard lock(state_->protocol_mutex);
      gathering_view_.invalidate();
    }
    instance_copy_ = std::move(copy);
    copy_presence_map_.store(instance_copy_ ? instance_copy_->presence_map() : 0);
    copy_channel_.store(instance_copy_ ? instance_copy_->serial() : 0);
    party_session_->set_instance_channel(instance_copy_ ? instance_copy_->channel_key() : 0);
    if (gathering_) {
      gathering_->set_nodes(state_->gathering_feature->nodes_for(instance_copy_.get()));
    }
  }

  // Login must fail rather than admit a resumed instance character into shared state.
  void select_instance_copy(std::int64_t character_id, int map_id) {
    auto copy = prepare_instance_copy(character_id, map_id);
    if (gathering_) {
      state_->gathering_feature->nodes_for(copy.get());
    }
    adopt_instance_copy(std::move(copy));
  }

  // Companion visibility follows the copy (mounts and vehicles of other copies stay hidden).
  // Locks the protocol mutex to deliver; never call it while holding that lock.
  void refresh_companion_channel() {
    if (!companions_) {
      return;
    }
    const auto changed = companions_->set_channel(companion_channel_key());
    deliver_companion_update(*state_, changed.broadcast);
  }

  // Volume doodad phases (PC2S_LevelTriggerReq ChangeDoodadPhase) belong to the map or copy:
  // they are recorded in its doodad state, sent to this character now and to the other players
  // there, and saved in this character's progress. A doodad already in the phase is neither
  // sent again nor re-saved when the character walks into the volume again.
  void apply_volume_doodads(const travel::TravelPlan &plan, const WorldPosition &position,
                            SessionFrameContext &frame) {
    if (plan.doodad_phases.empty()) {
      return;
    }

    std::vector<gathering::GatheringFeature::VolumeDoodadPhase> phases;
    for (const auto &change : plan.doodad_phases) {
      try {
        phases.push_back({std::stoull(change.unit_id), change.phase});
      } catch (const std::exception &) {
        spdlog::warn("Volume doodad phase names an invalid unit {}", change.unit_id);
      }
    }

    const auto source_view = gathering_view_.snapshot();
    const auto source_nodes = state_->gathering_feature->nodes_for(instance_copy_.get());
    const auto changes = state_->gathering_feature->record_volume_doodad_phases(
        position.map_id, instance_copy_.get(), phases, monotonic_ticks());
    auto own = gathering_state_packets(changes, source_nodes, source_view.copy_channel);
    for (auto &packet : own) {
      if (packet.local_plan) {
        packet.local_view_epoch = source_view.epoch;
      }
    }
    frame.session_packets.insert(frame.session_packets.end(),
                                 std::make_move_iterator(own.begin()),
                                 std::make_move_iterator(own.end()));
    // Delivered at once: a map move of the same plan would change this session's copy before
    // the frame's own gathering broadcasts go out.
    if (!changes.empty()) {
      deliver_gathering_changes(*state_, changes, copy_channel_.load(), connection_id());
    }
    save_volume_doodads(phases);
  }

  // Saves volume doodad phases in WorldProgress.doodad_phases (keyed by wire unit id, never a
  // tutorial unit: tools/build_map_travel.py checks it). Retries once after a revision conflict.
  void save_volume_doodads(
      const std::vector<gathering::GatheringFeature::VolumeDoodadPhase> &phases) {
    constexpr int kSaveAttempts = 2;
    for (int attempt = 0; attempt < kSaveAttempts; ++attempt) {
      try {
        const auto before = state_->auth.world_progress(*handoff_);
        auto after = before;
        for (const auto &entry : phases) {
          after.doodad_phases[entry.unit_id] = entry.phase;
        }
        if (after.doodad_phases == before.doodad_phases) {
          return;
        }
        // Storage accepts exactly one revision step per commit.
        after.revision = before.revision + 1;
        if (state_->auth.save_world_progress(*handoff_, before, after)) {
          return;
        }
      } catch (const std::exception &error) {
        spdlog::error("Volume doodad save failed connection={}: {}", connection_id(),
                      error.what());
        return;
      }
    }
    spdlog::warn("Volume doodad save lost a revision race connection={}", connection_id());
  }

  // The character's saved volume doodad phases of `map_id` come back with the map, but only
  // where the map (or copy) has no live state yet. This character receives them with the map's
  // doodad snapshot; players already there receive them now. Worker strand only; locks the
  // protocol mutex to deliver, so never call it while holding that lock.
  void restore_volume_doodads(int map_id, const std::map<std::uint64_t, int> &saved_phases) {
    if (!state_->travel_catalog || saved_phases.empty()) {
      return;
    }
    std::vector<gathering::GatheringFeature::VolumeDoodadPhase> saved;
    for (const auto unit_id : state_->travel_catalog->volume_doodads(map_id)) {
      const auto found = saved_phases.find(unit_id);
      if (found != saved_phases.end()) {
        saved.push_back({unit_id, found->second});
      }
    }
    const auto changes = state_->gathering_feature->record_volume_doodad_phases(
        map_id, instance_copy_.get(), saved, monotonic_ticks(), true);
    if (!changes.empty()) {
      deliver_gathering_changes(*state_, changes, copy_channel_.load(), connection_id());
    }
  }

  // Moves the character for an accepted travel plan, or back onto its current map when the
  // client is waiting for a move that was refused (the same-map handoff releases the client's
  // pending transition). Busy, dead and private-encounter characters do not travel.
  bool travel_map_move(const WorldPosition &origin, const WorldPosition &destination,
                       int cut_scene_id) {
    if (travel_blocked()) {
      spdlog::warn("Map travel blocked connection={}: character has an active action or "
                   "private encounter",
                   connection_id());
      return false;
    }
    if (begin_server_map_move(destination, true, cut_scene_id) != MapMoveResult::started) {
      return false;
    }
    housing_session_->cancel_pending_entry();
    pending_exit_ = destination;
    gm_session_->forget_spawned_units();
    spdlog::info("Map travel connection={} from_map={} to_map={} cut_scene={}", connection_id(),
                 origin.map_id, destination.map_id, cut_scene_id);
    return true;
  }

  // Natural travel keeps HP, stamina, statuses and cooldowns, so an action in progress, death
  // or GM-spawned map actors must end first.
  bool travel_blocked() const {
    return other_action_busy() || gm_session_->has_spawned_units() || events_player_dead();
  }

  // A zone crossing into or out of a map with entrance or exit conditions (points of interest)
  // follows the same MapCondition rules as other travel; the client checked them before it
  // asked. Without the travel profile such crossings stay refused.
  bool zone_travel_allowed(const WorldPosition &origin, int destination_map_id) {
    const auto *from = state_->map_channels->zone_map(origin.map_id);
    const auto *to = state_->map_channels->zone_map(destination_map_id);
    if (!from || !to) {
      return false;
    }
    if (!from->gated && !to->gated) {
      return true;
    }
    return state_->travel_catalog &&
           state_->travel_catalog->travel_allowed(origin.map_id, destination_map_id,
                                                  travel_context(origin));
  }

  bool housing_area(int map_id) const {
    const auto *zone = state_->map_channels->zone_map(map_id);
    return zone && zone->type == "HousingArea";
  }

  // Walking into a housing area's cells enters its channel where the character stands, like a
  // visit without a recorded return point: walking out again steps onto the parent map.
  bool walk_into_housing(const WorldPosition &origin, const WorldPosition &arrival) {
    if (companions_ && companions_->riding()) {
      return false;
    }
    if (!housing_session_->walk_in(arrival)) {
      return false;
    }
    gm_session_->forget_spawned_units();
    spdlog::info("Zone channel move into housing connection={} from_map={} to_map={}",
                 connection_id(), origin.map_id, arrival.map_id);
    return true;
  }

  // Keeps a map travel the client is waiting for until travel_blocked() clears (see
  // resume_deferred_travel). A later request replaces an earlier one.
  void defer_map_travel(const WorldPosition &origin, const WorldPosition &destination,
                        int cut_scene_id) {
    const auto now = monotonic_ticks();
    deferred_travel_ = DeferredTravel{origin.map_id,
                                      destination,
                                      cut_scene_id,
                                      std::nullopt,
                                      now + kDeferredTravelRetryTicks,
                                      now + kDeferredTravelLifetimeTicks};
    spdlog::info("Map travel deferred until the action ends connection={} from_map={} to_map={}",
                 connection_id(), origin.map_id, destination.map_id);
  }

  void defer_zone_channel_move(const WorldGameplayRequest &request,
                               const WorldPosition &position) {
    const auto now = monotonic_ticks();
    deferred_travel_ = DeferredTravel{position.map_id,
                                      position,
                                      0,
                                      request,
                                      now + kDeferredTravelRetryTicks,
                                      now + kDeferredTravelLifetimeTicks};
  }

  std::optional<std::int64_t> deferred_travel_retry_tick() const {
    if (!deferred_travel_) {
      return std::nullopt;
    }
    return deferred_travel_->next_retry_tick;
  }

  // Gameplay ticks retry a deferred travel once the blocking action has ended. Worker strand
  // only. Returns true when the retry started a map move (or closed the session), so the
  // caller must not continue with the departed map's tick.
  bool resume_deferred_travel(const WorldPosition &position) {
    if (!deferred_travel_) {
      return false;
    }

    const auto now = monotonic_ticks();
    const bool expired = now >= deferred_travel_->expires_tick;
    if (expired || position.map_id != deferred_travel_->origin_map_id) {
      spdlog::warn("Deferred travel dropped connection={} map={}: {}", connection_id(),
                   position.map_id,
                   expired ? "the character stayed busy" : "the character changed maps");
      deferred_travel_.reset();
      return false;
    }
    if (travel_blocked()) {
      deferred_travel_->next_retry_tick = now + kDeferredTravelRetryTicks;
      return false;
    }

    const auto deferred = std::move(*deferred_travel_);
    deferred_travel_.reset();
    spdlog::info("Deferred travel resumed connection={} map={} zone_move={}", connection_id(),
                 position.map_id, deferred.zone_request.has_value());
    if (deferred.zone_request) {
      return receive_zone_channel_move(*deferred.zone_request, position);
    }
    return travel_map_move(position, deferred.destination, deferred.cut_scene_id);
  }

public:
  // Requires the caller to hold state_->protocol_mutex.
  std::optional<int> current_map_id_locked() const {
    const auto position = handshake_.current_position();
    if (!position) {
      return std::nullopt;
    }
    return position->map_id;
  }

  // One coherent recipient map/copy generation, safe to capture on any executor.
  GatheringViewIdentity current_gathering_view() const {
    return gathering_view_.snapshot();
  }

  // Companion relations carry the view selected under the companion-world lock.
  // Revalidate the source first and hold both guards until the frame is queued.
  void bind_companion_frame(Frame &frame, const companion::CompanionSourceBinding &binding) {
    const auto world = state_->companion_feature.world();
    if (!world || binding.connection_id != connection_id()) {
      throw std::logic_error("Companion frame has an invalid recipient or source");
    }
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
    if (binding.initial_add && binding.connection_id == 2 &&
        binding.unit_id == companion_entry_unit_for_testing.load()) {
      companion_entry_binding_observed.store(true);
    }
#endif
    const GatheringViewIdentity view{binding.view.epoch, binding.view.map_id,
                                     binding.view.copy_channel};
    std::weak_ptr<WorldSession> recipient = shared_session();
    const auto opcode = frame.opcode();
    frame.prepare_for_enqueue = [world, recipient, binding, view, opcode](Frame &queued) {
      const auto session = recipient.lock();
      if (!session || session->closed() || !session->gathering_view_.matches(view)) {
        return false;
      }

#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
      if (binding.terminal && opcode == companion::kMountUnitNotification &&
          companion_first_frame_fault_armed.exchange(false)) {
        companion_first_frame_fault_observed.store(true);
        throw std::runtime_error("Injected companion dismount enqueue failure");
      }
      if (binding.terminal && opcode == companion::kUnitRemoveNotification &&
          companion_dismount_inserted.load() && companion_remove_fault_armed.exchange(false)) {
        companion_remove_fault_observed.store(true);
        throw std::runtime_error("Injected companion removal enqueue failure");
      }
#endif

      constexpr int kMaximumSnapshotAttempts = 3;
      // UnitAdd has a fixed set of numeric fields and at most 64 bounded passenger IDs.
      // Reserve for the snapshot, encoded frame and source revalidation copy before reading it.
      constexpr std::size_t kCompanionSnapshotBytes = 1024 * 1024;
      for (int attempt = 0; attempt < kMaximumSnapshotAttempts; ++attempt) {
        std::optional<nlohmann::json> snapshot;
        std::shared_ptr<gathering::ConstructionBudget::Lease> construction;
        if (binding.initial_add) {
          construction = gathering::ConstructionBudget::acquire();
          construction->charge(3 * kCompanionSnapshotBytes);
          snapshot = world->snapshot_unit_add(binding);
          if (!snapshot) {
            return false;
          }
          std::size_t snapshot_bytes = 0;
          charge_local_json(*snapshot, 0, snapshot_bytes);
          if (snapshot_bytes > kCompanionSnapshotBytes) {
            throw std::runtime_error("Companion snapshot exceeds its preparation budget");
          }
          Frame replacement;
          {
            std::lock_guard lock(session->state_->protocol_mutex);
            replacement = session->state_->protocol.encode(opcode, *snapshot);
          }
          if (replacement.bytes.size() > kCompanionSnapshotBytes) {
            throw std::runtime_error("Companion snapshot exceeds its wire budget");
          }
          queued.bytes = std::move(replacement.bytes);
          queued.shared = std::move(replacement.shared);
        }

        auto source_guard = world->acquire_source_queue_guard(
            binding, world, snapshot ? &*snapshot : nullptr);
        if (!source_guard) {
          if (!binding.initial_add) {
            return false;
          }
          continue;
        }
        auto view_guard = session->gathering_view_.acquire_queue_guard(
            view, std::nullopt, std::nullopt, false, session);
        if (!view_guard) {
          return false;
        }
        struct QueueScope {
          std::shared_ptr<companion::CompanionWorld::QueueGuard> source;
          std::shared_ptr<GatheringViewState::QueueGuard> view;
          std::shared_ptr<gathering::ConstructionBudget::Lease> construction;
        };
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
        queued.after_enqueue = [source_guard, binding, opcode] {
          source_guard->mark_ready();
          if (binding.terminal && opcode == companion::kMountUnitNotification &&
              companion_remove_fault_armed.load()) {
            companion_dismount_inserted.store(true);
          }
        };
#else
        queued.after_enqueue = [source_guard] {
          source_guard->mark_ready();
        };
#endif
        queued.enqueue_scope = std::make_shared<QueueScope>(
            QueueScope{std::move(source_guard), std::move(view_guard), std::move(construction)});
        return true;
      }
      throw std::runtime_error("Companion snapshot changed repeatedly during encoding");
    };
  }

  bool gathering_sector_ready(const GatheringViewIdentity &view, std::uint32_t sector) const {
    return gathering_view_.sector_ready(view, sector);
  }

  // Bind an already encoded local plan to this recipient without retaining the session.
  // The preparation runs later on the I/O executor; no protocol lock is held while it
  // inspects node state or reconstructs a sector's gathering fields.
  void bind_gathering_frame(Frame &frame, const GatheringViewIdentity &view) {
    std::weak_ptr<WorldSession> recipient = shared_session();
    const auto sector = frame.local_plan ? frame.local_plan->source_sector() : std::nullopt;
    const auto sector_generation = sector ? gathering_view_.sector_generation(view, *sector)
                                          : std::nullopt;
    const auto initial = frame.local_plan ? frame.local_plan->initial_sector() : std::nullopt;
    if (initial && initial != sector) {
      throw std::logic_error("Gathering sector snapshot has an inconsistent source");
    }
    const bool initial_sector = initial.has_value();
    bind_local_gathering_packet(
        frame, view,
        [recipient, sector, sector_generation, initial_sector](const GatheringViewIdentity &expected) {
          const auto session = recipient.lock();
          if (!session || session->closed() || !session->gathering_view_.matches(expected)) {
            return false;
          }
          if (!sector) {
            return true;
          }
          return sector_generation && session->gathering_view_.sector_matches(
                                          expected, *sector, *sector_generation, !initial_sector);
        },
        [recipient](std::uint16_t opcode, const nlohmann::json &message) {
          const auto session = recipient.lock();
          if (!session || session->closed()) {
            throw std::runtime_error("Gathering recipient closed during packet reconstruction");
          }
          std::lock_guard lock(session->state_->protocol_mutex);
          return session->state_->protocol.encode(opcode, message);
        },
        [recipient, view, sector, sector_generation, initial_sector] {
          const auto session = recipient.lock();
          if (!session || session->closed()) {
            return std::shared_ptr<GatheringViewState::QueueGuard>{};
          }
          return session->gathering_view_.acquire_queue_guard(
              view, sector, sector_generation, !initial_sector, session);
        });
  }

  // A non-streaming map needs one ordered catch-up before its recipients can see deltas.
  // Recheck the complete snapshot after encoding so the codec never runs under the source lock.
  Frame make_full_map_snapshot_marker(const GatheringViewIdentity &view) {
    Frame marker;
    const auto nodes = state_->gathering_feature->nodes_for(instance_copy_.get());
    std::weak_ptr<WorldSession> recipient = shared_session();
    marker.prepare_for_enqueue = [nodes, recipient, view](Frame &prepared) {
      constexpr std::size_t kMaximumSnapshotFrames = 4096;
      constexpr int kMaximumAttempts = 3;
      if (!view.map_id) {
        return false;
      }
      const auto same_changes = [](const auto &left, const auto &right) {
        return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
            [](const auto &first, const auto &second) {
              return first.node == second.node && first.active == second.active &&
                     first.phase == second.phase && first.tick == second.tick &&
                     first.revision == second.revision;
            });
      };
      for (int attempt = 0; attempt < kMaximumAttempts; ++attempt) {
        const auto construction = gathering::ConstructionBudget::acquire();
        construction->charge(gathering::ConstructionBudget::kSectorBytes);
        const auto charge_node = [construction] {
          construction->charge(gathering::ConstructionBudget::kNodeBytes);
        };
        std::vector<GatheringStateChange> changes;
        {
          std::lock_guard lock(nodes->publication_mutex());
          changes = nodes->depleted_on_map(*view.map_id, charge_node);
          auto doodads = nodes->doodads_on_map(*view.map_id, charge_node);
          changes.insert(changes.end(), doodads.begin(), doodads.end());
        }
        auto packets = gathering_state_packets(changes, nodes, view.copy_channel);
        if (packets.size() > kMaximumSnapshotFrames) {
          throw std::runtime_error("Gathering map snapshot has too many packets");
        }
        auto frames = std::make_shared<std::vector<Frame>>();
        frames->reserve(packets.size());
        const auto session = recipient.lock();
        if (!session || session->closed()) {
          return false;
        }
        {
          std::lock_guard lock(session->state_->protocol_mutex);
          for (const auto &packet : packets) {
            frames->push_back(session->state_->protocol.encode(packet.opcode, packet.message));
          }
        }
        struct SourceGuard {
          explicit SourceGuard(std::shared_ptr<GatheringNodes> owner)
              : nodes(std::move(owner)), lock(nodes->publication_mutex()) {
          }
          std::shared_ptr<GatheringNodes> nodes;
          std::unique_lock<std::recursive_mutex> lock;
        };
        const auto ordering = std::make_shared<SourceGuard>(nodes);
        auto confirmed = nodes->depleted_on_map(*view.map_id, charge_node);
        auto confirmed_doodads = nodes->doodads_on_map(*view.map_id, charge_node);
        confirmed.insert(confirmed.end(), confirmed_doodads.begin(), confirmed_doodads.end());
        if (!same_changes(changes, confirmed)) {
          continue;
        }
        auto view_guard = session->gathering_view_.acquire_queue_guard(
            view, std::nullopt, std::nullopt, false, session);
        if (!view_guard) {
          return false;
        }
        struct QueueScope {
          std::shared_ptr<SourceGuard> ordering;
          std::shared_ptr<GatheringViewState::QueueGuard> view;
          std::shared_ptr<gathering::ConstructionBudget::Lease> construction;
        };
        prepared.prepared_batch = std::move(frames);
        prepared.after_enqueue = [view_guard] {
          view_guard->mark_all_ready();
        };
        prepared.enqueue_scope = std::make_shared<QueueScope>(
            QueueScope{ordering, std::move(view_guard), construction});
        return true;
      }
      throw std::runtime_error("Gathering map snapshot changed repeatedly during encoding");
    };
    return marker;
  }

  // InstanceCopy::serial of the copy the character occupies; 0 on shared maps. Any executor.
  std::uint64_t current_copy_channel() const {
    return copy_channel_.load();
  }

  // A closed raid channel (or the member's own leave) removes it from the raid map. Runs after
  // any frame the worker strand is processing.
  void post_raid_eviction(int raid_map_id) {
    asio::post(worker_strand_, [self = shared_session(), raid_map_id] {
      try {
        self->events_session_->on_raid_eviction(raid_map_id);
      } catch (const std::exception &error) {
        spdlog::error("Raid channel exit failed connection={}: {}", self->connection_id(),
                      error.what());
      }
    });
  }

private:
  void synchronize_player_state() {
    combat_player_->synchronize_player_state();
  }

  void publish_committed_player_life() {
    combat_player_->publish_committed_player_life();
  }

  // Shared-world quests (any map outside the private tutorial instance). The quest book reads
  // the persisted character and progress, evaluates against server-owned state (tracked
  // position, announced units, owned items) and commits before any packet is returned.

  // Logs static request metadata only (animation, parameter kind, CMS interaction ID, map).
  void log_unhandled_interaction_start(const WorldGameplayRequest &request,
                                       const std::optional<WorldPosition> &position) const {
    const auto &message = request.message;
    std::string param_kind = "none";
    int interaction_id = 0;
    if (const auto param = message.find("param"); param != message.end() && param->is_object() &&
                                                  !param->empty()) {
      param_kind = param->begin().key();
      if (param->contains("interaction") && param->at("interaction").is_object()) {
        interaction_id = param->at("interaction").value("interaction_id", 0);
      }
    }

    const int map_id = position ? position->map_id : 0;
    const bool gathering_map_loaded =
        position && state_->gathering_feature->has_map(position->map_id);
    spdlog::info("Native world interaction start unhandled connection={} anim={} param={} "
                 "interaction={} map={} gathering_session={} gathering_map={}",
                 connection_id(), message.value("interaction_anim_id", 0), param_kind,
                 interaction_id, map_id, gathering_ != nullptr, gathering_map_loaded);
  }

  // Runs on the worker after entry preparation, before the completion is posted. Publishing
  // before claim_world_admission() lets a revocation ordered after the claim find this
  // connection in disconnect_account(). A connection whose claim fails is closed.
  void publish_admin_identity(const Handoff &handoff) {
    std::lock_guard lock(admin_mutex_);
    admin_identity_ = AdminIdentity{handoff.account_id, handoff.character_id, handoff.game_master,
                                    party_session_->identity() ? party_session_->identity()->name
                                                               : std::string()};
  }

  void finish_authentication(std::optional<Handoff> handoff, Frame response) {
    // An operator disconnect or a socket failure may have been ordered before this completion.
    // Never resurrect such a connection; a pending operator close still runs afterwards.
    if (closed() || admin_disconnect_requested_.load()) {
      return;
    }
    if (!handoff) {
      spdlog::warn("Native world credential rejected");
      close_on_io();
      return;
    }
    if (!state_->auth.claim_world_admission(*handoff)) {
      spdlog::warn("Native world handoff was revoked, superseded or expired before entry; "
                   "character {}",
                   handoff->character_id);
      close_on_io();
      return;
    }

    handoff_ = std::move(handoff);
    population_lease_ = state_->auth.admit_world_character();

    spdlog::info("Native world credential accepted; character {}", handoff_->character_id);
    std::vector<Frame> frames;
    frames.push_back(std::move(response));
    complete_request(std::move(frames), false);
  }

  GameplayPacket gm_invincible_attribute(bool enabled) const {
    return gm_session_->gm_invincible_attribute(enabled);
  }

  // Candidate in-place revival for a dead player (opcode 20039). A living player or a repeated
  // request changes nothing. The durable dead flag is cleared first, under the revision guard. A
  // refused revival (no known position, revision conflict, store error) is logged and answered
  // with no frames before any revival packet or HP/SP/status effect exists, so the player stays
  // dead in memory and in the store and the session stays usable; only a later commit with the
  // same stale revision would fail, as it does for any other progress writer.
  std::vector<GameplayPacket> revive_player() {
    return combat_player_->revive_player();
  }

  // The last movement position, or the persisted character position when the player died without
  // moving since entry (the position the session entered the world with).
  std::vector<GameplayPacket> persist_player_revival(
      std::optional<WorldPosition> destination = std::nullopt, int revive_task_id = 0) {
    return combat_player_->persist_player_revival(destination, revive_task_id);
  }

  void persist_player_death(std::vector<GameplayPacket> &packets) {
    combat_player_->persist_player_death(packets);
  }

  GameplayPacket equip_error(const WorldGameplayRequest &request, x5::ec::EErrorCode code) {
    return inventory::InventorySession::equip_error(request, code);
  }

  // Catalog stats of `character` with the applied carried-weight tier on top. Session stat
  // refreshes (equipment, trades, tempering) go through here, so they keep the slowdown.
  nlohmann::json session_stats(const Character &character) const {
    return inventory_session_->session_stats(character);
  }

  bool allow_gm_request() {
    return gm_session_->allow_gm_request();
  }

  bool allow_player_car() {
    return gm_session_->allow_player_car();
  }

  void gm_reply(const std::string &text) {
    gm_session_->gm_reply(text);
  }

  void sync_gm_combat_protection() {
    gm_session_->sync_gm_combat_protection();
  }

  // What the server knows about one watched unit right now.

  std::vector<GameplayPacket> debug_watch_packets(const std::optional<WorldPosition> &position,
                                                  std::int64_t ticks) {
    return gm_session_->debug_watch_packets(position, ticks);
  }

  // The services an overworld login creates; a character leaving the tutorial lacks them.
  void ensure_shared_world_services() {
    const auto character = state_->auth.world_character(*handoff_);
    if (!character) {
      return;
    }
    if (!quest_intent_ledger_) {
      quest_intent_ledger_ =
          std::make_shared<quests::QuestIntentLedger>(state_->auth.world_progress(*handoff_));
      party_session_->set_quest_ledger(quest_intent_ledger_);
    }
    if (!gathering_) {
      create_gathering_session(*character, *handoff_);
    }
    if (!companions_) {
      create_companion_sessions(*character, *handoff_);
    }
    if (!quests_session_->has_book()) {
      create_world_quests(character->id);
    }
    ensure_player_vitals(*character, state_->auth.world_progress(*handoff_));
    sync_gm_combat_protection();
  }

  // Private instance key: the tutorial map is a per-character instance, so the character id
  // identifies this session's units. Empty when no character is authenticated.
  std::string instance_key() const {
    return handoff_ ? std::to_string(handoff_->character_id) : std::string();
  }

  // Worker-strand map identity follows the handshake, independently of optional NPC data.
  int session_map_id() const {
    return handshake_.map_id();
  }

  // The world selects visible sectors; Gathering owns their depleted-node/doodad payloads.
  std::vector<GameplayPacket> refresh_sector_sight(const WorldPosition &position) {
    if (!state_->sector_grid) {
      return {};
    }
    const auto sector_ids =
        sector_sight_.update(*state_->sector_grid, position.map_id, position.x_mm, position.y_mm);
    {
      const auto nodes = state_->gathering_feature->nodes_for(instance_copy_.get());
      std::lock_guard publication_lock(nodes->publication_mutex());
      gathering_view_.synchronize_sectors(gathering_view_.snapshot(), sector_sight_.visible());
    }
    if (sector_ids.empty()) {
      return {};
    }
    return state_->gathering_feature->sector_snapshots(position.map_id, sector_ids,
                                                       instance_copy_.get());
  }

  // ChangeStatNoti after an equipped tempering success: every stat whose value changed plus the
  // vitals present in the current stats (Observed order of captured entries: ascending stat).
  GameplayPacket polish_stat_notification(std::int64_t unit_id, const nlohmann::json &before,
                                          const nlohmann::json &after,
                                          const nlohmann::json &current) const {
    return inventory_session_->equipment_stat_notification(unit_id, before, after, current);
  }

  // Loads the authoritative state for a progression change. A tutorial session owns its cached
  // progress revision; other sessions read storage. Empty without progression definitions.
  // Commits one progression transition, then refreshes the cached snapshot and combat stats.
  // When the stats changed, appends the observed build follow-ups: CombatWeaponGaugeNoti (78)
  // and ChangeStatNoti (47, SCS_Equip) with the changed stats and vitals. Stats are computed
  // before the commit, so a build the stat model cannot evaluate is refused unchanged.
  // Effects of a committed level change that the change itself does not carry: a level-up
  // refills a living player's HP/SP (Observed: the official level-up ChangeStatNoti reports
  // full vitals), the party/guild identity and cached nub take the new level, and observers
  // receive ChangeCharacterPublicNoti (37, Observed for other players' level-ups).
  void apply_level_change(const WorldProgress &progress, std::vector<GameplayPacket> &packets) {
    character_session_->apply_level_change(progress, packets);
  }

  void log_credential_diagnostic() {
    const auto received = std::string_view(credential_sample().data(), credential_sample().size());
    const auto diagnostic = state_->auth.diagnose_world_credential(received);

    const auto log_match = [this](std::string_view match_type, const CredentialPrefixMatch &match) {
      spdlog::info("World credential diagnostic connection={} match={} offset={} length={} "
                   "ssl_pending={}",
                   connection_id(), match_type, match.offset, match.length,
                   credential_pending_bytes());
    };

    if (diagnostic.packet_key.length == 0 && diagnostic.char_token.length == 0) {
      log_match("none", {});
      return;
    }
    if (diagnostic.packet_key.length > 0) {
      log_match("packet_key", diagnostic.packet_key);
    }
    if (diagnostic.char_token.length > 0) {
      log_match("char_token", diagnostic.char_token);
    }
  }

  const party::MemberPresence *chat_party_identity() const override {
    return party_session_->identity();
  }

  std::optional<int> chat_authenticated_world() const override {
    return handoff_ ? std::optional<int>(handoff_->world_id) : std::nullopt;
  }

  bool chat_has_handoff() const override {
    return handoff_.has_value();
  }

  ChatIdentity chat_exit_identity() const override {
    return ChatIdentity{handoff_->world_id, handoff_->character_id, {}};
  }

  ChatIdentity chat_entry_identity() const override {
    return {handoff_->world_id, handoff_->character_id, party_session_->identity()->name};
  }

  std::uint64_t chat_connection_id() const override {
    return connection_id();
  }

  bool chat_closed() const override {
    return closed();
  }

  std::vector<std::uint64_t> chat_party_connections(std::int64_t character_id) const override {
    return state_->party_feature->world()->party_connections(character_id);
  }

  chat::ChatFeature::EntryHandler chat_entry_handler() const override {
    std::lock_guard lock(state_->protocol_mutex);
    return state_->chat_feature.entry_handler_locked();
  }

  chat::ChatFeature::EntryHandler chat_exit_handler() const override {
    std::lock_guard lock(state_->protocol_mutex);
    return state_->chat_feature.exit_handler_locked();
  }

  chat::ChatFeature::CommandHandler chat_command_handler() const override {
    std::lock_guard lock(state_->protocol_mutex);
    return state_->chat_feature.command_handler_locked();
  }

  std::uint64_t party_connection_id() const override {
    return connection_id();
  }

  bool party_closed() const override {
    return closed();
  }

  bool party_has_tutorial() const override {
    return tutorial_player_->current_instance() != nullptr;
  }

  std::optional<nlohmann::json> party_character_stats(const Character &character) const override {
    if (!state_->character_feature.has_stats()) {
      return std::nullopt;
    }
    return state_->character_feature.stats(character);
  }

  const nlohmann::json *party_combat_stats() const override {
    return combat_player_->has_simulation()
               ? &std::as_const(*combat_player_).current_simulation()->player_stats()
               : nullptr;
  }

  const WorldProgress *party_tutorial_progress() const override {
    return tutorial_player_->current_instance() ? &tutorial_player_->current_instance()->progress()
                                                : nullptr;
  }

  party::TimePoint party_now() const override {
    return party::Clock::now();
  }

  std::optional<GuildActor> guild_identity() const override {
    const auto *identity = party_session_->identity();
    if (!identity || !handoff_) {
      return std::nullopt;
    }

    GuildActor actor;
    actor.world_id = handoff_->world_id;
    actor.char_id = identity->char_id;
    actor.name = identity->name;
    actor.level = identity->level;
    actor.char_class = identity->char_class;
    actor.account_id = handoff_->account_id;
    return actor;
  }

  std::optional<CompanyStatus> guild_company_status(std::int64_t character_id) const override {
    if (!state_->company_feature->service()) {
      return std::nullopt;
    }
    return state_->company_feature->service()->status(character_id);
  }

  std::uint64_t guild_connection_id() const override {
    return connection_id();
  }

  std::int64_t guild_requester_id() const override {
    return party_session_->identity() ? party_session_->identity()->char_id : 0;
  }

  bool guild_closed() const override {
    return closed();
  }

  std::optional<company::CompanyCharacter> company_character() const override {
    if (!handoff_) {
      return std::nullopt;
    }
    return company::CompanyCharacter{handoff_->account_id, handoff_->character_id};
  }

  std::optional<std::int64_t> company_selected_character_id() const override {
    const auto *identity = party_session_->identity();
    return identity ? std::optional<std::int64_t>(identity->char_id) : std::nullopt;
  }

  const WorldProgress *company_tutorial_progress() const override {
    return tutorial_player_->current_instance() ? &tutorial_player_->current_instance()->progress()
                                                : nullptr;
  }

  std::optional<WorldProgress> company_current_progress() const override {
    if (!handoff_) {
      return std::nullopt;
    }
    return current_progress();
  }

  std::optional<int> company_dialogue_npc() const override {
    // Tutorial maps track their own dialogue target; on shared-world maps the quest book
    // records the NPC opened with DialogueStartReq (Donella, Devicas).
    const auto *instance = tutorial_player_->current_instance();
    if (instance) {
      const auto tutorial_npc = instance->dialogue_npc_cms_id();
      if (tutorial_npc) {
        return tutorial_npc;
      }
    }
    return quests_session_->dialogue_npc_template_id();
  }

  bool company_has_guild_membership(std::int64_t character_id) const override {
    return state_->guild_feature->service() &&
           state_->guild_feature->service()->status(character_id).guild_id != 0;
  }

  void company_publish_activity_experience(std::int64_t character_id,
                                           std::int64_t experience) override {
    session_events_.publish(CharacterActivityExperienceGained{character_id, experience});
  }

  void company_publish_membership_changed() override {
    session_events_.publish(CompanyMembershipChanged{});
  }

  void company_publish_progress_committed(const WorldProgress &progress) override {
    session_events_.publish(PlayerProgressCommitted{progress});
  }

  bool housing_has_handoff() const override {
    return handoff_.has_value();
  }

  bool housing_closed() const override {
    return closed();
  }

  std::int64_t housing_account_id() const override {
    return handoff_->account_id;
  }

  std::int64_t housing_character_id() const override {
    return handoff_->character_id;
  }

  int housing_world_id() const override {
    return handoff_->world_id;
  }

  const std::string *housing_character_name() const override {
    const auto *identity = party_session_->identity();
    return identity ? &identity->name : nullptr;
  }

  std::uint64_t housing_connection_id() const override {
    return connection_id();
  }

  std::uint64_t housing_player_unit_id(std::int64_t character_id) const override {
    return player_wire_unit_id(character_id);
  }

  bool housing_has_tutorial() const override {
    return tutorial_player_->current_instance() != nullptr;
  }

  const WorldProgress *housing_tutorial_progress() const override {
    return tutorial_player_->current_instance() ? &tutorial_player_->current_instance()->progress()
                                                : nullptr;
  }

  void housing_publish_progress(const WorldProgress &progress) override {
    session_events_.publish(PlayerProgressCommitted{progress});
  }

  std::shared_ptr<housing::SessionPort> housing_session_port() override {
    return std::static_pointer_cast<housing::SessionPort>(shared_session());
  }

  std::optional<WorldPosition> housing_current_position() const override {
    std::lock_guard lock(state_->protocol_mutex);
    return handshake_.current_position();
  }

  bool housing_begin_map_move(const WorldPosition &destination) override {
    return begin_server_map_move(destination) == MapMoveResult::started;
  }

  std::optional<WorldPosition>
  housing_public_return_origin(const std::optional<WorldPosition> &origin) const override {
    return events_session_->housing_return_origin(origin);
  }

  std::optional<int> housing_raid_parent_map(int map_id) const override {
    return state_->events_feature->raid_parent_map(map_id);
  }

  // Housing areas share their parent map's coordinates, so a character without a recorded
  // origin steps out onto the parent map where it stands (map 20101 -> 9012). The tutorial
  // start remains the last resort when the travel profile names no parent arrival.
  WorldPosition
  housing_default_return_position(const std::optional<WorldPosition> &current) const override {
    if (current && state_->travel_catalog) {
      if (const auto parent = state_->travel_catalog->parent_arrival(*current)) {
        return *parent;
      }
    }
    return {kDiagnosticTutorialMapId, kDiagnosticSpawnX, kDiagnosticSpawnY, kDiagnosticSpawnZ,
            kDiagnosticSpawnYaw};
  }

  void housing_clear_pending_exit() override {
    pending_exit_.reset();
  }

  void housing_stage_pending_exit(const WorldPosition &destination) override {
    pending_exit_ = destination;
  }

  void housing_gm_reply(const std::string &line) override {
    gm_reply(line);
  }

  std::int64_t housing_monotonic_ticks() const override {
    return combat_monotonic_ticks();
  }

  // Garden interactions drain SP exactly like gathering: the same combat simulation flow.
  housing::HousingVitals housing_vitals() override {
    housing::HousingVitals vitals;
    vitals.dead = [this] {
      return gathering_player_dead();
    };
    vitals.begin_stamina = [this](const housing::HousingStaminaFlow &flow)
        -> std::optional<std::vector<housing::WorldPacket>> {
      const GatheringStaminaFlow gathering_flow{flow.cost, flow.wire_start_ns, flow.execute_ms,
                                                flow.regen_resume_ms, flow.server_ticks};
      auto announcement = gathering_begin_stamina(gathering_flow);
      if (!announcement) {
        return std::nullopt;
      }
      std::vector<housing::WorldPacket> packets;
      for (auto &packet : *announcement) {
        packets.push_back({packet.opcode, std::move(packet.message)});
      }
      return packets;
    };
    vitals.spend_stamina = [this](std::int64_t ticks) {
      if (gathering_spend_stamina(ticks)) {
        session_events_.publish(PlayerCombatSnapshotChanged{});
      }
    };
    vitals.cancel_stamina =
        [this](std::int64_t ticks) -> std::optional<housing::WorldPacket> {
      auto stopped = gathering_cancel_stamina(ticks);
      if (!stopped) {
        return std::nullopt;
      }
      return housing::WorldPacket{stopped->opcode, std::move(stopped->message)};
    };
    return vitals;
  }

  bool nuia_has_handoff() const override {
    return handoff_.has_value();
  }

  bool nuia_has_tutorial() const override {
    return tutorial_player_->current_instance() != nullptr;
  }

  std::uint64_t nuia_player_unit_id() const override {
    return player_wire_unit_id(handoff_->character_id);
  }

  std::int64_t nuia_monotonic_ticks() const override {
    return combat_monotonic_ticks();
  }

  bool nuia_has_character_stats() const override {
    return state_->character_feature.has_stats();
  }

  nlohmann::json nuia_character_stats(const Character &character) const override {
    return state_->character_feature.stats(character);
  }

  bool nuia_has_combat() const override {
    return combat_player_->has_simulation();
  }

  bool nuia_player_dead() const override {
    return std::as_const(*combat_player_).current_simulation()->player_dead();
  }

  bool nuia_other_action_busy() const override {
    return other_action_busy(false);
  }

  bool nuia_processing_busy() const override {
    return processing_ && processing_->busy();
  }

  bool nuia_riding() const override {
    return companions_ && companions_->riding();
  }

  bool nuia_teleport_pending() override {
    return escape_feature_->teleport_pending();
  }

  bool nuia_server_teleport_pending() override {
    return escape_feature_->server_teleport_pending();
  }

  bool nuia_save_progress(const WorldProgress &before, const WorldProgress &after) override {
    return state_->auth.save_world_progress(*handoff_, before, after);
  }

  void nuia_publish_progress(const WorldProgress &progress) override {
    session_events_.publish(PlayerProgressCommitted{progress});
  }

  const nlohmann::json &nuia_adopt_blessing_stats(const nlohmann::json &stats) override {
    // Nui blessing originally copies its prepared model; the caller still uses that model
    // for the growth diff after reading combat's authoritative current-vitals snapshot.
    combat_player_->current_simulation()->update_player_stats(stats);
    return combat_player_->current_simulation()->player_stats();
  }

  void nuia_publish_inventory_snapshot_locked(const Character &character,
                                              const nlohmann::json *prepared_stats) override {
    std::lock_guard lock(state_->protocol_mutex);
    session_events_.publish(PlayerInventorySnapshotChanged{character, prepared_stats});
  }

  GameplayPacket nuia_growth_notification(std::int64_t character_id, const nlohmann::json &before,
                                          const nlohmann::json &after,
                                          const nlohmann::json &current) const override {
    return polish_stat_notification(player_wire_unit_id(character_id), before, after, current);
  }

  std::vector<GameplayPacket> nuia_legacy_revive() override {
    return revive_player();
  }

  std::optional<WorldPosition> nuia_revival_position() const override {
    return combat_player_->revival_position();
  }

  std::optional<std::vector<GameplayPacket>>
  nuia_request_server_teleport(const WorldPosition &destination, std::int64_t now) override {
    auto outcome = escape_feature_->request_server_teleport(destination, now);
    if (outcome.refusal != EscapeRefusal::none) {
      return std::nullopt;
    }
    return std::move(outcome.packets);
  }

  std::vector<GameplayPacket> nuia_persist_revival(const WorldPosition &destination,
                                                   int task_id) override {
    return persist_player_revival(destination, task_id);
  }

  std::vector<GameplayPacket> nuia_revive_combat(std::int64_t now) override {
    return combat_player_->current_simulation()->revive_if_dead(now);
  }

  void nuia_publish_combat_snapshot() override {
    session_events_.publish(PlayerCombatSnapshotChanged{});
  }

  void nuia_publish_player_life() override {
    publish_committed_player_life();
  }

  bool inventory_has_handoff() const override {
    return handoff_.has_value();
  }

  std::int64_t inventory_character_id() const override {
    return handoff_->character_id;
  }

  std::uint64_t inventory_connection_id() const override {
    return connection_id();
  }

  std::uint64_t inventory_player_unit_id(std::int64_t character_id) const override {
    return player_wire_unit_id(character_id);
  }

  std::int64_t inventory_monotonic_ticks() const override {
    return combat_monotonic_ticks();
  }

  bool inventory_has_tutorial() const override {
    return tutorial_player_->current_instance() != nullptr;
  }

  const WorldProgress *inventory_tutorial_progress() const override {
    return tutorial_player_->current_instance() ? &tutorial_player_->current_instance()->progress()
                                                : nullptr;
  }

  bool inventory_has_character_stats() const override {
    return state_->character_feature.has_stats();
  }

  nlohmann::json inventory_character_stats(const Character &character) const override {
    return state_->character_feature.stats(character);
  }

  bool inventory_has_combat() const override {
    return combat_player_->has_simulation();
  }

  bool inventory_player_in_combat() const override {
    return combat_player_->has_simulation() &&
           std::as_const(*combat_player_).current_simulation()->player_in_combat();
  }

  bool inventory_action_busy() const override {
    return other_action_busy() || (processing_ && processing_->busy());
  }

  const nlohmann::json *inventory_combat_stats() const override {
    return combat_player_->has_simulation()
               ? &std::as_const(*combat_player_).current_simulation()->player_stats()
               : nullptr;
  }

  const nlohmann::json &inventory_update_combat_stats(nlohmann::json stats) override {
    combat_player_->current_simulation()->update_player_stats(std::move(stats));
    return combat_player_->current_simulation()->player_stats();
  }

  bool inventory_update_items(std::int64_t revision,
                              const std::vector<CharacterItem> &mutations) override {
    return state_->auth.update_world_items(*handoff_, revision, mutations);
  }

  bool inventory_discard_items(std::int64_t revision, const std::vector<std::int64_t> &item_ids,
                               const std::map<int, int> &stackable_counts) override {
    return state_->auth.discard_world_items(*handoff_, revision, item_ids, stackable_counts);
  }

  bool inventory_apply_polish(std::int64_t revision, const ItemPolishChange &change) override {
    return state_->auth.apply_world_item_polish(*handoff_, revision, change);
  }

  bool inventory_save_progress(const WorldProgress &before, const WorldProgress &after) override {
    return state_->auth.save_world_progress(*handoff_, before, after);
  }

  void inventory_publish_equipment_revision(std::int64_t revision) override {
    session_events_.publish(PlayerEquipmentRevisionCommitted{revision});
  }

  void inventory_publish_world_loot_progress(const WorldProgress &progress) override {
    session_events_.publish(PlayerWorldLootProgressCommitted{progress});
  }

  void inventory_publish_weapon_selection(const WorldProgress &progress) override {
    if (auto *tutorial = tutorial_player_->current_instance()) {
      tutorial->adopt_committed_progress(progress);
    }
    session_events_.publish(PlayerWorldLootProgressCommitted{progress});
  }

  void inventory_publish_snapshot(const Character &character,
                                  const nlohmann::json *prepared_stats) override {
    session_events_.publish(PlayerInventorySnapshotChanged{character, prepared_stats});
  }

  void inventory_refresh_snapshot(const Character &character,
                                  const nlohmann::json *prepared_stats) override {
    // This is the existing sink. Its caller retains the original held/unheld codec scope.
    handshake_.refresh_items_and_stats(character, prepared_stats);
    state_->presence.refresh_equipment(connection_id(), character, prepared_stats);
    if (combat_player_->has_simulation()) {
      combat_player_->current_simulation()->update_player_weapon(character);
    }
  }

  std::uint64_t events_connection_id() const override {
    return connection_id();
  }

  bool events_closed() const override {
    return closed();
  }

  std::optional<events::EventsCharacter> events_character() const override {
    if (!handoff_) {
      return std::nullopt;
    }
    return events::EventsCharacter{handoff_->world_id, handoff_->character_id};
  }

  const party::MemberPresence *events_party_identity() const override {
    return party_session_->identity();
  }

  std::int64_t events_monotonic_ticks() const override {
    return combat_monotonic_ticks();
  }

  std::int64_t events_epoch_ticks() const override {
    return unix_epoch_ticks_now();
  }

  world_metrics::TimedMutex &events_protocol_mutex() const override {
    return state_->protocol_mutex;
  }

  std::optional<WorldPosition> events_position_locked() const override {
    return handshake_.current_position();
  }

  std::optional<WorldPosition> events_pending_map_destination() const override {
    return pending_exit_;
  }

  bool events_has_tutorial() const override {
    return tutorial_player_->current_instance() != nullptr;
  }

  bool events_in_housing() const override {
    return housing_session_->in_housing();
  }

  bool events_other_action_busy() const override {
    return other_action_busy();
  }

  bool events_has_gm_units() const override {
    return gm_session_->has_spawned_units();
  }

  bool events_player_dead() const override {
    return combat_player_->has_simulation() &&
           std::as_const(*combat_player_).current_simulation()->player_dead();
  }

  bool events_begin_map_move(const WorldPosition &destination,
                             bool preserve_player_combat) override {
    return begin_server_map_move(destination, preserve_player_combat) == MapMoveResult::started;
  }

  void events_stage_map_destination(const WorldPosition &destination) override {
    housing_session_->cancel_pending_entry();
    pending_exit_ = destination;
    gm_session_->forget_spawned_units();
  }

  const MapChannels *events_map_channels() const override {
    return state_->map_channels ? &*state_->map_channels : nullptr;
  }

  bool events_zone_channel_move(const WorldGameplayRequest &request,
                                const std::optional<WorldPosition> &position) override {
    return receive_zone_channel_move(request, position);
  }

  WorldProgress events_world_progress() const override {
    return tutorial_player_->current_instance() ? tutorial_player_->current_instance()->progress()
                                                : state_->auth.world_progress(*handoff_);
  }

  bool events_save_world_progress(const WorldProgress &before,
                                  const WorldProgress &after) override {
    return state_->auth.save_world_progress(*handoff_, before, after);
  }

  void events_adopt_committed_progress(const WorldProgress &progress) override {
    session_events_.publish(PlayerProgressCommitted{progress});
  }

  void events_deliver_observers(const std::vector<GameplayPacket> &packets) override {
    deliver_observer_packets(*state_, connection_id(), packets);
  }

  void events_gm_reply(const std::string &message) override {
    gm_reply(message);
  }

  std::optional<gm::GmIdentity> gm_identity() const override {
    if (!handoff_) {
      return std::nullopt;
    }
    return gm::GmIdentity{handoff_->account_id, handoff_->character_id, handoff_->world_id,
                          handoff_->game_master};
  }

  std::optional<ChatIdentity> gm_chat_identity() const override {
    if (!handoff_ || !party_session_->identity()) {
      return std::nullopt;
    }
    return ChatIdentity{handoff_->world_id, party_session_->identity()->char_id,
                        party_session_->identity()->name};
  }

  std::uint64_t gm_connection_id() const override {
    return connection_id();
  }

  bool gm_closed() const override {
    return closed();
  }

  std::int64_t gm_monotonic_ticks() const override {
    return combat_monotonic_ticks();
  }

  std::int64_t gm_epoch_ticks() const override {
    return unix_epoch_ticks_now();
  }

  world_metrics::TimedMutex &gm_protocol_mutex() const override {
    return state_->protocol_mutex;
  }

  std::optional<WorldPosition> gm_position_locked() const override {
    return handshake_.current_position();
  }

  std::string gm_instance_key() const override {
    return instance_key();
  }

  int gm_session_map_id() const override {
    return session_map_id();
  }

  bool gm_has_combat() const override {
    return combat_player_->has_simulation();
  }

  bool gm_combat_add_monster(std::uint64_t unit_id, int npc_cms_id,
                             const WorldPosition &position) override {
    return combat_player_->current_simulation()->add_gm_monster(unit_id, npc_cms_id, position);
  }

  bool gm_combat_decorate_spawn(GameplayPacket &packet) const override {
    return std::as_const(*combat_player_).current_simulation()->decorate_spawn(packet);
  }

  std::vector<GameplayPacket> gm_combat_add_abnormal_gauge(std::uint64_t unit_id,
                                                           combat_rules::AbnormalType type,
                                                           int amount,
                                                           std::int64_t ticks) override {
    return combat_player_->current_simulation()->gm_add_abnormal_gauge(unit_id, type, amount,
                                                                       ticks);
  }

  std::vector<GameplayPacket> gm_combat_add_status_effect(std::uint64_t unit_id,
                                                          int status_effect_id,
                                                          std::int64_t ticks) override {
    return combat_player_->current_simulation()->gm_add_status_effect(unit_id, status_effect_id,
                                                                      ticks);
  }

  std::vector<GameplayPacket> gm_combat_cancel_skill(std::uint64_t unit_id,
                                                     const WorldPosition &position,
                                                     std::int64_t ticks) override {
    return combat_player_->current_simulation()->gm_cancel_skill(unit_id, position, ticks);
  }

  std::vector<GameplayPacket> gm_combat_damage_monster(std::uint64_t unit_id, int amount,
                                                       std::int64_t ticks) override {
    return combat_player_->current_simulation()->gm_damage_monster(unit_id, amount, ticks);
  }

  std::vector<GameplayPacket> gm_combat_execute_self_effect(int effect_id,
                                                            const WorldPosition &position,
                                                            std::int64_t ticks) override {
    return combat_player_->current_simulation()->gm_execute_self_effect(effect_id, position, ticks);
  }

  std::vector<GameplayPacket> gm_combat_kill_player(const WorldPosition &position,
                                                    std::int64_t ticks) override {
    return combat_player_->current_simulation()->gm_kill_player(position, ticks);
  }

  std::vector<GameplayPacket> gm_combat_place_monster(std::uint64_t unit_id,
                                                      const WorldPosition &destination,
                                                      std::int64_t ticks) override {
    return combat_player_->current_simulation()->gm_place_monster(unit_id, destination, ticks);
  }

  std::vector<GameplayPacket> gm_combat_remove_status_effect(std::uint64_t unit_id,
                                                             int status_effect_id,
                                                             std::int64_t ticks) override {
    return combat_player_->current_simulation()->gm_remove_status_effect(unit_id, status_effect_id,
                                                                         ticks);
  }

  std::size_t gm_combat_reset_cooldowns() override {
    return combat_player_->current_simulation()->gm_reset_cooldowns();
  }

  std::vector<GameplayPacket> gm_combat_restore_vitals(std::int64_t ticks) override {
    return combat_player_->current_simulation()->gm_restore_vitals(ticks);
  }

  std::vector<GameplayPacket> gm_combat_set_player_stat(int stat_type, int value,
                                                        std::int64_t ticks) override {
    return combat_player_->current_simulation()->gm_set_player_stat(stat_type, value, ticks);
  }

  std::vector<GameplayPacket> gm_combat_toggle_monster_ai(std::uint64_t unit_id, bool enabled,
                                                          std::int64_t ticks) override {
    return combat_player_->current_simulation()->gm_toggle_monster_ai(unit_id, enabled, ticks);
  }

  bool gm_combat_visible_monster(std::uint64_t unit_id) const override {
    return std::as_const(*combat_player_).current_simulation()->is_visible_world_monster(unit_id);
  }

  std::optional<int> gm_combat_monster_health(std::uint64_t unit_id) const override {
    return std::as_const(*combat_player_).current_simulation()->live_monster_health(unit_id);
  }

  std::optional<CombatSession::MonsterDebugView>
  gm_combat_monster_debug_view(std::uint64_t unit_id, std::int64_t ticks) const override {
    return std::as_const(*combat_player_).current_simulation()->monster_debug_view(unit_id, ticks);
  }

  bool gm_combat_player_dead() const override {
    return std::as_const(*combat_player_).current_simulation()->player_dead();
  }

  const nlohmann::json &gm_combat_player_stats() const override {
    return std::as_const(*combat_player_).current_simulation()->player_stats();
  }

  std::vector<GameplayPacket> gm_combat_remove_monster(std::uint64_t unit_id,
                                                       std::int64_t ticks) override {
    return combat_player_->current_simulation()->remove_gm_monster(unit_id, ticks);
  }

  void gm_combat_update_player_stats(nlohmann::json stats) override {
    combat_player_->current_simulation()->update_player_stats(std::move(stats));
  }

  ClientCatalog *gm_client_catalog() const override {
    return state_->client_catalog.get();
  }

  const ShopCatalog *gm_shop_catalog() const override {
    return state_->shop_feature.catalog().get();
  }

  bool gm_has_character_stats() const override {
    return state_->character_feature.has_stats();
  }

  bool gm_has_tutorial() const override {
    return static_cast<bool>(tutorial_player_->current_instance());
  }

  bool gm_tutorial_data_available() const override {
    return static_cast<bool>(state_->tutorial_feature.data());
  }

  bool gm_in_housing() const override {
    return housing_session_->in_housing();
  }

  std::optional<int> gm_region_map(const WorldPosition &destination,
                                   bool keep_public_channel) const override {
    return state_->map_channels
               ? state_->map_channels->region_map_for(destination, keep_public_channel)
               : std::nullopt;
  }

  std::optional<int> gm_housing_area_parent(int map_id) const override {
    return state_->travel_catalog ? state_->travel_catalog->housing_area_parent(map_id)
                                  : std::nullopt;
  }

  std::optional<Character> gm_character() const override {
    return state_->auth.world_character(*handoff_);
  }

  WorldProgress gm_progress() const override {
    return tutorial_player_->current_instance() ? tutorial_player_->current_instance()->progress()
                                                : state_->auth.world_progress(*handoff_);
  }

  WorldProgress gm_stored_progress() const override {
    return state_->auth.world_progress(*handoff_);
  }

  bool gm_save_progress(const WorldProgress &before, const WorldProgress &after) override {
    std::optional<quests::QuestIntentLedger::Sequence> intent;
    if (quest_intent_ledger_ && before.quests != after.quests) {
      intent = quests_begin_mutation(quest_intent_ledger_, after);
      if (!intent) {
        return false;
      }
      try {
        quests_watch_mutation(quest_intent_ledger_, *intent, std::nullopt);
      } catch (...) {
        quest_intent_ledger_->finish_prepared(*intent, false);
        throw;
      }
    }
    bool saved{};
    try {
      saved = state_->auth.save_world_progress(*handoff_, before, after);
    } catch (...) {
      if (intent) {
        quests_retry_ambiguous_mutation(*intent);
      }
      throw;
    }
    if (intent) {
      try {
        quest_intent_ledger_->finish_prepared(*intent, saved);
        if (!quests_mark_mutation_resolved(*intent, false)) {
          throw std::logic_error("GM quest reconciliation owner changed");
        }
        quests_forget_mutation(*intent);
      } catch (...) {
        quests_retry_ambiguous_mutation(*intent);
        throw;
      }
    }
    return saved;
  }

  std::optional<std::vector<CharacterItem>> gm_commit_item_trade(const WorldProgress &before,
                                                                 const WorldProgress &after,
                                                                 const ItemTrade &trade) override {
    std::optional<quests::QuestIntentLedger::Sequence> intent;
    if (quest_intent_ledger_ && before.quests != after.quests) {
      intent = quests_begin_mutation(quest_intent_ledger_, after);
      if (!intent) {
        return std::nullopt;
      }
      try {
        quests_watch_mutation(quest_intent_ledger_, *intent, std::nullopt);
      } catch (...) {
        quest_intent_ledger_->finish_prepared(*intent, false);
        throw;
      }
    }
    std::optional<std::vector<CharacterItem>> result;
    try {
      result = state_->auth.commit_world_item_trade(*handoff_, before, after, trade);
    } catch (...) {
      if (intent) {
        quests_retry_ambiguous_mutation(*intent);
      }
      throw;
    }
    if (intent) {
      try {
        quest_intent_ledger_->finish_prepared(*intent, result.has_value());
        if (!quests_mark_mutation_resolved(*intent, false)) {
          throw std::logic_error("GM item reconciliation owner changed");
        }
        quests_forget_mutation(*intent);
      } catch (...) {
        quests_retry_ambiguous_mutation(*intent);
        throw;
      }
    }
    return result;
  }

  void gm_refresh_traded_items(const Character &before, const ShopTrade &trade,
                               const std::vector<CharacterItem> &committed) override {
    shop_session_->on_item_trade_committed(before, trade, committed);
  }

  nlohmann::json gm_session_stats(const Character &character) const override {
    return session_stats(character);
  }

  GameplayPacket gm_stat_notification(std::uint64_t unit_id, const nlohmann::json &before,
                                      const nlohmann::json &after,
                                      const nlohmann::json &current) const override {
    return polish_stat_notification(unit_id, before, after, current);
  }

  std::vector<GameplayPacket> gm_execute_progression(const GmCommand &command) override {
    const auto edit = character_progression_edit(command);
    if (!edit) {
      return {};
    }
    return character_session_->apply_operator_edit(*edit);
  }

  void gm_apply_level_change(const WorldProgress &progress,
                             std::vector<GameplayPacket> &packets) override {
    apply_level_change(progress, packets);
  }

  std::vector<GameplayPacket> gm_advance_combat(std::int64_t ticks,
                                                const WorldPosition &position) override {
    return advance_session_combat(ticks, position);
  }

  void gm_persist_player_death(std::vector<GameplayPacket> &packets) override {
    persist_player_death(packets);
  }

  std::vector<GameplayPacket> gm_revive_player() override {
    return revive_player();
  }

  std::optional<std::vector<GameplayPacket>>
  gm_dispatch_event_request(const WorldGameplayRequest &request) override {
    const auto event = event_gm::parse_event_command(request.opcode, request.message);
    if (!event.matched) {
      return std::nullopt;
    }
    return events_session_->execute_gm_command(event);
  }

  std::vector<std::string> gm_event_help_lines() const override {
    return event_gm::help_lines();
  }

  std::vector<GameplayPacket>
  gm_channel_event_info(const std::optional<WorldPosition> &position) override {
    return events_session_->channel_event_info(position);
  }

  std::optional<std::vector<GameplayPacket>>
  gm_dispatch_expression_request(const WorldGameplayRequest &request) override {
    return expression_feature_->dispatch_gm(request);
  }

  std::optional<std::vector<GameplayPacket>>
  gm_dispatch_housing_request(const WorldGameplayRequest &request) override {
    return housing_session_->dispatch_gm(request);
  }

  bool gm_has_guilds() const override {
    return static_cast<bool>(state_->guild_feature->service());
  }

  std::int64_t gm_own_guild_id(std::int64_t character_id) const override {
    return state_->guild_feature->service()->status(character_id).guild_id;
  }

  x5::ec::EErrorCode gm_guild_add_experience(std::int64_t guild_id, int amount) override {
    return state_->guild_feature->service()->gm_add_experience(guild_id, amount);
  }

  x5::ec::EErrorCode gm_guild_set_level(std::int64_t guild_id, int level) override {
    return state_->guild_feature->service()->gm_set_level(guild_id, level);
  }

  x5::ec::EErrorCode gm_guild_disband(std::int64_t guild_id) override {
    return state_->guild_feature->service()->gm_disband(guild_id);
  }

  x5::ec::EErrorCode gm_guild_set_master(std::int64_t guild_id,
                                         std::int64_t character_id) override {
    return state_->guild_feature->service()->gm_set_master(guild_id, character_id);
  }

  x5::ec::EErrorCode gm_guild_reset_cooldowns(std::int64_t character_id) override {
    return state_->guild_feature->service()->gm_reset_cooldowns(character_id);
  }

  bool gm_has_companies() const override {
    return static_cast<bool>(state_->company_feature->service());
  }

  CompanyRecord gm_company_record(std::int64_t character_id) const override {
    return state_->company_feature->service()->status(character_id).record;
  }

  std::optional<std::int64_t> gm_company_threshold(int company_type, int level) const override {
    return state_->company_feature->service()->contribution_for_position(company_type, level);
  }

  CompanyResult gm_commit_company_record(const CompanyRecord &record) override {
    return state_->company_feature->service()->set_record(record);
  }

  bool gm_has_quest_book() const override {
    return quests_session_->has_book();
  }

  QuestResult gm_plan_quest_acceptance(int quest_id, bool check_conditions,
                                       const WorldProgress &progress,
                                       const QuestWorldView &view) override {
    return quests_session_->plan_acceptance(quest_id, check_conditions, progress, view);
  }

  WorldQuestBook::GmCompletion gm_plan_quest_completion(const std::vector<int> &quest_ids,
                                                        const WorldProgress &progress,
                                                        const QuestWorldView &view) override {
    return quests_session_->plan_completion(quest_ids, progress, view);
  }

  const AnnouncedUnit *gm_interaction_npc(std::uint64_t unit_id) const override {
    return npc_player_->interaction_npc(unit_id);
  }

  bool gm_has_mail_storage() const override {
    return state_->mail_feature.has_repository();
  }

  std::optional<MailRecord> gm_deliver_mail_record(const MailRecord &mail) override {
    return state_->mail_feature.deliver_record(mail);
  }

  void gm_notify_mail_peer(const MailRecord &mail,
                           const std::shared_ptr<gm::GmPeer> &peer) override {
    state_->mail_feature.notify_peer(mail, *this, [peer](Frame frame) {
      peer->post_frame(std::move(frame));
    });
  }

  bool gm_has_fog_storage() const override {
    return static_cast<bool>(state_->fog_repository);
  }

  std::optional<std::size_t> gm_reveal_fog() override {
    return reveal_world_map(*state_->fog_repository, *handoff_, *state_->client_catalog);
  }

  bool gm_begin_map_move(const WorldPosition &destination) override {
    return begin_server_map_move(destination) == MapMoveResult::started;
  }

  void gm_stage_map_move(const WorldPosition &destination, bool to_tutorial) override {
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
    if (general_transition_fault_observed.load()) {
      general_transition_staged_observed.store(true);
    }
#endif
    housing_session_->cancel_pending_entry();
    housing_session_->forget_origin();
    if (to_tutorial) {
      pending_exit_ = destination;
    } else {
      pending_exit_.reset();
      const bool populated = state_->npc_feature.population_for(destination.map_id) != nullptr;
      npc_player_->set_population_map(populated ? std::optional<int>(destination.map_id)
                                                : std::nullopt);
      ensure_shared_world_services();
    }
  }

  std::optional<std::vector<GameplayPacket>>
  gm_request_server_teleport(const WorldPosition &destination, std::int64_t ticks) override {
    auto outcome = escape_feature_->request_server_teleport(destination, ticks);
    if (outcome.refusal != EscapeRefusal::none) {
      return std::nullopt;
    }
    return std::move(outcome.packets);
  }

  std::optional<gm::GmTarget> gm_find_target(const std::string &name,
                                             std::int64_t character_id) override {
    std::optional<gm::GmTarget> found;
    std::vector<std::shared_ptr<WorldSession>> live_sessions;
    {
      std::lock_guard lock(state_->protocol_mutex);
      for (const auto &[connection_id, weak_session] : state_->sessions) {
        auto session = weak_session.lock();
        if (!session) {
          continue;
        }
        auto summary = session->admin_summary_locked();
        const bool matches = character_id != 0
                                 ? summary.character_id == character_id
                                 : gm::same_character_name(summary.character_name, name);
        if (summary.authenticated && summary.position && matches) {
          gm::GmPeerSnapshot peer{session->connection_id(), summary.account_id,
                                  summary.character_id,     summary.authenticated,
                                  summary.game_master,      std::move(summary.character_name),
                                  summary.position};
          found = gm::GmTarget{std::shared_ptr<gm::GmPeer>(session, session->gm_session_.get()),
                               std::move(peer)};
        }
        // Final session releases remain outside the shared codec mutex, as before.
        live_sessions.push_back(std::move(session));
      }
    }
    return found;
  }

  gm::GmOnlinePlayers gm_online_players() override {
    gm::GmOnlinePlayers players;
    {
      std::lock_guard lock(state_->protocol_mutex);
      for (const auto &[connection_id, weak_session] : state_->sessions) {
        auto session = weak_session.lock();
        if (!session) {
          continue;
        }
        const auto summary = session->admin_summary_locked();
        if (summary.authenticated && summary.position) {
          players.lines.push_back(gm::online_player_line(
              summary.character_name, summary.game_master, summary.position->map_id));
        }
        players.retained_peers.push_back(std::move(session));
      }
    }
    return players;
  }

  const AnnouncedUnit *gm_find_announced_unit(std::uint64_t unit_id) const override {
    return unit_registry_.find_in_instance(unit_id, session_map_id(), instance_key());
  }

  UnitAnnounceResult gm_announce_npc(AnnouncedUnit unit) override {
    return unit_registry_.announce(std::move(unit));
  }

  void gm_despawn_unit(std::uint64_t unit_id) override {
    unit_registry_.despawn(unit_id);
  }

  Frame gm_encode_packet_locked(std::uint16_t opcode, const nlohmann::json &message) override {
    return state_->protocol.encode(opcode, message);
  }

  void gm_post_frame(Frame frame) override {
    post_frame(std::move(frame));
  }

  void gm_close_on_io() override {
    close_on_io();
  }

  asio::any_io_executor gm_io_executor() override {
    return io_executor();
  }

  asio::any_io_executor gm_worker_executor() override {
    return worker_strand_;
  }

  std::shared_ptr<void> retain_gm_owner() override {
    return shared_session();
  }

  std::optional<std::int64_t> gm_next_gameplay_tick() const override {
    return next_gameplay_tick();
  }

  bool expression_has_character() const override {
    return handoff_.has_value();
  }

  bool expression_closed() const override {
    return closed();
  }

  std::uint64_t expression_connection_id() const override {
    return connection_id();
  }

  std::int64_t expression_character_id() const override {
    return handoff_->character_id;
  }

  std::uint64_t expression_player_unit_id() const override {
    return player_wire_unit_id(handoff_->character_id);
  }

  std::int64_t expression_now_ticks() const override {
    return combat_monotonic_ticks();
  }

  WorldProgress expression_progress() const override {
    return tutorial_player_->current_instance() ? tutorial_player_->current_instance()->progress()
                                                : state_->auth.world_progress(*handoff_);
  }

  bool save_expression_progress(const WorldProgress &before, const WorldProgress &after) override {
    return state_->auth.save_world_progress(*handoff_, before, after);
  }

  void adopt_expression_progress(const WorldProgress &after) override {
    session_events_.publish(PlayerProgressCommitted{after});
  }

  expression::ExpressionBlockers expression_blockers() const override {
    expression::ExpressionBlockers result;
    result.riding = companions_ && companions_->riding();
    result.dead = combat_player_->has_simulation() &&
                  std::as_const(*combat_player_).current_simulation()->player_dead();
    result.using_skill = combat_player_->has_simulation() && combat_player_->busy();
    result.interacting =
        (tutorial_player_->current_instance() && tutorial_player_->current_instance()->busy()) ||
        (gathering_ && gathering_->busy());
    return result;
  }

  void deliver_expression_observers(const std::vector<GameplayPacket> &packets) override {
    deliver_observer_packets(*state_, connection_id(), packets);
  }

  void reply_to_expression_command(const std::string &message) override {
    gm_reply(message);
  }

  void append_expression_snapshot(const GameplayPacket &packet,
                                  std::vector<Frame> &pending_notifications) override {
    std::lock_guard lock(state_->protocol_mutex);
    pending_notifications.push_back(state_->protocol.encode(packet.opcode, packet.message));
  }

  bool escape_has_character() const override {
    return handoff_.has_value();
  }

  bool escape_closed() const override {
    return closed();
  }

  std::uint64_t escape_connection_id() const override {
    return connection_id();
  }

  std::uint64_t escape_player_unit_id() const override {
    return player_wire_unit_id(handoff_->character_id);
  }

  std::int64_t escape_now_ticks() const override {
    return combat_monotonic_ticks();
  }

  escape::EscapeBlockers escape_blockers() const override {
    escape::EscapeBlockers result;
    result.dead = combat_player_->has_simulation() &&
                  std::as_const(*combat_player_).current_simulation()->player_dead();
    result.busy = other_action_busy();
    result.riding = companions_ && companions_->riding();
    return result;
  }

  asio::any_io_executor escape_io_executor() override {
    return io_executor();
  }

  asio::any_io_executor escape_worker_executor() override {
    return worker_strand_;
  }

  std::shared_ptr<void> retain_escape_owner() override {
    return shared_session();
  }

  bool finish_escape_revival(std::vector<GameplayPacket> &packets) override {
    return nuia_session_->finish_escape_revival(packets);
  }

  void adopt_escape_position(const WorldPosition &position) override {
    reset_movement_vitals();
    std::lock_guard lock(state_->protocol_mutex);
    handshake_.set_authoritative_position(position);
    spdlog::info("Native world escape teleport completed connection={}", connection_id());
  }

  bool escape_god_mode() const override {
    return gm_session_->god_mode();
  }

  GameplayPacket escape_invincibility_packet(bool enabled) const override {
    return gm_invincible_attribute(enabled);
  }

  void append_encoded_escape_packets(const std::vector<GameplayPacket> &packets,
                                     std::vector<Frame> &frames) override {
    std::lock_guard lock(state_->protocol_mutex);
    for (const auto &packet : packets) {
      frames.push_back(state_->protocol.encode(packet.opcode, packet.message));
    }
  }

  void enqueue_escape_frame(Frame frame) override {
    enqueue_frame(std::move(frame));
  }

  std::int64_t progression_character_id() const override {
    return handoff_->character_id;
  }

  std::optional<Character> progression_character() override {
    return state_->auth.world_character(*handoff_);
  }

  WorldProgress progression_stored_progress() override {
    return state_->auth.world_progress(*handoff_);
  }

  bool progression_save(const WorldProgress &before, const WorldProgress &after) override {
    return state_->auth.save_world_progress(*handoff_, before, after);
  }

  bool progression_save_activity(const WorldProgress &before,
                                 const WorldProgress &after) override {
    return commit_instance_activity(*handoff_, before, after, {}).has_value();
  }

  bool character_has_handoff() const override {
    return handoff_.has_value();
  }

  std::uint64_t character_player_unit_id(std::int64_t character_id) const override {
    return player_wire_unit_id(character_id);
  }

  const WorldProgress *character_tutorial_progress() const override {
    return tutorial_player_->current_instance() ? &tutorial_player_->current_instance()->progress()
                                                : nullptr;
  }

  bool character_has_combat() const override {
    return combat_player_->has_simulation();
  }

  const nlohmann::json *character_combat_stats() const override {
    return combat_player_->has_simulation()
               ? &std::as_const(*combat_player_).current_simulation()->player_stats()
               : nullptr;
  }

  void character_update_combat_stats(const nlohmann::json &stats) override {
    combat_player_->current_simulation()->update_player_stats(stats);
  }

  bool character_refill_player_vitals() override {
    return combat_player_->current_simulation()->refill_player_vitals();
  }

  std::optional<int> character_party_level() const override {
    if (!party_session_->identity()) {
      return std::nullopt;
    }
    return party_session_->identity()->level;
  }

  world_metrics::TimedMutex &character_protocol_mutex() const override {
    return state_->protocol_mutex;
  }

  GameplayPacket character_error(const WorldGameplayRequest &request,
                                 x5::ec::EErrorCode code) const override {
    return inventory::InventorySession::equip_error(request, code);
  }

  GameplayPacket character_stat_notification(std::int64_t unit_id, const nlohmann::json &before,
                                             const nlohmann::json &after,
                                             const nlohmann::json &current) const override {
    return polish_stat_notification(unit_id, before, after, current);
  }

  void character_deliver_observers(const std::vector<GameplayPacket> &packets) override {
    deliver_observer_packets(*state_, connection_id(), packets);
  }

  bool quests_has_handoff() const override {
    return handoff_.has_value();
  }

  std::shared_ptr<quests::QuestIntentLedger> quests_intent_ledger() const override {
    return quest_intent_ledger_;
  }

  std::optional<QuestServiceMembership> quests_service_membership() override {
    if (!handoff_) {
      return std::nullopt;
    }
    return state_->auth.read_quest_service_membership(*handoff_);
  }

  std::string quests_new_effect_id() override {
    return random_hex(16);
  }

  std::optional<std::vector<CharacterItem>> quests_commit_unaffiliated(
      const QuestServiceMembership &membership, const WorldProgress &before,
      const WorldProgress &after, const ItemTrade &trade) override {
    if (!handoff_) {
      throw std::runtime_error("Unaffiliated quest transition lost character ownership");
    }
    auto result = state_->auth.commit_unaffiliated_activity(*handoff_, membership, before, after,
                                                             trade);
    if (result && !trade.granted_items.empty()) {
      if (const auto live = state_->auth.world_character(*handoff_)) {
        const auto *stats = combat_player_->has_simulation()
                                ? &combat_player_->current_simulation()->player_stats()
                                : nullptr;
        session_events_.publish(PlayerInventorySnapshotChanged{*live, stats});
      }
    }
    return result;
  }

  QuestMutationResult quests_commit_service_effects(
      const std::string &effect_id, const WorldProgress &before,
      const std::optional<WorldProgress> &after, const ItemTrade &trade,
      const QuestServiceEffects &effects) override {
    if (!handoff_) {
      throw std::runtime_error("Quest service effect lost character ownership");
    }
    auto result = state_->auth.commit_quest_mutation(*handoff_, effect_id, before, after,
                                                      trade, effects);
    if (result.status == QuestMutationStatus::committed && !trade.granted_items.empty()) {
      if (const auto live = state_->auth.world_character(*handoff_)) {
        const auto *stats = combat_player_->has_simulation()
                                ? &combat_player_->current_simulation()->player_stats()
                                : nullptr;
        session_events_.publish(PlayerInventorySnapshotChanged{*live, stats});
      }
    }
    return result;
  }

  QuestMutationResult
  quests_commit_combat_event(const std::string &operation_id, const std::string &source_fingerprint,
                             const WorldProgress &before, const std::optional<WorldProgress> &after,
                             const ItemTrade &trade, const QuestServiceEffects &effects) override {
    if (!handoff_) {
      throw std::runtime_error("Combat quest event lost character ownership");
    }
    auto result = state_->auth.commit_combat_quest_event(
        *handoff_, operation_id, source_fingerprint, before, after, trade, effects);
    if (result.status != QuestMutationStatus::conflict && !trade.granted_items.empty()) {
      const auto live = state_->auth.world_character(*handoff_);
      if (!live) {
        throw std::runtime_error("Committed combat quest inventory is unavailable");
      }
      const auto *stats = combat_player_->has_simulation()
                              ? &combat_player_->current_simulation()->player_stats()
                              : nullptr;
      session_events_.publish(PlayerInventorySnapshotChanged{*live, stats});
    }
    return result;
  }

  void combat_settle_quest_events(CombatSession &simulation, const WorldPosition &position,
                                  std::int64_t ticks,
                                  std::vector<GameplayPacket> &packets) override {
    if (simulation.quest_events().empty()) {
      return;
    }
    try {
      quests_session_->record_combat_events(simulation.quest_events(), position, ticks, packets);
      simulation.take_quest_events();
    } catch (...) {
      // No later producer, death checkpoint or monster reward may change the captured view.
      close_without_location_checkpoint();
      throw;
    }
  }

  bool has_unsettled_combat_quest_events() const {
    return quests_session_->has_pending_combat_events() || !pending_direct_quest_events_.empty() ||
           (combat_player_->has_simulation() &&
            !combat_player_->current_simulation()->quest_events().empty());
  }

  MonsterRewardCommitResult quests_commit_monster_service_effects(
      const std::string &receipt_id, const WorldProgress &before,
      const std::optional<WorldProgress> &after, const ItemTrade &trade,
      const QuestServiceEffects &effects) override {
    if (!handoff_) {
      throw std::runtime_error("Monster quest effect lost character ownership");
    }
    auto result = state_->auth.commit_monster_reward_with_effects(*handoff_, receipt_id, before,
                                                                   after, trade, effects);
    if (result.status == MonsterRewardCommitStatus::committed && !trade.granted_items.empty()) {
      if (const auto live = state_->auth.world_character(*handoff_)) {
        const auto *stats = combat_player_->has_simulation()
                                ? &combat_player_->current_simulation()->player_stats()
                                : nullptr;
        session_events_.publish(PlayerInventorySnapshotChanged{*live, stats});
      }
    }
    return result;
  }

  std::optional<quests::QuestIntentLedger::Sequence>
  quests_begin_mutation(const std::shared_ptr<quests::QuestIntentLedger> &ledger,
                        const WorldProgress &after) override {
    auto prepared = quests::QuestIntentLedger::prepare(after);
    std::lock_guard lock(state_->quest_intent_order_mutex);
    return ledger->begin_prepared(std::move(prepared));
  }

  void quests_watch_mutation(const std::shared_ptr<quests::QuestIntentLedger> &ledger,
                             quests::QuestIntentLedger::Sequence intent,
                             const std::optional<std::string> &receipt_id) override {
    if (pending_quest_reconciliation_ && pending_quest_reconciliation_->ledger == ledger &&
        pending_quest_reconciliation_->intent == intent &&
        pending_quest_reconciliation_->receipt_id == receipt_id) {
      return;
    }
    if (!handoff_ || pending_quest_reconciliation_) {
      throw std::runtime_error("Cannot retain a quest mutation reconciliation owner");
    }
    QuestReconciliation reconciliation{ledger, intent, receipt_id};
    const auto registry = state_->quest_reconciliation_registry.lock();
    if (!registry) {
      throw std::runtime_error("World listener is no longer available for quest reconciliation");
    }
    std::lock_guard lock(state_->character_admission_mutex);
    const auto [entry, inserted] =
        registry->owners.emplace(handoff_->character_id, shared_session());
    if (!inserted) {
      throw std::runtime_error("Character already has a quest reconciliation owner");
    }
    pending_quest_reconciliation_.emplace(std::move(reconciliation));
  }

  bool quests_mark_mutation_resolved(quests::QuestIntentLedger::Sequence intent,
                                     bool receipt_retired) noexcept override {
    if (!pending_quest_reconciliation_ || pending_quest_reconciliation_->intent != intent) {
      return false;
    }
    pending_quest_reconciliation_->ledger_resolved = true;
    pending_quest_reconciliation_->reward_retired = receipt_retired;
    return true;
  }

  void quests_forget_mutation(quests::QuestIntentLedger::Sequence intent) override {
    if (!pending_quest_reconciliation_ || pending_quest_reconciliation_->intent != intent ||
        !handoff_) {
      throw std::logic_error("Unknown quest reconciliation owner");
    }
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
    if (pending_quest_reconciliation_->ledger_resolved &&
        quest_success_forget_fault_armed.exchange(false)) {
      quest_success_forget_fault_observed.store(true);
      throw std::runtime_error("Injected completed quest owner retirement failure");
    }
#endif
    const auto registry = state_->quest_reconciliation_registry.lock();
    if (!registry) {
      throw std::runtime_error("World listener is no longer available for quest reconciliation");
    }
    std::shared_ptr<WorldSession> retained;
    {
      std::lock_guard lock(state_->character_admission_mutex);
      const auto entry = registry->owners.find(handoff_->character_id);
      if (entry == registry->owners.end() || entry->second.get() != this) {
        throw std::logic_error("Quest reconciliation owner changed");
      }
      retained = std::move(entry->second);
      registry->owners.erase(entry);
      pending_quest_reconciliation_.reset();
    }
  }

  void quests_retry_ambiguous_mutation(quests::QuestIntentLedger::Sequence intent) override {
    if (!pending_quest_reconciliation_ || pending_quest_reconciliation_->intent != intent) {
      return;
    }
    // Seal stale writes and activate the independent owner before posting the close.
    // The supervisor retains the operation if close or worker dispatch fails.
    close_without_location_checkpoint();
  }

  void reconcile_ambiguous_quest_mutation() {
    if (quests_session_->has_pending_combat_events()) {
      std::vector<GameplayPacket> departed_packets;
      quests_session_->retry_combat_events(departed_packets, true);
      if (combat_player_->has_simulation()) {
        combat_player_->current_simulation()->take_quest_events();
      }
      pending_direct_quest_events_.clear();
      return;
    }
    if (!pending_quest_reconciliation_ || !handoff_) {
      return;
    }
    auto &pending = *pending_quest_reconciliation_;
    const auto intent = pending.intent;
    const bool is_reward = pending.receipt_id.has_value();
    try {
      if (!pending.ledger_resolved) {
        if (!state_->auth.fence_world_entry(*handoff_)) {
          throw std::runtime_error("Quest reconciliation revision fence was refused");
        }
        const auto progress = state_->auth.world_progress(*handoff_);
        pending.ledger->finish_mutation(intent, progress);
        pending.ledger_resolved = true;
      }
    } catch (const std::exception &error) {
      if (pending.failed_attempts < 10) {
        ++pending.failed_attempts;
      }
      spdlog::error("Quest reconciliation failed connection={} attempt={}: {}. "
                    "Restore database availability; retries continue with capped backoff.",
                    connection_id(), pending.failed_attempts, error.what());
      quests_retry_ambiguous_mutation(intent);
      return;
    } catch (...) {
      if (pending.failed_attempts < 10) {
        ++pending.failed_attempts;
      }
      spdlog::error("Quest reconciliation failed connection={} with an unknown error. "
                    "Restore database availability; retries continue with capped backoff.",
                    connection_id());
      quests_retry_ambiguous_mutation(intent);
      return;
    }
    try {
      if (is_reward && !pending.reward_retired) {
        quests_session_->retire_reconciled_reward_intent(*pending.receipt_id, intent);
        pending.reward_retired = true;
      }
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
      if (quest_retirement_fault_armed.exchange(false)) {
        quest_retirement_fault_observed.store(true);
        throw std::runtime_error("Injected quest owner retirement failure");
      }
#endif
      quests_forget_mutation(intent);
      release_settled_character_admission();
    } catch (const std::exception &error) {
      spdlog::error("Quest reconciliation completed but its owner could not be released "
                    "connection={}: {}. Recovery remains fenced and will retry.",
                    connection_id(), error.what());
    }
  }

  bool quests_has_tutorial() const override {
    return static_cast<bool>(tutorial_player_->current_instance());
  }

  std::uint64_t quests_connection_id() const override {
    return connection_id();
  }

  std::uint64_t quests_player_unit_id(std::int64_t character_id) const override {
    return player_wire_unit_id(character_id);
  }

  const AnnouncedUnit *quests_interaction_npc(std::uint64_t unit_id) const override {
    return npc_player_->interaction_npc(unit_id);
  }

  void quests_apply_level_change(const WorldProgress &progress,
                                 std::vector<GameplayPacket> &packets) override {
    character_session_->apply_level_change(progress, packets);
  }

  std::optional<QuestCompanyStanding> quests_company_standing() const override {
    const auto &companies = state_->company_feature->service();
    if (!companies || !handoff_) {
      return std::nullopt;
    }

    const auto status = companies->status(handoff_->character_id);
    QuestCompanyStanding standing;
    standing.company_type = status.record.company_type;
    standing.contribution = status.record.contribution;
    standing.position_level = status.position_level;
    standing.pioneer_permit_valid = status.permit_valid;
    standing.at_capped_level_ceiling = status.at_capped_level_ceiling;
    return standing;
  }

  bool quests_apply_company_rewards(const QuestCompanyRewards &rewards,
                                     std::vector<GameplayPacket> &packets) override {
    const auto &companies = state_->company_feature->service();
    if (!companies || !handoff_) {
      return false;
    }

    const auto character_id = handoff_->character_id;
    for (const auto &unlock : rewards.unlocks) {
      const auto result =
          companies->unlock_position(character_id, unlock.company_type, unlock.level);
      // A character outside the rewarded company keeps its own standing; the quest itself
      // is already complete and its other rewards stand.
      if (result.error != x5::ec::Success) {
        spdlog::info("Quest company unlock skipped connection={} company={} level={} ec={}",
                     connection_id(), unlock.company_type, unlock.level,
                     x5::ec::EErrorCode_Name(result.error));
        if (result.error != x5::ec::Company_IsNotJoined &&
            result.error != x5::ec::Company_NotMatched) {
          return false;
        }
      }
    }
    if (rewards.contribution <= 0) {
      return true;
    }

    // CompanyContribution rows name no company: the points go to the current company and
    // still respect its capped levels. Non-members gain nothing.
    const auto gain = companies->add_contribution(character_id, rewards.contribution);
    if (!gain) {
      spdlog::warn("Quest company contribution not credited connection={} points={}: no "
                   "company membership or the company record was not stored",
                   connection_id(), rewards.contribution);
      return false;
    }
    packets.push_back(
        company_packet(company_world::kCompanyUpdateNotification, gain->status.record));
    if (gain->position_changed) {
      session_events_.publish(CompanyMembershipChanged{});
    }
    return true;
  }

  std::optional<int> quests_guild_level() const override {
    const auto &guilds = state_->guild_feature->service();
    if (!guilds || !handoff_) {
      return std::nullopt;
    }
    const auto status = guilds->status(handoff_->character_id);
    if (status.guild_id == 0) {
      return std::nullopt;
    }
    return status.guild_level;
  }

  void quests_apply_guild_experience(std::int64_t experience) override {
    const auto &guilds = state_->guild_feature->service();
    if (!guilds || !handoff_ || experience <= 0) {
      return;
    }
    const auto amount =
        static_cast<int>(std::min<std::int64_t>(experience, std::numeric_limits<int>::max()));
    // Characters outside a guild keep the quest's other rewards; the guild points are lost.
    if (!guilds->award_experience(handoff_->character_id, amount)) {
      spdlog::info("Quest guild experience not credited connection={} points={}",
                   connection_id(), amount);
    }
  }

  std::optional<int> quests_hp_percent() const override {
    const auto *stats = character_combat_stats();
    if (!stats) {
      return std::nullopt;
    }
    const auto read_stat = [stats](const char *key) -> std::optional<std::int64_t> {
      const auto found = stats->find(key);
      if (found == stats->end()) {
        return std::nullopt;
      }
      if (found->is_number_integer()) {
        return found->get<std::int64_t>();
      }
      if (!found->is_string()) {
        return std::nullopt;
      }
      const auto text = found->get<std::string>();
      std::int64_t value{};
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
      }
      return value;
    };
    // Stat 69 is current health and stat 18 maximum health (combat.cpp kHealth, kMaxHealth).
    const auto health = read_stat("69");
    const auto maximum = read_stat("18");
    if (!health || !maximum || *maximum <= 0) {
      return std::nullopt;
    }
    return static_cast<int>(std::clamp<std::int64_t>(*health * 100 / *maximum, 0, 100));
  }

  std::optional<int> quests_doodad_phase(std::uint64_t unit_id) const override {
    const auto position = handshake_.current_position();
    if (!position) {
      return std::nullopt;
    }
    return state_->gathering_feature->doodad_phase(position->map_id, unit_id,
                                                   instance_copy_.get());
  }

  std::optional<quests::QuestInstanceItem> quests_instance_item(int item_cms_id) const override {
    const auto *catalog = state_->shop_feature.catalog().get();
    const auto *item = catalog ? catalog->item(item_cms_id) : nullptr;
    if (!item || item->storage != ShopItemStorage::instance) {
      return std::nullopt;
    }
    return quests::QuestInstanceItem{item->grade, item->max_durability};
  }

  std::optional<std::vector<CharacterItem>>
  quests_commit_items(const WorldProgress &before, const WorldProgress &after,
                      const ItemTrade &trade) override {
    auto stored = state_->auth.commit_world_item_trade(*handoff_, before, after, trade);
    if (!stored) {
      return std::nullopt;
    }
    // Keep the cached world snapshot's item list (and combat stats) equal to the inventory.
    if (const auto live = state_->auth.world_character(*handoff_)) {
      const auto *stats = combat_player_->has_simulation()
                              ? &combat_player_->current_simulation()->player_stats()
                              : nullptr;
      session_events_.publish(PlayerInventorySnapshotChanged{*live, stats});
    }
    return stored;
  }

  MonsterRewardCommitResult quests_commit_monster_reward(
      const std::string &receipt_id, const WorldProgress &before,
      const std::optional<WorldProgress> &after, const ItemTrade &trade) override {
    auto result = state_->auth.commit_monster_reward(*handoff_, receipt_id, before, after, trade);
    if (result.status == MonsterRewardCommitStatus::committed && !trade.granted_items.empty()) {
      if (const auto live = state_->auth.world_character(*handoff_)) {
        const auto *stats = combat_player_->has_simulation()
                                ? &combat_player_->current_simulation()->player_stats()
                                : nullptr;
        session_events_.publish(PlayerInventorySnapshotChanged{*live, stats});
      }
    }
    return result;
  }

  bool combat_is_raid_map(int map_id) const override {
    return state_->events_feature->is_raid_map(map_id);
  }

  std::optional<std::uint64_t> combat_raid_instance_owner(std::int64_t character_id,
                                                          int map_id) const override {
    return state_->events_feature->raid_instance_owner(character_id, map_id);
  }

  std::shared_ptr<InstanceCopy> combat_instance_copy(int map_id) const override {
    if (!instance_copy_ || instance_copy_->map_id() != map_id) {
      return {};
    }
    return instance_copy_;
  }

  bool combat_has_handoff() const override {
    return handoff_.has_value();
  }

  std::int64_t combat_character_id() const override {
    return handoff_->character_id;
  }

  std::uint64_t combat_connection_id() const override {
    return connection_id();
  }

  std::uint64_t combat_wire_player_id(std::int64_t character_id) const override {
    return player_wire_unit_id(character_id);
  }

  std::int64_t combat_monotonic_ticks() const override {
    return admitted_combat_tick_.value_or(monotonic_ticks());
  }

  bool combat_uses_ordered_schedule() const override {
    return true;
  }

  bool combat_closed() const override {
    return closed();
  }

  bool combat_services_entered() const override {
    return player_services_entered_;
  }

  bool combat_in_housing() const override {
    return housing_session_->in_housing();
  }

  bool combat_riding() const override {
    return companions_ && companions_->riding();
  }

  bool combat_other_action_busy() const override {
    return other_action_busy();
  }

  bool combat_has_tutorial() const override {
    return static_cast<bool>(tutorial_player_->current_instance());
  }

  const WorldProgress &combat_tutorial_progress() const override {
    return tutorial_player_->current_instance()->progress();
  }

  const std::shared_ptr<const TutorialData> &combat_base_tutorial_data() const override {
    return state_->tutorial_feature.data();
  }

  void combat_accept_tutorial_progress(WorldProgress &&progress) override {
    tutorial_player_->current_instance()->accept_combat_progress(std::move(progress));
  }

  std::vector<GameplayPacket> combat_advance_tutorial(CombatSession &simulation, std::int64_t ticks,
                                                      const WorldPosition &position) override {
    return simulation.advance(ticks, position, *tutorial_player_->current_instance());
  }

  ItemInventoryAccess combat_item_inventory() override {
    ItemInventoryAccess inventory;
    inventory.quantity = [this](int item_cms_id) {
      if (const auto *tutorial = tutorial_player_->current_instance()) {
        return tutorial->stack_quantity(item_cms_id);
      }
      return inventory_session_->stack_quantity(item_cms_id);
    };
    inventory.consume_one = [this](int item_cms_id) {
      if (auto *tutorial = tutorial_player_->current_instance()) {
        return tutorial->consume_used_item(item_cms_id);
      }
      return inventory_session_->consume_skill_item(item_cms_id);
    };
    return inventory;
  }

  bool combat_has_character_stats() const override {
    return state_->character_feature.has_stats();
  }

  nlohmann::json combat_character_stats(const Character &character) const override {
    return state_->character_feature.stats(character);
  }

  std::optional<Character> combat_character() const override {
    return state_->auth.world_character(*handoff_);
  }

  WorldProgress combat_stored_progress() const override {
    return state_->auth.world_progress(*handoff_);
  }

  std::optional<WorldPosition> combat_last_position() const override {
    return handshake_.last_position();
  }

  bool combat_register_pending_death() noexcept override {
    try {
      if (!handoff_) {
        return false;
      }
      const auto registry = state_->quest_reconciliation_registry.lock();
      if (!registry) {
        return false;
      }
      std::lock_guard lock(state_->character_admission_mutex);
      if (!admitted_character_ || *admitted_character_ != handoff_->character_id) {
        return false;
      }
      const auto admission = state_->character_admissions.find(handoff_->character_id);
      if (admission == state_->character_admissions.end() ||
          admission->second != connection_id()) {
        return false;
      }
      const auto entry = registry->death_owners.find(handoff_->character_id);
      if (entry == registry->death_owners.end() || entry->second.get() != this) {
        return false;
      }
      pending_death_owner_ = true;
      return true;
    } catch (...) {
      return false;
    }
  }

  bool combat_forget_pending_death() noexcept override {
    try {
      if (!pending_death_owner_ || !handoff_) {
        return false;
      }
      const auto registry = state_->quest_reconciliation_registry.lock();
      if (!registry) {
        return false;
      }
      {
        std::lock_guard lock(state_->character_admission_mutex);
        const auto entry = registry->death_owners.find(handoff_->character_id);
        if (entry == registry->death_owners.end() || entry->second.get() != this) {
          return false;
        }
        pending_death_owner_ = false;
      }
      return true;
    } catch (...) {
      return false;
    }
  }

  void combat_pending_death_updated() noexcept override {
    // Set the fence synchronously, before any later worker or close checkpoint can run.
    preserve_durable_location_on_close_.store(true);
    try {
      if (!activate_close_supervisor()) {
        spdlog::critical("Could not activate death close recovery connection={}; "
                         "admission remains fenced", connection_id());
      }
      request_shutdown_close();
      if (!pending_death_owner_) {
        return;
      }
      if (death_retry_armed_ || death_recovery_confirmed_) {
        return;
      }
      constexpr unsigned kBurstAttempts = 10;
      const auto seconds = death_failed_attempts_ >= kBurstAttempts
                               ? 60U
                               : std::min(1U << std::min(death_failed_attempts_, 5U), 30U);
      death_reconciliation_timer_.expires_after(std::chrono::seconds(seconds));
      death_reconciliation_timer_.async_wait(
          [self = shared_session()](const boost::system::error_code &error) {
            try {
              asio::post(self->worker_strand_, [self, error] {
                self->death_retry_armed_ = false;
                if (!self->pending_death_owner_) {
                  return;
                }
                if (error) {
                  if (self->death_failed_attempts_ < 10) {
                    ++self->death_failed_attempts_;
                  }
                  spdlog::error("Death reconciliation timer failed connection={}: {}",
                                self->connection_id(), error.message());
                } else {
                  self->reconcile_ambiguous_death();
                }
                if (self->pending_death_owner_ && !self->death_recovery_confirmed_) {
                  self->combat_pending_death_updated();
                }
              });
            } catch (...) {
              spdlog::critical("Death recovery dispatch failed connection={}; "
                               "character admission remains fenced for operator recovery",
                               self->connection_id());
            }
          });
      death_retry_armed_ = true;
    } catch (...) {
      death_retry_armed_ = false;
      try {
        spdlog::critical("Death recovery scheduling failed connection={}; "
                         "character admission remains fenced for operator recovery",
                         connection_id());
      } catch (...) {
      }
    }
  }

  void reconcile_ambiguous_death() {
    const auto result = combat_player_->reconcile_pending_death();
    if (result == combat::CombatPlayerSession::DeathRecovery::committed_reconnect) {
      death_recovery_confirmed_ = true;
      spdlog::info("Death confirmed connection={}; completing close recovery", connection_id());
      return;
    }
    if (death_failed_attempts_ < 10) {
      ++death_failed_attempts_;
    }
    if (result == combat::CombatPlayerSession::DeathRecovery::conflict ||
        result == combat::CombatPlayerSession::DeathRecovery::none) {
      spdlog::critical("Death reconciliation could not establish an outcome connection={}; "
                       "character admission remains fenced", connection_id());
    }
  }

  bool combat_commit_death(std::int64_t revision,
                           const std::vector<EquippedDurabilityChange> &changes) override {
    return state_->auth.commit_world_death(*handoff_, revision, changes);
  }

  std::optional<DeathReconciliationSnapshot> combat_fence_death_reconciliation() override {
    if (!handoff_) {
      throw std::logic_error("Death reconciliation requires a character handoff");
    }
    return state_->auth.fence_world_death_reconciliation(*handoff_);
  }

  bool combat_commit_revival(std::int64_t revision, const WorldPosition &position) override {
    return state_->auth.commit_world_revival(*handoff_, revision, position);
  }

  bool combat_commit_shrine_revival(const WorldProgress &before, const WorldProgress &after,
                                    const WorldPosition &position) override {
    return state_->auth.commit_world_shrine_revival(*handoff_, before, after, position);
  }

  bool combat_has_revival_supplies() const override {
    return state_->nuia_feature.has_statues();
  }

  std::vector<GameplayPacket> combat_revival_supplies(int task_id, WorldProgress &progress,
                                                      const Character &character) const override {
    return state_->nuia_feature.apply_revival_supplies(task_id, progress, character);
  }

  std::vector<GameplayPacket>
  combat_revive_at_shrine(const WorldGameplayRequest &request) override {
    return nuia_session_->revive_at_shrine(request);
  }

  bool combat_uses_durable_monster_rewards() const override {
    return true;
  }

  MonsterRewardCapture combat_snapshot_monster_rewards(const MonsterDefeat &defeat) override {
    if (!handoff_ || !quest_intent_ledger_ || defeat.reward_credit_id == 0 ||
        defeat.reward_event_id.empty()) {
      throw std::runtime_error("World monster defeat lacks a durable identity");
    }
    const auto party_world = state_->party_feature->world();
    MonsterRewardCapture capture;
    const bool cliffs_script = defeat.position.map_id == kCliffsEncounterMapId &&
        (defeat.npc_cms_id == kCliffsEliteBossNpcId ||
         defeat.npc_cms_id == kCliffsCaptainBossNpcId);
    if (cliffs_script) {
      if (!state_->gathering_feature->has_map(defeat.position.map_id)) {
        throw std::runtime_error("Cliffs boss script requires its gathering profile");
      }
      const auto copy_channel = copy_channel_.load();
      const auto scope = copy_channel == 0
                             ? std::string{"shared"}
                             : state_->gathering_process_nonce + ":" +
                                   std::to_string(copy_channel);
      capture.boss_script = BossScriptIntent{defeat.reward_event_id, handoff_->world_id,
                                               defeat.position.map_id, defeat.npc_cms_id, scope};
    }
    if (const auto &catalog = state_->quests_feature.catalog()) {
      capture.relevant_quest_ids = catalog->monster_quest_ids(defeat.npc_cms_id);
    }
    const auto channel = party_session_->identity() ? party_session_->identity()->channel_key : 0;
    party_world->capture_nearby_party_members(
        handoff_->character_id, defeat.position, channel, kGuessedPartyRangeMm,
        [&](const std::optional<party::MemberPresence> &killer,
            const std::vector<party::PartyWorld::NearbyMember> &nearby) {
          // Party -> quest ordering is fixed. This callback only reserves bounded in-memory
          // snapshots; it never performs I/O, waits on workers or reenters PartyWorld.
          std::lock_guard order_lock(state_->quest_intent_order_mutex);
          auto killer_quest_capture = quest_intent_ledger_->acquire_capture();
          if (!killer_quest_capture) {
            throw std::runtime_error("World monster defeat cannot snapshot killer quests");
          }
          const auto killer_position = killer && killer->position ? *killer->position
                                                                   : defeat.position;
          capture.recipients.push_back({handoff_->account_id, handoff_->character_id,
                                        handoff_->world_id, defeat.reward_event_id,
                                        defeat.reward_credit_id, defeat.npc_cms_id,
                                        killer_position, true});
          capture.recipients.back().allow_monster_offered_quests = true;
          capture.quest_captures.push_back(std::move(killer_quest_capture));
          for (const auto &member : nearby) {
            if (!member.alive) {
              continue;
            }
            if (member.account_id <= 0 || member.world_id != handoff_->world_id ||
                !member.quest_ledger) {
              throw std::runtime_error("World monster defeat cannot identify a party recipient");
            }
            auto quest_capture = member.quest_ledger->acquire_capture();
            if (!quest_capture) {
              throw std::runtime_error("World monster defeat cannot snapshot party quests");
            }
            capture.recipients.push_back({member.account_id, member.char_id, member.world_id,
                                          defeat.reward_event_id, defeat.reward_credit_id,
                                          defeat.npc_cms_id, member.position, false});
            capture.recipients.back().allow_monster_offered_quests = true;
            capture.quest_captures.push_back(std::move(quest_capture));
          }
        });
    if (capture.boss_script && !boss_supervisor_registered_) {
      const auto supervisor = state_->boss_wake_supervisor.lock();
      if (!supervisor) {
        throw std::runtime_error("Boss retry supervisor is unavailable");
      }
      const RetainedOwnerKey key{handoff_->character_id, connection_id()};
      std::lock_guard lock(state_->character_admission_mutex);
      const auto owner = state_->character_admissions.find(handoff_->character_id);
      if (!admitted_character_ || *admitted_character_ != handoff_->character_id ||
          owner == state_->character_admissions.end() || owner->second != connection_id() ||
          !supervisor->reserve_owner(key)) {
        throw std::runtime_error("Cannot reserve boss retry owner");
      }
      if (!supervisor->activate_owner(key)) {
        if (!supervisor->cancel_reservation(key)) {
          spdlog::critical("Boss retry reservation rollback failed connection={}; "
                           "shutdown must remain fenced", connection_id());
        }
        throw std::runtime_error("Cannot activate boss retry owner");
      }
      boss_supervisor_registered_ = true;
    }
    return capture;
  }

  bool combat_enqueue_monster_rewards(const std::vector<MonsterRewardReceipt> &receipts) override {
    return state_->auth.enqueue_monster_rewards(receipts);
  }

  bool combat_enqueue_monster_rewards_with_script(
      const std::vector<MonsterRewardReceipt> &receipts,
      const std::optional<BossScriptIntent> &script) override {
    return state_->auth.enqueue_monster_rewards_with_script(receipts, script);
  }

  bool combat_mark_monster_reward_ready(const std::string &event_id) override {
    return state_->auth.mark_monster_reward_ready(event_id);
  }

  bool combat_publish_boss_script(const BossScriptIntent &intent) override {
    if (!live_boss_publication_) {
      const auto expected_scope = instance_copy_
                                      ? state_->gathering_process_nonce + ":" +
                                            std::to_string(instance_copy_->serial())
                                      : std::string{"shared"};
      if (intent.copy_scope != expected_scope) {
        throw std::runtime_error("Boss script no longer belongs to the captured copy");
      }
      auto operation = std::make_shared<LiveBossPublication>();
      operation->intent = intent;
      operation->copy = instance_copy_;
      operation->origin = current_boss_origin_;
      live_boss_publication_ = std::move(operation);
      return false;
    }
    if (live_boss_publication_->intent != intent) {
      throw std::runtime_error("Another boss script is already being published");
    }
    if (live_boss_publication_->origin_wake_failed.exchange(false)) {
      live_boss_publication_->origin_queued = true;
    }
    if (live_boss_publication_->origin_queued && !live_boss_publication_->published) {
      drive_live_boss_publication();
    }
    return live_boss_publication_->published;
  }

  void combat_retire_boss_script(const BossScriptIntent &intent) noexcept override {
    if (live_boss_publication_ && live_boss_publication_->published &&
        live_boss_publication_->intent == intent) {
      live_boss_publication_.reset();
    }
  }

  std::shared_ptr<LiveBossPublication> bind_boss_origin(BossOrigin origin) {
    const auto operation = live_boss_publication_;
    if (!operation || operation->origin_bound || operation->origin_queued ||
        (operation->origin != origin && !closed())) {
      return {};
    }
    operation->origin_bound = true;
    return operation;
  }

  void begin_boss_publication_after_origin(const std::shared_ptr<LiveBossPublication> &operation) {
    if (!operation || live_boss_publication_ != operation) {
      return;
    }
    operation->origin_queued = true;
    drive_live_boss_publication();
  }

  // Called on the I/O executor after the origin response or tick has entered its queue.
  void wake_boss_publication_after_origin(
      const std::shared_ptr<LiveBossPublication> &operation) noexcept {
    if (!operation) {
      return;
    }
    try {
      asio::post(worker_strand_, [self = shared_session(), operation] {
        try {
          self->begin_boss_publication_after_origin(operation);
        } catch (...) {
          operation->origin_wake_failed.store(true);
          self->request_supervised_close();
        }
      });
    } catch (...) {
      operation->origin_wake_failed.store(true);
      try {
        spdlog::critical("Could not wake boss publication connection={}; reward remains fenced",
                         connection_id());
      } catch (...) {
      }
    }
  }

  void prepare_live_boss_delivery(LiveBossPublication &operation) {
    const auto channel = operation.copy ? operation.copy->serial() : 0;
    const auto nodes = state_->gathering_feature->nodes_for(operation.copy.get());
    auto budget = gathering::ConstructionBudget::acquire();
    budget->charge(gathering::ConstructionBudget::kSectorBytes);
    const auto packets = gathering_state_packets(*operation.changes, nodes, channel);
    if (packets.empty() || packets.size() > 2) {
      throw BossPublicationOversize();
    }
    std::vector<RetainedFrameDelivery::Target> targets;
    std::vector<std::weak_ptr<WorldSession>> recipients;
    std::vector<GatheringViewIdentity> views;
    {
      std::lock_guard lock(state_->protocol_mutex);
      std::vector<Frame> prototypes;
      prototypes.reserve(packets.size());
      std::size_t shared_bytes = gathering::ConstructionBudget::kSectorBytes;
      std::size_t per_recipient_frames = 0;
      for (const auto &packet : packets) {
        if (!packet.local_plan || !packet.local_plan->source_sector()) {
          throw std::logic_error("Boss notification lacks its source plan");
        }
        const auto retained = local_packet_retained_bytes(packet);
        constexpr std::size_t kMaximumBossWireBytes = 4096;
        if (shared_bytes > gathering::ConstructionBudget::kMaximumBatchBytes -
                               kMaximumBossWireBytes ||
            retained > gathering::ConstructionBudget::kMaximumBatchBytes -
                           kMaximumBossWireBytes - shared_bytes) {
          throw BossPublicationOversize();
        }
        const auto prototype_bytes = retained + kMaximumBossWireBytes;
        budget->charge(prototype_bytes);
        shared_bytes += prototype_bytes;
        auto frame = state_->protocol.encode(packet.opcode, packet.message);
        if (frame.bytes.size() > kMaximumBossWireBytes) {
          throw BossPublicationOversize();
        }
        frame.shared = std::make_shared<const Bytes>(std::move(frame.bytes));
        frame.local_retained_bytes = retained;
        frame.local_allocation = std::make_shared<const LocalFrameAllocation>(
            LocalFrameAllocation{retained, packet.local_construction});
        frame.local_plan = packet.local_plan;
        frame.local_message = std::make_shared<const nlohmann::json>(packet.message);
        const auto frame_bytes = RetainedFrameDelivery::frame_bytes(frame);
        if (!frame_bytes || *frame_bytes > RetainedFrameDelivery::kMaximumBytes -
                                               per_recipient_frames) {
          throw BossPublicationOversize();
        }
        per_recipient_frames += *frame_bytes;
        prototypes.push_back(std::move(frame));
      }
      const auto page_limit = boss_recipient_page_limit(per_recipient_frames, prototypes.size(),
                                                         shared_bytes);
      budget->charge(page_limit * (per_recipient_frames + gathering::ConstructionBudget::kNodeBytes));
      if (!operation.sealed) {
        const auto next = state_->next_connection_id.load();
        if (next == 0) {
          throw std::overflow_error("World connection identifiers are exhausted");
        }
        operation.high_water = next - 1;
        operation.sealed = true;
      }
      const auto page = select_boss_recipient_page(state_->sessions, operation.cursor,
                                                    operation.high_water, page_limit);
      operation.page_end = page.end;
      operation.last_page = page.last;

      for (const auto connection : page.connection_ids) {
        const auto entry = state_->sessions.find(connection);
        const auto recipient = entry == state_->sessions.end() ? nullptr : entry->second.lock();
        if (!recipient || recipient->closed()) {
          continue;
        }
        const auto view = recipient->current_gathering_view();
        if (view.map_id != operation.intent.map_id || view.copy_channel != channel) {
          continue;
        }
        std::vector<Frame> frames;
        frames.reserve(prototypes.size());
        for (std::size_t index = 0; index < packets.size(); ++index) {
          const auto &packet = packets[index];
          const auto sector = packet.local_plan ? packet.local_plan->source_sector() : std::nullopt;
          if (!sector || !recipient->gathering_sector_ready(view, *sector)) {
            continue;
          }
          auto frame = prototypes[index];
          recipient->bind_gathering_frame(frame, view);
          frames.push_back(std::move(frame));
        }
        if (frames.empty()) {
          continue;
        }
        targets.push_back({connection, view, std::move(frames)});
        recipients.push_back(recipient);
        views.push_back(view);
      }
    }
    auto delivery = std::make_shared<RetainedFrameDelivery>(operation.intent.event_id,
                                                             std::move(targets));
    operation.recipients = std::move(recipients);
    operation.views = std::move(views);
    operation.posted_at.assign(operation.recipients.size(), Clock::time_point{});
    operation.budget = std::move(budget);
    operation.delivery = std::move(delivery);
  }

  void drive_live_boss_publication() {
    const auto operation = live_boss_publication_;
    if (!operation || !operation->origin_queued || operation->published || operation->oversize) {
      return;
    }
    try {
      if (!operation->lease) {
        operation->lease = state_->gathering_feature->reserve_boss_script_sources(
            operation->intent, operation->copy.get());
        if (!operation->lease) {
          return;
        }
      }
      if (!operation->changes) {
        if (!operation->lease->mark_write_started()) {
          throw std::runtime_error("Boss script source lease is unavailable");
        }
        if (operation->copy) {
          operation->changes = operation->lease->install_private(monotonic_ticks());
        } else {
          const auto settlement = state_->auth.settle_shared_boss_script(operation->intent);
          operation->changes = operation->lease->install(settlement, unix_epoch_ticks_now(),
                                                         monotonic_ticks());
        }
        if (!operation->changes) {
          return;
        }
      }
      if (!operation->delivery) {
        prepare_live_boss_delivery(*operation);
      }
      for (std::size_t index = 0; index < operation->recipients.size(); ++index) {
        const auto recipient = operation->recipients[index].lock();
        if (!recipient || recipient->closed() ||
            recipient->current_gathering_view() != operation->views[index]) {
          operation->delivery->confirm_departed(index);
          continue;
        }
        if (operation->delivery->needs_rebuild(index) ||
            operation->delivery->close_required(index)) {
          recipient->request_close();
          continue;
        }
        auto attempt = operation->delivery->begin(index);
        if (!attempt) {
          constexpr auto kDeliveryTimeout = std::chrono::seconds(30);
          if (operation->posted_at[index] != Clock::time_point{} &&
              Clock::now() - operation->posted_at[index] >= kDeliveryTimeout &&
              operation->delivery->timeout(index)) {
            recipient->request_close();
          }
          continue;
        }
        operation->posted_at[index] = Clock::now();
        std::weak_ptr<WorldSession> owner = shared_session();
        std::weak_ptr<WorldSession> weak_recipient = recipient;
        const auto delivery = operation->delivery;
        recipient->post_frame(attempt->frame,
            [owner, weak_recipient, operation, delivery,
             attempt = *attempt](FrameEnqueueStatus status) {
              const auto current = weak_recipient.lock();
              const bool same_view = current && !current->closed() &&
                  current->current_gathering_view() == attempt.view;
              operation->completion.finish(*delivery, attempt, status, same_view);
              if (const auto session = owner.lock()) {
                session->combat_schedule_monster_reward_retry();
              }
            });
      }
      if (operation->completion.invariant_failed()) {
        spdlog::critical("Boss script delivery completion is inconsistent event={}; "
                         "publication remains fenced", operation->intent.event_id);
        return;
      }
      if (!operation->completion.complete(*operation->delivery)) {
        return;
      }
      if (!operation->last_page) {
        operation->cursor = operation->page_end;
        operation->delivery.reset();
        operation->recipients.clear();
        operation->views.clear();
        operation->posted_at.clear();
        operation->budget.reset();
        combat_schedule_monster_reward_retry();
        return;
      }
      const bool published = operation->copy
                                 ? state_->auth.mark_private_boss_script_applied(operation->intent)
                                 : state_->auth.mark_boss_script_published(operation->intent);
      if (!published || !operation->lease->acknowledge_publication()) {
        throw std::runtime_error("Boss script publication acknowledgement is unresolved");
      }
      operation->published = true;
      state_->gathering_feature->schedule_respawn();
    } catch (const BossPublicationOversize &error) {
      operation->oversize = true;
      spdlog::critical("Boss script event={} cannot fit its delivery budget: {}; "
                       "publication remains fenced for operator recovery",
                       operation->intent.event_id, error.what());
    } catch (const std::exception &error) {
      spdlog::error("Boss script publication remains pending event={}: {}",
                    operation->intent.event_id, error.what());
    } catch (...) {
      spdlog::error("Boss script publication remains pending event={}: unknown error",
                    operation->intent.event_id);
    }
  }

  void process_pending_monster_rewards(std::vector<GameplayPacket> *deferred_packets = nullptr) {
    if (!handoff_ || tutorial_player_->current_instance()) {
      return;
    }
    if (has_unsettled_combat_quest_events()) {
      combat_schedule_monster_reward_retry();
      return;
    }
    try {
      for (;;) {
        const auto pending = state_->auth.pending_monster_rewards(*handoff_);
        if (pending.empty()) {
          break;
        }
        for (const auto &receipt : pending) {
          std::vector<GameplayPacket> packets;
          quests_session_->record_queued_monster_reward(receipt, packets);
          if (deferred_packets) {
            deferred_packets->insert(deferred_packets->end(),
                                     std::make_move_iterator(packets.begin()),
                                     std::make_move_iterator(packets.end()));
          } else {
            for (const auto &packet : packets) {
              combat_send_owner(packet);
            }
          }
        }
      }
    } catch (const std::exception &error) {
      spdlog::error("Pending monster reward processing failed connection={}: {}", connection_id(),
                    error.what());
      combat_schedule_monster_reward_retry();
    }
  }

  void combat_notify_monster_rewards(const std::vector<MonsterRewardReceipt> &receipts) override {
    const auto party_world = state_->party_feature->world();
    for (const auto &receipt : receipts) {
      if (handoff_ && receipt.character_id == handoff_->character_id) {
        // A disconnected owner cannot deliver quest updates. Leave its durable receipt for
        // the next admitted session to drain after the old encounter has finished.
        if (!closed()) {
          process_pending_monster_rewards();
        }
        continue;
      }
      const auto connection = party_world->connection_of(receipt.character_id);
      if (!connection) {
        continue;
      }
      std::shared_ptr<WorldSession> recipient;
      {
        std::lock_guard lock(state_->protocol_mutex);
        const auto found = state_->sessions.find(*connection);
        if (found != state_->sessions.end()) {
          recipient = found->second.lock();
        }
      }
      if (recipient) {
        asio::post(recipient->worker_strand_, [recipient, receipt] {
          if (!recipient->closed() && recipient->handoff_ &&
              recipient->handoff_->account_id == receipt.account_id &&
              recipient->handoff_->character_id == receipt.character_id &&
              recipient->handoff_->world_id == receipt.world_id) {
            recipient->process_pending_monster_rewards();
          }
        });
      }
    }
  }

  void run_monster_reward_retry(std::uint64_t ticket) {
    if (!monster_reward_retry_gate_.finish(ticket)) {
      return;
    }
    try {
      run_monster_reward_retry_body();
    } catch (const std::exception &error) {
      spdlog::error("Monster reward retry failed; recovery remains fenced: {}", error.what());
      try {
        request_close();
      } catch (...) {
        spdlog::critical("Could not close failed monster reward owner; recovery remains fenced");
      }
      combat_schedule_monster_reward_retry();
    } catch (...) {
      spdlog::critical("Monster reward retry failed; recovery remains fenced");
      try {
        request_close();
      } catch (...) {
        spdlog::critical("Could not close failed monster reward owner; recovery remains fenced");
      }
      combat_schedule_monster_reward_retry();
    }
  }

  void run_monster_reward_retry_body() {
    std::vector<GameplayPacket> packets;
    current_boss_origin_ = BossOrigin::retry;
    if (combat_player_->has_pending_world_defeats()) {
      combat_player_->retry_pending_world_monster_rewards(packets);
    }
    if (!closed()) {
      process_pending_monster_rewards();
      for (const auto &packet : packets) {
        combat_send_owner(packet);
      }
    } else if (!combat_player_->has_pending_world_defeats()) {
      release_settled_character_admission();
    } else if (!monster_reward_retry_gate_.armed()) {
      combat_schedule_monster_reward_retry();
    }

    const auto operation = live_boss_publication_;
    if (operation && !operation->origin_queued && !closed() &&
        Clock::now() - operation->created_at >= std::chrono::seconds(30)) {
      request_close();
    }
    const auto boss_origin = bind_boss_origin(BossOrigin::retry);
    if (!boss_origin) {
      return;
    }
    try {
      asio::post(io_executor(), [self = shared_session(), boss_origin] {
        self->wake_boss_publication_after_origin(boss_origin);
      });
    } catch (...) {
      boss_origin->origin_bound = false;
      spdlog::critical("Could not schedule boss origin barrier; reward remains fenced");
    }
  }

  void on_monster_reward_retry_timer(std::uint64_t ticket,
                                     const boost::system::error_code &error) noexcept {
    if (!monster_reward_retry_gate_.is_active(ticket)) {
      return;
    }
    if (error) {
      monster_reward_retry_gate_.finish(ticket);
      spdlog::critical("Monster reward retry timer failed; recovery remains fenced: {}",
                       error.message());
      return;
    }
    try {
      asio::post(worker_strand_, [self = shared_session(), ticket] {
        self->run_monster_reward_retry(ticket);
      });
    } catch (...) {
      monster_reward_retry_gate_.finish(ticket);
      spdlog::critical("Monster reward retry worker dispatch failed; recovery remains fenced");
    }
  }

  void arm_monster_reward_retry_timer(std::uint64_t ticket) noexcept {
    if (!monster_reward_retry_gate_.is_active(ticket)) {
      return;
    }
    try {
      monster_reward_retry_timer_.expires_after(std::chrono::seconds(2));
      monster_reward_retry_timer_.async_wait(
          [self = shared_session(), ticket](const boost::system::error_code &error) {
            self->on_monster_reward_retry_timer(ticket, error);
          });
    } catch (...) {
      monster_reward_retry_gate_.finish(ticket);
      spdlog::critical("Monster reward retry timer setup failed; recovery remains fenced");
    }
  }

  void combat_schedule_monster_reward_retry() override {
    const auto ticket = monster_reward_retry_gate_.arm();
    if (!ticket) {
      if (!monster_reward_retry_gate_.armed()) {
        spdlog::critical("Monster reward retry generation exhausted; recovery remains fenced");
      }
      return;
    }
    try {
      asio::post(io_executor(), [self = shared_session(), ticket = *ticket] {
        self->arm_monster_reward_retry_timer(ticket);
      });
    } catch (...) {
      monster_reward_retry_gate_.finish(*ticket);
      spdlog::critical("Monster reward retry I/O dispatch failed; recovery remains fenced");
    }
  }

  void combat_record_monster_reward(const WorldPosition &, const MonsterDefeat &defeat,
                                    std::vector<GameplayPacket> &packets) override {
    const auto source_view = gathering_view_.snapshot();
    const auto source_nodes = state_->gathering_feature->nodes_for(instance_copy_.get());
    const auto changes = state_->gathering_feature->on_monster_defeated(
        defeat.position.map_id, defeat.npc_cms_id, monotonic_ticks(), instance_copy_.get());
    if (!changes.empty()) {
      auto notifications = gathering_state_packets(changes, source_nodes, source_view.copy_channel);
      for (auto &packet : notifications) {
        if (packet.local_plan) {
          packet.local_view_epoch = source_view.epoch;
        }
      }
      packets.insert(packets.end(), std::make_move_iterator(notifications.begin()),
                     std::make_move_iterator(notifications.end()));
      deliver_gathering_changes(*state_, changes, source_view.copy_channel, connection_id());
    }
  }

  void combat_sync_protection() override {
    sync_gm_combat_protection();
  }

  std::optional<std::int64_t> combat_next_gameplay_tick() const override {
    return next_gameplay_tick();
  }

  void combat_publish_snapshot() override {
    session_events_.publish(PlayerCombatSnapshotChanged{});
  }

  void combat_publish_committed_life(bool dead, const nlohmann::json &stats) override {
    session_events_.publish(PlayerLifeCommitted{dead, stats});
  }

  void combat_send_owner(const GameplayPacket &packet) override {
    Frame frame;
    {
      std::lock_guard lock(state_->protocol_mutex);
      const auto retained_bytes = packet.local_plan ? local_packet_retained_bytes(packet) : 0;
      frame = state_->protocol.encode(packet.opcode, packet.message);
      if (packet.local_plan) {
        frame.local_retained_bytes = retained_bytes;
        frame.local_allocation = std::make_shared<const LocalFrameAllocation>(
            LocalFrameAllocation{frame.local_retained_bytes, packet.local_construction});
        frame.local_plan = packet.local_plan;
        frame.local_message = std::make_shared<const nlohmann::json>(packet.message);
        auto view = gathering_view_.snapshot();
        if (packet.local_view_epoch) {
          view = {*packet.local_view_epoch, packet.local_plan->expected_map(),
                  packet.local_plan->expected_channel()};
        }
        bind_gathering_frame(frame, view);
      }
    }
    post_frame(std::move(frame));
  }

  void combat_send_observers(const std::vector<GameplayPacket> &packets) override {
    deliver_observer_packets(*state_, connection_id(), packets);
  }

  std::vector<GameplayPacket> combat_interrupt_gathering(std::int64_t ticks) override {
    if (!gathering_ || !gathering_->busy()) {
      return {};
    }
    auto outcome = gathering_->interrupt(ticks);
    if (!outcome.observers.empty()) {
      gathering_deliver_observers(outcome.observers);
    }
    return std::move(outcome.packets);
  }

  std::vector<GameplayPacket> combat_select_weapon_for_skill(const nlohmann::json &slot) override {
    return inventory_session_->select_weapon_for_skill(slot);
  }

  void combat_attach_world_loot() override {
    loot_player_->attach_world_loot();
  }

  SessionEventObservers session_event_observers() {
    SessionEventObservers observers;
    observers.protection = [this](const PlayerProtectionChanged &event) {
      if (combat_player_->has_simulation()) {
        combat_player_->current_simulation()->set_gm_protection(event.invincible, event.hidden,
                                                                event.infinite_stamina);
      }
    };
    observers.visibility = [this](const PlayerVisibilityChanged &event) {
      deliver_presence_events(state_->presence.set_hidden(event.connection_id, event.hidden));
    };
    observers.combat_snapshot = [this](const PlayerCombatSnapshotChanged &) {
      // Read the current model directly. Calling the publisher here would recurse through
      // the same synchronous snapshot topic. Preserve the old owned stats copy.
      const auto stats = std::as_const(*combat_player_).current_simulation()->player_stats();
      const bool dead = std::as_const(*combat_player_).current_simulation()->player_dead();
      handshake_.set_authoritative_player_state(stats, dead);
    };
    observers.company_membership = [this](const CompanyMembershipChanged &) {
      guild_session_->refresh_member();
    };
    observers.progress_committed = [this](const PlayerProgressCommitted &event) {
      if (tutorial_player_->current_instance()) {
        tutorial_player_->current_instance()->adopt_committed_progress(event.progress);
      }
      if (contents_guide_session_) {
        pending_guide_progress_ = event.progress;
      }
    };
    observers.inventory_stats = [this](const PlayerInventoryStatsChanged &event) {
      inventory_session_->on_inventory_stats_changed(event.character);
    };
    observers.inventory_snapshot = [this](const PlayerInventorySnapshotChanged &event) {
      // Callers retain their original codec-lock scope; this sink never acquires it.
      inventory_session_->on_inventory_snapshot_changed(event.character, event.prepared_stats);
    };
    observers.activity_experience = [this](const CharacterActivityExperienceGained &event) {
      guild_session_->record_activity_experience(event.character_id, event.experience);
    };
    observers.equipment_revision = [this](const PlayerEquipmentRevisionCommitted &event) {
      if (tutorial_player_->current_instance()) {
        tutorial_player_->current_instance()->accept_equipment_revision(event.revision);
      }
    };
    observers.world_loot_progress = [this](const PlayerWorldLootProgressCommitted &event) {
      loot_player_->on_world_progress_committed(event.progress);
      if (contents_guide_session_) {
        pending_guide_progress_ = event.progress;
      }
    };
    observers.progression_snapshot = [this](const PlayerProgressionSnapshotChanged &event) {
      // Synchronous publication retains the caller's original codec-lock scope.
      // The handshake copies the borrowed build into its one owned Character cache.
      handshake_.refresh_progression(event.level, event.experience, event.build);
    };
    observers.player_level = [this](const PlayerLevelChanged &event) {
      party_session_->update_level(event.level);
    };
    observers.player_life = [this](const PlayerLifeCommitted &event) {
      if (companions_) {
        companions_->observe_committed_life(event.dead);
      }
      state_->presence.refresh_player_life(connection_id(), event.dead, event.stats);
    };
    return observers;
  }

  WorldProgress load() override {
    return current_progress();
  }

  std::optional<Character> character() override {
    return state_->auth.world_character(*handoff_);
  }

  std::optional<std::vector<CharacterItem>>
  commit(const WorldProgress &before, const WorldProgress &after, const ItemTrade &trade) override {
    return state_->auth.commit_world_item_trade(*handoff_, before, after, trade);
  }

  std::optional<std::int64_t> shop_character_id() const override {
    if (!handoff_) {
      return std::nullopt;
    }
    return handoff_->character_id;
  }

  bool shop_has_tutorial() const override {
    return tutorial_player_->current_instance() != nullptr;
  }

  std::int64_t shop_monotonic_ticks() const override {
    return combat_monotonic_ticks();
  }

  std::int64_t shop_epoch_ticks() const override {
    return epoch_ticks();
  }

  std::optional<MerchantTarget> shop_target(std::uint64_t unit_id) const override {
    const auto *unit = npc_player_->interaction_npc(unit_id);
    if (!unit) {
      return std::nullopt;
    }
    MerchantTarget target;
    target.unit_id = unit->unit_id;
    target.npc_cms_id = unit->npc_template_id;
    target.position = {unit->map_id, unit->position.x_mm, unit->position.y_mm, unit->position.z_mm,
                       0.0F};
    return target;
  }

  std::optional<WorldPosition> shop_position() const override {
    return handshake_.current_position();
  }

  const trade::TradeCatalog *shop_trade_catalog() const override {
    return state_->trade_feature.catalog().get();
  }

  bool shop_has_weight_measurement() const override {
    return state_->inventory_feature->has_capacity();
  }

  shop::MerchantWeightMeasurement shop_measure_weight(const Character &character,
                                                      const WorldProgress &progress) override {
    const auto measured = state_->inventory_feature->measure_weight(character, progress);
    return {measured.total, measured.safe};
  }

  const nlohmann::json *shop_player_stats() const override {
    return combat_player_->has_simulation()
               ? &std::as_const(*combat_player_).current_simulation()->player_stats()
               : nullptr;
  }

  ConditionContext shop_condition_context() const override {
    ConditionContext conditions;
    const auto &companies = state_->company_feature->service();
    if (!companies || !handoff_) {
      return conditions;
    }

    const auto status = companies->status(handoff_->character_id);
    const auto company_name = company_type_name(status.record.company_type);
    if (!company_name.empty()) {
      conditions.company_levels[company_name] = status.position_level;
    }
    conditions.has_pioneer_permit = status.permit_valid;
    return conditions;
  }

  bool market_ready() const override {
    return handoff_.has_value() && player_services_entered_;
  }

  bool market_has_tutorial() const override {
    return tutorial_player_->current_instance() != nullptr;
  }

  std::int64_t market_epoch_ticks() const override {
    return unix_epoch_ticks_now();
  }

  const nlohmann::json *market_player_stats() const override {
    return combat_player_->has_simulation()
               ? &std::as_const(*combat_player_).current_simulation()->player_stats()
               : nullptr;
  }

  void market_append_encumbrance(std::vector<GameplayPacket> &packets) override {
    inventory_session_->append_encumbrance_changes(packets, false);
  }

  bool mail_closed() const override {
    return closed();
  }

  bool mail_entered() const override {
    return player_services_entered_;
  }

  std::optional<mail::MailOwner> mail_owner() const override {
    if (!handoff_) {
      return std::nullopt;
    }
    return mail::MailOwner{handoff_->account_id, handoff_->character_id, handoff_->world_id};
  }

  bool mail_has_tutorial() const override {
    return tutorial_player_->current_instance() != nullptr;
  }

  std::int64_t mail_epoch_ticks() const override {
    return epoch_ticks();
  }

  const nlohmann::json *mail_player_stats() const override {
    return combat_player_->has_simulation()
               ? &std::as_const(*combat_player_).current_simulation()->player_stats()
               : nullptr;
  }

  world_metrics::TimedMutex &mail_protocol_mutex() const override {
    return state_->protocol_mutex;
  }

  Frame mail_encode(const GameplayPacket &packet) override {
    return state_->protocol.encode(packet.opcode, packet.message);
  }

  void mail_append_encumbrance(std::vector<GameplayPacket> &packets) override {
    inventory_session_->append_encumbrance_changes(packets, false);
  }

  void mail_post_frame(Frame frame) override {
    post_frame(std::move(frame));
  }

  void mail_disconnect_after_uncertain_commit() override {
    admin_disconnect();
  }

  const WorldProgress *item_use_tutorial_progress() const override {
    return tutorial_player_->current_instance() ? &tutorial_player_->current_instance()->progress()
                                                : nullptr;
  }

  const nlohmann::json *item_use_player_stats() const override {
    return combat_player_->has_simulation()
               ? &std::as_const(*combat_player_).current_simulation()->player_stats()
               : nullptr;
  }

  bool item_use_has_combat() const override {
    return combat_player_->has_simulation();
  }

  const std::vector<int> *item_use_effect_list(int item_cms_id) const override {
    return std::as_const(*combat_player_).current_simulation()->item_effect_list(item_cms_id);
  }

  bool item_use_effect_ready() const override {
    return !std::as_const(*combat_player_).current_simulation()->player_dead() &&
           handshake_.current_position().has_value();
  }

  std::vector<GameplayPacket> item_use_apply_effects(const std::vector<int> &effect_ids) override {
    const auto position = *handshake_.current_position();
    const auto ticks = combat_monotonic_ticks();
    auto &simulation = *combat_player_->current_simulation();
    auto packets = simulation.apply_item_effects(effect_ids, position, ticks);
    combat_settle_quest_events(simulation, position, ticks, packets);
    return packets;
  }

  std::optional<std::int64_t> item_use_next_gameplay_tick() const override {
    return next_gameplay_tick();
  }

  bool item_use_is_quest_item(int item_cms_id) const override {
    const auto &catalog = state_->quests_feature.catalog();
    return catalog && catalog->is_quest_use_item(item_cms_id);
  }

  void item_use_record_quest_events(QuestEvents events, FrameContext &frame) override {
    if (events.empty() || !frame.gameplay_position) {
      return;
    }
    if (!pending_direct_quest_events_.empty()) {
      throw std::logic_error("A previous direct item quest event is unresolved");
    }
    pending_direct_quest_events_ = std::move(events);
    try {
      quests_session_->record_combat_events(pending_direct_quest_events_, *frame.gameplay_position,
                                            combat_monotonic_ticks(), frame.session_packets);
      pending_direct_quest_events_.clear();
    } catch (...) {
      close_without_location_checkpoint();
      throw;
    }
  }

  GatheringActorIdentity gathering_actor_identity() const override {
    std::lock_guard lock(state_->character_admission_mutex);
    if (!handoff_ || !admitted_character_ || *admitted_character_ != handoff_->character_id) {
      throw std::runtime_error("Gathering requires an admitted character");
    }
    const auto channel = copy_channel_.load();
    const auto scope = channel == 0 ? std::string{"shared"}
                                    : state_->gathering_process_nonce + ":" +
                                          std::to_string(channel);
    return {handoff_->account_id, handoff_->character_id, handoff_->world_id, channel, scope};
  }

  bool gathering_register_recovery(
      std::shared_ptr<GatheringRecoveryOperation> operation) override {
    constexpr std::size_t kMaximumPendingGatheringRecoveries = 4096;

    if (!operation || !handoff_) {
      return false;
    }
    const auto &plan = operation->plan();
    if (plan.account_id != handoff_->account_id ||
        plan.character_id != handoff_->character_id || plan.world_id != handoff_->world_id) {
      return false;
    }
    const auto identity = gathering_actor_identity();
    if (plan.channel != identity.channel || plan.source_scope != identity.source_scope) {
      return false;
    }

    std::lock_guard lock(state_->character_admission_mutex);
    if (!admitted_character_ || *admitted_character_ != plan.character_id) {
      return false;
    }
    const auto admission = state_->character_admissions.find(plan.character_id);
    if (admission == state_->character_admissions.end() ||
        admission->second != connection_id() ||
        state_->pending_gathering_recoveries.size() >= kMaximumPendingGatheringRecoveries) {
      return false;
    }
    return state_->pending_gathering_recoveries
        .emplace(plan.character_id, PendingGatheringRecovery{std::move(operation), false})
        .second;
  }

  void gathering_recovery_updated(
      std::shared_ptr<GatheringRecoveryOperation> operation) override {
    if (!operation || !handoff_ || operation->plan().character_id != handoff_->character_id) {
      throw std::runtime_error("Invalid gathering recovery owner");
    }
    const auto character_id = handoff_->character_id;
    if (operation->status() == GatheringRecoveryStatus::pending) {
      schedule_gathering_recovery(state_, io_executor(), worker_strand_, character_id, operation);
      return;
    }

    std::lock_guard lock(state_->character_admission_mutex);
    const auto entry = state_->pending_gathering_recoveries.find(character_id);
    if (entry == state_->pending_gathering_recoveries.end() ||
        entry->second.operation != operation) {
      throw std::runtime_error("Gathering recovery owner was not registered");
    }
    state_->pending_gathering_recoveries.erase(entry);
  }

  void gathering_force_reconnect() override {
    close_without_location_checkpoint();
  }

  GatheringPreparedQuest gathering_prepare_quest(
      const QuestEvents &events, const WorldPosition &position, const WorldProgress &before,
      const WorldProgress &gathered, const ItemTrade &trade) override {
    auto plan = std::make_shared<quests::GatheringQuestPlan>(
        quests_session_->plan_gathering_events(events, position, before, gathered, trade));
    GatheringPreparedQuest prepared;
    prepared.after = plan->after;
    prepared.trade = plan->trade;
    prepared.effects = plan->effects;
    prepared.packets = [plan](const std::vector<CharacterItem> &stored) {
      quests::QuestsSession::finalize_gathering_packets(*plan, stored);
      return plan->packets;
    };

    if (!events.empty() || plan->after.quests != before.quests) {
      auto intent = quests_session_->reserve_gathering_intent(plan->after);
      if (!intent) {
        throw std::runtime_error("Cannot reserve the gathering quest intent");
      }
      try {
        prepared.finalize_intent = [intent](bool committed) {
          intent->finish(committed);
        };
      } catch (...) {
        intent->finish(false);
        throw;
      }
    }
    return prepared;
  }

  std::vector<GameplayPacket> gathering_publish_committed(
      const WorldProgress &before, const WorldProgress &after,
      const std::vector<CharacterItem> &rows) override {
    static_cast<void>(before);
    static_cast<void>(rows);
    std::vector<GameplayPacket> packets;
    session_events_.publish(PlayerWorldLootProgressCommitted{after});
    quests_apply_level_change(after, packets);
    return packets;
  }

  bool gathering_has_tutorial() const override {
    return tutorial_player_->current_instance() != nullptr;
  }

  bool gathering_conditions_met(const nlohmann::json &conditions,
                                const WorldPosition &position) override {
    return quests_session_->requirements_met(conditions, position);
  }

  bool gathering_sector_streaming() const override {
    return state_->sector_grid.has_value();
  }

  bool gathering_other_action_busy() const override {
    return (tutorial_player_->current_instance() && tutorial_player_->current_instance()->busy()) ||
           (combat_player_->has_simulation() && combat_player_->busy()) || nuia_session_->busy() ||
           (processing_ && processing_->busy());
  }

  std::int64_t gathering_monotonic_ticks() const override {
    return combat_monotonic_ticks();
  }

  std::vector<GameplayPacket> gathering_advance_combat(const WorldPosition &position,
                                                       std::int64_t ticks) override {
    if (!combat_player_->has_simulation()) {
      return {};
    }
    sync_gm_combat_protection();
    auto packets = combat_player_->advance_session_combat(ticks, position);
    combat_player_->persist_player_death(packets);
    combat_player_->synchronize_player_state();
    return packets;
  }

  bool gathering_player_dead() const override {
    return combat_player_->has_simulation() &&
           std::as_const(*combat_player_).current_simulation()->player_dead();
  }

  std::optional<std::vector<GameplayPacket>>
  gathering_begin_stamina(const GatheringStaminaFlow &flow) override {
    if (!combat_player_->has_simulation()) {
      return std::vector<GameplayPacket>{};
    }
    return combat_player_->current_simulation()->begin_interaction_stamina(
        flow.cost, flow.wire_start_ns, flow.execute_ms, flow.regen_resume_ms, flow.server_ticks);
  }

  bool gathering_spend_stamina(std::int64_t ticks) override {
    if (!combat_player_->has_simulation()) {
      return false;
    }
    combat_player_->current_simulation()->spend_interaction_stamina(ticks);
    return true;
  }

  std::optional<GameplayPacket> gathering_cancel_stamina(std::int64_t ticks) override {
    if (!combat_player_->has_simulation()) {
      return std::nullopt;
    }
    return combat_player_->current_simulation()->cancel_interaction_stamina(ticks);
  }

  bool gathering_effect_available(int effect_id) const override {
    return combat_player_->has_simulation() &&
           std::as_const(*combat_player_).current_simulation()->can_apply_stat_recovery(effect_id);
  }

  std::vector<GameplayPacket> gathering_apply_effect(int effect_id, const WorldPosition &position,
                                                      std::int64_t ticks) override {
    if (!combat_player_->has_simulation()) {
      return {};
    }
    auto &simulation = *combat_player_->current_simulation();
    auto packets = simulation.apply_item_effects({effect_id}, position, ticks);
    combat_settle_quest_events(simulation, position, ticks, packets);
    return packets;
  }

  std::vector<GameplayPacket> gathering_tool_wear_updates(const CharacterItem &tool) override {
    return inventory_session_->tool_wear_updates(tool);
  }

  void gathering_deliver_observers(const std::vector<GameplayPacket> &packets) override {
    deliver_observer_packets(*state_, connection_id(), packets);
  }

  const WorldProgress *processing_tutorial_progress() const override {
    return tutorial_player_->current_instance() ? &tutorial_player_->current_instance()->progress()
                                                : nullptr;
  }

  const nlohmann::json *processing_player_stats() const override {
    return combat_player_->has_simulation()
               ? &std::as_const(*combat_player_).current_simulation()->player_stats()
               : nullptr;
  }

  std::int64_t processing_monotonic_ticks() const override {
    return combat_monotonic_ticks();
  }

  bool processing_other_action_busy() const override {
    return other_action_busy();
  }

  // Crafting stations placed on a housing lot. Lot-local object positions compose with the
  // lot's and a building's transforms, so the reach covers the whole lot plus the ordinary
  // station allowance around its centre (emulator policy).
  std::optional<ProcessingPlacement> processing_placed_station(std::uint64_t unit_id,
                                                               int map_id) const override {
    const auto placed = housing_session_->placed_doodad(unit_id, map_id);
    if (!placed) {
      return std::nullopt;
    }

    ProcessingPlacement placement;
    placement.unit_id = unit_id;
    placement.doodad_cms_id = placed->doodad_cms_id;
    placement.x_mm = placed->lot_centre_x_mm;
    placement.y_mm = placed->lot_centre_y_mm;
    placement.z_mm = placed->lot_centre_z_mm;
    placement.range_mm = placed->lot_radius_mm + kProcessingStationRangeMm;
    return placement;
  }

  bool companion_has_handoff() const override {
    return handoff_.has_value();
  }

  bool companion_is_game_master() const override {
    return handoff_ && handoff_->game_master;
  }

  std::uint64_t companion_connection_id() const override {
    return connection_id();
  }

  std::int64_t companion_character_id() const override {
    return handoff_->character_id;
  }

  std::int64_t companion_epoch_ticks() const override {
    return epoch_ticks();
  }

  std::uint64_t companion_channel_key() const override {
    return instance_copy_ ? instance_copy_->channel_key() : 0;
  }

  companion::CompanionViewToken companion_view_token() const override {
    const auto view = current_gathering_view();
    return {view.epoch, view.map_id, view.copy_channel};
  }

  void companion_entry_failed() noexcept override {
    request_supervised_close();
  }

  bool companion_allow_gm_request() override {
    return allow_gm_request();
  }

  bool companion_allow_player_car() override {
    return allow_player_car();
  }

  void companion_reply(const std::string &message) override {
    gm_reply(message);
  }

  void companion_trade_commands(const std::vector<companion::CompanionGmCommand> &commands,
                                 companion::SessionResult &result) override {
    if (trade_) {
      companion::append_session_result(result, trade_->gm_command(commands, epoch_ticks()));
    }
  }

  void companion_enter_trade(const WorldPosition &position,
                              companion::SessionResult &result) override {
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
    if (companion_entry_fault_armed.exchange(false)) {
      throw std::runtime_error("Injected failure after companion attach");
    }
#endif
    if (trade_) {
      companion::append_session_result(result, trade_->enter_world(position, epoch_ticks()));
    }
  }

  void companion_trade_dismounted(companion::SessionResult &result) override {
    if (trade_ && !preserve_durable_location_on_close_.load() &&
        !map_staging_checkpoint_fence_.load()) {
      companion::append_session_result(result, trade_->wagon_dismounted(epoch_ticks()));
    }
  }

  void companion_trade_boarded(companion::SessionResult &result) override {
    if (trade_) {
      companion::append_session_result(result, trade_->wagon_boarded());
    }
  }

  companion::SessionResult dismount_dead_companion() {
    if (!companions_) {
      return {};
    }
    auto result = companions_->dismount_if_dead(epoch_ticks());
    if (!result.broadcast.packets.empty()) {
      companion_trade_dismounted(result);
    }
    return result;
  }

  void companion_capture_wagons(
      const std::vector<companion::VehicleSnapshot> &vehicles) override {
    if (!trade_ || !handoff_ || map_staging_checkpoint_fence_.load() ||
        (preserve_durable_location_on_close_.load() && !death_recovery_confirmed_)) {
      return;
    }
    if (close_wagon_captured_) {
      return;
    }
    const auto wagon = trade_->capture_wagon_settlement(vehicles, closing_position_);
    if (!wagon) {
      return;
    }
    {
      std::lock_guard lock(state_->character_admission_mutex);
      if (!state_->pending_world_settlements
               .emplace(handoff_->character_id, PendingWorldSettlement{wagon, std::nullopt})
               .second) {
        throw std::runtime_error("A previous world settlement still owns this character");
      }
    }
    close_wagon_captured_ = true;
  }

  bool companion_settle_wagons(const std::vector<companion::VehicleSnapshot> &vehicles) override {
    companion_capture_wagons(vehicles);
    if (!close_wagon_captured_) {
      return true;
    }
    return reconcile_pending_settlement(*handoff_);
  }

  // Runs `!mount` / `!dismount` on the worker strand. It happens outside a request frame, so
  // every packet, this connection's included, is posted directly. The tutorial instance has no
  // companion session and housing visits are excluded (Candidate emulator policy).
  void run_player_mount_command(PlayerMountAction action,
                                const std::function<void(std::string)> &completion) {
    try {
      if (closed() || preserve_durable_location_on_close_.load() || !player_services_entered_) {
        completion("Enter the game world before using mount commands.");
        return;
      }
      if (!companions_ || housing_session_->in_housing()) {
        completion("Mounts are not available here.");
        return;
      }

      companion::PlayerCompanionOutcome outcome;
      if (action == PlayerMountAction::mount) {
        outcome = state_->companion_feature.player_mount(*companions_, *this);
      } else {
        outcome = state_->companion_feature.player_dismount(*companions_, *this);
      }

      deliver_companion_update(*state_, outcome.result.broadcast);
      std::vector<Frame> own_frames;
      encode_companion_packets(outcome.result.own, own_frames);
      encode_companion_packets(outcome.result.trailing, own_frames);
      for (auto &frame : own_frames) {
        post_frame(std::move(frame));
      }
      completion(outcome.reply);
    } catch (const std::exception &error) {
      spdlog::error("Player mount command failed connection={}: {}", connection_id(), error.what());
      completion("The server could not change your mount. Try again.");
    }
  }

  std::optional<WorldPosition> unstuck_destination(const WorldPosition &position) const {
    const auto &spawns = state_->combat_feature.revival_spawns();
    if (!spawns) {
      return std::nullopt;
    }

    auto destination = spawns->nearest(position, false);
    if (!destination) {
      destination = spawns->nearest(position, true);
    }
    if (destination || !state_->travel_catalog) {
      return destination;
    }

    const auto *map = state_->travel_catalog->map(position.map_id);
    if (!map || map->parent_map_id <= 0) {
      return std::nullopt;
    }
    auto parent_position = position;
    parent_position.map_id = map->parent_map_id;
    destination = spawns->nearest(parent_position, false);
    if (!destination) {
      destination = spawns->nearest(parent_position, true);
    }
    return destination;
  }

  void run_player_unstuck_command(const std::function<void(std::string)> &completion) {
    try {
      if (closed() || preserve_durable_location_on_close_.load() || !handoff_ ||
          !player_services_entered_) {
        completion("Enter the game world before using !unstuck.");
        return;
      }
      if (housing_session_->in_housing()) {
        completion("Leave the housing area before using !unstuck.");
        return;
      }
      const auto blockers = escape_blockers();
      if (blockers.dead) {
        completion("Revive before using !unstuck.");
        return;
      }
      if (blockers.busy || blockers.riding ||
          (processing_ && processing_->busy()) || escape_feature_->teleport_pending() ||
          (travel_session_ && travel_session_->cut_scene_active()) ||
          gm_session_->has_spawned_units()) {
        completion("Finish your current action and dismount before using !unstuck.");
        return;
      }
      if (current_progress().trade_run.phase != TradeRunPhase::idle) {
        completion("Finish your trade run before using !unstuck.");
        return;
      }

      const auto now = Clock::now();
      if (last_unstuck_ && now - *last_unstuck_ < std::chrono::minutes(5)) {
        completion("Unstuck is cooling down. Try again in a few minutes.");
        return;
      }

      std::optional<WorldPosition> position;
      {
        std::lock_guard lock(state_->protocol_mutex);
        position = handshake_.current_position();
      }
      if (!position) {
        completion("A safe location is unavailable here.");
        return;
      }

      const auto destination = unstuck_destination(*position);
      if (!destination) {
        completion("No safe location is available for this map.");
        return;
      }

      // Reload the selected map even when it is unchanged. Unlike a long same-map
      // teleport, this clears the old visibility state and loads the destination area.
      if (!travel_map_move(*position, *destination, 0)) {
        completion("Unstuck could not start the map move. Try again.");
        return;
      }

      last_unstuck_ = now;
      completion("Moving you to a safe location.");
    } catch (const std::exception &error) {
      spdlog::error("Player unstuck command failed connection={}: {}", connection_id(),
                    error.what());
      completion("Unstuck could not move you. Try again.");
    }
  }

  void companion_deliver_update(const companion::CompanionUpdate &update) override {
    deliver_companion_update(*state_, update);
  }

  const AnnouncedUnit *trade_interaction_npc(std::uint64_t unit_id) const override {
    return npc_player_->interaction_npc(unit_id);
  }

  std::int64_t trade_epoch_ticks() const override {
    return epoch_ticks();
  }

  LootPersistence loot_tutorial_persistence(const Handoff &entry) override {
    return tutorial_loot_persistence(
        state_->auth, entry, *tutorial_player_->current_instance(), [this](const Character &live) {
          const auto *stats = combat_player_->has_simulation()
                                  ? &combat_player_->current_simulation()->player_stats()
                                  : nullptr;
          session_events_.publish(PlayerInventorySnapshotChanged{live, stats});
        });
  }

  std::shared_ptr<party::PartyWorld> loot_party_world() override {
    return state_->party_feature->world();
  }

  loot::PartyChannelKey loot_party_channel_key() override {
    return [this] {
      return party_session_->identity() ? party_session_->identity()->channel_key : 0;
    };
  }

  loot::PartyLootDelivery loot_party_delivery() override {
    return [state = state_, connection = connection_id()](const party::PartyUpdate &update) {
      deliver_party_update(*state, update, connection, nullptr);
    };
  }

  void loot_install_defeat_observer(MonsterDefeatObserver observer) override {
    combat_player_->current_simulation()->set_defeat_observer(std::move(observer));
  }

  std::int64_t loot_character_id() const override {
    return handoff_->character_id;
  }

  std::optional<Character> loot_stored_character() override {
    return state_->auth.world_character(*handoff_);
  }

  WorldProgress loot_stored_progress() override {
    return state_->auth.world_progress(*handoff_);
  }

  LootClaimResult loot_lookup_claim(const std::string &receipt_id,
                                     const std::string &corpse_fingerprint) override {
    return state_->auth.lookup_world_loot_claim(*handoff_, receipt_id, corpse_fingerprint);
  }

  LootClaimResult loot_commit_items(const LootCommit &commit) override {
    return state_->auth.commit_world_loot_claim(
        *handoff_, commit.receipt_id, commit.corpse_fingerprint, commit.before, commit.after,
        ItemTrade{commit.granted_items, {}, {}});
  }

  void loot_force_reconnect() override {
    asio::post(io_executor(), [self = shared_session()] {
      self->close_on_io();
    });
  }

  bool loot_has_combat() const override {
    return combat_player_->has_simulation();
  }

  bool loot_has_world_mobs() const override {
    return combat_player_->has_world_mobs();
  }

  const nlohmann::json &loot_current_combat_stats() const override {
    return combat_player_->current_simulation()->player_stats();
  }

  std::int64_t loot_monotonic_ticks() const override {
    return combat_monotonic_ticks();
  }

  void loot_publish_snapshot(const Character &character,
                             const nlohmann::json *prepared_stats) override {
    session_events_.publish(PlayerInventorySnapshotChanged{character, prepared_stats});
  }

  std::int64_t tutorial_monotonic_ticks() const override {
    return combat_monotonic_ticks();
  }

  bool tutorial_has_combat() const override {
    return combat_player_->has_simulation();
  }

  bool tutorial_combat_handles(const WorldGameplayRequest &request) const override {
    return combat_player_->handles(request);
  }

  bool tutorial_combat_busy() const override {
    return combat_player_->busy();
  }

  bool tutorial_player_in_combat() const override {
    return inventory_player_in_combat();
  }

  std::vector<GameplayPacket> tutorial_advance_combat(std::int64_t ticks,
                                                      const WorldPosition &position) override {
    return combat_player_->advance_tutorial(ticks, position);
  }

  std::vector<GameplayPacket> tutorial_receive_combat(const WorldGameplayRequest &request,
                                                      const WorldPosition &position,
                                                      std::int64_t ticks) override {
    return combat_player_->receive(request, position, ticks);
  }

  std::vector<GameplayPacket>
  tutorial_revive_at_shrine(const WorldGameplayRequest &request) override {
    return nuia_session_->revive_at_shrine(request);
  }

  bool tutorial_has_loot() const override {
    return loot_player_->has_session();
  }

  bool tutorial_loot_handles(std::uint16_t opcode) const override {
    return loot_player_->handles(opcode);
  }

  std::vector<GameplayPacket> tutorial_receive_loot(const nlohmann::json &request,
                                                    const WorldPosition &position,
                                                    std::int64_t ticks) override {
    return loot_player_->receive(request, position, ticks);
  }

  std::vector<GameplayPacket> tutorial_advance_loot(std::int64_t ticks) override {
    return loot_player_->advance(ticks);
  }

  void tutorial_apply_level_change(const WorldProgress &progress,
                                   std::vector<GameplayPacket> &packets) override {
    apply_level_change(progress, packets);
  }

  void tutorial_append_encumbrance_changes(std::vector<GameplayPacket> &packets,
                                           bool initial) override {
    inventory_session_->append_encumbrance_changes(packets, initial);
  }

  void tutorial_persist_player_death(std::vector<GameplayPacket> &packets) override {
    persist_player_death(packets);
  }

  void tutorial_collect_activity_rewards(std::vector<GameplayPacket> &) override {
    // Activity rewards are committed with the EXP grant and settled from the durable outbox.
  }

  bool tutorial_authenticated() const override {
    return handoff_.has_value();
  }

  void tutorial_end_combat() override {
    combat_player_->end_tutorial_combat();
  }

  void tutorial_reset_loot() override {
    loot_player_->reset();
  }

  std::optional<int> tutorial_population_map() const override {
    return state_->npc_feature.population()
               ? std::optional<int>(state_->npc_feature.population()->map_id())
               : std::nullopt;
  }

  bool tutorial_has_party_identity() const override {
    return party_session_->identity() != nullptr;
  }

  void tutorial_party_graduation(const WorldProgress &progress) override {
    party_session_->on_tutorial_graduation(progress);
  }

  bool tutorial_has_mail_repository() const override {
    return state_->mail_feature.has_repository();
  }

  bool tutorial_has_shop_catalog() const override {
    return state_->shop_feature.catalog() != nullptr;
  }

  std::optional<MailRecord> tutorial_deliver_completion_mail(std::int64_t ticks) override {
    return state_->mail_feature.deliver_tutorial_completion(
        handoff_->account_id, handoff_->world_id, handoff_->character_id, ticks);
  }

  std::optional<std::string> tutorial_graduation_notice() const override {
    return state_->character_feature.graduation_notice(handoff_->game_master);
  }

  void tutorial_gm_reply(const std::string &text) override {
    gm_reply(text);
  }

  void tutorial_set_population_map(std::optional<int> map_id) override {
    npc_player_->set_population_map(map_id);
  }

  std::optional<Character> tutorial_stored_character() override {
    return state_->auth.world_character(*handoff_);
  }

  void tutorial_create_gathering(const Character &character) override {
    create_gathering_session(character, *handoff_);
  }

  void tutorial_create_companions(const Character &character) override {
    create_companion_sessions(character, *handoff_);
  }

  void tutorial_create_quests(std::int64_t character_id) override {
    create_world_quests(character_id);
  }

  bool tutorial_save_world_position(const WorldPosition &position) override {
    return state_->auth.save_world_position(*handoff_, position);
  }

  std::uint64_t tutorial_connection_id() const override {
    return connection_id();
  }

  UnitRegistry &npc_registry() override {
    return unit_registry_;
  }

  const UnitRegistry &npc_registry() const override {
    return unit_registry_;
  }

  int npc_session_map_id() const override {
    return session_map_id();
  }

  std::string npc_instance_key() const override {
    return instance_key();
  }

  bool npc_has_tutorial() const override {
    return tutorial_player_->has_instance();
  }

  bool npc_authenticated() const override {
    return handoff_.has_value();
  }

  std::int64_t npc_monotonic_ticks() const override {
    return combat_monotonic_ticks();
  }

  bool npc_player_alive() const override {
    const auto *simulation = std::as_const(*combat_player_).current_simulation();
    return handoff_ && simulation && !simulation->player_dead();
  }

  std::string npc_reaction_scope() const override {
    if (tutorial_player_->has_instance()) {
      return std::string(npc::kPrivateReactionScopePrefix) + instance_key();
    }
    const auto channel = party_session_->identity() ? party_session_->identity()->channel_key : 0;
    return "shared:" + std::to_string(session_map_id()) + ":" + std::to_string(channel);
  }

  void npc_deliver_observers(const std::vector<GameplayPacket> &packets) override {
    if (!tutorial_player_->has_instance()) {
      deliver_observer_packets(*state_, connection_id(), packets);
    }
  }

  PreparedFrameInput prepare_frame(SessionFrameState &state) override {
    PreparedFrameInput prepared;
    auto &gameplay_request = prepared.request;
    auto &gameplay_position = prepared.gameplay_position;
    auto &party_position = prepared.party_position;
    auto &response = state.response;
    auto &position_to_checkpoint = state.position_to_checkpoint;
    auto &movement_to_broadcast = state.movement_to_broadcast;
    auto &pending_notifications = state.pending_notifications;
    auto &metadata = state.metadata;

    {
      std::lock_guard lock(state_->protocol_mutex);
      const bool log_packet_metadata =
          state_->credential_diagnostic_enabled || state_->bootstrap_diagnostic_enabled;
      WorldPacketMetadata *metadata_output = log_packet_metadata ? &metadata : nullptr;
      response =
          handshake_.receive(incoming_, combat_monotonic_ticks(), epoch_ticks(), metadata_output);
      gameplay_request = handshake_.take_gameplay_request();
      if (gameplay_request) {
        metadata.sequence = gameplay_request->sequence;
      }
      // Combat sessions are rebuilt on map changes; GM protections follow them.
      sync_gm_combat_protection();
      if (tutorial_player_->current_instance() || combat_player_->has_simulation() || gathering_ ||
          processing_ || quests_session_->has_book() || state_->nuia_feature.has_data()) {
        gameplay_position = handshake_.current_position();
      }
      party_position = handshake_.current_position();
      gathering_view_.publish(party_position ? std::optional<int>(party_position->map_id)
                                             : std::nullopt,
                              copy_channel_.load());
      if (incoming_.opcode() == kMoveRequest) {
        position_to_checkpoint = handshake_.last_position();
        movement_to_broadcast = handshake_.take_last_movement();
      }
      while (auto notification = handshake_.take_pending_notification()) {
        pending_notifications.push_back(std::move(*notification));
      }
    }

    return prepared;
  }

  void observe_movement_mode(SessionFrameContext &frame) override {
    expression_feature_->observe_movement(frame.state.movement_to_broadcast);
  }

  void observe_map_load_reset(SessionFrameContext &frame) override {
    const auto *gameplay_request = frame.request();

    if (gameplay_request && gameplay_request->opcode == kMapLoadCompleteRequest && handoff_) {
      expression_feature_->on_map_load_complete();
      nuia_session_->cancel_action();
    }
  }

  void observe_frame_metrics(SessionFrameContext &) override {
    const auto request_opcode = incoming_.opcode();
    if (world_metrics::enabled()) {
      world_metrics::count_frame_in(request_opcode, incoming_.bytes.size());
    }
  }

  void observe_patrol_position(SessionFrameContext &) override {
    npc_player_->observe_patrol_position(incoming_.opcode());
  }

  void observe_map_load_vitals(SessionFrameContext &frame) override {
    combat_player_->observe_map_load_vitals(frame, frame.state.response.has_value());
  }

  void observe_shared_map_mobs(SessionFrameContext &frame) override {
    combat_player_->observe_shared_map_mobs(frame, frame.state.response.has_value());
  }

  bool handle_shared_loot(SessionFrameContext &frame) override {
    return loot_player_->handle_shared(frame);
  }

  void observe_movement_vitals(SessionFrameContext &frame) override {
    combat_player_->observe_movement_vitals(frame, frame.state.movement_to_broadcast);
  }

  bool handle_shared_revival(SessionFrameContext &frame) override {
    return combat_player_->handle_shared_revival(frame);
  }

  bool handle_party(SessionFrameContext &frame) override {
    const auto *gameplay_request = frame.request();
    auto &party_update = frame.state.party_update;

    // Party membership and party chat span connections. PartyWorld owns the shared
    // state; its packets are delivered after this frame's own response.
    if (gameplay_request && party::PartyWorld::handles(gameplay_request->opcode)) {
      party_update = party_session_->receive(*gameplay_request);
      return true;
    }

    return false;
  }

  bool handle_guild(SessionFrameContext &frame) override {
    const auto *gameplay_request = frame.request();
    auto &guild_update = frame.state.guild_update;

    // Guild invitations span connections. GuildService publishes the packets for other
    // characters; this frame's own responses follow its response like party packets.
    if (gameplay_request && state_->guild_feature->world() &&
        GuildWorld::handles(gameplay_request->opcode)) {
      guild_update = guild_session_->receive(*gameplay_request);
      return true;
    }

    return false;
  }

  bool handle_housing_request(SessionFrameContext &frame) override {
    return housing_session_->handle_request(frame);
  }

  bool handle_housing_toggle(SessionFrameContext &frame) override {
    return housing_session_->handle_toggle(frame);
  }

  bool handle_housing_departure(SessionFrameContext &frame) override {
    return housing_session_->handle_departure(frame);
  }

  bool handle_zone_channel_move(SessionFrameContext &frame) override {
    const auto *gameplay_request = frame.request();
    auto &party_position = frame.party_position;

    if (gameplay_request && gameplay_request->opcode == kZoneChannelMoveRequest) {
      if (receive_zone_channel_move(*gameplay_request, party_position)) {
        frame.invalidate_positions();
      }
      return true;
    }

    return false;
  }

  // Shared-world level triggers (instance, dungeon and portal volumes, scripted cutscenes),
  // UI map travel (ChannelTriggerEntryReq) and the end of travel cutscenes. The private
  // tutorial instance and housing areas answer these requests themselves.
  bool handle_map_travel(SessionFrameContext &frame) override {
    const auto *request = frame.request();
    if (!request || !handoff_ || !state_->travel_catalog ||
        tutorial_player_->current_instance() || housing_session_->in_housing()) {
      return false;
    }
    if (!travel_session_) {
      travel_session_ = std::make_unique<travel::TravelSession>(
          state_->travel_catalog, player_wire_unit_id(handoff_->character_id));
    }

    auto &packets = frame.session_packets;
    if (request->opcode == travel::kCutSceneEndRequest) {
      auto ended = travel_session_->end(*request);
      if (!ended) {
        return false;
      }
      frame.state.completed_guide_cut_scene = request->message.value("cut_scene_id", 0);
      packets.insert(packets.end(), ended->begin(), ended->end());
      const auto destination = travel_session_->take_cut_scene_destination();
      if (destination) {
        if (frame.party_position && travel_map_move(*frame.party_position, *destination, 0)) {
          frame.invalidate_positions();
        } else {
          packets.push_back({travel::kVisibleChannelNotification,
                             {{"unit_id", std::to_string(player_wire_unit_id(handoff_->character_id))},
                              {"visible_channel", "3"}}});
          spdlog::warn("Cutscene end point could not complete its map move connection={}",
                       connection_id());
        }
      }
      return true;
    }

    const auto position = frame.party_position;
    if (!position) {
      return false;
    }
    const auto context = travel_context(*position);
    if (request->opcode == world_request::kInteractionUnitRequest) {
      const auto plan = state_->travel_catalog->cliffs_gate(*position, request->message, context);
      if (!plan.handled) {
        return false;
      }

      nlohmann::json response{{"packet_seq", {{"seq", std::to_string(request->sequence)}}}};
      if (!plan.refusal.empty() || travel_session_->cut_scene_active() || other_action_busy() ||
          gm_session_->has_spawned_units() || events_player_dead()) {
        response["ec"] = "Interaction_ConditionsNotMatch";
      } else {
        const auto started = travel_session_->play(plan, *position, combat_monotonic_ticks());
        packets.insert(packets.end(), started.begin(), started.end());
        response["target_unit_id"] = request->message.at("target_unit_id");
        response["interaction_id"] = request->message.at("param").at("interaction_id");
      }
      packets.push_back({kInteractUnitResponse, std::move(response)});
      return true;
    }
    const auto plan = request->opcode == travel::kLevelTriggerRequest
                          ? state_->travel_catalog->level_trigger(*position, request->message,
                                                                  context)
                          : state_->travel_catalog->channel_entry(*position, request->message,
                                                                  context);
    if (!plan.handled) {
      return false;
    }

    const auto cut_scene_packets = travel_session_->play(plan, *position, combat_monotonic_ticks());
    packets.insert(packets.end(), cut_scene_packets.begin(), cut_scene_packets.end());
    apply_volume_doodads(plan, *position, frame);
    if (plan.destination) {
      if (travel_map_move(*position, *plan.destination, plan.cut_scene_id)) {
        frame.invalidate_positions();
      } else if (travel_blocked()) {
        defer_map_travel(*position, *plan.destination, plan.cut_scene_id);
      }
      return true;
    }
    if (!plan.refusal.empty()) {
      spdlog::warn("Map travel refused connection={} map={} opcode={}: {}", connection_id(),
                   position->map_id, request->opcode, plan.refusal);
    }
    if (plan.client_waits_for_move) {
      if (travel_map_move(*position, *position, 0)) {
        frame.invalidate_positions();
      } else if (travel_blocked()) {
        // The release handoff is owed as well; without it movement stays locked there.
        defer_map_travel(*position, *position, 0);
      }
    }
    return true;
  }

  void observe_server_map_completion(SessionFrameContext &frame) override {
    const auto *gameplay_request = frame.request();
    auto &session_packets = frame.session_packets;
    auto &response = frame.state.response;

    // A server-owned map move finishes with this MapLoadCompleteReq; the handshake has
    // answered MapMoveCompleteNoti. Repopulate the new map before any map-bound handler.
    if (gameplay_request && gameplay_request->opcode == kMapLoadCompleteRequest && response &&
        (housing_session_->has_pending_entry() || pending_exit_)) {
      session_packets = complete_server_map_move();
    }
  }

  void observe_raid_arrival(SessionFrameContext &frame) override {
    const auto *gameplay_request = frame.request();
    auto &response = frame.state.response;

    // A member whose raid channel closed while it was still loading leaves on arrival.
    if (gameplay_request && gameplay_request->opcode == kMapLoadCompleteRequest && response) {
      events_session_->on_map_load_complete();
    }
  }

  bool handle_market(SessionFrameContext &frame) override {
    return market_session_->handle(frame);
  }

  bool handle_events(SessionFrameContext &frame) override {
    return events_session_->handle(frame, frame.state.raid_update);
  }

  bool handle_world_chat(SessionFrameContext &frame) override {
    const auto *gameplay_request = frame.request();
    auto &session_packets = frame.session_packets;
    auto &action_deadline = frame.action_deadline;
    auto &party_update = frame.state.party_update;

    if (gameplay_request && gameplay_request->opcode == kWorldChatRequest) {
      session_packets = chat_session_->receive(*gameplay_request, party_update);
      action_deadline = next_gameplay_tick();
      return true;
    }

    return false;
  }

  bool handle_companion_trade(SessionFrameContext &frame) override {
    const auto *gameplay_request = frame.request();
    auto &party_position = frame.party_position;
    auto &companion_result = frame.state.companion_result;

    if (gameplay_request && companions_ && handoff_ &&
        companions_->handles_registration_animation(gameplay_request->opcode,
                                                    gameplay_request->message)) {
      auto result = companions_->receive_registration_animation(
          *gameplay_request, party_position, player_wire_unit_id(handoff_->character_id),
          combat_monotonic_ticks(), registration_blocked());
      deliver_interaction_observers(result.own);
      companion::append_session_result(companion_result, std::move(result));
      frame.action_deadline = next_gameplay_tick();
      return true;
    }

    // Mounts, vehicles, house mates and trade runs (non-tutorial sessions). Their replies
    // follow the frame's response; shared unit packets and trailing replies are delivered
    // after the frame (below), so a wagon's UnitAddNoti precedes TradeStartNoti.
    if (gameplay_request && companions_ &&
        route_companion_request(*gameplay_request, party_position, companion_result)) {
      return true;
    }

    return false;
  }

  void observe_companion_own_packets(SessionFrameContext &frame) override {
    auto &session_packets = frame.session_packets;
    auto &companion_result = frame.state.companion_result;

    if (companions_ && companions_->registration_busy() && frame.party_position) {
      auto registration = companions_->advance_registration(
          combat_monotonic_ticks(), *frame.party_position, registration_blocked());
      deliver_interaction_observers(registration.own);
      companion::append_session_result(companion_result, std::move(registration));
    }

    session_packets.insert(session_packets.end(),
                           std::make_move_iterator(companion_result.own.begin()),
                           std::make_move_iterator(companion_result.own.end()));
    companion_result.own.clear();

    // Trade-good sales advance TradePayment objectives after the sale's own reply.
    if (trade_) {
      record_quest_events(trade_->take_quest_events(), frame);
    }
  }

  bool handle_statue(SessionFrameContext &frame) override {
    return nuia_session_->handle_statue(frame);
  }

  bool handle_statue_busy_skill(SessionFrameContext &frame) override {
    return nuia_session_->handle_statue_busy_skill(frame);
  }

  bool handle_item_use(SessionFrameContext &frame) override {
    return state_->item_use_feature.handle(item_use_.get(), frame, *this);
  }

  bool handle_company(SessionFrameContext &frame) override {
    const auto *gameplay_request = frame.request();
    auto &session_packets = frame.session_packets;

    // Company dialogue functions (join, change, pioneer permits). Other dialogue
    // functions keep their existing handling.
    if (gameplay_request && gameplay_request->opcode == company_world::kDialogueFunctionRequest &&
        state_->company_feature->has_dialogues()) {
      session_packets = company_session_->dialogue(*gameplay_request);
      return true;
    }

    return false;
  }

  bool handle_social_gm(SessionFrameContext &frame) override {
    return gm_session_->handle_social_gm(frame);
  }

  bool handle_generic_gm(SessionFrameContext &frame) override {
    return gm_session_->handle_generic_gm(frame);
  }

  bool handle_equip(SessionFrameContext &frame) override {
    const bool handled = inventory_session_->handle_equip(frame);
    if (handled) {
      std::vector<GameplayPacket> public_updates;
      for (const auto &packet : frame.session_packets) {
        if (packet.opcode == 168 ||
            (packet.opcode == 172 && packet.message.value("ec", std::string{}) == "Success")) {
          public_updates.push_back(packet);
        }
      }
      deliver_observer_packets(*state_, connection_id(), public_updates);
    }
    return handled;
  }

  bool handle_discard(SessionFrameContext &frame) override {
    return inventory_session_->handle_discard(frame);
  }

  bool handle_polish(SessionFrameContext &frame) override {
    return inventory_session_->handle_polish(frame);
  }

  bool handle_progression(SessionFrameContext &frame) override {
    return character_session_->handle(frame);
  }

  bool handle_escape(SessionFrameContext &frame) override {
    return escape_feature_->handle(frame);
  }

  bool handle_expression(SessionFrameContext &frame) override {
    return expression_feature_->handle(frame);
  }

  bool handle_npc_threat(SessionFrameContext &frame) override {
    return npc_player_->handle_threat(frame);
  }

  bool handle_merchant(SessionFrameContext &frame) override {
    return shop_session_->handle(frame);
  }

  bool handle_processing(SessionFrameContext &frame) override {
    const bool handled = state_->processing_feature.handle(processing_.get(), frame, *this);
    if (handled && processing_) {
      record_quest_events(processing_->take_quest_events(), frame);
    }
    return handled;
  }

  bool handle_gathering(SessionFrameContext &frame) override {
    const auto view = gathering_view_.snapshot();
    const auto first_new_packet = frame.session_packets.size();
    const auto channel = copy_channel_.load();
    const bool handled = state_->gathering_feature->handle(gathering_.get(), frame, *this,
                                                           frame.state.gathering_broadcasts);
    for (std::size_t index = first_new_packet; index < frame.session_packets.size(); ++index) {
      auto &packet = frame.session_packets[index];
      if (packet.local_plan) {
        packet.local_view_epoch = view.epoch;
      }
    }
    if (channel != copy_channel_.load() && !frame.state.gathering_broadcasts.empty()) {
      // A portal doodad moved the character to another copy or map: its changes belong to the
      // copy it left, so they are delivered there now rather than with the new copy's channel.
      deliver_gathering_changes(*state_, frame.state.gathering_broadcasts, channel,
                                connection_id());
      frame.state.gathering_broadcasts.clear();
      state_->gathering_feature->schedule_respawn();
    }
    if (handled && gathering_) {
      record_quest_events(gathering_->take_quest_events(), frame);
    }
    return handled;
  }

  // Quest objectives follow server-committed crafts and doodad interactions. Their packets follow
  // the producer's own reply in the same frame.
  void record_quest_events(QuestEvents events, SessionFrameContext &frame) {
    if (events.empty() || !frame.gameplay_position) {
      return;
    }
    quests_session_->record_events(events, *frame.gameplay_position, frame.session_packets);
  }

  bool handle_tears(SessionFrameContext &frame) override {
    return nuia_session_->handle_tears(frame);
  }

  void observe_adventure_lifecycle(SessionFrameContext &frame) override {
    if (!adventure_) {
      return;
    }
    auto packets =
        adventure_->advance(combat_monotonic_ticks(), frame.party_position, adventure_blocked());
    deliver_interaction_observers(packets);
    frame.session_packets.insert(frame.session_packets.end(),
                                 std::make_move_iterator(packets.begin()),
                                 std::make_move_iterator(packets.end()));
  }

  bool handle_adventure(SessionFrameContext &frame) override {
    const auto *request = frame.request();
    if (!request || !handoff_ || !frame.party_position || !state_->adventure_catalog ||
        tutorial_player_->current_instance()) {
      return false;
    }
    if (!adventure_) {
      auto items = tutorial::tutorial_reward_persistence(
          state_->auth, *handoff_, [this](const Character &live) {
            const auto *stats = combat_player_->has_simulation()
                                    ? &combat_player_->current_simulation()->player_stats()
                                    : nullptr;
            session_events_.publish(PlayerInventorySnapshotChanged{live, stats});
          });
      adventure_ = std::make_unique<adventure::Session>(
          state_->adventure_catalog,
          static_cast<std::int64_t>(player_wire_unit_id(handoff_->character_id)),
          static_cast<ProgressAccess &>(*this),
          [this](const WorldProgress &before, const WorldProgress &after) {
            return state_->auth.save_world_progress(*handoff_, before, after);
          },
          std::move(items), state_->character_feature.progression());
      adventure_->set_requirement_check(
          [this](const nlohmann::json &conditions, const WorldPosition &position) {
            return quests_session_->requirements_met(conditions, position);
          });
    }
    if (!adventure_->handles(*request, *frame.party_position)) {
      return false;
    }
    const auto revision_before = load().revision;
    std::vector<GameplayPacket> packets;
    try {
      packets = adventure_->receive(*request, *frame.party_position, combat_monotonic_ticks(),
                                    adventure_blocked());
    } catch (const std::exception &error) {
      // A failure on one world object (for example a bad reward definition or a commit
      // conflict) refuses that request. Dropping the connection instead left the stock client
      // retrying TLS reconnects and unable to return to the world.
      spdlog::error("Adventure request failed connection={} opcode={}: {}", connection_id(),
                    request->opcode, error.what());
      packets = adventure_->abort(*request);
      frame.session_packets.insert(frame.session_packets.end(),
                                   std::make_move_iterator(packets.begin()),
                                   std::make_move_iterator(packets.end()));
      return true;
    }
    deliver_interaction_observers(packets);
    apply_level_change(load(), packets);
    // An activation committed state that AdventureObjectStatus and GetItem objectives read
    // (First Hunt's knife), so credit them now rather than at the next time sync.
    if (load().revision != revision_before) {
      quests_session_->refresh_objectives(*frame.party_position, packets);
    }
    frame.session_packets.insert(frame.session_packets.end(),
                                 std::make_move_iterator(packets.begin()),
                                 std::make_move_iterator(packets.end()));
    frame.action_deadline = next_gameplay_tick();
    return true;
  }

  void observe_tear_snapshot(SessionFrameContext &frame) override {
    nuia_session_->observe_tear_snapshot(frame);
  }

  void observe_quest_snapshot(SessionFrameContext &frame) override {
    quests_session_->observe_snapshot(frame);
  }

  void observe_utility_bag_snapshot(SessionFrameContext &frame) override {
    inventory_session_->observe_utility_bag_snapshot(frame);
  }

  bool handle_utility_bag(SessionFrameContext &frame) override {
    return inventory_session_->handle_utility_bag(frame);
  }

  bool handle_world_quests(SessionFrameContext &frame) override {
    return quests_session_->handle(frame);
  }

  void observe_shared_population(SessionFrameContext &frame) override {
    if (npc_player_->observe_shared_population(frame) && frame.party_position) {
      sector_sight_.arm(frame.party_position->map_id);
    }
  }

  void observe_sector_sight(SessionFrameContext &frame) override {
    auto &party_position = frame.party_position;
    auto &session_packets = frame.session_packets;

    if (!tutorial_player_->current_instance() && party_position &&
        (!state_->sector_grid || !state_->sector_grid->has_map(party_position->map_id)) &&
        incoming_.opcode() == kMapLoadCompleteRequest) {
      GameplayPacket marker;
      marker.local_full_map_snapshot = true;
      session_packets.push_back(std::move(marker));
    }
    if (!tutorial_player_->current_instance() && party_position) {
      auto sector_packets = refresh_sector_sight(*party_position);
      session_packets.insert(session_packets.end(), std::make_move_iterator(sector_packets.begin()),
                             std::make_move_iterator(sector_packets.end()));
    }
  }

  void observe_npc_sight(SessionFrameContext &frame) override {
    npc_player_->observe_npc_sight(frame);
  }

  bool handle_private_combat(SessionFrameContext &frame) override {
    return combat_player_->handle_private_combat(frame);
  }

  void observe_unhandled_interaction(SessionFrameContext &frame) override {
    const auto *gameplay_request = frame.request();
    auto &gameplay_position = frame.gameplay_position;

    // An interaction start that no shared-world service consumed leaves the client waiting
    // in its pending animation. Name what was asked so a failed live trial is diagnosable.
    if (!tutorial_player_->current_instance() && gameplay_request &&
        gameplay_request->opcode == kInteractionStartRequest) {
      log_unhandled_interaction_start(*gameplay_request, gameplay_position);
    }
  }

  bool handle_tutorial_tail(SessionFrameContext &frame) override {
    auto &action_deadline = frame.action_deadline;
    auto &pending_notifications = frame.state.pending_notifications;

    // Durable owner work finishes before the original combined deadline sample and codec
    // lock. Only the application's frame and handshake outputs are encoded below.
    if (auto result = tutorial_player_->handle_frame(frame.request(), frame.gameplay_position)) {
      auto &packets = result->packets;
      auto &finished_teleport = result->finished_teleport;
      auto &map_change = result->map_change;
      frame.state.completed_guide_cut_scene = result->completed_cut_scene;
      action_deadline = next_gameplay_tick();
      if (contents_guide_session_ || state_->contents_guides) {
        for (const auto &packet : packets) {
          if (affects_contents_guides(packet)) {
            frame.state.guide_action_packets.push_back(packet);
          }
        }
      }
      std::lock_guard lock(state_->protocol_mutex);
      for (auto &packet : packets) {
        if (combat_player_->has_simulation() && !combat_player_->decorate_spawn(packet)) {
          continue;
        }
        npc_player_->track_announced_unit(packet);
        pending_notifications.push_back(state_->protocol.encode(packet.opcode, packet.message));
      }
      if (finished_teleport) {
        reset_movement_vitals();
        handshake_.set_authoritative_position(*finished_teleport);
      }
      if (map_change) {
        // Captured order: MapMoveStartNoti, then removal of the previous map's units.
        gathering_view_.invalidate();
        pending_notifications.push_back(handshake_.begin_map_move(map_change->spawn));
        const GameplayPacket removal{kUnitRemoveNotification,
                                     {{"unit_ids", map_change->removed_unit_ids}}};
        npc_player_->track_announced_unit(removal);
        pending_notifications.push_back(state_->protocol.encode(removal.opcode, removal.message));
        if (map_change->completion_mail) {
          // The welcome mail was committed by the Tutorial owner; this only announces it.
          const auto notification = mail::mail_delivery_notification(*map_change->completion_mail);
          pending_notifications.push_back(
              state_->protocol.encode(notification.opcode, notification.message));
        }
      }
    }

    return false;
  }

  void observe_start_playing_patrol(SessionFrameContext &frame) override {
    const bool entering_world =
        incoming_.opcode() == kStartPlayingRequest && frame.state.response.has_value();
    npc_player_->observe_start_playing_patrol(entering_world, frame.session_packets);
  }

  void observe_encumbrance(SessionFrameContext &frame) override {
    auto &session_packets = frame.session_packets;
    auto &response = frame.state.response;

    const bool entering_world = incoming_.opcode() == kStartPlayingRequest && response.has_value();
    inventory_session_->append_encumbrance_changes(session_packets, entering_world);
  }

  void observe_session_encoding(SessionFrameContext &frame) override {
    auto &session_packets = frame.session_packets;
    auto &pending_notifications = frame.state.pending_notifications;
    auto &metadata = frame.state.metadata;

    append_contents_guides(frame);
    {
      std::lock_guard lock(state_->protocol_mutex);
      for (auto &packet : session_packets) {
        if (packet.local_full_map_snapshot) {
          pending_notifications.push_back(make_full_map_snapshot_marker(gathering_view_.snapshot()));
          continue;
        }
        const auto error_code = packet.message.find("ec");
        if (error_code != packet.message.end() && *error_code == "BadRequest") {
          // Keep the refusal attributable without logging a private packet body.
          spdlog::warn("World request refused connection={} request_opcode={} "
                       "sequence={} response_opcode={} ec=BadRequest",
                       connection_id(), incoming_.opcode(), metadata.sequence, packet.opcode);
        }
        const auto retained_bytes = packet.local_plan ? local_packet_retained_bytes(packet) : 0;
        auto encoded = state_->protocol.encode(packet.opcode, packet.message);
        if (packet.local_plan) {
          encoded.local_retained_bytes = retained_bytes;
          encoded.local_allocation = std::make_shared<const LocalFrameAllocation>(
              LocalFrameAllocation{encoded.local_retained_bytes, packet.local_construction});
          encoded.local_plan = packet.local_plan;
          encoded.local_message = std::make_shared<const nlohmann::json>(packet.message);
          auto view = gathering_view_.snapshot();
          if (packet.local_view_epoch) {
            view = {*packet.local_view_epoch, packet.local_plan->expected_map(),
                    packet.local_plan->expected_channel()};
          }
          bind_gathering_frame(encoded, view);
        }
        pending_notifications.push_back(std::move(encoded));
      }
    }
  }

  void append_contents_guides(SessionFrameContext &frame) {
    if (!state_->contents_guides || !handoff_ || !frame.party_position) {
      return;
    }
    if (incoming_.opcode() == kMapLoadCompleteRequest) {
      guide_world_ready_ = false;
      return;
    }
    const bool entering = incoming_.opcode() == kStartPlayingRequest && frame.state.response;
    if (entering) {
      guide_world_ready_ = true;
    }
    if (!guide_world_ready_) {
      return;
    }
    if (!contents_guide_session_ && !entering) {
      return;
    }
    try {
      if (!contents_guide_session_) {
        const auto quests = state_->quests_feature.catalog();
        contents_guide_session_ = std::make_unique<guides::Session>(
            state_->contents_guides,
            [quests](int id) -> const nlohmann::json * {
              return quests ? quests->quest(id) : nullptr;
            },
            state_->guide_items);
      }
      auto &actions = frame.state.guide_action_packets;
      for (const auto &packet : frame.session_packets) {
        if (affects_contents_guides(packet)) {
          actions.push_back(packet);
        }
      }
      const bool changed = std::ranges::any_of(actions, [](const GameplayPacket &packet) {
        return packet.opcode != kGuideInteractUnitResponse;
      });
      std::optional<WorldProgress> progress;
      std::optional<Character> character;
      if (entering || changed || pending_guide_progress_) {
        if (tutorial_player_->current_instance()) {
          progress = tutorial_player_->current_instance()->progress();
        } else if (pending_guide_progress_) {
          progress = std::move(pending_guide_progress_);
        } else {
          progress = state_->auth.world_progress(*handoff_);
        }
        pending_guide_progress_.reset();
        character = state_->auth.world_character(*handoff_);
        if (!character) {
          spdlog::warn("Contents guide character no longer available connection={}", connection_id());
          return;
        }
      }
      auto guidance = contents_guide_session_->observe(*frame.party_position, std::move(progress),
                                                        std::move(character), actions,
                                                        frame.state.completed_guide_cut_scene);
      frame.session_packets.insert(frame.session_packets.end(),
                                   std::make_move_iterator(guidance.begin()),
                                   std::make_move_iterator(guidance.end()));
    } catch (const std::exception &error) {
      spdlog::error("Contents guide update failed connection={} error={}", connection_id(),
                    error.what());
    }
  }

  void observe_riding_vitals_reset(SessionFrameContext &) override {
    combat_player_->observe_riding_vitals_reset();
  }

  void observe_final_deadline(SessionFrameContext &frame) override {
    auto &action_deadline = frame.action_deadline;

    // Combat events settle at each producer boundary, before a kill can start another quest.
    action_deadline = next_gameplay_tick();
  }

  void post_movement_presence(SessionFrameContext &frame) override {
    auto &position_to_checkpoint = frame.state.position_to_checkpoint;
    auto &movement_to_broadcast = frame.state.movement_to_broadcast;
    auto &companion_result = frame.state.companion_result;

    if (position_to_checkpoint && movement_to_broadcast) {
      // Housing visitors share presence per channel and instance players per copy, not per map.
      auto presence_position = *position_to_checkpoint;
      if (const int presence_map = presence_map_override(); presence_map != 0) {
        presence_position.map_id = presence_map;
      }
      auto events =
          state_->presence.move(connection_id(), presence_position, *movement_to_broadcast);
      housing_session_->observe_movement(*position_to_checkpoint);
      // A rider's movement is broadcast as the ridden unit's MoveNoti instead of its own
      // (Candidate); presence still updates visibility.
      if (companions_ && companions_->riding()) {
        std::erase_if(events, [](const WorldPresenceEvent &event) {
          return event.kind == WorldPresenceEventKind::move;
        });
      }
      deliver_presence_events(std::move(events));
      if (companions_) {
        auto moved = companions_->move(*position_to_checkpoint, &*movement_to_broadcast);
        companion_result.broadcast.append(std::move(moved.broadcast));
      }
    }
  }

  void post_companion_entry(SessionFrameContext &frame) override {
    auto &party_position = frame.party_position;
    auto &response = frame.state.response;
    auto &companion_result = frame.state.companion_result;

    if (companions_ && incoming_.opcode() == kStartPlayingRequest && response.has_value() &&
        party_position) {
      if (pending_region_companion_handoff_ && companions_->entered()) {
        try {
          companion::append_session_result(
              companion_result,
              companions_->reenter_region(*party_position, epoch_ticks(), companion_view_token()));
          companion_enter_trade(*party_position, companion_result);
        } catch (const std::exception &error) {
          spdlog::error("Companion region reentry failed connection={}: {}", connection_id(),
                        error.what());
          request_supervised_close();
          return;
        } catch (...) {
          spdlog::error("Companion region reentry failed connection={}", connection_id());
          request_supervised_close();
          return;
        }
      } else {
        enter_companion_world(*party_position, companion_result);
      }
      pending_region_companion_handoff_ = false;
    }
  }

  void post_companion_delivery(SessionFrameContext &frame) override {
    auto &pending_notifications = frame.state.pending_notifications;
    auto &companion_result = frame.state.companion_result;

    companion::append_session_result(companion_result, dismount_dead_companion());

    deliver_companion_update(*state_, companion_result.broadcast, connection_id(),
                             &pending_notifications);
    encode_companion_packets(companion_result.own, pending_notifications);
    encode_companion_packets(companion_result.trailing, pending_notifications);
  }

  void post_gathering_changes(SessionFrameContext &frame) override {
    auto &gathering_broadcasts = frame.state.gathering_broadcasts;

    if (!gathering_broadcasts.empty()) {
      deliver_gathering_changes(*state_, gathering_broadcasts, copy_channel_.load(),
                                connection_id());
      state_->gathering_feature->schedule_respawn();
    }
  }

  void post_player_services(SessionFrameContext &frame) override {
    auto &party_position = frame.party_position;
    auto &response = frame.state.response;
    auto &pending_notifications = frame.state.pending_notifications;

    const bool entered_world = incoming_.opcode() == kStartPlayingRequest && response.has_value();
    if (handoff_ && party_session_->identity() && party_position && !closed()) {
      auto flag_position = *party_position;
      const auto update = combat_player_->update_pvp_presence(
          entered_world, player_services_entered_, flag_position, presence_map_override(),
          static_cast<bool>(tutorial_player_->current_instance()));
      if (update.registration) {
        player_services_entered_ = *update.registration;
        chat_session_->on_enter_world(player_services_entered_);
      } else if (update.notification) {
        const auto &packet = *update.notification;
        {
          std::lock_guard lock(state_->protocol_mutex);
          pending_notifications.push_back(state_->protocol.encode(packet.opcode, packet.message));
        }
        deliver_observer_packets(*state_, connection_id(), {packet});
      }
    }
  }

  void post_party_update(SessionFrameContext &frame) override {
    auto &party_position = frame.party_position;
    auto &response = frame.state.response;
    auto &pending_notifications = frame.state.pending_notifications;
    auto &party_update = frame.state.party_update;

    const bool entered_world = incoming_.opcode() == kStartPlayingRequest && response.has_value();

    try {
      party_update.append(party_session_->on_enter_or_refresh(party_position, entered_world));
    } catch (const std::exception &error) {
      spdlog::error("Party presence update failed connection={}: {}", connection_id(),
                    error.what());
    }
    if (!party_update.packets.empty() || !party_update.unit_changes.empty()) {
      deliver_party_update(*state_, party_update, connection_id(), &pending_notifications);
      state_->party_feature->schedule_tick();
    }
  }

  void post_raid_update(SessionFrameContext &frame) override {
    auto &pending_notifications = frame.state.pending_notifications;
    auto &raid_update = frame.state.raid_update;

    if (!raid_update.packets.empty() || !raid_update.evictions.empty()) {
      deliver_raid_update(*state_, raid_update, connection_id(), &pending_notifications);
      state_->events_feature->schedule_raid_tick();
    }
  }

  void post_emotion_snapshot(SessionFrameContext &frame) override {
    const bool entered_world =
        incoming_.opcode() == kStartPlayingRequest && frame.state.response.has_value();
    expression_feature_->on_enter_world(entered_world, frame.state.pending_notifications);
  }

  void post_guild_update(SessionFrameContext &frame) override {
    auto &response = frame.state.response;
    auto &pending_notifications = frame.state.pending_notifications;
    auto &guild_update = frame.state.guild_update;

    const bool entered_world = incoming_.opcode() == kStartPlayingRequest && response.has_value();

    try {
      guild_update.append(guild_session_->on_enter_world(entered_world));
    } catch (const std::exception &error) {
      spdlog::error("Guild presence update failed connection={}: {}", connection_id(),
                    error.what());
    }
    if (!guild_update.empty()) {
      deliver_guild_update(*state_, guild_update, connection_id(), &pending_notifications);
    }
  }

  void post_housing_snapshot(SessionFrameContext &frame) override {
    auto &party_position = frame.party_position;
    auto &response = frame.state.response;
    auto &pending_notifications = frame.state.pending_notifications;

    const bool entered_world = incoming_.opcode() == kStartPlayingRequest && response.has_value();

    try {
      auto housing_packets = housing_session_->publish_presence(entered_world, party_position);
      if (!housing_packets.empty()) {
        std::lock_guard lock(state_->protocol_mutex);
        for (const auto &packet : housing_packets) {
          pending_notifications.push_back(state_->protocol.encode(packet.opcode, packet.message));
        }
      }
    } catch (const std::exception &error) {
      spdlog::error("Housing presence update failed connection={}: {}", connection_id(),
                    error.what());
    }
  }

  void post_progress_checkpoint(SessionFrameContext &frame) override {
    auto &position_to_checkpoint = frame.state.position_to_checkpoint;

    if (position_to_checkpoint && handoff_ && !preserve_durable_location_on_close_.load() &&
        !map_staging_checkpoint_fence_.load() &&
        Clock::now() - last_progress_checkpoint_ >= kWorldProgressCheckpointInterval) {
      try {
        if (state_->auth.save_world_position(
                *handoff_, events_session_->persisted_position(*position_to_checkpoint))) {
          last_progress_checkpoint_ = Clock::now();
        }
      } catch (const std::exception &error) {
        spdlog::error("World progress checkpoint failed: {}", error.what());
      }
    }
  }

  void report_frame_rejection(std::uint16_t incoming_opcode, std::string_view step,
                              const std::exception &error) override {
    // Payloads can contain account or character secrets; retain metadata-only attribution.
    spdlog::warn("Native world opcode {} rejected at {}: {}", incoming_opcode, step, error.what());
  }

  void log_frame_result(std::uint16_t incoming_opcode, const SessionFrameState &state,
                        bool rejected) override {
    const auto &response = state.response;
    const auto &pending_notifications = state.pending_notifications;
    const auto &metadata = state.metadata;
    if (state_->credential_diagnostic_enabled || state_->bootstrap_diagnostic_enabled) {
      auto action = "ignored";
      if (rejected) {
        action = "rejected";
      } else if (response || !pending_notifications.empty()) {
        action = "responded";
      }

      spdlog::info("Native world client packet connection={} opcode={} bytes={} "
                   "sequence={} protobuf_bytes={} family={} status={} action={}",
                   connection_id(), incoming_opcode, incoming_.bytes.size(), metadata.sequence,
                   metadata.protobuf_bytes, metadata.family.empty() ? "none" : metadata.family,
                   metadata.status.empty() ? "none" : metadata.status, action);
    }
  }

  struct CombatScheduleSnapshot {
    std::shared_ptr<combat::CombatTimeline> timeline;
    std::optional<std::int64_t> deadline;
  };

  class ScopedCombatTick final {
  public:
    ScopedCombatTick(std::optional<std::int64_t> &target, std::optional<std::int64_t> tick)
        : target_(target), previous_(std::exchange(target, tick)) {}

    ~ScopedCombatTick() {
      target_ = previous_;
    }

  private:
    std::optional<std::int64_t> &target_;
    std::optional<std::int64_t> previous_;
  };

  CombatScheduleSnapshot combat_schedule_snapshot(std::optional<std::int64_t> deadline) const {
    const auto *simulation = combat_player_->current_simulation();
    return {simulation ? simulation->world_combat_timeline() : nullptr, deadline};
  }

  static WorldState::CombatWakeKey
  combat_wake_key(const std::shared_ptr<combat::CombatTimeline> &timeline,
                  combat::CombatTimeline::Owner owner) {
    return {reinterpret_cast<std::uintptr_t>(timeline.get()), owner.id, owner.generation};
  }

  void wake_ordered_combat_head(const std::shared_ptr<combat::CombatTimeline> &timeline) {
    const auto owner = timeline->head_owner();
    if (!owner) {
      return;
    }
    std::shared_ptr<WorldSession> session;
    {
      std::lock_guard lock(state_->combat_order_mutex);
      const auto found = state_->combat_order_owners.find(combat_wake_key(timeline, *owner));
      if (found != state_->combat_order_owners.end()) {
        session = found->second.lock();
      }
    }
    if (!session) {
      throw std::runtime_error("Ordered combat head has no retained native owner");
    }
    asio::post(session->io_executor(), [session, timeline, owner] {
      if (session->combat_schedule_ && session->combat_schedule_->timeline() == timeline &&
          session->combat_schedule_->owner() == *owner) {
        session->pump_ordered_combat();
      }
    });
  }

  void retire_combat_schedule() {
    if (!combat_schedule_) {
      return;
    }
    const auto timeline = combat_schedule_->timeline();
    const auto key = combat_wake_key(timeline, combat_schedule_->owner());
    combat_schedule_->retire();
    {
      std::lock_guard lock(state_->combat_order_mutex);
      state_->combat_order_owners.erase(key);
    }
    combat_schedule_.reset();
    ordered_close_pending_.store(false);
    wake_ordered_combat_head(timeline);
  }

  void finish_ordered_combat(const CombatScheduleSnapshot &snapshot) {
    if (combat_schedule_) {
      const auto old_timeline = combat_schedule_->timeline();
      const bool same_population = snapshot.timeline == old_timeline;
      if (combat_schedule_->active()) {
        combat_schedule_->complete(same_population ? snapshot.deadline : std::nullopt);
      }
      if (!same_population && !combat_schedule_->closing()) {
        retire_combat_schedule();
      } else {
        wake_ordered_combat_head(old_timeline);
      }
    }
    if (!combat_schedule_ && snapshot.timeline && !closed()) {
      auto schedule = std::make_unique<CombatWorkSchedule>(snapshot.timeline, connection_id(),
                                                           shared_session());
      const auto key = combat_wake_key(snapshot.timeline, schedule->owner());
      {
        std::lock_guard lock(state_->combat_order_mutex);
        if (!state_->combat_order_owners.emplace(key, shared_session()).second) {
          schedule->retire();
          throw std::logic_error("Native combat wake identity was reused");
        }
      }
      combat_schedule_ = std::move(schedule);
      ordered_close_pending_.store(true);
      ordered_close_submitted_.store(false);
      ordered_close_tick_.reset();
      ordered_close_ready_.store(false);
      ordered_failed_combat_detached_ = false;
      gameplay_timer_.cancel();
      // Joining a population has not executed combat. Admit its first observation at
      // registration time, rather than inserting an old deadline behind the frontier.
      const auto first_tick =
          snapshot.deadline
              ? std::optional<std::int64_t>(std::max(*snapshot.deadline, monotonic_ticks()))
              : std::nullopt;
      combat_schedule_->initialize(first_tick);
    }
    if (combat_schedule_) {
      pump_ordered_combat();
    }
  }

  void pump_ordered_combat() {
    if (!combat_schedule_ || combat_schedule_->active()) {
      return;
    }
    const auto next = combat_schedule_->next();
    if (!next) {
      return;
    }
    const auto claimed = combat_schedule_->claim(monotonic_ticks());
    using Timeline = combat::CombatTimeline;
    if (claimed.status == Timeline::ClaimStatus::waiting) {
      // Completion wakes the next owner. Only wake another owner here; posting to
      // ourselves while another operation is claimed would spin the I/O executor.
      if (claimed.head_owner && *claimed.head_owner != combat_schedule_->owner()) {
        wake_ordered_combat_head(combat_schedule_->timeline());
      }
      return;
    }
    if (claimed.status == Timeline::ClaimStatus::not_due) {
      gameplay_timer_.expires_after(std::chrono::nanoseconds(
          std::max<std::int64_t>(0, next->token.tick - monotonic_ticks()) * 100));
      gameplay_timer_.async_wait([self = shared_session()](boost::system::error_code error) {
        if (!error) {
          self->pump_ordered_combat();
        }
      });
      return;
    }
    if (claimed.status != Timeline::ClaimStatus::ready) {
      throw std::logic_error("Native combat work became stale before dispatch");
    }
    ordered_dispatch_tick_.store(next->token.tick);
    if (next->kind == Timeline::Kind::input) {
      process_frame(next->token.tick);
    } else if (next->kind == Timeline::Kind::milestone) {
      resolve_gameplay_tick(next->token.tick);
    } else {
      ordered_close_ready_.store(true);
      run_close_hook_stages();
    }
  }

  void process_frame(std::optional<std::int64_t> admitted_tick = std::nullopt) {
    try {
      auto self = shared_session();
      const auto posted_at = world_metrics::post_stamp();
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
      if (frame_worker_post_fault_armed.exchange(false)) {
        frame_worker_post_fault_observed.store(true);
        if (worker_post_fault_unknown.load()) {
          throw 1;
        }
        throw std::bad_alloc();
      }
#endif
      asio::post(worker_strand_, [self, posted_at, admitted_tick] {
        try {
          const ScopedCombatTick combat_tick(self->admitted_combat_tick_, admitted_tick);
          if ((!admitted_tick && (self->closed() || self->admin_disconnect_requested_.load() ||
                                  self->state_->shutting_down.load())) ||
              self->preserve_durable_location_on_close_.load()) {
            if (admitted_tick) {
              self->ordered_worker_failed_.store(true);
              self->request_supervised_close();
            }
            return;
          }
          world_metrics::record_strand_latency(world_metrics::HistogramId::FrameStrandLatencyUs,
                                               posted_at);
          const world_metrics::ScopedTimer frame_timer(
              world_metrics::HistogramId::ProcessFrameHandlerUs);
          std::vector<GameplayPacket> recovered_rewards;
          if (self->incoming_.opcode() == kStartPlayingRequest) {
            self->process_pending_monster_rewards(&recovered_rewards);
          }
          WorldPacketMetadata metadata;
          self->current_boss_origin_ = BossOrigin::frame;
          auto outcome = self->frame_pipeline_.run(self->incoming_.opcode(), metadata);
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
          if (gameplay_completion_fault_armed.load()) {
            self->resolve_gameplay_tick();
          }
#endif
          auto response = std::move(outcome.response);
          auto pending_notifications = std::move(outcome.pending_notifications);
          if (!recovered_rewards.empty()) {
            std::lock_guard lock(self->state_->protocol_mutex);
            for (const auto &packet : recovered_rewards) {
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
              if (recovered_reward_encoding_fault_armed.exchange(false)) {
                recovered_reward_encoding_fault_observed.store(true);
                throw std::bad_alloc();
              }
#endif
              pending_notifications.push_back(self->state_->protocol.encode(packet.opcode, packet.message));
            }
          }
          const auto action_deadline = outcome.action_deadline;
          const auto combat_snapshot = self->combat_schedule_snapshot(action_deadline);
          const bool rejected = outcome.rejected;
          const auto boss_origin = self->bind_boss_origin(BossOrigin::frame);

          try {
            std::vector<Frame> frames;
            frames.reserve(pending_notifications.size() + (response ? 1 : 0));
            if (response) {
              frames.push_back(std::move(*response));
            }
            for (auto &notification : pending_notifications) {
              frames.push_back(std::move(notification));
            }
            auto lease = self->reserve_posted_frames(frames);
            if (!lease) {
              spdlog::warn("World response exceeded pending I/O frame budget connection={}",
                           self->connection_id());
              if (boss_origin) {
                boss_origin->origin_bound = false;
              }
              self->ordered_worker_failed_.store(true);
              self->request_supervised_close();
              return;
            }
            asio::post(self->io_executor(),
                       [self, frames = std::move(frames), action_deadline, rejected,
                        lease = std::move(lease), boss_origin, combat_snapshot]() mutable {
                         static_cast<void>(lease);
                         try {
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
                           if (frame_response_io_fault_armed.exchange(false)) {
                             frame_response_io_fault_observed.store(true);
                             throw std::bad_alloc();
                           }
#endif
                           self->finish_ordered_combat(combat_snapshot);
                           if (self->closed()) {
                             self->wake_boss_publication_after_origin(boss_origin);
                             return;
                           }
                           if (rejected) {
                             self->close_on_io();
                             self->wake_boss_publication_after_origin(boss_origin);
                             return;
                           }
                           auto next_action_deadline = action_deadline;
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
                           if (tick_worker_post_fault_armed.load() ||
                               tick_response_io_fault_armed.load()) {
                             constexpr std::int64_t kTestTimerDelayTicks = 100000;
                             next_action_deadline = monotonic_ticks() + kTestTimerDelayTicks;
                           }
#endif
                           self->schedule_gameplay_tick(next_action_deadline);
                           // Resolve presence after preceding I/O map-transition resets.
                           self->complete_request(std::move(frames), self->presence_published_);
                           self->wake_boss_publication_after_origin(boss_origin);
                         } catch (...) {
                           self->ordered_worker_failed_.store(true);
                           self->request_supervised_close();
                           try {
                             self->close_on_io();
                           } catch (...) {
                           }
                           self->wake_boss_publication_after_origin(boss_origin);
                         }
                       });
          } catch (const std::exception &error) {
            if (boss_origin) {
              boss_origin->origin_bound = false;
            }
            try {
              spdlog::error("Could not schedule world response connection={}: {}",
                            self->connection_id(), error.what());
            } catch (...) {
            }
            self->ordered_worker_failed_.store(true);
            self->request_supervised_close();
          } catch (...) {
            if (boss_origin) {
              boss_origin->origin_bound = false;
            }
            throw;
          }
        } catch (const std::exception &error) {
          try {
            spdlog::error("World frame worker failed connection={}: {}",
                          self->connection_id(), error.what());
          } catch (...) {
          }
          self->ordered_worker_failed_.store(true);
          self->request_supervised_close();
        } catch (...) {
          try {
            spdlog::critical("World frame worker failed connection={} with an unknown error",
                             self->connection_id());
          } catch (...) {
          }
          self->ordered_worker_failed_.store(true);
          self->request_supervised_close();
        }
      });
    } catch (...) {
      try {
        spdlog::critical("Could not dispatch world frame worker connection={}", connection_id());
      } catch (...) {
      }
      ordered_worker_failed_.store(true);
      request_supervised_close();
    }
  }

  void schedule_gameplay_tick(std::optional<std::int64_t> deadline) {
    if (combat_schedule_) {
      pump_ordered_combat();
      return;
    }
    if (!gameplay_tick_gate_.reschedule(deadline)) {
      return;
    }
    gameplay_timer_.cancel();
    if (!deadline) {
      return;
    }
    const auto remaining_ticks = std::max<std::int64_t>(0, *deadline - monotonic_ticks());
    gameplay_timer_.expires_after(std::chrono::nanoseconds(remaining_ticks * 100));
    gameplay_timer_.async_wait(
        [self = shared_session(),
         generation = gameplay_tick_gate_.generation()](boost::system::error_code error) {
          if (error || self->closed() || !self->gameplay_tick_gate_.try_start(generation)) {
            return;
          }
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
          if (tick_worker_post_fault_armed.load() || tick_response_io_fault_armed.load()) {
            tick_worker_post_timer_observed.store(true);
          }
#endif
          self->resolve_gameplay_tick();
        });
  }

  std::optional<std::int64_t> next_gameplay_tick() const {
    auto deadline = tutorial_player_->next_action_tick();
    if (adventure_) {
      const auto adventure_deadline = adventure_->next_action_tick();
      if (adventure_deadline && (!deadline || *adventure_deadline < *deadline)) {
        deadline = adventure_deadline;
      }
    }
    if (companions_) {
      const auto registration_deadline = companions_->registration_deadline();
      if (registration_deadline && (!deadline || *registration_deadline < *deadline)) {
        deadline = registration_deadline;
      }
    }
    const auto statue_deadline = nuia_session_->next_action_tick();
    if (statue_deadline && (!deadline || *statue_deadline < *deadline)) {
      deadline = statue_deadline;
    }
    const auto patrol_deadline = npc_player_->next_action_tick();
    if (patrol_deadline && (!deadline || *patrol_deadline < *deadline)) {
      deadline = patrol_deadline;
    }
    const auto combat_deadline = combat_player_->next_action_tick();
    if (combat_deadline && (!deadline || *combat_deadline < *deadline)) {
      deadline = combat_deadline;
    }
    if (loot_player_->has_session()) {
      const auto corpse_deadline = loot_player_->next_action_tick();
      if (corpse_deadline && (!deadline || *corpse_deadline < *deadline)) {
        deadline = corpse_deadline;
      }
    }
    const auto watch_deadline = gm_session_->next_watch_tick();
    if (watch_deadline && (!deadline || *watch_deadline < *deadline)) {
      deadline = watch_deadline;
    }
    const auto travel_deadline = deferred_travel_retry_tick();
    if (travel_deadline && (!deadline || *travel_deadline < *deadline)) {
      deadline = travel_deadline;
    }
    return deadline;
  }

  void resolve_gameplay_tick(std::optional<std::int64_t> admitted_tick = std::nullopt) {
    try {
      const auto posted_at = world_metrics::post_stamp();
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
      if (tick_worker_post_fault_armed.exchange(false)) {
        tick_worker_post_fault_observed.store(true);
        if (worker_post_fault_unknown.load()) {
          throw 1;
        }
        throw std::bad_alloc();
      }
#endif
      asio::post(worker_strand_, [self = shared_session(), posted_at, admitted_tick] {
        try {
          const ScopedCombatTick combat_tick(self->admitted_combat_tick_, admitted_tick);
          if (!admitted_tick && self->ordered_close_pending_.load()) {
            self->complete_gameplay_tick({}, std::nullopt, false, false);
            return;
          }
          if ((!admitted_tick && self->admin_disconnect_requested_.load()) ||
              self->preserve_durable_location_on_close_.load()) {
            if (admitted_tick) {
              self->ordered_worker_failed_.store(true);
              self->request_supervised_close();
            }
            return;
          }
          world_metrics::record_strand_latency(world_metrics::HistogramId::TickStrandLatencyUs,
                                               posted_at);
          const world_metrics::ScopedTimer tick_timer(world_metrics::HistogramId::GameplayTickUs);
          if (self->closed() && !admitted_tick) {
            return;
          }
          std::vector<Frame> notifications;
          std::optional<std::int64_t> deadline;
          bool rejected = false;
          try {
            std::optional<WorldPosition> position;
            {
              std::lock_guard lock(self->state_->protocol_mutex);
              position = self->handshake_.current_position();
            }
            if (!position) {
              // A timer can expire while the worker is moving maps, then run after
              // begin_map_move has hidden the position until MapLoadCompleteReq. The
              // load-completion packet re-arms gameplay; this stale tick must not close TLS.
              spdlog::info("World gameplay timer deferred during map loading connection={}",
                           self->connection_id());
              self->complete_gameplay_tick({}, std::nullopt, false, admitted_tick.has_value());
              return;
            }
            if (self->resume_deferred_travel(*position)) {
              // The client is loading another map (or the session is closing); the departed
              // map's tick must not run. Map-load completion re-arms gameplay.
              self->complete_gameplay_tick({}, std::nullopt, false, admitted_tick.has_value());
              return;
            }
            if (!self->tutorial_player_->current_instance() &&
                !self->combat_player_->has_simulation() && !self->npc_player_->has_patrol() &&
                !self->nuia_session_->has_statue_session() &&
                !(self->companions_ && self->companions_->registration_busy()) &&
                !(self->adventure_ && self->adventure_->busy())) {
              self->complete_gameplay_tick({}, self->deferred_travel_retry_tick(), false,
                                           admitted_tick.has_value());
              return;
            }
            // Movement deadlines share the packet worker; accepted surfacing cancels drowning.
            const auto ticks = admitted_tick.value_or(monotonic_ticks());
            std::vector<GameplayPacket> packets;
            if (self->npc_player_->has_patrol()) {
              packets = self->npc_player_->advance_patrol(ticks, *position);
            }
            if (self->tutorial_player_->current_instance()) {
              auto tutorial_packets = self->tutorial_player_->advance(ticks, *position);
              packets.insert(packets.end(), std::make_move_iterator(tutorial_packets.begin()),
                             std::make_move_iterator(tutorial_packets.end()));
            }
            self->current_boss_origin_ = BossOrigin::tick;
            self->combat_player_->advance_gameplay_tick(ticks, *position, packets);
            if (self->adventure_) {
              auto adventure_packets =
                  self->adventure_->advance(ticks, position, self->adventure_blocked());
              self->deliver_interaction_observers(adventure_packets);
              packets.insert(packets.end(), std::make_move_iterator(adventure_packets.begin()),
                             std::make_move_iterator(adventure_packets.end()));
            }
            if (self->companions_) {
              auto registration = self->companions_->advance_registration(ticks, *position,
                                                                          self->registration_blocked());
              self->deliver_interaction_observers(registration.own);
              packets.insert(packets.end(), std::make_move_iterator(registration.own.begin()),
                             std::make_move_iterator(registration.own.end()));
            }
            if (self->loot_player_->has_session()) {
              auto corpse_packets = self->loot_player_->advance(ticks);
              packets.insert(packets.end(), std::make_move_iterator(corpse_packets.begin()),
                             std::make_move_iterator(corpse_packets.end()));
            }
            if (self->nuia_session_->has_statue_session()) {
              auto expired = self->nuia_session_->advance(ticks, *position);
              packets.insert(packets.end(), std::make_move_iterator(expired.begin()),
                             std::make_move_iterator(expired.end()));
            }
            if (self->gm_session_->watches_due(ticks)) {
              auto watch_packets = self->debug_watch_packets(position, ticks);
              self->gm_session_->mark_watches_emitted(ticks);
              packets.insert(packets.end(), std::make_move_iterator(watch_packets.begin()),
                             std::make_move_iterator(watch_packets.end()));
            }
            if (self->tutorial_player_->current_instance()) {
              self->apply_level_change(self->tutorial_player_->current_instance()->progress(), packets);
            }
            self->inventory_session_->append_encumbrance_changes(packets, false);
            self->persist_player_death(packets);
            std::vector<Frame> dismount_notifications;
            auto dismounted = self->dismount_dead_companion();
            deliver_companion_update(*self->state_, dismounted.broadcast, self->connection_id(),
                                     &dismount_notifications);
            self->encode_companion_packets(dismounted.own, dismount_notifications);
            self->encode_companion_packets(dismounted.trailing, dismount_notifications);
            deadline = self->next_gameplay_tick();
            std::lock_guard lock(self->state_->protocol_mutex);
            for (const auto &packet : packets) {
              notifications.push_back(self->state_->protocol.encode(packet.opcode, packet.message));
            }
            notifications.insert(notifications.end(),
                                 std::make_move_iterator(dismount_notifications.begin()),
                                 std::make_move_iterator(dismount_notifications.end()));
          } catch (const std::exception &failure) {
            spdlog::error("World timed gameplay action failed: {}", failure.what());
            rejected = true;
            self->ordered_worker_failed_.store(true);
          }
          self->complete_gameplay_tick(std::move(notifications), deadline, rejected,
                                       admitted_tick.has_value());
        } catch (const std::exception &error) {
          try {
            spdlog::error("World gameplay worker failed connection={}: {}",
                          self->connection_id(), error.what());
          } catch (...) {
          }
          self->ordered_worker_failed_.store(true);
          self->request_supervised_close();
        } catch (...) {
          try {
            spdlog::critical("World gameplay worker failed connection={} with an unknown error",
                             self->connection_id());
          } catch (...) {
          }
          self->ordered_worker_failed_.store(true);
          self->request_supervised_close();
        }
      });
    } catch (...) {
      try {
        spdlog::critical("Could not dispatch gameplay tick worker connection={}", connection_id());
      } catch (...) {
      }
      ordered_worker_failed_.store(true);
      request_supervised_close();
    }
  }

  void complete_gameplay_tick(std::vector<Frame> notifications,
                              std::optional<std::int64_t> deadline, bool rejected,
                              bool admitted = false) {
    const auto io_posted_at = world_metrics::post_stamp();
    const auto boss_origin = bind_boss_origin(BossOrigin::tick);
    const auto combat_snapshot = combat_schedule_snapshot(deadline);
    try {
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
      if (gameplay_completion_fault_armed.exchange(false)) {
        gameplay_completion_fault_observed.store(true);
        if (gameplay_completion_fault_unknown.load()) {
          throw 1;
        }
        throw std::bad_alloc();
      }
#endif
      asio::post(io_executor(), [self = shared_session(), notifications = std::move(notifications),
                                 deadline, rejected, io_posted_at, boss_origin, combat_snapshot,
                                 admitted]() mutable {
        try {
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
          if (tick_response_io_fault_armed.exchange(false)) {
            tick_response_io_fault_observed.store(true);
            throw std::bad_alloc();
          }
#endif
          world_metrics::record_strand_latency(world_metrics::HistogramId::IoPostToRunUs,
                                               io_posted_at);
          self->gameplay_tick_gate_.complete();
          if (!rejected && (admitted || !self->combat_schedule_)) {
            self->finish_ordered_combat(combat_snapshot);
          }
          if (self->closed()) {
            self->wake_boss_publication_after_origin(boss_origin);
            return;
          }
          if (rejected) {
            self->close_on_io();
            self->wake_boss_publication_after_origin(boss_origin);
            return;
          }
          for (auto &frame : notifications) {
            self->enqueue_frame(std::move(frame));
          }
          self->schedule_gameplay_tick(deadline);
          self->wake_boss_publication_after_origin(boss_origin);
        } catch (...) {
          self->ordered_worker_failed_.store(true);
          self->request_supervised_close();
          try {
            self->close_on_io();
          } catch (...) {
          }
          self->wake_boss_publication_after_origin(boss_origin);
        }
      });
    } catch (...) {
      if (boss_origin) {
        boss_origin->origin_bound = false;
      }
      throw;
    }
  }

  void capture_close_presence_events() {
    constexpr std::size_t kMaximumCloseRecipients = 4096;
    const auto capacity = std::min<std::size_t>(state_->maximum_connections,
                                                kMaximumCloseRecipients);
    auto budget = gathering::ConstructionBudget::acquire();
    budget->charge(gathering::ConstructionBudget::kSectorBytes);
    for (std::size_t index = 0; index < capacity; ++index) {
      budget->charge(gathering::ConstructionBudget::kNodeBytes);
    }
    std::vector<ClosePresenceTarget> captured;
    static_assert(std::is_nothrow_move_constructible_v<ClosePresenceTarget>);
    captured.reserve(capacity);
    bool more = false;
    {
      // Presence changes and recipient map/copy publication share this ordering domain.
      std::lock_guard lock(state_->protocol_mutex);
      state_->presence.begin_leave(connection_id());
      auto page = state_->presence.leave_page(connection_id(), capacity, [&](const auto &event) {
        const auto found = state_->sessions.find(event.recipient_connection_id);
        const auto recipient = found == state_->sessions.end() ? nullptr : found->second.lock();
        if (!recipient || recipient->closed()) {
          return;
        }
        const auto view = recipient->gathering_view_.snapshot();
        if (!view.map_id) {
          return;
        }
        captured.push_back({event, recipient, view});
      });
      if (!page.more) {
        state_->sessions.erase(connection_id());
      }
      more = page.more;
    }
    {
      std::lock_guard lock(close_presence_mutex_);
      close_presence_events_ = std::move(captured);
      close_presence_capture_budget_ = std::move(budget);
      close_presence_events_ready_ = true;
      close_presence_leave_pending_ = false;
      close_presence_more_ = more;
    }
  }

  std::shared_ptr<ClosePresenceBatch> make_close_presence_batch(
      const std::vector<ClosePresenceTarget> &events) {
    constexpr std::size_t kMaximumCloseRecipients = 4096;
    if (events.size() > kMaximumCloseRecipients) {
      throw std::runtime_error("Close presence exceeds its recipient limit");
    }
    auto batch = std::make_shared<ClosePresenceBatch>();
    batch->budget = gathering::ConstructionBudget::acquire();
    batch->budget->charge(gathering::ConstructionBudget::kSectorBytes);
    for (std::size_t index = 0; index < events.size(); ++index) {
      batch->budget->charge(gathering::ConstructionBudget::kNodeBytes);
    }
    std::vector<RetainedFrameDelivery::Target> targets;
    targets.reserve(events.size());
    batch->recipients.reserve(events.size());
    batch->views.reserve(events.size());
    std::lock_guard lock(state_->protocol_mutex);
    for (const auto &captured : events) {
      const auto &event = captured.event;
      if (event.kind != WorldPresenceEventKind::remove) {
        throw std::logic_error("Unexpected non-removal close presence event");
      }
      const auto recipient = captured.recipient.lock();
      if (!recipient || recipient->closed()) {
        continue;
      }
      const auto view = captured.view;
      if (!view.map_id) {
        throw std::runtime_error("Close presence recipient has no settled view");
      }
      auto frame = state_->protocol.encode(kUnitRemoveNotification, event.message);
      std::weak_ptr<WorldSession> weak_recipient = recipient;
      frame.prepare_for_enqueue = [weak_recipient, view](Frame &queued) {
        const auto current = weak_recipient.lock();
        if (!current || current->closed()) {
          return false;
        }
        auto guard = current->gathering_view_.acquire_queue_guard(
            view, std::nullopt, std::nullopt, false, current);
        if (!guard) {
          return false;
        }
        queued.enqueue_scope = std::move(guard);
        return true;
      };
      targets.push_back({event.recipient_connection_id, view, {std::move(frame)}});
      batch->recipients.push_back(recipient);
      batch->views.push_back(view);
    }
    batch->delivery = std::make_shared<RetainedFrameDelivery>(
        "close-" + std::to_string(connection_id()), std::move(targets));
    return batch;
  }

  // Worker strand only. Each recipient has at most one outstanding post; a timeout or
  // uncertain post closes that view instead of replaying a possibly inserted removal.
  void drive_close_presence_batch() noexcept {
    try {
      drive_close_presence_batch_impl();
    } catch (...) {
      {
        std::lock_guard lock(close_presence_mutex_);
        close_presence_failed_ = true;
      }
      if (close_presence_failed_attempts_ < 10) {
        ++close_presence_failed_attempts_;
      }
      spdlog::critical("Close presence recovery failed connection={}; admission remains fenced",
                       connection_id());
      schedule_close_presence_retry();
    }
  }

  void drive_close_presence_batch_impl() {
    bool leave_pending = false;
    {
      std::lock_guard lock(close_presence_mutex_);
      leave_pending = close_presence_leave_pending_;
    }
    if (leave_pending) {
      capture_close_presence_events();
    }
    std::shared_ptr<ClosePresenceBatch> batch;
    {
      std::lock_guard lock(close_presence_mutex_);
      batch = close_presence_batch_;
    }
    if (!batch) {
      auto retry_budget = gathering::ConstructionBudget::acquire();
      retry_budget->charge(gathering::ConstructionBudget::kSectorBytes);
      std::vector<ClosePresenceTarget> events;
      {
        std::lock_guard lock(close_presence_mutex_);
        if (!close_presence_events_ready_) {
          return;
        }
        if (close_presence_events_.size() > 4096) {
          throw std::runtime_error("Close presence exceeds its recipient limit");
        }
        for (std::size_t index = 0; index < close_presence_events_.size(); ++index) {
          retry_budget->charge(gathering::ConstructionBudget::kNodeBytes);
        }
        events = close_presence_events_;
      }
      batch = make_close_presence_batch(events);
      {
        std::lock_guard lock(close_presence_mutex_);
        close_presence_batch_ = batch;
        close_presence_events_.clear();
        close_presence_events_ready_ = false;
        close_presence_capture_budget_.reset();
        close_presence_failed_ = false;
        close_presence_failed_attempts_ = 0;
      }
    }
    if (batch->delivery->complete()) {
      complete_close_presence_page(batch);
      return;
    }
    for (std::size_t index = 0; index < batch->recipients.size(); ++index) {
      const auto recipient = batch->recipients[index].lock();
      if (!recipient || recipient->closed() ||
          !recipient->gathering_view_.matches(batch->views[index])) {
        batch->delivery->confirm_departed(index);
        continue;
      }
      if (batch->delivery->close_required(index)) {
        try {
          recipient->request_close();
        } catch (...) {
          spdlog::error("Could not close unresolved presence recipient connection={}",
                        recipient->connection_id());
        }
        continue;
      }
      const auto attempt = batch->delivery->begin(index);
      if (!attempt) {
        continue;
      }
      std::weak_ptr<WorldSession> weak_owner = shared_session();
      std::weak_ptr<WorldSession> weak_recipient = recipient;
      recipient->post_frame(attempt->frame,
          [batch, weak_owner, weak_recipient, attempt = *attempt](FrameEnqueueStatus status) {
            const auto target = weak_recipient.lock();
            const bool same_view = target && !target->closed() &&
                                   target->gathering_view_.matches(attempt.view);
            if (!batch->delivery->finish(attempt, status, same_view)) {
              return;
            }
            if (const auto owner = weak_owner.lock()) {
              try {
                asio::post(owner->worker_strand_, [owner] {
                  owner->drive_close_presence_batch();
                });
              } catch (...) {
                spdlog::critical("Close presence dispatch failed; recovery remains fenced");
              }
            }
          });
    }
    if (batch->delivery->complete()) {
      complete_close_presence_page(batch);
      return;
    }
    schedule_close_presence_retry();
  }

  void complete_close_presence_page(const std::shared_ptr<ClosePresenceBatch> &batch) {
    bool more = false;
    {
      std::lock_guard lock(close_presence_mutex_);
      if (close_presence_batch_ != batch) {
        return;
      }
      close_presence_batch_.reset();
      more = close_presence_more_;
      close_presence_leave_pending_ = more;
      close_presence_complete_ = !more;
    }
    if (more) {
      drive_close_presence_batch();
    } else if (closed()) {
      release_settled_character_admission();
    }
  }

  void schedule_close_presence_retry() noexcept {
    if (close_presence_retry_armed_) {
      return;
    }
    try {
      const auto seconds = close_presence_failed_attempts_ >= 10
                               ? 60U
                               : std::min(1U << std::min(close_presence_failed_attempts_, 5U), 30U);
      close_presence_retry_timer_.expires_after(std::chrono::seconds(seconds));
      close_presence_retry_timer_.async_wait(
          [self = shared_session()](const boost::system::error_code &error) {
            try {
              asio::post(self->worker_strand_, [self, error] {
                self->close_presence_retry_armed_ = false;
                if (error) {
                  spdlog::error("Close presence retry timer failed connection={}: {}",
                                self->connection_id(), error.message());
                }
                std::shared_ptr<ClosePresenceBatch> batch;
                {
                  std::lock_guard lock(self->close_presence_mutex_);
                  batch = self->close_presence_batch_;
                }
                if (batch) {
                  for (std::size_t index = 0; index < batch->recipients.size(); ++index) {
                    batch->delivery->timeout(index);
                  }
                }
                self->drive_close_presence_batch();
              });
            } catch (...) {
              spdlog::critical("Close presence retry dispatch failed; recovery remains fenced");
            }
          });
      close_presence_retry_armed_ = true;
    } catch (...) {
      spdlog::critical("Close presence retry scheduling failed; recovery remains fenced");
    }
  }

  void deliver_presence_events(std::vector<WorldPresenceEvent> events) {
    struct Delivery {
      std::shared_ptr<WorldSession> recipient;
      Frame frame;
    };

    // Every field of a presence event message is derived from the SUBJECT
    // (mover/unit), never from the receiving session: party_id/faction/guild
    // decorations resolve the subject's own state. Identical messages can
    // therefore share one encoded payload across recipients.
    struct EncodedEvent {
      std::shared_ptr<const Bytes> wire;
      std::optional<GameplayPacket> selfie;
      std::optional<GameplayPacket> housing_pose;
      bool failed{false};
    };

    std::vector<Delivery> deliveries;
    {
      std::lock_guard lock(state_->protocol_mutex);
      // message content -> encoded shared payload; keyed by full JSON value so
      // only provably identical bodies ever share bytes (no opcode-only dedup).
      std::map<nlohmann::json, EncodedEvent> encoded;
      for (const auto &event : events) {
        const auto recipient = state_->sessions.find(event.recipient_connection_id);
        if (recipient == state_->sessions.end()) {
          continue;
        }

        auto session = recipient->second.lock();
        if (!session) {
          continue;
        }

        auto &slot = encoded[event.message];
        if (!slot.wire && !slot.failed) {
          const std::uint16_t opcode = event.kind == WorldPresenceEventKind::add
                                           ? kUnitAddNotification
                                           : event.kind == WorldPresenceEventKind::remove
                                                 ? kUnitRemoveNotification
                                                 : kMoveNotification;
          try {
            // Grouped players carry PlayerNub.party_id, as in the official captures.
            auto message = event.message;
            if (event.kind == WorldPresenceEventKind::add) {
              state_->combat_feature.player_pvp().decorate_unit_add(message);
              state_->party_feature->world()->decorate_unit_add(message);
              if (state_->guild_feature->world()) {
                state_->guild_feature->world()->decorate_unit_add(message);
              }
              // A player already in selfie mode keeps its pose for viewers who arrive later.
              slot.selfie = state_->expression_feature.late_viewer_notification(message);
              // Likewise a player still seated on housing furniture.
              if (state_->housing_feature) {
                if (auto pose = state_->housing_feature->late_viewer_pose(message)) {
                  slot.housing_pose = GameplayPacket{pose->opcode, std::move(pose->message)};
                }
              }
            }
            auto frame = state_->protocol.encode(opcode, message);
            slot.wire = std::make_shared<const Bytes>(std::move(frame.bytes));
          } catch (const std::exception &error) {
            slot.failed = true;
            spdlog::warn("Native world presence notification encoding failed: {}", error.what());
          }
        }
        if (!slot.wire) {
          continue;
        }

        Frame frame;
        frame.shared = slot.wire;
        deliveries.push_back({session, std::move(frame)});
        for (const auto *late : {&slot.selfie, &slot.housing_pose}) {
          if (!*late) {
            continue;
          }
          try {
            deliveries.push_back(
                {session, state_->protocol.encode((*late)->opcode, (*late)->message)});
          } catch (const std::exception &error) {
            spdlog::warn("Native world presence notification encoding failed: {}",
                         error.what());
          }
        }
      }
    }

    for (auto &delivery : deliveries) {
      const auto executor = delivery.recipient->io_executor();
      const auto posted_at = world_metrics::post_stamp();
      asio::post(executor, [recipient = std::move(delivery.recipient),
                            frame = std::move(delivery.frame), posted_at]() mutable {
        world_metrics::record_strand_latency(world_metrics::HistogramId::IoPostToRunUs,
                                             posted_at);
        recipient->enqueue_frame(std::move(frame));
      });
    }
  }

  void publish_presence() {
    const int housing_map = housing_session_->presence_map();
    if (tutorial_player_->current_instance() && housing_map == 0) {
      // Map 8000 is private to this character. Shared-world presence begins
      // only after a later, server-controlled transition into a public map.
      return;
    }
    std::vector<WorldPresenceEvent> events;
    try {
      std::lock_guard lock(state_->protocol_mutex);
      auto snapshot = handshake_.presence_snapshot();
      if (!snapshot) {
        return;
      }
      if (const int presence_map = presence_map_override(); presence_map != 0) {
        snapshot->position.map_id = presence_map;
      }
      // An invisible game master re-enters presence hidden after every map move.
      snapshot->hidden = gm_session_->invisible();

      state_->presence.enter(connection_id(), *snapshot, events);
      presence_published_ = true;
    } catch (const std::exception &error) {
      spdlog::warn("Native world presence registration failed: {}", error.what());
      close_on_io();
      return;
    }

    deliver_presence_events(std::move(events));
  }

  void prepare_close_companion_batch() {
    auto budget = gathering::ConstructionBudget::acquire();
    budget->charge(gathering::ConstructionBudget::kSectorBytes +
                   gathering::ConstructionBudget::kNodeBytes);
    const auto first = close_companion_detach_->page(close_companion_cursor_, 1);
    if (first.recipients.empty()) {
      if (!first.complete) {
        throw std::logic_error("Companion detach page made no progress");
      }
      close_companion_cursor_ = first.next;
      close_companion_delivery_complete_ = true;
      close_companion_detach_.reset();
      return;
    }

    const auto &packets = *first.recipients.front().packets;
    std::vector<Frame> prototypes;
    prototypes.reserve(packets.size());
    std::size_t shared_bytes = gathering::ConstructionBudget::kSectorBytes +
                               gathering::ConstructionBudget::kNodeBytes;
    std::size_t per_recipient_bytes = 0;
    {
      std::lock_guard lock(state_->protocol_mutex);
      for (const auto &packet : packets) {
        constexpr std::size_t kMaximumWireBytes = 4096;
        budget->charge(kMaximumWireBytes);
        shared_bytes += kMaximumWireBytes;
        auto frame = state_->protocol.encode(packet.opcode, packet.message);
        if (frame.bytes.size() > kMaximumWireBytes) {
          throw std::runtime_error("Companion detach frame exceeds its wire limit");
        }
        frame.shared = std::make_shared<const Bytes>(std::move(frame.bytes));
        const auto bytes = RetainedFrameDelivery::frame_bytes(frame);
        if (!bytes || *bytes > RetainedFrameDelivery::kMaximumBytes - per_recipient_bytes) {
          throw std::runtime_error("Companion detach frame exceeds its delivery limit");
        }
        per_recipient_bytes += *bytes;
        prototypes.push_back(std::move(frame));
      }
    }
    const auto limit = boss_recipient_page_limit(per_recipient_bytes, prototypes.size(),
                                                  shared_bytes);
    budget->charge(limit * (per_recipient_bytes + gathering::ConstructionBudget::kNodeBytes));
    const auto page = close_companion_detach_->page(close_companion_cursor_, limit);
    auto batch = std::make_shared<CloseCompanionBatch>();
    std::vector<RetainedFrameDelivery::Target> targets;
    targets.reserve(page.recipients.size());
    batch->recipients.reserve(page.recipients.size());
    batch->views.reserve(page.recipients.size());
    {
      std::lock_guard lock(state_->protocol_mutex);
      for (const auto &entry : page.recipients) {
        const auto found = state_->sessions.find(entry.connection_id);
        const auto recipient = found == state_->sessions.end() ? nullptr : found->second.lock();
        const GatheringViewIdentity view{entry.view.epoch, entry.view.map_id,
                                         entry.view.copy_channel};
        if (!recipient || recipient->closed() || !recipient->gathering_view_.matches(view)) {
          continue;
        }
        std::vector<Frame> frames;
        frames.reserve(prototypes.size());
        for (const auto &prototype : prototypes) {
          auto frame = prototype;
          recipient->bind_companion_frame(frame, entry.source);
          frames.push_back(std::move(frame));
        }
        targets.push_back({entry.connection_id, view, std::move(frames)});
        batch->recipients.push_back(recipient);
        batch->views.push_back(view);
      }
    }
    batch->delivery = std::make_shared<RetainedFrameDelivery>(
        "companion-close-" + std::to_string(connection_id()), std::move(targets));
    batch->posted_at.resize(batch->recipients.size());
    batch->budget = std::move(budget);
    batch->next = page.next;
    batch->last = page.complete;
    close_companion_batch_ = std::move(batch);
  }

  bool drive_close_companion_delivery() {
    if (close_companion_delivery_complete_) {
      return true;
    }
    if (!close_companion_detach_) {
      close_companion_delivery_complete_ = true;
      return true;
    }
    if (!close_companion_batch_) {
      prepare_close_companion_batch();
    }
    const auto batch = close_companion_batch_;
    if (!batch) {
      return close_companion_delivery_complete_;
    }
    for (std::size_t index = 0; index < batch->recipients.size(); ++index) {
      const auto recipient = batch->recipients[index].lock();
      if (!recipient || recipient->closed() ||
          !recipient->gathering_view_.matches(batch->views[index])) {
        batch->delivery->confirm_departed(index);
        continue;
      }
      if (batch->delivery->needs_rebuild(index) || batch->delivery->close_required(index)) {
        recipient->request_supervised_close();
        continue;
      }
      const auto attempt = batch->delivery->begin(index);
      if (!attempt) {
        constexpr auto kDeliveryTimeout = std::chrono::seconds(30);
        if (batch->posted_at[index] != Clock::time_point{} &&
            Clock::now() - batch->posted_at[index] >= kDeliveryTimeout &&
            batch->delivery->timeout(index)) {
          recipient->request_supervised_close();
        }
        continue;
      }
      batch->posted_at[index] = Clock::now();
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
      {
        std::unique_lock lock(companion_terminal_pause_mutex);
        if (companion_terminal_pause_armed &&
            attempt->connection == companion_terminal_pause_recipient.load()) {
          companion_terminal_pause_armed = false;
          companion_terminal_pause_entered = true;
          companion_terminal_pause_condition.notify_all();
          companion_terminal_pause_condition.wait_for(lock, std::chrono::seconds(30), [] {
            return companion_terminal_pause_released;
          });
        }
      }
#endif
      std::weak_ptr<WorldSession> weak_recipient = recipient;
      recipient->post_frame(attempt->frame,
          [batch, weak_recipient, attempt = *attempt](FrameEnqueueStatus status) {
            const auto current = weak_recipient.lock();
            const bool same_view = current && !current->closed() &&
                                   current->gathering_view_.matches(attempt.view);
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
            if (attempt.connection == companion_terminal_pause_recipient.load() &&
                status == FrameEnqueueStatus::stale && !same_view) {
              companion_terminal_stale_observed.store(true);
            }
#endif
            batch->completion.finish(*batch->delivery, attempt, status, same_view);
          });
    }
    if (!batch->completion.complete(*batch->delivery)) {
      return false;
    }
    close_companion_cursor_ = batch->next;
    close_companion_batch_.reset();
    if (batch->last) {
      close_companion_detach_.reset();
      close_companion_delivery_complete_ = true;
    }
    return close_companion_delivery_complete_;
  }

  void settle_close_world_once() {
    if (close_settlement_ready_) {
      return;
    }
    if (pending_quest_reconciliation_ || has_unsettled_combat_quest_events() ||
        (combat_player_->has_pending_death() && !death_recovery_confirmed_)) {
      return;
    }
    try {
      if (!close_position_captured_) {
        std::lock_guard lock(state_->protocol_mutex);
        auto position = handshake_.last_position();
        if (position) {
          position = events_session_->persisted_position(*position);
        }
        closing_position_ = std::move(position);
        close_position_captured_ = true;
      }
      if (!close_companions_complete_) {
        if (close_companions_started_ && companions_ && !companions_->entered()) {
          if (!close_wagon_captured_ && !close_companion_detach_) {
            spdlog::critical("World close companion outcome is uncertain connection={}; "
                             "admission remains fenced", connection_id());
            return;
          }
          close_companions_complete_ = true;
        } else {
          close_companions_started_ = true;
          const bool settled = !companions_ || leave_companions();
          if (!settled && companions_ && companions_->entered()) {
            // The staged world detach did not publish; its immutable wagon record, if any,
            // remains owned by this admission and will not be overwritten on retry.
            return;
          }
          if (!settled && !close_wagon_captured_) {
            spdlog::critical("World close could not capture wagon recovery connection={}; "
                             "admission remains fenced", connection_id());
            return;
          }
          close_companions_complete_ = true;
        }
      }
      if (!close_companions_complete_) {
        return;
      }
      if (handoff_ && closing_position_ && !close_wagon_captured_ &&
          !preserve_durable_location_on_close_.load() &&
          !map_staging_checkpoint_fence_.load() && !close_position_recorded_) {
        {
          std::lock_guard lock(state_->character_admission_mutex);
          state_->pending_world_settlements.insert_or_assign(
              handoff_->character_id, PendingWorldSettlement{std::nullopt, closing_position_});
        }
        close_position_recorded_ = true;
      }
      if (handoff_ && (close_wagon_captured_ || close_position_recorded_) &&
          !reconcile_pending_settlement(*handoff_)) {
        return;
      }
    } catch (const std::exception &error) {
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
      if (std::string_view(error.what()) == "Companion detach construction budget is exhausted") {
        companion_budget_exhaustion_observed.store(true);
      }
#endif
      spdlog::error("World close settlement failed connection={}: {}", connection_id(),
                    error.what());
      return;
    } catch (...) {
      spdlog::critical("World close settlement failed connection={}; "
                       "admission remains fenced", connection_id());
      return;
    }
    if (!close_events_started_) {
      close_events_started_ = true;
      try {
        if (state_->events_feature->has_raid_service() && handoff_) {
          events_session_->on_leave_world();
        }
        close_events_complete_ = true;
      } catch (const std::exception &error) {
        spdlog::error("World close event detach failed connection={}: {}", connection_id(),
                      error.what());
        return;
      } catch (...) {
        spdlog::critical("World close event detach failed connection={}; "
                         "admission remains fenced", connection_id());
        return;
      }
    }
    if (!close_events_complete_) {
      return;
    }
    // The next entry fences any uncertain transaction and retries a retained settlement.
    close_settlement_ready_ = true;
    release_settled_character_admission();
  }

  // The stage cursor belongs to the I/O executor. Each completed step is advanced before
  // the next can fail; only stages with idempotent removal or cancellation are retried.
  void run_close_hook_stages() noexcept {
    struct HookCompletion {
      std::atomic<bool> &finished;
      ~HookCompletion() {
        finished.store(true);
      }
    } hook_completion{close_hook_finished_};

    try {
      while (close_hook_stage_ != CloseHookStage::done) {
        switch (close_hook_stage_) {
        case CloseHookStage::view:
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
          if (close_hook_retry_fault_armed.load() &&
              close_hook_retry_fault_connection.load() == 0) {
            close_hook_retry_fault_connection.store(connection_id());
          }
#endif
          try {
            std::lock_guard lock(state_->protocol_mutex);
            gathering_view_.invalidate_for_close();
          } catch (...) {
            // A closed transport rejects all later enqueues even if the epoch is exhausted.
          }
          close_hook_stage_ = CloseHookStage::pvp;
          break;
        case CloseHookStage::pvp:
          state_->combat_feature.player_pvp().leave(connection_id());
          close_hook_stage_ = CloseHookStage::expression;
          break;
        case CloseHookStage::expression:
          expression_feature_->on_leave_world();
          close_hook_stage_ = CloseHookStage::population;
          break;
        case CloseHookStage::population:
          population_lease_.release();
          close_hook_stage_ = CloseHookStage::presence;
          break;
        case CloseHookStage::presence:
          {
            std::lock_guard lock(close_presence_mutex_);
            close_presence_leave_pending_ = true;
          }
          close_hook_stage_ = CloseHookStage::party;
          try {
            asio::post(worker_strand_, [self = shared_session()] {
              self->drive_close_presence_batch();
            });
          } catch (...) {
            // The retained supervisor drives this same pending stage.
          }
          break;
        case CloseHookStage::party:
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
          if (close_hook_retry_fault_connection.load() == connection_id()) {
            ++close_hook_party_stage_calls;
          }
#endif
          if (party_session_->attached() && !try_close_party_step([&] {
                deliver_party_update(*state_, party_session_->on_leave_world());
                state_->party_feature->schedule_tick();
              })) {
            // Delivery can have succeeded partially; replay is not safe.
            close_hook_uncertain_.store(true);
            return;
          }
          close_hook_stage_ = CloseHookStage::guild;
          break;
        case CloseHookStage::guild:
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
          if (close_hook_retry_fault_connection.load() == connection_id() &&
              close_hook_retry_fault_armed.exchange(false)) {
            close_hook_retry_fault_observed.store(true);
          }
          if (close_hook_retry_fault_connection.load() == connection_id() &&
              close_hook_retry_fault_blocked.load()) {
            throw std::bad_alloc();
          }
#endif
          guild_session_->on_leave_world();
          close_hook_stage_ = CloseHookStage::chat;
          break;
        case CloseHookStage::chat:
          {
            const auto departure = chat_session_->on_leave_world();
            if (departure == chat::ChatSession::DepartureStatus::uncertain) {
              // The external exit handler may already have observed this departure.
              close_hook_uncertain_.store(true);
              return;
            }
            if (departure == chat::ChatSession::DepartureStatus::pending) {
              return;
            }
          }
          close_hook_stage_ = CloseHookStage::housing;
          break;
        case CloseHookStage::housing:
          housing_session_->on_leave_world();
          close_hook_stage_ = CloseHookStage::settlement;
          break;
        case CloseHookStage::settlement:
          close_hook_stage_ = CloseHookStage::gameplay_timer;
          try {
            asio::post(worker_strand_, [self = shared_session()] {
              self->settle_close_world_once();
            });
          } catch (...) {
            // The retained supervisor also runs settlement on the worker strand.
          }
          break;
        case CloseHookStage::gameplay_timer:
          gameplay_timer_.cancel();
          close_hook_stage_ = CloseHookStage::escape_timer;
          break;
        case CloseHookStage::escape_timer:
          escape_feature_->on_leave_world();
          close_hook_stage_ = CloseHookStage::done;
          break;
        case CloseHookStage::done:
          break;
        }
      }
      close_hook_succeeded_.store(true);
    } catch (...) {
      try {
        spdlog::error("Close hook stage failed connection={}; retrying", connection_id());
      } catch (...) {
      }
    }
  }

  void try_submit_ordered_close() noexcept {
    try {
      if (!combat_schedule_ || !ordered_close_tick_) {
        return;
      }
      // A failed insertion was never admitted. Other owners may have progressed since
      // then, so retry at the current clock; only an accepted close freezes its tick.
      if (!combat_schedule_->closing()) {
        ordered_close_tick_ = monotonic_ticks();
      }
      combat_schedule_->submit_close(*ordered_close_tick_);
      ordered_close_submitted_.store(true);
      pump_ordered_combat();
    } catch (const std::exception &error) {
      try {
        spdlog::error("Ordered combat close remains pending connection={}: {}", connection_id(),
                      error.what());
      } catch (...) {
      }
    } catch (...) {
    }
  }

  void on_connection_closed() override {
    if (!retain_close_presence_owner() || !activate_close_supervisor()) {
      close_hook_finished_.store(true);
      close_hook_uncertain_.store(true);
      try {
        spdlog::critical("Could not retain close recovery connection={}; "
                         "character admission remains fenced", connection_id());
      } catch (...) {
      }
      return;
    }
    if (combat_schedule_) {
      ordered_close_tick_ = monotonic_ticks();
      try_submit_ordered_close();
    } else {
      run_close_hook_stages();
    }
  }

  std::unique_ptr<CombatWorkSchedule> combat_schedule_;
  std::optional<std::int64_t> admitted_combat_tick_;
  std::atomic<bool> ordered_worker_failed_{};
  std::atomic<bool> ordered_close_pending_{};
  std::atomic<std::int64_t> ordered_dispatch_tick_{};
  std::atomic<bool> ordered_close_ready_{};
  std::atomic<bool> ordered_close_submitted_{};
  std::optional<std::int64_t> ordered_close_tick_;
  std::atomic<bool> ordered_close_posted_{};
  bool ordered_failed_combat_detached_{};
  QuestEvents pending_direct_quest_events_;

  asio::steady_timer gameplay_timer_;
  asio::steady_timer monster_reward_retry_timer_;
  asio::steady_timer death_reconciliation_timer_;
  asio::steady_timer close_presence_retry_timer_;
  RetainedRetryGate monster_reward_retry_gate_;
  std::atomic<bool> shutdown_close_in_flight_{};
  bool boss_supervisor_registered_{};
  bool close_supervisor_completed_{};
  bool close_combat_detached_{};
  bool close_position_captured_{};
  bool close_companions_started_{};
  bool close_companions_complete_{};
  std::shared_ptr<companion::CompanionWorld::RetainedDetach> close_companion_detach_;
  companion::CompanionDetachCursor close_companion_cursor_;
  std::shared_ptr<CloseCompanionBatch> close_companion_batch_;
  bool close_companion_delivery_complete_{};
  bool close_position_recorded_{};
  bool close_events_started_{};
  bool close_events_complete_{};
  enum class CloseHookStage {
    view,
    pvp,
    expression,
    population,
    presence,
    party,
    guild,
    chat,
    housing,
    settlement,
    gameplay_timer,
    escape_timer,
    done
  };
  CloseHookStage close_hook_stage_{CloseHookStage::view};
  bool close_hook_failure_reported_{};
  std::atomic<bool> close_hook_finished_{};
  std::atomic<bool> close_hook_succeeded_{};
  std::atomic<bool> close_hook_uncertain_{};
  std::atomic<bool> close_hook_retry_in_flight_{};
  std::shared_ptr<LiveBossPublication> live_boss_publication_;
  BossOrigin current_boss_origin_{BossOrigin::frame};
  std::shared_ptr<WorldState> state_;
  asio::strand<asio::thread_pool::executor_type> worker_strand_;
  GameplayTickGate gameplay_tick_gate_;
  WorldHandshake handshake_;
  // Observer construction only captures this; it never reads a feature or publishes.
  // Every feature consumer is declared later and is destroyed before this bus.
  SessionEventBus session_events_;
  std::unique_ptr<tutorial::TutorialPlayerSession> tutorial_player_;
  std::unique_ptr<nuia::NuiaSession> nuia_session_;
  std::unique_ptr<combat::CombatPlayerSession> combat_player_;
  // Declared after combat_player_: the combat observer borrows its owned private loot domain.
  std::unique_ptr<loot::LootPlayerSession> loot_player_;
  std::unique_ptr<GatheringSession> gathering_;
  std::unique_ptr<guides::Session> contents_guide_session_;
  std::optional<WorldProgress> pending_guide_progress_;
  bool guide_world_ready_{};
  // Declared after tutorial_player_: its persistence callbacks read the tutorial's cached progress.
  std::unique_ptr<ProcessingSession> processing_;
  // Declared after tutorial_player_ and combat_player_: its persistence and effect callbacks use
  // both.
  std::unique_ptr<ItemUseSession> item_use_;
  // Non-tutorial sessions only: mounts, vehicles and mates, and the trade run. Both commit
  // through storage directly, so they never share a revision chain with a tutorial session.
  std::unique_ptr<companion::CompanionSession> companions_;
  std::unique_ptr<adventure::Session> adventure_;
  // Shared-world travel cutscene state; created at the first travel request.
  std::unique_ptr<travel::TravelSession> travel_session_;
  // A travel accepted (or a release handoff owed) while the character was busy. The client
  // keeps its pending map transition, which blocks movement at the boundary, until a server
  // map move arrives; gameplay ticks retry this one. Worker strand only.
  struct DeferredTravel {
    int origin_map_id{};
    // Map travel: the planned destination and its MapMoveStartNoti cutscene.
    WorldPosition destination;
    int cut_scene_id{};
    // Zone channel move: the request, resolved again from the position at retry time.
    std::optional<WorldGameplayRequest> zone_request;
    std::int64_t next_retry_tick{};
    std::int64_t expires_tick{};
  };
  std::optional<DeferredTravel> deferred_travel_;
  // The instance or dungeon copy the character occupies (InstanceCopies), kept across same-map
  // handoffs and released when it leaves the map. Worker strand only; presence publication and
  // gathering delivery read the published presence map and channel instead.
  std::shared_ptr<InstanceCopy> instance_copy_;
  GatheringViewState gathering_view_;
  std::atomic<int> copy_presence_map_{};
  std::atomic<std::uint64_t> copy_channel_{};
  std::atomic<bool> preserve_durable_location_on_close_{};
  std::atomic<bool> map_staging_checkpoint_fence_{};
  std::optional<std::int64_t> admitted_character_;
  // Worker strand only: retries of a world entry whose character was still held by its
  // previous session (bounded by WorldState::maximum_admission_retries).
  int admission_retries_{};
  std::optional<WorldPosition> closing_position_;
  bool close_wagon_captured_{};
  bool close_settlement_ready_{};
  struct QuestReconciliation {
    std::shared_ptr<quests::QuestIntentLedger> ledger;
    quests::QuestIntentLedger::Sequence intent{};
    std::optional<std::string> receipt_id;
    unsigned failed_attempts{};
    bool ledger_resolved{};
    bool reward_retired{};
  };
  std::optional<QuestReconciliation> pending_quest_reconciliation_;
  bool pending_death_owner_{};
  bool death_retry_armed_{};
  bool death_recovery_confirmed_{};
  unsigned death_failed_attempts_{};
  std::mutex close_presence_mutex_;
  std::shared_ptr<ClosePresenceBatch> close_presence_batch_;
  std::vector<ClosePresenceTarget> close_presence_events_;
  bool close_presence_events_ready_{};
  bool close_presence_leave_pending_{};
  bool close_presence_more_{};
  std::shared_ptr<gathering::ConstructionBudget::Lease> close_presence_capture_budget_;
  bool close_presence_failed_{};
  bool close_presence_complete_{};
  bool close_presence_retry_armed_{};
  unsigned close_presence_failed_attempts_{};
  std::unique_ptr<trade::TradeSession> trade_;
  std::optional<Handoff> handoff_;
  bool pending_region_companion_handoff_{};
  // Counts this character in the realm population from credential acceptance until close.
  WorldPopulation::Lease population_lease_;
  // Operator console copy of the accepted identity (no credential bytes). admin_mutex_ is a
  // leaf lock: it may be taken while holding state_->protocol_mutex, never the reverse.
  struct AdminIdentity {
    std::int64_t account_id{};
    std::int64_t character_id{};
    bool game_master{};
    std::string character_name;
  };
  // Set by any thread requesting an operator close; read on the stream executor.
  std::atomic_bool admin_disconnect_requested_{false};
  mutable std::mutex admin_mutex_;
  std::optional<AdminIdentity> admin_identity_;
  // Written in the constructor, before the session is shared.
  std::int64_t connected_at_unix_{};
  // Inventory constructs its owned polish engine at this same member-initialization point.
  std::unique_ptr<inventory::InventorySession> inventory_session_;
  // Units this server announced to this session, resolved only through this session's own
  // registry. Confined to the session worker; destroyed with the session.
  UnitRegistry unit_registry_{kMaxAnnouncedUnits};
  std::unique_ptr<npc::NpcPlayerSession> npc_player_;
  // Sectors this connection was told about on its current shared map (SectorUpdateNoti).
  SectorSightTracker sector_sight_;
  Frame incoming_;
  Clock::time_point last_progress_checkpoint_{};
  std::optional<Clock::time_point> last_unstuck_;
  bool presence_published_{};
  // Worker-strand entry guard. Map transitions and duplicate StartPlaying never resend welcome.
  bool player_services_entered_{};
  // Generic map arrivals also serve natural channels, Events and GM travel.
  std::optional<WorldPosition> pending_exit_;
  std::unique_ptr<housing::HousingSession> housing_session_;
  std::unique_ptr<chat::ChatSession> chat_session_;
  std::unique_ptr<party::PartySession> party_session_;
  std::unique_ptr<guild::GuildSession> guild_session_;
  std::unique_ptr<company::CompanySession> company_session_;
  // Feature session borrows this adapter and the server-owned feature.
  std::unique_ptr<events::EventsSession> events_session_;
  std::unique_ptr<character::CharacterSession> character_session_;
  std::unique_ptr<quests::QuestsSession> quests_session_;
  std::shared_ptr<quests::QuestIntentLedger> quest_intent_ledger_;
  std::unique_ptr<gm::GmSession> gm_session_;
  std::unique_ptr<expression::ExpressionSession> expression_feature_;
  std::unique_ptr<escape::EscapeSession> escape_feature_;
  std::unique_ptr<shop::ShopSession> shop_session_;
  std::unique_ptr<market::MarketSession> market_session_;
  std::unique_ptr<mail::MailSession> mail_session_;
  // Destroy the pipeline before the session fields its operation bindings borrow.
  SessionFramePipeline frame_pipeline_;
};

// The registry is a world/session concern; node selection, encoding policy and timer ownership
// live in GatheringFeature. This stack adapter never escapes the synchronous delivery call.
class GatheringRegistryContext final : public gathering::GatheringWorldContext {
public:
  explicit GatheringRegistryContext(WorldState &state) : state_(state) {
  }

  Frame gathering_encode(const GameplayPacket &packet) override {
    const auto retained_bytes = packet.local_plan ? local_packet_retained_bytes(packet) : 0;
    auto frame = state_.protocol.encode(packet.opcode, packet.message);
    if (packet.local_plan) {
      frame.local_retained_bytes = retained_bytes;
      frame.local_allocation = std::make_shared<const LocalFrameAllocation>(
          LocalFrameAllocation{frame.local_retained_bytes, packet.local_construction});
      frame.local_plan = packet.local_plan;
      frame.local_message = std::make_shared<const nlohmann::json>(packet.message);
    }
    return frame;
  }

  void gathering_each_peer_locked(
      std::uint64_t excluded_connection,
      const std::function<void(gathering::GatheringPeer)> &visitor) override {
    std::lock_guard lock(state_.protocol_mutex);
    for (const auto &[connection_id, weak_session] : state_.sessions) {
      if (connection_id == excluded_connection) {
        continue;
      }
      auto session = weak_session.lock();
      if (!session) {
        continue;
      }
      const auto view = session->current_gathering_view();
      visitor({view.map_id, view.copy_channel,
               [session, view](Frame frame) {
                 session->bind_gathering_frame(frame, view);
                 session->post_frame(std::move(frame));
               },
               [session, view](std::uint32_t sector) {
                 return session->gathering_sector_ready(view, sector);
               }});
    }
  }

private:
  WorldState &state_;
};

void deliver_gathering_changes(WorldState &state, const std::vector<GatheringStateChange> &changes,
                               std::uint64_t channel, std::uint64_t excluded_connection_id) {
  GatheringRegistryContext context(state);
  state.gathering_feature->deliver_changes(changes, channel, excluded_connection_id, context);
}

struct SessionFrameDelivery {
  std::shared_ptr<WorldSession> recipient;
  Frame frame;
};

// Encodes one cross-connection payload (party or guild) for a live connection, or appends it
// to `local_frames` when it addresses the calling session. Requires state.protocol_mutex.
void queue_session_frame(WorldState &state, std::uint64_t connection_id, std::uint16_t opcode,
                         const nlohmann::json &message, std::uint64_t local_connection_id,
                         std::vector<Frame> *local_frames,
                         std::vector<SessionFrameDelivery> &deliveries) {
  const bool local = local_frames != nullptr && connection_id == local_connection_id;
  std::shared_ptr<WorldSession> session;
  if (!local) {
    const auto found = state.sessions.find(connection_id);
    if (found == state.sessions.end()) {
      return;
    }
    session = found->second.lock();
    if (!session) {
      return;
    }
  }

  try {
    auto frame = state.protocol.encode(opcode, message);
    if (local) {
      local_frames->push_back(std::move(frame));
    } else {
      deliveries.push_back({std::move(session), std::move(frame)});
    }
  } catch (const std::exception &error) {
    spdlog::warn("World notification opcode={} encoding failed: {}", opcode, error.what());
  }
}

using SharedBytes = std::shared_ptr<const Bytes>;

// Same delivery shape as queue_session_frame but pushes a pre-encoded shared
// payload; used by fan-out paths that encode once per message. Requires
// state.protocol_mutex (session map lookup) — same contract as the caller.
void queue_shared_frame(WorldState &state, std::uint64_t connection_id,
                        const SharedBytes &wire_bytes,
                        std::vector<SessionFrameDelivery> &deliveries) {
  const auto found = state.sessions.find(connection_id);
  if (found == state.sessions.end()) {
    return;
  }
  auto session = found->second.lock();
  if (!session) {
    return;
  }
  Frame frame;
  frame.shared = wire_bytes;
  deliveries.push_back({std::move(session), std::move(frame)});
}

// Only registry/codec/lifetime operations remain here. Party/Guild own the ordered
// expansion from their membership update into addressed packets and observer labels.
class MembershipReplicationRegistryContext final : public party::PartyReplicationContext,
                                                   public guild::GuildReplicationContext {
public:
  MembershipReplicationRegistryContext(WorldState &state, std::uint64_t local_connection_id,
                                       std::vector<Frame> *local_frames,
                                       std::vector<SessionFrameDelivery> &deliveries)
      : state_(state), local_connection_id_(local_connection_id), local_frames_(local_frames),
        deliveries_(deliveries) {
  }

  void queue_packet(std::uint64_t connection_id, std::uint16_t opcode,
                    const nlohmann::json &message) override {
    queue_session_frame(state_, connection_id, opcode, message, local_connection_id_, local_frames_,
                        deliveries_);
  }

  std::vector<std::uint64_t> observers(std::uint64_t subject_connection_id) override {
    return state_.presence.observers(subject_connection_id);
  }

  void queue_shared_observers(std::uint16_t opcode, const nlohmann::json &message,
                              const std::vector<std::uint64_t> &observers) override {
    try {
      auto frame = state_.protocol.encode(opcode, message);
      const SharedBytes wire = std::make_shared<const Bytes>(std::move(frame.bytes));
      for (const auto observer : observers) {
        queue_shared_frame(state_, observer, wire, deliveries_);
      }
    } catch (const std::exception &error) {
      spdlog::warn("World notification opcode={} encoding failed: {}", opcode, error.what());
    }
  }

private:
  WorldState &state_;
  const std::uint64_t local_connection_id_;
  std::vector<Frame> *local_frames_;
  std::vector<SessionFrameDelivery> &deliveries_;
};

void deliver_observer_packets(WorldState &state, std::uint64_t subject_connection_id,
                              const std::vector<GameplayPacket> &packets) {
  if (packets.empty()) {
    return;
  }

  std::vector<SessionFrameDelivery> deliveries;
  {
    std::lock_guard lock(state.protocol_mutex);
    // Encode once per packet; every observer receives the same immutable wire
    // bytes instead of a per-recipient re-encode under the codec mutex.
    std::vector<SharedBytes> encoded;
    encoded.reserve(packets.size());
    for (const auto &packet : packets) {
      try {
        auto frame = state.protocol.encode(packet.opcode, packet.message);
        encoded.push_back(std::make_shared<const Bytes>(std::move(frame.bytes)));
      } catch (const std::exception &error) {
        encoded.push_back(nullptr);
        spdlog::warn("World notification opcode={} encoding failed: {}", packet.opcode,
                     error.what());
      }
    }
    for (const auto observer : state.presence.observers(subject_connection_id)) {
      for (const auto &wire : encoded) {
        if (wire) {
          queue_shared_frame(state, observer, wire, deliveries);
        }
      }
    }
  }

  for (auto &delivery : deliveries) {
    delivery.recipient->post_frame(std::move(delivery.frame));
  }
}

void deliver_party_update(WorldState &state, const party::PartyUpdate &update,
                          std::uint64_t local_connection_id, std::vector<Frame> *local_frames) {
  std::vector<SessionFrameDelivery> deliveries;
  {
    std::lock_guard lock(state.protocol_mutex);
    MembershipReplicationRegistryContext context(state, local_connection_id, local_frames,
                                                 deliveries);
    state.party_feature->replicate_update(update, context);
  }

  for (auto &delivery : deliveries) {
    delivery.recipient->post_frame(std::move(delivery.frame));
  }
}

void deliver_guild_update(WorldState &state, const guild_world::GuildWorldUpdate &update,
                          std::uint64_t local_connection_id, std::vector<Frame> *local_frames) {
  if (!state.guild_feature->world()) {
    return;
  }

  std::vector<SessionFrameDelivery> deliveries;
  {
    std::lock_guard lock(state.protocol_mutex);
    MembershipReplicationRegistryContext context(state, local_connection_id, local_frames,
                                                 deliveries);
    state.guild_feature->replicate_update(update, context);
  }

  for (auto &delivery : deliveries) {
    delivery.recipient->post_frame(std::move(delivery.frame));
  }
}

void deliver_companion_update(WorldState &state, const companion::CompanionUpdate &update,
                              std::uint64_t local_connection_id, std::vector<Frame> *local_frames) {
  if (update.packets.empty()) {
    return;
  }

  try {
    std::vector<SessionFrameDelivery> deliveries;
    {
      std::lock_guard lock(state.protocol_mutex);
      for (const auto &packet : update.packets) {
        const auto found = state.sessions.find(packet.connection_id);
        const auto recipient = found == state.sessions.end() ? nullptr : found->second.lock();
        if (!recipient || recipient->closed()) {
          continue;
        }
        if (!packet.source) {
          throw std::logic_error("Companion fanout packet lacks a source binding");
        }
        auto frame = state.protocol.encode(packet.opcode, packet.message);
        recipient->bind_companion_frame(frame, *packet.source);
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
        if (packet.connection_id == 2 && companion_fanout_allocation_fault_armed.exchange(false)) {
          companion_fanout_allocation_fault_observed.store(true);
          throw std::bad_alloc();
        }
#endif
        if (local_frames && packet.connection_id == local_connection_id) {
          local_frames->push_back(std::move(frame));
        } else {
          deliveries.push_back({recipient, std::move(frame)});
        }
      }
    }

    for (auto &delivery : deliveries) {
      delivery.recipient->post_frame(std::move(delivery.frame));
    }
  } catch (...) {
#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
    {
      std::unique_lock lock(companion_fanout_pause_mutex);
      if (companion_fanout_pause_armed && companion_fanout_allocation_fault_observed.load()) {
        companion_fanout_pause_armed = false;
        companion_fanout_pause_entered = true;
        companion_fanout_pause_condition.notify_all();
        companion_fanout_pause_condition.wait_for(lock, std::chrono::seconds(30), [] {
          return companion_fanout_pause_released;
        });
      }
    }
#endif
    // The source already changed and some notifications may have been queued. Close
    // every affected connection instead of replaying an uncertain partial fanout.
    const auto close_recipient = [&](const companion::OutboundPacket &packet) noexcept {
      try {
        if (!packet.source) {
          spdlog::critical("Companion fanout lacks a captured recipient view connection={}",
                           packet.connection_id);
          return;
        }
        std::shared_ptr<WorldSession> recipient;
        {
          std::lock_guard lock(state.protocol_mutex);
          const auto found = state.sessions.find(packet.connection_id);
          if (found != state.sessions.end()) {
            recipient = found->second.lock();
          }
        }
        if (recipient) {
          const auto &view = packet.source->view;
          recipient->request_supervised_close_for_view(
              {view.epoch, view.map_id, view.copy_channel});
        }
      } catch (...) {
        try {
          spdlog::critical("Companion fanout could not fence connection={}",
                           packet.connection_id);
        } catch (...) {
        }
      }
    };
    for (const auto &packet : update.packets) {
      close_recipient(packet);
    }
    throw;
  }
}

void deliver_raid_update(WorldState &state, const raid::RaidUpdate &update,
                         std::uint64_t local_connection_id, std::vector<Frame> *local_frames) {
  std::vector<SessionFrameDelivery> deliveries;
  std::vector<std::pair<std::shared_ptr<WorldSession>, int>> evicted;
  {
    std::lock_guard lock(state.protocol_mutex);
    for (const auto &packet : update.packets) {
      queue_session_frame(state, packet.connection_id, packet.opcode, packet.message,
                          local_connection_id, local_frames, deliveries);
    }

    for (const auto &eviction : update.evictions) {
      const auto found = state.sessions.find(eviction.connection_id);
      if (found == state.sessions.end()) {
        continue;
      }
      if (auto session = found->second.lock()) {
        evicted.emplace_back(std::move(session), eviction.map_id);
      }
    }
  }

  for (auto &delivery : deliveries) {
    delivery.recipient->post_frame(std::move(delivery.frame));
  }
  for (auto &[session, map_id] : evicted) {
    session->post_raid_eviction(map_id);
  }
}

// The registry boundary keeps session ownership and codec locking in composition.
class LocalChatRegistryView final : public chat::LocalChatView {
public:
  LocalChatRegistryView(WorldState &state, std::vector<std::shared_ptr<WorldSession>> &retained)
      : state_(state), retained_(retained) {
  }

  std::optional<std::uint64_t> speaker_connection(std::int64_t character_id) override {
    for (const auto &[connection_id, weak_session] : state_.sessions) {
      auto session = weak_session.lock();
      if (!session) {
        continue;
      }
      const auto summary = session->admin_summary_locked();
      if (summary.authenticated && summary.character_id == character_id &&
          session->admin_in_world_locked()) {
        retained_.push_back(std::move(session));
        return connection_id;
      }
    }
    return std::nullopt;
  }

  std::vector<std::uint64_t> observers(std::uint64_t connection_id) const override {
    return state_.presence.observers(connection_id);
  }

  std::optional<std::int64_t> observer_character(std::uint64_t connection_id) override {
    const auto found = state_.sessions.find(connection_id);
    if (found == state_.sessions.end()) {
      return std::nullopt;
    }
    auto session = found->second.lock();
    if (!session || !session->admin_in_world_locked()) {
      return std::nullopt;
    }
    const auto summary = session->admin_summary_locked();
    retained_.push_back(std::move(session));
    return summary.authenticated ? std::optional<std::int64_t>(summary.character_id) : std::nullopt;
  }

private:
  WorldState &state_;
  std::vector<std::shared_ptr<WorldSession>> &retained_;
};

class WorldOfflineCharacterReservation final : public OfflineCharacterReservation {
public:
  WorldOfflineCharacterReservation(std::shared_ptr<WorldState> state, std::int64_t character_id,
                                   std::uint64_t token)
      : state_(std::move(state)), character_id_(character_id), token_(token) {
  }

  ~WorldOfflineCharacterReservation() override {
    if (write_pending_) {
      spdlog::error("Offline admin write for character {} has an unknown outcome; admission "
                    "remains fenced pending operator recovery", character_id_);
      return;
    }
    std::lock_guard lock(state_->character_admission_mutex);
    const auto entry = state_->offline_character_reservations.find(character_id_);
    if (entry != state_->offline_character_reservations.end() && entry->second == token_) {
      state_->offline_character_reservations.erase(entry);
    }
  }

  void begin_write() override {
    write_pending_ = true;
  }

  void finish_write() override {
    write_pending_ = false;
  }

private:
  std::shared_ptr<WorldState> state_;
  std::int64_t character_id_;
  std::uint64_t token_;
  bool write_pending_{};
};

class WorldListener final : public NativeWorldListener,
                            public housing::HousingExpiryOwner,
                            public std::enable_shared_from_this<WorldListener>,
                            private mail::MailWorldContext {
public:
  WorldListener(asio::io_context &io_context, asio::thread_pool &workers, AuthService &auth,
                const std::filesystem::path &protocol_directory,
                std::chrono::milliseconds handshake_delay, bool credential_diagnostic_enabled,
                bool bootstrap_diagnostic_enabled,
                std::shared_ptr<const TutorialData> tutorial_data,
                std::shared_ptr<const CharacterStats> character_stats,
                std::shared_ptr<ClientCatalog> client_catalog,
                std::shared_ptr<const GatheringData> gathering_data,
                std::shared_ptr<const LootData> loot_data,
                std::shared_ptr<const ProcessingData> processing_data,
                const std::filesystem::path &catalog_root,
                std::shared_ptr<const NpcPopulation> npc_population,
                std::shared_ptr<GuildService> guilds, std::shared_ptr<housing::HousingHub> housing,
                unsigned maximum_connections, WorldSocialServices social)
      : workers_(workers), io_context_(io_context),
        state_(std::make_shared<WorldState>(
            auth, protocol_directory, handshake_delay, credential_diagnostic_enabled,
            bootstrap_diagnostic_enabled, std::move(tutorial_data), std::move(character_stats),
            std::move(client_catalog), std::move(gathering_data), std::move(loot_data),
            std::move(processing_data), catalog_root, std::move(npc_population), io_context)),
        housing_feature_(std::make_shared<housing::HousingFeature>(io_context.get_executor(),
                                                                   workers.get_executor())) {
    // Load persistent fences before the listener can accept any world admission. A failed
    // read aborts startup rather than letting a possibly uncertain admin write race entry.
    const auto quarantines = auth.offline_admin_quarantines();
    state_->offline_admin_quarantines.insert(quarantines.begin(), quarantines.end());

    // Set before the delegated transport accepts connections.
    state_->maximum_connections = maximum_connections;
    quest_reconciliation_registry_ = std::make_shared<QuestReconciliationRegistry>();
    state_->quest_reconciliation_registry = quest_reconciliation_registry_;
    std::weak_ptr<WorldState> weak_state = state_;
    RetainedOwnerSupervisor::Options boss_options;
    boss_options.capacity = maximum_connections;
    boss_options.page_size = std::min<std::size_t>(128, maximum_connections);
    boss_wake_supervisor_ = std::make_shared<RetainedOwnerSupervisor>(
        boss_options,
        [weak_state](RetainedOwnerKey key, RetainedOwnerSupervisor::Completion completion) {
          const auto state = weak_state.lock();
          if (!state) {
            return RetainedOwnerSubmit::definitely_not_posted;
          }
          std::shared_ptr<WorldSession> owner;
          {
            std::lock_guard lock(state->character_admission_mutex);
            const auto registry = state->quest_reconciliation_registry.lock();
            if (registry) {
              const auto entry = registry->death_owners.find(key.character_id);
              if (entry != registry->death_owners.end() &&
                  entry->second->connection_id() == key.generation) {
                owner = entry->second;
              }
            }
          }
          if (!owner) {
            spdlog::critical("Boss retry owner is missing character={} connection={}; "
                             "recovery remains fenced", key.character_id, key.generation);
            return RetainedOwnerSubmit::definitely_not_posted;
          }
          return owner->submit_supervised_boss_wake(std::move(completion));
        },
        [](RetainedOwnerAlarm alarm, RetainedOwnerKey key) {
          spdlog::critical("Boss retry supervisor alarm={} character={} connection={}",
                           static_cast<int>(alarm), key.character_id, key.generation);
        });
    state_->boss_wake_supervisor = boss_wake_supervisor_;
    close_wake_supervisor_ = std::make_shared<RetainedOwnerSupervisor>(
        boss_options,
        [weak_state](RetainedOwnerKey key, RetainedOwnerSupervisor::Completion completion) {
          const auto state = weak_state.lock();
          if (!state) {
            return RetainedOwnerSubmit::definitely_not_posted;
          }
          std::shared_ptr<WorldSession> owner;
          {
            std::lock_guard lock(state->character_admission_mutex);
            const auto registry = state->quest_reconciliation_registry.lock();
            if (registry) {
              const auto entry = registry->death_owners.find(key.character_id);
              if (entry != registry->death_owners.end() &&
                  entry->second->connection_id() == key.generation) {
                owner = entry->second;
              }
            }
          }
          if (!owner) {
            spdlog::critical("Close retry owner is missing character={} connection={}; "
                             "admission remains fenced", key.character_id, key.generation);
            return RetainedOwnerSubmit::definitely_not_posted;
          }
          return owner->submit_supervised_close_wake(std::move(completion));
        },
        [](RetainedOwnerAlarm alarm, RetainedOwnerKey key) {
          spdlog::critical("Close retry supervisor alarm={} character={} connection={}",
                           static_cast<int>(alarm), key.character_id, key.generation);
        });
    state_->close_wake_supervisor = close_wake_supervisor_;
    state_->gathering_feature->set_respawn_delivery(
        [weak_state](const std::vector<GatheringStateChange> &changes, std::uint64_t channel) {
          if (const auto world = weak_state.lock()) {
            deliver_gathering_changes(*world, changes, channel, 0);
          }
        });
    if (state_->gathering_feature->enabled()) {
      const auto world_id = auth.world_status().world_id;
      gathering::GatheringSharedSourceStore source_store;
      source_store.list = [&auth, world_id](const std::optional<SharedGatheringSourceKey> &after,
                                            std::size_t limit) {
        return auth.shared_gathering_sources(world_id, after, limit);
      };
      source_store.transition = [&auth, world_id](
                                    const SharedGatheringSourceTransition &transition,
                                    std::int64_t ticks, bool require_due) {
        return auth.update_shared_gathering_source(world_id, transition, ticks, require_due);
      };
      source_store.fence = [&auth, world_id](const SharedGatheringSourceTransition &initial) {
        return auth.fence_shared_gathering_source(world_id, initial);
      };
      source_store.post_worker = [&workers](std::function<void()> task) {
        asio::post(workers, std::move(task));
      };
      state_->gathering_feature->set_shared_source_store(std::move(source_store));
      // Load and validate authoritative shared phases before opening world admission.
      state_->gathering_feature->hydrate_shared_sources();
    } else if (!auth.shared_gathering_sources(auth.world_status().world_id, std::nullopt, 1)
                    .empty()) {
      throw std::runtime_error(
          "Gathering profile is unavailable but persisted shared sources require recovery");
    }
    // Startup has settled shared boss scripts under the exclusive owner lease. Hydration
    // above installs every authoritative source before these acknowledgements unblock quests.
    // Private intents must already have been retired by the startup recovery owner.
    std::optional<std::string> after_boss_script;
    std::size_t acknowledged_boss_scripts = 0;
    for (;;) {
      const auto scripts = auth.pending_boss_scripts(auth.world_status().world_id,
                                                      after_boss_script,
                                                      kMonsterRewardRecoveryPageSize);
      if (scripts.size() > kMonsterRewardRecoveryPageSize) {
        throw std::runtime_error("Boss script recovery exceeded its page bound");
      }
      if (scripts.empty()) {
        break;
      }
      for (const auto &script : scripts) {
        if (script.copy_scope != "shared" ||
            (after_boss_script && script.event_id <= *after_boss_script) ||
            ++acknowledged_boss_scripts > kMaximumStoredMonsterRewardEvents) {
          throw std::runtime_error("Boss script recovery returned an invalid startup page");
        }
        if (!auth.mark_boss_script_published(script)) {
          throw std::runtime_error("Boss script was not settled before source hydration");
        }
        after_boss_script = script.event_id;
      }
      if (scripts.size() < kMonsterRewardRecoveryPageSize) {
        break;
      }
    }

    state_->party_feature->set_delivery([weak_state](const party::PartyUpdate &update) {
      if (const auto world = weak_state.lock()) {
        deliver_party_update(*world, update);
      }
    });
    state_->events_feature->set_raid_delivery([weak_state](const raid::RaidUpdate &update) {
      if (const auto world = weak_state.lock()) {
        deliver_raid_update(*world, update);
      }
    });

    if (!social.guilds) {
      social.guilds = std::move(guilds);
    }
    attach_social(std::move(social));
    housing_feature_->set_hub(std::move(housing));
    state_->housing_feature = housing_feature_;
  }

  ~WorldListener() override {
    housing_feature_->on_listener_destroyed();
  }

  unsigned short port() const override {
    return transport_->port();
  }

  void stop() override {
    transport_->stop();
    housing_feature_->stop();
  }

  void shutdown_when_drained(std::function<void()> completion) override {
    if (!completion) {
      throw std::invalid_argument("Drained world shutdown requires a completion");
    }
    bool expected = false;
    if (!shutdown_started_.compare_exchange_strong(expected, true)) {
      return;
    }
    state_->shutting_down.store(true);
    try {
      auto io_guard = std::make_shared<asio::executor_work_guard<asio::io_context::executor_type>>(
          asio::make_work_guard(io_context_));
      auto completion_in_flight = std::make_shared<std::atomic<bool>>(false);
      stop();
#ifdef CHRONICLES_TEST_SHUTDOWN_START_FAULT
      if (shutdown_start_fault_for_testing.exchange(false)) {
        throw std::runtime_error("Injected failure after listener stop");
      }
#endif
      shutdown_thread_ = std::jthread(
          [state = state_, supervisor = boss_wake_supervisor_,
           close_supervisor = close_wake_supervisor_,
           executor = io_context_.get_executor(), completion = std::move(completion),
           completed = shutdown_completed_,
           queued = shutdown_completion_queued_, io_guard, completion_in_flight](
              std::stop_token stop_token) mutable {
            std::uint64_t cursor = 0;
            auto last_report = Clock::now();
            while (!stop_token.stop_requested() && !completed->load()) {
              try {
                std::vector<std::shared_ptr<WorldSession>> page;
                bool sessions_empty = false;
                {
                  std::lock_guard lock(state->protocol_mutex);
                  const auto selected = select_boss_recipient_page(
                      state->sessions, cursor,
                      (std::numeric_limits<std::uint64_t>::max)(), 128);
                  page.reserve(selected.connection_ids.size());
                  for (const auto connection : selected.connection_ids) {
                    const auto found = state->sessions.find(connection);
                    if (found != state->sessions.end()) {
                      if (auto session = found->second.lock()) {
                        page.push_back(std::move(session));
                      } else {
                        state->sessions.erase(found);
                      }
                    }
                  }
                  cursor = selected.last ? 0 : selected.end;
                  sessions_empty = state->sessions.empty();
                }
                for (const auto &session : page) {
                  if (!session->closed()) {
                    session->request_shutdown_close();
                  }
                }
                bool owners_empty = false;
                {
                  std::lock_guard lock(state->character_admission_mutex);
                  const auto registry = state->quest_reconciliation_registry.lock();
                  owners_empty = registry && registry->owners.empty() &&
                                 registry->death_owners.empty() &&
                                 state->character_admissions.empty() &&
                                 state->pending_gathering_recoveries.empty() &&
                                 state->pending_world_settlements.empty() &&
                                 state->offline_character_reservations.empty();
                }
                const auto health = supervisor->health();
                const auto close_health = close_supervisor->health();
                if (sessions_empty && owners_empty &&
                    state->active_connections.load() == 0 &&
                    health.retained == 0 && close_health.retained == 0 &&
                    supervisor->stop() && close_supervisor->stop() &&
                    !completion_in_flight->exchange(true)) {
                  const auto result = dispatch_boss_wake(
                      [&](auto handler) {
                        asio::post(executor, std::move(handler));
                        queued->store(true);
                      },
                      [completed, completion, io_guard] {
                        if (!completed->exchange(true)) {
                          try {
                            completion();
                          } catch (...) {
                            io_guard->reset();
                            throw;
                          }
                          io_guard->reset();
                        }
                      },
                      [] { spdlog::critical("World shutdown completion failed"); },
                      [completion_in_flight] { completion_in_flight->store(false); });
                  if (result == RetainedOwnerSubmit::definitely_not_posted) {
                    completion_in_flight->store(false);
                  }
                }
                if (Clock::now() - last_report >= std::chrono::seconds(30)) {
                  spdlog::warn("World shutdown is draining retained owners: boss={} "
                               "close={} in_flight={} uncertain={} overdue={}",
                               health.retained, close_health.retained, health.in_flight,
                               health.uncertain,
                               health.overdue);
                  last_report = Clock::now();
                }
              } catch (const std::exception &error) {
                spdlog::error("World shutdown drain failed; retrying: {}", error.what());
              } catch (...) {
                spdlog::error("World shutdown drain failed; retrying");
              }
              std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
          });
    } catch (...) {
      shutdown_started_.store(false);
      throw;
    }
  }

  bool shutdown_completion_queued() const noexcept override {
    return shutdown_completion_queued_->load();
  }

  bool shutdown_started() const noexcept override {
    return shutdown_started_.load();
  }

  bool shutdown_finished() const noexcept override {
    return shutdown_completed_->load();
  }

  std::size_t shutdown_close_post_attempts() const noexcept override {
    return state_->shutdown_close_post_attempts.load();
  }

  void resynchronize_quest_company(std::int64_t character_id) override {
    std::shared_ptr<WorldSession> target;
    // Keep every successfully locked session alive until after protocol_mutex is released.
    // Destruction can itself need that mutex during connection teardown.
    std::vector<std::shared_ptr<WorldSession>> retained_sessions;
    {
      std::lock_guard lock(state_->protocol_mutex);
      retained_sessions.reserve(state_->sessions.size());
      for (const auto &[connection_id, weak_session] : state_->sessions) {
        auto session = weak_session.lock();
        if (!session) {
          continue;
        }
        const auto summary = session->admin_summary_locked();
        if (summary.authenticated && summary.character_id == character_id &&
            session->admin_in_world_locked()) {
          target = session;
          retained_sessions.push_back(std::move(session));
          break;
        }
        retained_sessions.push_back(std::move(session));
      }
    }
    if (target) {
      // asio::post throws on scheduling failure; callers can then retry the refresh.
      target->resynchronize_quest_company(character_id);
    }
  }

  std::unique_ptr<OfflineCharacterReservation>
  reserve_offline_character(std::int64_t character_id) override {
    std::lock_guard lock(state_->character_admission_mutex);
    if (state_->character_admissions.contains(character_id) ||
        state_->pending_gathering_recoveries.contains(character_id) ||
        state_->pending_world_settlements.contains(character_id) ||
        state_->offline_character_reservations.contains(character_id) ||
        state_->offline_admin_quarantines.contains(character_id) ||
        state_->next_offline_reservation == 0) {
      return {};
    }

    const auto token = state_->next_offline_reservation++;
    state_->offline_character_reservations.emplace(character_id, token);
    // Allocation failure leaves the fence in place instead of permitting a competing entry.
    return std::make_unique<WorldOfflineCharacterReservation>(state_, character_id, token);
  }

  std::vector<WorldConnectionSummary> connections() override {
    std::vector<WorldConnectionSummary> summaries;
    // Hold the strong references until after unlocking, so a final release never runs a
    // session destructor under the protocol mutex.
    std::vector<std::shared_ptr<WorldSession>> live_sessions;
    {
      std::lock_guard lock(state_->protocol_mutex);
      for (const auto &[connection_id, weak_session] : state_->sessions) {
        auto session = weak_session.lock();
        if (!session) {
          continue;
        }
        summaries.push_back(session->admin_summary_locked());
        live_sessions.push_back(std::move(session));
      }
    }
    return summaries;
  }

  bool disconnect(std::uint64_t connection_id) override {
    std::shared_ptr<WorldSession> session;
    {
      std::lock_guard lock(state_->protocol_mutex);
      const auto found = state_->sessions.find(connection_id);
      if (found != state_->sessions.end()) {
        session = found->second.lock();
      }
    }
    if (!session) {
      return false;
    }

    session->admin_disconnect();
    return true;
  }

  std::size_t disconnect_account(std::int64_t account_id) override {
    std::vector<std::shared_ptr<WorldSession>> matching;
    {
      std::lock_guard lock(state_->protocol_mutex);
      for (const auto &[connection_id, weak_session] : state_->sessions) {
        auto session = weak_session.lock();
        if (session && session->admin_summary_locked().account_id == account_id) {
          matching.push_back(std::move(session));
        }
      }
    }

    for (const auto &session : matching) {
      session->admin_disconnect();
    }
    return matching.size();
  }

  void set_world_entry_handler(std::function<void(const ChatIdentity &)> handler) override {
    std::lock_guard lock(state_->protocol_mutex);
    state_->chat_feature.set_entry_handler_locked(std::move(handler));
  }

  void set_world_exit_handler(std::function<void(const ChatIdentity &)> handler) override {
    std::lock_guard lock(state_->protocol_mutex);
    state_->chat_feature.set_exit_handler_locked(std::move(handler));
  }

  void set_player_command_handler(
      std::function<bool(const ChatIdentity &, const std::string &)> handler) override {
    std::lock_guard lock(state_->protocol_mutex);
    state_->chat_feature.set_command_handler_locked(std::move(handler));
  }

  void set_gm_reply_handler(
      std::function<void(const ChatIdentity &, const std::string &)> handler) override {
    state_->gm_feature.set_reply_handler(std::move(handler));
  }

  void set_gm_announce_handler(
      std::function<std::size_t(const ChatIdentity &, const std::string &)> handler) override {
    state_->gm_feature.set_announce_handler(std::move(handler));
  }

  void set_repositories(WorldRepositories repositories) override {
    std::lock_guard lock(state_->protocol_mutex);
    state_->events_feature->set_ranking_repository(repositories.ranking);
    state_->market_feature.set_repository(repositories.market);
    state_->mail_feature.set_repository(repositories.mail);
    state_->fog_repository = std::move(repositories.fog);
  }

  void enable_graduation_grant() override {
    std::lock_guard lock(state_->protocol_mutex);
    state_->character_feature.enable_graduation();
  }

  void set_entry_admission_wait(std::chrono::milliseconds wait) override {
    const auto retries = std::max<std::chrono::milliseconds::rep>(
        0, wait.count() / kAdmissionRetryInterval.count());
    state_->maximum_admission_retries.store(
        static_cast<int>(std::min<std::chrono::milliseconds::rep>(
            retries, std::numeric_limits<int>::max())));
  }

  bool claim_mail(const Character &character, std::int64_t mail_id,
                  std::function<void(MailClaimStatus)> completion) override {
    return state_->mail_feature.claim_online(character, mail_id, std::move(completion), *this);
  }

  std::size_t notify_mail(const MailRecord &mail) override {
    return state_->mail_feature.notify_online(mail, *this);
  }

  bool player_pvp(const ChatIdentity &identity, std::optional<bool> enabled,
                  std::function<void(std::string)> completion) override {
    auto selected = in_world_session(identity);
    if (!selected || !completion) {
      return false;
    }

    selected->player_pvp_command(enabled, std::move(completion));
    return true;
  }

  bool player_mount(const ChatIdentity &identity, PlayerMountAction action,
                    std::function<void(std::string)> completion) override {
    auto selected = in_world_session(identity);
    if (!selected || !completion) {
      return false;
    }

    selected->player_mount_command(action, std::move(completion));
    return true;
  }

  bool player_unstuck(const ChatIdentity &identity,
                      std::function<void(std::string)> completion) override {
    auto selected = in_world_session(identity);
    if (!selected || !completion) {
      return false;
    }
    selected->player_unstuck_command(std::move(completion));
    return true;
  }

  // The in-world session of `identity`'s character on this realm, if it has one.
  std::shared_ptr<WorldSession> in_world_session(const ChatIdentity &identity) {
    if (identity.world_id != state_->auth.world_status().world_id) {
      return {};
    }

    std::lock_guard lock(state_->protocol_mutex);
    for (const auto &[connection_id, weak_session] : state_->sessions) {
      auto session = weak_session.lock();
      if (!session) {
        continue;
      }
      const auto summary = session->admin_summary_locked();
      if (summary.authenticated && summary.character_id == identity.character_id &&
          session->admin_in_world_locked()) {
        return session;
      }
    }
    return {};
  }

  std::vector<std::int64_t> local_chat_audience(const ChatIdentity &speaker) override {
    if (speaker.world_id != state_->auth.world_status().world_id) {
      return {};
    }

    std::vector<std::int64_t> audience;
    std::vector<std::shared_ptr<WorldSession>> live_sessions;
    {
      std::lock_guard lock(state_->protocol_mutex);
      LocalChatRegistryView view(*state_, live_sessions);
      audience = state_->chat_feature.local_audience_locked(speaker.character_id, view);
    }
    return audience;
  }

  std::size_t broadcast_system_notice(const std::string &text, SystemNoticeStyle style) override {
    const auto packet = chat::system_notice_packet(text, style);

    std::vector<std::shared_ptr<WorldSession>> recipients;
    Frame frame;
    {
      std::lock_guard lock(state_->protocol_mutex);
      frame = state_->protocol.encode(packet.opcode, packet.message);
      for (const auto &[connection_id, weak_session] : state_->sessions) {
        auto session = weak_session.lock();
        if (session && session->admin_in_world_locked()) {
          recipients.push_back(std::move(session));
        }
      }
    }

    for (const auto &recipient : recipients) {
      recipient->post_frame(frame);
    }
    return recipients.size();
  }

  void run(asio::io_context &io_context, const tcp::endpoint &endpoint,
           const WorldTlsOptions &options) {
    transport_ = start_world_tls_listener(
        io_context, endpoint, options,
        [self = shared_from_this()](tcp::socket socket, std::shared_ptr<ssl::context> tls_context) {
          if (self->state_->shutting_down.load() ||
              self->state_->active_connections >= self->state_->maximum_connections) {
            boost::system::error_code ignored;
            socket.close(ignored);
            spdlog::warn("Native world connection limit reached");
            return;
          }

          auto session = std::make_shared<WorldSession>(std::move(socket), std::move(tls_context),
                                                        self->state_, self->workers_);
          session->run();
        });
  }

  // Renews or packs expired housing contracts about once a minute. The sweep runs on the worker
  // pool because it may wait for owners' session strands.
  bool housing_listener_open() const override {
    return transport_->is_open();
  }

  void schedule_housing_expiry() {
    housing_feature_->schedule_expiry(weak_from_this());
  }

private:
  int mail_world_id() const override {
    return state_->auth.world_status().world_id;
  }

  world_metrics::TimedMutex &mail_protocol_mutex() const override {
    return state_->protocol_mutex;
  }

  Frame mail_encode(const GameplayPacket &packet) override {
    return state_->protocol.encode(packet.opcode, packet.message);
  }

  void mail_each_peer(const std::function<bool(mail::MailPeer)> &visitor) override {
    for (const auto &[connection_id, weak_session] : state_->sessions) {
      auto session = weak_session.lock();
      if (!session) {
        continue;
      }
      const auto summary = session->admin_summary_locked();
      mail::MailPeer peer{summary.authenticated,
                          summary.account_id,
                          summary.character_id,
                          [session] {
                            return session->admin_in_world_locked();
                          },
                          [session](Frame frame) {
                            session->post_frame(std::move(frame));
                          },
                          [session](std::int64_t mail_id, mail::ClaimCompletion completion) {
                            session->claim_mail(mail_id, std::move(completion));
                          }};
      if (!visitor(std::move(peer))) {
        break;
      }
    }
  }

  // Connect the shared guild and company services before the first connection is accepted.
  // HousingHub owns guild housing events and expiry; do not attach its legacy coordinator.
  void attach_social(WorldSocialServices social) {
    state_->company_feature = std::make_unique<company::CompanyFeature>(
        std::move(social.companies), std::move(social.company_dialogues));
    std::weak_ptr<WorldState> weak_state = state_;
    state_->guild_feature = std::make_shared<guild::GuildFeature>(
        std::move(social.guilds), [weak_state](const guild_world::GuildWorldUpdate &update) {
          if (const auto state = weak_state.lock()) {
            deliver_guild_update(*state, update);
          }
        });
    state_->guild_feature->subscribe();
  }

  asio::thread_pool &workers_;
  asio::io_context &io_context_;
  std::shared_ptr<WorldState> state_;
  std::shared_ptr<QuestReconciliationRegistry> quest_reconciliation_registry_;
  std::shared_ptr<RetainedOwnerSupervisor> boss_wake_supervisor_;
  std::shared_ptr<RetainedOwnerSupervisor> close_wake_supervisor_;
  std::shared_ptr<WorldTlsListener> transport_;
  std::shared_ptr<housing::HousingFeature> housing_feature_;
  std::atomic<bool> shutdown_started_{};
  std::shared_ptr<std::atomic<bool>> shutdown_completed_{
      std::make_shared<std::atomic<bool>>(false)};
  std::shared_ptr<std::atomic<bool>> shutdown_completion_queued_{
      std::make_shared<std::atomic<bool>>(false)};
  std::jthread shutdown_thread_;
};

} // namespace

#ifdef CHRONICLES_TEST_SHUTDOWN_START_FAULT
void arm_shutdown_start_fault_for_testing() noexcept {
  shutdown_start_fault_for_testing.store(true);
}
#endif

#ifdef CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
void arm_companion_remove_fault_for_testing() noexcept {
  companion_dismount_inserted.store(false);
  companion_remove_fault_observed.store(false);
  companion_remove_fault_armed.store(true);
}

bool companion_remove_fault_observed_for_testing() noexcept {
  return companion_remove_fault_observed.load();
}

void observe_companion_entry_binding_for_testing(std::uint64_t unit_id) noexcept {
  companion_entry_binding_observed.store(false);
  companion_entry_unit_for_testing.store(unit_id);
}

bool companion_entry_binding_observed_for_testing() noexcept {
  return companion_entry_binding_observed.load();
}

void arm_companion_first_frame_fault_for_testing() noexcept {
  companion_first_frame_fault_observed.store(false);
  companion_first_frame_fault_armed.store(true);
}

bool companion_first_frame_fault_observed_for_testing() noexcept {
  return companion_first_frame_fault_observed.load();
}

void arm_companion_entry_close_fault_for_testing() noexcept {
  companion_close_post_fault_observed.store(false);
  companion_close_post_fault_armed.store(true);
  companion_entry_fault_armed.store(true);
}

bool companion_close_post_fault_observed_for_testing() noexcept {
  return companion_close_post_fault_observed.load();
}

void arm_companion_terminal_pause_for_testing(std::uint64_t recipient) {
  std::lock_guard lock(companion_terminal_pause_mutex);
  companion_terminal_pause_recipient.store(recipient);
  companion_terminal_pause_entered = false;
  companion_terminal_pause_released = false;
  companion_terminal_stale_observed.store(false);
  companion_terminal_pause_armed = true;
}

bool wait_companion_terminal_pause_for_testing() {
  std::unique_lock lock(companion_terminal_pause_mutex);
  return companion_terminal_pause_condition.wait_for(lock, std::chrono::seconds(10), [] {
    return companion_terminal_pause_entered;
  });
}

void release_companion_terminal_pause_for_testing() {
  std::lock_guard lock(companion_terminal_pause_mutex);
  companion_terminal_pause_released = true;
  companion_terminal_pause_condition.notify_all();
}

bool companion_terminal_stale_observed_for_testing() noexcept {
  return companion_terminal_stale_observed.load();
}

void set_companion_detach_budget_for_testing(
    std::shared_ptr<companion::CompanionDetachBudget> budget) {
  std::lock_guard lock(companion_test_budget_mutex);
  companion_budget_exhaustion_observed.store(false);
  companion_test_budget = std::move(budget);
}

bool companion_budget_exhaustion_observed_for_testing() noexcept {
  return companion_budget_exhaustion_observed.load();
}

void arm_quest_supervisor_faults_for_testing() noexcept {
  quest_supervisor_post_fault_observed.store(false);
  quest_retirement_fault_observed.store(false);
  quest_supervisor_post_fault_armed.store(true);
  quest_retirement_fault_armed.store(true);
}

bool quest_supervisor_post_fault_observed_for_testing() noexcept {
  return quest_supervisor_post_fault_observed.load();
}

bool quest_retirement_fault_observed_for_testing() noexcept {
  return quest_retirement_fault_observed.load();
}

void arm_quest_success_forget_fault_for_testing() noexcept {
  quest_success_forget_fault_observed.store(false);
  quest_success_forget_fault_armed.store(true);
}

void arm_recovered_reward_encoding_fault_for_testing() noexcept {
  recovered_reward_encoding_fault_observed.store(false);
  recovered_reward_encoding_fault_armed.store(true);
}

bool recovered_reward_encoding_fault_observed_for_testing() noexcept {
  return recovered_reward_encoding_fault_observed.load();
}

void arm_gameplay_completion_fault_for_testing(bool unknown) noexcept {
  gameplay_completion_fault_observed.store(false);
  gameplay_completion_fault_unknown.store(unknown);
  gameplay_completion_fault_armed.store(true);
}

bool gameplay_completion_fault_observed_for_testing() noexcept {
  return gameplay_completion_fault_observed.load();
}

void arm_frame_worker_post_fault_for_testing(bool unknown) noexcept {
  frame_worker_post_fault_observed.store(false);
  worker_post_fault_unknown.store(unknown);
  frame_worker_post_fault_armed.store(true);
}

bool frame_worker_post_fault_observed_for_testing() noexcept {
  return frame_worker_post_fault_observed.load();
}

void arm_tick_worker_post_fault_for_testing(bool unknown) noexcept {
  tick_worker_post_fault_observed.store(false);
  tick_worker_post_timer_observed.store(false);
  worker_post_fault_unknown.store(unknown);
  tick_worker_post_fault_armed.store(true);
}

bool tick_worker_post_fault_observed_for_testing() noexcept {
  return tick_worker_post_fault_observed.load();
}

bool tick_worker_post_timer_observed_for_testing() noexcept {
  return tick_worker_post_timer_observed.load();
}

void arm_map_transition_log_fault_for_testing() noexcept {
  map_transition_log_fault_observed.store(false);
  map_transition_log_fault_armed.store(true);
}

bool map_transition_log_fault_observed_for_testing() noexcept {
  return map_transition_log_fault_observed.load();
}

void arm_map_transition_io_fault_for_testing() noexcept {
  map_transition_io_fault_observed.store(false);
  map_transition_io_fault_armed.store(true);
}

bool map_transition_io_fault_observed_for_testing() noexcept {
  return map_transition_io_fault_observed.load();
}

void arm_response_io_fault_for_testing(bool timer) noexcept {
  if (timer) {
    tick_response_io_fault_observed.store(false);
    tick_worker_post_timer_observed.store(false);
    tick_response_io_fault_armed.store(true);
  } else {
    frame_response_io_fault_observed.store(false);
    frame_response_io_fault_armed.store(true);
  }
}

bool response_io_fault_observed_for_testing(bool timer) noexcept {
  return timer ? tick_response_io_fault_observed.load() : frame_response_io_fault_observed.load();
}

void arm_close_hook_retry_fault_for_testing() noexcept {
  close_hook_party_stage_calls.store(0);
  close_hook_retry_fault_connection.store(0);
  close_hook_retry_fault_observed.store(false);
  close_hook_retry_fault_blocked.store(true);
  close_hook_retry_fault_armed.store(true);
}

void release_close_hook_retry_fault_for_testing() noexcept {
  close_hook_retry_fault_blocked.store(false);
}

bool close_hook_retry_fault_observed_for_testing() noexcept {
  return close_hook_retry_fault_observed.load();
}

unsigned close_hook_party_stage_calls_for_testing() noexcept {
  return close_hook_party_stage_calls.load();
}

bool quest_success_forget_fault_observed_for_testing() noexcept {
  return quest_success_forget_fault_observed.load();
}

void arm_companion_fanout_allocation_fault_for_testing() noexcept {
  companion_fanout_allocation_fault_observed.store(false);
  companion_fanout_old_view_skipped.store(false);
  companion_fanout_allocation_fault_armed.store(true);
}

bool companion_fanout_allocation_fault_observed_for_testing() noexcept {
  return companion_fanout_allocation_fault_observed.load();
}

void arm_companion_fanout_pause_for_testing() {
  std::lock_guard lock(companion_fanout_pause_mutex);
  companion_fanout_pause_entered = false;
  companion_fanout_pause_released = false;
  companion_fanout_pause_armed = true;
}

bool wait_companion_fanout_pause_for_testing() {
  std::unique_lock lock(companion_fanout_pause_mutex);
  return companion_fanout_pause_condition.wait_for(lock, std::chrono::seconds(10), [] {
    return companion_fanout_pause_entered;
  });
}

void release_companion_fanout_pause_for_testing() {
  std::lock_guard lock(companion_fanout_pause_mutex);
  companion_fanout_pause_released = true;
  companion_fanout_pause_condition.notify_all();
}

bool companion_fanout_old_view_skipped_for_testing() noexcept {
  return companion_fanout_old_view_skipped.load();
}

void arm_companion_transition_fault_for_testing() noexcept {
  companion_transition_fault_observed.store(false);
  companion_transition_fault_armed.store(true);
}

bool companion_transition_fault_observed_for_testing() noexcept {
  return companion_transition_fault_observed.load();
}

void arm_general_transition_fault_for_testing() noexcept {
  general_transition_fault_observed.store(false);
  general_transition_staged_observed.store(false);
  general_transition_fault_armed.store(true);
}

bool general_transition_fault_observed_for_testing() noexcept {
  return general_transition_fault_observed.load();
}

bool general_transition_staged_observed_for_testing() noexcept {
  return general_transition_staged_observed.load();
}
#endif

WorldHandshake::WorldHandshake(Protocol &protocol, bool bootstrap_diagnostic_enabled,
                               std::shared_ptr<const CharacterStats> character_stats)
    : protocol_(protocol), bootstrap_diagnostic_enabled_(bootstrap_diagnostic_enabled),
      character_bootstrap_(std::move(character_stats)) {
}

bool WorldHandshake::configure_bootstrap_character(
    Character character, std::optional<WorldProgress> non_tutorial_progress,
    std::optional<int> overworld_map, std::optional<int> instance_map) {
  // Overworld entry needs a persisted position on exactly the configured map; a missing position
  // keeps the tutorial entry, and no spawn point is invented for the overworld.
  const bool overworld = overworld_map && *overworld_map != kDiagnosticTutorialMapId &&
                         character.world_position &&
                         character.world_position->map_id == *overworld_map;
  // A followup tutorial instance (map 10117) resumes from its persisted position.
  const bool instance =
      instance_map && character.world_position && character.world_position->map_id == *instance_map;
  if (!bootstrap_diagnostic_enabled_ ||
      !tutorial::supports_diagnostic_tutorial_start(character.class_id) ||
      !is_supported_character_id(character.id) ||
      (character.world_position && character.world_position->map_id != kDiagnosticTutorialMapId &&
       !overworld && !instance)) {
    return false;
  }
  if (overworld) {
    map_id_ = *overworld_map;
  } else if (instance) {
    map_id_ = *instance_map;
  } else {
    map_id_ = kDiagnosticTutorialMapId;
  }

  character_bootstrap_.prepare_stats(character);
  diagnostic_unit_id_ = static_cast<std::uint32_t>(player_wire_unit_id(character.id));
  character_bootstrap_.set_entry_dead(non_tutorial_progress && non_tutorial_progress->dead);
  set_learned_crafts(non_tutorial_progress ? non_tutorial_progress->learned_crafts
                                           : std::set<int>{});
  if (non_tutorial_progress) {
    inventory_bootstrap_.configure(non_tutorial_progress);
    housing_bootstrap_.set_items(std::move(non_tutorial_progress->housing_items));
    trade_bootstrap_.configure(non_tutorial_progress);
  } else {
    inventory_bootstrap_.configure(non_tutorial_progress);
    housing_bootstrap_.clear();
    trade_bootstrap_.configure(non_tutorial_progress);
  }
  character_bootstrap_.select_character(std::move(character));
  return true;
}

Frame WorldHandshake::entry_response() {
  return protocol_.encode(kCharacterEntryResponse, nlohmann::json::object());
}

void WorldHandshake::set_achievement_snapshot(nlohmann::json snapshot) {
  achievement_snapshot_ = std::move(snapshot);
}

void WorldHandshake::set_learned_crafts(const std::set<int> &learned_crafts) {
  learned_crafts_ = learned_crafts;
}

void WorldHandshake::refresh_items_and_stats(const Character &character,
                                             const nlohmann::json *prepared_stats) {
  character_bootstrap_.refresh_items_and_stats(character, prepared_stats);
}

void WorldHandshake::refresh_progression(int level, std::int64_t experience,
                                         const CharacterBuild &build) {
  character_bootstrap_.refresh_progression(level, experience, build);
}

void WorldHandshake::set_perk_snapshot(nlohmann::json snapshot) {
  character_bootstrap_.set_perk_snapshot(std::move(snapshot));
}

void WorldHandshake::set_game_master(bool game_master) {
  character_bootstrap_.set_game_master(game_master);
}

void WorldHandshake::set_authoritative_player_state(const nlohmann::json &stats, bool dead) {
  character_bootstrap_.set_authoritative_player_state(stats, dead);
}

void WorldHandshake::set_authoritative_position(const WorldPosition &position) {
  if (!character_bootstrap_.selected_character() || position.map_id != map_id_) {
    throw std::logic_error("Unsupported authoritative player relocation");
  }

  character_bootstrap_.relocate(position);
  last_position_ = position;
}

Frame WorldHandshake::begin_map_move(const WorldPosition &spawn, int cut_scene_id,
                                     bool restore_gliding) {
  if (!character_bootstrap_.selected_character() || !map_move_completed_ || exited_) {
    throw std::logic_error("A map move requires a character on a loaded map");
  }

  // The client answers MapMoveStartNoti with the same MapLoadComplete/StartPlaying
  // requests as the first entry, so the handshake re-arms both stages for the new map.
  map_id_ = spawn.map_id;
  character_bootstrap_.relocate(spawn);
  last_position_ = spawn;
  last_movement_.reset();
  map_move_completed_ = false;
  start_playing_completed_ = false;
  restore_gliding_on_map_completion_ = restore_gliding;

  const nlohmann::json spawn_point{
      {"pos",
       {{"millimeter_x", spawn.x_mm}, {"millimeter_y", spawn.y_mm}, {"millimeter_z", spawn.z_mm}}},
      {"yaw", spawn.yaw}};
  nlohmann::json message{{"map_id", spawn.map_id},
                         {"spawn_point", spawn_point},
                         {"started_timestamp", std::to_string(unix_epoch_ticks_now())}};
  // MapMoveCutScene: the client plays this cutscene around the map load.
  if (cut_scene_id > 0) {
    message["cut_scene_id"] = cut_scene_id;
  }
  return protocol_.encode(kMapMoveStartNotification, message);
}

std::optional<Frame> WorldHandshake::receive(const Frame &frame, std::int64_t monotonic_ticks,
                                             std::int64_t epoch_ticks,
                                             WorldPacketMetadata *metadata) {
  if (exited_) {
    throw std::runtime_error("Character has already left the connection");
  }

  const auto decoded = protocol_.decode(frame);
  if (metadata) {
    metadata->family = decoded.value("family", "none");
    metadata->status = decoded.value("status", "none");
    metadata->sequence = decoded.value("sequence", std::uint32_t{});
    metadata->protobuf_bytes = decoded.value("protobuf_bytes", std::size_t{});
  }
  if (decoded.value("status", "") != "decoded" || decoded.value("family", "") != "PC2S") {
    throw std::runtime_error("Expected a known client protobuf packet");
  }
  if (decoded.at("sequence").get<std::uint32_t>() != next_sequence_) {
    throw std::runtime_error("Unexpected client packet sequence");
  }

  gameplay_request_ = WorldGameplayRequest{frame.opcode(), next_sequence_, decoded.at("message")};
  ++next_sequence_;
  const auto &request = decoded.at("message");
  switch (frame.opcode()) {
  case world_request::kNetworkDelayRequest:
    world_metrics::record_client_round_trip(
        request.value("client_round_trip_nanoseconds", std::string("0")));
    return std::nullopt;
  case kTimeSyncRequest: {
    nlohmann::json response{{"ticks", wire_monotonic_tick(monotonic_ticks)},
                            {"timestamp", std::to_string(epoch_ticks)}};
    copy_present(request, response, "try_count", "try_count");
    copy_present(request, response, "try_ticks", "try_ticks");
    copy_present(request, response, "try_cycles64", "try_cycles64");
    copy_present(request, response, "last_round_trip_nanoseconds", "last_round_trip_nanoseconds");
    auto time_response = protocol_.encode(kTimeSyncResponse, response);
    if (character_bootstrap_.selected_character() && !map_move_started_) {
      const nlohmann::json spawn_point{
          {"pos", diagnostic_spawn_position(*character_bootstrap_.selected_character())},
          {"yaw", character_bootstrap_.selected_character()->world_position
                      ? character_bootstrap_.selected_character()->world_position->yaw
                      : kDiagnosticSpawnYaw}};
      const nlohmann::json map_start{{"map_id", map_id_}, {"spawn_point", spawn_point}};
      auto map_move_start = protocol_.encode(kMapMoveStartNotification, map_start);
      pending_notifications_.push_back(std::move(time_response));
      if (const auto base = character_bootstrap_.base_snapshot()) {
        pending_notifications_.push_back(protocol_.encode(base->opcode, base->message));
      }
      const auto inventory =
          inventory_bootstrap_.items_snapshot(*character_bootstrap_.selected_character());
      // Official entries place the expertise snapshot before the inventory and the learned
      // recipe snapshot after it. Without expertise levels the client refuses to open any
      // crafting station window.
      {
        const auto expertise = processing::ProcessingFeature::expertise_entry_snapshot();
        pending_notifications_.push_back(protocol_.encode(expertise.opcode, expertise.message));
      }
      pending_notifications_.push_back(protocol_.encode(inventory.opcode, inventory.message));
      {
        const auto crafts = processing::ProcessingFeature::crafts_entry_snapshot(learned_crafts_);
        pending_notifications_.push_back(protocol_.encode(crafts.opcode, crafts.message));
      }
      if (const auto perks = character_bootstrap_.perk_snapshot()) {
        pending_notifications_.push_back(protocol_.encode(perks->opcode, perks->message));
      }
      if (const auto money = inventory_bootstrap_.money_snapshot()) {
        pending_notifications_.push_back(protocol_.encode(money->opcode, money->message));
      }
      append_shop_adjacent_snapshots();
      if (achievement_snapshot_) {
        pending_notifications_.push_back(
            protocol_.encode(character::kAchievementSnapshotNotification, *achievement_snapshot_));
      }
      map_move_started_ = true;
      return map_move_start;
    }

    return time_response;
  }
  case kPingRequest: {
    nlohmann::json response{{"server_send_tick", wire_monotonic_tick(monotonic_ticks)},
                            {"timestamp", std::to_string(epoch_ticks)}};
    copy_present(request, response, "ping_cycles64", "ping_cycles64");
    copy_present(request, response, "last_round_trip_nanoseconds", "last_round_trip_nanoseconds");
    return protocol_.encode(kPongResponse, response);
  }
  case kMapLoadCompleteRequest: {
    if (!character_bootstrap_.selected_character() || !map_move_started_ || map_move_completed_) {
      return std::nullopt;
    }

    const WorldPosition spawn_position{
        map_id_,
        character_bootstrap_.selected_character()->world_position
            ? character_bootstrap_.selected_character()->world_position->x_mm
            : kDiagnosticSpawnX,
        character_bootstrap_.selected_character()->world_position
            ? character_bootstrap_.selected_character()->world_position->y_mm
            : kDiagnosticSpawnY,
        character_bootstrap_.selected_character()->world_position
            ? character_bootstrap_.selected_character()->world_position->z_mm
            : kDiagnosticSpawnZ,
        character_bootstrap_.selected_character()->world_position
            ? character_bootstrap_.selected_character()->world_position->yaw
            : kDiagnosticSpawnYaw};
    nlohmann::json notification{
        {"map_id", map_id_},
        {"player_unit_id", diagnostic_unit_id_},
        {"nub", player_unit_nub(*character_bootstrap_.selected_character(), spawn_position,
                                character_bootstrap_.stats(), character_bootstrap_.dead())}};
    if (restore_gliding_on_map_completion_) {
      // The client applies PlayerNub.move_mode during local-player restoration and enters
      // glide without issuing another start request (recovered build 40367e24).
      notification["nub"]["player"]["move_mode"] = "MoveMode_Gliding";
    }
    restore_gliding_on_map_completion_ = false;
    // The official Magic entry sends two bodyless weapon-gauge frames, then
    // attribute/trait snapshots before the client's StartPlaying request.
    {
      const auto packet = combat::entry_weapon_gauge();
      pending_notifications_.push_back(protocol_.encode(packet.opcode, packet.message));
    }
    {
      const auto packet = combat::entry_weapon_gauge();
      pending_notifications_.push_back(protocol_.encode(packet.opcode, packet.message));
    }
    {
      const auto packet = combat::entry_unit_attribute(diagnostic_unit_id_, 1);
      pending_notifications_.push_back(protocol_.encode(packet.opcode, packet.message));
    }
    {
      const auto packet = combat::entry_unit_traits(diagnostic_unit_id_);
      pending_notifications_.push_back(protocol_.encode(packet.opcode, packet.message));
    }
    {
      const auto packet = combat::entry_unit_attribute(diagnostic_unit_id_, 2);
      pending_notifications_.push_back(protocol_.encode(packet.opcode, packet.message));
    }
    map_move_completed_ = true;
    return protocol_.encode(kMapMoveCompleteNotification, notification);
  }
  case kStartPlayingRequest: {
    if (!character_bootstrap_.selected_character() || !map_move_completed_ ||
        start_playing_completed_) {
      return std::nullopt;
    }

    auto response =
        protocol_.encode(kStartPlayingResponse, {{"unit_id", std::to_string(diagnostic_unit_id_)}});
    {
      const auto packet = combat::entry_unit_attribute(diagnostic_unit_id_, 1);
      pending_notifications_.push_back(protocol_.encode(packet.opcode, packet.message));
    }

    if (character_bootstrap_.dead()) {
      // Reconnecting restores an existing death. Publish its presentation only after
      // the local unit exists; do not replay damage or commit another death penalty.
      const auto position = current_position().value();
      const auto unit_id = std::to_string(diagnostic_unit_id_);
      {
        const auto packet = combat::saved_player_demise(unit_id, monotonic_ticks);
        pending_notifications_.push_back(protocol_.encode(packet.opcode, packet.message));
      }
      {
        const auto packet = combat::saved_player_death(unit_id, position);
        pending_notifications_.push_back(protocol_.encode(packet.opcode, packet.message));
      }
    }

    start_playing_completed_ = true;
    return response;
  }
  case kMoveRequest: {
    if (character_bootstrap_.selected_character() && map_move_completed_) {
      const auto position = request.find("pos");
      // Protobuf omits zero-valued coordinates from its JSON representation.
      if (position != request.end() && position->is_object()) {
        const auto yaw = request.value("body_yaw", kDiagnosticSpawnYaw);
        if (!std::isfinite(yaw)) {
          throw std::runtime_error("World movement yaw is invalid");
        }

        last_position_ = WorldPosition{map_id_, position->value("millimeter_x", 0),
                                       position->value("millimeter_y", 0),
                                       position->value("millimeter_z", 0), yaw};
        last_movement_ = request;
      }
    }

    if (bootstrap_diagnostic_enabled_ && logged_move_requests_ < kMaximumDiagnosticMoveLogs) {
      ++logged_move_requests_;
      const auto interval_ms =
          last_move_received_tick_ ? (monotonic_ticks - *last_move_received_tick_) / 10000 : -1;
      last_move_received_tick_ = monotonic_ticks;
      const auto position = request.find("pos");
      const bool has_mode = request.contains("mode");
      const bool has_detail = request.contains("detail");
      const bool has_velocity = request.contains("velocity");
      const bool has_pred_pos = request.contains("pred_pos");
      const bool has_pred_milli = request.contains("pred_milli");
      if (position != request.end() && position->is_object() &&
          position->contains("millimeter_x") && position->contains("millimeter_y") &&
          position->contains("millimeter_z")) {
        spdlog::info("World bootstrap diagnostic move index={} interval_ms={} "
                     "pos_mm=({},{},{}) mode={} detail={} velocity={} pred_pos={} pred_milli={}",
                     logged_move_requests_, interval_ms, position->at("millimeter_x").get<int>(),
                     position->at("millimeter_y").get<int>(),
                     position->at("millimeter_z").get<int>(), has_mode, has_detail, has_velocity,
                     has_pred_pos, has_pred_milli);
      } else {
        spdlog::info("World bootstrap diagnostic move index={} interval_ms={} pos=absent "
                     "mode={} detail={} velocity={} pred_pos={} pred_milli={}",
                     logged_move_requests_, interval_ms, has_mode, has_detail, has_velocity,
                     has_pred_pos, has_pred_milli);
      }
    }

    return std::nullopt;
  }
  case kCharacterExitRequest:
    exited_ = true;
    return protocol_.encode(kCharacterExitResponse, nlohmann::json::object());
  default:
    // The bootstrap listener deliberately emits no fabricated gameplay state.
    return std::nullopt;
  }
}

std::optional<Frame> WorldHandshake::take_pending_notification() {
  if (pending_notifications_.empty()) {
    return std::nullopt;
  }

  auto notification = std::move(pending_notifications_.front());
  pending_notifications_.pop_front();
  return notification;
}

std::optional<WorldGameplayRequest> WorldHandshake::take_gameplay_request() {
  auto request = std::move(gameplay_request_);
  gameplay_request_.reset();
  return request;
}

int WorldHandshake::map_id() const {
  return map_id_;
}

std::optional<WorldPosition> WorldHandshake::last_position() const {
  return last_position_;
}

// Candidate placement: persisted housing objects and unredeemed trade vouchers are announced
// after the money snapshot, only when the character owns any. Official empty snapshots (131,
// 142) were observed but their position relative to 130/126 was not checked.
void WorldHandshake::append_shop_adjacent_snapshots() {
  if (auto packet = housing_bootstrap_.snapshot()) {
    pending_notifications_.push_back(protocol_.encode(packet->opcode, packet->message));
  }

  if (const auto vouchers = trade_bootstrap_.snapshot()) {
    pending_notifications_.push_back(protocol_.encode(vouchers->opcode, vouchers->message));
  }
}

std::optional<WorldPosition> WorldHandshake::current_position() const {
  if (!character_bootstrap_.selected_character() || !map_move_completed_ || exited_) {
    return std::nullopt;
  }

  const WorldPosition position{map_id_,
                               character_bootstrap_.selected_character()->world_position
                                   ? character_bootstrap_.selected_character()->world_position->x_mm
                                   : kDiagnosticSpawnX,
                               character_bootstrap_.selected_character()->world_position
                                   ? character_bootstrap_.selected_character()->world_position->y_mm
                                   : kDiagnosticSpawnY,
                               character_bootstrap_.selected_character()->world_position
                                   ? character_bootstrap_.selected_character()->world_position->z_mm
                                   : kDiagnosticSpawnZ,
                               character_bootstrap_.selected_character()->world_position
                                   ? character_bootstrap_.selected_character()->world_position->yaw
                                   : kDiagnosticSpawnYaw};
  return last_position_.value_or(position);
}

std::optional<WorldPresenceSnapshot> WorldHandshake::presence_snapshot() const {
  const auto position = current_position();
  if (!position) {
    return std::nullopt;
  }
  return WorldPresenceSnapshot{*character_bootstrap_.selected_character(), *position,
                               character_bootstrap_.stats(), false, character_bootstrap_.dead()};
}

std::optional<nlohmann::json> WorldHandshake::take_last_movement() {
  auto movement = std::move(last_movement_);
  last_movement_.reset();
  return movement;
}

bool WorldHandshake::exited() const {
  return exited_;
}

std::shared_ptr<NativeWorldListener> start_native_world(
    asio::io_context &io_context, asio::thread_pool &workers, AuthService &auth,
    const tcp::endpoint &endpoint, const std::filesystem::path &protocol_directory,
    const std::filesystem::path &certificate_file, const std::filesystem::path &private_key_file,
    std::chrono::milliseconds handshake_delay, bool tls12_only, bool credential_diagnostic_enabled,
    bool bootstrap_diagnostic_enabled, std::shared_ptr<const TutorialData> tutorial_data,
    std::shared_ptr<const CharacterStats> character_stats,
    std::shared_ptr<ClientCatalog> client_catalog,
    std::shared_ptr<const GatheringData> gathering_data, std::shared_ptr<const LootData> loot_data,
    std::shared_ptr<const ProcessingData> processing_data,
    const std::filesystem::path &catalog_root, std::shared_ptr<const NpcPopulation> npc_population,
    std::shared_ptr<GuildService> guilds, std::shared_ptr<housing::HousingHub> housing,
    unsigned maximum_connections, WorldSocialServices social) {
  if (maximum_connections == 0 || maximum_connections > kMaximumWorldCapacity) {
    throw std::invalid_argument("maximum world connections must be between 1 and " +
                                std::to_string(kMaximumWorldCapacity));
  }
  if (handshake_delay < std::chrono::milliseconds::zero() ||
      handshake_delay > kMaximumWorldHandshakeDelay) {
    throw std::invalid_argument("World handshake delay must be between 0 and 10000 ms");
  }

  auto listener = std::make_shared<WorldListener>(
      io_context, workers, auth, protocol_directory, handshake_delay, credential_diagnostic_enabled,
      bootstrap_diagnostic_enabled, std::move(tutorial_data), std::move(character_stats),
      std::move(client_catalog), std::move(gathering_data), std::move(loot_data),
      std::move(processing_data), catalog_root, std::move(npc_population), std::move(guilds),
      std::move(housing), maximum_connections, std::move(social));
  listener->run(io_context, endpoint,
                WorldTlsOptions{certificate_file, private_key_file, tls12_only});
  listener->schedule_housing_expiry();
  return listener;
}

} // namespace chronicles
