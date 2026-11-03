# testing module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
# Interface module for the shared test-support headers (no sources): test programs link it
# for the helpers in this folder. The former aggregated tests.cpp moved to the auth,
# persistence and protocol modules' test folders in phase 1b-2.
chronicles_add_module(testing
  DEPENDS
    persistence
    protocol
    world/features/company
    world/features/housing
    world/features/loot
  LIBRARIES
    GTest::gtest
    spdlog::spdlog
)
