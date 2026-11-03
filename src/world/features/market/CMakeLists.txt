# world/features/market module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(world/features/market
  SOURCES
    market.cpp
    market_session.cpp
    market_storage.cpp
  DEPENDS
    auth
    common
    data
    persistence
    protocol
    world/core
  LIBRARIES
    chronicles-messages
    libpqxx::pqxx
    nlohmann_json::nlohmann_json
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(world/features/market
  NAME chronicles-market-tests
  SOURCES
    tests/market_session_tests.cpp
    tests/market_tests.cpp
  MODULES
    auth
    common
    data
    persistence
    protocol
    testing
    world/core
  LIBRARIES
    libpqxx::pqxx
)
