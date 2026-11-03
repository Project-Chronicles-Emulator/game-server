#pragma once

#include "auth/auth.hpp"
#include "auth/native_auth.hpp"
#include "data/client_catalog.hpp"
#include "world/features/fog/fog_repository.hpp"

#include <set>

namespace chronicles {

// Static client contract: GET/PUT /api/front/CharFogArea{Load,Save}Req/<char_id>.
// Saves are incremental FogAreaSaveReq bodies, not complete map snapshots.
class FogRest {
public:
  // `fog` is the repository the routes read and write once the AuthService session check
  // (`character_owner`) has authorized the selected character.
  explicit FogRest(AuthService &auth, FogRepository &fog, ClientCatalog *catalog = nullptr);

  static bool handles(std::string_view path);

  NativeHttpResponse handle(std::string_view method, std::string_view path,
                            std::string_view authorization, std::string_view content_type,
                            const std::string &body);

private:
  AuthService &auth_;
  FogRepository &fog_;
  bool validate_catalog_{};
  std::set<int> known_areas_;
};

} // namespace chronicles
