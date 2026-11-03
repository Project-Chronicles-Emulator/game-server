# services/social module. DEPENDS lists the modules whose headers it includes;
# Dependencies are declared explicitly below.
chronicles_add_module(services/social
  SOURCES
    character_social.cpp
  DEPENDS
    auth
    world/features/company
    world/features/guild
    world/features/housing
  PRECOMPILED_HEADERS
)
