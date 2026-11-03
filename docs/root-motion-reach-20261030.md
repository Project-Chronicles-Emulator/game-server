# Player root-motion "Invalid Request" repair (2026-10-30)

## Report

A player saw an "Invalid Request" modal after some skill uses, more often in a town. The
client shows that text for `BadRequest` (error 4); see [dead-reconnect.md](dead-reconnect.md).

## Live evidence (read-only)

The hosted server's generic refusal log line names the request and response opcodes only.
Across the three newest server logs it recorded 12 `BadRequest` refusals: ten for
`PC2S_StartX5RootMotionReq` (20033) and two for the separate `UtilityBagSetReq` (20080).
In the newest log, 6 of 85 root-motion reports from one session were refused. Each refused
report arrived 200-400 ms after an accepted `StartSkillReq`, usually right after running or a
chain of skills. Payloads are not logged, so the failing check was inferred from captures
(below); no position, character or unit value is recorded here.

## Cause

`CombatSession::receive_player_motion` refused a report when the start-to-destination
distance exceeded `max_distance_mm * 1.05 + 50 mm`, measured in 3D. Replaying that rule over
the 1,407 official 20033 frames in the private captures (2026-10-24..26) refuses 54 of them
(3.8 %), the same order as the live rate:

- **Fixed overshoot.** Reports exceed the authored reach by at most 196 mm independent of its
  length (500 mm to 3,500 mm). A proportional allowance refuses short lunges: skill 2249 is
  authored at 500 mm and reported 590-675 mm.
- **Adapted path longer than its limit.** `Adapted` motions play their whole authored path
  when `adaptedSetupData.maxMoveDistance` is shorter. Skill 1100518 (1 m limit, 2,559 mm
  path) reported 2.4-2.7 m in all 24 frames, which the old rule always refused.
- **Terrain height.** Ground-following motions take their height from steps, slopes and
  ledges; official reports change height by up to 1.5 m. The 3D check counted that against
  the horizontal reach, so stairs and kerbs in towns refused otherwise normal lunges.

No official report had a target that differed from the skill start, a start far from the
skill start, or an end tick after its stage end (maximum +2 ms), so those checks are kept.

## Repair

- The reach is `max(max_distance_mm, path_length_mm) * 1.05 + 300 mm`.
- Ground-following motions compare that reach with the horizontal distance and bound the
  height change separately by the existing 5 m start-position tolerance. The one exported
  non-ground motion keeps the 3D comparison.
- Each refused report now logs its reason (`start_far_from_player`, `beyond_authored_reach`,
  `invalid_yaw`, `target_mismatch`) with the skill id, stage and, for reach, the measured
  distances. No positions or unit ids are logged.

Replaying the new rule over the same 1,407 official frames refuses none of them (none of these
frames uses the single non-ground motion). Teleport distances, foreign skill instances, wrong
stages and late deadlines are still refused.

## Limits

Positions remain client-reported; there is no terrain or navmesh check. The 300 mm margin and
the 5 m height bound are local policy fitted to the captures, not recovered official rules.
The live refusals were attributed by elimination and replay, not by their payloads. Original-
client confirmation in a town after deployment is still required.

## Deployment

Commit `caa91d2` (rebased on the live `aa043f8`) was deployed on 2026-10-30 at 06:19 UTC as a
server-binary-only release. The full Release suite passed (2,086 registered, 0 failed, 128
opt-in skips). Post-switch verification confirmed the running hash, all manifest files,
both listeners, the startup profiles, schema 36 and unchanged player-state counts. Private
receipts are under `out/agent-team-reports/server-maintainer/root-motion-reach-caa91d2/`.
The protocol Page mirror was not synchronized from this session.
