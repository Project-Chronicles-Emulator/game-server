# Chronicles launcher

Current player/operator release and installer instructions: [Project Chronicles 0.2.0](launcher-release.md).
The older trials below retain their dated evidence and do not define current package behavior.

`chronicles-launcher` is a Windows WebView2 application with an artwork-led player home screen,
realm status, news articles, account sign-in and game settings. Its default **Named account** mode
uses the configured server: sign in from the account menu or press **Play** to sign in and launch.
The native host keeps the authenticated session in memory while the launcher stays open. Each
Play requests a fresh five-minute grant and generates an account-specific bootstrap archive.
Its first use activates a bounded reconnect credential in that same URL; see
[account-system.md](account-system.md) for expiration and revocation. Sign-in errors stay in the
dialog, and expired sessions require a fresh login. The
launcher clears the password field and does not save the password, bearer token, or grant in
settings or logs. Steam launches require the game to be closed before switching accounts; the
guarded private-copy target allows another client to start with a different fresh grant. Start
the persistent server
separately with `tools/start_persistent.ps1 -RequireNamedLogin`, and create named accounts with
`tools/manage_account.ps1` as described in [account-system.md](account-system.md).

The explicit **Disposable local test** mode retains the earlier development flow:

1. starts `chronicles-server` with a disposable memory-backed account and the captured tutorial
   bootstrap sequence;
2. generates the loopback TLS certificate for the prototype world listener when it is missing;
3. generates a build-pinned local `IniBootstrap` archive and launches the original client through
   Steam with `-textconfig -IniBootstrap=...`;
4. for clients started outside the generated-bootstrap path, can find the region list in process
   memory and rewrite official regional auth addresses to `http://127.0.0.1:<port>`;
5. keeps original endpoint bytes for that fallback and can restore them, and re-applies the
   redirect after the client's auth reset clears the current address;
6. automatically arms the guarded world-entry helper when the client is running. It handles the
   one-connection TLS callback and post-TLS startup branch changes that were previously manual.

No installed game file is modified. The endpoint redirect changes heap strings. The world-entry
helper additionally makes a guarded temporary two-byte executable change and substitutes one
live SSL callback pointer. It restores both after the connection trial. See
[world-entry-findings.md](world-entry-findings.md) for the evidence and limits.

## Build and run

The launcher is part of the normal build and adds the `webview2` vcpkg dependency (Microsoft
Edge WebView2 loader, pinned by the manifest). The WebView2 **runtime** is required at run time;
it is present on current Windows 11 installations.

```powershell
cd 'D:\PROJETOS\Archeage Emulator\Emulator'
cmake --build --preset release --target chronicles-launcher chronicles-world-tls-local
& .\out\build\windows\bin\Release\launcher\chronicles-launcher.exe
```

Settings are stored in `out/launcher/settings.json`. Server output, the generated certificate,
the generated bootstrap archive, helper log, WebView2 profile and runtime backups also stay under
ignored `out/launcher/`. Python and the pinned extracted config under `out/native-config/raw/`
are required to regenerate the bootstrap archive. The launcher reports missing inputs before
asking Steam to start the game.

The live restart recorded Steam passing the generated `out/launcher/IniBootstrap...bin` argument
to the game, even with an older local archive still saved in Steam launch options. Clearing
Steam's saved option is recommended for clarity, but the launcher action supplied the archive
used in that observed run.

The build copies `launcher/web/` beside the executable, and the UI loads that bundled
`web/index.html`. Re-run the launcher build target after HTML/CSS/JS changes to refresh the copy;
C++ recompilation is unnecessary for an asset-only change. A source-page browser preview remains
available but does not connect to the native controller.

The launcher hides in the Windows notification area when it detects a running game. When the
last matching game process closes, the launcher automatically returns, preserving its normal or
maximized size. Clicking its tray icon also restores it during play; subsequent status updates
leave that manually restored window visible. The executable, taskbar, window and tray use the
bundled gold Project Chronicles compass icon. `tools/build_launcher_icon.py` regenerates its checked-in ICO
with Pillow; normal builds do not require Pillow.

The running check requires the configured client's executable path and a live process handle.
An exited process with a retained handle is ignored. Client status is sent to the window before
optional memory scanning, and each PlaySharp bridge request has a 30-second response limit so a
stalled bridge cannot hold the last visible status indefinitely. An executable that remains live
without a game window still counts as running; the launcher does not infer whether the game is
playable from its window state.

Validation on 2026-10-26: the Release launcher build passed. An isolated native WebView2 host
smoke check passed ten assertions covering the embedded window icon, initial stopped state,
start-to-tray transition, manual restore, a remaining client, malformed state, last-client exit,
maximized restore and repeated stopped updates. Its harness and output are under ignored
`out/launcher-tray-candidate/`. This injects launcher status messages; it does not establish a
new original-game launch/exit trial or change protocol compatibility.

