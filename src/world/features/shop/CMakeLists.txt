# world/features/shop module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(world/features/shop
  SOURCES
    item_bundle.cpp
    npc_shop.cpp
    shop_session.cpp
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
chronicles_add_module_tests(world/features/shop
  SOURCES
    tests/native_shop_tests.cpp
    tests/npc_shop_service_tests.cpp
    tests/shop_session_tests.cpp
  MODULES
    auth
    common
    data
    persistence
    protocol
    testing
    world/core
    world/features/character
    world/features/npc
    world/features/processing
    world/session
  LIBRARIES
    Boost::beast
    OpenSSL::SSL
)
