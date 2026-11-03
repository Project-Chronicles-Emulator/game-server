# All recovered GM command types and emulator usage

This lists **all 126 fields** of the recovered `GmCmd.cmd` oneof for client build
`40367e24241c9481bfc07fbf9ef1919f25fb0eaa`. A field is a structured protocol operation,
not necessarily an official console spelling. The official `GMCommand.csv` and aliases were
not exported. The emulator therefore defines its own text grammar.

## How to run them

1. Give the account the `gm` role and sign out/in again (see [GM setup](gm-commands.md)).
2. In the game chat input, enter **`/gm help`** and press Enter. For these commands the client
   intercepts `/gm ` and sends a GM request, rather than sending ordinary public chat.
3. For example, enter `/gm pos`, `/gm god on`, or `/gm help 5`. The server replies privately
   as `[GM]`. A normal player typing the same commands is refused.
4. Alternatively press **Tilde (`~`)** to open the GM console and enter `pos`, `god on`, etc.,
   without `/gm`. The chat and console paths both send `PC2S_GmCmdTextReq` (20009).

The syntax shown below is the text after `/gm `. Placeholders in angle brackets must be
replaced; square brackets mean optional arguments. Coordinates are meters; yaw is degrees.
IDs name catalog rows unless explicitly called unit, character, guild or lot IDs.

The client's GM resurrection button and debug windows also send `PC2S_GmCmdReq` (20008),
whose `cmds` entries select the fields listed below. Other structured senders have not been
established in this client. Tools can encode those requests, but typing JSON into game chat
will not send a structured packet. Mixed structured batches are not supported across every
command module; send one command per request.

**Supported** means a bounded emulator behavior, not verified official-server parity.
**Limited** describes a narrower or emulator-defined mapping. **Unavailable** means there is
no behavior-producing implementation; a refusal is not counted as support. The in-game help
and [main guide](gm-commands.md) include aliases and subsystem prerequisites.

## Fields 1–25

- **1 `debug_watch` — Limited.** `watch <unit|me|spawned> [position|path|ai|all]` and
  `unwatch [unit]`. Structured `{unit_id,type,add}` supports Position, Path and AI only.
  Eight overlays maximum, refreshed at 500 ms. Overlay rendering in the original client is
  unverified; AbnormalStatus and SkillCollision subscriptions are unavailable.
- **2 `debug_ghost` — Limited.** `debug_ghost [on|off]` (also `ghost`, `fly`). Enables server
  protection/infinite stamina, not client collision removal or actual free flight.
- **3 `debug_npc_field` — Unavailable.** No server navigation-field data. The command reports why.
- **4 `debug_player_regen_stat` — Limited.** `regen_stat` sends authoritative regen/vital stats
  to the debug panel; rendering is unverified.
- **5 `change_class` — Unavailable.** Durable class changes, class skills and weapon migration
  are not implemented. Create another character to select a different class.
- **6 `item_add` — Supported.** `item_add <item_id> [count]`; catalog and inventory limits apply.
- **7 `itembag_clear` — Limited, destructive.** `itembag_clear` or `clear_bag` removes unequipped
  instances and stack units not reserved by utility slots. Equipment, housing and vouchers
  remain. Structured `equip=true` is refused.
- **8 `map_move` — Limited.** `map_move <map_id> [x y z]` (also `tp_map`). Structured requests
  support map ID and explicit `pos` only; spawn/NPC/portal/sector selectors are refused.
- **9 `monster_kill` — Limited.** `monster_kill <unit|spawned>` (also `kill`) kills your private
  combat-enabled `spawn_npc` actors through normal defeat handling. Shared population is not edited.
- **10 `spawn_npc` — Limited.** `spawn_npc <npc_id> [count 1-10]`; fifty private actors maximum.
  Only profiles with complete combat/locomotion can fight; others are visual-only.
- **11 `spawn_projectile` — Unavailable.** Standalone projectile spawning is not implemented.
- **12 `mail_send` — Limited.** `mail_send <online-name|me> [item <item_id> [count] | gold <amount>]`.
  Durable mail plus a live notification; needs the mail store and item catalog. Structured
  `char_id` must resolve to an online character, and `account_id`, when supplied, must own it.
  One attachment kind per command. Offline/account-wide delivery uses the operator API instead.
  Each command creates a new delivery: inspect the mailbox before retrying an uncertain result.
