# Movement and animation protocol: 2026-10-24 research checkpoint

## Provenance and limits

This is protocol research, not an implementation contract. `protocol/opcodes.json` supplies
wire opcodes; `protocol/descriptors.normalized.pb` supplies field numbers, types, and enums.
Counts below come from the decoded official-client files named in
[`capture-corpus.md`](capture-corpus.md): tutorial `full-tutorial-capture-20261024-152100`
(T, 4,077 frames), `generic-part-1-through-level-6-20261024-153513` (P1, 36,676), and
`generic-part-2-20261024-162411` (P2, 30,219). These three captures have build_id
`40367e24241c9481bfc07fbf9ef1919f25fb0eaa`; **that is not the memory-dump SHA**.
The corpus document records source-file hashes. Analysis used decoded packet types and
sanitized field values, not published account, character, unit, position, token, or session
values. `Observed` means a decoded packet is present in these captures; `Schema only` means
its message definition is known but no instance was found here. A message observed for an NPC
or the local player does not establish a two-player observer relay. No IDA handler was
analyzed or annotated in this checkpoint.

**Critical gap:** `PC2S_MoveReq` and `PS2C_MoveNoti` occur **zero times each** in all
70,972 newly decoded frames. Their schemas describe client movement and peer movement, but
these captures do not establish request frequency, observer notification frequency, relay
latency, interpolation behavior, or why the user sees 1–3-second position jumps. An earlier
local two-client relay report in `docs/world-entry-findings.md` is not a measured official
client cadence. Do not replace this gap with timings of skill or state-transition packets.

## Continuous movement and control

| Message (direction, opcode) | Descriptor fields, in field-number order | T / P1 / P2 | Evidence and interpretation |
| --- | --- | ---: | --- |
| `PC2S_MoveReq` (client→server, **20087**) | 1 `tick:fixed64`; 2 `mode:MoveMode`; 3 `detail:MoveDetail`; 4 `pos:WorldPos`; 5 `body_yaw:float`; 6 `lock_on_unit_id:uint64`; 7 `pred_pos:WorldPos`; 8 `velocity:WorldPos`; 9 `pred_milli:uint32`; 10 `pos2:WorldPos` | **0 / 0 / 0** | Schema only; ordinary movement request candidate by name and matching notification layout. Optional fields are not proved to occur together. |
| `PS2C_MoveNoti` (server→client, **177**) | 1 `unit_id:uint64`; 2 `tick:fixed64`; 3 `mode:MoveMode`; 4 `detail:MoveDetail`; 5 `pos:WorldPos`; 6 `body_yaw:float`; 7 `lock_on_unit_id:uint64`; 8 `pred_pos:WorldPos`; 9 `velocity:WorldPos`; 10 `pred_milli:uint32`; 11 `pos2:WorldPos` | **0 / 0 / 0** | Schema only; `unit_id` distinguishes the moving unit. Distribution to nearby observers is a candidate until a paired official two-client trace establishes it. |
| `PS2C_MoveRollback` (server→client, **178**) | 1 `ec:EErrorCode`; 2 `pos:WorldPos`; 3 `yaw:float` | **62 / 131 / 121** | Observed correction/error; sanitized `ec` distribution: `BadRequest` 205, `Move_UsingSkill` 104, `Move_InvalidRequest` 5. This does **not** establish regular movement traffic. |
| `PS2C_MoveCancelNoti` (server→client, **179**) | 1 `prev_move_mode:MoveMode`; 2 `yaw:float`; 3 `tick:fixed64`; 4 `pos:WorldPos` | **0 / 0 / 0** | Schema only. |
| `PC2S_ManualMovementReq` (client→server, **20059**) | 1 `input:MoveInput` | **0 / 0 / 0** | Schema only; the descriptor exposes `MoveInput`, not decoded input values in these captures. |
| `PC2S_StartGlidingReq` (client→server, **20032**) | 1 `body_yaw:float`; 2 `tick:fixed64`; 3 `pos:WorldPos` | **0 / 4 / 8** | Observed gliding start requests. No paired per-observer movement frames. |
| `PS2C_StartGlidingFailNoti` (server→client, **180**) | 1 `ec:EErrorCode`; 2 `body_yaw:float`; 3 `tick:fixed64`; 4 `pos:WorldPos` | **0 / 0 / 0** | Schema only. |
| `PS2C_SprintEndNoti` (server→client, **55**) | 1 `sprint_failed:bool`; 2 `stamina:int32`; 3 `tick:sfixed64` | **44 / 219 / 355** | Observed sprint-end/resource event, not a periodic position update. |

