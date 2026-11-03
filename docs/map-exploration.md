# World-map exploration persistence

The map's revealed fog areas are saved per character through the front REST
address advertised at realm entry. Previously neither exploration route existed,
so client discoveries were lost on reconnect.

The matching demo client uses:

- `GET /api/front/CharFogAreaLoadReq/<char_id>`: binary
  `x5.pb.rest.FogAreaLoadRes`, with Success and the saved `fog_area_ids`.
- `PUT /api/front/CharFogAreaSaveReq/<char_id>`: binary
  `x5.pb.rest.FogAreaSaveReq`, with `Content-Type: application/protobuf`.
  The successful response has HTTP 200 and an empty body.
- Both routes use the front session's Bearer token. The server requires that
  the requested character is the session's selected character. An optional
  launcher path prefix is supported; these routes have no hot-key `/1` suffix.

Static analysis shows that the client sends **one newly revealed id** at a time.
Storage therefore unions discoveries and never replaces or clears the saved set.
Duplicate requests, delayed batches and empty saves cannot remove progress.
The server returns success only after the PostgreSQL transaction commits.

Migration 25 adds `emulator_character_fog_areas`, keyed by character and FogArea
CMS id, with cascading character deletion. Ownership is checked by account,
character and realm. A character-row lock serializes concurrent inserts and the
capacity check. Exploration is independent of world-progress revisions, so
concurrent quest, inventory and movement updates cannot overwrite it. The memory
backend has the same merge semantics but is intentionally disposable.

Requests are bounded to 32 KiB and 4,096 ids, with at most 4,096 distinct saved
areas per character. Invalid/nonpositive ids and malformed protobuf fail without
partial changes. When a client catalog is configured, its verified FogArea table
is indexed once at startup and unknown ids are rejected. Without a catalog,
positive ids are range/count checked only. These bounds are local policy.

The client still decides when a fog polygon is discovered. Server-side geometry
and travel validation are not recovered here; discovery grants no reward.
Default-unlocked polygons remain client catalog behavior. The map's **Adventure
Progress percentage** also depends on objectives/collections and is separate from
revealing fog. Ordinary discovery does not fabricate completion counters or unlock every area.
Progress already lost before this implementation cannot be reconstructed from
the screenshot. The stock client may not resend areas discovered earlier in its
current session; a new connection starts the supported save/load lifecycle.

## GM reveal of the complete world map

A game master can enter `/gm reveal_map` in chat, or `/gm unlock_map` as an alias.
The server merges every positive, unique id from the verified `FogArea` catalog into
**the issuing character's** saved discoveries. The researched export contains 1,063
areas. The operation is idempotent, preserves previous discoveries, and uses the same
atomic memory/PostgreSQL union as normal fog saves. Late client saves cannot undo it.
Ownership and realm checks still apply; ordinary player handoffs are refused.
A missing, corrupt, empty, malformed or over-capacity catalog fails without a write.
There is no player-name argument, reset option or arbitrary list of fog ids.

After the private `[GM]` success reply, return to character selection and select the
same character again. The recovered client loads saved fog after character selection;
no live world notification for reloading fog has been established. Rendering all areas
in the original client still requires a live trial. This command changes persisted fog
only: Adventure Progress objectives, rewards, quests, physical collision and zone access
flags remain unchanged. The separately maintained [local zone access helper](local-zone-unlock.md)
addresses disabled demo-zone movement restrictions; it is not part of fog persistence.

For named destinations and authored exploration anchors, use the separately generated
exploration guide documented in [the exploration places guide](exploration-places.md).

## Discovery stops while a dialogue is stuck

The client reveals a fog area from its locality-cell tick. Once per second it converts the
player's position to a 5100 cm cell, and on a cell change it looks up that cell's single
FogArea id, reveals it locally and sends one save. The tick does nothing while any client
"directing" mode is active. An NPC or crafting-station conversation turns on such a mode
when `PS2C_DialogueStartRes` arrives. It turns off only when the client accepts a
`PS2C_DialogueEndNoti` whose `player_unit_id` is the local player's wire unit
(`(character_id << 4) | 1`).

