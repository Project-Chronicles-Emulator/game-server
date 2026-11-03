#pragma once

// Front REST routes for hot-key quick slots, served on the lobby HTTP listener that
// WorldEnterRes.Granted.front_rest_address names.
//
// Client contract (static analysis; dumps/ida_research/hotkeys-20261026):
// - Load: GET {front_rest_address}/api/front/CharHotKeyLoadReq/{char_id}/{n}
//   (AAC_HotKey_REST_SendCharHotKeyLoadReq, n = 1). The response body is a binary
//   x5.pb.rest.CharHotKeys; the client applies it to FX5HotKeySlotManager for status 200-206.
// - Save: PUT {front_rest_address}/api/front/CharHotKeySaveReq/{char_id}/{n} with
//   Content-Type application/protobuf and one x5.pb.rest.CharHotKey (a slot without a value
//   clears that slot).
// - Both send "Authorization: Bearer <session token>", the token of the front WebSocket.
// The client starts a summoned mount only from a quick slot holding a SummonNpc id, so these
// routes are what make the summon key usable. The meaning of {n} is unknown (always 1).

#include "auth/auth.hpp"
#include "auth/native_auth.hpp"
#include "world/features/hotkeys/hotkey_repository.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace chronicles {

class HotKeyRest {
public:
  // `hotkeys` is the repository the routes read and write once the AuthService session check
  // (`character_owner`) has authorized the character.
  HotKeyRest(AuthService &auth, HotKeyRepository &hotkeys);

  // True for a path under /api/front/ naming a hot-key route (any prefix before it is allowed).
  static bool handles(std::string_view path);

  // Answers a hot-key route. `authorization` is the raw Authorization header.
  NativeHttpResponse handle(std::string_view method, std::string_view path,
                            std::string_view authorization, std::string_view content_type,
                            const std::string &body);

private:
  NativeHttpResponse load(const std::string &token, std::int64_t character_id);
  NativeHttpResponse save(const std::string &token, std::int64_t character_id,
                          std::string_view content_type, const std::string &body);

  AuthService &auth_;
  HotKeyRepository &hotkeys_;
};

} // namespace chronicles
