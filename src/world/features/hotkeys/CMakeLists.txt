# world/features/hotkeys module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
# Hot-key quick slots served through the front REST adapter (hotkey_rest.hpp) and stored by
# the module's own repositories (hotkey_repository.hpp, hotkey_storage.cpp).
chronicles_add_module(world/features/hotkeys
  SOURCES
    hotkey_rest.cpp
    hotkey_storage.cpp
  DEPENDS
    auth
    persistence
  LIBRARIES
    chronicles-messages
    libpqxx::pqxx
    nlohmann_json::nlohmann_json
    spdlog::spdlog
  PRECOMPILED_HEADERS
)

# These existing REST/storage tests moved intact from the companion test program.
chronicles_add_module_tests(world/features/hotkeys
  SOURCES
    tests/hotkey_tests.cpp
  MODULES
    auth
    persistence
    testing
)