On 2026-10-27 a hosted 0.2.1 client stopped revealing the map right after a crafting-station
dialogue. That release still answered station dialogues with the raw character ID. The client
ignored the notification, kept the conversation open and never ran the locality tick again.
Every fog request the client made had succeeded; it simply made no more. World re-entry in
the same game process did not clear the state. Seamless zone-channel travel uses the same
gate. The correction is the crafting wire-unit change documented in
[processing.md](processing.md); it must be deployed by the server maintainer.

Until that build is live, a player in this state should restart the game client. Areas walked
through while stuck were never saved and must be visited again. Static evidence, 23 annotated
functions and the live read-only memory observations are in
`../dumps/ida_research/fog-discovery-stall-20261027/`.

## Evidence and validation

`protocol/fog-exploration-evidence.json` records the matching-build static
contract. Detailed pseudocode, fingerprints, the function index and annotation
journal are under `../dumps/ida_research/fog-20261026/`. The eight analyzed functions
were annotated in the matching IDA database and saved successfully in place.
Static contract recovery is separate from observed local-client behavior.

Build and run the focused checks from `Emulator/`:

```powershell
cmake --build out/build-escape --config Release --target chronicles-fog-tests chronicles-server --parallel 2
& ./out/build-escape/src/Release/chronicles-fog-tests.exe
python tools/fog_smoke_test.py ./out/build-escape/src/Release/chronicles-server.exe ./out/postgresql-tools/pgsql/bin
```

The C++ suite covers incremental merge, duplicates, empty requests, relogin,
ownership/realm isolation, invalid routes and payloads, catalog membership,
capacity rollback, independent gameplay revisions and character deletion. Its
optional PostgreSQL case additionally covers simultaneous saves, reopening the
store and idempotent migrations. The HTTP smoke starts a disposable database and
server, saves through the native protobuf routes, restarts both the server and
PostgreSQL, logs in again and verifies every discovery survived. It then stops
its private cluster.
These checks do not establish original-client rendering or deploy the feature.

Recorded results on 2026-10-26: the Release `chronicles-fog-tests` and
`chronicles-server` targets built successfully in the reused `out/build-escape`.
All seven focused cases passed with the disposable PostgreSQL environment,
including the real catalog case. The focused CTest run passed six and skipped
the optional database case, which passed separately and in the HTTP runner.
The HTTP runner passed incremental, duplicate, empty and refused saves before
and after one server restart and one database restart; its cluster was stopped.
Evidence is under ignored `out/fog-persistence-fda7ce5e9f/`,
`out/fog-storage-test-c3e843e81f/`, and `out/fog-tests-memory.json`.
The existing `integration.memory` and `integration.native` CTest checks also
passed. The child processes used the existing `OPENSSL_ia32cap=0` diagnostic;
this does not establish a fix for normal OpenSSL CPU dispatch.

The active local game service was not restarted or migrated. Its deployment
belongs to the dedicated maintainer under `docs/chronicles-server-maintenance.md`.

Additional GM reveal verification on 2026-10-26: all eight fog cases passed with an
owned disposable PostgreSQL cluster, including catalog-wide GM reveal, idempotence,
reopened-store readback and delayed ordinary discoveries. The cluster was stopped.
The native synthetic TLS test `NativeWorld.GmMapRevealPersistsOnlyTheIssuingCharacterOverTls`
passed with two authenticated world connections, refusing the ordinary player and
persisting all catalog ids only for the GM. This is synthetic protocol verification,
not an original-client map-rendering trial. The `chronicles-server` Release target built.
The focused `chronicles-exploration-tests` target passed 24 cases with its optional
PostgreSQL case skipped in that run; all eight fog cases, including PostgreSQL, passed
separately with the disposable cluster. The focused target includes strict catalog-id
and GM parser checks. Its CTest entries use the `Exploration.` prefix to avoid collisions.
The wider concurrently edited test target encountered an unrelated missing
`nui_statue.hpp`; no full-suite pass is claimed.

After deployment, reconnect the original client, reveal another area, and
verify that area remains revealed after leaving and entering the world again.
The broader protocol reference retains unrelated pending source-review warnings;
this feature's verification does not certify those concurrent changes.