`MoveMode` values in the descriptor: Normal=0, Falling=1, Crouching=2,
RootMotion=3, Climbing=4, Parkour=5, Swimming=6, Flying=7, Gliding=8.
`MoveDetail`: Stop=0, Walk=1, Run=2, Sprint=3, Up=4, Down=5,
PT_SprintStepUp=21, PT_SwimVaulting=24, GL_Dive=35, SW_Dive=38;
additional turn, flight, parkour, and vertical-control variants are defined. These
are enum possibilities, **not** proof that all transitions occur in the captures. Diving
has detail variants, swimming has a mode, and jumping/falling may use Falling, parkour,
root motion, or an animation mapping: the exact jump request/transition is **Candidate**.

## State, posture, root motion, and animation

| Message (direction, opcode) | Descriptor fields, in field-number order | T / P1 / P2 | Evidence and interpretation |
| --- | --- | ---: | --- |
| `PC2S_UnitChangeStateReq` (client→server, **unresolved**) | 1 `state_type:EX5UnitStateType`; 2 `reason:EX5ReturnReasonType` | **0 / 0 / 0** | Descriptor exists; no assigned wire opcode in `protocol/opcodes.json`. Do not fabricate one. |
| `PS2C_UnitChangeStateNoti` (server→client, **32**) | 1 `unit_id:uint64`; 2 `tick:sfixed64`; 3 `state:EX5UnitStateType`; 4 `reason:EX5ReturnReasonType` | **54 / 1,053 / 618** | Observed state transitions: Combat 643, Idle 593, Demise 319, Alert 126, Return 44. Combat is a unit state, **not** a locomotion mode or proof of weapon animation. |
| `PC2S_ChangeAnimStanceReq` (client→server, **unresolved**) | 1 `new_anim_stance:int32`; 2 `noti:bool` | **0 / 0 / 0** | Schema only; stance-key meaning needs a matching catalog and capture. |
| `PC2S_StartSocialMotionReq` (client→server, **unresolved**) | 1 `type:EX5SocialMotion`; 2 `cur_state:EX5Posture`; 3 `reset_for_only_npc_server:bool` | **0 / 0 / 0** | Schema only. `EX5SocialMotion` defines Change_to_Sit=1, Change_to_Lie=2, Change_to_Default=3; `EX5Posture` has Default=1 and Sit=2. Sitting request route is **Candidate**, not observed. |
| `PC2S_StartX5MotionReq` (client→server, **unresolved**) | 1 `game_anim_mapping_id:int32`; 2 `target_anim_stance_mapping_id:int32`; 3 `loop_count:int32`; 4 `pos:WorldPos`; 5 `body_yaw:float` | **0 / 0 / 0** | Schema only; mapping IDs require catalog interpretation. |
| `PS2C_StartX5MotionRes` (server→client, **68**) | 1 `ec:EErrorCode` | **0 / 0 / 0** | Schema only. |
| `PS2C_StartX5MotionNoti` (server→client, **69**) | 1 `unit_id:uint64`; 2 `tick:sfixed64`; 3 `pos:WorldPos`; 4 `body_yaw:float`; 5 `anim_stance_change_data:AnimStanceChangeData`; 6 `x5_motion_data:X5MotionData` | **0 / 12 / 59** | Observed: 47 stance-change payloads and 24 animation-mapping payloads; sanitized stance mapping keys 0 (41), 50 (3), 33 (3). `AnimStanceChangeData` has `target_anim_stance_mapping_id`; `X5MotionData` has `game_anim_mapping_id`, `target_anim_stance_mapping_id`, `loop_count`. Mapping to sitting, attacks, jumping, or swimming **unverified**. |
| `PC2S_StartX5RootMotionReq` (client→server, **20033**) | 1 `root_motion:X5RootMotion` | **193 / 177 / 173** | Observed event-style root-motion requests, not ordinary walking frames. |
| `PS2C_StartX5RootMotionNoti` (server→client, **64**) | 1 `unit_id:uint64`; 2 `root_motion:X5RootMotion` | **28 / 1,017 / 989** | Observed unit root-motion events; multiple units contribute. Do not assume all are relays of local requests. |
| `PC2S_AdaptedRotationReq` (client→server, **20034**) | 1 `skill_instance_id:uint32`; 2 `stage_index:uint32`; 3 `layer_index:uint32`; 4 `end_yaw:float`; 5 `activate_tick:sfixed64`; 6 `end_tick:sfixed64` | **2 / 1 / 1** | Observed skill-stage rotation. |
| `PS2C_AdaptedRotationNoti` (server→client, **66**) | 1 `skill_instance_id:uint32`; 2 `stage_index:uint32`; 3 `layer_index:uint32`; 4 `end_yaw:float`; 5 `unit_id:uint64`; 6 `end_tick:sfixed64` | **14 / 227 / 250** | Observed unit rotation, not continuous locomotion. |
| `PS2C_AdaptedRotationFailRes` (server→client, **65**) | 1 `ec:EErrorCode`; 2 `skill_instance_id:int32`; 3 `yaw:float` | **0 / 0 / 0** | Schema only. |
| `PS2C_NpcMotionEndSyncNoti` (server→client, **106**) | 1 `skill_instance_id:uint32`; 2 `layer_index:uint32`; 3 `stage_index:uint32`; 4 `end_yaw:float`; 5 `end_tick:sfixed64`; 6 `unit_id:uint64`; 7 `dest_pos:WorldPos` | **11 / 146 / 89** | Observed NPC motion completion, not player walking cadence. |
| `PC2S_EmotionExecReq` (client→server, **20063**) | 1 `emotion_cms_id:int32`; 2 `pos:WorldPos`; 3 `yaw:float` | **0 / 0 / 0** | Schema only in these captures. |
| `PS2C_EmotionExecNoti` (server→client, **123**) | 1 `unit_id:uint64`; 2 `emotion_cms_id:int32`; 3 `tick:sfixed64`; 4 `pos:WorldPos`; 5 `yaw:float` | **0 / 4 / 0** | Observed emote; emotion-to-animation mapping still unknown. |

