# world/features/friends module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
# Friend lists, recommendations and presence (docs/friends.md).
chronicles_add_module(world/features/friends
  SOURCES
    friend_storage.cpp
    friends.cpp
    native_friend.cpp
  DEPENDS
    auth
    common
    data
    net
    persistence
    world/features/chat
    world/features/guild
  LIBRARIES
    chronicles-messages
    libpqxx::pqxx
    spdlog::spdlog
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(world/features/friends
  SOURCES
    tests/friends_tests.cpp
  MODULES
    services/front
    testing
)
