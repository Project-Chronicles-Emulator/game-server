set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(DESCRIPTORS "${PROJECT_SOURCE_DIR}/protocol/descriptors.normalized.pb")

execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/generate_protos.py"
    --descriptor "${DESCRIPTORS}" --list
  OUTPUT_VARIABLE PROTO_FILES
  OUTPUT_STRIP_TRAILING_WHITESPACE
  COMMAND_ERROR_IS_FATAL ANY
)

set(GENERATED_SOURCES)
set(GENERATED_HEADERS)
foreach(PROTO IN LISTS PROTO_FILES)
  string(REGEX REPLACE "\\.proto$" ".pb.cc" CC "${PROTO}")
  string(REGEX REPLACE "\\.proto$" ".pb.h" HH "${PROTO}")
  list(APPEND GENERATED_SOURCES "${GENERATED_DIR}/${CC}")
  list(APPEND GENERATED_HEADERS "${GENERATED_DIR}/${HH}")
endforeach()

add_custom_command(
  OUTPUT ${GENERATED_SOURCES} ${GENERATED_HEADERS}
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/generate_protos.py"
    --descriptor "${DESCRIPTORS}"
    --protoc "$<TARGET_FILE:protobuf::protoc>"
    --output "${GENERATED_DIR}"
  DEPENDS
    "${DESCRIPTORS}"
    "${PROJECT_SOURCE_DIR}/tools/generate_protos.py"
    protobuf::protoc
  VERBATIM
)

add_library(chronicles-messages STATIC ${GENERATED_SOURCES} ${GENERATED_HEADERS})
target_include_directories(chronicles-messages PUBLIC "${GENERATED_DIR}")
target_link_libraries(chronicles-messages PUBLIC protobuf::libprotobuf)
if(MSVC)
  target_compile_options(chronicles-messages PRIVATE /wd4251 /wd4996)
endif()

# Module folders are listed lowest layer first (tools/source_layout.json). Every module
# target exposes ${PROJECT_SOURCE_DIR}/src, so includes stay relative to src/. The apps/
# folder holds the server executables over these libraries; the install rules below it
# package them.
add_subdirectory(common)
add_subdirectory(distribution)
add_subdirectory(protocol)
add_subdirectory(data)
add_subdirectory(persistence)
add_subdirectory(auth)
add_subdirectory(net)
add_subdirectory(world/core)
add_subdirectory(world/features/character)
add_subdirectory(world/features/chat)
add_subdirectory(world/features/combat)
add_subdirectory(world/features/companion)
add_subdirectory(world/features/company)
add_subdirectory(world/features/escape)
add_subdirectory(world/features/events)
add_subdirectory(world/features/expression)
add_subdirectory(world/features/fog)
add_subdirectory(world/features/friends)
add_subdirectory(world/features/gathering)
add_subdirectory(world/features/guides)
add_subdirectory(world/features/gm)
add_subdirectory(world/features/guild)
add_subdirectory(world/features/hotkeys)
add_subdirectory(world/features/housing)
add_subdirectory(world/features/inventory)
add_subdirectory(world/features/item_use)
add_subdirectory(world/features/loot)
add_subdirectory(world/features/mail)
add_subdirectory(world/features/market)
add_subdirectory(world/features/npc)
add_subdirectory(world/features/nuia)
add_subdirectory(world/features/party)
add_subdirectory(world/features/processing)
add_subdirectory(world/features/quests)
add_subdirectory(world/features/shop)
add_subdirectory(world/features/trade)
add_subdirectory(world/features/tutorial)
add_subdirectory(world/features/adventure)
add_subdirectory(world/features/travel)
add_subdirectory(world/session)
add_subdirectory(services/admin)
add_subdirectory(services/agent_engine)
add_subdirectory(services/discord)
add_subdirectory(services/front)
add_subdirectory(services/news)
add_subdirectory(services/portal)
add_subdirectory(services/portal_storage)
add_subdirectory(services/social)
# chronicles-testing links GTest, which is only found when tests are configured.
if(BUILD_TESTING)
  add_subdirectory(testing)
