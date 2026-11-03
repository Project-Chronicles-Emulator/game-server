# Tutorial object interactions

Definitions come from the hash-checked client export for build
`40367e24241c9481bfc07fbf9ef1919f25fb0eaa`. `tools/tutorial_interactions.py`
joins placements, phase flows, conditions, and animation notifies. It rejects
changed anchors, unsupported conditions, missing execution events, and
disagreement between character animation rigs.

| Map | Object | Behavior |
| --- | --- | --- |
| 8000 | Locked door 49013 | Becomes available after the Bound Prowler objective; opening consumes one actual key and advances the main objective. |
| 8000 | Lever 49020 | Advances the objective and opens the separate gate. |
| 8000 | Gate 49014 | Follows the lever; direct requests cannot open it. |
| 8000 | Treasure chest 20080008 | Animation 100 opens it once and grants one necklace 11500101 as an item instance. |
| 10117 | Notes 2010032 / 2010033 | Animation 305 activates the character-owned note and grants its catalog's 100 experience once. |
| 10117 | Quest object 10000048 | Advances its objective, plays cutscene 1520, and resets its presentation after 10 seconds. |

The lever-to-gate link is an inference supported by the official trace: the
lever's phase notification precedes the placed gate's opening notification.
The placement export does not contain this script link. Generation resolves
both objects by static placement GUID and checks their CMS IDs. The locked
door's availability is likewise supported by its phase change after the
optional hunt objective, before the key reward is claimed.

Door, lever, and note execution events occur at 933 ms, 2,666 ms, and 614 ms.
Requests are checked against server elapsed time with a 100 ms polling/network
allowance. Client timestamps cannot grant earlier execution. Repeated starts
preserve the original server start time. Actions expire after 30 seconds;
cancellation before execution consumes nothing. Potion and object animations
cannot overlap.

Inventory, experience, objectives, and linked phases commit together before
success packets are returned. Failed commits leave the action unexecuted.
Retries cannot consume another key, repeat rewards, replay the second-map
cutscene, or reopen completed objectives. The cosmetic reset uses the existing
session timer; reconnect resets this temporary presentation. Opened doors and
gates restore from character state. Older saves with a completed lever also
project its linked gate open.

Map 10117 is a separate immutable profile under `followup_profiles`. Its
objects and dialogue targets cannot be addressed from map 8000. Shared-area
shops, NPC combat/loot, optional gathering with SP costs, resurrection objects,
and portal travel are separate gameplay paths.

## Treasure chest

The tutorial's "Treasure Chest" is looting object 20080008 (unit
281492156583942). Its interaction group 300 names animation 100, so the client
opens it with `StartInteractionAnimReq`, `ExecuteInteractionReq` and
`EndInteractionAnimReq`; the plain group-23 pickups still use `InteractUnitReq`.
The generator attaches the animation and its timing to every looting object
whose `Interaction` row has an animation, and marks equipment rewards as item
instances with the catalog grade and durability.

The chest-opening montage's execute notify differs by rig: 2,851 ms (Nuian male,
Elf, Returned), 2,874 ms (Warborn) and 2,937 ms (Nuian female); every rig ends at
4,000 ms. The profile keeps the earliest event, because the server does not trust
the client's rig, and the usual 100 ms allowance applies. The three official
captures executed 2,856, 2,857 and 2,938 ms after the start.

Official order, matched by the runtime:

1. `StartInteractionAnimReq` (animation 100, interaction 300) →
   `StartInteractionAnimNoti` with the request yaw and no `target_unit_id`.
2. `ExecuteInteractionReq` → `ActivateAdventureObjectRes` (Activated),
   `ItemsUpdateNoti` (reason `adventure_object_activate`, one new item row),
   `InteractionAnimRes`.
3. `EndInteractionAnimReq` → `InteractionAnimRes`.

