# world/features/loot module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(world/features/loot
  SOURCES
    loot.cpp
    loot_rules.cpp
    loot_feature.cpp
  DEPENDS
    auth
    common
    distribution
    persistence
    world/core
    world/features/combat
    world/features/tutorial
    world/features/party
  LIBRARIES
    chronicles-messages
    nlohmann_json::nlohmann_json
    spdlog::spdlog
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(world/features/loot
  SOURCES
    tests/loot_custom_tests.cpp
    tests/loot_tests.cpp
    tests/loot_feature_tests.cpp
  MODULES
    testing
    world/features/party
)
