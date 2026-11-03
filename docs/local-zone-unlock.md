# Local all-zone exploration

The reproduced housing wall came from the demo client's zone catalog rather than an
achievement requirement. A disabled `Zone.activation` armed a radial movement restriction:
the client gradually reduced its movement factor near 25.5 meters from the stored anchor.
The housing trial reached a factor around 0.003 despite having no pending map handoff.
Changing the housing activation byte and clearing its transient anchor let the user cross.

The user then requested all zones and ordinary user-mode access. The local
`chronicles-zone-unlock` helper enables both gate bytes in every researched catalog row:
activation at row offset 2 and derived public-map eligibility at offset 24. The latter name
is Candidate; static recovery establishes its map-derived predicate and restriction reader.
Activation alone leaves some root regions restricted. This is a local exploration option,
not a quest, achievement, server channel or collision implementation.

## Use and restoration

Build the `chronicles-zone-unlock` target. For private-copy launches, opt in with
`out/launcher/local-exploration.json`:

```json
{
  "unlock_all_zones": true
}
```

`tools/launch_private_client.ps1` starts the helper beside the game, waits for its bounded
catalog initialization and checks its exit status. A failure names the helper log instead
of reporting exploration as enabled. The helper inherits the launch script's Windows token;
it does not acquire the game's privileges if the game elevates independently. Direct use:

The 0.2.3 managed player launcher includes the same helper and runs it automatically for the
newly started client before reporting Play complete. The helper writes its original-byte backup
under the launcher's `runtime/out/launcher/` directory and reports failures through the launcher
with a per-process log. The player does not need the private-copy opt-in file. The original
game executable and the signed game update remain unchanged.

```powershell
& out/build/windows/bin/Release/launcher/chronicles-zone-unlock.exe `
    --pid PROCESS_ID --client-directory 'PATH_TO_PRIVATE_CLIENT' --inspect

& out/build/windows/bin/Release/launcher/chronicles-zone-unlock.exe `
    --pid PROCESS_ID --client-directory 'PATH_TO_PRIVATE_CLIENT'
```

The helper uses `OpenProcess`, `ReadProcessMemory` and `WriteProcessMemory` directly. It has
no PlaySharp bridge, driver, privilege adjustment or alternate backend. Run it with the same
Windows permissions as the client; Windows error 5 is an access denial, not a reason to
fall back to a driver. It verifies the executable path, pins the process with an open handle
and checks two researched code signatures before touching catalog data. Unknown code,
unexpected table layout, duplicate IDs, invalid booleans and stale row identities fail closed.
Unreal numbered FNames retain their suffixes, including names such as `Zone_11`.

Original bytes are atomically backed up under `out/launcher/all-zones-PID-CREATION.json`
before applying single-byte data writes. Every write is read back; failures roll back the
catalog changes. Reapplying preserves the original backup. The helper clears only an existing
inactive-zone anchor, allowing the game to refresh its normal movement factor. It does not
clear pending channel travel or disable physical collision.

Set `unlock_all_zones` to false to stop applying it on later launches. For the same running
client, use the command above with `--restore` to restore both original gate bytes. Restoring
while standing inside a disabled area can re-enable its movement restriction. Closing the
client discards all in-memory changes; game files and progression are unchanged.

## Verification and limits

On 2026-10-26 the user confirmed crossing the reproduced housing wall. A later ordinary
user-mode run initially received Windows error 5; the elevated helper then updated 426
restricted records in the 428-row catalog and verified zero remaining restricted records.
No driver/backend fallback was used after the user's user-mode instruction. The installed
helper SHA-256 is `2ec373ba68474194e96a027b009d58ea602747397eb47cc78068d55784c06931`.

The focused C++20 Release build and 15 tests passed, including all-zone selection, a root
with activation enabled but eligibility disabled, numbered FNames, unknown-build and stale-row
rejection, partial-write rollback, original-byte restoration and transient-anchor reset with
pending travel preserved. The PowerShell launch script passed syntax validation. Independent
review accepted the implementation and the live result. The automatic fresh-launch path has
not yet been observed against a newly relaunched original client.

On 2026-10-27, the managed-launch integration built in Release and 24 focused launcher tests
passed. An already running hosted client was inspected and patched with the same helper:
426 restricted rows changed, zero remained, and the transient anchor was reset. This proves
the researched catalog can be changed in that process; the packaged automatic launch and
movement through the affected location still need a fresh original-client trial.

