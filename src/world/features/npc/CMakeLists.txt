# world/features/npc module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(world/features/npc
  SOURCES
    map_static_npcs.cpp
    npc_ai.cpp
    npc_interactions.cpp
    npc_patrol.cpp
    npc_population.cpp
    npc_sight.cpp
    npc_feature.cpp
    npc_reactions.cpp
  DEPENDS
    common
    data
    persistence
    protocol
    world/core
    world/features/tutorial
  LIBRARIES
    nlohmann_json::nlohmann_json
    spdlog::spdlog
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(world/features/npc
  SOURCES
    tests/map_static_npcs_tests.cpp
    tests/npc_ai_tests.cpp
    tests/npc_interactions_tests.cpp
    tests/npc_patrol_tests.cpp
    tests/npc_population_tests.cpp
    tests/npc_sight_tests.cpp
    tests/npc_feature_tests.cpp
  MODULES
    data
    protocol
    world/core
)

# These unchanged TLS definitions use the accepted full-world fixture only in this target.
chronicles_add_module_tests(world/features/npc
  NAME chronicles-world-features-npc-native-tests
  SOURCES
    tests/native_npc_session_tests.cpp
  MODULES
    auth
    common
    data
    persistence
    protocol
    testing
    world/core
    world/features/processing
    world/features/shop
    world/session
  LIBRARIES
    Boost::asio
    Boost::beast
    OpenSSL::SSL
    spdlog::spdlog
)
