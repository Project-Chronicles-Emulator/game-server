# Native auth and lobby WebSockets

Chat uses this front connection as well. The verified split between front chat,
world Party chat, and front whispers is recorded in [native-chat.md](native-chat.md).

The demo client uses binary WebSocket messages at `/ws/auth` and `/ws/front`.
The connection code derives `ws` or `wss` from the corresponding HTTP base and
adds an `Authorization: Bearer <access_token>` header. World entry returns a front
REST base; `WorldEnterRes.Granted` has no separate front token field.

Each WebSocket message contains one or more complete application frames:

| Offset | Size | Meaning |
| --- | --- | --- |
| 0 | 2 | Little-endian total frame length, including this four-byte header |
| 2 | 2 | Little-endian packet ID within this service's ID space |
| 4 | remaining | Binary protobuf payload |

There is no application sequence field in these WebSocket frames. This differs
from the recorded C2S world packets, which have a sequence after the packet ID.
WebSocket fragmentation is assembled by Beast before the native decoder runs.

| Service | Request ID | Request | Response ID | Response |
| --- | ---: | --- | ---: | --- |
| auth | 20001 | `x5.pb.authwebsocket.WorldEnterReq` | 1 | `x5.pa2c.WorldEnterRes` |
| front | 20001 | `x5.pb.ws.front.CharListReq` | 6 | `x5.pb.frontwebsocket.CharListRes` |
| front | 20002 | `x5.pb.ws.front.CharCreateReq` | 7 | `x5.pb.frontwebsocket.CharCreateRes` |
| front | 20007 | `x5.pb.ws.front.CharSelectReq` | 12 | `x5.pb.frontwebsocket.CharSelectRes` |

`NativeWsDispatcher` sends these recovered payloads through the existing account,
world-entry and character ownership checks. Unsupported request IDs and malformed
frames are rejected. Native wire compatibility for these handlers is supported by
static client code and synthetic binary tests; it does not establish that a stock
client has completed the full login flow.

The local transport bounds a message to one MiB and 256 application packets;
responses are bounded to one MiB as well. These are emulator resource limits,
separate from the native 16-bit per-packet length.

## Client startup sequence

The client's `WorldEnterRes` granted handler (`0x14616A6E0`) stores
`front_rest_address` and immediately calls `X5_FrontWebSocket_SendCharListReq`
at `0x14616A91C`. That sender creates the front WebSocket if needed and sends
packet 20001. The server therefore does not need to send an unsolicited packet
before the first character-list request.

The front connection's OnConnected delegate is bound through socket event +56:
vtable `0x14A81FA68`, delegate `0x146175460`, callback `0x1461739F0`. It posts
internal connection event 3 and a UI status event. The callback itself sends no
native application packet.

`CharListRes` handler `0x146175D90` checks the result, imports the characters,
site name and character-creation restriction, then updates lobby presentation and
UI state. The traced normal completion branches and their character-presentation
helpers did not establish any extra mandatory RPC before the character screen.
Later UI actions use `0x145F15C80` for character creation, deletion and selection;
non-selection actions may refresh the list afterward.

The `FeatureMatrixNoti` handler (`0x146176BE0`) consumes settings and does not
send an acknowledgement in the traced handler. No speculative startup push was
added. This is a bounded static trace, not a claim that every possible feature,
account state or later UI action uses only the four implemented requests.

Structured startup evidence is in `../protocol/native-front-startup-evidence.json`.

### Observed optional friend-list request

The live client also requested `WSC2F_FriendListReq` (20024) before character
creation. Its two-byte protobuf payload carries the optional `recommend` field.
The matching response is `WSF2C_FriendListRes` (28). Both ID mappings come from
constant wrapper getters in `native-ws-opcodes.json`.

The request schema is `x5.pb.ws.front.FriendListReq`, with optional bool
`recommend = 1`. The response schema is `x5.pb.frontwebsocket.FriendListRes`, with
optional bool `recommend = 1` and repeated `x5.pb.FriendInfo friends = 2`.
There is no error-code field in this response.

A new emulator account can receive an empty friends list. Preserve the request's
`recommend` value and presence so normal and recommended lists stay distinct.
The handler should require an authenticated session with a world selected and
return response 28 on the existing socket. Closing the connection for this
recognized optional request causes an unnecessary reconnect.

Regression fixtures for empty responses are `04 00 1c 00` when `recommend` is
absent, `06 00 1c 00 08 00` when explicitly false, and `06 00 1c 00 08 01` when
true. Tests should cover those three cases, malformed input, missing or expired
authentication and pre-world access. The transport test should request friends
before character creation and then create, list and select on the same socket.

Once a character is selected, the same request returns the stored friends or
recommended list. The other friend requests (20020-20023) and their replies are
described in [friends.md](friends.md).

## Evidence and reproduction

Client image base is `0x140000000`. Function pseudocode is cached under
`../../dumps/native_ws_research/`; connection functions are cached under
`../../dumps/native_login_research/`.

- `0x146169200`: auth WebSocket connection and raw-message delegate registration.
- `0x146174530`: front WebSocket connection and raw-message delegate registration.
- `0x146168F90`: auth receive loop; reads total length, packet ID and payload at +4.
- `0x146173EC0`: front receive loop; same framing, with packet IDs below 65.
- `0x146169E70`: WorldEnterReq send, ID 20001, binary flag true.
- `0x1461740E0`: CharListReq send, ID 20001, binary flag true.
- `0x146174270`: CharSelectReq send, ID 20007, binary flag true.
- Wrapper vtables and exact constant getters for 103 native WebSocket messages are
  recorded in `../protocol/native-ws-opcodes.json`. Service ID spaces overlap.

From the emulator directory:

```powershell
python tools/extract_native_ws_opcodes.py `
  ../dumps/ArcheAgeChronicles.memdump.exe `
  protocol/native-ws-opcodes.json
```

The extractor accepts only the verified vtable layout and constant getter
instruction forms. It does not infer IDs from descriptor order.

## Transport integration tests

```powershell
python tools/native_smoke_test.py out/build/windows/bin/Release/chronicles-server.exe
python tools/native_smoke_test.py out/build/windows/bin/Release/chronicles-server.exe --world
```

Both commands launch an isolated memory-backed server on temporary loopback
ports with generated account credentials. The first exercises native protobuf
HTTP login, auth world entry, and front character creation, listing and selection.
It also covers fragmented WebSocket messages, multiple native packets in a
message, malformed frame rejection, and character ownership rejection.

The optional world test generates a temporary loopback TLS certificate using
`openssl`. It checks entry, time sync, Ping/Pong, exit, one-use handoff keys,
unknown-key rejection, and packet sequence enforcement. Its initial credential
is explicitly a provisional emulator fixture: the client code constructing the
protected world credential has not yet been verified. The test output reports
this limitation even when every synthetic transport check passes.
