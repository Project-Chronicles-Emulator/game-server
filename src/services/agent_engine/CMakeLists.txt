# services/agent_engine module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(services/agent_engine
  SOURCES
    agent_engine.cpp
    agent_engine_chat_completion.cpp
    agent_engine_data.cpp
    agent_engine_http.cpp
    agent_engine_knowledge.cpp
    agent_engine_model.cpp
    agent_engine_rest.cpp
    agent_engine_storage.cpp
    agent_engine_text.cpp
  DEPENDS
    auth
    data
    world/features/guild
  LIBRARIES
    Boost::asio
    Boost::beast
    chronicles-messages
    libpqxx::pqxx
    nlohmann_json::nlohmann_json
    protobuf::libprotobuf
    spdlog::spdlog
  DEFINITIONS
    _WIN32_WINNT=0x0A00
  PRECOMPILED_HEADERS
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(services/agent_engine
  NAME chronicles-agent-engine-tests
  SOURCES
    tests/agent_engine_tests.cpp
  MODULES
    data
  LIBRARIES
    nlohmann_json::nlohmann_json
)
