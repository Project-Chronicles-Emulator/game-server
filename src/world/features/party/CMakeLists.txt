# world/features/party module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(world/features/party
  SOURCES
    party.cpp
    party_world.cpp
    party_feature.cpp
    party_replication.cpp
  DEPENDS
    data
    persistence
    world/features/npc
    world/core
  LIBRARIES
    chronicles-messages
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Boost::asio
  DEFINITIONS
    _WIN32_WINNT=0x0A00
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(world/features/party
  SOURCES
    tests/party_tests.cpp
    tests/party_world_tests.cpp
    tests/party_session_tests.cpp
    tests/party_replication_tests.cpp
  MODULES
    protocol
    world/features/npc
    testing
)


# The unchanged full-world test uses the existing application fixture only in this target.
chronicles_add_module_tests(world/features/party
  NAME chronicles-world-features-party-native-tests
  SOURCES
    tests/native_party_integration_tests.cpp
  MODULES
    auth
    protocol
    testing
    world/session
  LIBRARIES
    Boost::asio
    Boost::beast
    OpenSSL::SSL
)
