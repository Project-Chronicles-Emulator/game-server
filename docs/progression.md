# Character progression: EXP, levels, build points, attributes and perks

`src/world/features/character/progression.hpp` owns server-authoritative character growth for client build
`40367e24241c9481bfc07fbf9ef1919f25fb0eaa` (executable dump SHA-256
`e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6`). It loads
`CharacterLevel`, `PerkNode`, `PerkTheme`, `Npc` and `NpcTemplate` once at startup. Every
operation edits a `WorldProgress` copy that the session commits as one revisioned transition;
no client value is trusted as an EXP amount, level or point balance.

## EXP and levels

`exp` on the wire is the **cumulative** total. `CharacterLevel.nextLevelExp` is the total at
which the next level is reached (500, 2000, 4000, 6500, ...). Evidence: the official tutorial
capture reports `exp=130` at level 1, then a 500 EXP quest reward reports `exp=630, level=2`;
every captured level-up (levels 2 to 8, two classes) lies between consecutive thresholds. The
cap row (level 40) is a sentinel that is never reached; EXP stops one below it.

| Source | Amount | Evidence |
| --- | --- | --- |
| Quest reward | `QuestReward` Exp rows, scaled by the operator rate | Observed |
| Adventure object | profile `experience`, scaled by the operator rate | Observed (tutorial profile) |
| Monster kill | `round_half_up((5 + npc_level / 4) * grade)` | Candidate fit, see below |
| GM `exp_up` / `set_level` | command value, not rate-scaled | Emulator tooling |

Kill EXP has no client table. The fit reproduces every captured kill (player levels 1-8, NPC
levels 2-8): grade multipliers Weak 0.4, Normal 1, Strong 1.6, Elite 16, Boss_B 60. Unobserved
Boss_A/Boss_C/Boss_S reuse 60 as emulator policy. Kill EXP is only awarded when combat
establishes the death (`TutorialSession::record_monster_defeat`).

Each reached level adds `CharacterLevel.charBuildPoint` (3) LevelUpPoints. Unspent points carry
over (Observed: 6 after skipping one allocation).

## Packets

| Packet | Shape | Evidence |
| --- | --- | --- |
| `ChangeCharacterBaseNoti` (36) | `exp` for a gain; plus `level` and `{1: LevelUpPoints}` on a level change; only `{2: WeaponPoints}` for a WeaponPoint grant | Observed |
| `ChangeCharacterPublicNoti` (37) | `unit_id`, `level` to observers | Observed for other players |
| `UpdateStatBonusReq` (20060) | `stats[{stat_type, value}]`, value is a **delta** | Observed (Int 30 -> 33 -> 36) |
| `ChangeStatBonusNoti` (120) | `{1: remaining LevelUpPoints}`, then 78 and 47 (`SCS_Equip`) | Observed |
| `ChangePerkReq` (20053) / `ChangePerkRes` (99) | echoed `packet_index`, both balances | Observed |
| `UnitPerkNoti` (101) | changed nodes to observers | Observed for other players only |
| `ResetPerkReq` (20054) / `ResetPerkNoti` (100) | theme reset, both balances | Schema only; Candidate |
| `WorldCharPerkNoti` (135) | auto-registered plus earned nodes and both balances | Observed shape |

A level-up refills a living player's HP/SP and sends 78 plus a 47 with the full vitals
(Observed in the tutorial level-up). Stat refusals answer `CommonErrorRes` with a `StatBonus_*`
code (Candidate; `ChangeStatBonusNoti` has no error field). Perk refusals answer
`ChangePerkRes.ec` with the recovered `PerkNode_*` code and the unchanged balances.

## Attributes and perks

LevelUpPoints raise Con, Dex, End, Int, Str or Tal (`StatBonus_NoConditionalStatType`
otherwise). `CharacterStats` adds the allocated bonuses to the class base attributes before
every derived formula, so power, HP, SP, armor, critical, penetration and elemental ratings all
follow. Observed: Wild Str 15 -> 18 raises physical power by 18 (`6 * Str + Dex`).

Perks (traits) cost `PerkNode.cost` WeaponPoints of the node's own weapon theme tree. Rules:

- The node belongs to the character's class (`PerkNode_InvalidClass`) and has `costType`
  WeaponPoint; auto-registered nodes cannot be changed.
- Level 0 deactivates a node and refunds its cost (Candidate; the refusal names are recovered).
- `FK_unlockNodeIds` is any-of (Candidate): multi-entry lists name members of one exclusive
  group. The whole resulting tree is validated, so deactivating a prerequisite that another
  active node needs is refused with `PerkNode_Locked`.
- At most one node per `groupId` (`PerkNode_ExclusiveActivation`).

Earned `Stat` perks (22 nodes, e.g. LongBow 1230605 TidalAttackPower +20) add their flat
`statAdd` to the character stats (Candidate; no captured frame isolates one). Skill-open,
theme-ability and skill-cost perk effects are stored and reported to the client, but combat does
not yet apply them server-side.

WeaponPoints come from `ExpertiseReward` CharBuildPoint rows on 145 open-world Raven Maumet
adventure objects (Observed: +2/+3 per activation) and from three `QuestReward` CharBuildPoint
rows. The tutorial profile generator now carries quest CharBuildPoint rewards
(`build_points`); neither tutorial map contains one, and shared-world adventure objects are not
yet served by the emulator.

## Persistence

Migration version 20 adds `level_up_points`, `weapon_points` and the six `*_bonus` columns
(mirroring `x5.pb.DbCharacter`) and `emulator_character_perks`. Level, cumulative EXP, points,
bonuses and perks commit with the same character revision as quests and inventory. Perk rows are
tombstoned with level 0, never deleted. Characters saved before this change keep level 1 with
EXP past later thresholds; their levels and LevelUpPoints are reconciled once at world entry.

## GM commands

The Tilde console (game-master accounts only) accepts `exp_up <n>`, `set_level <level>`,
`add_char_build_point <1|levelup|2|weapon> <n>`, `set_char_build_point <type> <n>`,
`reset_stat_bonus` and `force_reset_perk`, and the matching structured `GmCmd` fields. This is
an emulator grammar named after the recovered GmCmd fields, not the official text grammar.

## Verification and limits

```powershell
cmake --build --preset release --target chronicles-tests
ctest --preset release -R 'Progression|Quest|Tutorial|CharacterStats'
```

Eighteen focused `Progression*` tests cover cumulative thresholds and the captured exp=630
frame, multi-level awards, the cap, reconciliation, GM level changes, every captured kill grade,
attribute allocation and refusals, perk prerequisites/costs/exclusive groups and every
`PerkNode_*` refusal, theme reset, snapshots, request parsing, schema encoding, memory and
PostgreSQL persistence, a tutorial kill-plus-quest level-up, and real-catalog checks (38 Wild and
42 Magic auto-registered nodes; the captured LongBow purchase sequence; Str and Stat perk
effects). On 2026-10-26 these passed with the full tutorial, quest, stats, GM and combat suites;
the PostgreSQL test passed on a disposable cluster after applying the migration twice.

These are synthetic checks. Original-client acceptance of level-up, attribute and perk replies,
the in-game skill unlocks granted by perks, kill EXP for unobserved grades, perk deactivation
refunds and theme reset behavior remain unverified. Shared-world combat, shared-world quests and
Raven Maumet objects are not served by the emulator yet, so outside the tutorial only build
requests and GM commands change progression.