- **13 `accept_quest` — Limited.** `accept_quest <quest_id>` on a shared map; bypasses NPC/range
  and acceptance conditions, grants normal supported supplies, and commits once. Existing saved
  quests are not restarted. Tutorial story control remains separate.
- **14 `abandon_quest` — Unavailable.** Quest persistence currently forbids removing/resetting
  records and decreasing objective counters.
- **15 `completable_quest` — Limited.** Text alias: `complete_quest <quest_id>`. This emulator
  completes objectives **and turns in the quest**, rather than only marking it ready.
- **16 `completable_quest_all` — Limited.** `complete_quests [chain]` turns in active quests;
  `chain` follows accepted successors for at most fifty rounds.
- **17 `reward_quest` — Limited.** `reward_quest <quest_id>` is the same complete-and-turn-in path.
- **18 `remove_quest` — Unavailable.** Requires an explicit durable quest-reset/removal contract.
- **19 `complete_quest_progress` — Limited.** Structured `{cms_id}` maps to complete-and-turn-in.
  Use the text form `complete_quest <quest_id>`; there is no separate text alias for this field.
- **20 `set_level` — Supported.** `set_level <level>`; bounded by the catalog level cap.
- **21 `interact_doodad` — Unavailable as a GM operation.** Normal tutorial/housing interactions
  remain owned by their respective services.
- **22 `add_money` — Limited.** `add_money <gold_delta>` adds/removes Gold within its catalog cap;
  other currency types are not supported by this GM command.
- **23 `set_gm_stat` — Limited.** `set_gm_stat <stat> <value>` or `set_gm_stat clear` changes a
  temporary combat stat, or restores equipment/build stats. Use enum numbers or names such as
  `str`, `maxhp`. Values are bounded; HP cannot bypass death/revival. Recalculation resets overrides.
  Structured assignment requires `enable=true`; `clear=true` restores the normal stats.
- **24 `add_gm_stat` — Limited.** `add_gm_stat <stat> <delta|percent%>`. Structured `percent` is a
  ratio (`0.25` means 25%), including fractional percentages, and cannot be combined with a
  nonzero `value`; results must remain in the supported nonnegative range.
- **25 `suicide` — Supported.** `suicide` kills your own character, including in god mode, using
  the normal durable death path. Use `revive` afterward.

## Fields 26–50

- **26 `exp_up` — Supported.** `exp_up <positive_exp>` awards unscaled GM experience.
- **27 `add_status_effect` — Limited.** `add_status_effect <unit|me|spawned> <status_id>` (also
  `buff`). Only loaded timed statuses, a single stack, and the GM's own/private actors are supported.
- **28 `remove_status_effect` — Limited.** `remove_status_effect <unit|me|spawned> <status_id>`
  removes that active status and reverses its modifiers.
- **29 `invincible` — Supported.** `god [on|off]` (also `invincible`); protects against NPC,
  fall and drowning damage.
- **30 `invisible` — Limited.** `invisible [on|off]`; hides the player from peers/NPC perception,
  not necessarily that player's mount or vehicle.
- **31 `regen` — Limited.** Structured form maps to one-shot `heal`, restoring living HP/stamina;
  there is no continuous regeneration toggle. Use `heal` as text.
- **32 `move_unit` — Limited.** `move_unit <unit|me|spawned> <x> <y> <z> [rel]` places private
  combat NPCs or teleports yourself. `rel` adds coordinates to each target's position. Structured
  float positions are interpreted as meters (Candidate); speed/trading-post variants are refused.
- **33 `fly_mode` — Limited.** `fly [on|off]` provides server protection/infinite stamina.
  Use `up`, `down`, `fwd` or `tp` to move through obstacles; it does not disable client gravity.
- **34 `change_doodad_phase` — Unavailable as a GM operation.** Existing phase state is owned
  by tutorial/housing behavior.
- **35 `cancel_skill` — Limited.** `cancel_skill [unit|me|spawned]` ends a running action of your
  own character or a private GM monster.
