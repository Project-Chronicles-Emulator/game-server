#include "testing/membership_replication_test_support.hpp"
#include "world/features/guild/guild_replication.hpp"

#include <gtest/gtest.h>

namespace chronicles {
namespace {

using Json = nlohmann::json;
using ReplicationContext = testing_support::ReplicationContext<guild::GuildReplicationContext>;

std::unique_ptr<guild::GuildFeature> guild_feature() {
  GuildRules rules;
  rules.companies_enabled = false;
  auto service = std::make_shared<GuildService>(std::make_shared<MemoryGuildStore>(), rules, 1);
  auto feature = std::make_unique<guild::GuildFeature>(std::move(service), nullptr);
  feature->world()->attach(GuildActor{.world_id = 1, .char_id = 17, .name = "First"}, 71);
  feature->world()->attach(GuildActor{.world_id = 1, .char_id = 18, .name = "Second"}, 72);
  return feature;
}

TEST(GuildReplication, ResolvesOnlyOnlineCharactersAndSharesEachLabelAfterTheSubject) {
  auto feature = guild_feature();
  ReplicationContext context;
  context.visible[71] = {73, 74};
  guild_world::GuildWorldUpdate update;
  update.packets = {{99, 219, Json::object()}, {18, 220, {{"value", 7}}}};
  update.unit_changes = {{99, 9, "Absent"}, {17, 9, "Present"}, {18, 0, {}}};
  feature->replicate_update(update, context);

  EXPECT_EQ(context.calls,
            (std::vector<std::string>{"packet 72:220", "packet 71:218", "observers 71",
                                      "shared 218", "packet 72:218", "observers 72"}));
  ASSERT_EQ(context.shared.size(), 1);
  EXPECT_EQ(context.shared[0].first, GuildWorld::change_guild_unit_message(update.unit_changes[1]));
  EXPECT_EQ(context.shared[0].second, (std::vector<std::uint64_t>{73, 74}));
}

TEST(GuildReplication, CharacterResolutionRemainsInterleavedRatherThanPrecomputedAsABatch) {
  auto feature = guild_feature();
  ReplicationContext context;
  context.on_packet = [&](std::uint64_t, std::uint16_t) {
    feature->world()->detach(18, 72);
  };
  guild_world::GuildWorldUpdate update;
  update.packets = {{17, 219, Json::object()}, {18, 220, Json::object()}};
  update.unit_changes = {{18, 9, "Detached"}};
  feature->replicate_update(update, context);
  EXPECT_EQ(context.calls, (std::vector<std::string>{"packet 71:219"}));
}

TEST(GuildReplication, DisabledDirectoryProducesNoLookupQueueOrObserverEffects) {
  guild::GuildFeature feature(nullptr, nullptr);
  ReplicationContext context;
  guild_world::GuildWorldUpdate update;
  update.packets = {{17, 219, Json::object()}};
  update.unit_changes = {{17, 9, "Absent"}};
  feature.replicate_update(update, context);
  EXPECT_TRUE(context.calls.empty());
}

} // namespace
} // namespace chronicles
