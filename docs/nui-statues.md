# Nui Statues, offerings and shrine revival

This service targets the recovered ArcheAge Chronicles demo build
`40367e24241c9481bfc07fbf9ef1919f25fb0eaa`. The catalog calls the statue adventure
object `NuiIdol`; the separate floating collectible is `TearsOfNui`, item 2000.

## Runtime ownership

`tools/build_nui_statues.py` joins the verified catalog and raw map-layer manifests.
The current export has 788 map/spot placements: 512 statues and 276 flames across
118 map IDs. These are exported placements, not 118 hosted maps or every shrine
in future builds. Coordinates and spawn points come from the authored export;
missing layers never create guessed shrines. Common world layers are indexed using
world-asset ownership and three-dimensional Map.area bounds, matching the existing
revival index: runtimeDataLayerPath alone omitted nine of ten officially captured
statue targets. Map bounds do not prove dynamic layer activation policy. Each profile
records build identity,
source hashes, authored execution times, blessing thresholds, utility capacity
vectors and equipped-bag option groups, and revival supply tasks.

`src/world/features/nuia/nui_statue.cpp` owns the session's statue actions. The existing world session
supplies authenticated character ownership, the tracked map and position, storage
commits, live stats and the teleport handshake. The feature loads
`nui-statues/definitions.json` beside the configured client catalog; absence is
logged and leaves the older revival path available for legacy installations.
The normal client export and persistent startup helper generate this file.

### Discovery and activation

Discovery request 20050 records the character-owned Discovered state without a
reward and never lowers an Activated state. Activation uses interaction and
animation 10: StartInteractionAnimReq 20069, ExecuteInteractionReq 20070 and
EndInteractionAnimReq 20071. All five character rigs execute at 5,000 ms; montage
ends legitimately differ (6,000 ms for Nuian male and 6,333 ms for the other rigs).
A 100 ms server timing allowance is shared with the existing interaction policy.

Execution commits Activated state before publishing ActivateAdventureObjectRes 91
and InteractionAnimRes 167. End/cancellation acknowledges with 167. Repeated starts
keep the original deadline, repeated execution grants nothing, cancellation before
execution grants nothing, and actions expire after 30 seconds. Saved states restore
through the existing WorldCharAdventureObjectsNoti 127 snapshot even without the
separate tear-collection service. Direct activation requests cannot skip the
animation. Activation itself grants no HP, SP, items or blessing level.

The local safety policy requires a living character, the correct map and authored
target, a tracked position within 7 m, reported coordinates within the existing
5 m movement tolerance, no conflicting action or mount/teleport, and at most 32
requests per second. These bounds are emulator policy, not recovered official
range or collision rules. Invalid starts use StartInteractionAnimRes 165 rather
than leaving the client waiting on an unrelated common error.

### Tear offerings and permanent rewards

The real client sender uses ItemUseReq 20077, not a dedicated offering opcode. It
copies the selected statue unit ID, the selected tear item CMS/instance identity
and a count capped by the amount still needed to reach the maximum blessing level.
The emulator's existing stackable representation has no instance identity, so an
explicit nonzero item_id is refused consistently with other stackable item uses.
Whether the original client's tear picker emits a nonzero identity remains an
acceptance question; no captured offering is available.

The server validates item 2000, a positive owned count, the nearby authored statue,
remaining capacity and revision before consuming anything. Costs are incremental
NuiBlessingLevel.requiredTearsCount values, not cumulative thresholds. One offering
can cross several levels; dedicated_nui_tears stores only the remainder toward the
next level. The same transaction spends tears and saves the reached level and
remainder. A successful repeated frame sequence cannot spend twice within the
session; lower completed offering sequences are refused. Reconnect has no durable
request receipt and a new valid request is a new offering.

The candidate reply order is ItemsUpdateNoti 169 (use), ChangeNuiBlessingLevelNoti 74,
and, when a level changes, a stat refresh. The 30 catalog levels each grant 20 MaxHP
and no MaxSP, applied cumulatively by CharacterStats. Live stat refresh preserves
current HP/SP; offering is not an invented heal. Migration version 29 stores both
fields with defaults zero and bounds level 0..30 and partial tears 0..100,000.
Memory and PostgreSQL character, world-progress and operator projections preserve
the values. WorldCharBaseNoti exposes them on later entry.

Interaction 28 acknowledges the ordinary Use selection; its OpenNuiBlessing widget
is client-owned. UI reachability and stock-client offering acceptance remain open.

### Utility refill

Interaction/animation 16 is QuickSlotAllItemSupplement. The authored execute event
is 3,408 ms and the montage end is 4,400 ms. It does not activate the statue.

Effective capacity uses the consumable's UtilityBagFullFillCnt group. An equipped
bag in slot 24 selects its supported group's tier at bag catalog level minus one;
invalid indices fall back to zero, matching the recovered runtime getter rather
than the tooltip's display clamp. Without a supported bag/group the base capacity
is used. The client's separate cached per-group tier source is not reconstructed.

Refill economics are deliberately a conservative local policy: existing utility
slots reserve additional units from the character's owned stack up to their
catalog capacity, in slot order, without creating or consuming inventory. Multiple
slots cannot reserve more than the owned total. The revisioned bag state commits
before UtilityBagSetRes 104 and the animation acknowledgement. No official refill
frame was found; this is not a claim that the official service uses these economics.

