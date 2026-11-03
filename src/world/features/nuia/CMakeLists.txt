# world/features/nuia module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(world/features/nuia
  SOURCES
    nui_statue.cpp
    nuia_tears.cpp
    nuia_feature.cpp
    nuia_session.cpp
  DEPENDS
    common
    persistence
    protocol
    world/features/inventory
    world/features/quests
    world/core
  LIBRARIES
    nlohmann_json::nlohmann_json
    spdlog::spdlog
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(world/features/nuia
  SOURCES
    tests/nui_statue_tests.cpp
    tests/nuia_tears_tests.cpp
    tests/nuia_session_tests.cpp
  MODULES
    protocol
    testing
)


# Only the intact owning TLS cases depend on the full-world application fixture.
chronicles_add_module_tests(world/features/nuia
  NAME chronicles-world-features-nuia-native-tests
  SOURCES
    tests/native_nuia_tests.cpp
  MODULES
    auth
    data
    protocol
    testing
    world/features/character
    world/session
  LIBRARIES
    Boost::asio
    Boost::beast
    OpenSSL::SSL
)
