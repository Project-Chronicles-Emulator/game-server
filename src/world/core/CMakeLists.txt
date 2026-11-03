# world/core module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
# game_rates.cpp holds the operator reward rates consulted by tutorial EXP awards and edited
# by the admin console.
chronicles_add_module(world/core
  SOURCES
    frame_router.cpp
    game_rates.cpp
    instance_copies.cpp
    item_grant_commit.cpp
    map_channels.cpp
    progress_access.cpp
    progress_packets.cpp
    quest_effect_dispatcher.cpp
    retained_owner_supervisor.cpp
    player_snapshots.cpp
    session_event_bus.cpp
    unit_registry.cpp
    unit_ids.cpp
    world_presence.cpp
    world_sectors.cpp
  DEPENDS
    auth
    common
    data
    persistence
    protocol
  LIBRARIES
    nlohmann_json::nlohmann_json
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(world/core
  SOURCES
    tests/frame_router_tests.cpp
    tests/instance_copies_tests.cpp
    tests/item_grant_commit_tests.cpp
    tests/map_channels_tests.cpp
    tests/presence_index_tests.cpp
    tests/quest_effect_dispatcher_tests.cpp
    tests/retained_owner_supervisor_tests.cpp
    tests/session_event_bus_tests.cpp
    tests/unit_registry_tests.cpp
    tests/unit_ids_tests.cpp
    tests/world_sectors_tests.cpp
    tests/inventory_session_events_tests.cpp
  MODULES
    common
    data
    protocol
    persistence
    testing
)
