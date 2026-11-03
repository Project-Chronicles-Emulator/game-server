# world/features/quests module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
# Shared quest rules (tutorial instances and the shared-world quest service).
chronicles_add_module(world/features/quests
  SOURCES
    graduation_quests.cpp
    quest_job_boards.cpp
    quest_rules.cpp
    quests_feature.cpp
    quests_session.cpp
    world_quests.cpp
  DEPENDS
    common
    data
    persistence
    protocol
    world/core
    world/features/character
    world/features/inventory
  LIBRARIES
    nlohmann_json::nlohmann_json
    spdlog::spdlog
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(world/features/quests
  SOURCES
    tests/all_quest_kinds_tests.cpp
    tests/quest_tests.cpp
    tests/quests_feature_tests.cpp
    tests/world_quests_tests.cpp
  MODULES
    protocol
    testing
    world/features/tutorial
  LIBRARIES
    OpenSSL::Crypto
)