### Shrine revival and supplies

With the new index loaded, a dead player's ReviveReq selects a same-map authored
spawn, honoring to_nui_flame and Map.nuiFlameOnly. Nearest activated statue selection
is a local candidate policy; no cross-map/type fallback is invented. Exact official
eligibility and selection rules are unresolved.

The server first requests the existing teleport handshake. It retains durable dead
state until TeleportCompletionReq, then atomically commits dead=false, the spawn
position, supply inventory, utility slots and one progress revision through
commit_world_shrine_revival. Only then are revival stats/state and supply packets
published. Disconnect before completion leaves the player dead; conflicts publish
no revival success. Duplicate requests during the handshake do not start a second
revival. The GM in-place revive path remains separate and unchanged.

Authored ReviveTask 1 supplies item 40101101 up to maxCount 5 and requests utility
refill. The local interpretation tops up to five, never adds five on every revival
and never reduces an existing larger stack. The same conservative utility-reservation
policy applies. Exact official supply mutation/reply ordering is not captured.
Gold penalties, death delays and failure-code parity are not claimed.

## Evidence boundaries and remaining features

Static IDA findings and cached pseudocode are under
`dumps/ida_research/nui-statue-20261026/`. The matching IDB was saved with
stored-section verification after both annotation batches. The offering sender,
cumulative preview and equipped-bag capacity getter are static client evidence,
not a local-client trial.

Official P1 activation shows start -> notification -> execution after approximately
5,006 client ms -> Activated response -> interaction response -> end after about
6,335 client ms -> interaction response. It does not demonstrate healing or free
supplies. An official to_nui_flame=true revive begins TeleportReq, echoed start and
Invincible attribute; complete shrine selection/penalty semantics are not recovered.
No official offering, packet 74 or animation-16 success is in the maintained corpus.

The following remain outside the implemented bounded contract:

- Statue interactions 71/72 switch between ordinary and PK field channels according
  to CheckChannel and CheckMapType conditions. A separate PK channel service is not
  available; these interactions fail closed rather than pretending that the existing
  per-player PvP flag is a channel transition.
- Sanctuary status 50030001 and authored combat/NPC-spawn restriction radii are
  recovered, but their activation, entry/exit and removal triggers are unestablished.
  This implementation does not invent sanctuary combat or spawn exclusion behavior.
- Zone-completion behavior referenced by some statue placements, play-record counters,
  offering achievements, exact official refill economics, death penalties and UI
  acceptance remain unverified or unsupported.

## Recorded validation — 2026-10-26

- Release `chronicles-tests`, `chronicles-server` and `chronicles-account` built successfully.
- Focused CTest selection: 36 passed, two PostgreSQL environment gates skipped, zero failed.
  Both skipped cases subsequently passed in the isolated PostgreSQL run.
- Surrounding storage/progression/stats/combat/tutorial selection: 159 passed, 26 optional
  environment/profile/capture gates skipped, zero failed, out of 185 registered cases.
- Isolated PostgreSQL: seven C++ persistence cases passed, including atomic shrine revival
  and blessing round-trip; account/lobby/restart and named-account command checks passed.
  The disposable cluster was stopped. The existing live service/database was not changed.
- Python: ten shrine-export/capture tests, three tear-collection tests and eleven reference
  generator tests passed without skips. All ten official P1/P2 statue activations match the
  corrected common-layer placement export and authored execution deadline.
- Project clang-format dry-run and `git diff --check` passed. Earlier builds exposed a missing
  `progression.hpp` include in concurrently updated `world_quests.cpp`; adding that include
  preserved behavior and allowed the shared targets to compile.

TLS tests used the existing `OPENSSL_ia32cap=0` diagnostic workaround for this machine's
previously documented OpenSSL dispatch crash. Normal CPU-dispatch behavior, a full-suite pass
and stock-client compatibility are not inferred from these scoped checks. Structured evidence
is under ignored `out/nui-statue/validation.json`.

## Verification commands

From `Emulator/`:

```powershell
python tools/test_build_nui_statues.py
python tools/build_nui_statues.py
cmake --build --preset release --target chronicles-tests chronicles-server
ctest --preset release -R 'NuiStatue|ShrineRevival|NuiBlessing|NuiaTears|PlayerEscape' --output-on-failure
python tools/test_build_protocol_reference.py
python tools/build_protocol_reference.py --check
```

Synthetic tests cover activation/commit ordering, timing, duplicate starts/execution,
cancellation, invalid targets and states, offering thresholds/counts/replay protection,
refill conservation, bag option tiers, authored revival supplies, malformed profiles,
character stat rewards, memory/optional PostgreSQL persistence and the real TLS
activation/offering/reconnect dispatcher. A skipped private PostgreSQL/capture gate
is not proof. No deployment, service restart or original-client gameplay trial was
performed by this change.

## Reference publication

The private Page service is unavailable in this session: no Page read/edit tools are exposed.
The last confirmed publication fingerprint is retained rather than pretending to synchronize.
Concurrent runtime source reviews also remain explicit; the strict reference check must keep
reporting those pending sources and the stale Page until their owning reviews/publication
are completed. Local generated documentation is retained.