The necklace is a new unequipped row in the lowest free inventory slot with its
creation time. The row and the chest's Activated state commit in one storage
transaction under the session's progress revision. A full inventory answers
`ItemSlotLimit` and keeps the chest closed for a retry; a revision conflict
closes the stale connection, as for other tutorial commits. Reopening an
activated chest plays the animation without another item.

A rejected `StartInteractionAnimReq` answers `StartInteractionAnimRes` with the
error code, the captured rejection shape. The earlier `CommonErrorRes` reply
left the original client locked in its pending animation; that was the live
failure on the chest before this change.

## Level triggers, cutscenes and the Herald Knight door

The reported failure was the sealed door before the final boss. The official trace shows
that the door is not a doodad: after quest 10000044's final volume, the client also sends
`PC2S_LevelTriggerReq` (20084) for the same volume. The server answers with cutscene 1523.
When the client ends it (`PC2S_CutSceneEndReq`, 20055), the server sends `PS2C_TeleportReq`
to the arena behind the door, restores the visible channel and sets the Herald Knight's AI
phase. The previous emulator ignored opcode 20084, so the cutscene and teleport never came.

`tools/tutorial_level_triggers.py` now generates, for each tutorial instance:

- Level triggers from `LevelTriggerVolume`, `LevelTriggerTask` and `LevelTriggerCondition`,
  joined with the instance's placed volumes. A volume can have several rules; each rule has
  `PlayCutScene` or `ChangeDoodadPhase` tasks and grouped quest conditions. Rules that need
  unmodeled conditions, tasks or an unrecovered end point are kept with `supported: false`
  and never run.
- Cutscene follow-ups: the `CutSceneBind` server NPC binding (cutscene 1523 binds spawner
  `e8f654fb…` / NPC 5001527 to phase 2302400), the end-point map from `CutScene`, and, as
  labeled captured evidence, the cutscene-1523 arena teleport and the 10117/9011 end-point
  spawns. The client export has no server end-point table.
- The Herald Knight defeat script (captured): `BossKilledBannerNoti`, activation of the
  deactivated arena doodad 10100017, and cutscene 1521.

| Step | Server behavior |
| --- | --- |
| Final volume of 10000044 + LevelTriggerReq | VisibleChannelNoti 6, CutScenePlayNoti 1523 |
| CutSceneEndReq 1523 | TeleportReq to the arena, VisibleChannelNoti 3, SetNpcAiPhaseNoti 2302400 |
| TeleportStart/CompletionReq | Moves the authoritative position |
| Herald Knight finishing kill | Quest update, BossKilledBannerNoti, doodad phase 1, cutscene 1521 |
| CutSceneEndReq 1521 | MapMoveStartNoti 10117; the next MapLoadComplete/StartPlaying finish the move |
| Map 10117 doodad cutscene 1520 ended | MapMoveStartNoti to the shared world |

A cutscene's follow-up runs once; repeated or mismatched end requests do nothing. A character
saved in map 10117 resumes there. The 9011 end point is placed on the single hosted overworld
channel. Later boss phases (2302401+) are not emitted, and shared-world level triggers are not
handled.

## Verification

```powershell
python tools/build_tutorial_data.py
python tools/test_tutorial_interactions.py
python tools/verify_tutorial_interactions.py `
  out/captures/official-full-session-20261024-01/decoded-final.jsonl
cmake --build --preset release --target chronicles-tests
ctest --preset release -R 'TutorialInteractionTest|TutorialTest|Quests|TutorialData'
```

The private trace verifier checks three observed animations, the second-map
direct interaction, phase IDs, and lever/gate ordering. Its output contains
static CMS IDs without official character/session values. Note 2010033 has
captured evidence; note 2010032 is supported by matching tables and placement.
Synthetic tests cover encoding, atomic saves, retries, cancellation, expiry,
range/map checks, and reconnect. The chest tests cover the captured packet
order, the instance row, early execution, a full inventory, missing item
storage, reconnect, and the start-rejection shape. An original-client trial
against this implementation remains required, including the chest.
