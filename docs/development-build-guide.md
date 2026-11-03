# Project Chronicles Emulator — C++20

The launcher defaults to the local server. See [project configuration](docs/project-configuration.md)
for server profiles, distribution signing keys and optional Discord setup.

The maintained [packet and feature reference](docs/protocol-feature-reference.md) catalogs the
recovered demo messages by feature, implementation status, payload fields and captured evidence.
Read it before feature work and follow the [required maintenance procedure](docs/protocol-reference-maintenance.md)
to keep its local files and published Page current.

Emulator foundation: captured packet replay, a persistent account/character lobby, native bootstrap
and binary lobby adapters, and an experimental TLS world listener. The local tutorial world
checkpoints client movement and supports persistent floor pickups and initial quest interactions
in private character instances. See [docs/tutorial-interactions.md](docs/tutorial-interactions.md)
for the supported gameplay and [docs/persistence.md](docs/persistence.md) for saved movement fields.
The earlier nearby-player relay remains available in the bootstrap without the tutorial profile.

For the local loop, use the WebView2 launcher. Its player home screen includes realm availability,
news, account sign-in and settings, with development tools under Diagnostics. Its default
named-account mode verifies a password
against the persistent loopback server and generates a one-use account-specific bootstrap archive
before launching the client through Steam. The explicit disposable mode starts a memory server
and retains the reversible in-memory endpoint redirect. Both modes arm the guarded world-entry
helper automatically. See
[docs/launcher.md](docs/launcher.md).

For player distribution, [the backend and release guide](docs/distribution-backend.md) covers
configurable API/world addresses, HTTPS/WSS, signed game releases, native automatic patching,
recovery and packaging. The supplied configuration is local; remote hostname placeholders can
be replaced later. Distribution profiles use signed bootstrap templates and disable local memory
helpers. Original-client remote world entry remains a separate validation step.

```powershell
cmake --build --preset release --target chronicles-launcher chronicles-world-tls-local
& .\out\build\windows\bin\Release\launcher\chronicles-launcher.exe
```

See [VALIDATION.md](VALIDATION.md) for the tested build, full-capture replay results and
login/lobby integration results. The helper's temporary TLS and startup-branch changes are
described in [docs/world-entry-findings.md](docs/world-entry-findings.md).

This targets **ArcheAge Chronicles Demo**, build `40367e24241c9481bfc07fbf9ef1919f25fb0eaa`.

[Rankings, raid channels and channel events](docs/rankings-raids-channel-events.md) now have
bounded runtime services, durable leaderboard records and synthetic tests. Automatic qualified
boss records, authored event/raid scripts, reward settlement and original-client acceptance remain
unfinished; the feature is partial rather than a full gameplay compatibility claim.

Server-to-player [mail](docs/mail.md) supports durable item/currency claims through the native
mailbox and the desktop console's Mail page. Configure recurring chat/banner announcements and
private world-entry greetings on the console's Announcements page or with
`--server-messages-file config/server-messages.example.json`; see
[server messages and commands](docs/server-messages.md). Stock-client players can use `!help`,
`!pvp on|off|status`, `!mount` and `!dismount`; this build consumes unknown slash commands
locally. The server also accepts the `/` forms when a client forwards them. The
[PvP flag](docs/player-pvp.md) replicates faction state; shared
player damage and original-client acceptance remain separate unfinished work.

House-mate AI chat uses the client's agent-engine REST service. `--agent-engine-port 38011`
serves it (loopback peers only by default), and the launcher redirects the client's hard-coded
agent-engine URL to it. Replies come from a deterministic scripted model or an optional
OpenAI-compatible endpoint; see [docs/agent-engine.md](docs/agent-engine.md). Original-client
acceptance is still pending.

## Exploration guide

Open [the searchable destination list](docs/exploration-guide.html) in a browser for
411 static destinations, including villages, landmarks, housing areas and trading posts.
307 entries provide same-main-world GM teleport commands; approximate anchors and missing
coordinates are labeled explicitly. See [exploration destinations](docs/exploration-places.md)
for regeneration and safety limits. `/gm reveal_map` saves all 1,063 known fog areas for the
issuing GM character; select that character again to reload them. Fog reveal is separate
from Adventure Progress and zone access restrictions.

## Build on this machine

