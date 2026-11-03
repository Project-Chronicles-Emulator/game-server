# World population coverage

The region-routing repair applies to catalog-defined public regions and nested fields;
it contains no region-specific runtime IDs. Reconnects outside a nested field's bounds
return to its authored public parent only when no exit condition applies. Coordinates,
items and progression are preserved. Instances and gated exits retain their identity.
The affected player's Wayfaring Camp region was confirmed working after this repair.

## Audit of the researched extraction — 2026-11-03

The catalog has 198 candidate Demo/Release map entries after excluding private tutorial
maps and developer worlds. This includes instances, reward rooms and test-like catalog
entries; it is not a count of 198 fully playable regions.

| Population input | Candidate maps with definitions |
| --- | ---: |
| Server-owned civilians | 56 |
| Nonempty monster populations | 135 |
| Client-owned static NPC interaction targets | 26 |
| At least one of the above | 151 |
| None of the above | 47 |

These categories overlap. The monster profile contains 204 map records including empty
records and maps outside this candidate set; counting records as populated maps is wrong.
Static interaction targets authorize existing client actors and do not create server NPCs.

All 56 civilian files match regeneration from the verified extraction. A C++ check loads,
registers and encodes every active civilian population across these files. The existing
generated-monster check loads all map records and encodes the initial population on every
nonempty map. Channel-routing checks pass, including preservation of gated exits.
These are synthetic checks, not original-client acceptance in every region.

## Remaining source and behavior gaps

Among the 47 entries with no population definitions:

- 21 contain incomplete or invalid authored spawner data, such as missing selector rows
  or mandatory blank fields. These cannot safely be filled with invented NPC templates.
- Five contain inactive spawners requiring scripts. They must be activated by the owning
  encounter or quest; making them permanently active would change gameplay.
- 21 have no exported spawn placements in their selected map layers. This includes empty
  reward/transition areas as well as unresolved source coverage. Absence alone cannot
  establish whether a map should be empty or whether extraction is incomplete.

The ordinary public region `9091` currently has no population definitions. Its Map row
selects volume layers with no exported NPC spawner files. Other Zone_09 spawner files
exist, but assigning unlisted layers by filename similarity is not a verified repair.
This region remains unsupported pending authoritative layer/activation evidence.

Other population-bearing maps can still contain excluded placements or unsupported scripts.
The audit report preserves those reasons per map rather than marking the entire map complete.

## Reproduce and inspect

After the full data export, run from the game-server root:

```powershell
python tools/audit_world_populations.py --source data/generated `
    --output out/world-population-audit.local.json
```

The ignored local report lists counts, missing layers, excluded spawners and source/output
agreement for every candidate map. A civilian regeneration mismatch fails the command.
The report does not certify gameplay or require distributing any game files. Keep the
generated profiles, raw extraction and detailed local reports out of GitHub.
