# Friends, recommended friends and presence

Status: **partial** — server behavior is implemented for the recovered friend packets; it has
synthetic test coverage only. No official friend traffic was captured and the original client
has not yet been observed using these replies.

## Trigger

Adding a friend in the local client showed "System Error". The front WebSocket only answered
`WSC2F_FriendListReq` (20024). Every other friend request threw "Unsupported native front
WebSocket packet ID", which closed the front socket.

## Client behavior (static evidence)

Recovered from the client friend manager (`dumps/ida_research/friends-20261027`, names
`AAC_Friend_*`):

| Request | Reply the client handles | Client effect |
| --- | --- | --- |
| `FriendSearchReq` 20020 | `FriendSearchRes` 26 | Matched by `req_seq`. Add-by-name/add-by-character searches send `FriendAddReq(friend.account_id)` immediately. |
| `FriendAddReq` 20021 | `FriendAddRes` 27 | Appends the friend, removes the account from the recommended list, shows SystemMessage 340. |
| `FriendRemoveReq` 20022 | `FriendRemoveNoti` 29 | Removes from the friends or recommended list (`recommend`), shows 337 or 341 when an entry existed. |
| `FriendModifyReq` 20023 | none defined | The emulator answers with `FriendUpdateNoti` 30, which upserts the friend. |
| `FriendListReq` 20024 | `FriendListRes` 28 | `recommend` selects the recommended list; otherwise the friends list. |

- Every friend reply is ignored unless the client is playing in the world.
- `FriendInfo.status` is a oneof: `char` (online, with world, class, level and optional guild
  name) or `last_login_epochtime` (offline). The list shows "Online" or minutes/hours/days since
  that time. Epoch times are 100 ns Unix ticks.
- Error mapping: `InvalidChar` (6) shows "Unable to find target", `InvalidState` (7) "Friends
  list full", `DuplicateRequest` (35) "Friend request already sent"; other codes use the generic
  error display.
- The client pre-checks GameVariable `MaxFriendNum` (60) and duplicates before sending.
- The in-world "Add Friend" interaction searches by the target's `char_id` in add mode. The
  emulator sends `PlayerNub.char_id` as the raw character ID, which is what the search matches
  (Candidate: the interaction context field was read statically, not observed).
- `WSF2C_SystemMessageNoti` (2) displays a SystemMessage by key; its handler has no in-world
  gate.

## Emulator implementation

- `src/world/features/friends/friends.hpp`, `src/world/features/friends/friends.cpp`: transport-free `FriendService`. Friendships are account
  rows per realm. Adding is one-directional and immediate; the added account sees the requester
  in its recommended list until it adds the requester back or dismisses it (Candidate model).
- `src/world/features/friends/friend_storage.cpp`: `MemoryFriendStore`, `PostgresFriendStore` (schema version 31:
  `emulator_friends`, `emulator_friend_presence`).
- `src/persistence/character_summary_storage.cpp`: cheap character summaries on both account
  stores for name/ID/account search.
- `src/world/features/friends/native_friend.cpp`: front WebSocket adapter for the five requests, the presence
  `FriendUpdateNoti` pushes and the `SystemMessageNoti` `SM_FRIEND_ADD_ALARM` (335) push to an
  added player who is in the world and does not already list the requester.
- Presence: the world listener reports StartPlaying (existing entry handler) and the matching
  connection close (new exit handler). Online friends receive `FriendUpdateNoti` both times; the
  departure time becomes the persisted last login. A second connection of the same character
  keeps it online until both close.

### Rules and limits

- Friends list limit: GameVariable `MaxFriendNum` (60 in the shipped catalog).
- New friendships default the alias to the friend's character name, because an offline
  `FriendInfo` has no character name (Candidate). An empty `FriendModifyReq.alias` restores it.
- Alias: valid UTF-8 without control characters, at most 16 code points (Candidate: the client
  text policy was not recovered).
- Search by character name is exact and case-sensitive on the realm. Search by account resolves
  the account's in-world character, else its last played one, else its first lobby slot.
- Steam friend codes are not linked to accounts; those searches report `InvalidChar`.
- Removal is idempotent and always answered with `FriendRemoveNoti`.
- Refusals: self-add `BadRequest`; unknown target `InvalidChar`; already listed
  `DuplicateRequest`; full list `InvalidState`. Invalid or unknown alias targets receive
  `WSF2C_CommonErrorRes` `BadRequest`.
- Before a character is selected, `FriendListReq` keeps its previous empty-list reply.

### Not implemented

- Blocking, `pk_kill_count` (always 0) and Steam friend codes.
- Live pushes for level or guild changes while both players stay online; the next
  `FriendListReq` returns the stored values.
- Recommended-list pushes: the client has no recommendation notification other than the
  SystemMessage alarm, so the list refreshes when the client requests it.

## Verification

- `src/world/features/friends/tests/friends_tests.cpp`: service rules, presence pairing, alarms, persistence across a
  service restart, alias validation, and wire-level replies through `NativeWsDispatcher`.
- Original-client acceptance has not been observed yet.
