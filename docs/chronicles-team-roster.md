# Project Chronicles Team: Claude implementation group and SWE feature group

The human expanded the team on 2026-10-25 after upgrading Claude to 20x.
All fourteen identities run in OMP, with a separate durable session and serial
queue per identity. One Opus chief coordinates both groups. File reservations,
the integration build and deployment are coordinated across the whole team.

| Alias | Group / role | Model | Thinking | Responsibility |
| --- | --- | --- | --- | --- |
| chief | Claude chief | Claude Opus 5.5 | high | Priorities, ownership, acceptance and checkpoint handoff |
| builder | Claude builder | Claude Sonnet 5.5 | high | Combat core and sole shared source integration/build owner |
| builder-2 | Claude builder | Claude Sonnet 5.5 | high | Skill eligibility, costs, cooldowns and focused regression checks |
| builder-3 | Claude builder | Claude Sonnet 5.5 | high | NPC AI and runtime interfaces |
| builder-4 | Claude builder | Claude Sonnet 5.5 | high | Combat data, projectiles and status effects |
| builder-5 | Claude builder | Claude Sonnet 5.5 | high | Unequip and catalog-derived inventory weight rules |
| builder-6 | Claude builder | Claude Sonnet 5.5 | high | Currency rules and persistence handoff |
| reviewer | Claude reviewer | Claude Sonnet 5.5 | high | Independent review of Claude changes and final shared integration |
| server-maintainer | Claude maintenance | Claude Sonnet 5.5 | high | Latest remote main deployment, server restarts, backup, necessary reset and rollback |
| swe-1 | SWE builder | Devin SWE-2 | max | Party membership, leadership, reconnect and party packets |
| swe-2 | SWE builder | Devin SWE-2 | max | Missing lobby and character-selection behavior |
| swe-3 | SWE builder | Devin SWE-2 | max | World/local chat and authorized delivery |
| swe-4 | SWE builder | Devin SWE-2 | max | EXP/level progression, followed by traits and their persistence |
| swe-verifier | SWE reviewer | Devin SWE-2 | max | Independent verification of all SWE feature work |

These are initial areas, not permission to bypass current claims. Before each
assignment, the chief confirms the actual implementation gap, issue, owned
files, interface, worktree, checks and acceptance criteria. EXP and traits are
sequenced inside swe-4's scope. Existing claims require an explicit handoff.

```text
tools/agent_team/
  dispatch.py             # Validates group limits and persists agent requests
  dispatch.example.json   # Public fourteen-agent configuration without secrets
  omp.py                  # Owns each native OMP session and concurrent worker
  tasks.py                # Drains accepted work and requires each group's review
  model_pool.py           # Checks live included allowance before execution
  profiles/
    chief.md              # Coordinates both groups and final delivery
    builder.md            # Implements a reserved source slice
    reviewer.md           # Independently checks its group's work
```

## Runtime and review boundaries

The Claude group has at most six builders and one reviewer; the SWE group has
at most four builders and one verifier. Each identity processes only one request
at a time. Both groups can run concurrently under the shared chief.

Completion requires an independent reviewer from every group that implemented
work. Its review must follow that group's latest builder assignment. The Claude
integrator owns shared headers, source registration, migrations and integration
builds. The separate server-maintainer exclusively owns live deployment/restart
and rollback; it obtains an explicit build-slot handoff. Read
`docs/chronicles-server-maintenance.md` for the human-authorized necessary data reset
policy. SWE workers submit disjoint modules and integration handoffs.

## Models and allowance

Exact selectors are `anthropic/claude-opus-5-5`,
`anthropic/claude-sonnet-5-5` and `devin/swe-2`. The live OMP catalog confirms high
thinking for Claude and max thinking for SWE-2. Both saved Devin accounts already
report the Max subscription tier; no purchase is part of this setup.

Models are pinned without automatic cross-provider substitution. The runtime
checks fresh included allowance across both Devin accounts, retains Claude
headroom for the chief, and does not authorize paid overage. Claude's upgraded
account reported fresh capacity at configuration time. Both Devin accounts were
exhausted until 2026-10-25 08:00 UTC (05:00 Sao Paulo); verify live allowance before
assigning SWE work rather than assuming this snapshot is current.

When SWE is exhausted, leave its work unassigned, keep Claude work moving, and
start the four SWE builders and verifier when included capacity returns. Do not
submit doomed requests that block a shared chief task. Native-only Zen free
routes previously rejected OMP with HTTP 403 and remain disabled.

Manual Stop/Pause wins: never clear paused flags or reopen canceled requests.
At 32 turns, stop new intake, drain accepted work/reports, then create a NEW
checkpoint from saved progress. Read `docs/chronicles-team-standing-order.md` for
complete delivery and original-client acceptance gates.