- **36 `spawn_summon_npc` — Supported.** `spawn_summon_npc <summon_id>` on a shared map.
- **37 `despawn_summon_npc` — Supported.** `despawn_summon_npc` dismisses your summon.
- **38 `register_summon_npc` — Supported.** `register_summon_npc <summon_id>`.
- **39 `activate_gathering` — Unavailable.** GM gathering respawn is not implemented; normal
  points respawn using their catalog timers.
- **40 `set_world_level` — Unavailable.** World-level progression is not implemented.
- **41 `spawn_skillobject` — Unavailable.** Standalone skill-object spawning is not implemented.
- **42 `npc_ai_on` — Limited.** `npc_ai_on <unit|spawned>` resumes private GM combat NPC AI.
- **43 `npc_ai_off` — Limited.** `npc_ai_off <unit|spawned>` pauses private GM combat NPC AI,
  cancelling its current action/route. It remains damageable.
- **44 `npc_use_skill` — Unavailable.** Forced arbitrary NPC casts are not implemented.
- **45 `test_kick` — Supported.** `kick [seconds 0-60]` disconnects your own session normally.
- **46 `remove_quest_job_board` — Unavailable.** Quest job boards are not implemented.
- **47 `combat_formula_log` — Unavailable as a toggle.** Reports that existing combat logs are
  available; this response is not a formula-logging implementation.
- **48 `reset_stat_bonus` — Supported.** `reset_stat_bonus` refunds allocated attribute points.
- **49 `cooldown` — Limited.** `cooldown` or `reset_cooldowns` clears server skill deadlines.
  The client cooldown display is not reset by an invented notification.
- **50 `set_reaction` — Unavailable.** Forced hit-reaction control is not implemented.

## Fields 51–75

- **51 `add_abnormal_status_gauge` — Limited.** `gauge <unit|me|spawned> <type> <amount>` or the
  full field name. Types: `burning`, `freezing`, `electric_shock`/`shock`, `bleeding`,
  `poisoning`/`poison`, `stun`, `shrink`. Loaded gauge rules bound the value; a filled gauge applies
  its normal ailment status. Structured form accepts several named gauges in one request.
- **52 `set_class_action_form_gauge` — Unavailable.** Class action forms are not simulated.
- **53 `set_faction` — Unavailable.** Runtime faction changes are not implemented.
- **54 `set_adventure_object_state` — Unavailable as a GM operation.** Normal interactions
  retain ownership of the persistent adventure-object state.
- **55 `revive` — Supported.** `revive`, or the game's GM resurrection button.
- **56 `release_housing` — Supported.** `release_housing [lot]`; requires an applicable contract.
- **57 `company` — Unavailable.** Company progression/state is not modeled.
- **58 `game_time` — Limited.** `game_time [stop] <hour 0-24> [day_seconds 60-604800]`; affects
  only your client, defaults to a 3600-second day, and is not persisted.
- **59 `announce` — Limited.** `announce <text>` sends a World-chat notice. Structured display
  duration is not modeled; the text path is limited to 200 bytes.
- **60 `spawn_operable` — Supported.** `spawn_vehicle <vehicle_id>` (also `spawn_operable`).
- **61 `ignore_reaction` — Unavailable.** No server reaction override.
- **62 `npc_root_motion_in_place` — Unavailable.** Root-motion debugging is not implemented.
- **63 `set_pos_and_yaw` — Limited.** `tp <x> <y> <z> [yaw]` or
  `set_pos_and_yaw <x> <y> <z> [yaw]` moves yourself. Structured form accepts the self/default
  unit ID and explicit position; other-unit placement uses `move_unit` instead.
- **64 `damage_vehicle` — Supported.** `damage_vehicle <amount>` changes your vehicle's health.
- **65 `damage_destructible_object` — Unavailable.** Destructible objects are not implemented.
- **66 `damage_monster` — Limited.** `damage_monster <unit|spawned> <amount|percent%>` (also
  `damage`); percent means **current HP**, rounded upward, not maximum HP. Private GM monsters only.