Requirements for this preset: Visual Studio 2026 C++ workload, CMake 4.2+, vcpkg, Python with `protobuf`.
The manifest pins dependencies; generated sources come directly from the recovered descriptor set.
OpenSSL is pinned separately to 3.6.5 in `vcpkg-configuration.json`. Its Windows x64
AVX-IFMA RSA implementation preserves nonvolatile registers that the 3.6.0 build corrupted
on the development machine. The other dependencies retain the existing baseline.
The generator applies one C++ helper-name shim for `EX5RecipeSortType_Name`, which is both an original enum value and a name protoc would otherwise use for a function. Enum names, numbers and serialized descriptors are preserved.

```powershell
cd 'D:\PROJETOS\Archeage Emulator\Emulator'
. .\tools\dev_shell.ps1
$env:VCPKG_ROOT = 'D:\Programas\vcpkg'
python -m pip install -r requirements-build.txt
cmake --preset windows "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build --preset release
ctest --preset release -j 8
```

The preset uses Ninja Multi-Config with MSVC. Dot-source `tools/dev_shell.ps1` once in every
new terminal before `cmake`; it loads the Visual Studio x64 compiler environment that Ninja
needs. Executables are in `out/build/windows/bin/Release`; the player launcher (with its helpers), the operator launcher and the admin console are in its `launcher/`, `launcher-operator/` and `admin/` subfolders. The first build compiles pinned dependencies and can take tens of minutes; subsequent builds reuse vcpkg's binary cache. Windows is the currently tested platform; other platforms need their own CMake generator/toolchain configuration.

The retired Python/PowerShell test harnesses and obsolete tools were removed from
`tools/`. C++ tests remain available through CTest. See [tools/README.md](tools/README.md)
for retained workflows and the status of historical test commands.

## Packet replay

```powershell
$bin = '.\out\build\windows\bin\Release'
$capture = 'G:\Dumps\Archeage\ClientData_2026-10-23\LivePackets\native-log-final.bin'
& "$bin/chronicles-replay.exe" $capture .\protocol --tcp-loopback --jsonl .\out\replayed.jsonl
```

Without `--tcp-loopback`, replay runs entirely in memory. TCP mode creates a private ephemeral loopback socket pair, streams application frames, reassembles fragmented/coalesced reads, verifies identical bytes, decodes protobuf and checks semantic encode/decode round trips. Every decoded record is also rebuilt through the opcode-aware application frame encoder and decoded again. It never connects to the retail client or a remote game server. `--paced` preserves elapsed packet timing; gaps above 60 seconds abort rather than silently sleeping for an unbounded period.

The immutable capture has 17,055 complete records across 128 opcodes, plus an incomplete 753-byte final record. The incomplete tail is reported and excluded from replay. `--strict-tail` returns status 2 for any incomplete tail; malformed complete records always fail. Unknown opcodes, unknown protobuf fields and decode failures also return status 2. Decoded JSONL includes captured session values; keep it under ignored `out/`.

Native logger format, little endian:

| Layer | Fields |
|---|---|
| Log header | 40 ASCII hex build bytes, NUL, 8-byte raw base time, 8-byte raw offset |
| Record | elapsed milliseconds `u32`, application frame |
| S2C frame | total frame length `u16`, opcode `u16`, protobuf body |
| C2S frame | total frame length `u16`, opcode `u16`, sequence `u32`, protobuf body |

This format comes from the **application logger**. Native lobby WebSocket packets have a separate
recovered envelope; the world transport uses TLS. A controlled original-client trial established
a 100-byte initial world credential with the selected character token at offset 36. Replaying a
session's notifications does not implement authoritative game simulation.

For bounded, direction-indexed official-client world captures, see
[docs/packet-capture.md](docs/packet-capture.md). Raw packets and decoded values belong under
ignored `out/`.

## Run the local account/lobby server

For a disposable local run:

```powershell
$env:CHRONICLES_BOOTSTRAP_ACCOUNT = 'developer'
$env:CHRONICLES_BOOTSTRAP_PASSWORD = Read-Host 'New local password (12+ characters)' -MaskInput
& .\out\build\windows\bin\Release\chronicles-server.exe --storage memory --port 8080
```

Memory mode explicitly discards accounts and characters when the process exits. The bootstrap account is created only if missing; restarting does not reset an existing password.

