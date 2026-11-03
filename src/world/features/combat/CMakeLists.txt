# world/features/combat module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(world/features/combat
  SOURCES
    combat.cpp
    combat_timeline.cpp
    death_reconciliation.cpp
    combat_feature.cpp
    combat_player_session.cpp
    combat_bootstrap.cpp
    combat_rules.cpp
    combat_world.cpp
    fall_damage.cpp
    gliding.cpp
    movement_vitals.cpp
    player_pvp.cpp
    revival_spawns.cpp
  DEPENDS
    common
    data
    persistence
    protocol
    world/core
    world/features/npc
    world/features/tutorial
    world/features/quests
  LIBRARIES
    OpenSSL::Crypto
    chronicles-messages
    nlohmann_json::nlohmann_json
    spdlog::spdlog
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(world/features/combat
  SOURCES
    tests/combat_feature_tests.cpp
    tests/combat_player_session_tests.cpp
    tests/combat_bootstrap_tests.cpp
    tests/combat_item_tests.cpp
    tests/combat_rules_tests.cpp
    tests/combat_tests.cpp
    tests/combat_timeline_tests.cpp
    tests/death_reconciliation_tests.cpp
    tests/fall_damage_tests.cpp
    tests/gliding_tests.cpp
    tests/movement_vitals_tests.cpp
    tests/player_pvp_tests.cpp
    tests/revival_spawns_tests.cpp
  MODULES
    common
    data
    distribution
    testing
    protocol
    world/features/character
    world/features/npc
    world/session
)


# Complete owning TLS cases keep their original names, assertions and opt-in gates.
chronicles_add_module_tests(world/features/combat
  NAME chronicles-world-features-combat-native-tests
  SOURCES
    tests/native_combat_integration_tests.cpp
    tests/native_reward_region_tests.cpp
    tests/native_grid_copy_tests.cpp
    tests/native_quest_repair_tests.cpp
    tests/native_quest_rewards_tests.cpp
    tests/native_boss_publication_tests.cpp
    tests/native_death_pg_tests.cpp
    tests/native_exp_pg_tests.cpp
  MODULES
    auth
    common
    data
    persistence
    protocol
    testing
    world/core
    world/features/character
    world/features/gathering
    world/features/loot
    world/features/npc
    world/session
  LIBRARIES
    Boost::asio
    Boost::beast
    OpenSSL::SSL
)
