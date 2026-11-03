# services/front module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(services/front
  SOURCES
    native_ws.cpp
  DEPENDS
    auth
    net
    world/features/chat
    world/features/friends
    world/features/guild
    world/features/housing
  LIBRARIES
    chronicles-messages
    spdlog::spdlog
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(services/front
  SOURCES
    tests/native_ws_tests.cpp
  MODULES
    testing
)
