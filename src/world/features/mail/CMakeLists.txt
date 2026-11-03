# world/features/mail module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(world/features/mail
  SOURCES
    mail.cpp
    mail_rest.cpp
    mail_session.cpp
    mail_storage.cpp
    mail_types.cpp
    tutorial_completion_mail.cpp
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
chronicles_add_module_tests(world/features/mail
  SOURCES
    tests/mail_session_tests.cpp
    tests/mail_tests.cpp
    tests/native_mail_tests.cpp
    tests/tutorial_completion_mail_tests.cpp
  MODULES
    data
    persistence
    protocol
    testing
    world/core
    world/features/events
    world/features/fog
    world/features/market
    world/features/npc
    world/features/processing
    world/session
  LIBRARIES
    Boost::beast
    OpenSSL::SSL
)
