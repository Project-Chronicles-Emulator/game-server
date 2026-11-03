# Server executables: thin mains over the module libraries. Sources are relative to src/apps/
# and each program links the modules its sources include (verified by the deps mode of
# out/modular-plan/scaffold_module_cmake.py); transitive usage requirements carry the rest.
# The install rules that package these programs stay in the parent CMakeLists.txt.

add_executable(chronicles-replay replay_main.cpp replay_transport.cpp)
target_link_libraries(chronicles-replay PRIVATE chronicles-protocol Boost::asio)
if(WIN32)
  target_link_libraries(chronicles-replay PRIVATE ws2_32 mswsock)
  target_compile_definitions(chronicles-replay PRIVATE _WIN32_WINNT=0x0A00)
endif()

add_executable(chronicles-news news_main.cpp)
target_link_libraries(chronicles-news PRIVATE chronicles-services-news)

add_executable(chronicles-content content_main.cpp)
target_link_libraries(chronicles-content PRIVATE chronicles-services-portal_storage)

add_executable(chronicles-server
  server.cpp repositories.cpp postgres_server_lease.cpp auth_heartbeat.cpp)

target_link_libraries(chronicles-server PRIVATE
  chronicles-auth
  chronicles-common
  chronicles-data
  chronicles-distribution
  chronicles-net
  chronicles-services-admin
  chronicles-services-agent_engine
  chronicles-services-discord
  chronicles-services-front
  chronicles-services-news
  chronicles-services-portal_storage
  chronicles-services-social
  chronicles-world-core
  chronicles-world-session
  chronicles-world-features-chat
  chronicles-world-features-company
  chronicles-world-features-events
  chronicles-world-features-fog
  chronicles-world-features-friends
  chronicles-world-features-gathering
  chronicles-world-features-guild
  chronicles-world-features-hotkeys
  chronicles-world-features-housing
  chronicles-world-features-loot
  chronicles-world-features-mail
  chronicles-world-features-market
  chronicles-world-features-npc
  chronicles-world-features-processing
  PostgreSQL::PostgreSQL
  OpenSSL::Crypto
)
target_compile_definitions(chronicles-server PRIVATE
  CHRONICLES_DEFAULT_PROTOCOL_DIR="${PROJECT_SOURCE_DIR}/protocol"
  CHRONICLES_DEFAULT_AGENT_ENGINE_DIR="${PROJECT_SOURCE_DIR}/config/agent-engine"
)
if(CHRONICLES_REQUIRE_SEPARATE_LOGIN)
  target_compile_definitions(chronicles-server PRIVATE CHRONICLES_REQUIRE_SEPARATE_LOGIN)
endif()
if(WIN32)
  target_link_libraries(chronicles-server PRIVATE ws2_32 mswsock)
endif()

add_executable(chronicles-account account_main.cpp)
target_link_libraries(chronicles-account PRIVATE chronicles-auth)

if(BUILD_TESTING)
  # The fault trigger is compiled only into this isolated process-test executable.
  add_executable(chronicles-server-quest-scheduler-test
    server.cpp repositories.cpp postgres_server_lease.cpp auth_heartbeat.cpp
  )
  get_target_property(server_libraries chronicles-server LINK_LIBRARIES)
  get_target_property(server_definitions chronicles-server COMPILE_DEFINITIONS)
  target_link_libraries(chronicles-server-quest-scheduler-test PRIVATE ${server_libraries})
  target_compile_definitions(chronicles-server-quest-scheduler-test PRIVATE
    ${server_definitions} CHRONICLES_TEST_QUEST_SCHEDULER_FAULT
  )
  chronicles_use_precompiled_headers(chronicles-server-quest-scheduler-test)

  add_executable(chronicles-startup-recovery-tests
    tests/startup_reward_recovery_tests.cpp
    tests/quest_effect_timer_policy_tests.cpp
  )
  target_include_directories(chronicles-startup-recovery-tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(chronicles-startup-recovery-tests PRIVATE GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(chronicles-startup-recovery-tests)
endif()

foreach(target
  chronicles-server chronicles-account chronicles-news chronicles-content
  chronicles-replay)
  chronicles_process_resources(${target} server "Project Chronicles Server Tools")
endforeach()

# Targets that link nlohmann/json and spdlog through the module libraries reuse a
# precompiled header of common standard and third-party includes
# (cmake/ChroniclesPrecompiledHeaders.cmake).
chronicles_use_precompiled_headers(
  chronicles-server
  chronicles-account
)
