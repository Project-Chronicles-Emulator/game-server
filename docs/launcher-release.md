# Project Chronicles launcher distribution and release history

The current source defaults to the local server at `http://127.0.0.1:8080`, with world
connections at `127.0.0.1:8081`. It supplies no hosted service, signing key or Discord community.
See [project configuration](project-configuration.md) before packaging a hosted distribution.
Versioned checks below are historical implementation evidence; they do not establish a
published package under the current branding. Never distribute operator credentials.

## Player setup

The 0.2.1 hosted world helper handles both startup and TLS initialization for the
researched game build. It checks the selected certificate hostname before using
OpenSSL's default peer verifier and restores the temporary client changes after
connection. A fresh original-client trial reached the hosted world and the live
server accepted its credential. Local development retains its separate helper
behavior; unknown game builds fail closed.

Run the installer, or extract the complete portable ZIP and open `chronicles-launcher.exe`.
Use **Continue with Discord** for browser sign-in, or create/sign in to a Project Chronicles account.
To keep an existing account and its characters, sign in normally first and choose
**Link your Discord account**. A first unlinked Discord sign-in creates a separate account.
Both formats include the Windows C++ runtime, WebView2 loader and the verified Microsoft
Evergreen bootstrapper. Missing WebView2 is prepared per user before the launcher opens.
Internet access is required. No driver, bridge service, Steam client or Steam login is required.

The launcher discovers the demo through Steam's registered library directories and app manifest.
Without an explicit or already installed selection it selects the Steam game folder, otherwise
another existing Project Chronicles or ArcheAge folder in known local locations, and verifies and updates the
signed files in place, including in the Steam folder. Settings > Choose folder selects another
folder. A missing installation is downloaded automatically. The update cache and transaction
backups require additional free disk space during installation.

Both packaged launcher entrypoints allow the local realm. Additional realms come from the
operator's profile. Switching realms ends the previous realm's in-memory session and keeps
each realm's saved Windows credential.
Each service origin has independent signed-release rollback history in the installation marker.

An earlier unreleased unified candidate was built locally on 2026-10-27. Eight release
setup tests passed, and
the built launcher reported both live and local choices with live selected by default. The
existing published 0.2.4 packages were not rebuilt, and that candidate had no game trial.
Those historical packages do not describe the current local-default configuration.

The game release includes the matching Steam API compatibility library, its runtime DLLs,
network/overlay-disabled configuration, LGPL license and corresponding source. The launcher
installs those files through its signed, hashed update path; players do not copy DLLs manually.
The game bootstrap template is bound to the selected service origin and its launch grant.

## Connections and operator tools

The private admin console ships only a local profile using the existing private token
file. An operator-configured remote profile uses Windows OpenSSH with a protected key and pinned known_hosts;
the token remains on the VPS and the admin HTTP API remains loopback-only. No SSH key is bundled.
The generated admin profile references operator-local paths and must not be shared with players.

The hosted world helper matches the selected DNS host and port against a game-owned TCP peer.
It retains SSL_VERIFY_PEER and the client's protected callback. Its temporary startup change is
build guarded, checked by readback, and restored. The callback's complete verification policy is
not established; a failed hosted attempt requires restarting the client. Loopback diagnostics
retain their separately documented local exception. Other client builds fail closed.

Public communication literals use the existing Apache-licensed xorstr_ implementation from
PlaySharp Loader, with the SSE2 baseline. This obscures embedded strings; service endpoints and
public signing keys remain observable at runtime. Credentials and private signing keys are never
compiled into a launcher. Chronicles-owned launchers, server programs and helpers carry role icons and
0.2.0 Windows version resources; third-party executables are not modified.

## Build and package

Build with the documented Windows CMake preset, then run `tools/package_release.ps1` with
PowerShell 7.2 or later. It stages an explicit player-file allowlist, builds the portable ZIP,
and compiles the Modern UI NSIS installer. It refuses an existing output directory rather than
merging settings or credentials into a release. NSIS 3.13 and Microsoft's signed bootstrapper
are pinned in the private build dependency provenance. Packaging retains dependency licenses.
No code-signing certificate is configured, so Project Chronicles executables are unsigned.

The installer uses a per-user installation, Start menu/Desktop shortcuts and an uninstaller.
Uninstall removes its enumerated shipped files while preserving game, settings and other user
files. Both player formats use the same staged payload. Installer UI source is `installer/chronicles.nsi`;
icons and welcome art are generated by `tools/build_process_icons.py`.

## Release verification

The full Release build passed. The launcher suite ran 86 cases: 82 passed and four optional
backend-fixture cases skipped. The full CTest run ran 1,258 cases: 1,156 passed, 101 skipped,
and one Windows TLS case failed with SEH under normal OpenSSL CPU dispatch. That same case
passed using only a child-process OPENSSL_ia32cap=0 diagnostic; normal dispatch is unresolved.
The distribution integration test passed within the full run.

Both real admin profiles connected successfully. Player startup and WebView2 checks passed
with a minimal system-only PATH. A tampered operator profile with local selected was rejected
by the packaged player executable. Steam discovery located the installed demo on this computer.
The signed 0.2.0 managed game update verified successfully. The installer completed silent
installation and runtime checking, and uninstall preserved a user-created sentinel file.
Static scans found none of the selected plain API/world/public-key literals in packaged EXEs.
A clean computer without Steam installed and missing WebView2 has not been tested.

