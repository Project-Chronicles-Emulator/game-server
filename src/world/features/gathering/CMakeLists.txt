# world/features/gathering module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(world/features/gathering
  SOURCES
    gathering.cpp
    gathering_feature.cpp
    gathering_recovery.cpp
  DEPENDS
    common
    persistence
    protocol
    world/core
  LIBRARIES
    Boost::asio
    nlohmann_json::nlohmann_json
    OpenSSL::Crypto
    spdlog::spdlog
  DEFINITIONS
    _WIN32_WINNT=0x0A00
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(world/features/gathering
  SOURCES
    tests/gathering_feature_tests.cpp
    tests/gathering_recovery_tests.cpp
    tests/gathering_tests.cpp
    tests/native_gathering_integration_tests.cpp
    tests/quest_doodad_tests.cpp
  MODULES
    auth
    common
    data
    persistence
    protocol
    testing
    world/core
    world/features/npc
    world/features/processing
    world/session
  LIBRARIES
    Boost::beast
    OpenSSL::SSL
)
