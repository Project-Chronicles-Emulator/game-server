#include "world/features/escape/escape_feature.hpp"

#include "protocol/protocol.hpp"

#include <boost/asio/io_context.hpp>

#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

#include <gtest/gtest.h>

namespace chronicles::escape {
namespace {

class EscapeContextFake final : public EscapeSessionContext,
                                public std::enable_shared_from_this<EscapeContextFake> {
public:
  EscapeContextFake(boost::asio::io_context &io, boost::asio::io_context &worker)
      : io_(io), worker_(worker) {
  }

  bool escape_has_character() const override {
    return authenticated;
  }

  bool escape_closed() const override {
    return is_closed;
  }

  std::uint64_t escape_connection_id() const override {
    return 17;
  }

  std::uint64_t escape_player_unit_id() const override {
    ++identity_reads;
    return 0x1000000000000013ULL;
  }

  std::int64_t escape_now_ticks() const override {
    return now;
  }

  EscapeBlockers escape_blockers() const override {
    return blockers;
  }

  boost::asio::any_io_executor escape_io_executor() override {
    return io_.get_executor();
  }

  boost::asio::any_io_executor escape_worker_executor() override {
    return worker_.get_executor();
  }

  std::shared_ptr<void> retain_escape_owner() override {
    ++retentions;
    return shared_from_this();
  }

  bool finish_escape_revival(std::vector<GameplayPacket> &packets) override {
    trace.push_back("revival");
    if (insert_revival_packet) {
      packets.insert(std::next(packets.begin()), {47, nlohmann::json::object()});
    }
    return revival_succeeds;
  }

  void adopt_escape_position(const WorldPosition &position) override {
    trace.push_back("position");
    adopted_position = position;
  }

  bool escape_god_mode() const override {
    return god_mode;
  }

  GameplayPacket escape_invincibility_packet(bool enabled) const override {
    return {33, {{"attributes", {{"2", enabled ? 1 : 0}}}}};
  }

  void append_encoded_escape_packets(const std::vector<GameplayPacket> &packets,
                                     std::vector<Frame> &frames) override {
    trace.push_back("encode");
    for (const auto &packet : packets) {
      Frame encoded;
      encoded.bytes.push_back(static_cast<std::uint8_t>(packet.opcode));
      frames.push_back(std::move(encoded));
      if (throw_after_partial_encoding) {
        throw std::runtime_error("synthetic partial encoding failure");
      }
    }
  }

  void enqueue_escape_frame(Frame frame) override {
    trace.push_back("enqueue");
    sent.push_back(std::move(frame));
  }

  boost::asio::io_context &io_;
  boost::asio::io_context &worker_;
  bool authenticated{true};
  bool is_closed{};
  bool god_mode{};
  bool revival_succeeds{true};
  bool insert_revival_packet{};
  bool throw_after_partial_encoding{};
  std::int64_t now{100'000'000};
  EscapeBlockers blockers;
  mutable unsigned identity_reads{};
  unsigned retentions{};
  std::optional<WorldPosition> adopted_position;
  std::vector<std::string> trace;
  std::vector<Frame> sent;
  std::unique_ptr<EscapeSession> session;
};

struct EscapeFixture {
  explicit EscapeFixture(EscapeRules rules = short_cooldown())
      : feature(rules), context(std::make_shared<EscapeContextFake>(io, worker)) {
    context->session = feature.create_session(*context, io.get_executor());
  }

  ~EscapeFixture() {
    if (context) {
      context->is_closed = true;
      context->session->on_leave_world();
    }
    io.restart();
    io.poll();
    worker.restart();
    worker.poll();
    io.restart();
    io.poll();
  }

  static EscapeRules short_cooldown() {
    EscapeRules rules;
    rules.cooldown_ticks = 10'000;
    return rules;
  }

  FrameContext frame(std::uint16_t opcode, nlohmann::json message) const {
    FrameContext result(opcode, WorldGameplayRequest{opcode, 1, std::move(message)});
    result.party_position = WorldPosition{8000, 1000, 2000, 3000, 45.0F};
    return result;
  }

  FrameContext start_escape() {
    auto request =
        frame(20048, {{"candidate_pos",
                       {{"millimeter_x", 1100}, {"millimeter_y", 2000}, {"millimeter_z", 3000}}}});
    EXPECT_TRUE(context->session->handle(request));
    return request;
  }

  void complete_start(const FrameContext &request) {
    auto start = frame(20088, {{"tick", request.session_packets.front().message.at("tick")}});
    EXPECT_TRUE(context->session->handle(start));
  }

