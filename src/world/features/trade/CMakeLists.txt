# world/features/trade module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
# Trade runs and the trade session (docs/vehicles-and-trade.md).
chronicles_add_module(world/features/trade
  SOURCES
    trade_feature.cpp
    trade_run.cpp
    trade_session.cpp
  DEPENDS
    common
    data
    persistence
    protocol
    world/core
    world/features/companion
    world/features/npc
  LIBRARIES
    chronicles-messages
    nlohmann_json::nlohmann_json
    spdlog::spdlog
  PRECOMPILED_HEADERS
)


chronicles_add_module_tests(world/features/trade
  SOURCES
    tests/trade_tests.cpp
    tests/trade_feature_tests.cpp
  MODULES
    data
    persistence
    protocol
    testing
    world/core
    world/features/companion
    world/features/npc
)


# Complete owning TLS cases retain the existing opt-in fixture and public application API.
chronicles_add_module_tests(world/features/trade
  NAME chronicles-world-features-trade-native-tests
  SOURCES
    tests/native_trade_tests.cpp
  MODULES
    auth
    data
    protocol
    testing
    world/features/character
    world/features/companion
    world/features/npc
    world/session
  LIBRARIES
    Boost::asio
    Boost::beast
    OpenSSL::SSL
)
