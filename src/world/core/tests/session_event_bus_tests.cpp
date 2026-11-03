#include "world/core/session_event_bus.hpp"

#include <memory>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace chronicles {
namespace {

TEST(SessionEventBus, DeliversOnlyMatchingSubscribersInCompositionOrder) {
  std::vector<std::string> order;
  SessionEventBus::Subscriptions subscriptions;
  subscriptions.visibility.push_back([&order](const PlayerVisibilityChanged &event) {
    EXPECT_EQ(event.connection_id, 17U);
    order.push_back("presence");
  });
  subscriptions.visibility.push_back([&order](const PlayerVisibilityChanged &) {
    order.push_back("second");
  });
  subscriptions.protection.push_back([&order](const PlayerProtectionChanged &) {
    order.push_back("combat");
  });
  SessionEventBus bus(std::move(subscriptions));

  bus.publish(PlayerVisibilityChanged{17, true});
  EXPECT_EQ(order, (std::vector<std::string>{"presence", "second"}));
}

TEST(SessionEventBus, NestedPublicationFinishesSynchronouslyBeforeTheNextSubscriber) {
  std::vector<std::string> order;
  const SessionEventBus *active_bus = nullptr;
  SessionEventBus::Subscriptions subscriptions;
  subscriptions.visibility.push_back([&](const PlayerVisibilityChanged &) {
    order.push_back("outer-before");
    active_bus->publish(PlayerProtectionChanged{true, true, false});
    order.push_back("outer-after");
  });
  subscriptions.visibility.push_back([&](const PlayerVisibilityChanged &) {
    order.push_back("outer-second");
  });
  subscriptions.protection.push_back([&](const PlayerProtectionChanged &) {
    order.push_back("nested");
  });
  SessionEventBus bus(std::move(subscriptions));
  active_bus = &bus;

  bus.publish(PlayerVisibilityChanged{});
  EXPECT_EQ(order,
            (std::vector<std::string>{"outer-before", "nested", "outer-after", "outer-second"}));
}

TEST(SessionEventBus, PropagatesOriginalFailureAndStopsRemainingSubscribers) {
  bool later_called = false;
  SessionEventBus::Subscriptions subscriptions;
  subscriptions.player_state.push_back([](const PlayerCombatSnapshotChanged &) {
    throw std::runtime_error("original cache failure");
  });
  subscriptions.player_state.push_back([&](const PlayerCombatSnapshotChanged &) {
    later_called = true;
  });
  SessionEventBus bus(std::move(subscriptions));

  try {
    bus.publish(PlayerCombatSnapshotChanged{});
    FAIL() << "Expected the original exception";
  } catch (const std::runtime_error &error) {
    EXPECT_STREQ(error.what(), "original cache failure");
  }
  EXPECT_FALSE(later_called);
}

struct SubscriberProbe {
  explicit SubscriberProbe(int &copies) : copies_(copies) {
  }

  SubscriberProbe(const SubscriberProbe &other) : copies_(other.copies_) {
    ++copies_;
  }

  void operator()(const PlayerProtectionChanged &) const {
  }

  int &copies_;
};

TEST(SessionEventBus, PublishDoesNotCopySubscriberClosures) {
  int copies = 0;
  SessionEventBus::Subscriptions subscriptions;
  subscriptions.protection.push_back(SubscriberProbe(copies));
  SessionEventBus bus(std::move(subscriptions));
  const auto setup_copies = copies;

  for (int index = 0; index < 100; ++index) {
    bus.publish(PlayerProtectionChanged{});
  }
  EXPECT_EQ(copies, setup_copies);
}

TEST(SessionEventBus, OwnsSubscriberClosureUntilTheContainingBusIsDestroyed) {
  std::weak_ptr<int> weak;
  {
    auto owned = std::make_shared<int>(0);
    weak = owned;
    SessionEventBus::Subscriptions subscriptions;
    subscriptions.company.push_back([owned](const CompanyMembershipChanged &) {
      ++*owned;
    });
    SessionEventBus bus(std::move(subscriptions));
    owned.reset();

    ASSERT_FALSE(weak.expired());
    bus.publish(CompanyMembershipChanged{});
    bus.publish(CompanyMembershipChanged{});
    EXPECT_EQ(*weak.lock(), 2);
  }
  EXPECT_TRUE(weak.expired());
}

TEST(SessionEventBus, SubscriberResolvesRebuiltStateAtDeliveryTime) {
  int first = 0;
  int replacement = 0;
  int *current = &first;
  SessionEventBus::Subscriptions subscriptions;
  subscriptions.player_state.push_back([&current](const PlayerCombatSnapshotChanged &) {
    ++*current;
  });
  SessionEventBus bus(std::move(subscriptions));

  bus.publish(PlayerCombatSnapshotChanged{});
  current = &replacement;
  bus.publish(PlayerCombatSnapshotChanged{});
  EXPECT_EQ(first, 1);
  EXPECT_EQ(replacement, 1);
}

TEST(SessionEventBus, RejectsAnEmptySubscriberDuringComposition) {
  SessionEventBus::Subscriptions subscriptions;
  subscriptions.protection.emplace_back();
  EXPECT_THROW(SessionEventBus(std::move(subscriptions)), std::invalid_argument);
}

} // namespace
} // namespace chronicles
