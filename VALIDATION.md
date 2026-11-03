# Validation — 2026-10-23

## Trade run resumed on foot: black screen after entry — 2026-11-02 UTC

- Observed (live, original client): a character re-entering with its trade wagon out saw the
  world render, then turn fully black about 3 s later while sounds, movement, skills and combat
  continued. Server logs showed a normal entry (20085, 20086, then movement and skills). The
  run had started in the previous session; earlier entries of the same character without a run
  rendered normally. An operator position edit made before this investigation had also moved
  the character and the saved wagon map to point of interest 30103, away from the wagon.
- Static client analysis (IDA saved; dumps/ida_research/trade-reconnect-fade-20261101):
  `AAC_Trade_FX5TradeManager_OnTradeStartNoti` applies an instant fade to opaque black that
  only the riding branch of the 0.5 s trade tick clears. TradeStartNoti is a departure packet.
- Server change `0b87e67`: TradeStartNoti at entry only for an owner already driving (region
  re-entry); an owner on foot gets the wagon and TradeWagonPosNoti, and TradeStartNoti after
  boarding (RideUnitReq) or a seating trade escape. See docs/vehicles-and-trade.md step 10.
- Synthetic checks: trade 23 and companion 39 unit tests pass; native TLS trade tests (3) pass
  with the region-transition fixture set, including a reconnection on foot that receives 207 at
  entry and 56 before 205 on boarding. Standard Release CTest registered 2240 tests with one
  environment-only failure (Market.ExportedCatalogAndEveryResponseRoundTrips needs
  data/generated in the worktree). Protocol reference regenerated; Page mirror not synchronized.
- Deployed 2026-11-02 02:39 UTC (server SHA-256 `4d171c27...`, replacing `7ad09c51...`) after a
  verified backup and rollback copies; only the server binary changed. Listeners, health, 605
  manifest files, schema 37 and player-state counts were verified afterward.
- Operator repair (character offline, one guarded transaction): the character and its saved
  wagon map were restored from 30103 to 9012, the character at its last client-reported
  standing position about 120 m from the unchanged wagon coordinates; revision 456 -> 457.
- Not established: original-client acceptance of the deferred announcement after boarding;
  whether selling a stowed run's goods after a reconnection needs a non-idle client trade state.

## Saved-death reconnect presentation and spawn-time discovery — 2026-10-28 UTC

Read-only hosted evidence identified a saved-dead character reconnecting and sending automatic
object-discovery requests. The client localization maps the reported modal to BadRequest. Entry
had restored zero health but announced Idle and omitted the restored death presentation. The
reporting character identity remains unconfirmed; see docs/dead-reconnect.md for evidence limits.

Three new regressions fail on the initial live 0.2.5 source baseline. The repaired Release build passes
25 focused reconnect/discovery/statue checks and the complete C++ executable: 1,036 passed,
94 environment-gated skips, zero failures (1,130 total). The native HTTP/WebSocket/TLS smoke
passes 39 checks, and the protocol-reference generator suite passes 11 checks. Native TLS
verification includes once-only death presentation, unchanged saved progress before revival,
completed shrine revival and a later observer seeing Idle with positive health. No original-client
dead-reconnect or revival UI acceptance is claimed.

The repair preserves the death and existing transaction semantics. It does not automatically
resurrect a character or replay damage, durability loss or rewards. Deployment status is recorded
separately from this synthetic validation.
The final candidate was rebased onto the subsequently deployed 0.2.7 source; all five
critical CTest cases and the full rebased suite pass without removing the new release
features. Fix-specific Page notes were initially confirmed at sequence 143. The subsequent
shared-main recovery reconciles the full generated reference with the private Page;
its publication record tracks confirmed complete readback.

### Shared-main pull recovery — 2026-10-28 UTC

The local 0.2.5 commit and the incoming release history diverged. The recovery retains both
parents and the saved-death repair, preserves the current player/operator launcher policy,
and records the older unified launcher experiment as historical evidence. The additive
schema 33 migration and current authentication, social, graduation, vehicle and world
latency behavior remain intact. Backups preserve the pre-resolution files and Git index.

An isolated Release build reuses the pinned dependencies, including OpenSSL 3.6.5, without
changing the running local game or its installation. Core: 1,036 passed, 96 optional skips
(1,132 total). Launcher: 122 passed, four integration skips (126 total). Other unit suites:
273 passed, 14 optional skips (287 total). Hosted TLS identity: seven passed. All 12
integration checks passed, including native world, social, mail, distribution and launcher
self-update. Protocol generator: 11 passed. Release publisher: two passed, one optional skip.

Five launcher release fixtures now explicitly select their disposable game directory;
this prevents the user's running Steam game from triggering the production switch guard.
No production guard was relaxed. Independent merge review found no lost current runtime
changes, conflict markers or whitespace errors. These synthetic checks do not establish
original-client gameplay acceptance.


## Project Chronicles 0.2.5 integration candidate — 2026-10-27

The `integration/release-025` candidate combines the 0.2.5 launcher (Credential
Manager saved login, in-place Steam folder use, self-update mechanism), friends,
companies, guild levels and guild housing, utility bag persistence, world latency and
monster combat fixes, and three release-blocker repairs reviewed in this integration:

- **Account revocation:** ban, kick and password reset cancel pending world admission;
  a stale handoff cannot enter after reselection, ticket expiry or revocation; storage
  failures quarantine the account in-process. New `AuthTest` and real-socket
  `NativeWorldAdmission.*` tests; a mutation check (claim forced to succeed) fails five
  of them. The unbuilt `world-admission-gate-v2` and `world-revocation-barrier` drafts
  were not used (their socket tests could not reach the server pause point).
- **Guild housing cleanup:** cleanup exceptions no longer terminate the server, and
  housing left by a lost/failed leave, kick or disband cleanup is reconciled from the
  guild registry at startup and every housing sweep. New PostgreSQL regression
  `HousingStorage.PostgresRestartRepairsGuildHousingWhoseCleanupWasLost`.
- **Connection bounds:** defaults raised from 128 to 512 world sockets, 1,280
  HTTP/WebSocket connections and 1,024 front chat registrations for the 500-player
  target. These are resource bounds; no load test was run at these sizes.

Release build (Visual Studio 2026, vcpkg x64-windows, OpenSSL 3.6.5 override). Full
CTest: 1,536 registered, 1,421 passed, 114 environment-gated skips, one launcher
saved-login case that failed only under parallel Credential Manager use (3/3 serial
passes; launcher tests now hold a CTest resource lock). TLS processes used the
documented `OPENSSL_ia32cap=0` diagnostic. Disposable PostgreSQL: `test_postgres.py`
(including housing PostgreSQL cases), `test_reconnect_postgres.py`,
`test_discord_postgres.py` and `registration_smoke_test.py` passed.

Migration rehearsal on a local restore of a fresh live backup: the candidate server
applied the migration twice (schema 32 → 33, adding friends, friend presence and
character company tables) with every existing table's row count unchanged, and the
0.2.4 production binary with its own migration also started healthy on the migrated
schema (binary-only rollback). No original-client gameplay, hosted load test or
500-player capacity result is claimed.

## Project Chronicles 0.2.4 release candidate — 2026-10-27

The isolated `release/chronicles-0.2.4` branch combines launcher file verification,
Steam compatibility preflight, managed zone unlocking and the world packet/tick
latency change. The full Release build succeeded. A focused set of 58 launcher,
zone, NPC and world-mob tests passed, with two optional generated-profile skips.
The complete 1,307-case CTest run passed with zero failures and 104
environment-gated skips after copying the generated catalog fixture with a
matching manifest hash. The native TLS test run used process-local
`OPENSSL_ia32cap=0`; without it one pre-existing bag-unequip socket test raises
an OpenSSL dispatch access violation. This setting is a validation workaround,
not a claimed runtime repair.

The 0.2.4 installer and portable candidate contain the zone helper and only the
player launcher profile. Their recorded SHA-256 and byte counts match the files,
and the installer payload lists the same helper. A later original-client trial
on the verified live 0.2.4 server displayed 135 ms RTT on tutorial map 8000, including
during combat, against five ICMP samples of 144–145 ms. The shipped player launcher
entered the world and its TLS helper restored both temporary changes. The separate
zone helper reported `Invalid zone catalog pointer`. A later HUD observation on public map
9012 showed 132 ms RTT with one player, confirmed by an authenticated server read on the
same binary. Original-client zone crossing and a 500-player load result remain unverified. See
`docs/world-latency-release-20261027.md` for the exact revision, binary hash and scope.

## Mage elemental bursts: area spawn hits and 32-bit area ids — 2026-10-27

Read-only live server logs from 2026-10-27 showed every cast of skills 2274 and 2275
applying only the zero-coefficient interval effect 143514 (logged damage 0). The
official mage capture places each of its nine burst areas on the StartSkillReq target
and applies the area's `aoeSpawnSkillEffectId` at the created tick with the area unit
as caster; 113 area spawns with such an effect across the capture corpus agree.

The profile builder now exports `spawn_effect_id`, and combat applies it once when the
area appears. Area unit ids use the 32-bit block `0xD0000000 | sequence << 4 | 3`
instead of a bit-60 tag. Regenerated tutorial, combat and world-mob profiles differ
from their reproducible baseline only by added keys and effects.

Checks in an isolated Release build: `Combat.AreaSpawnHitLandsAtTheCreatedTickFromAThirtyTwoBitAreaUnit`
and, with `CHRONICLES_TUTORIAL_DEFINITIONS`, `Combat.StaticProfileLightningBurstLandsItsSpawnDamageOnTheTarget`
(2274 deals effect 160505 damage to its target) pass. All 149 Combat, CombatItem and
WorldMobs cases with the profile variables set either pass or skip only for absent
hash-pinned historical profiles. 100 Python builder/world-mob cases pass. The full test
executable ran 997 cases: 932 passed and 8 NativeWorld cases failed. The inspected
failures compare world-entry opcode order (126/128/130/176/78/33), which this change does
not touch, while `native_world.cpp` carries concurrent uncommitted edits; they were not
investigated further. No original-client trial or live deployment of this change has
been performed.

## Player launcher file verification and Steam preflight — 2026-10-27

The reported 0.2.1 screen remained at 5.39 GB while displaying
`X5/Content/Paks/X5-Windows.ucas`. The live signed game catalog lists that archive
as 41,065,461,616 bytes and includes the Steam v161 compatibility DLL and its two
runtime libraries. The previous verifier emitted progress only at file boundaries,
and a repair scanned existing files before acquiring the installation lock and then
again inside the installer. Source changes now report progress within a file,
perform only the locked scan during an update, hash the small compatibility subtree
on the fast startup path, and refuse a managed launch without the signed Steam DLLs.

The local Steam source DLL differs from the signed compatibility DLL. The existing
managed local copy's six Steam compatibility files matched their live signed catalog
hashes. The focused launcher build succeeded. Twelve installer/compatibility tests
passed, including progress within a large existing file, cancellation and same-size
DLL modification. The full launcher suite passed 99 cases and skipped four optional
integration fixtures. These checks do not establish the affected player's exact
game path, whether their 41 GB verification completed, or an original-client trial
of the new launcher on their computer. No backend or signed game catalog changed.

## No-Steam client realm discovery — 2026-10-26

Hosted world preparation (2026-10-26): the former hosted endpoint on port 8481
was reachable, and a normal TLS 1.3 connection validated its matching DNS certificate.
The historical hostname and address are omitted. The guarded hosted helper source now watches only a
game-owned connection to the selected DNS endpoint and retains the original client's
`SSL_VERIFY_PEER` mode and protected certificate callback. It may change the researched
startup gate only for that matching connection, then restores it. This is an endpoint check
and source review, not a compiled helper test or original-client hosted-world result. Earlier
live inspection showed the protected callback rejecting a chain-trusted local certificate with
`X509_V_ERR_APPLICATION_VERIFICATION` (50), so public certificate trust alone cannot establish
hosted client acceptance. The shipping world-entry gate remains open until a fresh original
client reaches the hosted world with the callback intact.

An isolated copy of the original Windows client `v26.10.18.734` loaded the clean Release
Steam API compatibility payload built from the pinned Goldberg source and the reviewed v161
patch. Steam was still running during this first realm trial, so it did not establish Steam
independence. The client displayed the green NA realm named **Project Chronicles Private Server**, accepted
its displayed **F Log In** shortcut, and showed the existing level-2 character
selection. The still-running loopback server logged successful native auth and realm requests,
followed by a front WebSocket packet. The original game executable was not modified, and
Steamless/RUNEAutoCracker were not applied.

The same isolated client later selected that character with the guarded local world-entry
helper using ordinary Windows user-mode process access. The initial unassisted connection
reached the server but failed its TLS handshake after the certificate. With the helper armed,
the loopback server completed TLS 1.3, accepted the native world credential and sent map and
bootstrap data. The client rendered the world HUD and an action in the tutorial area. The helper
restored and read back its two-byte startup gate and restored the original SSL callback field on
the same TLS object; the helper log is under ignored
`out/steam-compat-test/world-helper-usermode-B.log`. This is a local, build-guarded trial on the
researched client, not a hosted-world or general client-build result. The launcher now passes
its selected memory backend to the helper.

A fresh launcher-driven trial used the same isolated game copy with `steam.exe` absent. Play
issued a new grant and started the game; the client displayed **Project Chronicles Private Server** in NA,
then entered a playable overworld with the HUD visible. The launcher automatically armed the
user-mode helper for that new client PID. Its log records the temporary startup-gate and SSL
callback changes and their verified restoration. Windows showed an established game-owned
loopback connection to port 8081, `/health` reported one online world session, and the game
loaded the compatibility DLL from its own Steamworks v161 directory. `steam.exe` was still
absent at the final world check, then was restarted to restore the user's desktop state. A
machine with no Steam installation or Steam service was not tested.
The Release launcher, helper and launcher tests rebuilt after the handoff change; 76 launcher
tests passed and four optional backend-fixture tests skipped. The older launcher was replaced
and reopened while the game was running. Its Home view showed **Lobby online**, the realm name
and the running-game state. A later reopened launcher briefly hid after a delayed status
transition. The window now moves to the tray only after a launch requested in that launcher
instance. The rebuilt launcher stayed visible beside the running game after this correction;
its Home view showed **Project Chronicles Private Server**, **Lobby online** and **1 / 500**. Steam's restored
sign-in prompt was dismissed, leaving `steam.exe` absent while the game remained connected to
port 8081.

An earlier local trial returned HTTP 401 when the launcher renewed its password session during
the client intro, revoking a pending launch grant. `AuthService` now gives each issued grant its
own five-minute lifetime and preserves it across launcher password renewal. Explicit sign-out,
password reset and suspension continue to revoke grants. The Release `chronicles-tests` target
built successfully and three focused AuthTest cases passed, including the renewal regression.
The live loopback server process predates this source change, so the grant fix is not yet live.
Hosted world entry, signed game-release delivery and an updated server remain separate gates.
At the final local check, `/health` reported native login and the world listener ready, the
realm at 1/500, and PID 51732 listening on loopback ports 8080 and 8081.

## World mobs and gathering map-load correction — 2026-10-26

The human reconnect trial on the previous typed-ID build still produced invisible attackers.
A private 94-frame native-log snapshot confirms NPC skill notifications from an actor absent
from all preceding UnitAddNoti packets. The gathering adapter overwrote the outgoing map-load
batch after combat had queued its initial actor snapshot and marked it announced. Gathering
now appends its snapshots and results, preserving other services' packets and broadcasts.

