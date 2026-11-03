# world/features/tutorial module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(world/features/tutorial
  SOURCES
    tutorial.cpp
    tutorial_feature.cpp
    tutorial_player_session.cpp
  DEPENDS
    common
    persistence
    protocol
    world/features/character
    world/features/inventory
    world/features/quests
    auth
    world/core
    world/features/mail
  LIBRARIES
    chronicles-messages
    nlohmann_json::nlohmann_json
    spdlog::spdlog
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(world/features/tutorial
  SOURCES
    tests/tutorial_interaction_tests.cpp
    tests/tutorial_tests.cpp
    tests/tutorial_player_session_tests.cpp
  MODULES
    common
    protocol
    testing
    world/core
)

# These unchanged TLS definitions use the accepted full-world fixture only in this target.
chronicles_add_module_tests(world/features/tutorial
  NAME chronicles-world-features-tutorial-native-tests
  SOURCES
    tests/native_tutorial_session_tests.cpp
    tests/native_tutorial_activity_tests.cpp
  MODULES
    auth
    common
    data
    persistence
    protocol
    testing
    world/core
    world/features/mail
    world/features/npc
    world/features/processing
    world/features/quests
    world/session
  LIBRARIES
    Boost::asio
    Boost::beast
    OpenSSL::SSL
    spdlog::spdlog
)
