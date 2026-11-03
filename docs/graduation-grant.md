# Graduation test kit

While the realm is being tested, a character that enters the overworld below the level cap gets a
test kit so it can try every system at once. This is emulator policy, not official behavior.

## What a character receives

Applied once, at the start of the first world entry below the level cap, before the entry
snapshots are built. The client therefore receives the finished state like any other login.

| Part | Detail |
| --- | --- |
| Quests | Every quest of the shared catalog (1,571) completed through the quest book's game-master completion, with sequential successors followed. Rewards and EXP are granted by the same rules as `/gm complete_quests`. |
| Equipment | For the character's class: the highest tier and grade piece(s) of every equipment slot (armor, weapon, accessories, bag, glider, tools, costumes), plus class-neutral pieces. At most three look-alike pieces per slot (38-39 pieces). Pieces that require an unreachable level are left out. The item `level` is a tier (up to 80) and may exceed the character level cap (40). |
| Consumables | 200 health potions, 100 stamina potions and 20 repairers (a stock is raised to that amount, never lowered). |
| Gold | Raised to the catalog cap (`Money.limitUserMax`), never lowered. |
| Level | The level cap (40). Both build point balances (level-up and weapon points) are filled to their 100,000 maximum. Skills are not learned automatically; the player spends the points. |

The kit stays within the carried-weight limit: the real-catalog test measures every class against
the encumbrance rules. Quests 31 and 151 reward ten long bows each (item 4, weight 8,800); the quest
book books them as stackables, so the grant drops such misfiled equipment entries instead of
leaving 176,000 weight on the character.

## Who receives it, and when

* Any character that enters the world outside the tutorial with a level below the cap.
* Game master accounts are never granted, so they keep what they set themselves.
* The level is raised last and is the "already granted" marker. A character at the cap is never
  granted again; a character whose earlier steps failed is retried at its next entry.
* A character that finishes the tutorial in its current session is told in private chat to return
  to character selection and enter again. The session cannot show the kit, because the grant runs
  before the entry snapshots.

## Switching it off

The server grants the kit by default. Start `chronicles-server` with `--graduation-grant off`, or
set `graduation_grant` to `"off"` in the server configuration file, to stop granting it. Characters
that already received the kit keep it.

## Player car command

Every player (not only game masters) can type `/gm car` in the chat box to spawn the catalog's one
vehicle (Vehicle 200, a trade wagon) where they stand. The command takes no argument and is limited
to one car every five seconds per session. Every other vehicle, mount and mate command stays
game-master only (see [GM commands](gm-commands.md)).

## Storage limit

The stored quest record limit rose from 1,024 to 4,096, equal to the quest book's own bound, so a
character with all 1,571 quests can be saved.

## Verification

`src/world/session/tests/graduation_grant_tests.cpp` covers the loadout selection, the bundle planner (idempotence,
inventory slots, bounds, gold and stock caps) and, with the exported client catalog and quest
catalog, four classes end to end: level, build points, gold, 1,571 completed quests, equipment,
no encumbrance, no second grant, and no grant for a game master. These are synthetic checks.
The original client has not been observed showing a 1,571-entry quest list, the level-cap
character or the tier-80 gear, and equipability of the gear by the client is unverified.
