# Human correction: weapon combat inputs and Traits

On 2026-10-24, the human corrected the team's request for a hotbar skill slot
and skill-window screenshots. Use this correction in the active combat diagnosis.

## Human-reported behavior

- This game does not put combat skills in the numbered utility hotbar. Combat
  skill prompts appear on screen while fighting. Do not ask the human to assign
  a combat skill to one of those utility slots or assume an empty slot explains
  the inability to attack.
- The second supplied screenshot is the **Traits** page, not a combat skill
  assignment window.
- In the official game, a newly started character can already attack with
  predefined skills. The human believes these defaults are determined by the
  weapon. Weapon-specific defaults are a hypothesis to verify with the matching
  client data, official captures and IDA, not a confirmed wire contract.

## Screenshot evidence and boundaries

Private originals are preserved under ignored
`out/agent-team-reports/20261024-combat-ui-clarification/`:

- `gameplay.png`: tutorial "Shadows of Memory" / "Head to the Recalling Grounds",
  utility items in numbered slots, axe carried on the character's back.
- `traits.png`: Traits tab, "Axe (Equipped)", "Available Points: 0/0" and the
  weapon-related trait tree.

These new screenshots show an axe character. They must not be misattributed to
the earlier staff character, used as a timed new staff attack attempt, or treated
as proof of a particular key press, successful skill request or attack result.
The human did not supply a precise attack timestamp or key in this correction.

## Investigation direction

Correct the hotbar/learned-skill assumption in the chief's task notes and future
questions. Trace weapon-to-action defaults, current weapon/class/action-set and
stance/draw state, trait/perk inputs where relevant, contextual combat prompts
and the exact client eligibility checks. Compare initial official session state
with the emulator's initialization. Verify whether required weapon/action data
or notifications are absent before changing anything; do not invent unlock
packets, force-learn skills or suppress the error. Keep the original-client
"Cannot use this skill" failure open until the actual behavior is verified.

This correction does not require a restart or interrupt the other builders.