False-running-state correction on 2026-10-26: the launcher core and test target built in Release.
The process regression check passed with a live test process and an exited child whose handle was
still open. The launcher suite passed 72 tests with four optional skips. The observed launcher
instance had no matching game process while its last log line remained unchanged after a world
entry helper start; that supports stale status but does not identify which bridge request held
its worker. The corrected launcher has not yet been restarted or tried through an original-game
exit in this workspace.

Home uses the supplied harbor artwork, gold navigation and a prominent Play action. The readiness
strip reports client and lobby availability for development profiles and actual verified-download
progress for distribution profiles. News
articles are bundled offline content. Characters opens the game's existing selection flow, and
Store and Community state their current availability. Existing endpoint, redirect, server and
helper tools are under **Settings → Diagnostics** and **Activity logs**.

Distribution profiles pin the API origin, Ed25519 public key and patch channel in an adjacent
`launcher-config.json`. The launcher verifies and updates a managed game directory before login,
uses signed bootstrap templates without Python on player machines, and disables local memory
helpers. HTTPS/WSS and patch routes are served by the existing C++ backend. See
[distribution backend](distribution-backend.md) for local configuration, later hostname changes,
release publishing and recovery behavior.

For a local UI preview, place an empty `launcher-player-ui.flag` beside the development launcher.
It hides developer controls and restricts the Settings save action to the game folder while keeping
the existing local server, memory backend and client launch target. This marker is not a
distribution profile and does not make the development executable a player release.

Create an executable/UI distribution ZIP with `tools/package_launcher.ps1` and a validated
`-DistributionProfile` file. Packaging fails without it and rejects bridge, MCP executable and
driver files. Relocated executables
use an adjacent `runtime/` root; `--project-root PATH` selects an existing configured Emulator
directory instead. Runtime settings remain under that root's `out/launcher/`. The package excludes
local settings, credentials, game files and bootstrap inputs. See
[launcher distribution notes](../launcher/DISTRIBUTION.md) for prerequisites and current limits.

## CLI validation commands

These use the same controller as the window. The executable has a Windows GUI subsystem, so
some shells do not display its JSON output; check the exit code and generated files/logs there:

```powershell
# Read-only: no server start, no memory scan.
& .\out\build\windows\bin\Release\launcher\chronicles-launcher.exe --status

# Generate and validate a fresh local bootstrap archive without launching the game.
& .\out\build\windows\bin\Release\launcher\chronicles-launcher.exe --prepare-bootstrap

# Start the server if needed, discover the region list and apply the redirect.
& .\out\build\windows\bin\Release\launcher\chronicles-launcher.exe --patch

# Restore using the newest runtime backup (client must still be the same process).
& .\out\build\windows\bin\Release\launcher\chronicles-launcher.exe --restore
```

`--prepare-bootstrap` and `--patch` require Disposable local test mode. Named-account mode
requires the launcher Play action, which verifies a password and issues a new grant for each
launch.

## Guarded private-copy startup

Select **Guarded private copy** in Settings and set **Game directory** to a copy prepared with
`tools/prepare_private_client.ps1`. Install the experimental local shim in that copy with
`tools/xigncode_shim_experiment/install_private_shim.ps1` and enable its outbound firewall group
with `tools/private_client_firewall.ps1`. The launcher calls
`tools/launch_private_client.ps1`, which checks the private-copy marker, shim state, and game
executable firewall coverage before each launch. The original Steam installation is not
modified. Each Play action in Named account mode verifies the entered password and generates a
new one-use grant archive. Change the account name in Settings and enter that account's password
before launching another client. The launcher does not store passwords.

Two private client processes reached separate title screens concurrently with the shim. An
initial no-grant archive received HTTP 401 from the strict persistent server. In a later live
trial, two named test accounts were launched through separate GUI Play actions with distinct
one-use grant archives. Both copied clients reached the title screen and completed the launch
exchange: the server logged `PUT /launch/[redacted] -> 200` for each, followed by a native front
WebSocket packet ID 20001 (`CharListReq`) from each. Each client then created and selected its
own character. Separate PID-targeted world-entry helpers were active for clients 44128 and
26684. The server completed TLS 1.3 and accepted world credentials for characters 3 and 4;
both game windows concurrently displayed a wooded world-start scene. The helper logs report
restoration of the temporary startup bytes and SSL callback fields.