`X5RootMotion` fields: `start_pos`, `dest_pos`, `end_tick`, `rotation`,
`rotate_duration`, `ground_move`, `skill_instance_id`, `stage_index`, `scale`,
`start_yaw`, `blocked_time`, `target_unit_id`. `EX5UnitStateType` values: Spawn=0,
Idle=1, Alert=2, Combat=3, Return=4, Broken=5, Demise=6. A run/walk animation could
follow `MoveDetail` and motion mapping, but actual observer animation decoding is
**Candidate** without a paired client capture or matching IDA handler.

## Spawn, map change, and teleport boundaries

| Message (direction, opcode) | Relevant fields | T / P1 / P2 | Interpretation |
| --- | --- | ---: | --- |
| `PS2C_UnitAddNoti` (server→client, **10**) | `unit_id`, `nub`, `by_sight`, `debug_ghost`, `spawned_unit` | **counted 2,124 overall** | Observed appearance snapshot. `UnitNub` carries `pos`, `yaw`, and `player`/`npc`; each `PlayerNub`/`NpcNub` includes `state`, `move_mode`, `move_detail`, `anim_stance_key`. Decoded player snapshots include Crouching (8), Gliding (7), Falling (5), Swimming (4), RootMotion (10), besides Normal (319); this establishes state *at appearance*, not transitions. Player detail includes Sprint (117), Run (61) and gliding variants; NPC detail includes Run (530). |
| `PS2C_UnitRemoveNoti` (server→client, **11**) | `unit_ids` (repeated), `by_sight`, `debug_ghost` | **counted 2,111 overall** | Observed despawn/interest boundary; not movement animation. |
| `PS2C_MapMoveStartNoti` (server→client, **175**) | `map_id`, `spawn_point`, `started_timestamp`, `cut_scene_id`, `game_address`, `char_token`, `packet_key` | **3 / 5 / 7** | Observed map-transition boundary; secrets omitted. |
| `PS2C_MapMoveCompleteNoti` (server→client, **176**) | `map_id`, `player_unit_id`, `nub`, `riding_nub`, `raid_start_tick`, `pk_channel` | **3 / 5 / 7** | Observed map-entry snapshot. |
| `PC2S_TeleportStartReq` (client→server, **20088**) | `tick` | **1 / 0 / 6** | Observed explicit teleport sequence. |
| `PC2S_TeleportCompletionReq` (client→server, **20089**) | no fields | **1 / 0 / 6** | Observed explicit teleport sequence. |
| `PS2C_TeleportReq` (server→client, **181**) | `tick`, `unit_id`, `pos`, `body_yaw`, `reason` | **1 / 0 / 6** | Observed explicit teleport instruction. |
| `PS2C_TeleportNoti` (server→client, **182**) | `unit_id`, `pos`, `body_yaw` | **0 / 2 / 2** | Observed teleport notification; do not interpret as a locomotion frame. |
| `PC2S_RoadPathMovementReq` (client→server, **unresolved**) / `PS2C_RoadPathMovementNoti` (server→client, **116**) | request `carriage_id`, `stop`, `trading_post_id`; notification `result:RoadMovementResult` | **0 / 0 / 0** | Schema-only road/carriage path, not established as player steering. |

