# Static client data for the emulator

The verified demo-client extraction is under
`G:\Dumps\Archeage\ClientData_2026-10-23`. Its `Catalog/Tables` directory
contains 286 lossless JSONL conversions of the client's CMS gameplay tables.
The derived catalogs contain NPC definitions, classifications, and authored
spawner candidates. See that snapshot's `README.md`, `Catalog/summary.json`,
and `verification.json` for extraction and readback evidence. The original
extractor is `ArcheAgeAssetExtractor/build_catalog.py` in this workspace.

Prepare the server-readable copy from the verified snapshot:

```powershell
cd 'D:\PROJETOS\Archeage Emulator\Emulator'
python tools/export_client_data.py --source 'G:\Dumps\Archeage\ClientData_2026-10-23' --include-raw
python tools/export_client_data.py --verify-only
```

Omit `--include-raw` for the structured catalog alone.

The export creates `data/generated/catalog/Tables/<Table>.jsonl` and
`data/generated/catalog/{npcs,mobs,civilians,npc-spawners}.jsonl`. The root
`data/catalog-manifest.json` and the identical runtime `catalog/manifest.json`
record each file's byte length, SHA-256 digest, record count, the source catalog
summary hash, and the executable dump hash.
It is deterministic and suitable for review when the client snapshot changes.
The 93.6 MB generated catalog is ignored by Git; the manifest and this guide
are tracked. The server should read it once at startup or cache selected tables,
not parse JSONL on each gameplay request. Each table row retains the original
client field names and string values; numeric conversion and foreign-key
validation belong at the consuming boundary.

Pass `--client-data-dir data/generated/catalog` to `chronicles-server` to make
this catalog available to the runtime. The server opens the manifest at
startup and checks the Wild class and every configured starter item. The loader
validates each table's SHA-256 digest when that table is first accessed, then
caches its rows. Without the flag, the server's previous startup behavior is
unchanged.
The desktop launcher and `tools/start_persistent.ps1` pass the flag
automatically when the generated manifest exists.

The CMS tables cover items, classes, character models, skills, effects, quests,
NPCs, zones, and related static gameplay definitions. Authored spawners are not
a live server population. `--include-raw` copies all 11,720 files from the
verified raw extraction into ignored `data/generated/raw/` and checks each
against `extraction-manifest.jsonl`. A compact tracked
`data/raw-export-summary.json` records its manifest hash and totals. The raw
dataset contains cooked assets and map resources for future server systems;
it is not loaded automatically on startup. The export excludes private account
packets, credentials, and local archive key files.
Network schema and opcode inputs remain versioned under `protocol/`.

For a focused row lookup, `Item.jsonl` is at
`data/generated/catalog/Tables/Item.jsonl`, and class metadata is at
`data/generated/catalog/Tables/Character.jsonl`. The original source snapshot
is still the authority for future extraction work; update the manifest and
review changed hashes when replacing it.

## Generated NPC placements and level trigger volumes

Two offline generators derive server-readable data from the exported raw layers. Both read through
the hash-checked `Source` (catalog manifest and `raw-manifest.jsonl`), write nothing outside the
output directory, and are deterministic. They need the export made with `--include-raw`. Their
outputs are ignored by Git under `data/generated/`; regenerate them on every checkout that runs
the server. Run from the repository root:

```powershell
python tools/build_stationary_npc_placements.py --source data/generated --map-id 9012 `
    --output-dir data/generated/npc/placements
python tools/build_level_trigger_volumes.py --source data/generated `
    --output-dir data/generated/triggers
```

Each command prints a JSON line with the output `sha256`. For the export of
`ClientData_2026-10-23` the expected values are:

| Output | SHA-256 | Content |
| --- | --- | --- |
| `data/generated/npc/placements/map-9012.json` | `d94db565ccadeddeb5575d7fd349bed9f509609c37efb546eaf7b3eac28a7345` | 878,427 bytes; 753 stationary placements, 752 active, 8 of the 14 listed layers without a spawner file |
| `data/generated/triggers/ArcheWorld-level-triggers.json` | `82efee8b9759118e106aabf9c1684986355dd3797b318c24f5893f05a099acda` | 1,158,502 bytes; 1,540 volumes from 143 layer files |

A different hash means a different export or a changed generator; compare the `source_sha256`
block inside the file before trusting it. Verify with `Get-FileHash <file> -Algorithm SHA256`.

The placements file is read by the server only when started with `--npc-placements FILE`
(requires `--world-port`). `tools/start_persistent.ps1` takes an optional `-NpcPlacementsPath FILE`:
when it is passed, the launcher throws before touching the cluster if the value is empty or the
file is missing, otherwise it appends `--npc-placements <absolute path>` after
`--client-data-dir`. A relative path resolves against the current directory. The file may live
outside the workspace: it is read-only input and is deliberately not routed through the
launcher's workspace path check. Without the parameter the server arguments are byte-identical
to the launcher before this option. The launcher does not read the file; the server validates it.
The trigger export has no runtime consumer yet.

Pre-reviewed tutorial definitions: by default the launcher regenerates
`data/generated/tutorial/definitions.json` at every start with `tools/build_tutorial_data.py`.
To run with a definitions file that has already been reviewed instead, pass both
`-TutorialDefinitionsPath FILE` and `-TutorialDefinitionsSha256 HEX64` (for example the status
effects profile with SHA-256 `79e35356f85a0ef4482962a9bcde6f00855a71dd18cfa6788ba10878cfd23a52`).
The launcher requires both, hashes the file before any port, cluster or data step, and refuses a
missing, blank or wrong-hash file. It then installs the file at the definitions path (copy aside the
current file first if you need a rollback copy), checks the installed hash again and skips
regeneration. Without these two parameters the behaviour, including regeneration, and the server
arguments are unchanged. The maintainer-pinned variants
`start_persistent.chronicles-bc76d58-profile79e35356.ps1` (`ff135e1b...`) and the wrapper `b2bd36e9...`
are to be retired after a guarded adoption of this option.

Launcher pinning: a deployment that runs a hash-pinned copy of the launcher must re-pin it at its
next deployment. The launcher with these options has SHA-256
`178585835897531d5f58ab6daadde275dcb384e5ac054f61347f637d6497a400` (LF line endings); the
version with only the placements option had
`fee44cb69025f675931d5ec8db5233cb9ee613990cb1e0704da45d92792d8602`, the first version of that
option `2053199cce5f40d437bc7a92072d8efe9b19fe010cda7a2550414b2d525f89a3`, and the launcher before
both `0d32947e072698a4f4e935cc1d671c05e8ac5aceb184d8bff452e13318f03b5f`. The separate
`start_persistent.chronicles-reviewed*.ps1` copies are not part of the repository.

Limits that apply to this data:

- The placement set is partial: the eight listed layers without an `npc_spawner.json` are absent
  from the client data itself, not lost by the export. Placement height is approximate and the
  spawner point and candidate choices are documented as Candidate in the file's `rules`.
- Trigger volumes name a destination map and a `spawnPositionId`, but no table resolves that id,
  so an arrival position is not derivable. The one observed `channel` transition went to a
  different map than its authored id.
- The files hold catalog data only: no captures, player values, sessions or credentials.