The isolated C++20 Release server and main test binary rebuilt successfully. Of 31 selected
cases, 30 passed and one optional generated-gathering-profile case skipped because its specific
environment variable was unset; rerunning that case with the generated profile passed. All
31 distinct cases therefore passed, including 11 WorldMobs, 17 gathering cases and three
real-catalog native TLS cases. The added gathering-and-mobs TLS case checks both clients'
initial monster announcements, shared death, persisted EXP, killer-only loot and replay-safe
loot. The TLS cases use the documented child-process OPENSSL_ia32cap=0 diagnostic. This is
selected synthetic validation, not a full-suite run or original-client gameplay acceptance.

The tested server (`c23945bef1fb48a54b098cab71164092c00cd375a54947fa5bf07356d7048304`)
replaced the live loopback service at 17:35 America/Sao_Paulo. Both listeners and the
storage-backed overview/players routes passed readiness. PostgreSQL remained on its original
process; no migration or mob-profile change was made. The previous executable is retained
for rollback. Ignored evidence is under `out/mob-gathering-fix-20261026/`.

The open original client initially returned AuthChallenge HTTP 401 after the restart
invalidated its launch grant. A fresh launcher sign-in/Play then entered successfully.
Observed original-client screenshots show visible wolves, a combat effect and the target
health bar. The private 549-frame local log contains 54 player skill requests, 23 damage
notifications, three deaths, three EXP increases and three successful corpse-loot results.
Each of the three killed placements was re-announced at its original spawn pose with a fresh
ID about 12 seconds after death. All eight recorded NPC casts have a preceding UnitAddNoti.
This establishes the reported visibility failure is repaired in that session and observes
combat/reward/respawn packet flow; idle walking animation remains under visual acceptance.

The game, launcher, server and PostgreSQL processes subsequently exited together. No matching
Windows Application Error or Windows Error Reporting event was found; the cause is unknown.
The existing PostgreSQL cluster performed normal WAL recovery and became ready, and the same
tested server artifact was restored at 17:43 America/Sao_Paulo. Both listeners and the
storage-backed admin routes passed readiness again. No migration, database reinitialization,
account/password change or profile replacement was performed. Recovery metadata is in
`out/mob-gathering-fix-20261026/recovery-running.json`.

Protocol reference generation with explicit pending source reviews and all 11 generator tests
pass. Strict `--check` still fails on the shared checkout's outstanding owning-source reviews;
their hashes were not silently refreshed. The Page accepted eight guarded correction updates,
but follow-up readback returned `page_read_forbidden`. Publication fingerprint remains at its
last fully verified value, and the latest live-trial/recovery evidence is not mirrored there.

## Nui Statues, offerings and shrine revival — 2026-10-26

The scoped implementation and evidence boundaries are in [Nui Statues](docs/nui-statues.md).
Release tests/server/account targets built in the existing Windows preset. Focused CTest:
36 passed, two PostgreSQL gates skipped, zero failed; both skipped cases then passed in the
isolated PostgreSQL run (seven C++ persistence cases plus account/lobby/restart checks).
Surrounding storage, stats, progression, combat and tutorial selection: 159 passed, 26 optional
gates skipped, zero failed of 185. Python: 10 Nui export/capture + 3 tear collection + 11
reference-generator checks passed without skips. All ten official P1/P2 statue activations
join to the corrected common-layer export (788 placements: 512 statues and 276 flames across
118 catalog map IDs; these are not hosted-service counts).

The native TLS tests cover per-character activation, offering, reconnect and flame revival
with dead state retained until teleport completion and atomic supply/destination/life commit.
TLS used the documented `OPENSSL_ia32cap=0` diagnostic; normal CPU-dispatch behavior is not
certified. The private disposable PostgreSQL cluster was stopped, and no live service or
existing player database was replaced. No original-client trial or full-suite pass is claimed.
Sanctuary trigger rules, PK field channels, zone-completion hooks and exact official refill,
penalty and reply-order semantics remain unresolved or unsupported. Structured local evidence:
`out/nui-statue/validation.json`.

## Mounts, vehicles, mates and trade runs — boarding correction, 2026-10-26

The boarding fix replaces high-bit runtime companion IDs with a low-32-bit block and adds
neutral `MoveSpeedRate` (stat 84 = 1000) to summoned mounts. The original client's per-unit
notification dispatcher truncates an addressed unit ID before looking it up; the old bit-62
tag therefore prevented `MountUnitNoti` and `MoveNoti` from finding an otherwise visible
wagon. The failed pre-fix local trial and static evidence are recorded under
`../dumps/ida_research/vehicle-boarding-20261026/FINDINGS.md`. Thirty functions were indexed,
annotated and cached; the matching `.i64` save returned success with stored-section verification.

Release `chronicles-server`, `chronicles-tests` and `chronicles-vehicle-tests` built using the
existing `out/build-gm2` cache. The tested server SHA-256 was
`786cbaba8cb960919409156c4764fd6ed2820f83ec67d5204b3f207503f3d26f`.

- Vehicle suite: 33 registered, 31 passed, two PostgreSQL-gated cases skipped. It covers
  ownership/seat/range refusals, visibility, ridden movement, dismount, repair, destruction,
  catalog rules and memory persistence, plus new low-32-bit ID and neutral-speed regressions.
- With the catalog and map-9012 placement environment configured, the TLS tests
  `NativeWorld.SummonedHorseCarriesItsRiderAndNearbyPlayersSeeTheMountMove` and
  `NativeWorld.GmSpawnedWagonIsBoardedDrivenWatchedAndLeft` passed. The new GM-wagon case
  exercises spawn, boarding and response order, repeat-boarding refusal, watcher movement,
  dismount and reboarding through the real listener.
- The existing trade-route TLS case failed because its Ileste merchant is absent from the
  current placement export. The broad main-suite snapshot registered 865: 803 passed,
  60 skipped and two failed (that placement fixture and the previously documented
  `NativeWorld.BagUnequipWithCarriedWeightAboveDefaultSafeWeightHasNoRefusal` access violation).
  This is not a full-suite pass. Raw local test logs remain in ignored `out/vehicle-work/`.
- No schema or storage behavior changed; runtime companion IDs are transient. Earlier
  PostgreSQL companion tests remain historical evidence; they were not rerun here.

Post-fix original-client mount animation, forward/reverse, steering, braking, dismount and
reboarding acceptance remain pending. Computer Use could not start because its Authenticode
verification helper could not load `Microsoft.PowerShell.Security`; no fallback input method
was used. The active loopback backend was replaced concurrently by another local workflow;
this session did not stop that process or overwrite its deployment. The built executable above
is ready for coordinated adoption, not a claim that the current game session has been verified.

## Player marketplace — 2026-10-26

The shared Release `chronicles-market-tests` and `chronicles-server` targets built with the
existing `out/build-vehicle` dependency cache. The tested server SHA-256 is
`b6b2a0e6f3fc1601827a94cb0bbc05c84c732e7dcceafc3ead84c2e08c3551a9`.
This checkout also contains concurrent feature edits; these results validate the marketplace
contract and the selected integrations, not the entire checkout.

- All nine non-PostgreSQL marketplace cases pass: ownership, escrow, independent settlement,
  exact-once receipts, expiry, revoke, filters/pagination, revision conflicts, competing
  purchases, equipment attributes, registration retry, capacity, forged deltas and wire enums.
- The PostgreSQL marketplace case passes in the stopped disposable cluster. It verifies
  store recreation, persistent item/proceeds claims, purchase and claim races, deletion
  blockers and stale-revision rollback. The existing isolated PostgreSQL runner also passes.
- The two-player HTTP/TLS marketplace smoke passes all seven requests, real allowlisted
  material escrow, independent buyer/seller claims, repeated-claim refusal, history,
  revocation, item return and subsequent mail claims after tutorial revision adoption.
- Six existing integrations pass: memory, native authentication, native chat, native guild,
  native world and mail. The reference generator's 11 tests and Python compilation pass.

Logs are under ignored `out/market-*.log`, with unit XML in `out/market-unit-tests.xml`.
TLS tests use the existing child-process `OPENSSL_ia32cap=0` diagnostic. Normal OpenSSL
dispatch, original-client auction UI/time units/order, official fees/price bands and other
undocumented policies are not established. There was no production migration, running-server
restart, deployment or original-client trial. See [marketplace.md](docs/marketplace.md).

## Marketplace expired-listing retrieve — 2026-11-01

An original-client screenshot showed "Retrieve" on an expired own listing refused with the
`Market_NotSellRegisterCancel` text. The saved client database shows that this action sends
`MarketRevokeReq` (see `dumps/ida_research/market-retrieve-20261101/FINDINGS.md`). The server
now settles a closed, unsold listing when its seller revokes it, and the registered-by-me
query omits seller-settled listings.

- Release `chronicles-market-tests`: 13 passed, and the PostgreSQL case skipped without a
  database. The three new cases cover retrieving an expired material stack and expired
  equipment with its attributes, retrieving a cancelled listing, the sold-listing refusal,
  repeated-retrieve `Market_AlreadyClaimed` and hiding settled listings.
- `tools/test_postgres.py` passed in a stopped disposable cluster. It includes the extended
  `Market.PostgresSettlementPersistsAcrossStoreRecreation` retrieve and query checks.
- `tools/market_smoke_test.py` passed all seven TLS requests.

The tree also contained unrelated in-progress combat, gathering and session edits that were
compiled into the server binary used above. This is synthetic verification only. There was no
original-client retrieve trial, production migration or deployment.

## Mail, scheduled messages and player commands — 2026-10-26

The shared Release sources built as `chronicles-server`, `chronicles-tests`,
`chronicles-admin-tests`, `chronicles-admin` and `chronicles-account`, reusing
`out/build-vehicle` and the existing vcpkg cache. The tested server SHA-256 is
`98795879da2269cf8d7eee9d83f37f89e635a243584e141537cb72492ddb93fd`.
Results are under ignored `out/mail-chat-validation/`:

- The final 505-case main C++ run passed 443, skipped 61 optional environment/catalog
  cases and failed one concurrent GM test,
  `NativeWorld.GameMasterSessionCommandsTeleportProtectAndReply`. Its rapid sequence
  exceeds the existing four-GM-commands-per-second limit; the log records those refusals.
  Player commands use a separate dispatcher and limit. This is not a full-suite pass.
- All 26 mail, message, player-command and PvP checks passed; the additional PostgreSQL
  mail case was skipped in that ordinary run, then passed against the disposable database.
- All 32 operator API tests passed. The disposable PostgreSQL runner passed all five
  selected storage cases, including atomic mail rewards, revision/ownership enforcement,
  durable claim receipts and native reward encoding. Its private cluster was stopped.
- Both isolated HTTP/TLS smokes passed. Mail covered delivery, list/read/delete,
  concurrent/repeated claims, owner-only notifications and consecutive tutorial claims.
  Messages covered welcome-once after StartPlaying, private help and PvP status,
  recurring chat/banner delivery, persistence and disabling delivery.
- Five existing CTest integrations passed: memory, native authentication, native chat,
  native guild and native world. The operator UI logic check and nine reference-generator
  tests also passed.

TLS checks used the existing diagnostic `OPENSSL_ia32cap=0` only in their child process
environment. This does not resolve the separately documented normal-dispatch OpenSSL
failure. These are synthetic checks; there was no running-service restart, deployment or
original-client mailbox/chat/banner/PvP trial. PvP flag replication is implemented;
shared player-versus-player damage remains unimplemented. The stock client consumes
unknown slash aliases locally, so the supported player input is `!help` and
`!pvp on|off|status`; forwarded `/` aliases remain supported by the server.

Windows x64, Visual Studio 2026 / MSVC 14.51, CMake 4.2.3, C++20, Release configuration.
Pinned dependencies include Boost 1.89, protobuf 5.29.5, libpqxx 7.10.4 and OpenSSL 3.6.

## Results

| Check | Result |
|---|---|
| CMake configure and complete Release build | Passed |
| Native capture replay over a real loopback TCP socket | 17,055 / 17,055 records passed |
| Application frame encode/decode semantic round trips | 17,055 / 17,055 passed |
| Observed opcodes / recovered mappings | 128 / 345 |
| Decode errors, unknown opcodes and unknown fields in the corpus | Zero |
| Comparison with independent Python decoder | All 17,055 records agree |
| C++ unit tests | 29 passed |
| Live HTTP / binary protobuf / WebSocket integration, memory backend | Passed |
| Native HTTP and binary auth/front WebSocket integration | Passed with synthetic local login |
| TLS world entry, clock, ping/pong, exit and rejection checks | Passed with provisional credential fixture |
| Last complete CTest run | 62 / 62 passed |
| PostgreSQL migration and live integration, PostgreSQL 16.15 | Passed |
| Account login and character persistence across server restart | Passed |
| Original-client bootstrap, Steam mapping, login upgrade and realm entry | Observed HTTP 200 |
| Original-client character creation, selection and friend-list reply | Observed with PostgreSQL backend |
| Original-client world connection, normal client | Reached listener; rejected local certificate before TLS completed |
| Original-client world connection, temporary diagnostic | TLS 1.3, 100-byte credential, tutorial map and HUD observed; ping/pong sustained |
| Bidirectional native packet slicer | Synthetic partial-tail and C2S/S2C boundary tests passed; 528 official frames captured |
| Starter equipment and saved customization | World packet and lobby create/list unit tests passed; clothed character and sheathed weapon observed in the local tutorial world |
| Official 2026-10-23 BR world entry | 528/528 frames decoded and round-tripped; inventory arrived before `MapLoadCompleteReq`; skill requests observed; no client movement request in this run |
| Tutorial movement persistence | In-memory round trip and PostgreSQL migration/smoke passed; live database checkpoint observed; moved-position reconnect pending |
| Nearby-player replication | Synthetic TLS add, move, and remove test passed; two original clients appeared together with equipped gear, and server logs recorded peer adds and movement relays |
| Client data export | 291 catalog files and 11,720 raw files copied; all 12,011 hashes verified |
| Schema version 3 upgrade | Existing private PostgreSQL cluster migrated in one transaction after backup; two characters, 18 starter items, and saved position retained |
| Isolated PostgreSQL test cluster shutdown | Confirmed by successful runner completion |

The immutable source capture is `native-log-final.bin`, SHA-256:

```text
96b14c13062dce6c78f9396271cb66c8ce7d9e0dab140a02192e6af14e856240
```

Build ID: `40367e24241c9481bfc07fbf9ef1919f25fb0eaa`.
Complete records end at byte 1,375,560. The remaining 753 bytes form an incomplete final record and were excluded from replay.

C++ and Python emitted different float decimal spellings in 4,185 records. Both JSON representations were parsed through the independently loaded schema and compared for **exact protobuf value equality**; no tolerance was used.

## Commands exercised

```powershell
cmake --build --preset release
ctest --preset release -E integration.memory
ctest --preset release -R integration.memory
ctest --preset release
python tools/test_postgres.py out/build/windows/bin/Release/chronicles-server.exe out/postgresql-tools/pgsql/bin
```

Replay used `chronicles-replay ... --tcp-loopback --jsonl out/replayed.cpp.jsonl`, followed by `tools/verify_replay.py` against the existing Python JSONL output. Private decoded packet values remain under ignored `out/`.

The integration flows exercise wrong-password rejection, login, session revocation, world listing/entry, character creation/listing/selection, invalid ownership, binary protobuf responses and WebSocket lobby RPC. Unit tests additionally cover ticket expiry/single use, concurrent slot conflicts, frame corruption and unknown fields.

## Compatibility boundary

The native adapter now implements the recovered bootstrap JSON and world HTTP routes, plus
the binary `/ws/auth` and `/ws/front` envelope. Character list/create/select IDs come from
native wrapper getters, recorded in `protocol/native-ws-opcodes.json`. Server-list version and
protocol bytes were verified in the running client. See `NATIVE_PROTOCOL.md` and
`docs/native-websocket.md` for evidence and remaining inferences.

