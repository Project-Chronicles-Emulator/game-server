# world/features/item_use module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(world/features/item_use
  SOURCES
    item_use.cpp
    item_use_feature.cpp
  DEPENDS
    common
    data
    persistence
    protocol
    world/core
    world/features/inventory
    world/features/loot
  LIBRARIES
    chronicles-messages
    nlohmann_json::nlohmann_json
    spdlog::spdlog
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(world/features/item_use
  SOURCES
    tests/item_use_feature_tests.cpp
    tests/item_use_tests.cpp
  MODULES
    testing
)
