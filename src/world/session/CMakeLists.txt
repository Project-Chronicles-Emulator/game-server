# world/session module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(world/session
  SOURCES
    frame_pipeline.cpp
    graduation_grant.cpp
    local_packet_binding.cpp
    native_world.cpp
  DEPENDS
    auth
    common
    data
    net
    persistence
    protocol
    world/core
    world/features/adventure
    world/features/character
    world/features/chat
    world/features/combat
    world/features/companion
    world/features/company
    world/features/escape
    world/features/events
    world/features/expression
    world/features/fog
    world/features/gathering
    world/features/guides
    world/features/gm
    world/features/guild
    world/features/housing
    world/features/inventory
    world/features/item_use
    world/features/loot
    world/features/mail
    world/features/market
    world/features/npc
    world/features/nuia
    world/features/party
    world/features/processing
    world/features/quests
    world/features/shop
    world/features/trade
    world/features/tutorial
    world/features/travel
  LIBRARIES
    Boost::asio
    chronicles-messages
    nlohmann_json::nlohmann_json
    spdlog::spdlog
  DEFINITIONS
    _WIN32_WINNT=0x0A00
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(world/session
  SOURCES
    tests/combat_input_admission_tests.cpp
    tests/frame_pipeline_tests.cpp
    tests/gameplay_tick_gate_tests.cpp
    tests/gathering_view_identity_tests.cpp
    tests/session_event_composition_tests.cpp
    tests/native_world_tests.cpp
    tests/native_company_dialogue_tests.cpp
  MODULES
    auth
    common
    data
    persistence
    testing
    world/core
    world/features/character
    world/features/combat
    world/features/company
    world/features/events
    world/features/expression
    world/features/fog
    world/features/gathering
    world/features/gm
    world/features/guild
    world/features/housing
    world/features/inventory
    world/features/loot
    world/features/mail
    world/features/market
    world/features/npc
    world/features/nuia
    world/features/party
    world/features/processing
    world/features/quests
  LIBRARIES
    Boost::asio
    Boost::beast
    OpenSSL::Crypto
    spdlog::spdlog
)

if(BUILD_TESTING)
  # Compile the shutdown fault only into this isolated copy, never into the server library.
  get_target_property(session_dependencies chronicles-world-session LINK_LIBRARIES)
  add_library(chronicles-world-session-shutdown-fault STATIC
    frame_pipeline.cpp
    graduation_grant.cpp
    local_packet_binding.cpp
    native_world.cpp
  )
  target_include_directories(chronicles-world-session-shutdown-fault
    PUBLIC "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(chronicles-world-session-shutdown-fault PUBLIC ${session_dependencies})
  target_compile_definitions(chronicles-world-session-shutdown-fault
    PUBLIC _WIN32_WINNT=0x0A00
    PRIVATE CHRONICLES_TEST_SHUTDOWN_START_FAULT CHRONICLES_TEST_COMPANION_DELIVERY_FAULT
  )
  chronicles_use_precompiled_headers(chronicles-world-session-shutdown-fault)

  add_executable(chronicles-world-shutdown-fault-tests tests/native_shutdown_fault_tests.cpp)
  target_link_libraries(chronicles-world-shutdown-fault-tests PRIVATE
    chronicles-world-session-shutdown-fault
    chronicles-testing
    GTest::gtest_main
  )
  target_compile_definitions(chronicles-world-shutdown-fault-tests PRIVATE
    CHRONICLES_PROJECT_ROOT="${PROJECT_SOURCE_DIR}"
    CHRONICLES_PROTOCOL_DIR="${PROJECT_SOURCE_DIR}/protocol"
    CHRONICLES_MIGRATION_FILE="${PROJECT_SOURCE_DIR}/migrations/001_accounts_characters.sql"
  )
  chronicles_use_test_precompiled_headers(chronicles-world-shutdown-fault-tests)
  gtest_discover_tests(chronicles-world-shutdown-fault-tests)

  add_executable(chronicles-world-companion-fault-tests
    tests/native_companion_fault_tests.cpp
    tests/native_quest_supervision_tests.cpp
    tests/native_chat_fence_tests.cpp
  )
  target_link_libraries(chronicles-world-companion-fault-tests PRIVATE
    chronicles-world-session-shutdown-fault
    chronicles-testing
    GTest::gtest_main
  )
  target_compile_definitions(chronicles-world-companion-fault-tests PRIVATE
    CHRONICLES_PROJECT_ROOT="${PROJECT_SOURCE_DIR}"
    CHRONICLES_PROTOCOL_DIR="${PROJECT_SOURCE_DIR}/protocol"
    CHRONICLES_MIGRATION_FILE="${PROJECT_SOURCE_DIR}/migrations/001_accounts_characters.sql"
  )
  chronicles_use_test_precompiled_headers(chronicles-world-companion-fault-tests)
  gtest_discover_tests(chronicles-world-companion-fault-tests)
endif()
