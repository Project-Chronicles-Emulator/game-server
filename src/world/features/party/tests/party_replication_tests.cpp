#include "testing/membership_replication_test_support.hpp"
#include "world/features/party/party_replication.hpp"

#include <gtest/gtest.h>

namespace chronicles {
namespace {

using Json = nlohmann::json;
using ReplicationContext = testing_support::ReplicationContext<party::PartyReplicationContext>;

TEST(PartyReplication, PacketsThenEachSubjectAndItsCurrentObserversRetainTheOriginalOrder) {
  boost::asio::io_context io;
  party::PartyFeature feature(io, party::PartyWorldConfig{});
  ReplicationContext context;
  context.visible[71] = {74, 73, 74};
  party::PartyUpdate update;
  update.packets = {{77, 181, {{"value", 1}}}, {78, 182, {{"value", 2}}}};
  update.unit_changes = {{71, 17, 9}, {72, 18, 0}};
  feature.replicate_update(update, context);

  EXPECT_EQ(context.calls, (std::vector<std::string>{"packet 77:181", "packet 78:182",
                                                     "packet 71:187", "observers 71", "shared 187",
                                                     "packet 72:187", "observers 72"}));
  ASSERT_EQ(context.shared.size(), 1);
  EXPECT_EQ(context.shared.front().first,
            party::PartyWorld::change_party_unit_message(update.unit_changes[0]));
  // The original presence view is consumed as returned. No sorting or deduplication is added.
  EXPECT_EQ(context.shared.front().second, (std::vector<std::uint64_t>{74, 73, 74}));
}

TEST(PartyReplication, ObserverFailureOccursAfterSubjectQueueAndBeforeLaterUnitChanges) {
  boost::asio::io_context io;
  party::PartyFeature feature(io, party::PartyWorldConfig{});
  ReplicationContext context;
  context.fail_observers = true;
  party::PartyUpdate update;
  update.packets = {{77, 181, Json::object()}};
  update.unit_changes = {{71, 17, 9}, {72, 18, 0}};
  EXPECT_THROW(feature.replicate_update(update, context), std::runtime_error);
  EXPECT_EQ(context.calls,
            (std::vector<std::string>{"packet 77:181", "packet 71:187", "observers 71"}));
  EXPECT_TRUE(context.shared.empty());
}

} // namespace
} // namespace chronicles