For persistence, install PostgreSQL 16 and PowerShell 7.2 or later, then run the
database installer from `Emulator/`. PostgreSQL must already be running:

```powershell
pwsh -File tools/install_database.ps1
```

The script prompts for the PostgreSQL administrator password and a new application
password, creates the `chronicles` login/database, and installs the complete schema.
Use `-PostgresBin 'C:\Program Files\PostgreSQL\16\bin'` if `psql` is not on PATH.
Host, port, database and login can be changed with `-DatabaseHost`, `-DatabasePort`,
`-DatabaseName` and `-DatabaseUser`. Existing login passwords are never reset.
An existing current database is preserved; older or unrelated schemas are rejected.
See [database installation](docs/persistence.md#database-installation) for details.

Start the server with the same database settings:

```powershell
$env:CHRONICLES_DATABASE_URL = 'host=127.0.0.1 port=5432 dbname=chronicles user=chronicles connect_timeout=5'
$env:PGPASSWORD = Read-Host 'Database password' -MaskInput
& .\out\build\windows\bin\Release\chronicles-server.exe --port 8080 --migrate .\migrations\001_accounts_characters.sql
```

PostgreSQL is the default backend. `migrations/001_accounts_characters.sql` is the
single complete installation schema. The existing `--migrate` option can apply it
at startup; installation is atomic and protected by an advisory lock. Reapplying it
to the current schema leaves data unchanged. Historical upgrades, diagnostics and
rollback SQL files are not part of the distribution. Runtime operations use
parameterized SQL and database uniqueness constraints. Sessions and handoff tickets
are process-local; restarting invalidates them while retaining accounts and characters.

The listeners bind to `127.0.0.1`. PBKDF2-HMAC-SHA256 password hashes use random salts and 600,000
iterations. Bearer tokens have a one-hour lifetime; a successful login revokes previous sessions
and handoff tickets for that account. Selection tickets expire after 60 seconds and can be consumed
once, through either the ticket boundary or the experimental world listener's packet-key lookup.
A new selection revokes the previous ticket.

Local rules currently allow accounts of 3–32 ASCII letters/digits/underscore, four character slots (0–3), character names of 3–16 ASCII letters/digits, and the eleven recovered class enum values. These are development rules, not verified original service policies.

## Connect the original client to the local adapters

Native mode is opt-in and intended for this loopback development setup. It maps the client's Steam
login to the local account configured below. It does not validate Steam tickets or contact Steam.
The original password/challenge login flow is not implemented.

```powershell
$env:CHRONICLES_BOOTSTRAP_ACCOUNT = 'developer'
$env:CHRONICLES_BOOTSTRAP_PASSWORD = Read-Host 'Local account password (12+ characters)' -MaskInput
& .\out\build\windows\bin\Release\chronicles-server.exe `
    --storage memory --port 8080 --native-client local
```

Use the existing password if the account already exists in PostgreSQL; bootstrap credentials do
not reset it. The same environment values both provision a missing account and authenticate the
native local mapping. The server must be running before restarting the game through its normal
Steam launcher.

The loose client configuration can be staged and restored with this helper. In the current demo,
the packaged INI takes precedence: a restarted client continued to use its official bootstrap
address. This helper alone has therefore **not established a working redirect**.

```powershell
# Apply once; the helper refuses to overwrite an existing backup.
.\tools\native_client.ps1 -Action Configure -Port 8080

# Return to the original configuration, then restart the game.
.\tools\native_client.ps1 -Action Restore
```

The default game directory is
`E:\SteamLibrary\steamapps\common\ArcheAge Chronicles Demo`; pass `-GameDirectory` for another
installation. The helper edits only `BootstrapUrl` in the `[Server]` section of
`X5\Config\DefaultX5Game.ini`. It stores an integrity-checked backup manifest under ignored
`out/native-client/` and refuses restoration if the configured file changed afterward.
`BootstrapUrl` controls service discovery; `GeoIP` is a separate geolocation service and
does not accept bootstrap regions JSON. Runtime inspection found the packaged configuration in
the client's cache despite the loose file. If no loose file exists, `-BaseConfig` can supply the
original extracted INI; this machine uses its copy under `out/native-config/`. The supported
`-textconfig` launch flag skips the binary config cache. The completed trial still used official
regions despite Steam forwarding the flag. See [native-client-config.md](docs/native-client-config.md).

For the current-session trial, the active NA region address was redirected in memory with
original-byte backups and readback verification. Restarting the game removes that redirect.
The local helper starts a disposable server with `NA` response regions:

```powershell
.\tools\start_local.ps1 -WorldPort 8081
```

It generates a local TLS certificate and a temporary account without printing its password.
Direct launches can set `CHRONICLES_CLIENT_REGION` to match the selected client region.

For persistent local client trials on this machine, use the existing PostgreSQL 16 tools:

```powershell
.\tools\start_persistent.ps1
```

This helper creates or reuses a private cluster under ignored `out/local-postgres/` on loopback
port 5433, applies migrations, and starts native HTTP on 8080 and TLS on 8081. It keeps the local
account credentials between restarts and reuses the certificate and key from `out/native-server/`.
The credentials file is private; do not commit it. Restarting the emulator still invalidates
sessions and handoff tickets. The first persistent launch cannot recover characters that existed
only in the previous memory process.

With the game closed, `tools/textconfig_client.ps1 -Action Configure` stages all extracted text
configuration and adds the development certificate through `[SSL] DebuggingCertificatePath`.
Launch through Steam with `-textconfig`. Restore with `-Action Restore` before restoring the older
single-file configuration layer. See [TEXTCONFIG_CLIENT.md](TEXTCONFIG_CLIENT.md) for the backup
and restore workflow. The loose-file trial did not redirect the client and has been restored.

The supported `-textconfig -IniBootstrap=PATH` input accepts a serialized configuration archive.
`tools/binary_config_codec.py` builds a candidate under ignored `out/native-config/`, retaining
the original cache bytes, adding game-local certificate trust, and injecting the custom bootstrap
settings. Twelve focused tests and independent layout review passed. V3 reached the local
bootstrap and login successfully; V4 also preserves the user's existing WebSocket verification
preference. See [binary-config-offline-findings.md](docs/binary-config-offline-findings.md)
for generation commands, evidence and candidate assumptions.

Native HTTP routes include bootstrap JSON, protobuf world lists and world entry. Binary WebSocket
adapters at `/ws/auth` and `/ws/front` use the recovered packet IDs for world entry and character
listing, creation and selection. They require the bearer header during upgrade. See
[NATIVE_PROTOCOL.md](NATIVE_PROTOCOL.md) for the evidence and current limits. The original client
now reaches the local bootstrap, Steam-login mapping and world list. The adapter also implements
`/api/auth/LoginUpgradeReq` for the browse-to-realm transition. The original client completed
realm entry, character creation and selection using PostgreSQL. On a normal world connection,
the client still rejects the local TLS certificate. A controlled diagnostic trial reached the
tutorial world; see the compatibility boundary below.

The supported demo version defaults to `hotfix-26.10.18.734`, with its recovered 32-byte protocol
fingerprint. Other inspected builds can override those values with `CHRONICLES_CLIENT_VERSION`
and `CHRONICLES_CLIENT_PROTOCOL_HEX`; the latter is hexadecimal decoded to raw protocol bytes.

To enable the experimental world listener, supply a PEM certificate and matching PEM private key:

```powershell
& .\out\build\windows\bin\Release\chronicles-server.exe `
    --storage memory --port 8080 --native-client local `
    --world-port 8081 `
    --tls-cert .\out\native-client\world-cert.pem `
    --tls-key .\out\native-client\world-key.pem `
    --protocol-dir .\protocol
```

Those PEM files must exist before startup. The original client rejects this local certificate
without a temporary diagnostic override. Without `--world-port`, character selection omits the
game address and packet key. With it, selection advertises the local TLS endpoint and a one-use
32-byte key. The listener accepts either that synthetic raw-key fixture or the observed 100-byte
client credential carrying the character token at offset 36. Its first 36 bytes remain opaque.
After authentication, the listener sends `CharEntryRes` and handles time sync, ping and character
exit. `--world-bootstrap-diagnostic true` (persistent helper: `-WorldBootstrapDiagnostic`) adds
a class-specific tutorial-map sequence for local testing. It sends map-start after time sync,
waits for map-load completion, then sends a player nub and responds to start-playing. The
listener checkpoints position and body yaw. With the generated tutorial profile loaded from
the client data directory, map 8000 is private per character and supports the floor pickups,
quest triggers, dialogue, and animated objects described in
[docs/tutorial-interactions.md](docs/tutorial-interactions.md). Initial server-timed combat has
synthetic coverage with provisional formulas; see [docs/tutorial-combat.md](docs/tutorial-combat.md).
Later map transitions remain pending. Without that profile, the earlier nearby-player bootstrap
relay is used.

The PostgreSQL schema stores character appearance, level, experience, position, and item instances
with stable inventory and equipment slots. New Wild characters receive starter clothing and a
weapon; existing Wild characters were backfilled once by schema version 3. A verified client
`MoveReq` updates position and body yaw. Item and experience writes have owner-checked storage
APIs. Tutorial rewards use a separate atomic gameplay revision with persistent stackables,
consumed objects, quests, and utility slots. The original client has picked up floor potions;
the consumption increment has synthetic TLS coverage and awaits its live client trial.
Camera yaw and pitch have nullable columns only: the recovered movement packet does not contain
a camera angle.

For separate named accounts with passwords, use
[docs/account-system.md](docs/account-system.md). The private PostgreSQL helper and
`tools/manage_account.ps1` support account creation and password verification. Start the server
with `tools/start_persistent.ps1 -RequireNamedLogin` for named-account tests; the launcher then
requests a short-lived, one-use grant tied to the verified account for each game launch.
`-NativeIdentityAccounts` remains a loopback-only identity-claim experiment; it is not a
password login and must not be used to authenticate a laptop client. Omitting the switch retains
the existing shared `localdev` original-client character unless `-RequireNamedLogin` is set. The
original client has not yet completed an observed named-account grant login. See
[docs/laptop-readiness.md](docs/laptop-readiness.md) for the remaining client and LAN work.
The original game still loads Xigncode even when started directly with a local bootstrap.
See [docs/xigncode-local.md](docs/xigncode-local.md) for the observed startup path and the
current single-PC multi-client limit.
The server reads the verified static client catalog from `data/generated/catalog` when its
manifest is present. See [docs/client-data.md](docs/client-data.md) for export and verification.

## Development API

`GET /health` reports the implementation's compatibility status. Other routes use this explicitly emulator-owned interface:

| Method/path | Request | Recovered response type |
|---|---|---|
| POST `/emulator/v1/login` | JSON `{ "account": "...", "password": "..." }` | `x5.pa2c.AuthChallengeRes` |
| POST `/emulator/v1/launch-grant` | Bearer token | One-use bootstrap URL valid for five minutes |
| POST `/emulator/v1/logout` | Bearer token | HTTP 204; revokes only that account's sessions and handoffs |
| GET `/emulator/v1/worlds` | Bearer token | `x5.pa2c.WorldListRes` |
| POST `/emulator/v1/world/enter` | `x5.pb.authwebsocket.WorldEnterReq` | `x5.pa2c.WorldEnterRes` |
| GET `/emulator/v1/characters` | Bearer token, world already entered | `x5.pb.frontwebsocket.CharListRes` |
| POST `/emulator/v1/characters/create` | `x5.pb.ws.front.CharCreateReq` | `x5.pb.frontwebsocket.CharCreateRes` |
| POST `/emulator/v1/characters/select` | `x5.pb.ws.front.CharSelectReq` | `x5.pb.frontwebsocket.CharSelectRes` |

Authenticated requests need `Authorization: Bearer <access_token>`. Non-login POST requests accept protobuf JSON or `Content-Type: application/x-protobuf`. Responses default to protobuf JSON; `Accept: application/x-protobuf` requests binary protobuf. Success enum values can be omitted from JSON under proto3 defaults. Int64 IDs are JSON strings.

Example creation payload:

```json
{"slot": 0, "char_name": "Archetester", "char_class": "EX5CharacterClass_Fight"}
```

The development WebSocket at `/emulator/v1/lobby/ws` requires the same bearer header during upgrade. Its **development-only JSON envelope** supports `CharListReq`, `CharCreateReq`, and `CharSelectReq`:

```json
{"request_id": 1, "type": "CharListReq", "payload": {}}
```

Replies preserve `request_id` and return `type` plus `payload`, or an `error`. Payloads use the recovered protobuf types. This envelope and the HTTP paths are not claimed to match the original client.

## Server admin console

`chronicles-admin.exe` (**Project Chronicles Server Admin**) is a WebView2 desktop console for a running
server: realm name, capacity and status, accounts (create, password reset, role, ban), characters
(level/EXP, currency, position, rename, delete), live players with kick, live World chat, EXP rate presets and
scheduled bonus events, chat/on-screen announcements, a live log tail and graceful shutdown.
It uses the emulator-owned `/admin/v1` API, which is enabled only with `--admin-token-file` and
accepts loopback connections only by default. `tools/start_persistent.ps1` enables it and writes
the token to ignored `out/admin/admin.token`.

```powershell
cmake --build --preset release --target chronicles-admin
& .\out\build\windows\bin\Release\admin\chronicles-admin.exe
```

See [docs/admin-console.md](docs/admin-console.md) for the security model, settings
persistence, API routes and evidence limits.

## Verification

```powershell
ctest --preset release
python .\tools\smoke_test.py .\out\build\windows\bin\Release\chronicles-server.exe
# Optional: use PostgreSQL binaries to start/stop a separate ephemeral test cluster.
python .\tools\test_postgres.py .\out\build\windows\bin\Release\chronicles-server.exe .\out\postgresql-tools\pgsql\bin
```

The isolated PostgreSQL runner creates a fresh cluster on an ephemeral loopback port, exercises migration and persistence across a server restart, and stops the cluster in a `finally` block. It does not install a Windows service or touch existing databases. Use PostgreSQL binaries from the [official Windows distribution](https://www.postgresql.org/download/windows/); they are not part of the source tree.

Unit coverage includes frame fragmentation/coalescing, malformed lengths, incomplete tails, C2S
sequence handling, unknown fields, password rejection, state gates, session/ticket expiry and
revocation, ticket single use, character ownership and concurrent slot conflicts. Native adapter
tests cover bootstrap JSON, protobuf replies, malformed requests, binary lobby framing and world
bootstrap packets. The server smoke test covers HTTP, binary protobuf and WebSocket lobby requests.

## Original-client compatibility still to verify

The original client has completed local login, character creation and selection with PostgreSQL
persistence. In a controlled 2026-10-23 trial, a one-connection TLS diagnostic and a separate
temporary post-TLS startup-branch patch let it complete TLS 1.3 and present its 100-byte world
credential. With the opt-in Wild tutorial bootstrap sequence, the client loaded the map and
displayed its character and HUD at the extracted spawn position. The world connection remained
open and exchanged ping/pong. The WebView2 launcher later reproduced entry from a fresh Steam
client using its generated local bootstrap and guarded world-entry helper, with no separate
manual patch command. The user moved in the tutorial world and the server received `MoveReq`
packets, but currently ignores them. These temporary client interventions are restored after
each trial; an ordinary unpatched client still rejects the emulator certificate. Authoritative
movement and gameplay remain unverified.

The next work is to identify the client's protected certificate and startup acceptance rules,
then replace the local diagnostic with a compatible connection path. General map loading, entity
replication and authoritative gameplay still need implementation. See [VALIDATION.md](VALIDATION.md)
for the exact observed packet sequence and test boundaries.

For a read-only snapshot of the client's pending TLS context, the development server accepts
`--world-handshake-delay-ms 0..10000`. The default is zero. The delay uses an asynchronous timer
before negotiation; it does not change certificates, verification or packet bytes. The persistent
helper forwards an explicit `-WorldHandshakeDelayMs` value. For example:

```powershell
.\tools\start_persistent.ps1 -WorldHandshakeDelayMs 2000
```

Use the normal helper invocation to return to zero delay after the diagnostic session. The
server also logs bounded handshake metadata without retaining TLS payloads or keys.

`--world-advertised-host HOST` (helper: `-WorldAdvertisedHost`) changes only the host part of
the `game_address` sent in `CharSelectRes`; the listener still binds to loopback. With a DNS
name such as `localhost`, the original client sends that name as TLS SNI; with an IP literal it
sends no SNI. This did not change the current verification failure.

### Realm list presentation and capacity

The single emulator realm is listed as **Project Chronicles Private Server** under the site display name
**Project Chronicles**. These options change its realm-list entry and admission policy:

| Option | Default | Effect |
| --- | --- | --- |
| `--world-name TEXT` | `Project Chronicles Private Server` | `ClientWorldInfo.world_name`, launcher roster and health summary |
| `--site-display-name TEXT` | `Project Chronicles` | `ClientSiteInfo.display_name` |
| `--world-capacity N` | `500` | Characters connected to the world service at once (1–100000) |
| `--max-http-connections N` | `1280` | Maximum concurrent HTTP and WebSocket connections (1–100000). In-world clients keep front/auth WebSockets open, so the default allows two per player at the 500-player target plus launcher traffic; not load tested |
| `--max-world-connections N` | `512` | Maximum accepted world TCP sessions (1–100000); not load tested |
| `--world-label none\|new\|recommended` | `none` | `ClientWorldInfo.label` |
| `--world-maintenance true` | off | Lists the realm as `ServerState_GmOnly`; realm entry returns `WorldIsUnderMaintenance` |
| `--world-char-creation-restricted true` | off | Sets `char_creation_restricted`, `allow_char_create=false`; creation returns `CharCreationRestricted` |

Authentication work uses a bounded, separate worker pool. When its global or socket-peer
slots are full, the server returns HTTP 503 with `Retry-After: 1`; a launcher or client must
retry with bounded backoff. HTTP 429 limits verified bad passwords to 12 per peer/account
and 40 per peer per minute; registration permits 30 attempts per peer per minute. Forwarded
IP headers are ignored. A proxy or NAT shares one peer budget among its users, so abusive
traffic can delay others behind it. A proxy must not be used as a shared ingress without an
explicit trusted-proxy design.

Text values are 1–64 bytes without control characters. Population counts characters whose world
credential was accepted, until their world connection closes; lobby sessions are excluded. The
listed state is `Normal` below 80% of capacity, `Congested` from 80%, and `Overloaded` at capacity.
These thresholds are emulator policy, not recovered official values. At capacity, realm entry
returns `ServerOverload`; the official `QueueTicket` waiting queue is not implemented. Admission is
checked at realm entry, so players already in the lobby can briefly exceed capacity.
`WorldListRes.last_world_id` is set once the account owns a character on the realm.
`GET /health` adds a `world` object with `id`, `name`, `online`, `capacity`, `state`
(`normal`, `congested`, `full`, `maintenance`) and `char_creation_restricted`; the launcher's
realm panel displays the name and population. Whether the original client renders the label,
congestion state and creation restriction has not been observed yet.

`--world-tls12-only true` (helper: `-WorldTls12Only`) limits the world listener to TLS 1.2 for
diagnosis. The default listener still permits TLS 1.2 and 1.3. This option tests whether the
client's post-handshake startup depends on the negotiated protocol version; it does not change
the client's certificate verification callback. In the original-client trial, TLS 1.2 still
failed certificate verification. A one-connection local verifier override completed TLS 1.2,
but the client closed the world socket without sending application data.

The original client verifies the world certificate with its own VM-protected callback
(`0x1461E8860`), on top of normal chain checks. That callback returns
`X509_V_ERR_APPLICATION_VERIFICATION` (50) for the development certificate even though the
certificate is a loaded trusted root, the host matches its SAN, and UE pinning was tried.
See `../dumps/ida_research/world-tls-live/FINDINGS.md`. A failed credential read after TLS now
logs how many bytes arrived, without their contents.

An explicit `chronicles-world-tls-local` diagnostic can temporarily use OpenSSL's default verifier
on one matching loopback world TLS object. Earlier trials completed TLS but closed before sending
application data. The launcher's `--enter-world` helper combines a guarded SSL callback change
with the temporary startup branch change. The verifier diagnostic alone is not a world-entry fix. See
[docs/launcher.md](docs/launcher.md) for the guarded diagnostic modes and limits.

## Source layout

- `src/protocol/protocol.*`: recovered descriptors/opcodes, application frames, dynamic protobuf decoding.
- `src/apps/replay_*`: offline and TCP-loopback replay.
- `src/auth/auth.*`: typed auth/lobby state and ticket boundary.
- `src/auth/native_auth.*`: opt-in native bootstrap and HTTP login/world adapter.
- `src/services/front/native_ws.*`: native auth/front WebSocket framing and character requests.
- `src/world/session/native_world.*`: experimental TLS world listener and bootstrap packets.
- `src/world/features/loot/loot.*`, `src/world/features/loot/loot_rules.*`, `tools/build_loot_data.py`: monster corpse loot (see
  [docs/monster-loot.md](docs/monster-loot.md)); the generated profile is read from
  `data/generated/loot/definitions.json` next to the catalog export. Guessed (custom) rules are on
  by default and can be switched off with `--loot-custom-rules off` or `--loot-rules-file`.
- `src/persistence/`, `migrations/`: memory/PostgreSQL persistence. `account_repository.hpp`, `character_repository.hpp` and `character_state_repository.hpp` are the account, character-row and revisioned character-state interfaces; `character_state_types.hpp` holds their shared row types, `postgres/character_rows.hpp` the shared SQL helpers, `memory_database.hpp` the in-memory tables, and `memory_repositories.hpp`/`postgres_repositories.hpp` the two backends (`character_repositories.hpp` assembles the set of three). The market, mail, ranking, hot-key and fog repositories live with their features in `src/world/features/{market,mail,events,hotkeys,fog}/*_repository.hpp` with `Memory*`/`Postgres*` classes, matching the guild, friend, company and housing stores that already lived there.
- `src/services/admin/admin_*`, `src/world/core/game_rates.*`: operator console API, log ring and live EXP rates.
- `admin/`: the `chronicles-admin` WebView2 desktop console for that API.
- `src/apps/server.cpp`: Boost.Beast asynchronous HTTP/WebSocket listener; blocking work runs in a worker pool.
- `protocol/`: recovered metadata plus SHA-256 import manifest. Only `x5msg` syntax labels are normalized to `proto3`; message layouts stay unchanged.
- `tools/`: schema generation, corpus comparison and integration tests.
- `launcher/`: WebView2 (Edge/Chromium) Windows launcher. The C++ controller starts the server,
  generates the local bootstrap, launches Steam, and runs the guarded world-entry helper through
  `playsharp-bridge.exe`. It retains a reversible endpoint redirect for clients started outside
  this path. The UI is plain HTML/CSS/JS in `launcher/web/`.
  See [docs/launcher.md](docs/launcher.md).

At startup `src/apps/repositories.cpp` builds one `Repositories` set from `--storage`.
Memory mode constructs a single `MemoryDatabase`; every memory repository guards its tables
with that database's one mutex, so commits spanning accounts, characters, items, market,
mail and company permits stay atomic. PostgreSQL mode hands every repository the same
`CHRONICLES_DATABASE_URL` connection string and each call still opens its own
`pqxx::connection` — there is no connection pool. Feature repositories that keep
per-character rows register `CharacterDeletionHook` callbacks on the shared
`MemoryDatabase`: mail erases a deleted character's recipient rows, open market escrow
vetoes the deletion, and fog erases its discovered areas. Tests build the same repositories
through `src/testing/memory_repositories.hpp`.

Source modules are the folders under `src/` listed in `tools/source_layout.json`; a file belongs
to the longest module folder that contains it. Each module folder has a `CMakeLists.txt` calling
`chronicles_add_module`, whose `DEPENDS` lists the modules it includes; the include check fails
when an include is missing from `DEPENDS`, when `DEPENDS` breaks the layer rules or lists an
unused module. `DEPENDS` entries may name a module in the same layer or in a layer listed for
the module's layer in `layer_dependencies`. Includes are written relative to `src/`. Tests live
in the module's `tests/` folder and build as one program per module. CTest runs
`tools.module_includes`, which enforces these rules; when it fails either move the include to a
lower header or add the module to `DEPENDS` in its `CMakeLists.txt`.

No captured traffic, account credentials or archive encryption keys are bundled as source inputs.

## World monster populations

`tools/start_persistent.ps1` now builds the world monster profile from the verified raw/catalog
export. The native listener automatically loads `world-mobs/definitions.json` beside the catalog.
To regenerate only that profile, run `python tools/build_world_mobs.py` from `Emulator/`.
The same startup also writes every playable map's civilians with
`python tools/build_stationary_npc_placements.py --all-maps`; the listener loads each
`npc/placements/map-<id>.json` beside the catalog, so every map announces its own NPCs.
Public maps share monster health, AI and life cycles; matched raid channels remain isolated.
See [docs/world-mobs.md](docs/world-mobs.md) for spawn coverage, local balance/selection policies,
validation and the remaining activation, navigation and original-client acceptance limits.