endif()
add_subdirectory(apps)

install(TARGETS chronicles-server chronicles-account chronicles-news chronicles-content
  RUNTIME DESTINATION . COMPONENT Backend)
install(FILES $<TARGET_RUNTIME_DLLS:chronicles-server> $<TARGET_RUNTIME_DLLS:chronicles-account>
  DESTINATION . COMPONENT Backend)
# vcpkg app-local deployment includes DLLs imported indirectly (notably LIBPQ/OpenSSL).
# They are not all represented by TARGET_RUNTIME_DLLS in the imported package metadata.
install(DIRECTORY "$<TARGET_FILE_DIR:chronicles-server>/"
  DESTINATION . COMPONENT Backend FILES_MATCHING PATTERN "*.dll" PATTERN "gtest*" EXCLUDE
  PATTERN "launcher" EXCLUDE PATTERN "launcher-operator" EXCLUDE PATTERN "admin" EXCLUDE)
install(FILES "${PROJECT_SOURCE_DIR}/protocol/descriptors.normalized.pb"
  "${PROJECT_SOURCE_DIR}/protocol/opcodes.json"
  "${PROJECT_SOURCE_DIR}/protocol/native-ws-opcodes.json"
  DESTINATION protocol COMPONENT Backend)
install(FILES "${PROJECT_SOURCE_DIR}/migrations/001_accounts_characters.sql"
  DESTINATION migrations COMPONENT Backend)
install(FILES "${PROJECT_SOURCE_DIR}/tools/install_database.ps1"
  DESTINATION tools COMPONENT Backend)
install(FILES "${PROJECT_SOURCE_DIR}/config/services/game.example.json"
  DESTINATION config COMPONENT Backend)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/config/distribution/"
  DESTINATION config COMPONENT Backend)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/config/agent-engine/"
  DESTINATION agent-engine COMPONENT Backend)
install(FILES "${PROJECT_SOURCE_DIR}/docs/distribution-backend.md"
  DESTINATION . RENAME README.md COMPONENT Backend)

if(BUILD_TESTING)
  # Each module defines its own test program (phase 1b-2); this aggregate target keeps
  # the documented "cmake --build --preset release --target chronicles-tests" working.
  add_custom_target(chronicles-tests DEPENDS
    chronicles-agent-engine-tests
    chronicles-admin-tests
    chronicles-auth-flow-tests
    chronicles-currency-tests
    chronicles-data-tests
    chronicles-discord-tests
    chronicles-distribution-tests
    chronicles-event-tests
    chronicles-exploration-tests
    chronicles-fog-tests
    chronicles-housing-tests
    chronicles-market-tests
    chronicles-news-tests
    chronicles-net-tests
    chronicles-persistence-tests
    chronicles-portal-tests
    chronicles-protocol-tests
    chronicles-roster-tests
    chronicles-services-front-tests
    chronicles-vehicle-tests
    chronicles-world-core-tests
    chronicles-world-features-character-tests
    chronicles-world-features-chat-tests
    chronicles-world-features-combat-tests
    chronicles-world-features-company-tests
    chronicles-world-features-escape-tests
    chronicles-world-features-expression-tests
    chronicles-world-features-friends-tests
    chronicles-world-features-gathering-tests
    chronicles-world-features-gm-tests
    chronicles-world-features-guild-tests
    chronicles-world-features-inventory-tests
    chronicles-world-features-item_use-tests
    chronicles-world-features-loot-tests
    chronicles-world-features-mail-tests
    chronicles-world-features-npc-tests
    chronicles-world-features-nuia-tests
    chronicles-world-features-party-tests
    chronicles-world-features-processing-tests
    chronicles-world-features-quests-tests
    chronicles-world-features-shop-tests
    chronicles-world-features-tutorial-tests
    chronicles-world-session-tests
  )
endif()
