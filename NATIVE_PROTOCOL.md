# Original-client protocol evidence

The native adapter is disabled by default. Its legacy Steam-to-configured-account and identity
mapping modes remain loopback development options and do not validate platform tickets.
Configured remote listeners require HTTPS and named-account launch grants backed by launcher
authentication. See `docs/distribution-backend.md` for that mode and its compatibility limits.

## Bootstrap and HTTP authentication

The client reads `Server.GeoIP` and `Server.BootstrapUrl` from its project
`Config/DefaultX5Game.ini`. `GeoIP` accepts semicolon-separated geolocation URLs; its callback
reads a JSON `country` field. It is separate from service discovery.
`BootstrapUrl` is a base URL; the client appends `api/auth/BootstrapReq` and parses this JSON:

```json
{
  "regions": [
    {
      "name": "local",
      "auth_address": "http://127.0.0.1:8080"
    }
  ]
}
```

The adapter serves that response at `GET /native/bootstrap` and
`GET /api/auth/BootstrapReq`. Optional maintenance and client-log fields are omitted.
`/native/bootstrap` is an emulator convenience route, not the native geolocation response.

The loose INI override was tested with a restarted original client and did not change its
bootstrap destination. Runtime inspection confirmed the expected `../../../X5/` project path,
but the client's config cache contained the packaged nine-section `DefaultX5Game.ini`, including
the official bootstrap address. A reliable redirect must account for packaged-file priority.

Native HTTP request builders set `Content-Type: application/protobuf`, serialize ordinary protobuf
message bodies, and use `Authorization: Bearer <access_token>` after authentication. The adapter
returns `application/protobuf` for message replies.

Native world-list and WorldGet requests can read public world metadata before local authentication;
those replies omit account character counts and do not create sessions. A redirected running client
may retain its existing opaque platform bearer when returning to the title screen. In explicitly
enabled local mode, world entry or a native WebSocket upgrade authenticates the configured emulator
credentials and binds that opaque value as a process-local session alias. It does not verify the
platform token, forward it, log it or persist it. Aliases accept at most 4096 bytes and share account
logout/revocation behavior. Development API routes do not create these aliases.

| Request | Response | Evidence |
| --- | --- | --- |
| `GET /api/auth/WorldListReq` | `x5.pa2c.WorldListRes` | Request builder and callback |
| `GET /api/auth/WorldGet/<world_id>` | `x5.pa2c.WorldListRes` | Request builder and shared callback |
| `PUT /api/auth/WorldEnterReq/<world_id>` | `x5.pa2c.WorldEnterRes` | Request builder and callback RTTI |
| `PUT /api/auth/steam/AuthReq` | `x5.pa2c.AuthChallengeRes` | Original client request parsed successfully; response accepted before WorldListReq in the V3 trial |
| `PUT /api/auth/LoginUpgradeReq` | `x5.pa2c.AuthChallengeRes` | Original client route observed; recovered request and callback RTTI establish schema pairing |
| `PUT /api/auth/AuthLogoutReq` | Empty HTTP 200 response | Request builder installs no response callback; client immediately resets local auth state |

For the Steam route, the adapter parses `x5.pc2a.AuthSteamReq` and requires a nonempty `auth_token`.
The V3 bootstrap trial observed this route returning HTTP 200, followed by WorldListReq and
LoginUpgradeReq when selecting a realm. Platform-ticket contents are never logged, persisted or
forwarded. This local mapping does not validate the ticket with Steam.

### Named-account launch grant

The emulator's separate JSON login verifies a named account and password against its password
hash. An authenticated launcher can then request a random, five-minute launch grant.
It places the grant in the bootstrap URL path:
`/launch/<grant>/api/auth/BootstrapReq`. That bootstrap advertises an `auth_address` with the
same `/launch/<grant>` prefix, so the original client's subsequent native HTTP calls carry it.
The prefixed Steam AuthReq consumes the grant atomically and receives a fresh bearer session for
the password-authenticated account and activates a 24-hour reconnect credential. LoginUpgradeReq
rotates that account's session, and later
world and character operations use the bearer. The Steam ticket remains unverified and is not
the account credential in this mode. The initial grant is consumed once; subsequent Steam
requests on the same prefix authenticate with its activated reconnect credential. PostgreSQL
stores only its SHA-256 digest, so it survives a backend restart. Password reset, banning,
operator sign-out, or issuing a new launch grant revoke it. Game logout and launcher sign-out
preserve an activated credential for the running game. The credential is a replayable bearer
secret until its fixed expiration or revocation; anyone who obtains the URL can use it.

`--native-require-launch-grant true` rejects unprefixed Steam AuthReq and disables the old
opaque-bearer alias fallback. Keep this setting enabled for named-account tests. The grant is
embedded in a URL and therefore sensitive; the server redacts prefixed paths from its logs.
Remote deployments must use HTTPS and must redact these paths in proxies and access logs too.
The prefixed exchange and account isolation have synthetic tests. The original client has not
yet been observed completing this launch-grant flow, and a second computer needs separate
network reachability and TLS validation.

For a local two-client protocol test, `--native-identity-accounts true` opts into one emulator
account per nonempty `AuthSteamReq.identity`. The account name is a SHA-256-derived alias, so the
raw identity is not stored as an account name. The adapter creates a local account on first login
and rotates that account's session during `LoginUpgradeReq`, leaving another identity's session
active. This is strictly a loopback development mapping: neither the ticket nor the claimed
identity is authenticated. It is **not username/password login** and must not be exposed as an
account login service to another machine. A client redirected mid-session with only an old
platform bearer must perform a fresh native login in this mode. Clients claiming the same identity
still share one account and replace each other's login. The persistent PowerShell helper exposes
this as `-NativeIdentityAccounts`; omitting it keeps the existing local account and characters.

