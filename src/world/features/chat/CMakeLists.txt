# world/features/chat module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(world/features/chat
  SOURCES
    chat.cpp
    native_chat.cpp
    player_commands.cpp
    server_messages.cpp
    chat_feature.cpp
    system_notice.cpp
  DEPENDS
    auth
    net
    protocol
    world/core
    world/features/party
  LIBRARIES
    nlohmann_json::nlohmann_json
    protobuf::libprotobuf
    spdlog::spdlog
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(world/features/chat
  SOURCES
    tests/chat_tests.cpp
    tests/native_chat_tests.cpp
    tests/player_commands_tests.cpp
    tests/server_messages_tests.cpp
    tests/chat_session_tests.cpp
    tests/chat_replication_tests.cpp
  MODULES
    services/front
    testing
  LIBRARIES
    nlohmann_json::nlohmann_json
)


# The unchanged full-world test uses the existing application fixture only in this target.
chronicles_add_module_tests(world/features/chat
  NAME chronicles-world-features-chat-native-tests
  SOURCES
    tests/native_chat_integration_tests.cpp
  MODULES
    auth
    protocol
    testing
    world/session
  LIBRARIES
    Boost::asio
    Boost::beast
    OpenSSL::SSL
)
