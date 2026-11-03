# world/features/escape module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
# Player escape and its server teleport handshake (docs/player-escape.md).
chronicles_add_module(world/features/escape
  SOURCES
    escape_feature.cpp
    player_escape.cpp
  DEPENDS
    common
    data
    persistence
    protocol
    world/core
  LIBRARIES
    Boost::asio
    nlohmann_json::nlohmann_json
    spdlog::spdlog
  DEFINITIONS
    _WIN32_WINNT=0x0A00
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(world/features/escape
  SOURCES
    tests/escape_feature_tests.cpp
    tests/native_escape_tests.cpp
    tests/player_escape_tests.cpp
  MODULES
    auth
    data
    net
    protocol
    testing
    world/core
    world/features/character
    world/features/fog
    world/features/housing
    world/features/mail
    world/features/market
    world/features/npc
    world/features/processing
    world/session
  LIBRARIES
    Boost::asio
    Boost::beast
    OpenSSL::SSL
)
