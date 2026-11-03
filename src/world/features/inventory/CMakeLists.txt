# world/features/inventory module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
# Item tempering (ItemPolish) catalog index (docs/inventory-polish-evidence.md), carried-weight
# encumbrance tiers (docs/inventory-weight-evidence.md) and the utility bag (quick item slots
# 1-4) shared by tutorial and shared-world sessions.
chronicles_add_module(world/features/inventory
  SOURCES
    inventory_encumbrance.cpp
    inventory_rules.cpp
    item_polish_catalog.cpp
    utility_bag.cpp
    inventory_capacity.cpp
    inventory_feature.cpp
    inventory_items.cpp
    inventory_bootstrap.cpp
    inventory_session.cpp
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
chronicles_add_module_tests(world/features/inventory
  SOURCES
    tests/inventory_encumbrance_tests.cpp
    tests/inventory_rules_tests.cpp
    tests/item_polish_catalog_tests.cpp
    tests/utility_bag_tests.cpp
    tests/inventory_session_tests.cpp
    tests/inventory_bootstrap_tests.cpp
  MODULES
    data
    protocol
)


# Only these intact native integration tests use the shared full-world fixture.
chronicles_add_module_tests(world/features/inventory
  NAME chronicles-world-features-inventory-native-tests
  SOURCES
    tests/native_inventory_tests.cpp
  MODULES
    auth
    data
    protocol
    testing
    world/features/character
    world/session
  LIBRARIES
    Boost::asio
    Boost::beast
    OpenSSL::SSL
)