  boost::asio::io_context io;
  boost::asio::io_context worker;
  EscapeFeature feature;
  std::shared_ptr<EscapeContextFake> context;
};

TEST(EscapeFeature, BroadOwnershipRetainsLazyConstructorFailureOnUnrelatedRequests) {
  auto invalid = EscapeFixture::short_cooldown();
  invalid.candidate_max_distance_mm = 0;
  EscapeFixture fixture(invalid);
  EXPECT_EQ(fixture.context->identity_reads, 0U);

  auto unrelated = fixture.frame(20003, nlohmann::json::object());
  fixture.context->authenticated = false;
  EXPECT_FALSE(fixture.context->session->handle(unrelated));
  fixture.context->authenticated = true;
  EXPECT_THROW(fixture.context->session->handle(unrelated), std::invalid_argument);
}

TEST(EscapeFeature, UnknownAndTradingRequestsFallThroughWithoutReplacingQueuedPackets) {
  EscapeFixture fixture;
  auto unknown = fixture.frame(20003, nlohmann::json::object());
  unknown.session_packets.push_back({999, nlohmann::json::object()});
  EXPECT_FALSE(fixture.context->session->handle(unknown));
  EXPECT_EQ(fixture.context->identity_reads, 1U);
  EXPECT_EQ(unknown.session_packets.front().opcode, 999);

  auto trading = fixture.frame(20048, {{"is_trading", true}});
  EXPECT_FALSE(fixture.context->session->handle(trading));
  EXPECT_FALSE(fixture.context->session->teleport_pending());
}

TEST(EscapeFeature, ShrinePreflightConstructsStateWhilePassiveStatueQueryDoesNot) {
  auto invalid = EscapeFixture::short_cooldown();
  invalid.candidate_max_distance_mm = 0;
  EscapeFixture fixture(invalid);

  EXPECT_FALSE(fixture.context->session->teleport_pending());
  EXPECT_EQ(fixture.context->identity_reads, 0U);
  EXPECT_THROW(fixture.context->session->server_teleport_pending(), std::invalid_argument);
}

TEST(EscapeFeature, AcceptedEscapeReplacesOwnPacketsAndKeepsTheWireHandshakeOrder) {
  EscapeFixture fixture;
  auto request = fixture.frame(
      20048, {{"candidate_pos",
               {{"millimeter_x", 1100}, {"millimeter_y", 2000}, {"millimeter_z", 3000}}}});
  request.session_packets.push_back({999, nlohmann::json::object()});
  ASSERT_TRUE(fixture.context->session->handle(request));
  ASSERT_EQ(request.session_packets.size(), 2U);
  EXPECT_EQ(request.session_packets[0].opcode, 181);
  EXPECT_EQ(request.session_packets[1].opcode, 49);

  fixture.complete_start(request);
  auto complete = fixture.frame(20089, nlohmann::json::object());
  ASSERT_TRUE(fixture.context->session->handle(complete));
  ASSERT_EQ(complete.session_packets.size(), 3U);
  EXPECT_EQ(complete.session_packets[0].opcode, 49);
  EXPECT_EQ(complete.session_packets[1].opcode, 33);
  EXPECT_EQ(complete.session_packets[2].opcode, 51);
  EXPECT_EQ(fixture.context->trace, (std::vector<std::string>{"revival", "position"}));
  ASSERT_TRUE(fixture.context->adopted_position);
  EXPECT_EQ(fixture.context->adopted_position->x_mm, 1100);
}

TEST(EscapeFeature, RevivalInsertionPrecedesPositionAdoptionAndGodOverrideStaysLast) {
  EscapeFixture fixture;
  fixture.context->god_mode = true;
  fixture.context->insert_revival_packet = true;
  const auto request = fixture.start_escape();
  fixture.complete_start(request);
  auto complete = fixture.frame(20089, nlohmann::json::object());
  ASSERT_TRUE(fixture.context->session->handle(complete));
  ASSERT_EQ(complete.session_packets.size(), 5U);
  EXPECT_EQ(complete.session_packets[0].opcode, 49);
  EXPECT_EQ(complete.session_packets[1].opcode, 47);
  EXPECT_EQ(complete.session_packets.back().message.at("attributes").at("2"), 1);
  EXPECT_EQ(fixture.context->trace, (std::vector<std::string>{"revival", "position"}));
}

TEST(EscapeFeature, CaughtShrineFailureSkipsPositionAndDoesNotScheduleAnotherExpiry) {
  EscapeFixture fixture;
  fixture.context->revival_succeeds = false;
  const auto request = fixture.start_escape();
  fixture.complete_start(request);
  const auto previous_retentions = fixture.context->retentions;
  auto complete = fixture.frame(20089, nlohmann::json::object());
  ASSERT_TRUE(fixture.context->session->handle(complete));

  ASSERT_EQ(complete.session_packets.size(), 2U);
  EXPECT_EQ(complete.session_packets.front().opcode, 24);
  EXPECT_EQ(complete.session_packets.front().message.at("ec"), "BadRequest");
  EXPECT_FALSE(fixture.context->adopted_position);
  EXPECT_EQ(fixture.context->retentions, previous_retentions);
}

TEST(EscapeFeature, TimerFiresOnIoThenEncodesOnWorkerAndDeliversOnIo) {
  EscapeFixture fixture;
  fixture.start_escape();
  fixture.context->now += EscapeFixture::short_cooldown().cooldown_ticks;
  fixture.io.run();
  EXPECT_TRUE(fixture.context->sent.empty());
  EXPECT_TRUE(fixture.context->trace.empty());

  fixture.worker.run();
  EXPECT_EQ(fixture.context->trace, (std::vector<std::string>{"encode"}));
  EXPECT_TRUE(fixture.context->sent.empty());

  fixture.io.restart();
  fixture.io.run();
  ASSERT_EQ(fixture.context->sent.size(), 1U);
  EXPECT_EQ(fixture.context->sent.front().bytes.front(), 51);
  EXPECT_EQ(fixture.context->trace, (std::vector<std::string>{"encode", "enqueue"}));
}

TEST(EscapeFeature, EarlyTimerRearmsBeforeTheActualCooldownDeadline) {
  EscapeFixture fixture;
  fixture.start_escape();
  fixture.io.run();
  fixture.worker.run();
  EXPECT_TRUE(fixture.context->sent.empty());

  fixture.context->now += EscapeFixture::short_cooldown().cooldown_ticks;
  fixture.io.restart();
  fixture.io.run();
  fixture.worker.restart();
  fixture.worker.run();
  fixture.io.restart();
  fixture.io.run();
  ASSERT_EQ(fixture.context->sent.size(), 1U);
  EXPECT_EQ(fixture.context->sent.front().bytes.front(), 51);
}

TEST(EscapeFeature, PartialEncodingSurvivesTheNarrowCatchAndIsStillDelivered) {
  EscapeFixture fixture;
  fixture.context->throw_after_partial_encoding = true;
  fixture.start_escape();
  fixture.context->now += EscapeFixture::short_cooldown().cooldown_ticks;
  fixture.io.run();
  EXPECT_NO_THROW(fixture.worker.run());
  fixture.io.restart();
  fixture.io.run();

  ASSERT_EQ(fixture.context->sent.size(), 1U);
  EXPECT_EQ(fixture.context->sent.front().bytes.front(), 51);
}

TEST(EscapeFeature, ClosedSessionSuppressesQueuedExpiryWork) {
  EscapeFixture fixture;
  fixture.start_escape();
  fixture.context->now += EscapeFixture::short_cooldown().cooldown_ticks;
  fixture.io.run();
  fixture.context->is_closed = true;
  fixture.context->session->on_leave_world();
  fixture.worker.run();

  EXPECT_TRUE(fixture.context->trace.empty());
  EXPECT_TRUE(fixture.context->sent.empty());
}

TEST(EscapeFeature, TimerCallbacksRetainContainingSessionUntilCancellationCompletes) {
  auto rules = EscapeFixture::short_cooldown();
  rules.cooldown_ticks = 60 * 10'000'000LL;
  EscapeFixture fixture(rules);
  fixture.start_escape();
  fixture.io.poll();
  std::weak_ptr<EscapeContextFake> weak = fixture.context;
  fixture.context.reset();
  ASSERT_FALSE(weak.expired());

  {
    const auto retained = weak.lock();
    retained->is_closed = true;
    retained->session->on_leave_world();
  }
  fixture.io.restart();
  fixture.io.run();
  EXPECT_TRUE(weak.expired());
}

TEST(EscapeFeature, ServerTeleportUsesTheSameLazyHandshakeWithoutEscapeCooldown) {
  EscapeFixture fixture;
  const WorldPosition destination{8000, 5000, 6000, 7000, 90.0F};
  const auto requested =
      fixture.context->session->request_server_teleport(destination, fixture.context->now);
  ASSERT_EQ(requested.packets.size(), 1U);
  EXPECT_EQ(requested.packets.front().opcode, 181);
  EXPECT_TRUE(fixture.context->session->teleport_pending());

  auto start = fixture.frame(20088, {{"tick", requested.packets.front().message.at("tick")}});
  ASSERT_TRUE(fixture.context->session->handle(start));
  auto complete = fixture.frame(20089, nlohmann::json::object());
  ASSERT_TRUE(fixture.context->session->handle(complete));
  ASSERT_TRUE(fixture.context->adopted_position);
  EXPECT_EQ(fixture.context->adopted_position->x_mm, destination.x_mm);
  fixture.io.run();
  EXPECT_TRUE(fixture.context->sent.empty());
}

} // namespace
} // namespace chronicles::escape