Selecting a realm after browse login sends LoginUpgradeReq with its current Bearer header.
The recovered body contains platform/device/locale metadata and optional withdrawal/kick fields;
it contains no token or world ID. The shared callback accepts AuthChallengeRes, stores its
access_token, and continues with WorldEnterReq for the selected realm. The local adapter requires
an existing live session and returns a fresh local login response, revoking the previous session.
It does not map unknown, expired or revoked tokens on this route. See
`../dumps/native_login_research/LoginUpgrade-findings.md` for static evidence. Synthetic upgrade
and subsequent character operations pass. The original client also accepted the new handler,
entered the realm and created/selected a character using the persistent backend.

Logout is idempotent, including for an unknown token from a previously connected platform
session. Known local sessions and their outstanding character handoffs are revoked. The client
reset clears its current auth-address copy but preserves the bootstrap region array and selected
region, so a session-only endpoint redirect must also update the selected region's address.

The password flow is separately visible in the client. Its recovered schemas are:

| Request | Response | Relevant fields |
| --- | --- | --- |
| `PUT /api/auth/AuthReq` (`x5.pc2a.AuthReq`) | `x5.pa2c.AuthRes` | `account`, `pass_shadow` bytes; `error_code`, `question` bytes |
| `PUT /api/auth/AuthChallengeReq` (`x5.pc2a.AuthChallengeReq`) | `x5.pa2c.AuthChallengeRes` | `account`, `challenge` bytes and device metadata; `account_id`, `access_token` |

The client decrypts `question` using a 32-byte password-derived key and a fixed IV. The exact
`pass_shadow` derivation, plaintext question format, and `challenge` response calculation are not
established. A protobuf shape alone cannot authenticate a password. Both native password routes
remain unimplemented; unauthenticated calls receive HTTP 401. A synthetic test verifies that a
caller choosing an existing password account's name as its Steam identity receives a separate
derived identity account, never that password account. A second machine would use the
password-verified launch grant described above. That path still needs an original-client
observation, reachable addresses, and LAN certificate validation before it can be considered
ready. Merely accepting a claimed identity or bearer token would allow account impersonation.

## World-list compatibility

The client compares the world's version string with its current version and compares protocol
bytes with a 32-byte compiled fingerprint. For the inspected demo build, both were verified in
the running client:

- Version: `hotfix-26.10.18.734`
- Protocol bytes, represented as hexadecimal:
  `b0fa233f08edc242480aa097e76541d4e256bd7641c856c424907eb2ecd91e15`

The protocol field contains those raw 32 bytes. It is distinct from the 40-character build
identifier stored in the native gameplay packet-log header.

## Reproducible evidence

### Tutorial interaction increment, 2026-10-24

The optional generated tutorial profile adds handlers for floor discovery and
pickup, quest acceptance/progress/reward, utility-slot assignment, dialogue,
and two animated quest objects on map 8000. Definitions come from hash-verified
client catalogs and placements. Eight floor target IDs and twelve NPC types
matched the official tutorial trace. Rewards and quest changes are saved
atomically before success packets are sent; retry and reconnect behavior has
synthetic TLS and PostgreSQL coverage. Original-client interaction verification
confirmed the floor pickup and five-potion display; potion use was still ignored
in that trial. The new consumption handler follows skill 1000 and effect
100000141 from the extracted action data. Health recovery, combat, and later map
transitions remain open.
See [docs/tutorial-interactions.md](docs/tutorial-interactions.md).

Decompiled function evidence is saved in `../dumps/native_login_research/`:

| Address | Responsibility |
| --- | --- |
| `0x144972c60` | Read `Server.GeoIP` from `DefaultX5Game.ini` |
| `0x144971b10` | Read `Server.BootstrapUrl` and append bootstrap path |
| `0x1419b4500` | Build project `Config/` path |
| `0x146186c50` | Start bootstrap and GeoIP requests |
| `0x14618d4c0` | Parse bootstrap regions JSON |
| `0x14618ab20` | Parse GeoIP JSON country, separately from bootstrap |
| `0x146189f10` | Pass BootstrapReq response to the same JSON parser |
| `0x146178500`, `0x146178780` | Build HTTP protobuf requests |
| `0x146182110` | Add bearer authorization |
| `0x14618f820` | Build world-list request |
| `0x14618f550` | Build WorldGet request |
| `0x14618f1d0` | Build WorldEnter request |
| `0x14618cd10` | Parse WorldListRes and update available worlds |
| `0x145fa8310` | Compare world version to local client version |
| `0x145fa6110` | Compare world protocol with compiled fingerprint |
| `0x1461893f0` | Read AuthRes and build AuthChallengeReq |
| `0x14616a000` | Decrypt password question |
| `0x146187300` | Send fire-and-forget AuthLogoutReq and immediately reset client auth |
| `0x146181600` | Clear auth/session state while retaining bootstrap regions and selected region |

The Steam request path literal is UTF-16 at `0x14a81fed8`. Parts of the adjacent Steam builder
remain protected in both the saved image and the current running process; no guessed machine
code was patched into the database.

Synthetic adapter tests establish parsing, response types, session and world-entry behavior.
They do not establish that the original client has completed login or entered the world.
