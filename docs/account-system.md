# Named accounts and password login

Creation and operator password reset share an 8-byte minimum and a 256-byte maximum
with the launcher. Existing password hashes remain valid. Optional Discord browser sign-in
and explicit linking to an existing signed-in account are described in
[Discord sign-in](discord-sign-in.md). Enabling it requires private server OAuth configuration
and the app's registered redirects; a bot token is not a sign-in credential.

The PostgreSQL account table stores a unique, case-insensitive login name and a
randomly salted PBKDF2-HMAC-SHA256 password hash. Passwords are never written to
the database. Login returns a process-local, one-hour bearer token; a successful
relogin revokes only that account's previous sessions and world handoffs.
Character queries, creation, selection, world handoff, movement checkpoints,
inventory writes, and progression writes use the authenticated account ID.

## Create and verify an account

Start the private PostgreSQL cluster with `tools/start_persistent.ps1`, build
`chronicles-account`, then use the wrapper below. It prompts for a masked
password and passes it to the C++ account tool through standard input. The
password is not a command-line argument, saved setting, or logged value.

```powershell
cmake --build --preset release --target chronicles-account
.\tools\manage_account.ps1 -Action Create -Account player_one
.\tools\manage_account.ps1 -Action Create -Account player_two
.\tools\manage_account.ps1 -Action Verify -Account player_two
```

Account names are 3–32 ASCII letters, digits, or underscores. Passwords are
8–256 bytes. An existing account is never silently reset by `Create` or by
server bootstrap. The wrapper currently targets the private PostgreSQL cluster
on `127.0.0.1:5433`; use it on the server host. Do not share the database
password or open PostgreSQL to the laptop.

The emulator-owned `POST /emulator/v1/login` route accepts JSON with `account`
and `password` and returns `AuthChallengeRes` with an access token on success.
`POST /emulator/v1/logout` takes that Bearer token and returns HTTP 204.
An authenticated launcher can call `POST /emulator/v1/launch-grant` to get a
five-minute bootstrap URL for that account. Its first use activates an account-bound
reconnect credential for up to 24 hours. The launcher authenticates
with `client: "launcher"`, keeps that session in native memory, and reuses it for
fresh grants until sign-out, expiry, or closing the launcher. The game receives a
separate session when it consumes a grant. After a server restart the same running game
can reauthenticate and obtain a new session without launching again. Sign-out from the launcher preserves an
already running game. The launcher clears the password field and does not save the password,
bearer token, or grant in its settings or logs. Start the persistent server with
`tools/start_persistent.ps1 -RequireNamedLogin` when testing named accounts;
this rejects the old unprefixed Steam-login and bearer-alias development routes.
The default listener is loopback. Configured remote listeners now require HTTPS and strict
named-account grants; see [distribution backend](distribution-backend.md). Separate accounts can hold live
sessions simultaneously; logging out or logging in as one does not revoke the
other. Account and character separation is covered by the focused AuthService
test, and the existing account password was verified against the migrated
private PostgreSQL database using `chronicles-account verify`.

The activated URL is a bearer credential: anyone who obtains it can authenticate as that
account until it expires or is revoked. The backend stores only its SHA-256 digest, redacts
the path in its own logs, and requires HTTPS for remote origins. Reverse proxies and other
infrastructure must also redact launch paths. A new launch, password reset, account ban or
operator sign-out revokes the credential; a game logout or launcher sign-out leaves an
already activated game credential usable so the running client can reconnect. Initial grants
remain five-minute, one-use in-memory handoffs. PostgreSQL schema 32 is required; the memory
backend preserves reconnect state only while the store object itself remains alive.

## Game master accounts

An operator can give an existing account the game master (GM) role. The client
cannot request it. Apply the role on the server host, then sign in again; an existing
session keeps the role it had at login.

```powershell
.\tools\manage_account.ps1 -Action Role -Account player_one -Role gm
.\tools\manage_account.ps1 -Action Role -Account player_one -Role player
```

`chronicles-account role ACCOUNT player|gm` performs the same change directly; it reads
`CHRONICLES_DATABASE_URL` and asks for no password. For a disposable memory-store server,
set `CHRONICLES_BOOTSTRAP_ACCOUNT_ROLE=gm` together with the bootstrap account variables.
The role is stored in `emulator_accounts.account_role` (schema step added to
`migrations/001_accounts_characters.sql`, applied by the normal startup migration).
The [server admin console](admin-console.md) can also change roles, reset passwords and
ban or unban accounts (`emulator_accounts.banned`, schema version 14) while the server runs.

A GM account receives:

- `AuthChallengeRes.gm=true` at login, launch-grant exchange and login upgrade.
- Realm entry while the realm is in maintenance (listed as `GmOnly`) or full. The
  capacity exception is emulator policy.
- `WorldCharBaseNoti.gm=true` when the character enters the world. Static analysis of the
  current client build shows this enables the GM console on the **Tilde** (`~`) key and
  adds a `[GM]` tag to the network-latency display.
- Execution of GM commands. Type `/gm <command>` in chat (or the command alone in the
  Tilde console); the client sends it to the server as `GmCmdTextReq`, and replies
  arrive as private `[GM]` chat lines. The full command list, including teleports,
  god mode, invisibility, infinite stamina, NPC spawning, items, levels and vehicles,
  is in [GM commands](gm-commands.md). The official command table (`GMCommand.csv`) is
  absent from the exported client data, so the grammar is defined by the emulator.

Limits: a non-GM account's GM packets are refused with a private reply and logged. At
most four GM requests per second are accepted. The commands are covered by synthetic
tests in `src/world/features/gm/tests/game_master_tests.cpp`, `src/world/features/gm/tests/gm_tools_tests.cpp` and
`src/world/session/tests/native_world_tests.cpp`; an original-client GM session has not been observed yet.
Research notes are in `../dumps/ida_research/gm-20261026/FINDINGS.md`.

## Original game client boundary

The original Steam client sends `AuthSteamReq`; it does not show a password
screen or send the account password itself. The launcher performs the password
check first. The generated bootstrap URL carries a one-use grant through the
client's native auth requests, and the server exchanges it for a session
belonging to the verified account. The old shared `localdev` mapping and opt-in
native identity mapping remain loopback development shortcuts. A native identity
string is a claim the emulator has not validated with Steam.

After the initial exchange, that same URL can authenticate the account again until its
reconnect credential expires or is revoked. It does not depend on an unverified Steam identity.

The native password challenge has a recovered request/response schema, but its
password-shadow derivation and challenge calculation are not established. See
`NATIVE_PROTOCOL.md`. The grant exchange passes synthetic integration tests,
and the new HTTPS/WSS checks use synthetic clients. Earlier local original-client trials are
recorded in `docs/launcher.md`; a remote original-client world-entry trial remains pending. See
`docs/distribution-backend.md` before deploying a listener on another machine.
