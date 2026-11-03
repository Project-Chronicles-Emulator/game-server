# Duplicate static NPC announcements — 2026-10-26

The user reported duplicate NPCs near Gus on map 9012. The map-wide placement
generator treated `staticNpcData` as an ordinary stationary NPC spawner and emitted
`PS2C_UnitAddNoti` for it. The client has a separate catalog/sector static-NPC
lifecycle, so the extra server actor can coexist with the authored static actor.
Gus (CMS 1001799) has one authored static placement, not two server spawn definitions.

`tools/build_stationary_npc_placements.py` now excludes every placement carrying
`staticNpcData`, even when it also carries `npcData`. It counts these records as
`excluded.client_static_npc` before server selector/stance resolution. Ordinary
stationary `npcData` placements retain their previous selection, positions, stance,
GUID and activation fields. This change does not collapse nearby NPCs by template
or distance: different authored actors can legitimately share a model or CMS ID.

The generated map-9012 file changes from 753 placements (752 active) to 145
placements (all active). All 145 ordinary placement records are exactly equal to
their previous records. The removed 608 records were static placements, including
Gus. The raw-source exclusion count is 609 because an additional static record was
already excluded by the older selection rules. Eight missing source layers remain
explicitly reported. Generated data stays ignored and is rebuilt from the existing
hash-checked export; no client game files are edited.

## Evidence and limits

The seven canonical official decoded captures contain 453 NPC-add records whose
spawner GUIDs match ordinary placements in this map file and zero matching static
placements. They contain no Gus NPC-add record. These counts support the ownership
distinction; they do not establish complete static activation semantics.

Static inspection used the existing database for input SHA-256
`e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6`,
image base `0x140000000`, demo build
`40367e24241c9481bfc07fbf9ef1919f25fb0eaa`:

- `AAC_PS2C_SectorUpdateNoti_Candidate_Handler`, RVA `0x61e5b10`,
  iterates field 5 (`spawned_static_npcs`), encodes indices with unit-type nibble
  7 and sector identity, and calls a separate manager through `0x145FA48E0`.
- `AAC_StaticNpcManager_Candidate_LoadSectorBindings`, RVA `0x5fa4210`,
  enumerates catalog sector bindings and placement definitions, checks activation
  fields and resolves or queues their static units.
- `AAC_StaticNpcManager_Candidate_ResolveSectorUnit`, RVA `0x5fa48e0`,
  wraps `AAC_StaticNpcManager_Candidate_ResolveAndTrackUnit`, RVA `0x5fa3f30`.
  The latter tracks per-sector IDs and resolves units through `0x145EB89B0`.

Exact static activation conditions and resolver allocation semantics remain
Candidate. Pseudocode, entry fingerprints, previous/current names, function extents
and the versioned function index are under
`../dumps/ida_research/static-npc-duplicates-20261026/`; four annotation records were
appended to the shared journal. Repeatable comments were read back and IDA confirmed
an in-place save to `D:\PROJETOS\Archeage Emulator\dumps\ArcheAgeChronicles.memdump.exe.i64`
with stored-section verification.

Server-controlled static activation and `PS2C_ChangeStaticNpcStateNoti` remain
unimplemented. The correction removes duplicate ordinary announcements; it does
not add static actors to the ordinary NPC interaction registry. New server-owned
unit IDs follow the reduced placement list, so the file must be loaded on a new
server process and clients must reconnect. The current process retains its
startup-loaded population. No live restart or corrected original-client visual
trial is claimed.

## Checks

- 29 Python generator tests pass, including static ownership, mixed data blocks,
  exclusion counts and ordinary NPC/merchant preservation.
- 11 tutorial-spawn tests and 11 protocol-reference generator tests pass.
- Real-export regeneration and record comparison preserve all 145 ordinary
  placements exactly and exclude Gus from ordinary announcements.
- All 32 C++ population/player-ID/registry checks pass without skips, including
  both real map-file tests, merchant interaction preservation, deterministic IDs
  and protocol encoding. An isolated CMake Release build uses the unchanged owning
  runtime sources and cached pinned vcpkg dependencies. Results are recorded under
  ignored `out/npc-duplicate-validation-20261026/`.

No shared build output, running service or persistent character data is modified
by these checks. Source-review drift from other ongoing feature work remains
separate from this correction.

## Subsequent interaction index

The optional `MapStaticNpcs` index now permits exact authored static identities to reach
supported dialogue and shop authorizers without adding them to the ordinary `UnitRegistry`
or sending duplicate visual spawns. `tools/build_map_static_npcs.py` exports unambiguous,
initially active single-point placements on supported Release or Demo ArcheWorld
Field/HousingArea maps. Demo covers 9011 and 9012, the Shining Shore public channels that
this client build can reach (added 2026-10-27, see [world-latency-20261027.md](world-latency-20261027.md)). Catalog interaction, map and distance checks still apply. Static activation, general
static AI and original-client acceptance remain unresolved. The earlier checks above verify
the duplicate-announcement correction; they are not execution results for this later index.
