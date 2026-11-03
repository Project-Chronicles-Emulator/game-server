chronicles_add_module(world/features/guides
  SOURCES
    contents_guides.cpp
  DEPENDS
    persistence
    protocol
    world/features/quests
  LIBRARIES
    nlohmann_json::nlohmann_json
)

chronicles_add_module_tests(world/features/guides
  SOURCES
    tests/contents_guides_tests.cpp
  MODULES
    protocol
)
