# world/features/processing module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(world/features/processing
  SOURCES
    processing.cpp
    processing_feature.cpp
  DEPENDS
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
chronicles_add_module_tests(world/features/processing
  SOURCES
    tests/native_processing_integration_tests.cpp
    tests/processing_feature_tests.cpp
    tests/processing_bootstrap_tests.cpp
    tests/processing_tests.cpp
  MODULES
    auth
    common
    data
    persistence
    protocol
    testing
    world/core
    world/features/npc
    world/session
  LIBRARIES
    Boost::beast
    OpenSSL::SSL
)
