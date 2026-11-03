# world/features/gm module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(world/features/gm
  SOURCES
    gm_commands.cpp
    gm_progression_edit.cpp
    gm_diagnostics.cpp
    gm_feature.cpp
    gm_tools.cpp
    gm_world_commands.cpp
    social_gm_commands.cpp
  DEPENDS
    auth
    common
    data
    persistence
    protocol
    world/core
    world/features/character
    world/features/combat
    world/features/company
    world/features/mail
    world/features/npc
    world/features/quests
    world/features/shop
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
chronicles_add_module_tests(world/features/gm
  SOURCES
    tests/game_master_tests.cpp
    tests/gm_diagnostics_tests.cpp
    tests/gm_feature_tests.cpp
    tests/gm_session_ports_tests.cpp
    tests/native_gm_tests.cpp
    tests/gm_tools_tests.cpp
  MODULES
    auth
    data
    protocol
    testing
    world/features/character
    world/features/escape
    world/features/fog
    world/features/housing
    world/features/mail
    world/features/market
    world/features/npc
    world/features/processing
    world/session
  LIBRARIES
    Boost::beast
    OpenSSL::SSL
)

# Map-reveal/catalog/parser validation under its historical CTest prefix. The sources
# come from three modules (verified with the deps mode of the scaffolder), so MODULES
# lists every module those files include besides this one.
chronicles_add_module_tests(world/features/gm
  NAME chronicles-exploration-tests
  TEST_PREFIX Exploration.
  SOURCES
    tests/gm_tools_tests.cpp
    ../fog/tests/fog_tests.cpp
    ../../../data/tests/client_catalog_tests.cpp
  MODULES
    data
    persistence
    protocol
    testing
    world/features/escape
    world/features/fog
    world/session
)
