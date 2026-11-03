# world/features/guild module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(world/features/guild
  SOURCES
    guild.cpp
    guild_storage.cpp
    guild_world.cpp
    native_guild.cpp
    guild_feature.cpp
    guild_replication.cpp
  DEPENDS
    auth
    common
    data
    net
    persistence
    world/core
    world/features/chat
    world/features/company
    world/features/npc
  LIBRARIES
    chronicles-messages
    libpqxx::pqxx
    nlohmann_json::nlohmann_json
    spdlog::spdlog
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(world/features/guild
  SOURCES
    tests/guild_tests.cpp
    tests/guild_session_tests.cpp
    tests/guild_replication_tests.cpp
  MODULES
    data
    protocol
    services/front
    testing
    world/features/chat
    world/features/company
    world/features/npc
)


# The unchanged full-world test uses the existing application fixture only in this target.
chronicles_add_module_tests(world/features/guild
  NAME chronicles-world-features-guild-native-tests
  SOURCES
    tests/native_guild_integration_tests.cpp
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
