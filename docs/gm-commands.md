# Game master (GM) commands

This is the in-game command reference for game master accounts on this emulator.
The research behind it is in `../dumps/ida_research/gm-20261026/FINDINGS.md`.

## 1. Make the account a game master

An operator runs this on the server host. Then the player signs out and signs in again.

```powershell
.\tools\manage_account.ps1 -Action Role -Account <account> -Role gm
```

The admin console can also create or change GM accounts. A GM account gets the
`[GM]` tag on the network-latency display, may enter a full or maintenance realm,
and is the only kind of account whose GM commands run.

## 2. Send a command in game

Use either method. Both send the same packet (`PC2S_GmCmdTextReq`).

- **Chat box (recommended):** type `/gm <command>` and press Enter, for example
  `/gm help`. The client sends any chat line that starts with `/gm ` to the server
  as a GM command.
- **GM console:** press **Tilde (`~`)** and type `<command>` without `/gm`.

Replies arrive as private chat lines from **`[GM]`**. The client ignores the debug
text packet, so chat is the only feedback channel. Values are case-insensitive.
Toggles accept `on`, `off` or nothing (nothing flips the current state).
Coordinates are meters; type `/gm pos` to see your own.

## 3. Movement and protection

| Command | Aliases | What it does |
| --- | --- | --- |
| `help [1-5]` | `commands` | Lists the commands in chat. |
| `pos` | `where`, `loc` | Shows your map, coordinates (meters) and facing. |
| `tp <x> <y> <z> [yaw]` | `teleport`, `set_pos_and_yaw`, `set_pos` | Teleports you within the current map; optional facing in degrees. Inside a housing area (map 20101) the coordinates are on the field it overlays (map 9012), so you leave the area. |
| `tp_map <map_id> [x y z]` | `map`, `warp` | Loads another map; keeps your x y z when omitted. |
| `up <m>` / `down <m>` / `fwd <m>` | `forward` | Moves you up, down or forward by meters (up to 5000), through walls and terrain. Inside a housing area you land on map 9012 like `tp`. |
| `goto <name>` | `tp_player`, `tpto` | Teleports you next to an online character. |
| `summon <name>` | `bring` | Teleports an online character next to you. |
| `players` | `who` | Lists online characters and their maps. |
| `god [on\|off]` | `invincible`, `invulnerable` | No damage from monsters, falls or drowning. |
| `invisible [on\|off]` | `invis`, `hide` | Other players stop seeing you; monsters stop noticing you. You still see everyone. |
| `stamina [on\|off]` | `infinite_stamina` | Sprinting and swimming cost no stamina. Turning it on also refills stamina. |
| `fly [on\|off]` | `noclip`, `freemove` | Free movement: god mode, infinite stamina and no fall damage at once. |
| `heal` | `restore` | Restores HP and stamina. |
| `revive` | | Revives you. The GM revive button also works now. |
| `suicide` | `kill_me` | Kills your character, even in god mode. The death is saved like any other. |
| `status` | `gm_status` | Shows your god/invisible/stamina/fly toggles. |

**About fly and no-clip.** The game client handles walls and gravity itself. It has
no local fly or no-clip command, and no server message to switch them off was found.
`fly on` therefore protects you while you explore. To pass through walls, floors or
mountains, use `up`, `down`, `fwd`, `tp`, `goto` and `tp_map`. To fall slowly from a
height, use the normal glider.

