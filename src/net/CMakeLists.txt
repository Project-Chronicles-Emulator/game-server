# net module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(net
  SOURCES
    native_ws_frames.cpp
    world_connection.cpp
    world_tls_diagnostics.cpp
    world_tls_listener.cpp
  DEPENDS
    common
    protocol
  LIBRARIES
    Boost::asio
    Boost::beast
    OpenSSL::SSL
    protobuf::libprotobuf
    spdlog::spdlog
  DEFINITIONS
    _WIN32_WINNT=0x0A00
)

# One test program for this module (phase 1b-2 Task 6). MODULES lists the other
# modules its tests include; test files are not part of the include check.
chronicles_add_module_tests(net
  SOURCES
    tests/read_resume_gate_tests.cpp
    tests/world_connection_tests.cpp
    tests/world_tls_listener_tests.cpp
)

if(BUILD_TESTING)
  target_compile_definitions(chronicles-net-tests PRIVATE
    LISTENER_TEST_FIXTURE_ROOT="${CMAKE_CURRENT_BINARY_DIR}/test-fixtures"
  )
endif()
