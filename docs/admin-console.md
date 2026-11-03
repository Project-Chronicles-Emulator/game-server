# Server admin console

`chronicles-admin.exe` (window title **Project Chronicles Server Admin**) is a desktop console for operating a
running `chronicles-server`. It talks only to the server's emulator-owned operator API under
`/admin/v1/`. That API is not a recovered official service, and no game client calls it.

## Start it

1. Start the server with an admin token file. The persistent helper does this by default:

   ```powershell
   .\tools\start_persistent.ps1
   ```

   It passes `--admin-token-file out/admin/admin.token` and
   `--admin-settings-file out/admin/server-settings.json`. Use `-DisableAdminConsole` to omit
   both. For a direct start, add the options yourself:

   ```powershell
   & .\out\build\windows\bin\Release\chronicles-server.exe --storage memory --port 8080 `
       --admin-token-file .\out\admin\admin.token `
       --admin-settings-file .\out\admin\server-settings.json
   ```

   The server creates the token file (64 random hexadecimal characters) when it is missing and
   reuses it afterward. Anyone who can read it controls the server. It lives under ignored
   `out/`; never commit, paste into chat or publish it.

2. Build and open the console:

   ```powershell
   cmake --build --preset release --target chronicles-admin
   & .\out\build\windows\bin\Release\admin\chronicles-admin.exe
   ```

   Select **Local testing** or an operator-configured server on the **Server** page. Selecting a
   preset connects immediately with existing operator access; it needs no address, token
   or SSH entry. It remembers the last connected server. Address/token fields are available
   under **Advanced: custom connection**. Options: `--server URL`, `--token-file PATH`,
   `--project-root PATH`.

## Automatic server selection

The console looks for its operator catalog in this order:

1. `%LOCALAPPDATA%\ChroniclesAdmin\admin-servers.json` (private, shared by admin builds).
2. `admin-servers.json` beside the executable.
3. The project's `out/release/current/admin/admin-servers.json` for development builds.

If no catalog exists, Local testing still uses the normal local address and token file.
The previous default connection saved as Custom migrates to its matching preset. Explicit
command-line connection overrides and genuinely custom saved connections remain available.
A late connection result from a previous selection cannot replace the current attempt.

The local preset reads the existing local admin token. The live preset uses the existing
operator SSH key and pinned host; the remote proxy reads its token on the VPS and calls the
loopback admin API. Neither tokens nor SSH keys are bundled into public player packages or
returned to WebView. Private profile files contain only connection details and credential
file references. The SSH key, trusted-host file and server-side proxy must already exist on
an operator PC; copying the public player launcher does not grant admin access.

Validation (2026-10-26, America/Sao_Paulo): the Release admin target built, the updated native
console connected to both local and live overview routes with only `--check-server` IDs,
and the opened desktop console displayed Connected on the live preset. JavaScript checks
exercise preset-only requests, hidden custom fields, repeated settings hydration and stale
local-failure rejection after a live switch, alongside the existing mail/message regressions.
Only the admin executable/interface and local operator catalog were updated. Server processes,
public access policy, credentials and account data were not changed.

## Realm editing and hosted persistence

Realm inputs use the last overview immediately. Input/change events mark the draft dirty;
a delayed remote overview cannot overwrite it, and edits made while a save is pending remain
unsaved afterward. Background chat polling pauses while a realm draft is dirty. The label is
the native enum (None, New, Recommended), not arbitrary text. The console reports Realm saved
only after `settings_saved: true`; temporary application and save failures keep the draft.

The hosted backend now uses `admin_settings_file` in its private backend configuration, pointing
to `C:/Chronicles/state/admin/settings.json`. Its existing Local Service account can modify that settings
directory, without write access to credentials or executables. The initial document preserves
the currently running realm, rates/events and automatic-message configuration. A guarded config
backup precedes enabling the path; disabled scheduled tasks remain stopped.

