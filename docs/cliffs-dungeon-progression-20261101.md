# Cliffs of Madness encounter progression — 2026-11-01

Player feedback reports that dungeon entry and monsters are visible after the previous release,
but the barricade after an elite stays closed and the Dimensional Rift is missing after the
captain. This is a player report, not a direct observation of the new candidate.

The official P2 capture associates the defeat of NPC 5001510 with a phase 2 update for the
barricade at sector (170, 289), local ID 5, and a phase 1 update for doodad 777 at local ID 8.
The captain is NPC 5001104. The capture enables doodad 10007 after that defeat, then enables the
rift at sector (170, 290), local ID 2, following interaction 23 with that object. Quest 10000146
requires the captain and then the phase 1 interaction with doodad 10130601 (or an alternative
destination trigger). Its authored interaction moves the player within map 30104.

The candidate updates these specific shared-world objects after server-owned defeat events.
It enables the rift directly after the captain as a custom progression workaround: the captured
post-boss object sequence is not yet implemented. Rift use requires the player's saved captain
objective, and the existing doodad interaction supplies the quest event and authored travel.
The cannon barricade, elite objects and rift can be reconstructed on map entry from saved quest
progress after a server restart. Existing shared object state is not reset during reconstruction.
The authored elite object and rift placements begin deactivated even though phase 1 is their
usable phase. Shared durable phase-1 activations for these two exact placements are included in
late-join and restart snapshots; otherwise a previously activated rift would be omitted as a
default-phase doodad. A generated-profile test covers both boss-source publication and fresh
hydration, with an ordinary initial-phase doodad as a control.

The gathering exporter now preserves each supported placement's authored `deactivation` flag.
The examined source has 59 matching deactivated ArcheWorld points covering 20 doodad IDs;
the regenerated profile also includes three such points in other worlds. When a recorded shared
source has been activated in its initial phase, sector and map snapshots retain that explicit
activation instead of treating it as an ordinary default-phase object. Older profiles retain the
two evidenced Cliffs exceptions. This does not implement or infer the other placements' activation
scripts or generic interaction gating; both still require evidence, as does client rendering.
Interacting with the unlocked doodad 777 in either authored phase starts captured cutscene 1550
and emits the existing doodad quest event; the travel profile exports this server-played scene.
The quest service checks that hidden quest 90100021 is acceptable and that Alight the Cliffs is
either progressing in group 4 or completed, matching the authored interaction conditions.
The quest catalog identifies the hidden quest as a phase 1 or 2 doodad acceptance.

Focused synthetic tests cover map scoping, defeat notifications, restart reconstruction,
repeated restoration, a refused rift interaction before captain credit, and successful quest
event and travel after credit. A focused doodad check covers the captured cutscene selection
and quest event; exporter and travel-session tests verify the scene's authored server-played
metadata, start and visibility restoration. A
two-client TLS test with the generated game profiles checks
that each relevant defeat sends the expected phase to the killer and an observer. These tests
do not establish that the original client renders the barricade, permits the rift interaction,
or completes the whole quest. Per-party dungeon
allocation, party credit, general boss scripts and the captured post-boss object remain open.
