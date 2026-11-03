# From game archives to server data

[← Documentation](../README.md) · [Local setup](getting-started.md) · [Architecture](architecture.md)

There are **two different operations**:

1. **Archive extraction:** read a compatible installed client's archives into a private snapshot.
2. **Runtime export:** validate that snapshot and generate the server-readable catalog/profiles.

`tools/export_client_data.py` performs the second operation. It does not decrypt
PAKs or discover an archive key. The portable archive reader and catalog builders
for the first operation are included under `tools/asset-extractor/`.

Binary-package users: game-server preview.2 includes the runtime generators.
Use the separate [asset extractor release](https://github.com/Project-Chronicles-Emulator/asset-extractor/releases)
for archive extraction, then run stages D onward from the game package directory.
Stage A below applies to a source checkout. Keep both packages in separate folders.

## What you need

- The researched client build, its `X5/Content/Paks` directory and enough disk space
  for a separate extraction. These files are not included in the project.
- Your matching local archive key in a private text file and a compatible local
  Oodle decoder DLL. Neither is distributed here or obtained by the script.
- A matching executable/memory-dump file containing the recovered descriptors for
  protocol extraction. A different or protected on-disk executable may not contain
  the same readable descriptors.
- .NET 10 SDK, Git, Python and `python -m pip install -r requirements-build.txt`.

The archive/footer adapter and feature generators target the researched demo build,
not every Unreal Engine or future Chronicles release. A schema that parses does
not establish behavioral compatibility with another build. Work on a new build in
a separate extraction directory and revalidate identities and hashes first.

## A. Build the portable archive reader

From `game-server/`:

```powershell
pwsh -File tools/build_asset_extractor.ps1
```

The script fetches [CUE4Parse](https://github.com/FabianFG/CUE4Parse/tree/28f385e622468c4ab84997d596b46bb1c3adfcfa)
at the parser revision matching the researched tool, into ignored
`out/dependencies/CUE4Parse`. It builds the extractor using that source and the
checked-in dependency-version override; no private sibling project or precompiled
DLL folder is required. An existing dependency checkout with a different revision
is refused rather than reset. CUE4Parse retains its own upstream license/notices.

Output: `tools/asset-extractor/bin/Release/net10.0/ChroniclesAssetExtractor.dll`.
The extractor does not load game runtime code or contact a game server. It reads
archives and writes an extraction tree. Its output must be outside the archive
source directory; keep it outside the entire installed client directory as well.

## B. Extract a private snapshot

Replace the example paths with your local files. Run from `game-server/`:

```powershell
$paks = 'D:\Games\Chronicles\X5\Content\Paks'
$snapshot = Join-Path (Get-Location) 'out/client-extraction'
$archiveKey = 'C:\ProjectChronicles\private\archive-key.local.txt'
$oodle = 'D:\LocalTools\oo2core_9_win64.dll'

dotnet tools/asset-extractor/bin/Release/net10.0/ChroniclesAssetExtractor.dll `
    $paks $snapshot --extract --key $archiveKey --oodle $oodle
```

Use a new snapshot directory for a new build. The reader inventories archive files,
validates candidate local keys against archive indices, and records extracted file
sizes and SHA-256 hashes. Read the final `failed` count and command exit status:
partial extraction is not a valid input snapshot.

For a small archive-reader check, `--extract-prefix X5/Content/Design/CMS/` restricts
the export to CMS files. That is useful for verification but is **not a full input**
for map, placement or tutorial generators.

## C. Recover protocol metadata and build catalogs

```powershell
$matchingDump = 'D:\PrivateResearch\ArcheAgeChronicles.memdump.exe'
python tools/asset-extractor/extract_protobuf.py $matchingDump "$snapshot/Protocol"
python tools/asset-extractor/build_catalog.py $snapshot
python tools/asset-extractor/verify_extraction.py $snapshot
```

The first command produces original and normalized descriptor sets and a manifest
containing the source hash. The catalog builder converts CMS rows without inventing
new gameplay rules, joins NPC/static placement information and produces
`Catalog/summary.json`. The final command independently reads back extracted files
and compares their hashes to the extraction manifest. Confirm it reports no errors.

The snapshot consumed by the next stage has this layout:

```text
client-extraction/
├── raw/X5/Content/...
├── Catalog/
│   ├── Tables/*.jsonl
│   ├── npcs.jsonl, mobs.jsonl, civilians.jsonl
│   ├── npc-spawners.jsonl
│   └── summary.json
├── Protocol/
│   ├── descriptors.pb
│   ├── descriptors.normalized.pb
│   └── manifest.json
└── extraction-manifest.jsonl
```

If you already have a verified snapshot with this layout, begin at stage D. Do not
rerun extraction against an existing research snapshot just to change one table.

## D. Export the runtime catalog

```powershell
python tools/export_client_data.py --source $snapshot --include-raw
python tools/export_client_data.py --verify-only
```

Omit `--include-raw` only when you need the structured catalog and no raw-data
generators. Verification checks every exported file against its manifest. A matching
complete raw tutorial export also causes the exporter to generate several tutorial,
quest, guide, interaction and exploration profiles; read its output rather than
assuming every optional generator ran.

| Output | Purpose |
| :--- | :--- |
| `data/generated/catalog/` | Runtime tables and hash manifest |
| `data/generated/raw/` | Local raw resources used by offline generators |
| `data/generated/raw-manifest.jsonl` | Raw export identities |
| `data/catalog-manifest.json` | Tracked catalog provenance summary; review before committing changes |
| `data/raw-export-summary.json` | Compact raw-export provenance summary |

Generated client data stays ignored. Commit reviewed code/provenance changes, not
raw assets, archive keys, client binaries, private captures or player/session values.

## E. Generate the profiles needed by your feature

Run generators from the game repository root. These commands use the verified
`data/generated` source and their checked-in default output paths:

```powershell
python tools/build_tutorial_data.py --source data/generated
python tools/build_combat_data.py --source data/generated
python tools/build_gathering_data.py --source data/generated
python tools/build_housing_data.py --source data/generated
python tools/build_stationary_npc_placements.py --source data/generated `
    --map-id 9012 --output-dir data/generated/npc/placements
python tools/build_level_trigger_volumes.py --source data/generated `
    --output-dir data/generated/triggers
```

These are separate outputs, not a promise that each system is complete. Run a
generator's `--help` before changing maps or supplying captures. Some generators
intentionally reject unknown source hashes. Never edit a guard merely to accept an
unreviewed build, and never convert a missing data source into invented official rules.

## F. Load the data deliberately

Add the catalog to the normal game command:

```powershell
.\out\build\windows\bin\Release\chronicles-server.exe `
    --config config/services/game.example.json `
    --client-data-dir data/generated/catalog
```

Several researched tutorial/combat/gathering profile paths are additionally gated by
`--world-bootstrap-diagnostic true` in `src/apps/server.cpp`. For that bounded
development path, append the flag explicitly after preparing the matching profiles.
Authored NPC population also requires the appropriate `--npc-placements FILE` input.
The catalog flag alone does not populate every map or enable every gameplay system.
Use [feature progress](../progress.md) and owning loader/source to identify prerequisites.

For the local tutorial trial, also use `--world-handshake-delay-ms 2000` with the
guarded launcher helper. Verify the startup log reports the catalog and tutorial
definitions loaded. Missing catalog data causes absent health/stamina snapshots;
missing tutorial/combat data leaves skills unavailable even if the map renders.
Restart the isolated game service and reconnect after supplying these inputs.
Do not substitute invented stat values for a missing catalog.

## Verification and boundaries

The portable reader was built from the pinned public parser source and checked on
the local researched client with a CMS-only extraction: 322 files, 67,796,078 bytes,
zero extraction/readback errors. This is archive/data verification, not an original-client
gameplay trial or a fresh full-world extraction. Exact file counts are build-specific.
The code and instructions publish no archive key or decoder binary.

The same tooling also recovered and pool-validated 92 descriptor files from the
matching local dump. Building the CMS-derived catalog, exporting its 286 tables
and verifying the runtime export succeeded. This restricted check did not extract
all raw map assets or validate the full-world profile generation path.
