# NPC interaction and mount repair, 2026-10-29

This repairs three independently demonstrated gaps. Original-client acceptance of the
candidate remains pending; synthetic TLS exchanges are not visual gameplay proof.

## NPC targets and exported interactions

The hosted catalog had no sibling `static-npcs/definitions.json`. The reviewed local export
contains 726 exact map-scoped static NPC identities on eight maps. Without that file, the
client can display an authored NPC while the server returns `Interaction_TargetNotFound`.
Deploying the file is required: rebuilding only the executable cannot correct that cause.
Startup now reports the number of loaded targets, or warns that the index is missing.

A second defect removed authoritative population NPCs from the session registry when they
left client sight. Returning to sight recreated the client actor but left dialogue and shop
authorization without a target; the next patrol update could also throw. Sight removals now
carry `by_sight=true` and retain the server's map/instance-scoped registry entry. Real despawn
and map departure still remove it. The focused patrol regression failed before the change.
A TLS regression leaves the merchant's sight, returns and opens the same shop successfully.

Live requests also reached Release Field map 9031 while gathering and processing exports
included only Demo fields. Their builders now explicitly include Release ArcheWorld Field
bounds, keeping unrelated exporter defaults and housing opt-in unchanged. The regenerated
gathering profile covers 27 maps, 119474 placements and 466 supported doodad placements;
processing covers 28 maps and 111 station placements. Existing recipe, reward, timing, inventory,
ownership, range, atomic commit and unsupported-type rules remain unchanged.

This does not implement arbitrary unhandled interaction types. In particular, the observed
shared-world chest/action requests outside the existing tutorial and gathering owners still
need their own stateful implementation and evidence. Map 1 requests seen during loading do
not establish a valid shared-world gathering map.

## Mount attachment and lifecycle

The initial horse `UnitAddNoti.npc.mount.passengers` was empty. Only the following
`MountUnitNoti` carried its rider. The client stores the snapshot's passenger list and uses
it when asynchronous actor readiness retries attachment. The initial snapshot now includes
the committed rider, as does an automatically seated trade wagon. The seat-zero optional
field in the subsequent mount notification remains present. Existing 32-bit typed companion
identities are preserved. Two snapshot assertions failed before this change.

Death now unseats a rider once, broadcasts `RiderKilled`, removes the summoned horse, and
settles a dismounted trade wagon through its existing persistence path. This runs after
committed life changes on request and timer paths. Dead players cannot board vehicles.
Disconnect explicitly unbinds viewers before removing the owned unit. Requested landing
positions must remain within the existing seven-metre boarding radius and have finite yaw;
otherwise the server uses the mount's position. That radius is a Candidate server policy.
Decimal unit IDs now reuse the existing overflow-checked parser.

The recovered mount sprint constants are require-SP 1, interval 500 ms, cost 0. No positive
SP cost was invented. Server mount-speed validation, water rules, complete wagon combat,
and exact horse removal animation timing remain open. The server still removes a horse
immediately after dismount; `HorseTable.DisMountActorRemoveDelay=1` is a client hint, not
proof of an official server removal schedule.

## Evidence and verification

Static analysis uses build `40367e24241c9481bfc07fbf9ef1919f25fb0eaa`, input SHA-256
`e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6`.
Eight inspected functions have preserved/added semantic names, repeatable comments,
pseudocode, byte fingerprints and an append-only annotation journal under
`dumps/ida_research/mount-lifecycle-20261029/`. The matching GUI database was saved
successfully in place with stored-section verification. The async seating failure mechanism
is a Candidate explanation until the original client demonstrates successful attachment.

Focused checks include population registry/patrol/map cleanup, decoded mount snapshots,
boarding, movement, ordinary and death dismount, disconnect cleanup, landing bounds,
real TLS horse/wagon exchanges, NPC sight round-trip shop use, generated-profile gathering
and processing tests, and exporter tests. PostgreSQL companion round-trip remains gated
on an isolated test database; no production player state is reset by this repair.

The complete Release build and default CTest run passed: 1853 executed, 120 optional skipped,
1973 registered checks and zero failures. Focused runs passed 90 NPC module cases,
26 companion module cases (one isolated-PostgreSQL case skipped), 40 gathering cases,
28 processing cases and 25 exporter cases. Real-data TLS runs passed all three companion
cases and all 12 NPC cases, including the merchant sight round-trip.

Deployment and original-client acceptance must be recorded separately after they happen.

The published reference Page rejected further edits with `patch_document_size_exceeded`.
Four guarded block edits were confirmed applied; the rest of the synchronization remains
pending. The last confirmed publication fingerprint is deliberately unchanged. Local
generation and reference unit tests pass, but `build_protocol_reference.py --check` must
continue reporting the Page publication mismatch until a complete readback is possible.
