# world/features/companion module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
# Summoned mounts, vehicles and house mates (docs/vehicles-and-trade.md).
chronicles_add_module(world/features/companion
  SOURCES
    companion_feature.cpp
    companion_session.cpp
    companion_registration.cpp
    companion_world.cpp
  DEPENDS
    common
    data
    persistence
    protocol
    world/core
    world/features/npc
  LIBRARIES
    chronicles-messages
    nlohmann_json::nlohmann_json
    spdlog::spdlog
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(world/features/companion
  NAME chronicles-vehicle-tests
  SOURCES
    tests/companion_tests.cpp
    tests/companion_feature_tests.cpp
  MODULES
    data
    protocol
    testing
    world/core
    world/features/npc
    world/features/trade
)


# Complete owning TLS cases retain the existing opt-in fixture and public application API.
chronicles_add_module_tests(world/features/companion
  NAME chronicles-world-features-companion-native-tests
  SOURCES
    tests/native_companion_tests.cpp
  MODULES
    auth
    data
    protocol
    testing
    world/features/character

    world/features/npc
    world/session
  LIBRARIES
    Boost::asio
    Boost::beast
    OpenSSL::SSL
)
