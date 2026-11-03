# world/features/company module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
# Companies (in-game factions); see docs/companies.md.
chronicles_add_module(world/features/company
  SOURCES
    company.cpp
    company_quest_settlement.cpp
    company_dialogues.cpp
    company_storage.cpp
    company_world.cpp
    company_feature.cpp
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
    spdlog::spdlog
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(world/features/company
  SOURCES
    tests/company_tests.cpp
    tests/company_session_tests.cpp
  MODULES
    auth
    data
    services/social
    testing
    world/features/gm
    world/features/guild
)
