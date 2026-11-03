# Native chat transport and initial service

The three official demo captures contain no chat messages. They record the native
world application logger, which does not observe the front WebSocket. Static
analysis of the matching client establishes two chat transports; lack of world
packets alone does not establish a separate host or process.

Client build: `40367e24241c9481bfc07fbf9ef1919f25fb0eaa`. Input SHA-256:
`e4002be7746485ba5e789c8ac798f48dce016d165e07c0e93e4e4e8e4fd693b6`.

## Verified client routing

`AAC_Chat_SendByChannelTransport` at `0x145E79A40` branches on the recovered
`EX5ChatMessageType` values:

| Type | Channel | Transport | Request | Notification |
| --- | --- | --- | --- | --- |
| 1 | Local | Front WebSocket | `WSC2F_ChatReq` 20008 | `WSF2C_ChatNoti` 13 |
| 2 | World | Front WebSocket | `WSC2F_ChatReq` 20008 | `WSF2C_ChatNoti` 13 |
| 3 | Company | Front WebSocket | `WSC2F_ChatReq` 20008 | `WSF2C_ChatNoti` 13 |
| 4 | Guild | Front WebSocket | `WSC2F_ChatReq` 20008 | `WSF2C_ChatNoti` 13 |
| 5 | Party | World connection | `PC2S_SendChatReq` 20038 | `PS2C_SendChatNoti` 75 |
| 6 | Whisper | Front WebSocket, separate request | `WSC2F_DirectMessageReq` 20009 | response 14, recipient notification 15 |

The front connector at `0x146174530` derives `ws` or `wss` from the advertised
front REST base, adds `/ws/front` and an Authorization bearer header, and stores
the WebSocket at context offset 6288. Both the ordinary front chat sender and
the whisper sender use that same socket member. This is the existing front
service, rather than evidence for a dedicated chat-only endpoint.

`AAC_Chat_SendDirectMessageFrontRequest` at `0x145E7A990` uses a cached recipient
character ID when available and otherwise sends the recipient name. Its binary
frame uses the normal four-byte native WebSocket header. There is no world
sequence number in these frames.

Recovered schemas:

- `ChatReq`: channel `type`, UTF-8 `message`.
- `ChatNoti`: `type`, `message`, `sender_name`.
- `DirectMessageReq`: recipient `char_id` or `char_name`, `message`.
- `DirectMessageRes`: `ec`, recipient `char_name`, `char_id`, `message`.
- `DirectMessageNoti`: sender `char_id`, `char_name`, `message`.

IDs are independently recorded by exact wrapper getters in
`protocol/native-ws-opcodes.json` and `protocol/opcodes.json`. The ChatNoti
factory at `0x1461C4FC0` constructs and parses the recovered front schema.
Cached pseudocode and the six-function annotation batch are under
`dumps/ida_research/chat-20261024/` in the parent workspace. Existing meaningful
IDA names are retained and aliases are documented in repeatable comments.

## Implemented service

`NativeChatService` runs inside the existing C++20 server and uses its
authenticated `/ws/front` transport. It implements World chat and online
whispers by exact character name or character ID, with a sender acknowledgement
for whispers and unsolicited recipient notifications. World chat includes a
sender echo. Offline/unknown whisper recipients return `DM_CharNotFound` (800).
These audience, echo, exact-name and online-only policies are emulator choices;
they have not been measured against the official server.

Character selection establishes identity after account/character ownership
checks. A chat payload cannot choose the sender name. Recipient routing uses
the selected realm and character. Draining a queue checks session validity again,
and changing characters drops queued messages addressed to the previous one.
Chat uses cached authenticated identities and performs no database access while
polling idle sockets.

Front connections poll bounded chat queues every 100 ms on the existing worker
pool. The transport serializes replies and pushes through one bounded write
queue per socket. A stalled recipient is disconnected independently. Logout or
token expiry closes an idle front connection on its next queue drain.

Current local limits are 1,024 UTF-8 bytes per message, ten messages per ten
seconds per selected character, 1,024 front connections, and 64 messages / 64 KiB
per recipient queue. Empty text, invalid UTF-8 and control characters are
rejected. These are resource policies, not recovered official limits. Chat text
and credentials are not written to runtime logs or the database.

## Local channel

Local (type 1) is the stock client's default chat channel. Its audience is the speaker
plus every in-world character whose client currently sees the speaker's unit, as recorded
by the world listener's shared presence (sight) registry: same map and within the presence
sight distance. The front dispatcher asks the world service for that audience on every
message; the client cannot supply it. The speaker always receives its own echo. A speaker
in a private tutorial instance is not in the shared registry, so only the echo is sent. A
hidden game master's Local message likewise reaches only itself. A speaker without an
in-world connection receives `CommonErrorRes` `BadRequest` (1), as before.

The radius is the emulator's existing presence sight distance, not a recovered official
Local chat range. Before this change every Local request was refused with a silent
`BadRequest`, so typed chat in the default tab never appeared while World-type server
notices did. Front chat refusals now log the channel number and reason (never the text).

## Remaining work and compatibility boundary

Company chat requires a membership service and is still refused rather than broadcast.
Guild chat uses the guild registry. Party chat belongs to the world transport and requires
party membership. Valid requests for unavailable channels receive front `CommonErrorRes`
(1) without closing the socket.
Friends, blocking, offline messages, moderation/history and cross-realm delivery
remain separate work.

No original-client chat acceptance trial or official front-WebSocket packet
capture has been completed. Such a trial must verify sender echo, recipient name
matching, channel audiences, limits and UI/error behavior. The existing world
logger will not supply that evidence for front messages.

## Checks

```powershell
cmake --build --preset release --target chronicles-tests chronicles-server
ctest --preset release -R 'NativeChat|NativeWebSocket'
python tools/native_chat_smoke_test.py out/build/windows/bin/Release/chronicles-server.exe
```

The smoke test launches an isolated in-memory server and three synthetic native
clients. It checks idle recipient delivery, UTF-8, whispers by name/ID, observer
exclusion, unknown recipients, mixed coalesced RPC/chat traffic, token revocation,
and continued lobby operations. It does not contact Steam or the official service.

On 2026-10-24, the Release build passed six chat tests and 30 existing
authentication, native-WebSocket and schema checks. The three-client chat smoke
test, existing native login/lobby smoke test, and JSON/binary/WebSocket lobby
smoke test passed using `chronicles-chat-validation.exe`. That separately named
binary uses the existing CMake libraries and avoids replacing the running local
server executable. The running service has not been restarted with these changes.
Incremental schema generation also preserved the timestamps of all 46 unchanged
generated source/header files. No original-client chat compatibility is claimed.