The opt-in local Steam route maps native requests to configured emulator credentials.
The original client now accepts this response and completes login upgrade and realm entry.
This local mapping does not validate Steam tickets. Password challenge login is not implemented.

The optional TLS world listener accepts an emulator-defined 32-byte credential or the original
client's observed 100-byte credential with a 64-character token at offset 36. The first 36 bytes
of the native credential remain opaque. It implements entry, time sync, ping/pong and exit.
TLS integration verifies certificate checking in the test client, frame parsing, one-use and
unknown-key rejection, and sequence validation. It does not establish the original client's
certificate policy; see `protocol/native-world-evidence.json`. The opt-in bootstrap diagnostic
loads one extracted Wild tutorial spawn. It now sends nearby-player add, move, and remove
notifications within that map. General map loading and gameplay simulation are not implemented.

The first restarted-client trial continued using the official service. Live configuration
inspection confirmed that the packaged `DefaultX5Game.ini` was cached instead of the loose
override. The active NA region and current auth URL were then redirected in memory, with
original-byte backups and readback verification. The original client reached the emulator's
WorldListReq endpoint; its retained official-session token initially caused HTTP 401 and an
empty NA server list. Other regions continued to use official endpoints.

Native discovery now returns public metadata without account data. Explicit local mode can map
a retained opaque token to the configured emulator account for entry or native WebSocket
upgrade. A synthetic 847-byte token, native upgrade with query parameters, character access,
and logout revocation all passed transport integration. That mapping does not validate the
official token. In the live retry, the original client displayed **Local Chronicles**, sent
`PUT /api/auth/WorldEnterReq/1` (HTTP 200), upgraded the native front WebSocket, and sent
`CharListReq` (ID 20001, empty protobuf). The original client subsequently sent
`CharCreateReq` (ID 20002, 995-byte payload) and `CharSelectReq` (ID 20007, 9-byte payload),
which the emulator handled successfully. In this early trial, its world connection reached the
TLS listener but rejected the development certificate with `tlsv1 alert unknown ca`; no world
credential or gameplay exchange had yet occurred.

The client also requested `FriendListReq` (ID 20024). The initial unsupported request closed
the lobby socket. The adapter now returns an empty `FriendListRes` (ID 28), preserving the
presence and value of the optional `recommend` field. Unit and synthetic transport tests cover
absent, false and true values and continued character operations on the same socket. The new
reply has not yet been observed by the original client.

A Windows crash/reboot cleared the temporary endpoint edits and memory-backed character.
Visual Studio was reopened and the updated Release build and all 30 tests passed after recovery.
`tools/textconfig_client.ps1` stages a reversible `-textconfig` configuration and the development
certificate through the client's `[SSL] DebuggingCertificatePath` setting. Static analysis supports
these settings. After staging, the user restarted through Steam and still saw official regions;
the emulator received no bootstrap request. Windows did not expose the protected client's command
line, but Steam's app-specific launch log confirms `-textconfig` was passed to the game executable.
The saved `Engine.ini` disappeared during startup. This trial does not establish a persistent
redirect or the effect of the certificate setting on world TLS.

Test processes are stopped after each integration flow. Local client trials use loopback ports
8080 and 8081. `tools/native_client.ps1` created a
reversible loose client configuration, with its backup manifest under ignored
`out/native-client/`. It is ineffective by itself because of packaged binary configuration precedence.
PostgreSQL results above belong to the previous development-interface validation; synthetic native
adapter integration uses memory storage. After recovery, `tools/start_persistent.ps1` initialized
a separate private PostgreSQL 16 cluster on loopback port 5433, applied the migration, and started
the native server on 8080 and TLS listener on 8081. Readiness and listener ownership checks passed;
`/health` reports both native adapters and the world listener enabled. No character has yet been
created through the original client in this persistent cluster.

The persistent launcher was also exercised with an already-running cluster and existing account.
Its original ACL reapplication required an unavailable audit privilege on restart. The helper now
applies only a newly constructed access-control section, keeping private user access without
requesting audit or owner changes. A subsequent server restart passed readiness, retained the
same PostgreSQL process, and preserved the credentials file byte-for-byte. Startup confirmed the
existing account and password were retained.

Eight focused Python codec tests passed for string bounds, duplicate preservation, raw encodings,
section insertion, custom INI parsing, opaque cache preservation, and native bootstrap URL
composition. The explicit archive candidate `out/native-config/IniBootstrap.local-v2.bin` has
SHA-256 `87666b3df86393ef9d62326430f672e7adfb58103e1254b2d02b6dbeef39ad36`.
Independent raw-byte review confirmed its compact field layout and trailing strings. Its custom
bootstrap base is `http://127.0.0.1:8080/`, which the client extends to `/api/auth/BootstrapReq`.
Both loose-file configuration layers were restored before this archive trial, including the
original saved `Engine.ini` that the client had removed.

The V2 archive trial failed during startup with `Unable to launch with RHI 'D3D11'` before any
local HTTP request. The original and candidate active Engine maps both retain
`DefaultGraphicsRHI_DX12` and `PCD3D_SM6` byte-for-byte. Further static analysis proved that
BinaryConfig has a separate four-byte empty-map trailer, which the bootstrap wrapper does not
consume. V2 incorrectly retained that trailer before its fourteen global INI strings; this
shifted the final Engine config name to `RuntimeOptions` and left `Engine` unread. The shared
cache ends at source offset `0x1A2ECC`; the final four bytes are the packaged-cache-only trailer.
See the versioned IDA findings and offline shifted-global reproduction before reusing this format.

The corrected `IniBootstrap.local-v3.bin` removes only the four-byte packaged-cache trailer.
SHA-256: `6cd2dcd5fd4f2be183633dd037b6e999d27d6a2384f39f1725a55dffcb569cc5`.
Ten codec tests pass, including a regression reproducing V2's shifted names and confirming V3's
fourteen names consume the archive exactly, with `GEngineIni=Engine`. Independent byte comparison
confirmed every other V2 byte, including the original graphics configuration, is unchanged.
The original-client V3 trial passed startup and reached local `GET /api/auth/BootstrapReq`,
`PUT /api/auth/steam/AuthReq`, and `GET /api/auth/WorldListReq`, each returning HTTP 200. The user
confirmed Local Chronicles appeared. Selecting it requested `PUT /api/auth/LoginUpgradeReq`,
which returned HTTP 404; the client displayed `AuthChallenge response(404) error`. Native login
upgrade was the next missing step before the new persistent-cluster character trial.

The adapter now handles LoginUpgradeReq with a live Bearer session, recovered protobuf body,
and the existing AuthChallengeRes response. Successful local login rotates the session token;
missing, unknown, expired or revoked sessions are rejected. Unit tests and synthetic native
transport checks passed, including the returned token's use for realm entry and character
operations. The original client subsequently completed LoginUpgradeReq and WorldEnterReq/1 with
HTTP 200, then requested the native character list and friend list on the same front connection.
It created a character (CharCreateReq payload 1,099 bytes) and selected it (CharSelectReq payload
9 bytes). A read-only database query confirmed one local account character persisted in PostgreSQL.
The new world connection reached TLS but the client sent `ssl/tls alert handshake failure`.
At this point in the investigation, no original-client application credential had been read.

The TLS listener now records negotiation metadata and peer-alert state without payloads or keys.
Its options, ciphers, certificate, verification and framing are unchanged. The rebuilt server
passed all 32 CTest checks again and restarted against the same PostgreSQL cluster.

V4 retains V3's proven bootstrap layout and restores the user's existing saved Engine override
`LwsWebSocket.bDisableCertValidation=False` instead of the packaged default. Independent raw-byte
comparison confirmed this is the only setting change. SHA-256:
`8dc9102460fc8aab3d651f7ee04b795207f0b9a0415147e96699a85aafedd103`.
Twelve codec tests pass. This is settings preservation, not a world TLS compatibility claim.

Live read-only inspection of the pending world connection (PlaySharp kernel backend, image
bytes matched) established the world TLS failure point. The client's `SSL` uses
`SSL_VERIFY_PEER` with its own VM-protected verify callback `0x1461E8860`; `verify_result`
changes from 0 to 50 (`X509_V_ERR_APPLICATION_VERIFICATION`) on the development
certificate, producing alert 40. The certificate is root #0 of the 203 loaded roots, so chain
trust is not the cause. Four follow-up trials still failed with error 50: `packet_key` equal to
the certificate SPKI SHA-256; advertising `localhost` (the client then sends SNI); a V5
bootstrap archive with `[SSL] PinnedPublicKeys` (replaced at startup by code-installed
`.xlgames.com` pins); and a reversible live rewrite of that pin record to `localhost` with the
development key digest, restored afterwards. Details:
`../dumps/ida_research/world-tls-live/FINDINGS.md`. Normal, unmodified world entry remains
blocked at this client-side certificate check.

An opt-in `chronicles-world-tls-local` diagnostic tested two one-connection overrides against
the original client: `SSL.verify_mode` set to zero, and the protected per-SSL verify callback
pointer set to null while `SSL_VERIFY_PEER` remained enabled. Both completed TLS 1.3 with the
emulator's certificate. In both trials the client then reset the connection before sending any
application bytes; the server's prototype credential read received 0 of 32 bytes. The cause of
the post-TLS close was unresolved in these trials, and world entry did not succeed. The tool
checks matching live code bytes and a loopback connection before writing, and each temporary
SSL object ended before its scheduled field restore. No executable bytes or game files changed.

Follow-up original-client trials on the restarted process used both diagnostic modes and the
server's 2-second handshake delay. Both completed TLS 1.3 and ended with 0 of 32 prototype
credential bytes. A bounded client state trace showed Connection socket/TLS wrapper present,
send-thread state zero and `verify_result=0`, followed by socket/TLS wrapper absent with the
send-thread state still zero. Static disassembly shows the protected TLS initializer's return
gates the initial application write, and its zero-return branch clears the wrapper and socket.
The live trace is consistent with that branch but did not directly capture the return register.
These trials did not yet verify a world credential or gameplay exchange.

A later opt-in TLS 1.2-only trial reached the same boundary. Without a client override, the
original client received the server certificate and sent fatal alert 40 before completing the
handshake. With the per-SSL callback replaced by OpenSSL's default verifier for one loopback
connection, the client completed TLS 1.2 with SNI `localhost`, then closed the connection after
sending 0 of the 32 prototype credential bytes. The live socket and TLS wrapper disappeared
while the send-thread state remained zero. This rules out TLS 1.3 negotiation as the cause of
the observed post-handshake close. The TLS 1.2 restriction is diagnostic and is disabled by
default.

After a game restart, the launcher scanner found a one-entry region-list candidate. Manager
validation was extended to allow one region when its selected-region and current-address
invariants hold; the fixture test passes. The live scanner did not confirm a manager or report
an address rewrite during this trial, so the one-region discovery path is not live-validated.

A later local one-connection trial completed TLS 1.3 and the server read a full 32-byte
application prefix before rejecting it as the emulator's provisional raw `packet_key`.
The temporary `mov al, 1` instruction used in that trial did not change the zero flag, so it
does not prove that the startup branch was forced. Other completed TLS trials still ended with
zero application bytes. At this point, the client's first-message format was still unknown.

An opt-in `--world-credential-diagnostic true` server mode now compares a rejected 32-byte
prefix and any already-buffered TLS plaintext with pending handoff values. It records only
match type, offset, length and buffered-byte count; it does not log credential contents or
change authentication. The Release build and all 44 CTest checks passed at this stage.

## Controlled original-client tutorial-world trial, 2026-10-23 20:38–20:40

This trial used a one-connection TLS diagnostic and a separate temporary post-TLS startup-branch
patch. The normal client still rejects the local certificate with its protected callback; neither
intervention is a production compatibility fix. The server ran with the opt-in
`--world-bootstrap-diagnostic true` tutorial sequence and a two-second handshake delay.

The original client completed TLS 1.3. The server read a 100-byte initial world credential and
matched the selected character's 64-character token at byte offset 36 without logging it. It
accepted character 1 and sent `PS2C_CharEntryRes` (5). The observed client/server sequence then
included:

| Client request | Server response | Observed effect |
| --- | --- | --- |
| `PC2S_TimeSyncReq` (20001) | `TimeSyncRes` (1), then opt-in `MapMoveStartNoti` (175) | Started tutorial map loading |
| `PC2S_MapLoadCompleteReq` (20085) | `MapMoveCompleteNoti` (176) with a selected-player `UnitNub` | Map and player state progressed |
| `PC2S_StartPlayingReq` (20086) | `StartPlayingRes` (183) with the same player unit ID | Start-playing handshake completed |
| `PC2S_Ping` (20003) | `Pong` (4) | World connection remained active |

The map-start notification contained the extracted Wild tutorial spawn point on map 8000:
WorldPos `(-795146, 712194, -698778)` in integer millimeters and yaw `158.56723` degrees.
With that explicit spawn point, the game showed its character and HUD on the tutorial terrain.
The client also sent `PC2S_NetworkDelay` (20004) and `PC2S_MoveReq` (20087); the diagnostic
server decoded and ignored them. The observed standing spawn and sustained connection do not
establish movement, combat, persistence of world state, or general gameplay compatibility.
That first trial used `PlayerNub.state = Spawn` (0).

## Controlled Idle-state follow-up, 2026-10-23 20:46–20:48

With the same reversible TLS and post-TLS diagnostics, the client again completed TLS 1.3,
sent the 100-byte credential, loaded map 8000, and displayed the character and HUD on the
tutorial terrain. This time `MapMoveCompleteNoti` explicitly set `PlayerNub.state` to
`EX5UnitStateType_Idle` (1). The client sent `StartPlayingReq` (20086); the server responded
with `StartPlayingRes` (183), then answered a ping. Twenty brief simulated W key presses did
not visibly move the character or produce another `MoveReq`. These synthetic key presses may
be too short for gameplay input, so the trial does not establish whether Idle enables movement.
The original post-TLS branch bytes were restored and verified by readback. The one-connection
TLS diagnostic reported restoration of its SSL field.

The test suite contains 46 checks. A complete CTest run passed 46 of 46 after the Idle-state
change, including the protobuf spawn-point and staged map/start-playing response checks.

## Captured movement-rate follow-up, 2026-10-23 20:56–20:58

The opt-in `MapMoveCompleteNoti` player nub now includes the captured stat
`EX5StatType_MoveSpeedRate` (84) at 1000 per mille. The rebuilt Release server passed the full
46-check CTest suite before the trial. With the same reversible one-connection TLS diagnostic
and temporary post-TLS branch patch, the client completed TLS 1.3, sent the 100-byte credential,
loaded map 8000, and displayed its character on the tutorial terrain. It sent
`MapLoadCompleteReq` (20085), `StartPlayingReq` (20086), two `MoveReq` (20087) packets at the
spawn position, and a ping. Thirty brief focused W key presses after the character appeared
produced no visible movement or further `MoveReq`. The two observed movement packets occurred
before those key presses and shifted only the vertical position near spawn. This trial confirms
the client accepted the additional stat but does not establish that the stat enables input;
brief synthetic key presses may not provide a sustained gameplay input.

The original post-TLS branch bytes `84 C0` were restored and verified by readback. The TLS
diagnostic reported restoration of its original SSL field. Ordinary unpatched client TLS
verification still rejects the local certificate.

## Launcher-managed original-client restart, 2026-10-23 21:24–21:28