The live backend was upgraded after a fresh offsite backup passed a full isolated restore and
the additive migration. PostgreSQL was not restarted. The local backend saved its active world
session, exited normally, and was upgraded after another fully restored backup. Both services
are healthy. Local and hosted data remain separate; no progress was merged or reset.
Public registration was opened at the operator's explicit request on 2026-10-27 UTC. TCP 443
and 8481 accept player connections from any source. New-account registration, duplicate
protection, login, realm/roster access and the signed update catalog passed live HTTPS checks.
SSH remains operator-restricted and public admin access returns HTTP 403. The later 0.2.1
trial verified original-client hosted entry; no capacity or latency benchmark is claimed.

## Launcher 0.2.1 connection and cancellation fixes

Startup publishes the server catalog before network work. Operator builds can select the
local or live realm while the selected realm is unavailable. Three consecutive failed
health checks display "Failed to connect to the server"; a successful check or a server
change clears that failure. Player builds continue to show live realms only.

Cancel update also interrupts file verification, including an individual large file,
and immediately shows a cancellation request. Network waits use bounded per-operation
timeouts; a request already inside Windows HTTP may finish its current wait before
cancellation is observed. Final transactional installation still completes or rolls back
safely. Build outputs include the matching player or operator profile.

Startup, Play and Check game files validate the signed catalog and reuse the completed
installation record. The record fingerprints the signed paths, hashes and sizes, so
switching between realm catalogs with identical game files does not rescan the game.
Per-realm rollback protection, file presence and size checks, and hashes of launch-critical
Steam compatibility files remain active. A changed file set triggers a full inspection.
Settings > Verify / repair game files explicitly hashes every file and repairs changes,
including same-size changes outside the compatibility subtree.
Verification and actual downloading have distinct headings. Cancellation consumes the
scheduled startup update so it cannot restart immediately afterward.

## Live 0.2.1 release

Runtime source revision: `a9bb609632cd2a5b84b991f1b89d2593af011223`. The private backend
configuration enables Discord sign-in with the app's registered redirect. A restored offsite
backup and additive migration 30 preceded the switch. The first readiness failure restored
the prior backend; an isolated candidate passed, then a retry waited for the old service
wrapper to finish before starting the new backend. Existing data and player endpoints were
preserved, and public administration still returns HTTP 403.

The packaged player launcher completed real browser authorization, persisted the existing
account's Discord link, signed in again through Discord and launched the original client.
The guarded helper completed world entry and restored both temporary changes; the server
reported the connected world session and the operator confirmed playable entry.

The Project Chronicles bot published the installer, portable ZIP and checksums in
announcements (historical community link omitted).
The changelog and sanitized release-check log (historical community link omitted)
exclude credentials and private player/session values. All five attachments were downloaded
independently from Discord and matched their local SHA-256 hashes. The superseded bot
announcement now links to 0.2.1 and its old launcher attachments were retired.

Focused validation: 127 C++ cases passed, four optional fixtures skipped, isolated PostgreSQL
identity checks passed, and 16 distribution/backend/updater checks passed. This is not a full
gameplay or load-test result. The existing signed game catalog remains 0.2.0 sequence 2;
the launcher is 0.2.1 and the client build remains v26.10.18.734.

## Launcher 0.2.2 repair

The 0.2.1 report showed "Checking existing game files" at 5.39 GB while verifying
`X5-Windows.ucas`. That archive is approximately 41 GB. Verification previously updated
the display only at file boundaries, so the number could remain unchanged during a
full archive hash. Verification now reports progress every 16 MiB of file input. An
update reads an existing file set once under the installation lock rather than running
the same full scan before installation and again inside installation. Explicit Verify /
repair still reads every file and checks the signed hashes.

The managed launcher now checks hashes for the small Steam v161 compatibility subtree
even on the fast startup path. Before starting the game, it also requires the signed
`steam_api64.dll`, `libprotobuf.dll` and `abseil_dll.dll` in the managed game folder.
Missing, modified or unpublished compatibility files stop the launch with a repair
message instead of allowing the game to fail with `Invalid OnlineSubsystemSteam`.
The signed game catalog remains version 0.2.0 sequence 2; these changes affect the
launcher only. A separate player's machine has not yet been inspected, so its exact
game path and file state remain unconfirmed.

## Launcher 0.2.3 zone access

Managed Play now starts the researched all-zone helper for the exact client process it launches.
The player and operator packages include the helper. It checks the client path and build,
backs up the original catalog gate bytes, updates activation and map eligibility, and checks
the result before Play reports success. A helper failure is shown with its process log path.
The changes exist only in client memory; the signed game catalog and server are unchanged.

The Release launcher and helper built, and 24 focused process, release-setup and zone tests
passed. The same helper inspected and updated a running hosted client: 426 restricted records
changed, with zero remaining. The packaged automatic path and actual crossing still require
a fresh original-client trial.

## Launcher 0.2.4 candidate

Version 0.2.4 aligns the launcher, installer, portable ZIP, user agent and on-screen metadata.
It combines the 0.2.2 repair and 0.2.3 managed zone helper in one isolated Release build.
The server candidate also includes the measured world-mob tick improvement.
All 1,307 registered CTests passed with 104 optional skips using the documented process-local
OpenSSL test setting. The installer and portable files contain the zone helper and the live-only
player profile; their manifest hashes match. See `VALIDATION.md` for the remaining live checks.
