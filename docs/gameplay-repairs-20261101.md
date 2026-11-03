# Gameplay repairs — 2026-11-01

These changes are local work in a shared, already-modified checkout. They have not been
validated in the original client or deployed by this task.

## Kill records and traits

Pallatius the Severer is NPC 5001535. The Coilstone's interaction condition requires
HuntMonster play record 524, which was not indexed when no quest referenced it. The quest
export now indexes all supported monster kill records, including authored NPC groups. A kill
updates the persistent record and sends its notification; world entry sends saved records.
Kills made before the record was tracked cannot be inferred retroactively.

The Coilstone and other supported adventure objects export their authored play-record gates
and build-point rewards. Pallatius's Coilstone grants two weapon trait points once. Character
levels award separate stat points; the exported level data does not establish a trait point
on every level.

## Combat, gathering and the pouch

The authored Stun status now stops an NPC's route and attack until the status expires or is
removed early. This does not establish timing for other knockdown and hit reactions. Damage
notifications for the player interrupt an active shared-world gathering action without
granting the reward.

Utility-pouch edits are refused while an NPC is engaged with the player. PvP combat-state
coverage is not established. Re-equipping an existing item in a slot refills it from owned,
unreserved items, even when the stack is smaller than the requested capacity.

The reported hold-to-butcher and fast animation remain open. The exported Knife interaction
1000 has `UseInputHoldDown=False`, a 2,930 ms execute notify and a 3,833 ms montage duration.
The server already rejects an execute earlier than the authored deadline minus 100 ms.
Live logs record interaction opcodes and timing, but not the target or animation payload;
they cannot identify the reported corpse attempt or explain its input and presentation.
Static client inspection found distinct key and hold input modes selected from a target
descriptor; its source for corpse handling remains unknown. The investigation is recorded
under `../dumps/ida_research/corpse-interaction-20261101/`.

## Water interactions

Official captures contain swimming stamina flow start/stop notifications and a suspended
regeneration checkpoint. The server now announces that flow while retaining its own stamina
accounting. Small, medium and large authored air bubbles are exported with their existing
stamina-recovery effects and timed respawn. These paths still need original-client testing.

## Verification

- Release server and the affected test targets built.
- Combat suite: 262 passed, 19 fixture-dependent skips. Inventory: 69 passed, three skips.
  Quest: 55 passed, ten skips. Tutorial: 44 passed, two skips. Gathering: 59 passed, one skip
  with native-world cases excluded from that aggregate.
- The native TLS swimming test passed using the documented OpenSSL diagnostic setting.
  The real-profile Pallatius Coilstone test and focused air-bubble, interrupted-gathering,
  pouch, stun and play-record tests passed.
- Quest, adventure, gathering, combat and reference generator Python tests passed. These
  tests and official captures are not original-client acceptance of the new server behavior.

The generated protocol reference retains pending source-review warnings because unrelated
work in the checkout was already modified. The cloud Page refused an update because its
document or stored history exceeds the service size limit. The last verified publication
hash remains unchanged.

An independent review rejected the initial candidate and identified early stun removal,
swimming-flow cleanup, zero-stamina entry, NPC engagement detection, bubble recovery
capacity and monster-reward revision conflicts. Local follow-up changes and focused tests
address those paths. The reward path retries three definitive revision conflicts with the
same random state; persistent conflicts or an ambiguous database failure still have no
durable defeat receipt. A fresh review of any final release candidate remains necessary.