Useful map IDs from the client Map table: `8000` tutorial island, `9011` and `9012`
Shining Shore public channels (`9012` has this server's NPCs), `9013` Glass Harbor
(temporary), `9021` Golden Ruins, `7000`-`7003` Hersander maps, `1` the full
ArcheWorld. Development maps such as `1006`-`1052` are in the table too. Maps
other than `9012` load without NPCs.

## World-map reveal and exploration destinations

Use `/gm reveal_map` (alias `/gm unlock_map`) to save every verified map fog area
for your own character. The matching export has 1,063 areas. Return to character
selection and select the character again to reload the saved map. Repeating the
command is safe, and delayed ordinary discoveries cannot remove the reveal.
This does not complete Adventure Progress, quests or rewards, or change zone access
and collision. See [map exploration](map-exploration.md) for persistence and client
refresh limits, and the generated exploration guide for named teleport destinations.

## 4. Items, money and character

| Command | Aliases | What it does |
| --- | --- | --- |
| `item_add <item_id> [count]` | `itemadd`, `additem` | Gives you items (count up to the item's stack limit). |
| `add_money <gold>` | `money`, `gold` | Adds gold; a negative value removes it. |
| `set_weapon_durability <value>` | `weapon_durability`, `durability` | Sets the durability of your equipped weapons (capped at each weapon's maximum). |
| `set_level <level>` | `level` | Sets your level (up to the level cap). |
| `exp_up <exp>` | `exp` | Adds experience. |
| `add_char_build_point <levelup\|weapon> <n>` | | Adds build points. |
| `set_char_build_point <levelup\|weapon> <n>` | | Sets build points. |
| `reset_stat_bonus` | | Refunds allocated stat points. |
| `force_reset_perk` | | Resets perks. |
| `spawn_npc <npc_id> [count 1-10]` | `spawn`, `spawn_monster`, `monster` | Spawns NPCs or monsters around you. |
| `despawn_npcs` | `clear_spawns` | Removes the NPCs you spawned. |
| `complete_quests` | `finish_quests`, `completable_quest_all` | Completes every quest in progress, with its normal rewards and EXP. |
| `complete_quests chain` | | Repeats until no quest is in progress, so story chains (successor quests) finish too. |
| `complete_quest <quest_id>` | `finish_quest`, `reward_quest` | Completes one quest, accepting it first if you do not have it. |
| `emotion_add <emotion_id>` | `add_emotion`, `emote_add` | Grants a non-Basic emote and saves it. Basic emotes are always available; the current client data has only Basic emotes, so this command refuses every id until such a row exists. |

Quest completion works on shared maps. The tutorial island runs its own story and
cutscenes, so leave it first (for example `/gm tp_map 9012`). A quest whose reward
the server cannot verify is marked complete without that reward. A quest whose reward
cannot be granted (for example a full item stack) stays in progress.

Spawned NPCs are only shown to you, and you can have up to 50 at once. A supported monster
with combat stats, an executable attack and complete catalog locomotion can be fought in
both the tutorial and an open-world map. It pursues within its range rules, faces its route,
stops to attack and returns home after losing its target. NPCs without the required definitions
remain visual-only and the command reports that limit. These are private GM test encounters;
they do not add public shared-world monsters or grant authored tutorial quest kills.
See [NPC locomotion validation](npc-locomotion-fix-20261026.md).

## 5. Vehicles, mounts, mates and housing

These commands work on shared maps (not in the tutorial instance).

| Command | What it does |
| --- | --- |
| `spawn_vehicle <vehicle_id>` (`spawn_operable`, `vehicle`) | Spawns a free vehicle where you stand. |
| `car` | Spawns the catalog's one vehicle (a trade wagon). Every player may use it, not only game masters; one per five seconds. |
| `damage_vehicle <amount>` | Damages your vehicle. |
| `register_summon_npc <id>`, `spawn_summon_npc <id>`, `despawn_summon_npc` | Registers, summons or dismisses a mount. |
| `register_mate_npc <id>`, `spawn_mate_npc`, `despawn_mate_npc`, `invite_mate_npc`, `add_mate_intimate <id> <value>` | House mate commands. |
| `trade_voucher_add <id> [valid_minutes]` | Adds one trade voucher; optional validity in minutes (not a count). |
| `housing_contract <small\|medium\|large> [monthly\|periodic]` | Rents the first free personal lot of that size in the housing zone and pays its housing permits (item `123`). Use it when the client's contract window is not reachable. |
| `teleport_to_home` | Goes to your house. |
| `housing_unlock <small\|medium\|large\|guild> [revoke]`, `release_housing [lot]`, `housing_remained_expiry_minutes <min> [monthly\|periodic\|close]`, `housing_account_reset` | Housing administration. |

Every housing command answers in chat: `applied`, the rented lot and channel, or the
refusal code with the next step (for example "You have no lot; rent one with
housing_contract small"). A quick housing test:

```
/gm item_add 123 1000
/gm housing_unlock medium
/gm housing_contract small
/gm teleport_to_home
```

A small lot costs 60 permits, medium 240 and large 480. The first medium or large contract
also costs Hersander Tokens unless `housing_unlock` granted that size. `housing_contract` is
an emulator command; the official GM table has no personal contract command.

## 6. Diagnostics and server

Type `/gm help 4` in game for this list. Diagnostics go to your own client only.

| Command | Aliases | What it does |
| --- | --- | --- |
| `game_time <hour> [day_seconds]` | `time` | Starts your sky clock at `hour` (0-24). One game day lasts `day_seconds` real seconds (60-604800, default 3600). |
| `game_time stop <hour> [day_seconds]` | | Freezes your sky clock at `hour`. |
| `regen_stat` | `regen_stats`, `debug_player_regen_stat` | Sends your HP/stamina regeneration stats to the client debug panel. |
| `watch <unit_id\|me\|spawned> [position\|path\|ai\|all]` | `debug_watch` | Streams debug overlays for a unit: its server position, its current route, or its AI state. `spawned` watches the NPCs you spawned. Up to 8 overlays; they refresh twice a second. |
| `unwatch [unit_id]` | | Stops one unit's overlays, or all of them. |
| `channel_event_info` | `channel_event` | Shows the current map's recovered event ID and local completion status. Objectives and saved event progress are not modeled. |
| `announce <text>` | `notice` | Sends a World chat notice from `[GM]` to every player (1-200 characters). |
| `kick [seconds]` | `test_kick` | Disconnects you after 0-60 seconds. |

The client's own debug windows also work: its unit watch sends the same watch request, and
its `X5ChannelEvent` console category asks for the channel-event panel. `debug_npc_field` is
answered with a refusal because the server has no NPC navigation data.

**About the overlays.** The client has real handlers for these debug packets, but whether the
retail client draws them depends on a debug component that may not exist in shipping builds.
The sky clock (`game_time`) goes to the normal world time system. None of this has been tried
on the original client yet. The research is in
`../dumps/ida_research/gm-diagnostics-20261026/FINDINGS.md`.

### Rankings, raids and channel events

The following are bounded testing/administration commands, not a complete official event system.
See [feature details and limits](rankings-raids-channel-events.md).

- `ranking_start <ranking_id>` starts a server-timed attempt.
- `ranking_finish <ranking_id>` records its elapsed milliseconds and returns the standing.
- `ranking_cancel` drops the current attempt.
- `raid_channel_enter <map_id>` forms a one-member match (10103, 20229 or 10301) if that map is
  free. The normal raid move request performs entry; no bosses are spawned.
- `complete_channel_event_objective` sets the current map status to Success. It neither executes
  an unrecovered objective nor grants a reward.
- `reset_channel_event_save_data` clears the current map's in-memory status and tribute count.

The existing `channel_event_info` command reads the current board state through the diagnostic
panel. Ranking record timing is an operator test path; automatic qualified boss records and
reward settlement remain missing.

## 7. All 126 recovered structured commands

The complete numbered list is in [GM command catalog](gm-command-catalog.md). It gives the
emulator text syntax, structured-field restrictions, and explicit unavailable/client-local
status for **every one of the 126 recovered fields**. The full set is still incomplete;
recognizing a field or returning a refusal does not implement its game system.

### Combat, quests and mail

Use `/gm help 5` for the additional commands. Unit edits apply only to your character (`me`)
or your private combat-enabled `spawn_npc` actors (`spawned` or their unit ID), never another
player or the shared NPC population.

- `monster_kill <unit|spawned>` and `damage_monster <unit|spawned> <amount|percent%>` use normal
  defeat handling. Percent means current HP, rounded upward, not maximum HP.
- `add_status_effect <unit|me|spawned> <id>`, `remove_status_effect <unit|me|spawned> <id>` and
  `gauge <unit|me|spawned> <type> <amount>` use loaded timed statuses and ailment rules.
- `npc_ai_off <unit|spawned>` pauses private NPC AI and cancels its action/route;
  `npc_ai_on <unit|spawned>` resumes it. `cancel_skill [unit|me|spawned]` ends an action.
- `move_unit <unit|me|spawned> <x> <y> <z> [rel]` places a private NPC or teleports yourself.
  Coordinates are meters; `rel` adds them to each target's current position.
- `cooldown` clears server cooldowns. The client's cooldown display can remain until expiry.
- `set_gm_stat <stat> <value>`, `add_gm_stat <stat> <delta|percent%>` and `set_gm_stat clear`
  control temporary combat stats. Values must remain nonnegative and at most 100,000,000;
  HP changes cannot cause death or revive a dead character. Equipment/build recalculation resets
  these overrides. Use `suicide` and `revive` for life-state transitions.
- `execute_skill_effect <id>` runs only supported, unconditional self-effect chains, at most
  eight linked effects. Damage, projectiles, consumption and other targets are refused.
- `accept_quest <id>` bypasses acceptance conditions; `accept_quest_if_passed_cond <id>` retains
  catalog conditions. Both require a shared map, save normal supplies, and do not restart saved
  quests. Abandoning/removing quests remains unavailable.
- `add_stat_bonus <con|dex|end|int|str|tal> <n>`, `set_stat_bonus <stat> <n>` and
  `learn_all_perks` are free GM grants. Attribute bonuses are capped at 1000; perks retain their
  prerequisites and one choice per exclusive group.
- `itembag_clear` (`clear_bag`) removes unequipped instances and unreserved stack units.
  Equipment and utility-slot reservations are preserved. This is destructive.
- `mail_send <online-name|me> [item <id> [count] | gold <amount>]` delivers persistent mail,
  with one attachment kind and a 30-day lifetime. It requires the mail store and item catalog.
  Every command creates a new delivery; inspect the mailbox before retrying an uncertain result.

## Limits

- **Testing:** synthetic tests cover the stated contracts. The newly added diagnostic,
  combat, quest and mail commands have no original-client acceptance trial in this work;
  any separate housing/vehicle observations belong to their owning validation records.
- **Rate limit:** at most four GM requests per second are accepted.
- **Invisible:** this hides you from players and monsters, but your own mount or
  vehicle is still announced as usual.
- **Summoning:** players in a private tutorial instance cannot be summoned or
  visited.
- **Command names:** the official GM command table is missing from the exported
  client data, so this server defines its own names.