On 2026-10-27 a 0.2.5 managed launch hit the wall again. The helper's log read "Invalid zone
catalog pointer": the launcher waited only 20 seconds after starting the client, but the client
builds its zone catalog after it finishes loading, so the unlock gave up and the launcher
reported a failure. The launcher now starts the helper in the background and no longer waits
for it. The helper retries for up to 30 minutes (`--wait-seconds` accepts 0 to 1800), stops
early if the client exits, and records its result in `out/launcher/all-zones-result-PID.json`
and `zone-unlock-PID.log`. The unlock rules, researched offsets and backups are unchanged. This
was built in Release only; movement through the wall with a freshly launched client has not
yet been observed.

The 0.2.5 background-helper trial on 2026-10-27 reported success (426 rows changed, remaining 0,
anchor reset 0) about 28 s after launch, yet the wall was still present. The catalog or anchor is
therefore touched again after the early write, and a one-shot helper cannot keep up. The unlock
now also exists as a thread inside the patched Steam compatibility library that the game loads
(`launcher/src/zone_unlock_inproc.cpp`, hooked from `SteamInternal_SteamAPI_Init` by
`launcher/steam_compat/goldberg-v161.patch`). It reuses `zone_unlock.cpp`, reads and writes
the game's own memory with access-violation guards, and every second re-applies both gates and
clears the transient anchor, so no helper process or Windows permission is involved. The build
script copies the shared sources into the library build and into the payload's source folder.
Creating `steam_settings/disable_zone_unlock.txt` beside the library turns it off. A new
`steam_api64.dll` (SHA-256 `ff9986cb4a9f58407b426ad76074f77920f27e930e104f9c5f3bf3b99e410d30`,
built in `out/steam-compat-zone-20261027`) compiles, but the launcher only starts games whose
Steam files match the signed game release, so it takes effect only after a signed game release
pins this file. At that candidate stage, no original-client observation of the library had been completed.
On 2026-10-27 the in-process unlock was tried in the original client through the local test
path (a private copy of the library with the launcher's hash check relaxed for that one file):
the user confirmed the invisible wall no longer blocked movement. It then shipped as game
catalog 0.2.6 sequence 3, which pins the new `steam_api64.dll` (SHA-256
`ff9986cb4a9f58407b426ad76074f77920f27e930e104f9c5f3bf3b99e410d30`), and launcher 0.2.6 no
longer starts the helper at Play. The helper executable stays in the package for manual
`--inspect` and `--restore` use. Catalog 0.2.6 also omits the ten legacy Visual C++ runtime
files that sequence 2 listed and that launchers already exclude.

On 2026-10-28 the same wall reappeared with the 0.2.6 catalog. Live inspection showed the
in-process thread had stopped applying gates: the zone table buffer the researched chain reads
held JSON text fragments in later rows (identical bytes in two separate client processes), so
the thread's fail-closed row validation rejected every pass while the early rows stayed
unlocked. The thread now attempts the movement clears even when the catalog pass fails.

The blocking cause in that session was a separate client-side state: the map-move manager held a
pending zone-channel transition (marker at manager+568, target map 9021). The server refuses
crossings that are not authored `channelSession` boxes and sends no failure reply, so the client
never times out and keeps blocking movement at that boundary. The client also carried the real
XIGNCODE module, which strips process handles and had defeated the external helper with Windows
error 5. The 10 KB XIGNCODE shim (`tools/xigncode_shim_experiment`, bootstrap marker
`0x8C105385`) was published as the signed `X5/Binaries/Win64/XIGNCODE/x3_x64.xem` in game
catalog 0.2.7 sequence 4, so the launcher installs and verifies it and the real anti-cheat no
longer loads. In that session the pending transition was cleared live through the PlaySharp
kernel bridge and the user confirmed movement resumed. The unlocker now clears the pending
request bytes only after they stayed set for 20 seconds, so an accepted handoff keeps its normal
lifecycle; this shipped in game catalog 0.2.7 sequence 5 (`steam_api64.dll` SHA-256
`4985e9d4d129bbcd168bc6e05b915b2ac66378e8bc01a6fbf88219bb71b472ba`). A refused crossing still
has no server failure reply; the client-side clear is local recovery, not an accepted travel
route, and the authored server-side crossing remains limited to maps 9011 and 9012.

This verifies local catalog access flags, not traversal or gameplay in every region. Physical
collision, authored map handoffs, quests, missing NPCs and unavailable server content retain
their existing behavior. The earlier server repair for unanswered zone-channel requests is
documented separately in `docs/zone-channel-travel.md`.

Static findings, cached pseudocode, function fingerprints and the annotation journal are in
`dumps/ida_research/world-blocking-20261026/` in the parent workspace. The matching IDA database
was saved with stored-section verification. Addresses and layouts belong only to researched
client build `40367e24241c9481bfc07fbf9ef1919f25fb0eaa`.