The server skipped its tutorial map bootstrap for both characters because its diagnostic
then required the Wild class on map 8000. It did not send `MapMoveStartNoti`, so the scene
does not prove server map enrollment or another player's presence. Peer rendering, movement
replication, equipment visibility, and a boot-time speedup remain unverified in this trial.
The launcher now enumerates every matching client in guarded private-copy mode and arms one
world-entry helper per PID. Each helper has its own `out/launcher/world-entry-<pid>.log`; the
Logs tab combines their tails. For a standalone diagnostic, the world helper can target a PID
explicitly:

```powershell
.\out\build\windows\bin\Release\launcher\chronicles-world-tls-local.exe `
    --enter-world --pid 12345 `
    --client-directory 'E:\ArcheAgeChronicles-LocalTrial'
```

With the rebuilt launcher, wait for the activity log to say each client PID is armed before
selecting its character. Each helper applies the build-guarded temporary TLS and startup
changes to its own process and restores them after the trial.

`--patch` can take a few minutes: a full writable-memory scan of this client covers roughly
13 GB, and discovery needs an endpoint scan plus pointer scans. The discovered manager is cached
in `out/launcher/client-session.json` and revalidated on the next run, which avoids rescanning
while the same client process stays alive.

## Redirect discovery and validation

Discovery never uses fixed addresses. It starts from string content and validates every step:

1. scan committed, private, writable memory for `https://auth-` / `http://auth-` (and, after a
   patch, for the replacement address);
2. read the whole candidate string and require an official `*.archeagechronicles.xlgames.com`
   auth address, or the exact replacement;
3. find pointers to that buffer, interpret each as an Unreal `FString`/`TArray` header, and keep
   only candidates that are the `auth_address` field of a plausible region entry (printable
   region name, plausible counts, valid address);
4. walk back to the first region entry, find the manager that owns the region array, then verify
   the array count/capacity, every region entry, the selected region name and the current auth
   address before writing anything.

The layout offsets used in step 3/4 are `AuthManagerLayout` in `launcher/src/auth_redirect.hpp`.
They come from the live client and the decompiled evidence in `../dumps/native_login_research/`:

| Offset | Meaning | Evidence |
| --- | --- | --- |
| `+24` | region `TArray`, 80-byte entries | live region array: `Asia`, `EU-West`, `NA` entries |
| entry `+0` | region name `FString` | live `NA` entry |
| entry `+16` | region auth address `FString` | live entry pointing at `https://auth-use-01...` |
| `+120` | selected region name `FString` | live value `NA` |
| `+208` | current auth address `FString` | `0x146187300` builds `api/auth` from it |

Bootstrap parse (`0x14618d4c0`) and reset (`0x146181600`) match this layout; the reset clears the
current address but keeps the region array, which is why the launcher re-applies on a timer.

Each write replaces only the text and the `NumElements` field of an existing `FString`. The
capacity must already fit the replacement; otherwise that target is skipped with a warning.
Writes are read back. Restore compares the live pointer, length and replacement text before
putting the saved header and text back, so a string changed by someone else is left alone.

### Agent-engine (AI chat) address

The client also hard-codes `http://modena.xlgames.corp:38011` as the base URL of its AI chat
service. While `agent_engine_redirect` is on (and `agent_engine_port` is not 0), the launcher
scans writable memory for FStrings holding exactly that text, verifies each header's pointer and
length, and rewrites them in place to `http://<server host>:<agent_engine_port>` with the same
patch/restore helpers (`launcher/src/fstring_patch.hpp`). This works in every launch mode, keeps
`agent-engine-backup-<pid>.json` in the settings directory, and is undone by Restore or on exit
with `restore_on_exit`. The replacement may not be longer than the official 32 characters. See
[agent-engine.md](agent-engine.md).

## Memory backend

Development settings can still select `playsharp-bridge.exe` (NDJSON over stdin/stdout) for
older protected-client research. The launcher never loads or unloads its driver. The bundled
`chronicles-launcher-memory` static library instead uses Windows process APIs for guarded
reads, writes and scans. It checks the selected process's executable path and liveness, bounds
requests and scans, and supports cancellation. Distribution profiles select this user-mode
backend and expose only the game folder in the player Settings page. The player package contains
no PlaySharp bridge, MCP service or driver. These library checks are synthetic until a matching
private client is tested through the full remote launch and world-entry flow.

## Server behavior

In Disposable local test mode, the launcher starts
`chronicles-server --storage memory --native-client local` with
`CHRONICLES_CLIENT_REGION` from Settings. A fresh random bootstrap password is generated per
start and passed only through the child environment. It is not logged or saved. Memory mode
means accounts and characters disappear when the server stops. When automatic world entry is
enabled, the managed server also receives `--world-handshake-delay-ms 2000` and
`--world-bootstrap-diagnostic true`. If an external server already owns port 8080, it must be
started with those diagnostic options to provide the tutorial packets and timing.

