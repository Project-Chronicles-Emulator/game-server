#include "world/session/frame_pipeline.hpp"

#include "common/world_metrics.hpp"
#include "persistence/character_state_types.hpp"

#include <gtest/gtest.h>

#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace chronicles {
namespace {

SessionEventObservers neutral_observers() {
  SessionEventObservers observers;
  observers.protection = [](const PlayerProtectionChanged &) {
  };
  observers.visibility = [](const PlayerVisibilityChanged &) {
  };
  observers.combat_snapshot = [](const PlayerCombatSnapshotChanged &) {
  };
  observers.company_membership = [](const CompanyMembershipChanged &) {
  };
  observers.progress_committed = [](const PlayerProgressCommitted &) {
  };
  return observers;
}

TEST(SessionEventComposition, NeutralEffectsResolveCurrentTargetsAndAdoptBorrowedProgressInOrder) {
  struct CurrentTargets {
    std::int64_t guild_revision{};
    WorldProgress tutorial;
  };
  CurrentTargets first;
  CurrentTargets second;
  CurrentTargets *current = &first;
  std::vector<std::string> order;
  const WorldProgress *publishing = nullptr;
  auto observers = neutral_observers();
  observers.company_membership = [&](const CompanyMembershipChanged &) {
    ++current->guild_revision;
    order.push_back("guild-refresh");
  };
  observers.progress_committed = [&](const PlayerProgressCommitted &event) {
    EXPECT_EQ(&event.progress, publishing);
    current->tutorial = event.progress;
    order.push_back("tutorial-adopt");
  };
  auto bus = compose_session_events(std::move(observers));
  EXPECT_TRUE(order.empty());

  WorldProgress progress;
  progress.revision = 7;
  publishing = &progress;
  bus.publish(CompanyMembershipChanged{});
  bus.publish(PlayerProgressCommitted{progress});
  EXPECT_EQ(order, (std::vector<std::string>{"guild-refresh", "tutorial-adopt"}));
  EXPECT_EQ(first.guild_revision, 1);
  EXPECT_EQ(first.tutorial.revision, 7);

  current = &second;
  progress.revision = 8;
  order.clear();
  bus.publish(CompanyMembershipChanged{});
  bus.publish(PlayerProgressCommitted{progress});
  EXPECT_EQ(order, (std::vector<std::string>{"guild-refresh", "tutorial-adopt"}));
  EXPECT_EQ(second.guild_revision, 1);
  EXPECT_EQ(second.tutorial.revision, 8);
  EXPECT_EQ(first.tutorial.revision, 7);
}

TEST(SessionEventComposition, ProgressionCacheCopiesTheBorrowedBuildUnderTheCallersExistingLock) {
  world_metrics::TimedMutex mutex;
  CharacterBuild owned_cache;
  CharacterBuild expected;
  const CharacterBuild *publishing = nullptr;
  auto observers = neutral_observers();
  observers.progression_snapshot = [&](const PlayerProgressionSnapshotChanged &event) {
    EXPECT_EQ(&event.build, publishing);
    EXPECT_EQ(event.level, 42);
    EXPECT_EQ(event.experience, 99);
    EXPECT_FALSE(std::async(std::launch::async, [&] {
                   if (!mutex.try_lock()) {
                     return false;
                   }
                   mutex.unlock();
                   return true;
                 }).get());
    owned_cache = event.build;
  };
  auto bus = compose_session_events(std::move(observers));
  {
    CharacterBuild borrowed;
    borrowed.stat_bonuses.emplace(1, 7);
    borrowed.perks.emplace(9001, 2);
    expected = borrowed;
    publishing = &borrowed;
    {
      std::lock_guard lock(mutex);
      bus.publish(PlayerProgressionSnapshotChanged{42, 99, borrowed});
    }
    borrowed.stat_bonuses.at(1) = 50;
    EXPECT_EQ(owned_cache, expected);
  }
  EXPECT_EQ(owned_cache, expected);
}

SessionEventObservers lifetime_observers(std::vector<std::string> &trace) {
  auto observers = neutral_observers();
  observers.progress_committed = [&trace](const PlayerProgressCommitted &event) {
    EXPECT_EQ(event.progress.revision, 9);
    trace.push_back("final-progress");
  };
  return observers;
}

struct RetainedBusConsumer {
  RetainedBusConsumer(SessionEventBus &events, std::vector<std::string> &trace)
      : events(events), trace(trace) {
    trace.push_back("consumer-construct");
  }

  ~RetainedBusConsumer() {
    trace.push_back("consumer-release");
    WorldProgress final_progress;
    final_progress.revision = 9;
    events.publish(PlayerProgressCommitted{final_progress});
  }

  SessionEventBus &events;
  std::vector<std::string> &trace;
};

TEST(SessionEventComposition,
     BusOutlivesItsConsumerWithoutInvokingSinksDuringConstructionOrDestruction) {
  struct ContainingSession {
    explicit ContainingSession(std::vector<std::string> &trace)
        : events(compose_session_events(lifetime_observers(trace))), consumer(events, trace) {
    }

    SessionEventBus events;
    RetainedBusConsumer consumer;
  };
  std::vector<std::string> trace;
  {
    ContainingSession containing(trace);
    EXPECT_EQ(trace, (std::vector<std::string>{"consumer-construct"}));
  }
  EXPECT_EQ(trace,
            (std::vector<std::string>{"consumer-construct", "consumer-release", "final-progress"}));
}

TEST(SessionEventComposition, MissingRequiredSessionEffectFailsBeforeAConsumerCanUseTheBus) {
  auto observers = neutral_observers();
  observers.company_membership = {};
  EXPECT_THROW(compose_session_events(std::move(observers)), std::invalid_argument);
}

} // namespace
} // namespace chronicles