## Cadence, server reuse, and next evidence

| Traffic | Measured ordinary movement interval | Why |
| --- | --- | --- |
| Client `MoveReq` | **Not measurable: zero requests** | No consecutive matching requests to timestamp. |
| Peer `MoveNoti` | **Not measurable: zero notifications** | No consecutive notifications for one observer/subject pair; no defensible observer/request ratio or latency. |
| Root-motion and state packets | **Not a locomotion cadence** | Per-capture local root-motion-request median adjacent gaps: 749 ms (T), 1,954.5 ms (P1), 2,031 ms (P2). Per-unit root-motion-notification positive-gap medians: 3,505 / 1,700 / 1,548 ms; per-unit state-transition medians: 5,339 / 7,109 / 8,482.5 ms. These sparse mixed-skill/unit events cannot be substituted for movement frequency. |

The current emulator accepts `MoveReq` and retains `last_movement` in
`src/world/session/native_world.cpp`; `WorldPresence::move` copies each request into a `MoveNoti`
for already-visible peers without a batching interval in that function
(`src/world/session/native_world.cpp:1683-1723`). That establishes existing server behavior,
**not** official-client traffic or end-to-end socket scheduling. The observed official
state/motion notifications and spawn snapshots warrant preserving mode, detail, and
stance information rather than treating all position changes as teleports. Next bounded
research needs simultaneous timestamped mover and observer world captures with a known
matching client build (and protected identities), plus read-only IDA handler inspection
after listing the existing instance and checking analysis status. Determine the real
MoveReq/MoveNoti interval and jump/sit mapping before claiming a full movement solution.

## 2026-10-24 follow-up: why ordinary movement is absent

