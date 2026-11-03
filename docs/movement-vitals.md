# Movement stamina, swimming and underwater damage

Player movement now feeds `MovementVitals` on the world-session worker strand. Its
deadlines join the existing gameplay timer for tutorial, follow-up and supported
shared-map sessions. Consumption and underwater damage continue without additional
client movement packets. Client timestamps never advance the simulation.

## Stamina

The matching exported GameVariable rows provide these defaults:

| Variable | Value used |
| --- | ---: |
| SprintRequireSP | 50 SP to begin a ground sprint |
| SprintConsumeSPPerTick | 3 SP |
| SprintConsumeTick | 600 ms |
| SwimConsumeSPPerTick | 10 SP |
| SwimSprintConsumeSPPerTick | 20 SP |
| SwimConsumeTick | 1,000 ms |

The values are loaded once from the verified catalog. Missing or invalid rows retain
the listed defaults and log the existing catalog warning. Interpreting the two tick
values as milliseconds remains a Candidate until an official movement trace establishes
their timing. Mounted and vehicle locomotion do not consume player movement stamina.

Accepted Normal/Sprint movement begins a ground sprint if sufficient stamina exists.
Swimming consumes its ordinary rate; Swimming/Sprint uses the faster rate. SurfaceIdle
is treated as resting afloat and consumes no stamina. Other stationary swimming still
consumes stamina, including a player who stops while submerged. Those mode/detail
interpretations are bounded local policy based on recovered enum names.

Stamina is the same stat 72 used by skills and consumables; maximum stamina is stat 19.
ChangeStatNoti carries SCS_SprintFlow or SCS_SwimFlow for movement costs. Stamina cannot
become negative. Refused or exhausted sprints emit SprintEndNoti (55) once; a held sprint
does not automatically restart after refusal. A new stop/start can try again. Fractional
drain intervals survive activity toggles, and repeated packets cannot postpone a charge.

Natural stamina regeneration is blocked while exerting. Resting, walking/running normally
and surface floating resume the existing SPRegen stat 25 after the existing one-second
pause/cadence. Combat skill costs and existing equipment/status changes share the same
vitals. This does not implement the catalog's separate 300 ms regen timing variables.

## Diving and surfacing

Recovered MoveMode_Swimming with MoveDetail_SW_Dive enters the submerged state.
MoveDetail_Stop, Up, Down, Run and other swimming details retain that state: stopping or
swimming upward is not proof that the player reached air. Swimming/SurfaceIdle or leaving
Swimming clears it immediately. VH_Submerged details belong to vehicle movement and are
not used as evidence that the player dived.

While submerged, each elapsed second removes 5% of maximum HP, rounded upward. This is
an explicit **local gameplay rule**, not a recovered official drowning formula. The first
tick is one second after diving; there is no invented oxygen stat or breathing grace period.
Natural HP regeneration is blocked underwater. Surfacing cancels pending drowning,
including a late queued tick, and a subsequent dive starts a fresh interval. Past blocked
time is never refunded through a burst of natural recovery.

DamageNoti uses the recovered EX5SkillEffectOriginType_Drowned value 16 and the shared
environmental-damage implementation. Lethal drowning uses the existing player death,
status cleanup, revision-guarded dead flag and equipment-durability transaction before
publication. Reconnect reloads that dead flag; revival clears the movement activity and
underwater state before normal play. Fall damage retains its existing contract.

## Boundaries and verification

Swimming animation and buoyancy remain client driven. Positions/modes/details are
client reports: this change adds no water-volume collision, depth query, forced ascent,
authoritative movement speed or anti-cheat proof of surfacing. A dishonest client can
misreport these states. Underwater state is transient across disconnect. Expertise
swimming-cost bonuses and official oxygen, fall/swim immunity and exhaustion reactions
are not reconstructed. SprintEndNoti is used to stop the client's sprint; a full server
speed/collision authority is still absent. Observer-side damage rendering is unverified.

Map changes, completed teleports/escape, death and revival reset movement state. Timed
vital regeneration now also runs outside the private combat instance, without enabling
private NPC simulation on shared maps. Large clock gaps aggregate bounded changes rather
than replaying an unbounded tick loop.

Unit/schema checks and native TLS tests are in `src/world/features/combat/tests/movement_vitals_tests.cpp` and
`src/world/session/tests/native_world_tests.cpp`. Their execution results belong to the feature reference;
test source alone is not validation. No original-client swimming/drowning trial or live
service deployment is claimed here.

Execution on 2026-10-26: the isolated C++20 Release build passed all 32 focused
cases (16 movement-vitals unit/schema checks, two new TLS/MemoryStore cases and
14 fall checks). The Combat regression run passed 91 and skipped 14 optional
environment checks, with zero failures. Tests used the existing documented
process-local OpenSSL portable CPU-dispatch diagnostic. Synthetic sockets and
MemoryStore reconnects do not certify PostgreSQL or original-client gameplay.
