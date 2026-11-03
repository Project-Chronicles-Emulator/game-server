# world/features/fog module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(world/features/fog
  SOURCES
    fog_rest.cpp
    fog_reveal.cpp
    fog_storage.cpp
  DEPENDS
    auth
    data
    persistence
  LIBRARIES
    chronicles-messages
    libpqxx::pqxx
    nlohmann_json::nlohmann_json
    spdlog::spdlog
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(world/features/fog
  NAME chronicles-fog-tests
  SOURCES
    tests/fog_tests.cpp
  MODULES
    persistence
    testing
)
