# Remote player movement rollback — 2026-10-27

## Report

On the hosted server (release 0.2.1), a second player joined map 9012. Watching from the other
client, every move that player made snapped backwards ("rollback"). The local client was not
recording `X5Packet.log` at the time, so this report uses server logs and static client analysis.

## Result

The server relays movement correctly but much too late, and with highly variable delay. The
original client re-anchors a remote unit at every `PS2C_MoveNoti` position and teleports it when
the displayed unit is more than 50 cm away. Delay variations of seconds therefore become large
backward and forward teleports. The relay fields are not the cause, and no server correction
packet was involved.

## Server evidence (read-only, hosted 0.2.1)

- The server sends no `PS2C_MoveRollback` (178) or `PS2C_TeleportReq` (181) for ordinary
  movement. The moving player's connection received only `MoveNoti`, unit add/remove, skill,
  stat, quest and interaction frames, so nothing corrected that player's own position.
- `WorldPresence::move` forwards each accepted `PC2S_MoveReq` (20087) to visible peers
  unchanged, adding only `unit_id`. Ordinary clients send `detail`, `pos`, `pred_pos` and
  `pred_milli` (local diagnostics: one request every 100–300 ms while running, about 4 m/s).
- After the 04:25 UTC restart, with two players on map 9012, the time from reading a movement
  frame header to finishing its processing was:

  | Connection | Movement frames | min | median | p90 | p99 | max |
  | --- | ---: | ---: | ---: | ---: | ---: | ---: |
  | 1 | 170 | 0 ms | 158 ms | 2,703 ms | 3,242 ms | 54.9 s |
  | 2 | 135 | 63 ms | 1,432 ms | 2,655 ms | 2,880 ms | 3,037 ms |

  The interval can include payload receive time, but the header and payload of these frames were
  read in the same millisecond in the inspected samples. With one player online, the same
  interval was 40–175 ms ([world-latency-20261027.md](world-latency-20261027.md)).
- The process used about 0.84 CPU cores over its first ten minutes, and host load was 56% of six
  cores during the sample. The other two-player sample also reported about 1.2 cores and nearly
  5,000 `MoveNoti` frames per 40 seconds per session.

The 50 cm rule makes this delay visible. While running at about 4 m/s, a change of only 125 ms
in relay delay between two consecutive frames causes a teleport. A sprinting player reaches the
same limit with about 60 ms.

## Client rule (static, IDA, same build as earlier movement research)

Saved in `../dumps/ArcheAgeChronicles.memdump.exe.i64`. The index and journal are in
`../dumps/ida_research/movement-rollback-20261027/`. These comments correct the earlier "snap"
wording.

1. `AAC_UnitMoveObject_Candidate_ApplyAndTick` (`0x145CC16A0`) stores the `MoveNoti` `pos` as
   the move-object position. It stores `pred_pos` as the target, or `pos` when no `pred_pos`
   is present. It then compares that position with the pawn's current displayed location.
2. `AAC_UnitMoveObject_Candidate_ChooseSnapOrInterpolate` (`0x145CC3890`) leaves the pawn where
   it is when it is within 50 cm of `pos` (Normal mode). Otherwise it teleports the pawn to
   `pos` and preserves the pawn's velocity.
3. The pawn then moves toward `pred_pos` for `pred_milli`. After that window,
   `AAC_UnitMoveObject_Candidate_StepVelocityTowardTarget` (`0x145CC3380`) keeps the same
   velocity until the next frame while `detail` is still moving.
4. The frame `tick` only affects the speed estimate. It does not compensate for delivery delay.

The rollback follows from this rule. A late frame lets the remote pawn keep running from the
previous direction. When the late frame arrives, its older `pos` is more than 50 cm behind, so the
pawn is teleported back. A frame that arrives sooner than the previous one has the same effect.

## Fix direction

The movement relay format does not need to change. The fix is to reduce and stabilize server
processing delay:

- The separate latency candidate on 0.2.1 (`out/worktrees/latency-codex-release`, recorded in
  its `docs/world-latency-release-20261027.md`) caches NPC attack options, sets `TCP_NODELAY`,
  moves per-frame transport traces to `debug`, and skips the civilian-patrol registry scan for
  ping and movement packets. These reduce the shared work that delays every packet.
- Multi-player scaling still needs attention after that candidate. Every session on a shared map
  advances all unowned map mobs at least every 100 ms, and both sessions serialize on the same
  world-mob mutex (`CombatSession::advance_private_encounter`). The protected work therefore
  grows with the number of players on the map.

Acceptance for movement: with two players on map 9012, movement read-to-processed delay should
stay well below 100 ms at the 90th percentile, and a watching client should show no backward
jumps while the other player runs in a straight line. Neither has been measured after a
deployment yet.

## Limits

- The 2026-10-27 live samples were taken on the unchanged 0.2.1 build. No candidate build was
  deployed or observed by this investigation.
- No paired mover/observer packet capture exists. The client rule is static analysis of the
  researched build. The observed rollback is the player's report and matches that rule.
- The protocol reference and its Page were not regenerated for this research-only note.