The WebView2 launcher's **Launch game** action generated a fresh local `IniBootstrap` archive,
started the original Steam client, and armed `chronicles-world-tls-local --enter-world` before
character selection. The already-running persistent server supplied the two-second handshake
delay and tutorial bootstrap sequence. The server received the client's local `BootstrapReq`,
Steam auth, world-list, login-upgrade, and world-enter requests; no separate endpoint patch
command was used. Steam's `console_log.txt` recorded the tracked game process using the
launcher-generated `out/launcher/IniBootstrap.local-22064-18267250.bin` archive, confirming
that this run did not depend on the older archive in saved Steam launch options. The helper
waited for the protected code to finish initializing, then applied
its guarded per-connection SSL callback and two-byte startup-branch overrides.

After selecting the saved character, the original client completed world TLS and sent its
100-byte credential. The server accepted the character, received `TimeSyncReq` (20001),
`MapLoadCompleteReq` (20085), `StartPlayingReq` (20086), and `MoveReq` (20087), and sent the
captured map/start-playing response sequence. The client displayed the tutorial character and
HUD on map 8000. This is an observed original-client trial, not just offline replay. Movement
requests are still decoded and ignored by the server, so authoritative movement and general
gameplay remain unverified.

A read-only PlaySharp kernel memory check after the trial read `84 C0` at the researched startup
branch address `0x146170ED1`, confirming its original two instruction bytes were restored in
this running client.

One launcher defect surfaced during the trial: a completed world helper was started again while
the established world connection remained active, replacing its log. The launcher now keeps
append-only helper output and only rearms after that connection closes; this lifecycle change
was compiled after the live session but has not had a second world-entry trial. The final Release
launcher and helper builds succeeded, and all 46 CTest checks passed. `git diff --check` found no
whitespace errors in tracked changes.

## Official packet capture and local gear follow-up, 2026-10-23

The bounded read-only slicer captured 528 complete original-client world frames
from a BR realm entry: 12 client-to-server and 516 server-to-client. The replay
tool decoded and round-tripped all 528. Basic attacks produced five
`StartSkillReq` and three `StartX5RootMotionReq` frames. Short simulated movement
key presses produced no `MoveReq` in that run, so the capture does not establish
walking behavior. Raw frames remain under ignored `out/captures/`.

The official entry trace places `MapMoveStartNoti` before `TimeSyncRes` and
`WorldCharItemsNoti` after the client's first `TimeSyncReq`. The emulator's
opt-in tutorial sequence now follows that order. Its Wild starter gear table
is shared by world inventory/equipment and the lobby's positional
`equip_cms_ids` response. Those Wild item IDs come from the client asset
catalog and still need an official Wild entry capture to confirm them.

The focused lobby create/list test and full 47-check CTest suite passed. In a
fresh launcher-managed trial, the original client completed TLS 1.3, sent the
100-byte world credential, loaded the tutorial map, and sent `StartPlayingReq`.
The local character appeared in the lobby and playable world with her starter
clothes and a sheathed weapon. The world server received `MapLoadCompleteReq`,
`StartPlayingReq`, and two initial `MoveReq` packets. The second movement packet
was only 23 mm from the first; repeated short `W` key presses did not visibly
move the character, so walking and live checkpoint recovery remain unverified.
The displayed skill slot was empty, and a short `1` press produced no skill
request. The server continued receiving world packets after the HUD appeared.
The live PostgreSQL character row contained map 8000 and the second reported
position after the server's two-second checkpoint interval. This confirms a
database write from a real client packet, but not restoration after reconnect.

## Persistence, nearby players, and client data, 2026-10-23

Schema version 3 adds character experience, item instances, and nullable camera
columns to the existing PostgreSQL account, character, and saved-position state.
Before applying it to the private loopback cluster, a custom-format backup was
saved at `out/local-postgres/pre-v3-20261023-222642.dump`. The transactional
migration completed with schema versions 1, 2, and 3 recorded. A read-only
check found both existing Wild characters, 18 item instances in total, and the
previously saved position. Restarting `tools/start_persistent.ps1` applied the
idempotent migration again and returned healthy listeners on 8080 and 8081,
with the existing PostgreSQL process still on 5433.

The world service now checkpoints verified `MoveReq` positions and body yaw,
including a final save on disconnect. It tracks players in the tutorial map and
sends recovered `UnitAddNoti`, `MoveNoti`, and `UnitRemoveNoti` shapes across
different world connections. A synthetic two-client TLS loopback test confirmed
mutual visibility, peer movement, and removal after disconnect. This is not a
visual test with two original game clients. The service bounds its world
connections and each outbound notification queue. Identity-based local accounts
are opt-in; their claimed Steam identities are not authenticated.

Inventory and equipment now come from persistent item instances rather than
hard-coded lobby and world fields. Wild starter items are created atomically
and existing Wild characters were backfilled once. Owner-scoped APIs support
item and experience updates, but no verified original-client gameplay packet
currently invokes those writes. The recovered `MoveReq` has body yaw, not
camera yaw or pitch; camera columns remain null until a source is identified.
Quest, skill, and other gameplay progress are not yet persisted.

The export tool copied and verified 286 CMS tables plus five derived catalog
files (291 files, 93,554,619 bytes) and 11,720 raw files (1,455,738,099
bytes) from the verified local client extraction. The generated data is ignored
by Git. The server loaded its manifest at startup and validated the Wild class
and starter item IDs. The full Release build succeeded; `ctest --preset release
--output-on-failure` passed 53 of 53 tests; three export-tool tests passed; and
`python tools/export_client_data.py --verify-only` checked all 12,011 hashes.

## Named-account preparation, 2026-10-23

The new `chronicles-account` tool uses the same `AuthService` password hashing
and PostgreSQL store as the server. Its PowerShell wrapper prompts without
echo and passes the password on standard input; no password is placed on its
command line or in a saved account file. The tool compiled in Release mode and
verified the existing `localdev` password against the persistent database
without printing the password or token. It did not create a second account in
the user's database. A focused test confirms two named accounts can remain
logged in simultaneously, retain separate characters, and relogin/logout
without revoking the other account. A second native-auth test confirms a
claimed Steam identity cannot authenticate as a named password account.

The complete CTest suite passed 55 of 55. The original client's Steam login
still uses the development mapping; it does not send the JSON account/password
login request. Its separate protobuf password challenge cannot be implemented
from the recovered schema alone. The server remains bound to loopback, so this
work does not yet make a second laptop able to join. See
`docs/account-system.md`, `NATIVE_PROTOCOL.md`, and `docs/laptop-readiness.md`.

## Named-account launcher grant, 2026-10-23

The launcher now asks for a named account password on every launch, logs in to the persistent
loopback server, requests a one-use five-minute grant, and writes that grant into its generated
bootstrap archive. The native adapter exchanges the prefixed Steam AuthReq for a session belonging
to the password-verified account. `-RequireNamedLogin` rejects the old unprefixed Steam mapping
and opaque bearer alias; the launcher checks the server's health response for that strict setting
before a named-account launch. Passwords, bearer tokens, and grant URLs are excluded from launcher
settings and logs. The archive is an ignored local artifact and its evidence sidecar redacts the
grant URL.

The Release build and a complete 58/58 CTest run passed. The first full run had two native
integration failures caused by comparing the JSON string form of a 64-bit account ID with the
integer form decoded from protobuf. Both integrations and then the full suite passed after the
test comparison was corrected. Twelve binary-config codec tests and `git diff --check` passed.
The final server and launcher rebuild passed after the strict-mode health guard was added; four
focused grant/native tests passed after that rebuild. The isolated PostgreSQL smoke test passed
account creation, password verification, account ownership, and persistence across one server
restart. The persistent server was restarted with `-RequireNamedLogin` and reports that strict
mode enabled on loopback HTTP and world listeners.
The installed Steam client has not been tested with the named-account grant. A later private-copy
trial below observed two copied clients completing that grant exchange.
HTTP and world listeners remain bound to loopback; a laptop cannot yet join.

## Direct client/Xigncode startup trial, 2026-10-23

Starting the game executable directly with a newly generated local launch-grant archive still
loaded `x3_x64.xem` and spawned `xxd-0.xem`. The trial client opened an external IPv6 connection,
whose purpose was not established, and was stopped. This did not test a Xigncode-free client or
two original clients. IDA static analysis identified the Xigncode load and initialization
wrappers in the unpacked image, annotated seven functions, saved the database, and updated its
function index and append-only journal. The installed executable's `.text` section has no raw
on-disk bytes, so an RVA from the dump is not a direct file patch offset. See
`docs/xigncode-local.md`.

## Private client preparation and egress probe, 2026-10-23

`tools/test_prepare_private_client.ps1` passed its synthetic dry-run, copy, hash, tamper, and
unsafe-link checks. The installed client was copied to `E:\ArcheAgeChronicles-LocalTrial` with
`-ConfirmLargePakCopy`, then `-Verify` passed for all 85 files (47,140,305,225 bytes copied).
The Steam installation was not changed. Later private-copy startup trials are recorded below.

A temporary app-specific Windows Firewall rule for a copied `curl.exe` allowed HTTP to
`127.0.0.1:18765`, blocked external HTTPS, and allowed external HTTPS after removal. The
`tools/private_client_firewall.ps1` helper passed enable/status/disable on a fixture private
root using the same external check. Fourteen outbound block rules are currently enabled for
the prepared private game tree; their application filters are all inside that tree and include
the game executable and `XIGNCODE/xxd-0.xem`. This is containment preparation, not proof that
every possible child process of the game is blocked.

## Private diagnostic shim trial, 2026-10-23

The experimental `x3_x64.xem` replacement built as a separate C++20 DLL and passed its
offline export/ABI test (CTest 1/1). The guarded private installer passed its synthetic
install/status/restore test. Installing the shim changed only the private client module and
left a byte-identical original backup in that private tree.

With the outbound rules active, the private client started but remained at a window titled
`XignCode`. After roughly 20 seconds, its process had not loaded the shim or fallback module,
opened a local login connection, or spawned the Xigncode helper. The process was stopped.
Restoring the original private `x3_x64.xem` and launching the same private client with the
same archive and firewall rules produced a window titled `ArcheAge Chronicles` and loaded the
original `x3_x64.xem`. That baseline was also stopped. These observations show the current
shim fails before the normal module-load point; they do not identify the exact check yet.
Neither trial reached local authentication or world entry. The existing no-grant v4 local bootstrap
archive was used solely to test early startup; the persistent server requires launch grants.

The startup loader's checksum was then reproduced offline: the original module's stored and
computed marker were both `0x82703FE2`; the first shim stored zero. The separate shim build
now writes its computed marker (`0x8C105385`) into the PE DOS header. Offline CTest 2/2,
the private-copy test, the guarded shim install/restore fixture, and `git diff --check`
passed. After reinstalling this corrected shim, the private client loaded it and opened an
`ArcheAge Chronicles` window without an observed `xxd-0.xem` helper. The emulator logged a
local `BootstrapReq -> 200`. The first client displayed the interactive title screen. Pressing
Return caused `PUT /api/auth/steam/AuthReq -> 401`; the title then displayed
`AuthChallenge response(401) error`. This was expected because the test archive had no
launch grant and the persistent server requires one. A second private client process started
concurrently, loaded the same replacement module, reached its own title screen, and made an
independent local `BootstrapReq -> 200`; it also displayed the 401 error. Both processes had
visible game windows and no observed `xxd-0.xem` helper or established non-loopback TCP
connection in the sampled process state. This proves simultaneous startup, not separate account
login, world entry, synchronization, or faster boot. The installed Steam game remained unchanged.

`tools/launch_private_client.ps1 -CheckOnly` passed against the private tree and active firewall
group. The targeted world entry helper rebuilt and rejected a mismatched client directory for a
real PID and invalid PID zero. A later trial armed a watcher for each private client, as recorded
below.

The GUI launcher gained a guarded private-copy target. Its Settings store loaded the private
root and target from `out/launcher/settings.json`, the read-only `--status` CLI reported
`client_launch_target: private_copy`, and the live WebView displayed **Launch private client**
while the external strict server was online. The launcher and world diagnostic targets built;
the 12 focused Encoding/AuthRedirect CTests passed; `node --check launcher/web/app.js` and
PowerShell parsing of `tools/launch_private_client.ps1` passed.

## Two named private clients, 2026-10-23

Two distinct named test accounts were created through `tools/manage_account.ps1` and used for
separate Play actions in the WebView2 launcher. With **Guarded private copy** selected, the
launcher generated a different one-use grant archive for each account and started two concurrent
processes from `E:\ArcheAgeChronicles-LocalTrial`. Both processes loaded the experimental private
Xigncode shim and reached their own interactive title screens. The original Steam installation
was not changed. The server logged a redacted grant fetch for each process, followed by
`PUT /launch/[redacted] -> 200` during login. Each client then opened the native front WebSocket
and sent packet ID 20001 (`CharListReq`). This is live evidence that both copied clients passed
the named-account launch exchange and reached character listing.

The launcher's world-entry helper watched PID 44128, and a second helper targeted PID 26684 in
the same private directory. Both clients created and selected separate characters (IDs 3 and 4).
The server completed TLS 1.3 on two world connections and accepted each client's world
credential. Both game windows displayed the wooded world-start scene concurrently. The helper
logs report restoration and readback of the temporary startup bytes and restoration of the SSL
callback field after each connection trial.

The initial two-client trial stopped before tutorial map enrollment because the diagnostic
accepted only Wild. After adding class-specific tutorial spawns, a later trial brought both
original clients through map start and map completion on map 8000. The server sent peer-add
notifications to both connections and relayed movement notifications in both directions.
Both game windows showed the other character nearby, clothed and carrying starter equipment.
The server also recorded changing positions from each client. This establishes local presence
and movement packet relay; skill effects, combat, reconnect restoration, and authoritative
movement remain unverified. Boot time was not measured.

## Official tutorial to shared-area capture, 2026-10-24

The original Steam client produced 6,224 complete bidirectional world frames
through tutorial maps 8000 and 10117 and into shared map 9011. The full
snapshot used 115 opcodes; `chronicles-replay` decoded and semantically
round-tripped all 6,224 frames. The copied snapshot ended with 1,647 bytes of
an incomplete record, which replay excluded. A separate shared-area slice
decoded 314 of 314 complete frames and included player unit-add records. The
user confirmed the second tutorial section was instanced and map 9011 was the
area where other players can appear. See `docs/official-tutorial-trace.md`.

## Tutorial interactions, 2026-10-24

The release server, account tool, and test binary built successfully. With
`CHRONICLES_TUTORIAL_TEST_PROFILE` set to the generated tutorial definitions,
CTest registered 87 tests: 86 passed, one opt-in PostgreSQL test was skipped,
and none failed. This includes a TLS trial with two synthetic accounts,
separate tutorial instances, pickup persistence, and no duplicate reward after
reconnect. The PostgreSQL storage test then passed separately through
`tools/test_postgres.py` in an isolated temporary cluster, including ownership
and concurrent revision conflicts. Its standard persistence/account smoke
checks also passed with a server restart.

The tutorial generator's three tests and the exporter’s three tests passed.
The mapping verifier matched eight captured floor pickups and twelve NPC types,
including their captured spawner GUIDs. These checks establish offline mapping,
synthetic protocol behavior, and durable state handling. They do not establish
original-client pickup behavior, combat, movement authority, or load capacity.
The persistent loopback server is running with the generated tutorial profile
for the next original-client pickup trial.

The original client then picked up the first potion object: the user confirmed
the pickup and showed five potions in the utility slot. Server metadata recorded
the interaction response, and after further pickups PostgreSQL held seven
potions. Skill-start requests were ignored, explaining the unchanged count on
the attempted use. Quest acceptance was also acknowledged by the server; this
does not establish the complete quest chain.

The following increment handles the extracted potion skill's consumption
event, atomically updates inventory/utility counts, and schedules skill end
without sleeping a worker. Twelve focused behavior/TLS tests, twelve NativeWorld
CTests, and four extraction tests passed. Its original-client consumption trial
is pending. No health recovery or combat compatibility is claimed.

