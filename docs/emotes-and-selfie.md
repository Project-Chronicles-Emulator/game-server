# Emotes, emotion ownership and selfie mode

`src/world/features/expression/player_expression.*` answers the emote request (`PC2S_EmotionExecReq`, 20063) and the
selfie-mode report (`PC2S_SelfieModeReq`, 20064). It also sends the character's owned-emotion
snapshot and grants. The world session routes both requests to it in tutorial, followup and
shared-map sessions.

## Evidence

**Official captures** (build `40367e24241c9481bfc07fbf9ef1919f25fb0eaa`):

- `PS2C_WorldCharEmotionsNoti` (144) appears once per character entry in T, P2, A1 and M1
  (twice in M1, once for each of its two entries). It is always empty (zero protobuf bytes).
  It follows `WorldCharAchievementsNoti` and precedes `WorldCharGuildNoti`. Map moves resend
  none.
- `PS2C_EmotionExecNoti` (123) appears four times in P1, always for other players' units:
  three Dance emotes (id 9) and one Chair_Sit_01 loop (id 14). Each one carries `unit_id`,
  `emotion_cms_id`, `tick`, `pos` and `yaw`.
- No `EmotionExecReq`, `EmotionExecRes`, `SelfieModeReq`, `SelfieModeNoti` or
  `EmotionsUpdateNoti` was captured.

**Catalog:** `Tables/Emotion.jsonl` has 12 rows (ids 1-10, 13 and 14). All rows are
`category=Basic`. Rows 13 and 14 have a `targetAnimStanceKey` and loop. GameVariable
`EmotionCooltimeSec=1.0`. `ChattingCommand` Social rows map chat aliases to emotion ids.
`ErrorCode` defines `Emotion_Dead` (9101) through `Emotion_NonGroundIdle` (9108).
`SystemMessage` 407 is a `ContentsUI_Emotion` achievement banner.

**Static client** (IDA, `dumps/ida_research/expression-20261026/`; 23 annotated functions):

- `AAC_Emotion_Manager_TryExecute` is called by the chat slash-command handler and the emote
  UI. It sends `EmotionExecReq{emotion_cms_id, pos = pawn location * 10 (cm to mm), yaw}`.
  `AAC_Emotion_Manager_CanExecute` refuses locally (system message 380) unless the emotion
  is in the owned set. It also refuses within the cooldown measured from the previous
  request, and in several blocked actor states.
- `AAC_PS2C_EmotionExecRes_Handle`: an error code shows its text and cancels the pending
  emote. Success (the proto3 default) plays the pending emote on the local pawn. **The
  requester's own emote therefore depends on this response.**
- `AAC_PS2C_EmotionExecNoti_Handle` and `AAC_PS2C_SelfieModeNoti_Handle` skip an actor
  whose virtual check at +1104 is true. This is the same guard the movement handler uses for
  the local player (Candidate). The notification handler applies the emote stance and starts
  the motion at the packet tick; it does not read `pos` or `yaw`.
- `AAC_Emotion_Manager_Candidate_SeedBasicEmotions` inserts every row whose category byte is 1
  (Candidate: Basic) into the owned set. Basic emotions therefore need no server list.
  `WorldCharEmotionsNoti` adds a list; `EmotionsUpdateNoti` adds new emotions and posts a
  "new emotion" UI event for ids not already owned.
- `AAC_PhotoMode_Candidate_Enter` creates the local selfie camera and then sends
  `SelfieModeReq{enter=true}`. `AAC_PhotoMode_Candidate_Exit` sends `enter=false`. No
  response message exists.
- `PC2S_StartSocialMotionReq` is referenced only by the packet-logging policy table and RTTI.
  No wrapper vtable, opcode or sender exists in this build.

## Emulator behavior

**Emotion catalog.** The listener loads the Emotion rows and `EmotionCooltimeSec` at startup.
A missing or implausible cooldown keeps 1.0 s and logs a warning. A malformed row fails startup.
A server without a client catalog knows no emotion, so it refuses every emote.

**Ownership.** Basic rows are always owned. Other categories (Reward, BM, None) need a grant.
Grants persist in `WorldProgress.owned_emotions` (table `emulator_character_emotions`,
schema version 27). They are written under the world-progress revision and are never
removed. The game-master text command `emotion_add <emotion_id>` grants one non-Basic
catalog emotion. It sends `EmotionsUpdateNoti` and replies in GM chat. The exported catalog
has no non-Basic row, so with the shipped data every grant is refused as already owned or
unknown.

