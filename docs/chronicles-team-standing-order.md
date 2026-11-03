# Project Chronicles Team standing implementation order

Authorized by the human on 2026-10-24. This is an ongoing implementation order,
not a request to stop after planning, research, one milestone, or a coordination
turn limit. The latest explicit human task takes priority over this backlog.

## Work order

1. Fix the original-client combat failure first: the human cannot attack or use
   skills and sees **Cannot use this skill**. Reproduce/trace the exact failure,
   identify the actual client request and server rejection/state, and fix the
   cause. Do not guess from the screenshot or merely suppress the error.
   Read `docs/combat-ui-clarification-20261024.md`: the human corrected the
   conventional-skill-hotbar assumption. Combat prompts are contextual; official
   starting attacks are already available, possibly by weapon. Verify that
   initialization rather than asking the human to assign utility hotbar slots.
2. Complete the combat-related packet-catalog work: player and monster attacks,
   class/weapon/skill availability, stages and combos, target/range checks,
   authoritative costs/cooldowns, damage and defence, effects/status/recovery,
   projectiles/collision, NPC perception/pursuit/skill selection, player and NPC
   death, revival, reconnect/persistence and shared/open-world/PvP behavior.
   Tutorial-only code and passing synthetic tests do not complete this scope.
3. Finish bare unequip, real weight-limit enforcement at item/economy mutation
   boundaries, and missing currencies including Blue Salt and Hersander Token.
   Derive rules from captures, matching-build IDA and catalog data; do not invent
   wire encodings, money amounts or rejection rules to make tests pass.
4. Continue the remaining packet-catalog features in dependency order until
   their actual acceptance criteria are satisfied. A new human order can
   interrupt, reprioritize or replace this default work.

The maintained reference is Page `page_d76bbd1a8b988191a91ed6afa2af9adf`:
https://chatgpt.com/space/page_d76bbd1a8b988191a91ed6afa2af9adf

Use `docs/protocol-feature-reference.md`, `docs/protocol-packet-catalog.json`,
`protocol/feature-coverage.json`, `docs/combat-evidence.md` and
`docs/capture-corpus.md`. The separate workflow tracker is
`protocol/implementation-backlog.json`; its completion state does not replace
the catalog's implemented/partial/missing runtime evidence labels.

## Ownership and continuity

The chief owns prioritization, dependencies, scoped assignments and acceptance.
The human expanded the roster on 2026-10-25: one Opus 5.5 chief, six Sonnet 5.5
builders at high thinking, one Sonnet 5.5 reviewer, four SWE-2 builders at max
thinking and one SWE-2 verifier at max. A separate Sonnet 5.5 high server
maintainer owns live deployment/restart/rollback. All fourteen workers stay in OMP.
The Claude builders advance skill eligibility,
combat mechanics, NPC AI, projectiles/status, equipment/weight and currencies in
parallel where dependencies allow. Maintain an explicit file/interface owner
matrix and use otherwise idle capacity for independent catalog work. Keep common
source registration and integration with one owner; parallelism never permits
overlapping edits. Reserve files, one build owner and one deployment owner. Inspect
existing partial changes and checkpoints before any new assignment. Never
discard another chat's work or start a duplicate worktree/build/research pass.

The SWE group advances disjoint party, missing lobby/character-selection, world
chat and EXP/trait features, checking Claude claims first. Each group requires
its own independent reviewer after its latest implementation assignment. The
shared integration/build owner remains the Claude integrator. Live deployment
belongs only to server-maintainer, following docs/chronicles-server-maintenance.md.
The human authorized necessary emulator-data resets after verified backup; this
exception supersedes the default data-preservation rule only for a proven
incompatibility within that documented scope.

When a milestone finishes, update its evidence and assign the next unfinished
step. When a feature is truly complete, select the next eligible catalog item
without waiting for another human order. Keep the full feature scope pending
across smaller milestones. Clearly record any concrete external blocker and
continue independently executable work; do not silently drop blocked items.

The native task has a 32-turn intake boundary. Save a concrete checkpoint and
stop assigning new work, then let all accepted workers and their deduplicated
final reports drain before closing the bounded task. Final reports are allowed
beyond the intake boundary; no new assignments are allowed there. Never close
as blocked merely because the counter is near its limit while workers are
active: closing cancels them. This boundary does not end the standing order.
The authorized Codex monitor may start a NEW managed task from the checkpoint.
Never reopen ended tasks or replay canceled requests/tool calls. Provider
fallback remains the supervisor's bounded responsibility, not an agent retry loop.

Use the requested Claude and SWE models from the current roster with fresh
confirmed included allowance and Claude headroom for the chief. Do not substitute
Beam, Gemini or other models automatically. Both Devin accounts participate when
either has allowance; leave SWE work unassigned while both are exhausted and
assign it after a live quota check. Claude work continues independently. No paid
overage: a model price reference is not a subscription balance.

**Manual Stop/Pause is not idle.** It halts this work and automatic continuation.
Do not clear paused flags, resume a manually stopped/canceled task, or let this
standing order override a later human instruction.

## Completion and delivery

Track every affected packet and the full feature, including failure paths and
shared-world behavior. Keep a gameplay item in progress while its original-client
acceptance remains unverified. In particular, the reported skill failure is not
fixed until the real client can attack/use the intended skills against the
reviewed running build and the server effects are correct. Record exact evidence
or a specific validation blocker; never substitute synthetic success for live proof.

For every delivery: implementation and focused regression checks, independent
combined review, commit only the scoped changes and necessary reviewed
dependencies, push the exact reviewed revision, deploy it under one service
owner, then independently verify the running binary and relevant gameplay/API
behavior. Preserve the working nanosecond-clock correction, existing launcher
content/news/registration/roster routes, configuration and PostgreSQL data.

Before a live switch, verify an authenticated database backup and the correct
persisted database settings; use the reviewed startup script at the established
project root, not fixture credentials. Verify the current process/path/ports,
stop only the owned service PID and restore it if deployment fails. Never kill
all compiler/server processes by image name or infer a successful build from a
pipeline's final `tail` exit code. A previous model's build may still live in an
OMP worker daemon; check actual OS ownership before starting another build.

Update `protocol/feature-coverage.json` with actual behavior and remaining gaps,
regenerate the reference, and synchronize the cloud Page with guarded edits and
readback. The monitor has the Page connector; report the precise needed sync
instead of declaring unrelated implementation blocked on the mirror. Do not
silently leave the Page stale or refresh publication hashes without readback.

Mark a workflow item **complete** only when its scope, independent review,
commit, remote ref, deployment and required live acceptance are evidenced.
Keep partial, unbuilt, unreviewed, unpushed or undeployed work explicitly pending.
No whole-catalog completion claim while any item or required packet behavior
remains incomplete, unknown or blocked.

Keep raw/decoded capture payloads, identities, chat text and credentials private.
Use the existing analyzed IDA MCP instance, confirming build/status before
research; do not reopen another agent's database or treat schemas as runtime proof.