The following original-client trial did not decrease the count. Server metadata
showed accepted StartSkillReq messages and start/end notifications, with no
ExecuteSkillEffectReq between them. The initial handler incorrectly depended on
that optional report. The correction schedules consumption from the extracted
435 ms event on the server, serializes timed actions with incoming requests on
a worker strand, and keeps persistence outside the socket event loop. The TLS
regression now deliberately omits the client effect report. Live verification
of the corrected count update is pending.

The unchanged combined-test executable then passed 60 selected gameplay/world
cases with the child-process `OPENSSL_ia32cap=0` override: 58 passed and two
opt-in checks skipped. Those PostgreSQL and private-capture checks passed in
their owners' separate runs. A second 52-case CTest selection passed 50 and
skipped the same two opt-ins. The combined combat/potion TLS case consumes
without a client effect report and applies server-timed attack damage. Normal
CPU dispatch reproduces a nonvolatile-register failure; it has not been fixed
or declared compatible. See `docs/openssl-local-diagnostic.md`. The loopback
trial will use the explicit `start_persistent.ps1 -PortableOpenSsl` switch.

### Coordinated local trial readiness, 2026-10-24

The combined server linked successfully and replaced the old loopback service
using the explicit portable OpenSSL process setting. The original client exited
normally first; PostgreSQL recorded its final position checkpoint on map 8000.
The existing character retained five potions in both inventory and utility slot
zero before the trial. The startup migration and regenerated tutorial definitions
loaded successfully, and both local listeners passed readiness checks.

A fresh named-account launch grant and bootstrap archive started the private
client, and the existing guarded world-entry helper reported ready. Computer Use
then reported a physical Escape stop before the operator entered the world.
No corrected potion consumption, current character-stat display, combat behavior,
or NPC placement has been visually verified in this trial. Preserve that distinction
from the passing synthetic and isolated PostgreSQL checks above.

The five gameplay chats agreed on ownership for interactions, quests and travel,
character statistics, combat, and NPC placement verification. Shared compiled
sources remain frozen for the first live trial. The quest owner has the next
transport increment after that trial; follow-up statistics and potion healing
remain staged work. A separate Cursor build was observed during coordination,
so a chat-level build agreement alone does not prove an exclusive checkout.

### Advanced stats integration and shared build repair, 2026-10-24

The six-file advanced character-stat patch was applied after an explicit request
to resume the stats trial. It indexes catalog equipment, grades, durability,
critical ratings, armor, and elemental calculations at startup. It preserves the
existing initial-stat API; active combat HP/SP remain session-owned. Equipment
request handling, active-slot persistence, and complete reward/modifier state
are separate unfinished work. See `docs/character-stats.md` for the supported
inputs and remaining capture disagreements.

The first shared build exposed an incomplete NPC motion increment from another
editor: two declared helpers had no definitions. A bounded repair completed the
trajectory implementation and its motion/phase event forwarding. Dead or
inactive NPCs no longer emit an idle state after demise. This does not establish
original-client NPC movement compatibility.

The first integration run also exposed a test-fixture problem: synchronous TLS
reads ignored the intended Beast deadline. The fixture now uses asynchronous
reads, and a silent-peer regression verifies an actual five-second timeout.
The expanded combat test remains enabled.

All 50 selected CTests passed in 26.68 seconds against the real generated
catalog/tutorial profile with the process-local portable OpenSSL setting. This
includes nine stat tests, 14 NPC AI tests, combat regressions, the combat/potion
TLS case, and the negative TLS timeout check. The Release server then built
successfully with SHA-256
`ABC0A28FD2627A103A46E3C8952E543C44317DAEEFFA7F3E072C3E67A979281D`.

The updated loopback server passed readiness checks, and a fresh named-account
grant started the private client with the existing guarded world-entry helper.
The original client reached the local realm screen, but its displayed F login
action did not advance after supported simulated key presses and refocusing.
The documented Computer Use API has no key-hold duration control, and repository
navigation rules prohibit clicking the control while a shortcut is displayed.
Physical keyboard help was requested. Later server metadata confirmed successful
TLS world entry for the preserved character, and a read-only screen inspection
confirmed the tutorial world. The character panel was closed, so its displayed
stats remain unverified. The earlier realm input blockage is historical, not the
current observed screen state.

A subsequent explicit request to activate local account registration replaced
the loopback backend with the separately tested registration build, SHA-256
`0A00D070021DC1629DC68D91CB182976FAFE24703217FD8AF34A4C4895FFE1E2`.
Database/log snapshots and the validated stats executable were preserved first.
Readiness passed, and a missing-field registration request returned HTTP 400
without creating an account. PostgreSQL and the separate distribution service
were not restarted. This backend restart invalidates its previous in-memory
game sessions; later game entry requires a fresh login/grant. The old game PID
was absent in a later process check, without a game stop command from this chat.
Launcher restart was handed to its owner after backend readiness.

## Distribution backend and automatic patches, 2026-10-24

The distribution release build completed for the native server, account tool, WebView2 launcher,
and test executables. The full CTest run registered 88 tests: 86 passed, two optional tutorial/
PostgreSQL tests were skipped, and none failed. The actual backend/WinHTTP integration exercised
signed installation, repair, corrupt-download rejection without replacing installed files,
retry, preservation of unlisted files, invalid manifest signatures, verified HTTPS password
login, configured launch-grant origins, authenticated WSS, and Windows rejection of an untrusted
test certificate. Installer tests also covered rollback and interrupted-commit recovery.

A separate local PostgreSQL-backed distribution service is healthy on API port 8480 and world
port 8481. Its configuration is ignored under `out/distribution-backend/backend.local.json`.
It serves a deliberately small signed support-file release, not a complete game client. The
packaged launcher installed that release with a clean Windows PATH; the backend package's help
command also passed with a clean PATH after bundling indirect LIBPQ/OpenSSL dependencies.

The final archives are `out/distribution/chronicles-launcher-win64-20261024-004606.zip` and
`out/distribution/chronicles-backend-win64-20261024-004606.zip`. Deployment placeholders and
publishing steps are documented in `docs/distribution-backend.md`. These tests do not establish
original-client startup or world entry from a managed distribution, public deployment, installer
code signing, or player-scale load capacity. The existing loopback game helper remains a separate
development-only mechanism.

## Launcher login state, 2026-10-24

Launcher sign-in now uses a native in-memory account session, separate from the remembered
account name and the game's one-use launch grant. The focused authentication run registered
45 tests: 44 passed and the standalone fixture-only account integration test was skipped.
That native integration test ran successfully inside `integration.distribution`, against a
disposable real backend, including incorrect credentials, successful login, token reuse,
validation, and HTTP 204 logout. AuthService tests verified that launcher login survives game
handoff/rotation, launcher logout preserves the game session, game logout preserves the launcher,
and a new password login still revokes the older login.

The full-suite snapshot registered 132 tests: 123 passed, eight optional tests were skipped,
and `TutorialTest.AnimatedQuestObjectConsumesKeyAtomicallyAndCannotRepeat` failed. It expected
opcode 2 after reconnect but received opcode 166. The concurrent tutorial implementation and
that test were left unchanged by this login work.

Browser UI checks used a disposable host-message fixture to exercise pending login, incorrect
password feedback, successful retry, signed-in profile/account controls, sign-out, expired login,
and offline realm state. No real player credentials were entered. The actual native transport
was validated separately by the backend integration test. The launcher retains no password or
token in UI state/settings, and existing accounts were not changed for validation.

### 2026-10-24 — Player account registration

The launcher and isolated server build passed for player registration. The final launcher suite
ran 54 tests: 50 passed and four service-fixture tests were skipped. The new registration fixture
then passed separately through actual WinHTTP against an isolated PostgreSQL-backed C++ server.
`tools/registration_smoke_test.py` verified input/method/content-type/body limits, normalized
duplicate names, concurrent duplicate creation, password preservation, registration throttling,
survival across backend restart, existing-session preservation and separate player rosters.
Its private backend and PostgreSQL processes stopped afterward. The existing native login/grant/
logout regression also passed against a disposable memory backend.

The actual transport check caught the old HTTP helper rejecting POST 201; the helper now accepts
that registration success status while retaining the existing GET behavior. A rerun passed.
New standalone checks also cover malformed reply sanitization, earlier login-error clearing,
invalid input without network requests, and preventing registration over an active account.
Hidden-browser fixtures verified short/mismatched passwords, pending/duplicate feedback, success
returning to sign-in, secret clearing and the 960-by-640 layout. These used synthetic credentials.

The server was linked into `out/registration-backend-build-20261024-093629/` using an isolated
OutDir/PDB. The normal tested server executable remained byte-for-byte unchanged with SHA-256
`ABC0A28FD2627A103A46E3C8952E543C44317DAEEFFA7F3E072C3E67A979281D`. No live server,
PostgreSQL, distribution service, launcher or game process was stopped or restarted for this work.
The updated backend must be adopted/restarted separately before a running realm serves registration.

### 2026-10-24 — Saved launcher sign-in

The launcher now supports opt-in Keep me signed in and Remember password through a separate,
user-scoped Windows DPAPI record. The final focused build passed. Of 46 standalone launcher
tests, 43 passed and three service-fixture tests were skipped. Eight new tests use the real
Windows encryption/decryption APIs and cover encrypted-at-rest storage, session-only and
password-only behavior, restoration, realm/account binding, expiry/renewal, rejected passwords,
offline retry pacing, removal, corruption, oversize input, and storage failures. The existing
native AccountSessionIntegration test also passed separately against a disposable real C++
backend; its process was stopped afterward. No live game or service was restarted.

A disposable browser host-message fixture verified both checkbox options, sign-in with a saved
password and an empty field, changing persistence while signed in, and forgetting saved login
while retaining the active session. These UI checks used synthetic credentials. Saved passwords
and tokens are excluded from serialized UI state and ordinary settings; player packages contain
neither the encrypted local record nor personal preferences. Native application reopen/tray
interaction was not driven during this validation.

### 2026-10-24 — Persistent launcher news

Replaced the four bundled dispatch placeholders with `GET /launcher/v1/news` from the configured
backend. The PostgreSQL `emulator_news` table stores drafts, article content and server-assigned
publication/update timestamps. The operator-only `chronicles-news` CLI supports atomic upsert and
withdrawal. The public HTTP route rejects writes and excludes drafts, with a 40-article limit.

The news library, operator tool, news tests and launcher targets built in Release. Three storage
unit tests and all 32 launcher unit tests passed. `tools/news_smoke_test.py` also passed against
a copied current backend using a separate ephemeral PostgreSQL cluster. It verified independent
repository instances, UTF-8 round trips, private drafts, editing without publication-date changes,
invalid-input atomicity, HTTP write rejection, persistence across server restart, withdrawal,
and the launcher's actual WinHTTP feed reader. That cluster was stopped in cleanup; existing
accounts, characters and live game services were not used for this integration check.

Hidden-browser checks used a disposable host-message fixture plus the actual exported API test
response. Empty news, manual refresh, article/date display and literal HTML-like text were checked
at the launcher's minimum 960x640 size, with no page errors or horizontal overflow. These are
launcher/news checks; they do not establish original-client remote world compatibility.

## Tutorial NPC aggro and pursuit, 2026-10-24

The tutorial profile now exports validated perception, leash/reset, combat-spacing,
and movement inputs for all 11 map-8000 monster types. `src/world/features/npc/npc_ai.*` implements a
local `Idle`/`Engaged`/`Returning` state machine on the existing world worker.
`CombatSession` retains authoritative skill deadlines, damage, death, and quest
credit. Active hostile NPCs with an executable captured attack can acquire through
proximity; all active hostile NPCs acquire after damage. Engaged NPCs pursue with
captured root-motion notifications (64 start, 106 end), attack, clear aggro beyond
leash, return home, and restore HP after the local reset delay.

Aggro and death notifications follow the official full-session trace: 98, 32
Combat, and 114 phase 1230 on acquisition; Demise and death, then 98 and 114 clears
without target/phase fields on death. The trace contains no leash, reset, Alert
pre-aggro implementation, or scripted Recalling Grounds phase reconstruction. The
leash path's `EX5UnitStateType_Return` state and reset delay are local choices.
Four tutorial monster types (5001083, 5001084, 5001085, and 5001529) have no
executable captured NPC attack, so they retaliate after damage but do not
proximity-aggro. Pursuit is straight-line and does not establish navmesh, terrain,
obstacle, hearing, threat, aggro timeout, or group-assist behavior.

An independent read-only code review found attack-less monsters chasing without
attacking, unsafe effect-shape reads, a degenerate pursuit/attack range edge, a
tautological settling test, and TLS assumptions about global NPC message order.
These were fixed with regression tests. The focused generated-profile run passed
71 of 72 registered tests, with one optional PostgreSQL test skipped. Its TLS
scenario approached a Skeleton Scout without attacking, received aggro, phase,
pursuit, NPC attack, and damage, escaped beyond leash, observed Return and Idle,
then reacquired and completed player combat. The complete CTest run registered 198
tests: 190 passed, eight optional integration/capture tests were skipped, and none
failed. All 17 tutorial extraction tests also passed. These results are offline
and synthetic verification, not an original-client trial.

The shared `chronicles-team` tools were unavailable in this chat, so no service ownership
or team-workflow review was recorded. No live server was restarted and no
original-client NPC aggro trial was performed. That trial and the required team
review remain pending.

### 2026-10-24 — Launcher store, character roster and community

Replaced the three placeholder pages with a persisted shop-link catalog, a private saved
character roster and operator-managed community destinations. Schema8 stores catalog/community
records separately from accounts and gameplay. The user's supplied Discord URL was published
and read back in the local launcher-content table; no products or shop URLs were invented.

The new C++ content/storage/operator/roster targets, launcher and server built in Release using
the agreed shared build slot. All 35 launcher unit tests, three content tests and the account
isolation/session-preservation roster test passed. `tools/portal_smoke_test.py` passed using a
copied backend and an isolated PostgreSQL cluster. It checked publication/drafts, unsafe-URL
rejection without overwriting stored content, HTTP write rejection, authenticated/account-scoped
rosters, preservation of game world context, content and character persistence across restart,
withdrawal, and actual native WinHTTP catalog/roster reads. The news smoke test also passed after
sharing the disposable-cluster setup helper. Both test clusters stopped during cleanup.

Hidden-browser checks used disposable account data exported by that integration test. Search,
empty results, catalog and Discord action ids, character details/equipped slots, clearing the
roster on sign-out and 960x640 layouts were checked without horizontal overflow or page errors.
External browser opening was wired through validated native link lookup and ShellExecuteW;
the browser fixture captured link actions rather than opening real shop/Discord pages. The URL
action initializes an STA COM apartment on its worker thread and balances it through RAII;
the final launcher-only rebuild passed after that Windows handler compatibility fix.

The existing server executable was preserved under
`chronicles-server.before-portal-20261024-022512.exe` before linking its replacement. No running
game, launcher or emulator/distribution service was stopped or restarted by this change. The
new backend/launcher need activation separately. These checks do not claim original-client
gameplay or remote world compatibility.

## Shared-world gathering service, 2026-10-26

`tools/build_gathering_data.py` generated the ignored gathering profile from the hash-checked
export: 15,322 placements inside the eight demo ArcheWorld map areas, 13 supported gathering
definitions and six animations. Thirteen gathering IDs without authored `ExpertiseReward` loot
are rejected at runtime. The joins were checked against Generic Part 1 only as static
evidence: its 12 own gathering starts resolved to placements 0.4-1.9 m away, the computed
execute times match the captured SP-flow durations, stack increases match pack 3060101, and
116 respawn intervals fall within the deactivate plus activation delay window. See
`docs/tutorial-gathering.md`.

