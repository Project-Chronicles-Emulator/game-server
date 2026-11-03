#include "world/features/gm/tests/unexpected_gm_context.hpp"

#include "world/features/combat/combat.hpp"
#include "world/features/gm/gm_feature.hpp"
#include "world/session/frame_pipeline.hpp"

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace chronicles::gm {
namespace {

constexpr std::int64_t kSecond = 10'000'000;
// Stands in for map 20101, the housing area that overlays field 9012.
constexpr int kTestHousingAreaMap = 20101;

const ShopCatalog &test_catalog() {
  static const ShopCatalog catalog([] {
    ShopCatalogTables tables;
    tables.game_variables = {{{"name", "ItemMaxStack"}, {"value", "1000"}},
                             {{"name", "ResetBaseDateTime"}, {"value", "2023-01-02 05:00:00"}}};
    tables.money = {{{"type", "Gold"}, {"limitUserMax", "1000000"}}};
    return tables;
  }());
  return catalog;
}

std::unique_ptr<CombatSession> test_combat() {
  auto data =
      std::make_shared<CombatData>(nlohmann::json{{"format_version", 1},
                                                  {"skills", nlohmann::json::array()},
                                                  {"effects", nlohmann::json::object()},
                                                  {"projectiles", nlohmann::json::object()},
                                                  {"npc_stats", nlohmann::json::object()},
                                                  {"npc_skills", nlohmann::json::object()},
                                                  {"weapon_items", nlohmann::json::object()},
                                                  {"recoveries", nlohmann::json::object()}});
  Character character;
  character.id = 19;
  character.class_id = 1;

  return std::make_unique<CombatSession>(
      std::move(data), nullptr, character,
      nlohmann::json{{"18", 100}, {"19", 100}, {"69", 100}, {"72", 100}});
}

bool available_on_another_thread(world_metrics::TimedMutex &mutex) {
  return std::async(std::launch::async,
                    [&mutex] {
                      if (!mutex.try_lock()) {
                        return false;
                      }
                      mutex.unlock();
                      return true;
                    })
      .get();
}

struct ListedPeerLifetime {
  explicit ListedPeerLifetime(std::vector<std::string> &trace) : trace_(trace) {
  }

  ~ListedPeerLifetime() {
    trace_.push_back("release-listed-peer");
  }

  std::vector<std::string> &trace_;
};

// A containing session with the production subscription factory and real GM session.
// Only external services are faked; unexpected capabilities fail through the default stub.
// Preserve the old TEST call shape only in this test's anonymous namespace.
class SessionHarness;
SessionEventBus compose_session_events(SessionHarness &containing,
                                       SessionEventObservers observers = {});

class SessionHarness final : public testing::UnexpectedGmContext,
                             public std::enable_shared_from_this<SessionHarness> {
public:
  SessionHarness(boost::asio::io_context &io, boost::asio::io_context &worker)
      : io_(io), worker_(worker), feature(mutex), events(compose_session_events(*this)),
        session(feature.create_session(*this, events)) {
    character.id = identity->character_id;
    character.class_id = 1;
    feature.set_reply_handler([this](const ChatIdentity &, const std::string &text) {
      EXPECT_TRUE(available_on_another_thread(mutex));
      replies.push_back(text);
      trace.push_back("reply");
    });
  }

  FrameContext text_frame(const std::string &text) const {
    return FrameContext(kGmCommandTextRequestOpcode,
                        WorldGameplayRequest{kGmCommandTextRequestOpcode, 7, {{"text", text}}});
  }

  FrameContext run(const std::string &text) {
    auto frame = text_frame(text);
    EXPECT_TRUE(session->handle_generic_gm(frame));
    return frame;
  }

  std::optional<GmIdentity> gm_identity() const override {
    return identity;
  }

  std::optional<ChatIdentity> gm_chat_identity() const override {
    ++chat_queries;
    return ChatIdentity{1, 19, "Keeper"};
  }

  std::uint64_t gm_connection_id() const override {
    return 88;
  }

  bool gm_closed() const override {
    return closed;
  }

  std::int64_t gm_monotonic_ticks() const override {
    return ticks;
  }

  std::int64_t gm_epoch_ticks() const override {
    return 123 * kSecond;
  }

  world_metrics::TimedMutex &gm_protocol_mutex() const override {
    return mutex;
  }

  std::optional<WorldPosition> gm_position_locked() const override {
    return position;
  }

  bool gm_has_combat() const override {
    return static_cast<bool>(combat);
  }

  bool gm_combat_add_monster(std::uint64_t unit_id, int npc_cms_id,
                             const WorldPosition &position) override {
    return combat->add_gm_monster(unit_id, npc_cms_id, position);
  }

  bool gm_combat_decorate_spawn(GameplayPacket &packet) const override {
    return combat->decorate_spawn(packet);
  }

  std::vector<GameplayPacket> gm_combat_add_abnormal_gauge(std::uint64_t unit_id,
                                                           combat_rules::AbnormalType type,
                                                           int amount,
                                                           std::int64_t ticks) override {
    return combat->gm_add_abnormal_gauge(unit_id, type, amount, ticks);
  }

  std::vector<GameplayPacket> gm_combat_add_status_effect(std::uint64_t unit_id,
                                                          int status_effect_id,
                                                          std::int64_t ticks) override {
    return combat->gm_add_status_effect(unit_id, status_effect_id, ticks);
  }

  std::vector<GameplayPacket> gm_combat_cancel_skill(std::uint64_t unit_id,
                                                     const WorldPosition &position,
                                                     std::int64_t ticks) override {
    return combat->gm_cancel_skill(unit_id, position, ticks);
  }

  std::vector<GameplayPacket> gm_combat_damage_monster(std::uint64_t unit_id, int amount,
                                                       std::int64_t ticks) override {
    return combat->gm_damage_monster(unit_id, amount, ticks);
  }

  std::vector<GameplayPacket> gm_combat_execute_self_effect(int effect_id,
                                                            const WorldPosition &position,
                                                            std::int64_t ticks) override {
    return combat->gm_execute_self_effect(effect_id, position, ticks);
  }

  std::vector<GameplayPacket> gm_combat_kill_player(const WorldPosition &position,
                                                    std::int64_t ticks) override {
    return combat->gm_kill_player(position, ticks);
  }

  std::vector<GameplayPacket> gm_combat_place_monster(std::uint64_t unit_id,
                                                      const WorldPosition &destination,
                                                      std::int64_t ticks) override {
    return combat->gm_place_monster(unit_id, destination, ticks);
  }

  std::vector<GameplayPacket> gm_combat_remove_status_effect(std::uint64_t unit_id,
                                                             int status_effect_id,
                                                             std::int64_t ticks) override {
    return combat->gm_remove_status_effect(unit_id, status_effect_id, ticks);
  }

  std::size_t gm_combat_reset_cooldowns() override {
    return combat->gm_reset_cooldowns();
  }

  std::vector<GameplayPacket> gm_combat_restore_vitals(std::int64_t ticks) override {
    return combat->gm_restore_vitals(ticks);
  }

  std::vector<GameplayPacket> gm_combat_set_player_stat(int stat_type, int value,
                                                        std::int64_t ticks) override {
    return combat->gm_set_player_stat(stat_type, value, ticks);
  }

  std::vector<GameplayPacket> gm_combat_toggle_monster_ai(std::uint64_t unit_id, bool enabled,
                                                          std::int64_t ticks) override {
    return combat->gm_toggle_monster_ai(unit_id, enabled, ticks);
  }

  bool gm_combat_visible_monster(std::uint64_t unit_id) const override {
    return combat->is_visible_world_monster(unit_id);
  }

  std::optional<int> gm_combat_monster_health(std::uint64_t unit_id) const override {
    return combat->live_monster_health(unit_id);
  }

  std::optional<CombatSession::MonsterDebugView>
  gm_combat_monster_debug_view(std::uint64_t unit_id, std::int64_t ticks) const override {
    return combat->monster_debug_view(unit_id, ticks);
  }

  bool gm_combat_player_dead() const override {
    return combat->player_dead();
  }

  const nlohmann::json &gm_combat_player_stats() const override {
    return combat->player_stats();
  }

  std::vector<GameplayPacket> gm_combat_remove_monster(std::uint64_t unit_id,
                                                       std::int64_t ticks) override {
    return combat->remove_gm_monster(unit_id, ticks);
  }

  void gm_combat_update_player_stats(nlohmann::json stats) override {
    combat->update_player_stats(std::move(stats));
  }

  const ShopCatalog *gm_shop_catalog() const override {
    return catalog;
  }

  bool gm_has_tutorial() const override {
    return has_tutorial;
  }

  bool gm_in_housing() const override {
    return in_housing;
  }

  std::optional<int> gm_housing_area_parent(int map_id) const override {
    return map_id == kTestHousingAreaMap ? std::optional<int>(9012) : std::nullopt;
  }

  ClientCatalog *gm_client_catalog() const override {
    return nullptr;
  }

  std::optional<int> gm_region_map(const WorldPosition &destination,
                                   bool keep_public_channel) const override {
    region_queries.emplace_back(destination, keep_public_channel);
    return region_map;
  }

  bool gm_begin_map_move(const WorldPosition &destination) override {
    trace.push_back("begin-map-move");
    map_moves.push_back(destination);
    return true;
  }

  void gm_stage_map_move(const WorldPosition &, bool) override {
    trace.push_back("stage-map-move");
  }

  std::optional<Character> gm_character() const override {
    return character;
  }

  WorldProgress gm_progress() const override {
    return progress;
  }

  std::optional<std::vector<CharacterItem>> gm_commit_item_trade(const WorldProgress &,
                                                                 const WorldProgress &after,
                                                                 const ItemTrade &) override {
    trace.push_back("commit-items");
    if (item_commit_refused) {
      return std::nullopt;
    }
    committed_progress = after;
    return std::vector<CharacterItem>{};
  }

  void gm_adopt_committed_progress(const WorldProgress &after) {
    trace.push_back("adopt-progress");
    progress = after;
  }

  void gm_refresh_traded_items(const Character &, const ShopTrade &,
                               const std::vector<CharacterItem> &) override {
    trace.push_back("refresh-items");
  }

  std::optional<std::vector<GameplayPacket>>
  gm_dispatch_event_request(const WorldGameplayRequest &) override {
    trace.push_back("event-parser");
    return event_result;
  }

  std::optional<std::vector<GameplayPacket>>
  gm_dispatch_expression_request(const WorldGameplayRequest &) override {
    trace.push_back("expression-parser");
    return expression_result;
  }

  std::optional<std::vector<GameplayPacket>>
  gm_dispatch_housing_request(const WorldGameplayRequest &) override {
    trace.push_back("housing-parser");
    return housing_result;
  }

  bool gm_has_guilds() const override {
    return true;
  }

  std::int64_t gm_own_guild_id(std::int64_t) const override {
    return 29;
  }

  x5::ec::EErrorCode gm_guild_add_experience(std::int64_t guild_id, int amount) override {
    EXPECT_EQ(guild_id, 29);
    guild_experience += amount;
    return x5::ec::Success;
  }

  bool gm_has_companies() const override {
    return true;
  }

  CompanyRecord gm_company_record(std::int64_t) const override {
    return company;
  }

  CompanyResult gm_commit_company_record(const CompanyRecord &record) override {
    trace.push_back("commit-company");
    CompanyResult result;
    result.error = company_commit_error;
    result.status.record = record;
    if (result.error == x5::ec::Success) {
      company = record;
    }
    return result;
  }

  bool gm_has_mail_storage() const override {
    return true;
  }

  std::optional<MailRecord> gm_deliver_mail_record(const MailRecord &mail) override {
    trace.push_back("deliver-mail");
    if (mail_delivery_throws) {
      throw std::runtime_error("fixture storage failure");
    }
    auto delivered = mail;
    delivered.id = 73;
    return delivered;
  }

  void gm_notify_mail_peer(const MailRecord &, const std::shared_ptr<GmPeer> &) override {
    trace.push_back("notify-mail");
    if (mail_notification_throws) {
      throw std::runtime_error("fixture post failure");
    }
  }

  std::optional<GmTarget> gm_find_target(const std::string &, std::int64_t character_id) override {
    trace.push_back("find-mail-target");
    target_queries.push_back(character_id);
    return mail_target;
  }

  GmOnlinePlayers gm_online_players() override {
    GmOnlinePlayers players;
    players.lines = {"First - map 9012", "Second [GM] - map 9012"};
    auto peer = std::make_shared<ListedPeerLifetime>(trace);
    listed_peer = peer;
    players.retained_peers.push_back(std::move(peer));
    return players;
  }

  std::optional<std::vector<GameplayPacket>>
  gm_request_server_teleport(const WorldPosition &destination, std::int64_t) override {
    trace.push_back("teleport");
    last_teleport = destination;
    return std::vector<GameplayPacket>{{33, {{"teleported", true}}}};
  }

  Frame gm_encode_packet_locked(std::uint16_t opcode, const nlohmann::json &) override {
    trace.push_back("encode");
    encoded_opcodes.push_back(opcode);
    return Frame{{1, 2, 3}, {}};
  }

  void gm_post_frame(Frame frame) override {
    trace.push_back("post-frame");
    posted.push_back(std::move(frame));
  }

  void gm_close_on_io() override {
    trace.push_back("close-io");
    closed = true;
  }

  boost::asio::any_io_executor gm_io_executor() override {
    return io_.get_executor();
  }

  boost::asio::any_io_executor gm_worker_executor() override {
    return worker_.get_executor();
  }

  std::shared_ptr<void> retain_gm_owner() override {
    return shared_from_this();
  }

  std::optional<std::int64_t> gm_next_gameplay_tick() const override {
    return 1234;
  }

  void gm_apply_protection_change(const PlayerProtectionChanged &event) {
    trace.push_back("protection");
    last_protection = event;
    if (combat) {
      combat->set_gm_protection(event.invincible, event.hidden, event.infinite_stamina);
    }
  }

  void gm_apply_visibility_change(const PlayerVisibilityChanged &event) {
    EXPECT_EQ(event.connection_id, 88);
    trace.push_back("visibility");
    if (visibility_throws) {
      throw std::runtime_error("fixture presence failure");
    }
  }

  void gm_refresh_combat_snapshot(const PlayerCombatSnapshotChanged &) {
    EXPECT_FALSE(available_on_another_thread(mutex));
    trace.push_back("refresh-combat");
  }

  void gm_refresh_company_membership(const CompanyMembershipChanged &) {
    trace.push_back("refresh-company");
  }

  boost::asio::io_context &io_;
  boost::asio::io_context &worker_;
  mutable world_metrics::TimedMutex mutex;
  std::optional<GmIdentity> identity{GmIdentity{3, 19, 1, true}};
  mutable int chat_queries{};
  bool closed{};
  std::int64_t ticks{10 * kSecond};
  std::optional<WorldPosition> position{WorldPosition{9012, 1000, 2000, 3000, 0}};
  std::unique_ptr<CombatSession> combat;
  const ShopCatalog *catalog{};
  bool has_tutorial{};
  Character character;
  WorldProgress progress;
  WorldProgress committed_progress;
  bool item_commit_refused{};
  std::optional<std::vector<GameplayPacket>> event_result;
  std::optional<std::vector<GameplayPacket>> expression_result;
  std::optional<std::vector<GameplayPacket>> housing_result;
  std::vector<std::string> trace;
  std::vector<std::string> replies;
  int guild_experience{};
  CompanyRecord company;
  x5::ec::EErrorCode company_commit_error{x5::ec::Success};
  bool mail_delivery_throws{};
  bool mail_notification_throws{};
  std::optional<GmTarget> mail_target;
  std::vector<std::int64_t> target_queries;
  std::weak_ptr<void> listed_peer;
  std::optional<WorldPosition> last_teleport;
  std::optional<int> region_map;
  bool in_housing{};
  mutable std::vector<std::pair<WorldPosition, bool>> region_queries;
  std::vector<WorldPosition> map_moves;
  std::vector<std::uint16_t> encoded_opcodes;
  std::vector<Frame> posted;
  std::optional<PlayerProtectionChanged> last_protection;
  bool visibility_throws{};
  GmFeature feature;
  SessionEventBus events;
  std::unique_ptr<GmSession> session;
};

// Test-only bridge; production event composition has no GM authority or overload.
SessionEventBus compose_session_events(SessionHarness &containing,
                                       SessionEventObservers observers) {
  observers.protection = [&containing](const PlayerProtectionChanged &event) {
    containing.gm_apply_protection_change(event);
  };
  observers.visibility = [&containing](const PlayerVisibilityChanged &event) {
    containing.gm_apply_visibility_change(event);
  };
  observers.combat_snapshot = [&containing](const PlayerCombatSnapshotChanged &event) {
    containing.gm_refresh_combat_snapshot(event);
  };
  observers.company_membership = [&containing](const CompanyMembershipChanged &event) {
    containing.gm_refresh_company_membership(event);
  };
  observers.progress_committed = [&containing](const PlayerProgressCommitted &event) {
    containing.gm_adopt_committed_progress(event.progress);
  };
  return ::chronicles::compose_session_events(std::move(observers));
}

class GmFeatureTest : public ::testing::Test {
protected:
  boost::asio::io_context io;
  boost::asio::io_context worker;
  std::shared_ptr<SessionHarness> harness = std::make_shared<SessionHarness>(io, worker);
};

TEST_F(GmFeatureTest, ConstructionDoesNotCallCapabilitiesOrPublishEffects) {
  EXPECT_TRUE(harness->trace.empty());
  EXPECT_EQ(harness->chat_queries, 0);
  EXPECT_FALSE(harness->session->god_mode());
  EXPECT_FALSE(harness->session->invisible());
  EXPECT_FALSE(harness->session->infinite_stamina());
  EXPECT_FALSE(harness->session->free_movement());
  EXPECT_FALSE(harness->session->has_spawned_units());
  EXPECT_FALSE(harness->session->next_watch_tick());
}

TEST_F(GmFeatureTest, CompositionUsesNamedEffectsAndKeepsPayloadReferencesSynchronous) {
  std::vector<std::string> order;
  SessionEventObservers observers;
  observers.activity_experience = [&](const CharacterActivityExperienceGained &event) {
    EXPECT_EQ(event.character_id, 19);
    EXPECT_EQ(event.experience, 7);
    order.push_back("guild-exp");
  };
  observers.inventory_stats = [&](const PlayerInventoryStatsChanged &event) {
    EXPECT_EQ(&event.character, &harness->character);
    order.push_back("stats");
  };
  nlohmann::json prepared_stats{{"18", 100}};
  observers.inventory_snapshot = [&](const PlayerInventorySnapshotChanged &event) {
    EXPECT_EQ(&event.character, &harness->character);
    EXPECT_EQ(event.prepared_stats, &prepared_stats);
    order.push_back("snapshot");
  };
  auto events = compose_session_events(*harness, std::move(observers));

  events.publish(CharacterActivityExperienceGained{19, 7});
  events.publish(PlayerInventoryStatsChanged{harness->character});
  events.publish(PlayerInventorySnapshotChanged{harness->character, &prepared_stats});
  EXPECT_EQ(order, (std::vector<std::string>{"guild-exp", "stats", "snapshot"}));
}

TEST_F(GmFeatureTest, RoleRefusalPrecedesOtherParsersAndDoesNotSpendTheGmBudget) {
  harness->identity->game_master = false;
  for (int attempt = 0; attempt < 5; ++attempt) {
    harness->run("unowned-command");
  }
  EXPECT_EQ(std::ranges::count(harness->trace, "event-parser"), 0);
  EXPECT_EQ(harness->replies.size(), 5);

  harness->identity->game_master = true;
  for (int request = 0; request < 4; ++request) {
    EXPECT_TRUE(harness->session->allow_gm_request());
  }
  EXPECT_FALSE(harness->session->allow_gm_request());
}

TEST_F(GmFeatureTest, MissingHandoffShortCircuitsIdentityDependentReplyReads) {
  harness->identity.reset();
  harness->session->gm_reply("ignored");
  harness->run("god on");
  EXPECT_EQ(harness->chat_queries, 0);
  EXPECT_TRUE(harness->trace.empty());
}

TEST_F(GmFeatureTest, SharedGmBudgetResetsAtTheExactOneSecondBoundary) {
  for (int request = 0; request < 4; ++request) {
    EXPECT_TRUE(harness->session->allow_gm_request());
  }
  ++harness->ticks;
  EXPECT_FALSE(harness->session->allow_gm_request());

  harness->ticks = 11 * kSecond - 1;
  EXPECT_FALSE(harness->session->allow_gm_request());
  ++harness->ticks;
  EXPECT_TRUE(harness->session->allow_gm_request());
}

TEST_F(GmFeatureTest, PlayerCarCooldownHasItsOwnFiveSecondWindow) {
  EXPECT_TRUE(harness->session->allow_player_car());
  EXPECT_TRUE(harness->session->allow_gm_request());
  harness->ticks += 5 * kSecond - 1;
  EXPECT_FALSE(harness->session->allow_player_car());
  ++harness->ticks;
  EXPECT_TRUE(harness->session->allow_player_car());
}

TEST_F(GmFeatureTest, UnrelatedRequestFallsThroughWithoutReplacingOutputOrDeadline) {
  FrameContext frame(65535, WorldGameplayRequest{65535, 7, nlohmann::json::object()});
  frame.session_packets.push_back({91, {{"existing", true}}});
  frame.action_deadline = 99;

  EXPECT_FALSE(harness->session->handle_generic_gm(frame));
  EXPECT_FALSE(harness->session->handle_social_gm(frame));
  ASSERT_EQ(frame.session_packets.size(), 1);
  EXPECT_EQ(frame.session_packets.front().opcode, 91);
  EXPECT_EQ(frame.action_deadline, 99);
  EXPECT_TRUE(harness->trace.empty());
}

TEST_F(GmFeatureTest, ClaimedEmptyEventResultStopsLaterParsersAndReplacesPackets) {
  harness->event_result = std::vector<GameplayPacket>{};
  auto frame = harness->text_frame("event-owned-command");
  frame.session_packets.push_back({91, nlohmann::json::object()});

  EXPECT_TRUE(harness->session->handle_generic_gm(frame));
  EXPECT_TRUE(frame.session_packets.empty());
  EXPECT_EQ(frame.action_deadline, 1234);
  EXPECT_EQ(harness->trace, std::vector<std::string>{"event-parser"});
}

TEST_F(GmFeatureTest, UnmatchedEventThenExpressionPreservesDispatchOrder) {
  harness->expression_result = std::vector<GameplayPacket>{{42, {{"from", "expression"}}}};
  const auto frame = harness->run("expression-owned-command");

  ASSERT_EQ(frame.session_packets.size(), 1);
  EXPECT_EQ(frame.session_packets.front().opcode, 42);
  EXPECT_EQ(harness->trace, (std::vector<std::string>{"event-parser", "expression-parser"}));
}

TEST_F(GmFeatureTest, StructuredRequestSkipsTextOnlyExpressionParser) {
  harness->housing_result = std::vector<GameplayPacket>{};
  FrameContext frame(
      kGmCommandRequestOpcode,
      WorldGameplayRequest{kGmCommandRequestOpcode, 7, {{"cmds", nlohmann::json::array()}}});

  EXPECT_TRUE(harness->session->handle_generic_gm(frame));
  EXPECT_EQ(harness->trace, (std::vector<std::string>{"event-parser", "housing-parser"}));
}

TEST_F(GmFeatureTest, MatchedInvalidDiagnosticDoesNotFallThroughToFeatureParsers) {
  const auto frame = harness->run("game_time nonsense");
  EXPECT_TRUE(frame.session_packets.empty());
  EXPECT_EQ(harness->trace, std::vector<std::string>{"reply"});
  ASSERT_EQ(harness->replies.size(), 1);
  EXPECT_TRUE(harness->replies.front().starts_with("Usage:"));
}

TEST_F(GmFeatureTest, SocialCommandKeepsItsOriginalBudgetAndDeadlineSemantics) {
  for (int request = 0; request < 5; ++request) {
    harness->session->allow_gm_request();
  }
  auto frame = harness->text_frame("guild_exp 17");
  frame.action_deadline = 99;

  EXPECT_TRUE(harness->session->handle_social_gm(frame));
  EXPECT_EQ(harness->guild_experience, 17);
  EXPECT_EQ(frame.action_deadline, 99);
  EXPECT_TRUE(harness->trace.empty());
}

TEST_F(GmFeatureTest, CompanyRefreshPublishesOnlyAfterSuccessfulCommit) {
  harness->company.company_type = 1;
  auto frame = harness->text_frame("company_contribution 7");
  EXPECT_TRUE(harness->session->handle_social_gm(frame));
  EXPECT_EQ(harness->trace, (std::vector<std::string>{"commit-company", "refresh-company"}));
  ASSERT_EQ(frame.session_packets.size(), 1);
  EXPECT_EQ(harness->company.contribution, 7);

  harness->trace.clear();
  harness->company_commit_error = x5::ec::GmCmd_Fail;
  auto refused_frame = harness->text_frame("company_contribution 9");
  EXPECT_TRUE(harness->session->handle_social_gm(refused_frame));
  EXPECT_EQ(harness->trace, std::vector<std::string>{"commit-company"});
  EXPECT_TRUE(refused_frame.session_packets.empty());
  EXPECT_EQ(harness->company.contribution, 7);
}

TEST_F(GmFeatureTest, VisibilityPublishesBeforeProtectionAndReplyThroughRealComposition) {
  harness->combat = test_combat();
  harness->run("invisible on");

  EXPECT_TRUE(harness->session->invisible());
  EXPECT_EQ(harness->trace, (std::vector<std::string>{"visibility", "protection", "reply"}));
  ASSERT_TRUE(harness->last_protection);
  EXPECT_TRUE(harness->last_protection->hidden);
  EXPECT_FALSE(harness->last_protection->invincible);
}

TEST_F(GmFeatureTest, PresenceFailurePropagatesAfterVisibilityStateChangeBeforeLaterEffects) {
  harness->combat = test_combat();
  harness->visibility_throws = true;
  auto frame = harness->text_frame("invisible on");

  EXPECT_THROW(harness->session->handle_generic_gm(frame), std::runtime_error);
  EXPECT_TRUE(harness->session->invisible());
  EXPECT_EQ(harness->trace, std::vector<std::string>{"visibility"});
  EXPECT_FALSE(harness->last_protection);
}

TEST_F(GmFeatureTest, GodAndFlyKeepInvinciblePacketAndProtectionFlagsConsistent) {
  harness->combat = test_combat();
  auto frame = harness->run("god on");
  ASSERT_EQ(frame.session_packets.size(), 1);
  EXPECT_EQ(frame.session_packets.front().opcode, 33);
  EXPECT_EQ(frame.session_packets.front().message.at("attributes").at("2"), 1);
  ASSERT_TRUE(harness->last_protection);
  EXPECT_TRUE(harness->last_protection->invincible);

  harness->run("fly on");
  const auto god_off_frame = harness->run("god off");
  EXPECT_FALSE(harness->session->god_mode());
  EXPECT_TRUE(harness->session->free_movement());
  EXPECT_TRUE(harness->last_protection->invincible);
  EXPECT_TRUE(harness->last_protection->infinite_stamina);
  EXPECT_EQ(god_off_frame.session_packets.front().message.at("attributes").at("2"), 1);
}

TEST_F(GmFeatureTest, CombatSnapshotSubscriberRunsInsideTheExistingCodecLock) {
  harness->combat = test_combat();
  harness->run("heal");
  EXPECT_EQ(harness->trace, (std::vector<std::string>{"refresh-combat", "reply"}));
}

TEST_F(GmFeatureTest, CommittedProgressPublicationPrecedesInventoryRefreshAndPacket) {
  harness->catalog = &test_catalog();
  harness->has_tutorial = true;
  auto frame = harness->run("add_money 7");

  EXPECT_EQ(harness->trace,
            (std::vector<std::string>{"event-parser", "expression-parser", "housing-parser",
                                      "commit-items", "adopt-progress", "refresh-items", "reply"}));
  EXPECT_EQ(harness->progress.gold, 7);
  ASSERT_EQ(frame.session_packets.size(), 1);
  EXPECT_EQ(frame.session_packets.front().opcode, 169);

  harness->trace.clear();
  harness->item_commit_refused = true;
  const auto refused_frame = harness->run("add_money 9");
  EXPECT_TRUE(refused_frame.session_packets.empty());
  EXPECT_EQ(std::ranges::count(harness->trace, "adopt-progress"), 0);
  EXPECT_EQ(std::ranges::count(harness->trace, "refresh-items"), 0);
  EXPECT_EQ(harness->progress.gold, 7);
}

TEST_F(GmFeatureTest, SavedMailUsesFreshRecipientLookupAndNotificationFailureStaysDelivered) {
  harness->catalog = &test_catalog();
  harness->mail_notification_throws = true;
  harness->mail_target =
      GmTarget{nullptr, GmPeerSnapshot{88, 3, 19, true, false, "Keeper", harness->position}};
  harness->run("mail_send me gold 5");

  EXPECT_EQ(harness->trace,
            (std::vector<std::string>{"deliver-mail", "find-mail-target", "notify-mail", "reply"}));
  EXPECT_EQ(harness->target_queries, std::vector<std::int64_t>{19});
  ASSERT_EQ(harness->replies.size(), 1);
  EXPECT_TRUE(harness->replies.front().starts_with("Mail delivered."));
}

TEST_F(GmFeatureTest, UncertainMailCommitNeverLooksUpOrNotifiesTheRecipient) {
  harness->catalog = &test_catalog();
  harness->mail_delivery_throws = true;
  harness->run("mail_send me gold 5");

  EXPECT_EQ(harness->trace, (std::vector<std::string>{"deliver-mail", "reply"}));
  EXPECT_TRUE(harness->target_queries.empty());
  ASSERT_EQ(harness->replies.size(), 1);
  EXPECT_TRUE(harness->replies.front().starts_with("Delivery result is uncertain."));
}

TEST_F(GmFeatureTest, SummonRunsOnTheRetainedTargetsWorkerAndPublishesEncodedPacketsAfterward) {
  std::weak_ptr<SessionHarness> weak_owner = harness;
  const WorldPosition destination{9012, 5000, 6000, 7000, 0};
  harness->session->post_gm_summon(destination, "Summoner");
  EXPECT_TRUE(harness->trace.empty());
  EXPECT_EQ(io.poll(), 0);

  auto *observed = harness.get();
  harness.reset();
  EXPECT_FALSE(weak_owner.expired());
  // Observe before owner release using a second queued operation retaining the same owner.
  auto observer_owner = weak_owner.lock();
  EXPECT_EQ(worker.poll(), 1);
  EXPECT_EQ(observed->trace,
            (std::vector<std::string>{"teleport", "encode", "post-frame", "reply"}));
  EXPECT_EQ(observed->encoded_opcodes, std::vector<std::uint16_t>{33});
  EXPECT_EQ(observed->last_teleport->x_mm, 5000);
  observer_owner.reset();
  EXPECT_TRUE(weak_owner.expired());
}

TEST_F(GmFeatureTest, CoordinateTeleportsLoadTheRegionChannelThatHoldsTheDestination) {
  // A destination in another region's area moves the GM to that region's channel.
  harness->region_map = 9021;
  harness->run("tp -355.9 328.7 -57.9");
  ASSERT_EQ(harness->region_queries.size(), 1U);
  EXPECT_FALSE(harness->region_queries[0].second);
  ASSERT_EQ(harness->map_moves.size(), 1U);
  EXPECT_EQ(harness->map_moves[0].map_id, 9021);
  EXPECT_EQ(harness->map_moves[0].x_mm, -355900);
  EXPECT_FALSE(harness->last_teleport);

  // A destination the current channel owns stays a same-map server teleport.
  harness->region_map.reset();
  harness->run("tp 1 2 3");
  ASSERT_TRUE(harness->last_teleport);
  EXPECT_EQ(harness->last_teleport->map_id, 9012);
  EXPECT_EQ(harness->map_moves.size(), 1U);
}

TEST_F(GmFeatureTest, CoordinateTeleportsFromAHousingAreaLandOnItsField) {
  // Live 2026-11-02: `tp` typed on housing map 20101 loaded an empty housing copy.
  harness->position = WorldPosition{kTestHousingAreaMap, -410000, 3405000, -630000, 0};
  harness->in_housing = true;
  harness->run("tp -769.7 3554.0 -608.6");
  ASSERT_EQ(harness->region_queries.size(), 1U);
  EXPECT_EQ(harness->region_queries[0].first.map_id, 9012);
  ASSERT_EQ(harness->map_moves.size(), 1U);
  EXPECT_EQ(harness->map_moves[0].map_id, 9012);
  EXPECT_EQ(harness->map_moves[0].x_mm, -769700);
  EXPECT_EQ(harness->map_moves[0].y_mm, 3554000);
  EXPECT_FALSE(harness->last_teleport);

  // A relative step out of the area lands on the same field, not on the housing map.
  harness->run("up 5");
  ASSERT_EQ(harness->map_moves.size(), 2U);
  EXPECT_EQ(harness->map_moves[1].map_id, 9012);
  EXPECT_EQ(harness->map_moves[1].z_mm, -625000);

  // A region that owns the destination still picks its own public channel.
  harness->region_map = 9021;
  harness->run("tp -355.9 328.7 -57.9");
  ASSERT_EQ(harness->map_moves.size(), 3U);
  EXPECT_EQ(harness->map_moves[2].map_id, 9021);
}

TEST_F(GmFeatureTest, MapTeleportWithoutCoordinatesKeepsTheNamedPublicChannel) {
  harness->run("tp_map 9021");
  ASSERT_EQ(harness->region_queries.size(), 1U);
  EXPECT_TRUE(harness->region_queries[0].second);
  ASSERT_EQ(harness->map_moves.size(), 1U);
  EXPECT_EQ(harness->map_moves[0].map_id, 9021);
  EXPECT_EQ(harness->map_moves[0].x_mm, 1000);
}

TEST_F(GmFeatureTest, KickRetainsTheSessionUntilItsIoTimerClosesTheConnection) {
  harness->run("kick 0");
  EXPECT_FALSE(harness->closed);
  EXPECT_EQ(worker.poll(), 0);
  std::weak_ptr<SessionHarness> weak_owner = harness;
  harness.reset();
  EXPECT_FALSE(weak_owner.expired());

  EXPECT_EQ(io.run(), 1);
  EXPECT_TRUE(weak_owner.expired());
}

TEST_F(GmFeatureTest, ReplyHandlerRunsOutsideMutexAndItsFailureIsContained) {
  int calls = 0;
  harness->feature.set_reply_handler([&](const ChatIdentity &, const std::string &) {
    EXPECT_TRUE(available_on_another_thread(harness->mutex));
    ++calls;
    throw std::runtime_error("fixture chat failure");
  });

  EXPECT_NO_THROW(harness->session->gm_reply("first"));
  EXPECT_NO_THROW(harness->session->gm_reply("second"));
  EXPECT_EQ(calls, 2);
}

TEST_F(GmFeatureTest, ListedPeersStayAliveThroughEveryReplyAndReleaseAfterTheListCompletes) {
  harness->feature.set_reply_handler([&](const ChatIdentity &, const std::string &text) {
    EXPECT_FALSE(harness->listed_peer.expired());
    EXPECT_TRUE(available_on_another_thread(harness->mutex));
    harness->replies.push_back(text);
    harness->trace.push_back("list-reply");
  });

  harness->run("players");

  EXPECT_EQ(harness->replies, (std::vector<std::string>{"Online characters: 2", "First - map 9012",
                                                        "Second [GM] - map 9012"}));
  EXPECT_EQ(harness->trace, (std::vector<std::string>{"list-reply", "list-reply", "list-reply",
                                                      "release-listed-peer"}));
  EXPECT_TRUE(harness->listed_peer.expired());
}

} // namespace
} // namespace chronicles::gm