- **67 `add_mate_intimate` — Supported.** `add_mate_intimate <mate_id> <value>`.
- **68 `spawn_destructible_object` — Unavailable.** Destructible objects are not implemented.
- **69 `spawn_mate_npc` — Supported.** `spawn_mate_npc` summons your mate on a shared map.
- **70 `despawn_mate_npc` — Supported.** `despawn_mate_npc` dismisses your mate.
- **71 `register_mate_npc` — Supported.** `register_mate_npc <mate_id>`.
- **72 `invite_mate_npc` — Supported.** `invite_mate_npc` invites your mate.
- **73 `toggle_circular_flight` — Unavailable.** Circular-flight control is not implemented.
- **74 `teleport_to_home` — Supported.** `teleport_to_home`; requires your house contract.
- **75 `npc_server_preprocessing_skilleffect` — Unavailable.** Forward simulation of arbitrary
  NPC skill effects is not implemented.

## Fields 76–100

- **76 `mate_npc_follow_owner` — Unavailable.** Mate-follow behavior is not implemented.
- **77 `draw_mesh` — Unavailable.** The server has no navigation/collision mesh to send.
- **78 `raycast` — Client-local.** Registered locally by this client, not run by the emulator.
  Its argument grammar/usable shipping UI have not been established; no server syntax is claimed.
- **79 `add_char_build_point` — Supported.** `add_char_build_point <levelup|weapon> <delta>`.
- **80 `set_char_build_point` — Supported.** `set_char_build_point <levelup|weapon> <value>`.
- **81 `force_reset_perk` — Supported.** `force_reset_perk` deactivates/refunds earned perks.
- **82 `despawn_npc_all` — Limited.** Structured form removes only your private spawned actors;
  text is `despawn_npcs`, not a global population clear.
- **83 `set_npc_spawner` — Unavailable.** Runtime spawner edits are not implemented.
- **84 `debug_projetile_spawnable_area` — Unavailable.** The recovered name contains this
  spelling; no projectile-volume data service exists.
- **85 `set_nui_blessing_level` — Unavailable as a GM operation.** Direct GM blessing-level
  assignment is not implemented. Ordinary indexed-shrine offerings have a separate bounded
  tear-spending, persistence and stat-reward path; their original-client acceptance is unverified.
- **86 `trade_voucher_add` — Supported with subsystem limits.**
  `trade_voucher_add <id> [valid_minutes]` grants one voucher on a shared map; the optional
  argument is a validity duration, not a count. Omitted/zero uses the catalog duration.
  Structured `valid_period` is also minutes.
- **87 `add_stat_bonus` — Limited GM grant.** `add_stat_bonus <con|dex|end|int|str|tal> <n>`;
  grants without spending points, resulting bonus at most 1000.
- **88 `set_stat_bonus` — Limited GM grant.** `set_stat_bonus <stat> <n 0-1000>` without spending
  points. A normal reset refunds the resulting allocations under the existing GM policy.
- **89 `lv_up_expertise` — Unavailable.** Expertise progression is not implemented.
- **90 `reset_expertise_lv` — Unavailable.** Expertise progression is not implemented.
- **91 `achievement_update` — Unavailable.** Achievements are not implemented.
- **92 `execute_skill_effect` — Limited.** `execute_skill_effect <effect_id>` runs a loaded,
  bounded self-effect chain containing supported healing/status/recovery/gauge operations only.
  Arbitrary damage, projectiles, consumption, conditional effects and non-self targets are refused.
- **93 `contents_unlock` — Unavailable.** Content-unlock flags are not implemented.
- **94 `check_static_npc` — Unavailable.** No static-NPC sector-check GM operation.
- **95 `party_channel_invite` — Unavailable.** Party-channel management is not implemented.
- **96 `housing_remained_expiry_minutes` — Supported.**
  `housing_remained_expiry_minutes <minutes> [monthly|periodic|close]`.
- **97 `add_bluesalt` — Unavailable.** Premium currency is not implemented.
- **98 `channel_event_info` — Limited.** `channel_event_info` shows the local map-event board;
  unrecovered objectives and durable event progress are not modeled.
- **99 `complete_channel_event_objective` — Limited.** Sets the current map's local status to
  Success; it does not execute unrecovered objectives or grant official event rewards.
- **100 `reset_channel_event_save_data` — Limited.** Clears the map's in-memory status/tribute
  ledger; this is not a durable official-event reset service.

## Fields 101–126

- **101 `guild_exp` — Unavailable.** No GM guild-experience operation.
- **102 `guild_level` — Unavailable.** No GM guild-level operation.
- **103 `contract_housing_guild` — Supported by the housing service.** Structured `{lot_number}`;
  text `contract_housing_guild <lot_number>`.