All nine builder unit tests passed. In an isolated `out/build-gathering` Release tree (to avoid
the concurrently used shared build slot), `chronicles-tests` built without warnings and passed
162 tests with 12 optional private/PostgreSQL tests skipped, including all 17 gathering tests
and the opt-in generated-profile check. The `chronicles-server` target did not link at that time
because of an unrelated in-progress `server.cpp` edit (`server_state_label`).

This is synthetic verification. World entry still always loads map 8000, so no original client
can reach the ArcheWorld gathering path, and no live server was restarted. SP cost, tool
durability, expertise experience and capacity limits remain unimplemented.

## NPC merchants: shop, repair, tokens, housing, vouchers, 2026-10-26

`src/world/features/shop/npc_shop.*`, `src/data/shop_catalog.*` and `src/data/shop_conditions.*` implement one catalog-backed
merchant service for `PC2S_InteractUnitReq` `param.shop`, `param.repair` and `param.voucher_group`:
open, Gold or Hersander Token purchases and sales of stacks, housing objects and equipment
instances, durability repair, evaluated interaction/good/voucher conditions, purchase limits, a
Candidate weight ceiling and trade vouchers. The always-refusing upstream gate is removed; the
target is resolved only through the units the server announced to the session. Gold, tokens,
stack/housing/voucher rows, purchase counters, item rows and repairs commit through
`Store::commit_item_trade` under one gameplay revision (schema v12 and v15). See
`docs/tutorial-npc-shops.md` for evidence labels and rule order.

The shared checkout could not configure or link for long periods while other chats edited it
(two unrelated targets named `chronicles-admin`, unfinished guild, housing, progression and
vehicle tests). The work was therefore built and tested from a private snapshot
(`out/worktrees/shop-build`, synced with `out/worktrees/shop-build-sync.ps1`, which leaves out
only those other chats' unbuildable test sources).

Snapshot results at the end of the work: `cmake --build --preset release --target
chronicles-tests chronicles-server chronicles-account` built without errors;
`ctest --preset release -E housing -j 4 --timeout 120` ran 753 registered tests with the exported
catalog available (integration tests included) and passed all but two: the unbuilt
`chronicles-vehicle-tests` target and
`NativeWorld.BagUnequipWithCarriedWeightAboveDefaultSafeWeightHasNoRefusal`, an access violation
that also reproduces in other chats' independently built test binaries and is unrelated to
merchants. The 43 `NpcShop*`, `ShopConditions*`, `NativeWorld.*Shop*` and
`NativeWorld.TutorialMerchant*` tests pass, including the opt-in replay of every captured price,
refund and the repair totals against the hash-checked export. A disposable PostgreSQL 16 cluster
applied `migrations/001_accounts_characters.sql` twice (idempotence) and passed ten
`Storage.Postgres*`, `TutorialStorage.Postgres*` and `NativeWorld.PostgresShop*` tests including
`Storage.PostgresShopTradeCommitsOrRollsBack` (commit, rollback on equipped removal and stale
wear, replay refusal).

No original-client merchant trial was possible: the tutorial maps place no shop NPC, map 9012
(23 shop, 10 repair, 3 voucher placements) is entered only through a persisted position that
nothing creates, and the only running game client and servers belong to another chat's
deployment, which was not restarted. All results above are synthetic checks, not original-client
behavior.

## GM diagnostics and server commands, 2026-10-26

`src/world/features/gm/gm_diagnostics.*` adds the client's debug-overlay and world-clock GM commands:
`game_time` (PS2C_WorldGameTimeNoti 158), `regen_stat` (DebugPlayerRegenStatNoti 155),
`watch`/`unwatch` and the client debug UI's `GmCmd.debug_watch` (DebugPositionNoti 152,
DebugPathNoti 151, DebugAINoti 150) and `channel_event_info` (DebugChannelEventInfoNoti 159).
`src/world/features/gm/gm_tools.*` adds `suicide`, `kick` (`GmCmd.test_kick`) and `announce`, plus the structured
`regen` and `despawn_npc_all` forms; `src/world/features/gm/gm_commands.*` adds `set_weapon_durability`. Static
client evidence is in `../dumps/ida_research/gm-diagnostics-20261026/FINDINGS.md` (29 IDA names
saved in place). See `docs/gm-commands.md` for the grammar and the 126-field GmCmd coverage table.

Built from the shared checkout in an isolated directory (`out/build-gm-diagnostics`, C++20
Release, shared vcpkg packages). Focused results: `GmDiagnostics.*` 7/7, `GmTools.*`,
`GameMasterCommands.*`, `Combat.GmSuicideKillsThePlayerEvenUnderGmProtection`,
`Combat.MonsterDebugViewReportsTheRouteWithoutChangingTheMonster` and the TLS socket test
`NativeWorld.GameMasterDiagnosticsReachTheIssuingClient` pass; GM, combat and NPC AI suites
together ran 172 tests with 156 passed and 16 environment-gated skips. Full `chronicles-tests`:
864 registered, 786 passed, 77 skipped, 1 failed
(`NativeWorld.BagUnequipWithCarriedWeightAboveDefaultSafeWeightHasNoRefusal`, the known access
violation recorded above, unrelated to GM code). No PostgreSQL run was needed: the change adds no
schema or storage operation (durability uses the existing item-trade commit).

All results are synthetic. No original-client GM session was run: whether the retail client
draws the debug overlays (its X5 debug subsystem may be absent) and whether the sky clock
visibly changes remain unobserved.

## Rankings, raid channels and tribute, 2026-10-26

`src/world/features/events/ranking.*`, `ranking_storage.cpp`, `raid_channel.*`, `channel_event.*` and
`event_gm_commands.*` add bounded runtime services for the previously missing 18-packet group.
See `docs/rankings-raids-channel-events.md` for the contracts and remaining gameplay work.
The feature is **partial**: operator-timed records are not automatic qualified boss scores,
a matched raid is not a scripted encounter, and in-memory offering counts are not durable events.

Synthetic Release validation reused the existing vcpkg cache in `out/build-events`:

- `chronicles-event-tests`: 39 cases registered, 38 passed and the environment-gated PostgreSQL
  case skipped in the normal run. Recovered packet encode/decode, ranking periods/order/own rank,
  raid lifecycle/refusals and tribute commits are covered.
- `RankingStorage.PostgresKeepsBestEntriesAndOrdersTheBoard`: passed separately in a disposable
  loopback cluster. Migration ran twice; concurrent best-entry writes and reopen/rank checks passed.
  A new concurrent reader/writer regression reproduced inconsistent own rank (56 versus 55 in its
  top list). A repeatable-read snapshot now keeps the list, own entry and rank count consistent;
  explicit C collation also preserves MemoryStore's holder-key ordering across database locales.
  The private test cluster was stopped successfully afterward; no persistent service was changed.
- Four Python channel-event extraction tests passed, including the verified private catalog joins.
  The generated profile contains 111 map-event bindings and five map/altar placements.
- Eleven protocol-reference generator tests passed. The previous schema/capture-only non-promotion
  test now uses an explicit missing-runtime fixture, preserving that policy after query support.
- Regression tests reproduced and fixed separate raid groups sharing a map, stale connections
  cancelling another seat and uncommitted tribute reservations appearing complete. Two more
  red/green lifecycle cases show that a map remains reserved until every departing session has
  actually left presence or disconnected, including a member still loading. A last-item TLS trial
  exposed the storage requirement to retain depleted stacks at quantity zero; the tribute planner
  now preserves that contract instead of erasing the row.
- The accepted-finish failure policy is preserved and explicitly tested: a store validation error
  consumes the attempt. This is not a simulated ambiguous-COMMIT test; consuming a failed finish
  also prevents silently retiming an attempt whose database commit may already have succeeded.

Three focused native TLS tests passed: `RankingsRaidChannelsAndTributesOverTls`,
`GameMasterDiagnosticsReachTheIssuingClient` and `HousingContractVisitEditAndExitOverTls`.
The event trial covers lists, GM-timed records, registration/cancel/match, status/debug routing,
the catalog countdown, raid entry, durable parent-map position, housing travel from a raid and
return to its public origin, last-item tribute, leaving before entry and reusing the seat, generic
GM raid-entry refusal, a 96-query burst bound and recovery after the rate window. The housing
regression first reproduced a return to a released raid map, then passed with public-origin
preservation and centralized raid admission. The query burst first passed every request through
storage; the session now allows 32 queries per second and returns sequenced BadRequest above it.

The local-only TLS validation target compiles the existing native socket-test source against the
same world libraries. The event fixture uses the verified catalog and generated event definitions
with a small NPC population, isolating optional full-world monster/static-NPC profiles rather than
mistaking their background packets for event responses. The earlier aggregate rebuild blocker
from concurrent Nui-statue sources and subsequent transient link mismatches were resolved before
these runs. `chronicles-server`, `chronicles-event-tests` and the scoped TLS target build in Release;
this does not certify a fresh full aggregate test run.

An earlier broad run completed 862 tests: 797 passed, 64 skipped and one failed with an access
violation in `NativeWorld.BagUnequipWithCarriedWeightAboveDefaultSafeWeightHasNoRefusal`.
The same failure was reproduced in earlier independently built binaries. Unrelated work was left
intact, and this is not an all-tests-green claim.

Static evidence: one raid countdown UI function was annotated in IDA and saved in place to
`ArcheAgeChronicles.memdump.exe.i64` with stored-sections verification. Pseudocode, fingerprint,
versioned index and journal are under `../dumps/ida_research/rank-raid-20261026/`. Static countdown
formatting is not evidence that the original client automatically sends a move request.

Documentation: feature coverage and all 18 affected packet overrides were updated, with bounded
implemented/partial labels and explicit missing scripts/rewards/recovery. Only the inspected new
feature sources received review fingerprints; unrelated concurrent changes retain their pending
reviews. Local reference regeneration succeeds with an explicit pending-review warning. Strict
`python tools/build_protocol_reference.py --check` still fails on those pending reviews; the Page
also remains stale because access returned HTTP 403 and authenticated Page tools are unavailable.
The last confirmed publication fingerprint was preserved. An independent read-only source review
identified the housing return, request-budget and ranking snapshot issues; its follow-up found no
remaining defect in those bounded fixes. No original-client acceptance or deployment is claimed.

## Combined pending-change review, 2026-10-26

The commit owner reports 137 Python cases run across combat, processing, exploration,
static-NPC, Nui shrine, world-mob, GM-catalog and protocol-reference suites: 136 passed and
one environment-gated case skipped. All 15 focused zone-unlock tests were rerun and passed;
the six launcher source/test fingerprints match the reviewed helper record.

Independent source review checked shared-map combat locking, extreme-coordinate sight
arithmetic and the queue-only quest-EXP contract. Their regression sources are present.
The Nui statue, static-NPC and housing drawing/craft contracts were reviewed and documented
without treating test-source presence as execution. Fresh C++ validation of the latest combined
diff was not run because external builds were active. The previously recorded aggregate-suite
inventory-test access violation remains an unresolved validation gap; this is not an
all-tests-green or original-client acceptance claim.

The canonical local reference/catalog were regenerated after the scoped documentation review;
all 11 reference-generator tests passed. A full Page synchronization hit a concurrent-edit guard
as the Page advanced from sequence 57 to 58. A fresh guarded update then published only the
Nui/static-NPC/housing-product additions and five affected packet notes, with successful readback
at sequence 59. Other concurrent Page wording was preserved. The whole Page is still not a
verified mirror of the generated document, so the previous publication fingerprint remains
unchanged. Strict reference checking remains failing on unrelated pending source reviews and
does not certify this combined commit or full Page synchronization.

## GM world-command expansion and complete field catalog, 2026-10-26

`docs/gm-command-catalog.md` lists all 126 recovered `GmCmd.cmd` fields in descriptor order,
with emulator syntax and explicit supported, limited, unavailable or client-local labels.
`tools/test_gm_command_catalog.py` verifies every field number/name against
`protocol/descriptors.normalized.pb`; its one test passed. The official command CSV grammar
remains unavailable. Chat `/gm <command>` and the Tilde console's `<command>` both use 20009;
structured tools use 20008. See `docs/gm-commands.md` and in-game `help 1` through `help 5`.

The bounded runtime addition in `src/world/features/gm/gm_world_commands.*` covers private GM monster damage/
defeat, timed status lifecycle and ailment gauges, action cancellation, AI pause/resume,
placement, temporary combat stats, server cooldown clearing, prevalidated self-effect chains,
shared-map quest acceptance and validated durable mail to self/online characters. Bag clearing
preserves equipped instances and utility-slot reservations; stat/perk grants retain capacity,
prerequisites and exclusive choices. Shared population and other players cannot be edited by
these unit commands. Missing owning systems remain refused; this is not complete implementation
of all 126 fields. Anti-cheat remains outside this work.

Fresh C++20 Release `chronicles-tests` and `chronicles-server` builds succeeded in
`out/build-gm-diagnostics`, reusing the pinned vcpkg cache. An earlier attempt failed to link
concurrent Nui-statue tests before their implementation became available; the successful build
includes that implementation rather than removing its tests. Existing Windows target-version
notices and unrelated discarded-smart-pointer warnings remain.

The focused filter `Gm*:GameMaster*:Combat.*:NpcAi.*:Progression*:WorldQuests.*:NativeWorld.GameMaster*`
ran **212 tests: 195 passed, 17 environment-gated skips, zero failures**. It includes the new
native TLS/MemoryStore mail delivery and forged-account refusal, structured schema round trips,
parser bounds/unsupported variants, exactly-once monster defeat, AI pause/placement, status
removal, life-state guards, quest supplies/conditions, bag reservations and perk capacity.
Regression cases were observed failing before the fixes for ignored stat/follower variants,
truncated mail currency enums, lost fractional percentages and the 1024-perk capacity bound;
they pass in the final focused run. The mail test's initial zero-time query was corrected to
use the current epoch: mailbox queries intentionally exclude mail sent after their query time.

After relinking the tests against the latest shared-monster library, the expanded filter also
included `WorldMobs.*`: **220 cases, 202 passed, 18 gated skips, zero failures**. The two
shared-mob regressions (cooldown locking across sessions and private-GM reward isolation) pass.
The CTest GM/WorldMobs/native-GM selection registered 50 cases: 49 passed, one generated-profile
case skipped, zero failures. Final full `chronicles-tests`: **926 registered, 842 passed,
83 skipped, one failed**. The remaining failure is the previously recorded access violation
`0xc0000005` in `NativeWorld.BagUnequipWithCarriedWeightAboveDefaultSafeWeightHasNoRefusal`.
An earlier aggregate run before the relink had 924 cases, 839 passed, 82 skipped and three
failures, including the two shared-mob cases whose concurrent runtime fixes were then linked.
The final aggregate result supersedes that earlier binary, and remains an explicit validation
gap rather than an all-tests-green claim.

These are synthetic checks, not an original-client gameplay trial or a deployment. PostgreSQL
was not rerun for this batch; quest/item transactions and mail delivery reuse existing storage
operations and add no schema. The server and any running game were not restarted.

The 126-field catalog check and all 11 protocol-reference generator tests passed again after
the source/documentation review. The generated local reference retains explicit pending
source reviews for shared/concurrent changes; their fingerprints were not blindly refreshed.
Page read/update tools are unavailable in this session, so this GM coverage expansion was not
synchronized to the published Page. Its last confirmed publication fingerprint was preserved;
strict `python tools/build_protocol_reference.py --check` remains blocked by pending source
reviews and the unverified full Page mirror.

## World monster populations, 2026-10-26

