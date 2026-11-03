#include "world/session/gathering_view_identity.hpp"
#include "world/session/boss_recipient_page.hpp"
#include "world/session/boss_supervised_dispatch.hpp"
#include "world/session/close_party_step.hpp"
#include "world/session/retained_retry_gate.hpp"
#include "world/session/local_packet_binding.hpp"
#include "world/session/local_packet_memory.hpp"

#include <gtest/gtest.h>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <stdexcept>
#include <future>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace chronicles {
namespace {

TEST(ClosePartyStep, PartialDeliveryFailureRemainsUnsettledWithoutAutomaticReplay) {
  int detached = 0;
  int delivered = 0;
  const bool settled = try_close_party_step([&] {
    ++detached;
    ++delivered;
    throw std::runtime_error("injected party delivery failure");
  });

  EXPECT_FALSE(settled);
  EXPECT_EQ(detached, 1);
  EXPECT_EQ(delivered, 1);
  EXPECT_TRUE(try_close_party_step([] {}));
}

TEST(BossSupervisedDispatch, FailedExecutorCanBeRetried) {
  boost::asio::any_io_executor invalid_executor;
  int calls = 0;
  int drained = 0;
  const auto result = dispatch_boss_wake(
      [&](auto handler) { boost::asio::post(invalid_executor, std::move(handler)); },
      [&] { ++calls; }, [] {}, [&] { ++drained; });

  EXPECT_EQ(result, RetainedOwnerSubmit::accepted);
  EXPECT_EQ(calls, 0);
  EXPECT_EQ(drained, 1);
}

TEST(BossSupervisedDispatch, ThrowAfterQueuedPostCancelsTheOldHandler) {
  boost::asio::io_context io;
  int calls = 0;
  int drained = 0;
  const auto result = dispatch_boss_wake(
      [&](auto handler) {
        boost::asio::post(io, std::move(handler));
        throw std::runtime_error("injected post failure");
      },
      [&] { ++calls; }, [] {}, [&] { ++drained; });

  EXPECT_EQ(result, RetainedOwnerSubmit::accepted);
  EXPECT_EQ(drained, 0);
  io.run();
  EXPECT_EQ(calls, 0);
  EXPECT_EQ(drained, 1);
}

TEST(BossSupervisedDispatch, StartedHandlerOwnsCompletionDespitePostException) {
  std::atomic<int> calls{};
  std::promise<void> started;
  std::promise<void> release;
  auto released = release.get_future();
  std::thread worker;
  const auto result = dispatch_boss_wake(
      [&](auto handler) {
        worker = std::thread(std::move(handler));
        started.get_future().wait();
        throw std::runtime_error("injected post failure");
      },
      [&] {
        ++calls;
        started.set_value();
        released.wait();
      }, [] {}, [] {});

  EXPECT_EQ(result, RetainedOwnerSubmit::accepted);
  release.set_value();
  worker.join();
  EXPECT_EQ(calls.load(), 1);
}

TEST(BossSupervisedDispatch, WorkAndFailureCallbackExceptionsDoNotEscapeHandler) {
  boost::asio::io_context io;
  int failures = 0;
  const auto result = dispatch_boss_wake(
      [&](auto handler) { boost::asio::post(io, std::move(handler)); },
      [] { throw std::runtime_error("injected worker failure"); },
      [&] {
        ++failures;
        throw std::runtime_error("injected completion failure");
      }, [] {});

  EXPECT_EQ(result, RetainedOwnerSubmit::accepted);
  EXPECT_NO_THROW(io.run());
  EXPECT_EQ(failures, 1);
}

TEST(BossSupervisedDispatch, FailedCompletionKeepsSupervisorOwnerRetained) {
  boost::asio::io_context io;
  std::promise<RetainedOwnerSupervisor::Completion> accepted_completion;
  auto accepted = accepted_completion.get_future();
  RetainedOwnerSupervisor supervisor(
      {},
      [&](RetainedOwnerKey, RetainedOwnerSupervisor::Completion completion) {
        auto result = dispatch_boss_wake(
            [&](auto handler) { boost::asio::post(io, std::move(handler)); },
            [] { throw std::runtime_error("injected worker failure"); },
            [] { throw std::runtime_error("injected completion failure"); }, [] {});
        accepted_completion.set_value(std::move(completion));
        return result;
      },
      [](RetainedOwnerAlarm, RetainedOwnerKey) {});
  ASSERT_TRUE(supervisor.reserve_owner({1, 1}));
  ASSERT_TRUE(supervisor.activate_owner({1, 1}));
  auto completion = accepted.get();

  EXPECT_NO_THROW(io.run());
  EXPECT_EQ(supervisor.health().retained, 1U);
  EXPECT_EQ(supervisor.health().in_flight, 1U);
  EXPECT_FALSE(supervisor.stop());

  completion(RetainedOwnerOutcome::settled);
  EXPECT_TRUE(supervisor.stop());
}

TEST(BossSupervisedDispatch, ExecutorRecoveryRetriesAfterMoreThanThreeFailures) {
  boost::asio::io_context io;
  auto work_guard = boost::asio::make_work_guard(io);
  std::thread io_thread([&] { io.run(); });
  std::atomic<int> attempts{};
  std::atomic<int> applied{};
  RetainedOwnerSupervisor::Options options;
  options.poll_interval = std::chrono::milliseconds(2);
  options.initial_backoff = std::chrono::milliseconds(2);
  options.maximum_backoff = std::chrono::milliseconds(4);
  options.overdue_after = std::chrono::seconds(1);
  options.scheduler_stall_after = std::chrono::seconds(1);
  RetainedOwnerSupervisor supervisor(
      options,
      [&](RetainedOwnerKey, RetainedOwnerSupervisor::Completion completion) {
        auto done = std::make_shared<RetainedOwnerSupervisor::Completion>(std::move(completion));
        return dispatch_boss_wake(
            [&](auto handler) {
              if (++attempts <= 5) {
                boost::asio::any_io_executor invalid;
                boost::asio::post(invalid, std::move(handler));
                return;
              }
              boost::asio::post(io, std::move(handler));
            },
            [&, done] {
              ++applied;
              (*done)(RetainedOwnerOutcome::settled);
            },
            [done] { (*done)(RetainedOwnerOutcome::retry); },
            [done] { (*done)(RetainedOwnerOutcome::retry); });
      },
      [](RetainedOwnerAlarm, RetainedOwnerKey) {});
  ASSERT_TRUE(supervisor.reserve_owner({1, 1}));
  ASSERT_TRUE(supervisor.activate_owner({1, 1}));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (supervisor.health().retained != 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(attempts.load(), 6);
  EXPECT_EQ(applied.load(), 1);
  EXPECT_TRUE(supervisor.stop());
  work_guard.reset();
  io.stop();
  io_thread.join();
}

TEST(BossSupervisedDispatch, QueuedThenThrowWaitsForDrainBeforeRetry) {
  boost::asio::io_context io;
  auto work_guard = boost::asio::make_work_guard(io);
  std::atomic<int> attempts{};
  std::atomic<int> applied{};
  RetainedOwnerSupervisor::Options options;
  options.poll_interval = std::chrono::milliseconds(2);
  options.initial_backoff = std::chrono::milliseconds(2);
  options.maximum_backoff = std::chrono::milliseconds(4);
  options.overdue_after = std::chrono::seconds(1);
  options.scheduler_stall_after = std::chrono::seconds(1);
  RetainedOwnerSupervisor supervisor(
      options,
      [&](RetainedOwnerKey, RetainedOwnerSupervisor::Completion completion) {
        auto done = std::make_shared<RetainedOwnerSupervisor::Completion>(std::move(completion));
        return dispatch_boss_wake(
            [&](auto handler) {
              const int attempt = ++attempts;
              boost::asio::post(io, std::move(handler));
              if (attempt == 1) {
                throw std::runtime_error("injected failure after enqueue");
              }
            },
            [&, done] {
              ++applied;
              (*done)(RetainedOwnerOutcome::settled);
            },
            [done] { (*done)(RetainedOwnerOutcome::retry); },
            [done] { (*done)(RetainedOwnerOutcome::retry); });
      },
      [](RetainedOwnerAlarm, RetainedOwnerKey) {});
  ASSERT_TRUE(supervisor.reserve_owner({1, 1}));
  ASSERT_TRUE(supervisor.activate_owner({1, 1}));
  const auto started_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (attempts.load() == 0 && std::chrono::steady_clock::now() < started_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(attempts.load(), 1);
  EXPECT_EQ(supervisor.health().in_flight, 1U);

  std::thread io_thread([&] { io.run(); });
  const auto settled_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (supervisor.health().retained != 0 && std::chrono::steady_clock::now() < settled_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(attempts.load(), 2);
  EXPECT_EQ(applied.load(), 1);
  EXPECT_TRUE(supervisor.stop());
  work_guard.reset();
  io.stop();
  io_thread.join();
}

TEST(BossRecipientPage, PagesAllExistingConnectionsWithoutIncludingLaterArrivals) {
  std::unordered_map<std::uint64_t, int> sessions;
  for (std::uint64_t connection = 1; connection <= 4097; ++connection) {
    sessions.emplace(connection, 0);
  }
  const auto first = select_boss_recipient_page(sessions, 0, 4097, 4096);
  ASSERT_EQ(first.connection_ids.size(), 4096U);
  EXPECT_EQ(first.connection_ids.front(), 1U);
  EXPECT_EQ(first.connection_ids.back(), 4096U);
  EXPECT_FALSE(first.last);

  const auto second = select_boss_recipient_page(sessions, first.end, 4097, 4096);
  ASSERT_EQ(second.connection_ids.size(), 1U);
  EXPECT_EQ(second.connection_ids.front(), 4097U);
  EXPECT_TRUE(second.last);

  sessions.erase(4097);
  sessions.emplace(4098, 0);
  const auto last = select_boss_recipient_page(sessions, first.end, 4097, 4096);
  EXPECT_TRUE(last.connection_ids.empty());
  EXPECT_TRUE(last.last);
  EXPECT_EQ(last.end, 4097U);
}

TEST(BossRecipientPage, ExactBoundaryAndInvalidLimitsAreExplicit) {
  std::unordered_map<std::uint64_t, int> sessions;
  for (std::uint64_t connection = 1; connection <= 4096; ++connection) {
    sessions.emplace(connection, 0);
  }
  const auto page = select_boss_recipient_page(sessions, 0, 4096, 4096);
  EXPECT_EQ(page.connection_ids.size(), 4096U);
  EXPECT_TRUE(page.last);
  EXPECT_THROW(select_boss_recipient_page(sessions, 0, 4096, 0), std::invalid_argument);
  EXPECT_THROW(select_boss_recipient_page(sessions, 5, 4, 1), std::invalid_argument);
}

TEST(BossRecipientPage, AdaptsToByteAndFrameLimitsWithoutDroppingARecipient) {
  using gathering::ConstructionBudget;
  EXPECT_EQ(boss_recipient_page_limit(1024, 1, ConstructionBudget::kSectorBytes), 4096U);
  const auto limit = boss_recipient_page_limit(24 * 1024 * 1024, 1,
                                                ConstructionBudget::kSectorBytes);
  EXPECT_EQ(limit, 2U);
  std::unordered_map<std::uint64_t, int> sessions{{3, 0}, {1, 0}, {2, 0}};
  const auto first = select_boss_recipient_page(sessions, 0, 3, limit);
  EXPECT_EQ(first.connection_ids, (std::vector<std::uint64_t>{1, 2}));
  const auto last = select_boss_recipient_page(sessions, first.end, 3, limit);
  EXPECT_EQ(last.connection_ids, (std::vector<std::uint64_t>{3}));
  EXPECT_TRUE(last.last);
  EXPECT_THROW(boss_recipient_page_limit(65 * 1024 * 1024, 1,
                                        ConstructionBudget::kSectorBytes),
               BossPublicationOversize);
}

TEST(BossDeliveryCompletion, UnexpectedLiveCompletionFencesPublication) {
  Frame frame;
  frame.bytes = {4, 0, 1, 0};
  RetainedFrameDelivery delivery("event", {{1, {1, 30104, 0}, {frame}}});
  BossDeliveryCompletion completion;
  const auto attempt = delivery.begin(0);
  ASSERT_TRUE(attempt);
  completion.finish(delivery, *attempt, FrameEnqueueStatus::inserted, true);
  EXPECT_TRUE(completion.complete(delivery));
  completion.finish(delivery, *attempt, FrameEnqueueStatus::inserted, true);
  EXPECT_TRUE(completion.invariant_failed());
  EXPECT_FALSE(completion.complete(delivery));
}

TEST(BossDeliveryCompletion, LateCompletionAfterDepartureDoesNotFencePublication) {
  Frame frame;
  frame.bytes = {4, 0, 1, 0};
  RetainedFrameDelivery delivery("event", {{1, {1, 30104, 0}, {frame}}});
  BossDeliveryCompletion completion;
  const auto attempt = delivery.begin(0);
  ASSERT_TRUE(attempt);
  ASSERT_TRUE(delivery.confirm_departed(0));
  completion.finish(delivery, *attempt, FrameEnqueueStatus::cancelled, false);
  EXPECT_FALSE(completion.invariant_failed());
  EXPECT_TRUE(completion.complete(delivery));
}

TEST(BossDeliveryCompletion, ReportedClosedRequiresActualViewDeparture) {
  Frame frame;
  frame.bytes = {4, 0, 1, 0};
  RetainedFrameDelivery delivery("event", {{1, {1, 30104, 0}, {frame}}});
  BossDeliveryCompletion completion;
  const auto attempt = delivery.begin(0);
  ASSERT_TRUE(attempt);
  completion.finish(delivery, *attempt, FrameEnqueueStatus::closed, true);
  EXPECT_TRUE(delivery.close_required(0));
  EXPECT_FALSE(completion.complete(delivery));
  ASSERT_TRUE(delivery.confirm_departed(0));
  EXPECT_TRUE(completion.complete(delivery));
}

TEST(RetainedRetryGate, FailedSchedulingAllowsRetryAndStaleCallbacksCannotClearIt) {
  RetainedRetryGate gate;
  const auto first = gate.arm();
  ASSERT_TRUE(first);
  EXPECT_FALSE(gate.arm());
  ASSERT_TRUE(gate.finish(*first));

  const auto second = gate.arm();
  ASSERT_TRUE(second);
  EXPECT_NE(*first, *second);
  EXPECT_FALSE(gate.is_active(*first));
  EXPECT_FALSE(gate.finish(*first));
  EXPECT_TRUE(gate.is_active(*second));
  EXPECT_TRUE(gate.finish(*second));
  EXPECT_FALSE(gate.armed());
}

class ProbePacketPlan final : public LocalPacketPlan {
public:
  struct Lock final {
    explicit Lock(bool &held) : held_(held) {
      held_ = true;
    }
    ~Lock() {
      held_ = false;
    }
    bool &held_;
  };

  int expected_map() const noexcept override {
    return 9012;
  }

  std::uint64_t expected_channel() const noexcept override {
    return 7;
  }

  std::size_t retained_bytes() const noexcept override {
    return sizeof(*this);
  }

  std::shared_ptr<void> begin_ordered_delivery() const override {
    return std::make_shared<Lock>(held);
  }
  std::optional<std::uint32_t> source_sector() const noexcept override {
    return 17;
  }

  std::optional<std::uint32_t> initial_sector() const noexcept override {
    return 17;
  }

  LocalPacketDecision prepare(nlohmann::json &message) const override {
    message["revision"] = revision;
    return LocalPacketDecision::replace;
  }

  mutable bool held{};
  int revision{1};
};

TEST(LocalPacketBinding, EncodingRunsOutsideSourceLockAndRevalidatesBeforeQueue) {
  auto state = std::make_shared<GatheringViewState>();
  state->publish(9012, 7);
  const auto identity = state->snapshot();
  state->synchronize_sectors(identity, {17});
  const auto generation = state->sector_generation(identity, 17);
  ASSERT_TRUE(generation);
  auto plan = std::make_shared<ProbePacketPlan>();
  Frame frame;
  frame.bytes = {4, 0, 1, 0};
  frame.local_plan = plan;
  frame.local_message = std::make_shared<const nlohmann::json>(nlohmann::json::object());
  bind_local_gathering_packet(
      frame, identity,
      [state](const auto &view) {
        return state->matches(view);
      },
      [plan](std::uint16_t, const nlohmann::json &) {
        EXPECT_FALSE(plan->held);
        plan->revision = 2;
        Frame replacement;
        replacement.bytes = {4, 0, 1, 0};
        return replacement;
      },
      [state, identity, generation] {
        return state->acquire_queue_guard(identity, 17, generation, false, state);
      });
  EXPECT_FALSE(frame.prepare_for_enqueue(frame));
  EXPECT_FALSE(plan->held);
  EXPECT_FALSE(state->sector_ready(identity, 17));
}

TEST(LocalPacketMemory, RejectsExcessiveNestingAndOversizedStrings) {
  nlohmann::json nested = nullptr;
  for (std::size_t depth = 0; depth <= kMaximumLocalPacketDepth; ++depth) {
    nested = nlohmann::json::array({std::move(nested)});
  }
  std::size_t total = 0;
  EXPECT_THROW(charge_local_json(nested, 0, total), std::runtime_error);

  nlohmann::json large = std::string(kMaximumLocalPacketRetainedBytes, 'x');
  total = 0;
  EXPECT_THROW(charge_local_json(large, 0, total), std::runtime_error);
}

TEST(GatheringViewState, LeaveAndReturnCannotReviveAnOldFrame) {
  GatheringViewState view;
  view.publish(9012, 7);
  const auto original = view.snapshot();
  ASSERT_TRUE(view.matches(original));
  view.synchronize_sectors(original, {17});
  const auto generation = view.sector_generation(original, 17);
  ASSERT_TRUE(generation);
  ASSERT_TRUE(view.mark_sector_ready(original, 17, *generation));
  EXPECT_TRUE(view.sector_ready(original, 17));

  view.invalidate();
  EXPECT_FALSE(view.matches(original));
  view.publish(9012, 7);
  const auto returned = view.snapshot();
  EXPECT_FALSE(view.matches(original));
  EXPECT_TRUE(view.matches(returned));
  EXPECT_FALSE(view.sector_ready(returned, 17));
  EXPECT_FALSE(view.mark_sector_ready(original, 17, *generation));
  EXPECT_GT(returned.epoch, original.epoch);
}

TEST(GatheringViewState, FailureCloseClaimCannotSealAReenteredView) {
  GatheringViewState view;
  view.publish(9012, 7);
  const auto original = view.snapshot();
  view.invalidate();
  view.publish(9012, 7);
  const auto reentered = view.snapshot();
  ASSERT_NE(original, reentered);
  EXPECT_FALSE(view.seal_for_close(original));
  EXPECT_TRUE(view.acquire_queue_guard(reentered, std::nullopt, std::nullopt, false, {}));

  EXPECT_TRUE(view.seal_for_close(reentered));
  EXPECT_FALSE(view.acquire_queue_guard(reentered, std::nullopt, std::nullopt, false, {}));
  EXPECT_THROW(view.publish(10111, 8), std::runtime_error);
  EXPECT_THROW(view.invalidate(), std::runtime_error);
  EXPECT_NO_THROW(view.invalidate_for_close());
  EXPECT_FALSE(view.snapshot().map_id);
}

TEST(GatheringViewState, SightExitInvalidatesLateSnapshotAndBoundsLongTravel) {
  GatheringViewState view;
  view.publish(9012, 7);
  const auto identity = view.snapshot();
  view.synchronize_sectors(identity, {17});
  const auto first = view.sector_generation(identity, 17);
  ASSERT_TRUE(first);
  ASSERT_TRUE(view.mark_sector_ready(identity, 17, *first));
  view.synchronize_sectors(identity, {18});
  EXPECT_FALSE(view.sector_ready(identity, 17));
  view.synchronize_sectors(identity, {17});
  const auto returned = view.sector_generation(identity, 17);
  ASSERT_TRUE(returned);
  EXPECT_GT(*returned, *first);
  EXPECT_FALSE(view.mark_sector_ready(identity, 17, *first));
  EXPECT_FALSE(view.sector_matches(identity, 17, *first, true));
  EXPECT_FALSE(view.sector_matches(identity, 17, *returned, true));
  EXPECT_TRUE(view.mark_sector_ready(identity, 17, *returned));
  EXPECT_TRUE(view.sector_matches(identity, 17, *returned, true));

  for (std::uint32_t sector = 0; sector < 5000; ++sector) {
    view.synchronize_sectors(identity, {sector});
    const auto current = view.sector_generation(identity, sector);
    ASSERT_TRUE(current);
    ASSERT_TRUE(view.mark_sector_ready(identity, sector, *current));
  }
}

TEST(GatheringViewState, MapAndCopyPublishAsOneIdentity) {
  GatheringViewState view;
  view.publish(9012, 7);
  const auto first = view.snapshot();
  view.publish(9012, 8);
  const auto second = view.snapshot();
  EXPECT_FALSE(view.matches(first));
  EXPECT_TRUE(view.matches(second));
  EXPECT_EQ(second.map_id, 9012);
  EXPECT_EQ(second.copy_channel, 8U);

  view.publish(std::nullopt, 123);
  const auto unavailable = view.snapshot();
  EXPECT_FALSE(view.matches(unavailable));
  EXPECT_EQ(unavailable.copy_channel, 0U);
}

TEST(GatheringViewState, WholeMapReadinessIsInvalidatedByDeparture) {
  auto view = std::make_shared<GatheringViewState>();
  view->publish(9012, 7);
  const auto identity = view->snapshot();
  {
    auto guard = view->acquire_queue_guard(identity, std::nullopt, std::nullopt, false, view);
    ASSERT_TRUE(guard);
    guard->mark_all_ready();
  }
  EXPECT_TRUE(view->sector_ready(identity, 17));
  const auto generation = view->sector_generation(identity, 17);
  ASSERT_TRUE(generation);
  EXPECT_EQ(*generation, 0U);
  EXPECT_TRUE(view->sector_matches(identity, 17, *generation, true));
  view->invalidate();
  view->publish(9012, 7);
  EXPECT_FALSE(view->sector_matches(identity, 17, *generation, true));
  EXPECT_FALSE(view->sector_ready(view->snapshot(), 17));
}

} // namespace
} // namespace chronicles
