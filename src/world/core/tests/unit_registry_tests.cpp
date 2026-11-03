#include "world/core/unit_registry.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace chronicles {
namespace {

AnnouncedUnit make_unit(std::uint64_t unit_id, std::string instance_key = "instance-a",
                        int map_id = 8000) {
    AnnouncedUnit unit;
    unit.unit_id = unit_id;
    unit.npc_template_id = 1234;
    unit.map_id = map_id;
    unit.instance_key = std::move(instance_key);
    unit.position = {-795100, 712230, -698700};
    return unit;
}

TEST(UnitRegistry, ParsesOnlyStrictNonZeroDecimalUnitIds) {
    EXPECT_EQ(parse_unit_id_text("123"), 123u);
    EXPECT_EQ(parse_unit_id_text("9223372036854775807"), 9223372036854775807ull);

    for (const char *rejected : {"", "0", "-1", "+1", " 1", "1 ", "1.5", "abc", "0x10",
                                 "9223372036854775808", "18446744073709551615",
                                 "99999999999999999999"}) {
        EXPECT_FALSE(parse_unit_id_text(rejected).has_value()) << rejected;
    }
}

TEST(UnitRegistry, ResolvesAnnouncedUnitWithTheServersOwnValues) {
    UnitRegistry registry(4);
    ASSERT_EQ(registry.announce(make_unit(5001)), UnitAnnounceResult::Added);

    const auto *unit = registry.find(5001);
    ASSERT_NE(unit, nullptr);
    EXPECT_EQ(unit->unit_id, 5001u);
    EXPECT_EQ(unit->npc_template_id, 1234);
    EXPECT_EQ(unit->map_id, 8000);
    EXPECT_EQ(unit->instance_key, "instance-a");
    EXPECT_EQ(unit->position.x_mm, -795100);
    EXPECT_EQ(unit->position.y_mm, 712230);
    EXPECT_EQ(unit->position.z_mm, -698700);
    EXPECT_EQ(registry.size(), 1u);
}

TEST(UnitRegistry, NeverAnnouncedOrForgedIdsDoNotResolve) {
    UnitRegistry registry(4);
    ASSERT_EQ(registry.announce(make_unit(5001)), UnitAnnounceResult::Added);

    for (const std::uint64_t forged :
         {std::uint64_t{0}, std::uint64_t{5000}, std::uint64_t{5002},
          std::numeric_limits<std::uint64_t>::max()}) {
        EXPECT_EQ(registry.find(forged), nullptr) << forged;
        EXPECT_EQ(registry.find_in_instance(forged, 8000, "instance-a"), nullptr) << forged;
    }
}

TEST(UnitRegistry, RejectsInvalidAnnouncementsWithoutRecordingThem) {
    UnitRegistry registry(4);
    EXPECT_EQ(registry.announce(make_unit(0)), UnitAnnounceResult::InvalidUnit);
    EXPECT_EQ(registry.announce(make_unit(std::uint64_t{1} << 63)),
              UnitAnnounceResult::InvalidUnit);

    auto no_template = make_unit(1);
    no_template.npc_template_id = 0;
    EXPECT_EQ(registry.announce(no_template), UnitAnnounceResult::InvalidUnit);

    auto no_map = make_unit(2);
    no_map.map_id = 0;
    EXPECT_EQ(registry.announce(no_map), UnitAnnounceResult::InvalidUnit);

    EXPECT_EQ(registry.announce(make_unit(3, "")), UnitAnnounceResult::InvalidUnit);
    EXPECT_EQ(registry.size(), 0u);
}

TEST(UnitRegistry, DuplicateAnnouncementIsRejectedAndKeepsTheOriginal) {
    UnitRegistry registry(4);
    ASSERT_EQ(registry.announce(make_unit(5001)), UnitAnnounceResult::Added);

    auto moved = make_unit(5001);
    moved.position = {1, 2, 3};
    EXPECT_EQ(registry.announce(moved), UnitAnnounceResult::AlreadyAnnounced);

    const auto *unit = registry.find(5001);
    ASSERT_NE(unit, nullptr);
    EXPECT_EQ(unit->position.x_mm, -795100);
    EXPECT_EQ(registry.size(), 1u);
}

TEST(UnitRegistry, DespawnRemovesTheUnitAndUnknownDespawnIsReported) {
    UnitRegistry registry(4);
    ASSERT_EQ(registry.announce(make_unit(5001)), UnitAnnounceResult::Added);
    ASSERT_EQ(registry.announce(make_unit(5002)), UnitAnnounceResult::Added);

    EXPECT_TRUE(registry.despawn(5001));
    EXPECT_EQ(registry.find(5001), nullptr);
    EXPECT_NE(registry.find(5002), nullptr);
    EXPECT_FALSE(registry.despawn(5001));
    EXPECT_FALSE(registry.despawn(9999));
    EXPECT_EQ(registry.size(), 1u);

    // A despawned id is not announced any more, so it can be announced again.
    EXPECT_EQ(registry.announce(make_unit(5001)), UnitAnnounceResult::Added);
}

TEST(UnitRegistry, CapacityIsBoundedWithAnExplicitErrorAndFreedByDespawnOrClear) {
    UnitRegistry registry(2);
    ASSERT_EQ(registry.capacity(), 2u);
    ASSERT_EQ(registry.announce(make_unit(1)), UnitAnnounceResult::Added);
    ASSERT_EQ(registry.announce(make_unit(2)), UnitAnnounceResult::Added);

    EXPECT_EQ(registry.announce(make_unit(3)), UnitAnnounceResult::CapacityExceeded);
    EXPECT_EQ(registry.find(3), nullptr);
    EXPECT_EQ(registry.size(), 2u);

    ASSERT_TRUE(registry.despawn(1));
    EXPECT_EQ(registry.announce(make_unit(3)), UnitAnnounceResult::Added);
    EXPECT_EQ(registry.announce(make_unit(4)), UnitAnnounceResult::CapacityExceeded);

    registry.clear();
    EXPECT_EQ(registry.size(), 0u);
    EXPECT_EQ(registry.find(2), nullptr);
    EXPECT_EQ(registry.announce(make_unit(4)), UnitAnnounceResult::Added);
}

TEST(UnitRegistry, ZeroCapacityIsRejected) {
    EXPECT_THROW(UnitRegistry(0), std::invalid_argument);
}

TEST(UnitRegistry, InstanceAndMapAreSeparatedByFindInInstance) {
    UnitRegistry registry(4);
    ASSERT_EQ(registry.announce(make_unit(5001, "instance-a", 8000)), UnitAnnounceResult::Added);

    EXPECT_NE(registry.find_in_instance(5001, 8000, "instance-a"), nullptr);
    EXPECT_EQ(registry.find_in_instance(5001, 8000, "instance-b"), nullptr);
    EXPECT_EQ(registry.find_in_instance(5001, 8001, "instance-a"), nullptr);
    EXPECT_EQ(registry.find_in_instance(5002, 8000, "instance-a"), nullptr);
    // The unfiltered lookup still finds it: the caller decides which instance it is in.
    EXPECT_NE(registry.find(5001), nullptr);
}

TEST(UnitRegistry, DistanceHelpersReturnRawMillimetersWithoutOverflow) {
    const UnitPositionMm origin{0, 0, 0};
    EXPECT_DOUBLE_EQ(horizontal_distance_mm(origin, {3, 4, 999}), 5.0);
    EXPECT_DOUBLE_EQ(distance_mm(origin, {2, 3, 6}), 7.0);
    EXPECT_DOUBLE_EQ(distance_mm({5, 5, 5}, {5, 5, 5}), 0.0);

    constexpr int kMin = std::numeric_limits<int>::min();
    constexpr int kMax = std::numeric_limits<int>::max();
    EXPECT_DOUBLE_EQ(distance_mm({kMin, 0, 0}, {kMax, 0, 0}), 4294967295.0);
    EXPECT_DOUBLE_EQ(horizontal_distance_mm({0, kMax, 0}, {0, kMin, 0}), 4294967295.0);
}

} // namespace
} // namespace chronicles