`src/world/features/combat/combat_world.cpp`, `tools/build_world_mobs.py` and the native world wiring add shared public
monster simulation, per-character Instance-map populations and isolated matched-raid populations.
The same-build generated profile contains 2,802 slots on 106 populated maps (196 definitions),
470 supported patrols, 941 stat definitions and 1,893 actions. It records missing layers and
excluded activation rules; 765 uncaptured types use explicit custom balance values. See
`docs/world-mobs.md` for the contracts, faction/selection policies and remaining fidelity.

C++20 Release server, main tests and event tests build in `out/build-world-mobs` with the existing
vcpkg installation. Selected CTest verification: 283 registered, 267 passed, 16 optional cases
skipped, zero failed. The module run is 251 passed/16 skipped; all eight WorldMobs tests pass,
including shared health and exactly one death event, a concurrent GM mutation, fresh respawn IDs,
one-time slots, map/interest isolation, private GM loot isolation, independent channels and real
profile loading/encoding with a usable offensive action for every hostile placed type. Fourteen
raid catalog/matchmaker tests pass, including channel IDs remaining distinct across rounds.

Both native TLS trials pass with the verified catalog: two public clients observe the same mob
and death, the killer receives persisted EXP and exclusive corpse loot, a repeated take cannot
duplicate gold, and catalog Instance maps provide separate mob lives per character. TLS uses the
documented child-process `OPENSSL_ia32cap=0` diagnostic; normal-dispatch TLS compatibility is not
claimed. The module and raid runs use normal CPU dispatch. All results are synthetic and the full
aggregate CTest suite was not run. Private logs/reports are `out/world-mobs-{units,native,events}`
with `.log`, `.xml` and `-summary.json` extensions. Binary/profile hashes are in
`out/world-mobs-validation-manifest.json`.

Python extraction/combat/patrol checks pass 105 cases. The protocol-reference generator checks
and final strict-check status are recorded with the generated reference. The PowerShell startup
helper passes syntax parsing and now generates the world profile before launch. No live service
was replaced, no original-client mob gameplay trial was performed and no push was performed by
this implementation session. Navigation, conditional activation, boss scripts, official balance,
party contribution and transient encounter crash recovery remain partial.

The four mob-related feature sections and twelve packet notes were updated on the canonical Page
with guarded edits; final mob evidence and Instance-map ownership were read back at sequence 60.
The local reference regenerates with the explicit pending-review option, and its 11 generator
checks pass. Strict `python tools/build_protocol_reference.py --check` still fails on outstanding
shared-source review fingerprints. The whole-document Page publication fingerprint was retained:
this session verified the mob sections, not every concurrently changed feature in that Page.
Later edits to the shared native world source/test files remain outside the recorded candidate's
binary/source hashes; the validation manifest pins the actual artifacts tested. Unrelated edits
were preserved. This is not a claim that every current shared-checkout feature was rebuilt or
accepted by the original client.

## World-mob live service replacement — 2026-10-26

Follow-up mob correction at 17:13 America/Sao_Paulo: the corrected Release candidate replaces
random-high-dword wire actor IDs with typed 32-bit IDs, while durable kill receipts use separate
server-only identities. Static MoveNoti dispatch proves high-dword zero extension before actor
lookup. Pathless `isRoaming=True` placements now receive bounded local walks: 687 slots, including
163 on map 9012 and all 21 Meadow Wolf slots. The walking fallback has no navmesh/terrain
projection; stationary models and unsupported movement remain explicit. Friendly/private target
validation and route pause/reset paths were corrected alongside this work.

Verification: Release server/main tests build; selected module checks register 268 cases, with
252 passes and 16 optional skips. All 11 WorldMobs cases pass. Two real-catalog native TLS checks
pass using the documented portable OpenSSL child setting; Python mob/combat/patrol checks pass
106 cases. Fifteen analyzed client functions were annotated and saved to the matching IDB with
stored-section verification; cached pseudocode and fingerprints are in
`../dumps/ida_research/world-mob-bugs-20261026/`. These are separate from original-client
acceptance, which remains pending against the corrected artifact.

The corrected candidate was staged with matching DLLs and profile, connections drained, and the
old service shut down gracefully. Both loopback ports and storage-backed admin overview/player
routes passed readiness. PostgreSQL retained its process, accounts/progress were preserved and
no migration was applied during this replacement. The exact server/profile hashes and rollback
path are in ignored `out/mob-bugfix-20261026/running.json`. The earlier re-entry TLS failure was
recovered by re-arming the existing guarded helper and confirming restored original bytes; no
permanent automatic launcher-reconnect acceptance is claimed.

At 19:43 UTC the game service was gracefully shut down with no connected players and replaced
with the previously tested mob Release artifact. Both loopback listeners (8080/8081) were verified
on the replacement process. PostgreSQL retained the same process and
the startup log confirms `World monster profile loaded: 196 maps`. Server/profile hashes match
`out/world-mobs-validation-manifest.json`; the staged executable and rollback metadata are in
ignored `out/world-mobs-live-20261026/`. The child retains the documented portable OpenSSL setting.

This is deployment/readiness evidence. It does not establish original-client spawns, fighting,
loot, EXP or respawn behavior. The attempted client launch predated replacement, and its grant
subsequently returned HTTP 401; no world connection reached the replacement in that attempt.
Storage-backed readiness subsequently exposed absent version-29 character columns
(`nui_blessing_level`, `dedicated_nui_tears`), causing HTTP 500 and the launcher's unavailable-realm
status. A full PostgreSQL custom-format backup was saved, then only the inspected additive
version-29 migration was applied in a transaction. The schema moved from versions 1–28 to 1–29;
admin overview and player-list routes returned 200 afterward. The database and server processes
stayed unchanged. Private backup/hash/SQL evidence is in the deployment directory. No remaining
migrations were applied. A 180-second native capture recorded zero frames; it supplies no mob
gameplay evidence. Fresh launcher sign-in and Play are pending. The selected synthetic results
above remain separate from the requested gameplay acceptance.

## Housing resident list, Construction Workshop and static NPC follow-up — 2026-10-26

Observed local-client evidence: the user confirmed GM lot rental and home teleport, and supplied
screenshots of the housing scene, intermittent empty resident lists, the Construction Workshop
and Station Architect Gyrenn reporting "Unable to find target." This establishes housing entry,
not acceptance of the follow-up fixes.

The follow-up restores owned-site and housing entry channel/version/visit metadata in
`CharSelectRes`. A zero/omitted resident-board channel uses only a known live housing location;
explicit invalid channels still fail. Static client analysis also establishes a local context
gate before sending the board request. Normal Residence Entry List navigation prepares pending
visit context that a scene-only GM teleport does not. Complete recovery of that client-local UI
state remains unverified. Eight analyzed functions have repeatable comments/names, cached
pseudocode, fingerprints and journal entries; the matching IDA database was saved successfully.

The verified map-static-NPC index exports 475 interaction targets, including Gyrenn on map
20101, without duplicate visual spawns or bypassing range/interaction/shop conditions.
Processing now includes shipped housing map bounds explicitly; the default gathering map
selection is unchanged. The generated processing profile has 9 maps, 12 stations, 39 placements,
111 supported recipes and 127 unsupported recipes. The Construction Workshop's 17 building
recipes consume stackable drawings/materials/Gold and produce housing storage quantities while
preserving installed counts in the revisioned transaction. Unsupported company standing and
learned recipes remain restricted.

Follow-up verification:

- Release `chronicles-server` and `chronicles-housing-tests` targets built successfully.
- Focused C++ selection: 58 tests in 7 suites passed, including selection/board metadata,
  real exported Gyrenn/workshop mappings, blueprint purchases, construction, overflow and
  merchant conditions. The isolated focused harness recompiles changed paths against a
  consistent cached support-object set; it is not an aggregate-suite pass.
- Python static-NPC generator: 6 passed; processing: 10 passed; gathering: 9 passed.
- Housing CTest selection: 44 passed, one PostgreSQL-environment-gated skip, no failures.
- Broad C++ validation remains separate; an earlier aggregate-target attempt encountered
  unrelated incomplete progression tests. No original-client follow-up acceptance is claimed.

After explicit user approval, the verified follow-up binary replaced the local server through
the existing graceful-shutdown/staging helper, retaining the persistent database. HTTP health
reported ok with native login and the world listener enabled; both ports belonged to the new
process. Startup confirmed 39 processing station placements and 111 recipes. The user must
reselect the character to receive refreshed selection metadata; no post-deployment game UI
acceptance is claimed.

The generated local protocol reference is updated with explicit pending-source-review warnings.
Page editing tools are unavailable in this session, so the cloud mirror is stale/unverified and
the last confirmed publication hash is retained. All 11 reference-generator tests pass. Strict
reference checking still fails on outstanding shared-source review fingerprints; the unverified
publication also remains a documented gap rather than certifying unrelated concurrent edits.

## Project Chronicles 0.2.0 release packaging (2026-10-26)

See `docs/launcher-release.md` for package contents, native realm policy, signed updates,
prerequisite setup, admin SSH access and exact verification boundaries. Full Release build passed.
Launcher: 82 passed / 4 optional skips. CTest: 1,156 passed / 101 skipped / one Windows TLS
SEH failure under normal OpenSSL dispatch; that case passed with the process-local diagnostic.
Installer install/runtime/uninstall preserved a user sentinel; packaged player rejected local
profile tampering; real local/live admin checks passed. Both backends were upgraded after
restore-verified backups, with PostgreSQL kept running. Hosted original-client acceptance and
a clean machine with neither Steam nor WebView2 installed remain pending. Public player
firewall access remains restricted until hosted acceptance. No capacity claim is made.

## Public player access (2026-10-27 UTC)

At the operator's explicit request, only the backend program's TCP 443 and 8481 were
opened to any source. A synthetic new account registered over public HTTPS (201), a
case-insensitive duplicate was rejected (409), login/realm/roster/signed catalog succeeded
(200), and logout succeeded (204). The catalog matches the published signed 0.2.0 release.
Public administration is still rejected (403), SSH remains restricted, and normal world TLS
validates TLS 1.3. No service restart, data reset or original-client world-entry proof is
implied by this network/configuration and HTTP acceptance check. Private test credentials
and account values are omitted.

## Launcher 0.2.1 offline selection, cancellation and fast startup

The observed stuck build had no adjacent distribution profile and rejected the old
loopback settings under the native player policy. Its initial error was delivered before
the WebView listener attached. Matching build profiles and a trusted UI-ready state replay
fix that path without weakening player origin restrictions. Startup and selection do not
perform optional network work; three failed health checks publish a connection failure.

All 25 focused CTest checks passed, covering launcher selection/error recovery, saved
login, installer cancellation/rollback/fast metadata checks and distribution integration.
The preceding launcher run passed 87 cases with four optional integration skips; the
new fast-startup case then passed in the focused run. All four distribution cases and
11 protocol-generator tests passed. No full emulator-suite rerun is claimed.

Observed UI: Cancel during a large archive hash produced "Update cancelled during file
verification". A queued automatic check could restart after manual cancellation; the
check now consumes that pending request before starting. On the final build the existing
managed live installation reached PLAY, retained the saved launcher login and reported
"Installed game release 0.2.0 is current" about three seconds after startup. Normal
startup checks signed-release identity, the completed marker, file presence and sizes;
explicit Verify / repair hashes contents and detects same-size local changes. Verification
and actual downloading have distinct headings. Offline failure/switching was exercised
with the focused synthetic transport regression; no final offline UI acceptance is claimed.
The local realm was gracefully stopped at zero population during the trial and restored
healthy afterward. Hosted gameplay acceptance remains a separate unverified gate.
### Hosted world entry: launcher 0.2.1 correction (2026-10-26)

- Built only `chronicles-world-tls-local` and the isolated
  `chronicles-world-tls-tests` target, retaining existing library dependencies;
  concurrent account/Discord edits were not rebuilt for this helper.
- All seven hosted TLS identity tests passed: exact DNS, case handling, prefix
  mismatch, missing identity, alternative host lists, weakened certificate
  verification, unsafe inheritance/hostname flags, and invalid/unreadable inputs.
- Fresh original-client hosted trial: the helper passed the selected-hostname
  guard, used OpenSSL's default verifier with SSL_VERIFY_PEER retained, observed
  both world network threads with verify_result zero, and restored both temporary
  fields with readback. The live server accepted the world credential. The game
  displayed its world/tutorial objective and live RTT while the player moved.
- An earlier timeout screenshot came from a process whose old helper was already
  running before replacement. It is not evidence that the new helper failed.
- This proves one hosted world entry on the researched client, not general
  gameplay or 50-player capacity. No server deployment or firewall change was
  needed. IDA annotation persistence remains unconfirmed after its MCP stalled.

### Project Chronicles 0.2.1 live deployment and Discord publication

Project Chronicles 0.2.1 production acceptance (2026-10-27 UTC): source a9bb609 was built and pushed; 127 focused C++ cases passed and four optional fixtures skipped. The isolated PostgreSQL identity/concurrent-first-login check and 16 signed-distribution/backend/updater checks passed. A fresh offsite database backup passed SHA-256 and disposable restore verification; additive migration 30 preserved existing account, character and exploration records. An initial readiness failure rolled back safely; an isolated candidate then passed and the retry finished the old service wrapper before launching the new backend. Live health/auth-options/news/content/catalog routes return 200, Discord is enabled and public administration returns 403. The operator completed real browser authorization, verified an existing-account link, signed out and signed in with Discord in the packaged player launcher. That session launched the original client into the hosted world. The helper checked the selected hostname, observed both network threads with verify_result zero and restored both temporary changes; the server reported the world session and the operator confirmed playable entry. Installer, portable ZIP, checksums, changelog and sanitized release-check log were published by the Project Chronicles bot; independent Discord downloads matched all five local hashes. This is bounded account/entry/release acceptance, not full gameplay or a load-test claim.

### Shared-world gathering correction (2026-10-27)

- Live symptom (hosted server, original client, read from the server log without payloads):
  two Roseberry Bush starts were silently ignored and two were answered with
  `StartInteractionAnimNoti` but never executed. Causes: the doodad gathering path
  (interaction 20001) had no handler; the notification named the raw character ID and a
  100 ns tick instead of the wire unit and the client's nanosecond clock; the reward commit
  did not advance the progress revision that storage requires.
- Isolated Release build `out/build/gathering-20261027`: `chronicles-tests` and
  `chronicles-server` built. 90 focused tests passed (gathering, sectors, movement vitals,
  Nui statues, processing) with two generated-profile skips; with the generated catalog and
  gathering profile the 38 gathering/sector tests passed, including the real-catalog
  gathering-with-mobs TLS test and the new placement/doodad TLS test. Builder tests 15/15.
- The full 991-case suite had 43 failures from concurrent uncommitted work (a new
  `WorldCharUtilityBagNoti` entry frame and an in-progress combat area test), none in
  gathering paths. The corrected server was not deployed and has not been tried with the
  original client; the deployed profile must be rebuilt to format version 2 first.

### Friend lists, recommendations and presence (2026-10-27)

- Trigger: adding a friend in the local client showed "System Error". Only
  `WSC2F_FriendListReq` (20024) was handled; any other friend request closed the front socket.
- Static evidence: the client friend manager was recovered and annotated in IDA
  (`dumps/ida_research/friends-20261027`, 33 `AAC_Friend_*`/front-dispatch/interaction names,
  database saved in place). See `docs/friends.md` for the reply mapping, `FriendInfo` presence oneof,
  error-code mapping and the Candidate one-directional recommendation model.
- Isolated Release build `out/build/friends-20261027`: `chronicles-tests` and
  `chronicles-server` built without new warnings. 15 friend cases passed (10 service including
  store-failure handling, 1 alias, 3 dispatcher wire-level, 1 PostgreSQL), together with the
  existing native WebSocket, native chat and guild cases (49 run, 48 passed, one optional
  exported-catalog skip).