Validation (2026-10-26, America/Sao_Paulo): the existing three focused backend cases passed for
realm save/reload, invalid realm refusal and private message persistence. UI regression checks
cover delayed overview replies during editing, paused background chat, accurate persistence
feedback, edits during save and a successful final save. The active admin interface was updated
and reopened. The VPS was restarted once while its task was enabled and no world connections
were present. Realm fields survived that restart; two saves of unchanged live values returned
`settings_saved: true` and disk readback confirmed both file creation and replacement. Capacity
64 and label None were preserved; no game/server binaries, account data or credentials changed.
The automation adapter could read the interface but could not obtain actionable control
geometry; the later capacity follow-up below records operator keyboard acceptance.

Capacity keyboard input follow-up: the capacity control now uses a plain text input with a
numeric keyboard hint. Save validates decimal digits and the inclusive 1-100000 range before
sending an integer. An admin-only host option forwards native-window focus back into WebView2,
including after activation; the player's launcher keeps its existing default. The updated
Release admin executable/interface were rebuilt and installed. Boundary tests reject empty,
zero, signed, fractional, scientific, oversized and nonnumeric values without a server request,
and accept 1, 64, 99999 and 100000. The operator subsequently confirmed that selecting 64 and typing a replacement directly
works in the reopened native admin tool.
The focus operation follows the official
[WebView2 controller contract](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2controller#movefocus).

## Rates shown after reopening

The Rates page shows the selected server's cached connection overview immediately while a
fresh request runs. If nothing has loaded, it shows Loading rather than a hardcoded 1.0x.
A request revision and connection guard prevent a late settings/overview reply from replacing
a newer rate change or another server's values. Draft percentages survive refreshes. Save
feedback distinguishes `settings_saved: true`, temporary application and request failure.

Validation (2026-10-27, America/Sao_Paulo): the live backend returned 500% (5x), and its settings
file also held 500%, before and after reopening the updated admin console. The backend was not
restarted and its rate was not changed for this repair. The local realm separately remained
at its own 100% rate. Two existing backend rate/event persistence checks passed. UI checks
verify immediate 5x hydration, late 1x refusal after a 5x save, draft preservation, reopening
and explicit warning when a response reports a nonpersistent change. JavaScript syntax and
whitespace checks pass. No game protocol behavior changed.

## What it manages

| Page | Function | Notes |
| --- | --- | --- |
| Dashboard | Realm state and population, capacity meter, EXP rate and active events, uptime, server clock, ports, storage backend | Refreshes every 5 s. Quick maintenance and character-creation switches. |
| Realm | Realm name, site display name, capacity, realm-list label, maintenance, closed character creation | Applies to the next realm list and realm entry. Saved to the settings file. |
| Accounts | Search, create (player or GM), reset password, change role, ban/unban, sign out everywhere | A ban refuses login with `LoginDenyRestrictedAccount`, revokes sessions and disconnects world connections. A role change applies at the next login. |
| Characters | Search by character or account; details with currency, positions, quests, items | Level/EXP, gold/Hersander tokens, saved position, rename and delete. Edits require the character to be offline. |
| Live players | Every world connection with character, account, level/class, live position, address and connected time | Refreshes every 3 s. **Kick** closes the connection after the normal position save. |
| Send mail | Deliver a system message with item, Gold or Hersander-token attachments to an account or character | Default expiry is 30 days. Unchanged retries keep the same delivery reference to prevent duplicate rewards. |
| World chat | Live World-channel chat (player messages and operator notices), text filter, pause/follow, and a send box with a sender label | Polled every 1.5 s; the sidebar counts unread player messages. Clicking a player line opens that character. |
| Rates & events | Base EXP rate (1x, 2x, 3x, 5x, 10x presets or 0-1000%) and scheduled EXP bonus events | Effective rate = base + active event bonuses, capped at 1000%. Saved to the settings file. |
| Announcements | Manual chat/on-screen notices, recurring message rotation and a private welcome on world entry | Recurring and welcome delivery have independent switches. See the evidence limits below. |
| Live logs | Server log tail with level and text filters, pause/follow; server log level control | The server keeps the latest 5,000 records in memory. |
| Server | Connection settings, graceful shutdown | Shutdown requires typing `shutdown`. |

## Security model

- The API is disabled unless `--admin-token-file` is supplied. Requests need
  `Authorization: Bearer <token>`; the token comparison is constant time.
- Only loopback peers are accepted. `--admin-allow-remote true` permits other peers and requires
  HTTPS (`--http-tls-cert`). Prefer leaving it off and using a VPN or SSH tunnel instead.
- The console host adds the token; its web page never receives it and can call only
  `/admin/v1/` routes on the configured origin. Plain HTTP is accepted for loopback addresses only.
- Mutating actions are logged with the target ID only. Passwords, tokens and notice text are not
  logged. Storage errors return `service_error` without database text.

## Behavior and limits

- **Settings persistence.** Realm values and rates saved from the console are written atomically
  to `--admin-settings-file`. On the next start they override the corresponding command-line
  values (`--world-name`, `--world-capacity`, maintenance and so on). Delete the file to return to
  the command-line values. Without the option, changes last until the server stops.
- **EXP rate.** It scales server-awarded EXP: tutorial quest rewards and adventure-object EXP,
  from the next award on. Monster kills do not award EXP in the emulator yet, so they are
  unaffected. The default 100% leaves every amount unchanged. Overflow saturates.
- **Events.** Times are Unix seconds evaluated with the server clock: zero start means already
  started, zero end means no end, and the end is exclusive. An event only adds an EXP bonus; it
  does not start any in-game content.
- **Server time.** The dashboard shows the server clock. In-game time of day is not controlled:
  the only recovered candidate, `PS2C_WorldGameTimeNoti`, is a debug message with no captured
  example or established units, so the emulator does not send it.
- **Guild membership.** Deleting a guild member is refused until it leaves or disbands, preserving the live guild registry.
- **Offline-only character edits.** A connected world session owns its character's state and
  saves its position on exit, so edits to an online character are refused with
  `character_online`. Kick the character first.
- **Teleport.** World entry still accepts only the tutorial map or the server's loaded overworld
  map (`--npc-placements`). A saved position on any other map is refused at entry.
- **Delete.** Removes the character and every row it owns (items, stackables, quests, purchases).
  This is an operator action; the lobby's own deletion flow remains unimplemented.
- **World chat history.** The server keeps the latest 1,000 World-channel messages of the front
  chat service in memory only: lost on restart, never written to logs, the database or the
  settings file. It contains player chat text, so treat the console as private. Whispers,
  Party chat (world transport) and group channels are not shown.

## Announcement evidence

The automatic-message editor uses minutes for the interval, a channel selector for each
recurring entry, and one welcome chat message per text line. Saving restarts the interval;
`settings_saved` determines whether changes survive restart. See
[server-messages.md](server-messages.md) for limits and timing semantics.

- **World chat** sends `WSF2C_ChatNoti` (type World) through the existing front WebSocket chat
  service to every character connected there. The sender label replaces the character name.
  This uses the same implemented path as player World chat. Display of an operator sender label in
  the original client has not been observed yet.
- **On-screen notice** sends `PS2C_SystemMessageNoti` with free `text` and a Text, Banner or
  Popup type to characters in a map. The official captures set only the CMS `key`, so whether
  the original client shows free text is **unverified**. Treat it as experimental.

## API reference

All routes require the bearer token. Bodies and responses are JSON. Errors are
`{"error": code, "message": text}` with an HTTP status.

| Method and path | Purpose |
| --- | --- |
| `GET /admin/v1/overview` | Server, realm, population and rates summary |
| `GET`, `POST /admin/v1/realm` | Read or change realm settings (any subset of fields) |
| `GET /admin/v1/accounts?search=` | List accounts |
| `POST /admin/v1/accounts` | Create `{account, password, role}` |
| `GET /admin/v1/accounts/{id}` | Account with characters |
| `POST /admin/v1/accounts/{id}/password\|role\|ban\|kick` | Account actions |
| `GET /admin/v1/characters?search=` | List characters |
| `GET /admin/v1/characters/{id}` | Character with items and progress |
| `POST /admin/v1/characters/{id}/progress\|currency\|position\|rename\|delete` | Offline edits |
| `GET /admin/v1/players` | Live world connections |
| `POST /admin/v1/players/{connection}/kick` | Disconnect one connection |
| `POST /admin/v1/announce` | `{text, chat, chat_sender, system_notice}` |
| `GET`, `POST /admin/v1/server-messages` | Read or replace the complete recurring/welcome message configuration |
| `POST /admin/v1/mail` | Durable system-mail delivery with an explicit recipient, stable delivery key and optional attachments; see [mail.md](mail.md) |
| `GET`, `POST /admin/v1/settings` | Read rates or set `experience_rate_percent` |
| `GET`, `POST /admin/v1/events` | List or create events |
| `POST /admin/v1/events/{id}`, `/events/{id}/delete` | Replace or delete one event |
| `GET /admin/v1/logs?after=&limit=&level=` | Log records after a sequence |
| `GET /admin/v1/chat?after=&limit=` | Recent World chat after a sequence (`messages`, `next_after`, `gap`) |
| `POST /admin/v1/logs/level` | Set the server log level |
| `POST /admin/v1/server/shutdown` | `{confirm: "shutdown"}`; HTTP 202 |

## Source and checks

- Server: `src/services/admin/admin_api.*`, `src/services/admin/admin_log_buffer.*`, `src/world/core/game_rates.*`,
  `src/persistence/admin_storage.cpp`, hooks in `src/apps/server.cpp`, `src/auth/auth.*`, `src/world/session/native_world.*`,
  `src/world/features/chat/native_chat.*` and `src/world/features/tutorial/tutorial.cpp`; schema version 14 adds `emulator_accounts.banned`.
- Console: `admin/src/` (host and API proxy) and `admin/web/` (interface). It reuses the
  launcher's WebView2 host and WinHTTP client.
- Tests: `chronicles-admin-tests` (`src/services/admin/tests/admin_tests.cpp`) covers token handling, loopback and
  token rejection, realm validation and persistence, bans, password reset, listing and search,
  offline character edits, rename/delete guards, rates/events and the log buffer. These are
  synthetic checks. The original client's reaction to announcements and kicks has not been
  observed yet.

The console's mail form supports account mail or an optional character recipient, separate
mail/attachment expiry, and up to 32 typed attachment rows. Its delivery reference stays the
same after an uncertain response while the payload and connected server remain unchanged.
Even a repeated confirmed submit checks the existing delivery. Change the payload or choose
**New mail** for another intended delivery. The reference is retained for the current console
session; keep it if investigating a delivery after closing the console.

`node tools/admin_messages_ui_test.cjs` validates DOM ID wiring and the actual UI handlers
against a fake native host, including uncertain delivery retries, changed payloads, attachment
conversion, expiry limits, message channels and interval conversion. It does not contact a
server or send mail. `node --check admin/web/admin.js` checks JavaScript syntax. A native
desktop visual trial remains separate from these synthetic checks.


## Recover an uncertain offline character edit

Offline character writes use a persistent quarantine marker. A storage error can leave the
marker in place even when the edit committed. Affected characters cannot enter or be edited;
other characters remain available. Never remove the marker merely to bypass that protection.

The fresh-install distribution does not ship the old offline recovery or rollback SQL.
A quarantined character requires a separately reviewed recovery procedure against the
authoritative character/company records and a verified backup. Stop every writer and
keep database access blocked throughout that recovery. Do not delete quarantine markers
or reinstall the schema to make the character available. The database installer preserves
existing current-schema records and does not resolve uncertain edits.

The fence assumes one world-listener process protected by the deployment's existing lease.
It is not a multi-server distributed lock. Database connectivity failures during startup
prevent the world listener from accepting players.