**Entry snapshot.** At the character's first `StartPlayingReq`, `WorldCharEmotionsNoti`
lists the granted emotions. It follows `StartPlayingRes` and precedes the guild snapshot.
For a Basic-only character, it is the observed empty message. It is sent once per
connection; map moves resend none.

**Emote request.** Checks run in this order. The first failure answers `EmotionExecRes{ec}`
to the requester only and sends nothing to anyone else:

| Check | Code |
| --- | --- |
| No loaded map, or an emotion id missing from the catalog | `Emotion_InvalidData` |
| A non-Basic emotion without a grant | `Emotion_NotOwned` |
| The player is dead | `Emotion_Dead` |
| A skill is in progress | `Emotion_UsingSkill` |
| A gathering or tutorial interaction is in progress | `Emotion_Interaction` |
| Riding, or the last movement mode is not Normal or Crouching | `Emotion_NonGroundIdle` |
| Less than the cooldown minus 200 ms since the last accepted emote | `Emotion_Cooltime` |

An accepted emote answers an empty `EmotionExecRes` (Success). Every client that currently
sees the player then receives `EmotionExecNoti` with the player's unit id, the emotion id,
the current server tick and the server's position of the player. The requested yaw is used
when it is finite and within ±360 degrees; otherwise the server's last yaw is used. An absent
yaw is the proto3 default 0. Refusals do not start the cooldown. Invisible game masters have
no viewers.

**Selfie mode.** A state change sends `SelfieModeNoti{unit_id, enter}` to every client that
sees the player. A repeated report sends nothing. Entering is ignored while dead or outside
a loaded map; leaving is always accepted. The requester gets no packet, because no response
exists and its own client ignores the notification. A shared registry remembers players in
selfie mode. A viewer who sees such a player later receives `SelfieModeNoti{enter=true}`
directly after that player's `UnitAddNoti`. Map loads and disconnects clear the state without
a notification, because the old viewers have already removed the unit.

## Limits

- **Candidate interpretations:** the Basic category byte in the client seeding loop; the
  local-actor meaning of the +1104 check; and the error-code order, which no official
  refusal establishes.
- **Emulator policy:** the 200 ms cooldown arrival allowance, server position instead of the
  client-reported position, ignoring selfie entry while dead, and replaying selfie state to
  later viewers.
- `Emotion_Reaction` (hit reactions or crowd control) is not modeled. Looping emotes (13, 14)
  are not replayed to players who come into view later, and the server does not track when a
  loop ends.
- The GM grant is the only grant source; no item, quest or shop in this build grants an
  emotion. `PC2S_StartSocialMotionReq` is not handled, because this client never sends it.
- No official emote, selfie or grant exchange was captured. No original-client trial has been
  performed.

## Verification

- `src/world/features/expression/tests/player_expression_tests.cpp` covers catalog parsing, the refusal order and codes, the
  cooldown and its allowance, yaw handling, selfie changes, grants, the selfie registry, the
  GM command parser and memory-store persistence. An opt-in check loads the exported catalog
  (`CHRONICLES_CLIENT_CATALOG`), and an opt-in PostgreSQL round trip runs against the
  isolated test cluster (`CHRONICLES_TUTORIAL_TEST_DATABASE_URL`). Every packet is encoded and
  decoded with the recovered descriptors.
- `NativeWorld.EmotesGrantsAndSelfieModeReachNearbyPlayersOverTls` runs three TLS world
  clients. It covers the empty entry snapshot, a refused unowned emote, a GM grant with
  `EmotionsUpdateNoti` and persistence, an accepted emote seen by the nearby player, a
  cooldown refusal that the viewer never sees, and selfie mode for both a current viewer and
  a later one.

**Results (2026-10-26, Release, author-run, synthetic only).** The emote, catalog, grant and
storage tests and the TLS test passed. The exported-catalog check passed with
`CHRONICLES_CLIENT_CATALOG`. The PostgreSQL round trip passed on a disposable loopback
cluster migrated to schema 28. The whole `chronicles-tests` executable ran 861 tests: 786
passed, 74 were skipped, and `NativeWorld.BagUnequipWithCarriedWeightAboveDefaultSafeWeightHasNoRefusal`
failed with an access violation. That failure reproduces with test builds made before this
change. The build used a private copy of the shared working tree, because other work was
changing the tree during this change. No independent review or original-client trial has
been performed.
