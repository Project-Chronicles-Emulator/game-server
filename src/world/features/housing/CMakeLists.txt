# world/features/housing module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
# Housing sites, guild housing areas and move packages (docs/housing.md). The front WebSocket
# dispatcher (services/front/native_ws.cpp) serves the WSC2F housing requests; guild_housing.cpp
# adds the guild directory and request link.
chronicles_add_module(world/features/housing
  SOURCES
    guild_housing.cpp
    housing_catalog.cpp
    housing_doodads.cpp
    housing_guild_service.cpp
    housing_service.cpp
    housing_storage.cpp
    housing_world.cpp
    native_housing.cpp
    housing_bootstrap.cpp
    housing_feature.cpp
    housing_session.cpp
  DEPENDS
    auth
    data
    net
    persistence
    world/features/chat
    world/features/company
    world/features/guild
    world/core
  LIBRARIES
    OpenSSL::Crypto
    chronicles-messages
    libpqxx::pqxx
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Boost::asio
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(world/features/housing
  NAME chronicles-housing-tests
  SOURCES
    tests/guild_housing_tests.cpp
    tests/housing_catalog_tests.cpp
    tests/housing_doodads_tests.cpp
    tests/housing_service_tests.cpp
    tests/housing_storage_tests.cpp
    tests/housing_world_tests.cpp
    tests/native_housing_tests.cpp
    tests/housing_session_tests.cpp
  MODULES
    auth
    data
    persistence
    services/front
    testing
    world/features/chat
    world/features/guild
  LIBRARIES
    libpqxx::pqxx
)


# This unchanged TLS definition alone needs the full-world application fixture.
chronicles_add_module_tests(world/features/housing
  NAME chronicles-world-features-housing-native-tests
  SOURCES
    tests/native_housing_session_tests.cpp
  MODULES
    auth
    data
    protocol
    testing
    world/session
  LIBRARIES
    Boost::asio
    Boost::beast
    OpenSSL::SSL
)