- PostgreSQL: an isolated loopback 16.15 cluster under ignored `out/friends/` applied the
  migration twice (idempotent, schema version 31). The friend persistence case
  (friendship, alias, dismissal and last login across service reconstruction, case-sensitive
  name summaries) and the existing guild PostgreSQL case passed against it.
- `tools/native_smoke_test.py` (memory backend) passed with the new friend search,
  self-add refusal and post-selection friend-list checks on the same front socket.
- The full 993-case suite had 41 failures, all from concurrent uncommitted work: every TLS
  world test stops at the shared entry helper (an extra opcode 140 frame before map load) and
  one in-progress combat area test. The new world exit-hook assertion lives in one of those
  TLS tests and therefore could not run; the world-mode smoke test also fails on other
  current builds. No friend case failed.
- Not deployed and not yet tried with the original client.

### World monster hit, reach and disengagement fixes (2026-10-27)

- Reported with the hosted original client: monsters in front of the player attacked without
  dealing damage, chased a fleeing player indefinitely, and sometimes landed melee hits from
  far away. A read-only live log excerpt (no payloads) showed three casts by one monster and
  no NPC damage. Causes and changes are in `docs/mob-combat-fix-20261027.md`.
- Isolated worktree `out/worktrees/mob-combat-20261027` (unmodified `HEAD` plus only the
  combat and NPC AI sources), Release `chronicles-tests` built. Combat/NpcAi/WorldMobs
  suites pass except `Combat.AreaSpawnHitLandsAtTheCreatedTickFromAThirtyTwoBitAreaUnit`,
  which belongs to concurrent area-spawn work and exercises no changed path.
- Generated world profile, `WorldMobs.GeneratedMonstersLandTheirAttacksOnAStandingPlayer`:
  838 monster types provoked by a player 4 m in front for 10 s; 0 attack without landing a
  hit and 0 never attack. The same simulation on a separately built unmodified `HEAD`
  (`out/worktrees/mob-combat-baseline-20261027`): 152 types attack without ever landing a hit.
- Full isolated `chronicles-tests` with the world, tutorial and catalog profiles: 868 passed,
  72 optional skips. Six native TLS fixtures (two in-place revive, gathering snapshot, fall
  damage, shared-map fall damage and drowning) fail identically on unmodified `HEAD` with the
  current generated data.
- The shared `out/build/windows` tree could not build because of concurrent uncommitted
  gathering changes in `native_world.cpp`. The fix was not deployed and has not been tried
  with the original client; delayed attack dashes and the 10-second give-up remain Candidate
  local policies.


### Hosted authentication reconnect deployment (2026-10-27)

- Reconnect-only source was applied to the deployed 0.2.4 revision in an isolated checkout;
  unrelated shared work was excluded. The candidate passed 29 focused authentication tests,
  three admin regressions, native HTTP smoke and an isolated PostgreSQL restart/expiry test.
- A fresh production backup was hash-checked and restored to an isolated database; the
  additive schema 32 migration passed against that copy. The staged hosted candidate passed
  its separate listener and health smoke before the production switch.
- The hosted backend was switched with verified rollback copies. The new binary, migration,
  manifest, schema, listeners, health and authentication options were verified afterward.
  An original-client disconnect/reconnect trial is pending. No game/session values are recorded.


### Ordinary housing NPC entry, rental admission and guild positions (2026-10-29)

- Observed local-client behavior (live server logs, before this change): opening Nivhen's
  housing window in the public world sent front 20012 with an empty payload (channel 0), which
  the server refused with 2041 `Housing_ChannelNotFound`. An earlier ordinary Residence Entry
  List trial sent channel list 20013 and visit 20015 with an explicit channel; the server moved
  the character into map 20101, received map-load completion and later returned it to its
  origin. Only packet IDs, payload sizes and server outcomes were inspected.
- Static client analysis (`dumps/ida_research/housing-ordinary-20261029`, IDA saved in place):
  the `HousingSiteAuthorize` dialogue task sends no packet; the rent window it opens sends
  20017, 20012 and, on confirmation, 20011. Channel 0 is labelled as the main-world board.
  Visits use the selected list row's channel. No sender or numeric ID exists for either
  release request in this build; front 20016 is `VAcceptQuestReq`.
- Server change: channel-0 board requests resolve to the live housing channel, otherwise the
  primary personal channel (emulator policy); explicit invalid channels stay refused. Player
  contracts require `GeneralHousingSite{S,M,L}positionLv` (Candidate server rule) before any
  charge. The production guild directory now reports roster company positions, which guild lot
  assignments require.
- Synthetic checks: `chronicles-housing-tests` covers channel-0 boards with empty and populated
  primary channels, live-channel precedence, explicit invalid channels, unchanged pending travel
  and contracts across repeated reads, contract company-position refusal without charge, the
  GM path, front 20011 with a `CompanyService`, and the production guild directory. Full
  Release CTest results for the deployed candidate are recorded in the private deployment
  receipts under `out/housing-ordinary-20261029/`.
- Pending: original-client acceptance of the repaired window, rental through Nivhen (which
  needs Blue Salt company level 3 and quest 160000 acceptable), construction, editing, doors,
  reconnect and guild housing on the updated server.
- Deployment (2026-10-29 UTC 20:13): revision `6a10874`, server SHA-256
  `22cd996dc7130ab0afa8612529f5d2cd40e163d7622bcd141c838f13f9ff821b`, replaced
  `691ad316...` (session repair `f44f51f`) after a verified database backup, saved rollback
  copies, an in-game restart notice and a graceful shutdown. Executable and payload-manifest
  hashes, both listeners (443/8481), preserved routes, schema 34 and player/housing row counts
  were verified afterward. Before deployment the candidate passed the full Release suite
  (2006 registered, 124 prerequisite-gated skips, no failures), the exported-catalog housing
  test, isolated PostgreSQL housing/market/ranking persistence and the three original-catalog
  companion TLS scenarios. Ordinary-client acceptance on the updated server is pending.

### Received request features, interactions and mounts (2026-10-29)

- Gliding starts, player root motion and adapted rotation, projectile hit/update reports,
  network delay telemetry, shared chests and records, mount registration animation, recipe
  learning, achievements, NPC threat reactions and preorder replies are described with their
  limits in [docs/received-packet-features-20261029.md](docs/received-packet-features-20261029.md).
- The batch is integrated on the accepted session repair `f44f51f`. The default Release suite
  ran 2,061 cases with 0 failures and 127 environment-gated skips. With the real catalog and
  regenerated profiles one pre-existing placement-data test failed; it fails identically
  without this batch.
- An isolated PostgreSQL cluster applied additive schemas 35 and 36 and passed the restart
  smoke and 12 repository cases, including recipe and achievement receipts.
- No original-client trial of these behaviors preceded deployment.
- Deployed 2026-10-29 21:35 UTC after a restored-backup migration check (schema 34 to 36, all
  counted rows preserved). Binary, manifest, listeners, health, routes, schema, player-state
  counts and new startup profiles were verified on the live host. Original-client trial pending.

### Quest objectives, corpse butchering and tool wear (2026-10-30)

- Quest objective audit: InteractMemo, CounterSuccessCrafting/CounterTryCrafting, Ethereal
  objectives, interact_npc reports and GetItem turn-in consumption, with the evidence table in
  [docs/quests.md](docs/quests.md). Corpse butchering (knife, `param.handling_loot`) and corpse
  lifetimes are described in [docs/monster-loot.md](docs/monster-loot.md); the shared item-trade
  validator now accepts tool wear.
- Default Release suite: 2,088 cases, 0 failures. The same suite with the real catalog and the
  regenerated gathering, world-mob and quest profiles: 0 failures, 98 environment-gated skips
  (PostgreSQL and tutorial-profile cases), including the TLS cases
  `NativeWorld.KillerButchersAWorldMonsterCorpseWithAKnifeOverTls`,
  `WorldMonstersAreSharedAcrossTlsClientsAndKilledByServerCombat` and
  `GatheringSnapshotPreservesWorldMonsterSpawnsAndRewards`. Exporter tests passed.
- Synthetic and real-profile verification only; no original-client trial yet. No schema change.
- Deployed 2026-10-30 06:46 UTC from merged main `f783c5f` (with the combat-special, root-motion,
  quick-slot and company-quest lines) together with the regenerated gathering, quest, tutorial
  and world-monster profiles, after a verified backup, rollback copies and a graceful restart;
  listeners, routes, schema 36, player-state counts and startup profiles (876 fully supported
  quests) were verified afterward. Original-client acceptance is pending.

### Company-gated quests and company level unlocks (2026-10-30)

- Observed (live server log): after LetsMove reached Blue Salt level 2 at its contribution cap,
  Donella offered quest 80101045 and the client's AcceptQuestReq (20016) was refused; the client
  showed "cannot start this quest". The deployed catalog marked the quest unsupported for
  `condition:Company` and `reward:CompanyLevelUnlock`.
- Static client analysis (`dumps/ida_research/company-requirement-20261030`, IDA saved in
  place) recovered the company requirement check order and the capped-ceiling test.
- Server change `c4dfe41`: company conditions are evaluated against `CompanyService` in the
  client's order, and CompanyLevelUnlock rewards apply after the completion commit and at world
  entry. The regenerated catalog differs from the deployed one only by company conditions and
  unlocks (827 fully supported quests).
- Synthetic checks: full Release CTest 2081 passed with 128 prerequisite-gated skips; the
  generated catalog loads; four original-catalog companion TLS scenarios passed.
- Deployed 2026-10-30 04:55 UTC (server SHA-256 `06c2c9fd...`, replacing `043027e1...`) with a
  verified backup, rollback copies, an in-game notice and a graceful restart; listeners, routes,
  schema 36 and player-state counts were verified afterward. Original-client acceptance of
  Donella's quest on the updated server is pending.
- Company contribution rewards (`1c3ccc4`, deployed 2026-10-30 06:04 UTC, server SHA-256
  `090e3d82...`): live data showed Donella's hidden quest 90116021 complete without its single
  CompanyContribution point, leaving LetsMove capped at Blue Salt 1500 (level 2) below the
  level 3 that Nivhen's rent selection requires. The server now credits CompanyContribution
  rewards once after the completion commit. Full Release CTest passed 2083 tests (128
  prerequisite-gated skips; one timing-sensitive WorldMetrics lock test failed once and passed
  on three reruns and the clean rerun). The guarded deployment kept schema 36 and all counted
  rows. An operator repair then credited the owed point (1500 -> 1501) after verifying the
  quest completion; the preparation backup holds the prior row. Original-client rental through
  Nivhen on the updated server is pending.

### Full quest catalog coverage (2026-10-30)

- Goal: every one of the 1,571 client quests supported. Before: 876 fully supported; the rest
  needed job boards, guild/mate/random-loot rewards, doodad or memo starts, acceptance costs,
  item/play-record/adventure conditions or unmodeled objectives, or named a start this client
  build never places.
- Static client analysis (IDA saved in place): the job board widget sends
  PC2S_QuestJobBoardReq when opening a QuestJobBoard doodad, AcceptQuestReq.unit and
  QuestRewardReq.unit_id name the board doodad, and confirmed quest offers send
  AcceptQuestReq.unit for the offered unit (AAC_QuestJobBoard_*, AAC_Quest_SendQuestRewardReqForUnit).
- Server change (branch codex/all-quests): job boards, abandonment and daily repetition
  (schema 37), doodad/memo/server-offered starts, CUSTOM GmOnly for development quests,
  acceptance gates, new conditions, objectives and rewards, quest and portal doodads in the
  shared doodad engine, quest events from combat, trade and item use, and population fixes so
  quest targets exist (dungeon layers, NpcGroup spawners, roaming NPCs). See docs/quests.md.
- Synthetic checks: full Release CTest 2,127 passed with the regenerated quest, gathering and
  world-mob profiles; QuestCatalog.EveryGeneratedQuestIsSupportedAndCompletesWithItsRewards
  completes all 1,571 quests with their rewards; Python builder tests pass (combat-data and
  agent-team failures need ignored capture files absent from the worktree). A server started
  with the regenerated data root loaded quests=1571 fully_supported=1571, 204 monster maps and
  56 NPC population maps; 	ools/native_smoke_test.py --world passed.
- Not established: original-client play of the new mechanics; the official job board draw and
  repeat semantics; destructible objects (DestroyObject is credited, test quest 9088 only).

### World-object equipment, full-wallet rewards and quest profiles (2026-11-01)

- Live report: taking First Hunt's hunting knife disconnected the player. Live logs showed every
  equipment reward from a shared adventure object (the knife, chest gear) rejected with an
  unknown grade name and the connection closed; the stock client then failed world TLS
  reconnects. Graduated characters (full test-kit wallet) also could not turn in gold-paying
  quests, loot kill-gold corpses, sell, deliver trade packs or claim gold achievements.
- Server change `2d1ca8e`: adventure rewards carry the catalog grade name; an adventure failure
  refuses that request instead of closing the connection; CUSTOM full-wallet rule (credits fill
  the wallet to the Money limit and the rest is granted). The live host now also has the
  gathering profile with quest doodads and all 56 NPC placement maps that match its full quest
  catalog. See docs/quests.md.
- Synthetic checks: full Release CTest 2,170 passed with the regenerated profiles; native world
  smoke test passed. Deployed 2026-11-01 12:28 UTC after a verified backup and rollback copies;
  listeners, routes, schema 37, player-state counts and startup profiles (gathering 750 doodad
  points, 56 NPC maps, 1,571 fully supported quests) were verified afterward.
- Not established: original-client acceptance of the knife pickup and full-wallet turn-ins; the
  official full-wallet rule; the client's TLS reconnect failure after a server disconnect.

### Instances, dungeons, portals and equipment quest rewards (2026-11-01)

- Live report: the first cave after the tutorial could not be entered; after relogging the
  character stood in the cave area with no objective. Static analysis (IDA saved,
  dumps/ida_research/instances-20261101): travel volumes send LevelTriggerReq and start a pending
  transition; UI travel sends ChannelTriggerEntryReq. The shared world answered neither.
- Server change `a5cdc93`: map travel profile (tools/build_map_travel.py) and planner
  (world/features/travel) for instances, gated maps, dungeons, portals, map-move cutscenes and
  scripted cutscene/doodad volumes; reconnect inside instances; equipment quest rewards as item
  rows. See docs/instances-and-dungeons.md.
- Synthetic checks: standard Release CTest 2,185 passed; the TLS test enters cave 10119 through
  its volume and leaves it with ChannelTriggerEntryReq {map_id: 0}. Deployed 2026-11-01 13:47 UTC
  after a verified backup and rollback copies; listeners, routes, schema 37, player-state counts
  and startup profiles (212 travel maps, 615 volumes) were verified afterward.
- Not established: original-client entry of instances and dungeons; per-party instance channels;
  same-map dungeon portals and cutscene end points outside the tutorial.

## Regional handoff continuity candidate, 2026-11-01

Local changes retain movement state across the zone-channel map load, reannounce the ridden
companion and reuse/checkpoint an active trade wagon. Focused movement, companion and trade
tests passed, including checkpoint refusal and a simulated interrupted checkpoint. Generated
profile TLS tests passed for a gliding handoff, same-horse transfer with an origin observer,
a driven wagon with two goods, ordinary region travel and same-map recovery. These checks
exercise the server and wire, not the original game client's animations or map-load behavior.

Character and wagon positions still commit in separate transactions. Recovery retains each
record's actual map, but an interruption between commits can put the player and wagon in
adjacent maps; automatic reunion and ambiguous commit outcomes remain unverified. The client
has not been used to observe uninterrupted flight or riding. The existing protocol Page
rejected a targeted update because its document or stored history exceeds the size limit;
the verified publication fingerprint remains unchanged.
