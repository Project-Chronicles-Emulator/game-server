# world/features/character module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
# Character levels, build points, attribute bonuses and perks (progression.hpp).
chronicles_add_module(world/features/character
  SOURCES
    achievements.cpp
    character_bootstrap.cpp
    character_feature.cpp
    character_session.cpp
    character_stats.cpp
    graduation_grant.cpp
    progression_edit.cpp
    progression.cpp
  DEPENDS
    common
    data
    persistence
    protocol
    world/core
    world/features/shop
  LIBRARIES
    chronicles-messages
    nlohmann_json::nlohmann_json
    spdlog::spdlog
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(world/features/character
  SOURCES
    tests/achievement_tests.cpp
    tests/character_feature_tests.cpp
    tests/character_stats_tests.cpp
    tests/graduation_grant_tests.cpp
    tests/graduation_owner_tests.cpp
    tests/progression_tests.cpp
  MODULES
    auth
    protocol
    testing
    world/features/gm
    world/features/inventory
    world/features/quests
    world/features/tutorial
    world/session
  LIBRARIES
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
)
