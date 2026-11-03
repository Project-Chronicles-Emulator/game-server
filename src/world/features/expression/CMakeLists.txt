# world/features/expression module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
# Emotes, emotion ownership and selfie mode (docs/emotes-and-selfie.md).
chronicles_add_module(world/features/expression
  SOURCES
    expression_feature.cpp
    player_expression.cpp
  DEPENDS
    common
    data
    persistence
    protocol
    world/core
  LIBRARIES
    chronicles-messages
    nlohmann_json::nlohmann_json
    spdlog::spdlog
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(world/features/expression
  SOURCES
    tests/expression_feature_tests.cpp
    tests/native_expression_tests.cpp
    tests/player_expression_tests.cpp
  MODULES
    auth
    data
    net
    persistence
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