Named-account mode requires an external persistent server on loopback. It disables the old
runtime auth-address redirect so a post-startup rewrite cannot erase the account-specific grant.
Two copied original clients completed the named-account grant exchange, sent character-list
requests, and had their world credentials accepted in the live private-copy trial. In a later
trial, both loaded the tutorial map and saw the other equipped character. The server logged
peer-add and movement relay packets in both directions. The installed Steam client has not
been tested with this grant path. A laptop cannot yet connect to a server on another machine. See
[laptop-readiness.md](laptop-readiness.md).

Stopping from the launcher terminates only the child it started. A server that was already
running is shown as "external" and is not stopped. `stop_server_on_exit` and `restore_on_exit`
are opt-in; by default closing the launcher leaves the server and the redirect in place.

## Automatic local world entry and standalone diagnostic

The original client rejects the emulator's trusted world certificate in a protected verify
callback. The launcher now starts `chronicles-world-tls-local --enter-world` in the background
when the client and server are ready. In guarded private-copy mode, it starts one helper per
matching client PID. It waits for the protected code signatures to become available during
startup. Each helper watches for one matching loopback world
connection, temporarily uses the existing success callback and allows the researched startup
branch to continue, then restores both original values. Output is in the launcher Logs tab
and the per-PID `out/launcher/world-entry-<pid>.log` files.

The same executable retains isolated TLS diagnostic modes. Build and run it from `Emulator/`
while the server has its two-second handshake delay enabled:

```powershell
cmake --build --preset release --target chronicles-world-tls-local
.\out\build\windows\bin\Release\launcher\chronicles-world-tls-local.exe
```

Select a character within ten minutes after the helper starts watching. The longer window
covers the client's opening cinematic, which can consume most of the previous three-minute
limit. The default trial keeps `SSL_VERIFY_PEER` enabled and
temporarily sets this connection's custom callback pointer to null, allowing OpenSSL's ordinary
chain check. Pass `--verify-none` to repeat the earlier trial that keeps the callback installed
but sets the verification mode to zero. Pass `--always-accept-callback` to retain a non-null
callback pointer while using the client's existing six-byte function at `0x14186A540` that
returns one. This option tests whether the protected initializer requires a callback to remain
present; the tool verifies that function's bytes before using it. The tool requires the researched
client build, the expected Steam game process, a TCP connection from that process to
`127.0.0.1` on the configured world port, and the expected world connection/SSL ownership chain.
It checks the live code bytes against the saved IDA image and reads each write back. After five
seconds, it restores the original field if the same object still exists. If the connection ended,
its temporary object is already gone. These isolated TLS modes do not change executable pages
or game files; `--enter-world` additionally changes and restores the guarded startup instruction.

The early original-client trials completed TLS 1.3. In each, the client then reset the connection
before sending application data; the server's provisional credential read received 0 of 32 bytes.
Two follow-up trials on a restarted client recorded the same transition: the world Connection
went from a live socket and TLS wrapper with no send thread to neither socket nor wrapper,
still with no send thread. The protected startup initializer gates the first application write,
which explains why changing only the verifier did not establish a world session. The tool logs
only state flags and OpenSSL's verification result; it does not log credentials.

In a later controlled 2026-10-23 trial, the one-connection TLS diagnostic was combined with a
separate temporary post-TLS startup-branch patch. The client completed TLS 1.3, sent its
100-byte credential, received the opt-in tutorial map/start-playing responses, and displayed
its character and HUD on the tutorial terrain. The world socket remained active for ping/pong.
The executable branch bytes were restored and verified by readback; the TLS callback field was
also reported restored by the watcher. A fresh launcher-managed trial repeated TLS and world
entry with the starter outfit and a sheathed weapon visible in the playable world. The server
received map-load and start-playing requests. The exact protected acceptance rules remain
unknown. Short simulated `W` presses did not visibly move the character, and live checkpoint
recovery remains unverified.

The helper uses the selected game directory, memory backend and world endpoint. Current release
catalogs use the bundled user-mode backend and adjacent helper; legacy development configuration
retains PlaySharp support. The helper checks matching-build code bytes before a temporary change.
Hosted connections preserve the original certificate-verification mode and protected callback.

## Compatibility boundary

The launcher automates the bootstrap, redirect, and previously manual local world-entry trial.
Normal client startup without the helper still rejects the local world certificate. A controlled
trial demonstrated tutorial map loads and nearby-player visibility with two original clients.
The server checkpoints movement and sends nearby-player notifications in that map; two synthetic
TLS clients passed mutual visibility and movement tests. General map loading and gameplay
simulation remain unimplemented. See
`../VALIDATION.md` and `../NATIVE_PROTOCOL.md`.
