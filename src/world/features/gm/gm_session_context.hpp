#pragma once

#include "world/features/gm/gm_session_ports.hpp"

namespace chronicles::gm {

// Legacy call-shape compatibility only. Production binds the focused interfaces directly.
// GmSession never retains this aggregate; its delegating constructor binds the eight roles.
class GmSessionContext : public GmSessionAccess,
                         public GmCombatCommands,
                         public GmCharacterCommands,
                         public GmSocialCommands,
                         public GmQuestCommands,
                         public GmWorldCommands,
                         public GmMailCommands,
                         public GmFeatureDispatch {
public:
  ~GmSessionContext() override = default;
};

} // namespace chronicles::gm