- **104 `release_housing_guild` — Supported by the housing service.** Structured `{lot_number}`;
  text `release_housing_guild <lot_number>`.
- **105 `content_reset_counter` — Unavailable.** Content-reset counters are not implemented.
- **106 `learn_all_perks` — Limited GM grant.** `learn_all_perks` grants maximum compatible
  class nodes without spending WeaponPoints. Exclusive groups preserve one choice rather than
  activating contradictory nodes; previously active choices are retained.
- **107 `toggle_graze` — Client-local.** Local console spelling is `togglegraze`; no emulator
  command is claimed. Usable shipping behavior is unverified.
- **108 `guild_master_authorization` — Unavailable.** No GM guild-master transfer operation.
- **109 `guild_reset_cooldown` — Unavailable.** No GM guild cooldown-reset operation.
- **110 `accept_quest_if_passed_cond` — Limited.** `accept_quest_if_passed_cond <quest_id>` is
  GM acceptance with catalog level/prerequisite conditions retained; NPC/range gates are bypassed.
- **111 `guild_disband` — Unavailable as a GM operation.** Normal guild disbanding remains in
  the guild service with its own authorization and cooldown rules.
- **112 `channel_incident_start` — Unavailable.** Channel incidents are not implemented.
- **113 `channel_incident_stop` — Unavailable.** Channel incidents are not implemented.
- **114 `follow_user` — Limited, one-time mapping.** `follow_user <online-name>` is a `goto`
  alias, not continuous following. Structured `{char_id}` visits that online character with the
  same private-tutorial restriction as `goto`. Nonzero `follower_char_id` is refused; the command
  can move only its issuer.
- **115 `housing_move_package_unpack` — Supported by the housing service.**
  `housing_move_package_unpack <lot_number> <package_id>`.
- **116 `housing_move_package_recover` — Supported by the housing service.**
  `housing_move_package_recover <package_id> [more_package_ids]`.
- **117 `stat_sync_warning` — Unavailable as a toggle.** Reports the diagnostic limitation;
  use `regen_stat` for a snapshot. Reporting the limitation is not feature support.
- **118 `set_weapon_durability` — Supported.** `set_weapon_durability <value>` applies to
  equipped weapons, capped at each catalog maximum, with guarded durable item updates.
- **119 `raid_channel_enter` — Limited.** `raid_channel_enter <map_id>` forms a bounded
  one-member raid match; the normal raid move performs entry. It does not spawn a raid encounter.
- **120 `bluesalt_fake_purchase_add` — Unavailable.** Paid purchases are not implemented.
- **121 `bluesalt_recovery_run` — Unavailable.** Paid purchases are not implemented.
- **122 `bluesalt_charge_claim_last` — Unavailable.** Paid purchases are not implemented.
- **123 `housing_unlock` — Supported.** `housing_unlock <small|medium|large|guild> [revoke]`.
- **124 `preorder_bonus_dev` — Unavailable.** Preorder entitlements are not implemented.
- **125 `channel_move` — Unavailable as a GM operation.** Use `tp_map` for explicit map travel;
  this is not a claim that map IDs and official channel numbers are interchangeable.
- **126 `housing_account_reset` — Supported by the housing service.** `housing_account_reset`.

## Extra emulator commands without a recovered GmCmd field

`help [1-5]`, `pos`, `tp`, `tp_map`, `up`, `down`, `fwd`, `goto`, `summon`, `players`, `stamina`,
`heal`, `status`, `complete_quests chain`, `housing_contract`, `reveal_map`/`unlock_map`,
`emotion_add`, `ranking_start`, `ranking_finish` and `ranking_cancel` have emulator-defined
text grammar. See the main guide for prerequisites and examples. `reveal_map` changes saved
map fog only; it neither unlocks blocked zones nor completes adventure rewards.

## Verification and limits

The new command paths have synthetic parser, combat, quest, progression and socket checks;
see `VALIDATION.md` for exact results and gated tests. This work did not run an original-client
trial of these added paths or the retail debug renderer. GM authorization and the existing four-request
per-second limit remain mandatory. Unsupported operations need their owning game systems,
not merely a descriptor, a parser or a fabricated success response. Anti-cheat is outside this
implementation scope.
