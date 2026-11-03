# world/features/events module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
# Boss leaderboards, raid channel matchmaking and channel events
# (docs/rankings-raids-channel-events.md).
chronicles_add_module(world/features/events
  SOURCES
    channel_event.cpp
    event_gm_commands.cpp
    events_feature.cpp
    events_session.cpp
    preorder_bonus.cpp
    raid_channel.cpp
    ranking.cpp
    ranking_storage.cpp
  DEPENDS
    common
    data
    persistence
    protocol
    world/core
    world/features/gm
    world/features/party
  LIBRARIES
    Boost::asio
    libpqxx::pqxx
    nlohmann_json::nlohmann_json
    spdlog::spdlog
  DEFINITIONS
    _WIN32_WINNT=0x0A00
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(world/features/events
  NAME chronicles-event-tests
  SOURCES
    tests/channel_event_tests.cpp
    tests/event_protocol_tests.cpp
    tests/events_session_tests.cpp
    tests/native_events_integration_tests.cpp
    tests/raid_channel_tests.cpp
    tests/ranking_tests.cpp
  MODULES
    auth
    common
    data
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

if(BUILD_TESTING)
  target_compile_definitions(chronicles-event-tests PRIVATE
    EVENTS_TEST_FIXTURE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/test-fixtures"
  )
endif()