**Observed capture boundaries.** The three `summary.json` files contain only
record totals and direction totals, **not opcode histograms**. Recounting their
private `packet-index.jsonl` files (P2's index is under `X5Packet-001/`) gives:

| Capture | Complete indexed frames | PC2S / PS2C | Unknown opcodes | Opcode 20087 / 177 | Original incomplete tail |
| --- | ---: | ---: | ---: | ---: | ---: |
| T | 4,077 | 1,119 / 2,958 | 0 | 0 / 0 | 11 bytes |
| P1 | 36,676 | 4,123 / 32,553 | 0 | 0 / 0 | 301 bytes |
| P2 | 30,219 | 4,546 / 25,673 | 0 | 0 / 0 | 27 bytes |

**Static collector behavior:** `tools/capture_native_session.py:41-55,79-140`
scans sequential complete records, excludes records preceding arming and an
incomplete final record, looks up opcode solely to *label* it, and writes each
complete record regardless of opcode. Unknown opcodes are retained as
`direction=unknown`; there is no opcode filtering, movement suppression,
deduplication, or rate limit in this collector. The three longer captures
were normalized from original slices (see their summary provenance); P2
explicitly validates every complete frame against its index. The 339 bytes of
reported incomplete original tails do not account for thousands of missing
ordinary movement frames. The original client's `X5Packet.log` only exposes
world application frames that its own logging path emits: **whether that
logger omits a movement transport or whether the official server suppresses
MoveReq/MoveNoti remains Candidate**. No `ManualMovementReq` (20059) or
`RoadPathMovementNoti` (116) appears either; no alternate/batched movement
opcode was demonstrated by the complete indexed frames. Nearby frequent
`StatSyncNoti` (54) is not a positional message by descriptor name.

**Likely logger blind spot (Static, not runtime-proven).** Existing IDA annotation
`AAC_Apply_PacketLoggingPolicies` (`0x14618FC70`) looks up the exact
`PC2S_MoveReq` name at `0x146191414`, then copies two per-packet policy bytes
to globals `0x14BF97B19` and `0x14C3DECE5`. Their IDB *initial* bytes
are both zero; vtable flag getters at `0x145C86C30` and `0x145C87010`
read them. Thus a built-in **client-side, per-packet logging policy exists
upstream of the capture tool**. Its runtime configuration and the meanings
of these two bits have not been recovered, so the zeros cannot prove that
movement is always omitted. They do make an unlogged MoveReq a stronger
Candidate explanation than collector filtering. The corresponding
`PS2C_MoveNoti` receive-side policy is not established yet. Do not conflate
the absence of a logger record with the absence of a network packet.

**Static IDA inspection (read-only):** existing GUI instance `k7ui`,
`ArcheAgeChronicles.memdump.exe`, was listed and `analysis_status` returned
`queue_empty=true`, `finished=true`, `function_count=705939`. Existing
annotation journal under `../dumps/ida_research/` gives dump SHA-256
`e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6`;
the capture build ID is *not* that hash. `PC2S_MoveReq` wrapper vtable
`0x14A735E88` has code references from `0x145C85420`, `0x145C87360`,
`0x145C87AD0`, `0x145CD3750`, and previously annotated
`AAC_PC2S_MoveReq_BuildFromMovementState` (`0x1460B80C0`). That last builder
reads actor position, converts centimeters to protocol millimeters, populates
position and conditional prediction fields, and calls `0x145C85420` to queue
the request. Its caller `AAC_Movement_Candidate_UpdatePivotAndQueueMoveReq`
(`0x1460B6370`) is gated by a movement component and actor/input conditions;
the cached pseudocode ends with diagnostic string `HorsePivotTurn`. Thus the
annotated path cannot yet be equated with every ordinary on-foot move. Other
vtable references establish alternative constructors, not their cadence.
Neither the timer period nor a reliable send interval was recovered in this
read-only pass. These are **Static**, not observed network frames.

**Static MoveNoti gameplay path, verified in IDA (`k7ui`).** Registration
`0x1461AE7F0` binds wrapper deserializer `0x14619E1B0` to callback
`0x1461DF140` (`sub_1461C17F0` pseudocode, call `sub_1461AE7F0(v2,
sub_1461DF140)`). Transport dispatch at `0x1461B86C0` schedules that callback
via `0x1461BD1D0`. Callback `0x1461DF140` selects either actor-category
branch `0x1461DF1E0` or `0x1461DF590`, not a single unconditional
position setter. Both branches convert packed `WorldPos` coordinates from
protocol millimeters to engine centimeters (`* 0.1`), carry `mode` and
`detail`, and branch on the protobuf goal oneof: `goal == 8` treats the
union pointer as `pred_pos`, `goal == 9` as `velocity`. A presence bit controls
`pos2`; a second presence bit controls `pred_milli`. The packet tick is copied
into the downstream movement state, and `lock_on_unit_id` is also read.
This establishes **Static** use of those fields, not yet interpolation
duration, playback delay, or observer frequency. `0x1461DF1E0` routes
`mode == 4` (Climbing) to a distinct actor function `0x146022810`; other
modes flow through `0x146023650`. `0x1461DF590` builds a parallel state
for its alternate actor category and calls `0x14607E610`, a movement-component
virtual method. Do not conflate the two actor categories with local and remote
players without further class/type resolution.

**Tick and cadence still unresolved.** No sender-timer constant or interval
was recovered from the four MoveReq wrapper references. These callbacks
forward the packet tick; they do not themselves prove comparison with synced
server time, stale/late rejection, snap threshold, or interpolation factor.
Those behaviors, and the exact animation triggered by each `MoveDetail`,
require following the downstream component virtual method(s). The official
logger's per-packet policy leaves ordinary movement frequency unmeasurable
from these captures.

IDA names/comments for registration, factory, dispatch, and both actor-category
branches were saved in `../dumps/ArcheAgeChronicles.memdump.exe.i64`
(`idb_save`: `ok=true`, `verification=stored-sections`). Source build identity,
addresses/RVAs, previous/current names, extents, first-16-byte fingerprints,
confidence, and evidence are in
`../dumps/ida_research/movement-20261024/function-index.json` and its
append-only `annotation-journal.jsonl`; three full callback pseudocode files
are under `pseudocode/`. These are static findings for the matching dump,
not observed official MoveNoti frames.

## 2026-10-27 follow-up: remote-player rollback

The remote-unit consumer re-anchors at every `MoveNoti` `pos` and teleports a displayed unit
that is more than 50 cm from it. On the hosted 0.2.1 server, relayed player movement waited
from hundreds of milliseconds to several seconds with two players online, which produced visible
backward jumps. See [movement-rollback-20261027.md](movement-rollback-20261027.md).
