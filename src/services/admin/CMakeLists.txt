# services/admin module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(services/admin
  SOURCES
    admin_api.cpp
    admin_log_buffer.cpp
  DEPENDS
    auth
    common
    data
    persistence
    world/core
    world/features/chat
    world/features/company
    world/features/guild
    world/features/mail
    world/session
  LIBRARIES
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    protobuf::libprotobuf
    spdlog::spdlog
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(services/admin
  NAME chronicles-admin-tests
  SOURCES
    tests/admin_tests.cpp
  MODULES
    common
    data
    protocol
    testing
    world/core
    world/features/chat
    world/features/guild
    world/features/mail
    world/session
  LIBRARIES
    spdlog::spdlog
)
