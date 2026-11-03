# Local world entry findings — 2026-10-23

This record describes the observed ArcheAge Chronicles Demo build
`40367e24241c9481bfc07fbf9ef1919f25fb0eaa`. Addresses below belong only to
that build. The detailed function evidence and saved IDA annotations are in
`../../dumps/ida_research/world-tls-live/` and
`../../dumps/ida_research/world-protocol-resume/`.

## Why the first world attempt failed

Character selection completed through the local front WebSocket. The client
connected to the emulator's loopback world listener on port 8081 and received
its development certificate, then sent TLS alert 40 and retried. The client
loaded that certificate as a trusted root and matched the advertised hostname,
but its protected per-connection verify callback set OpenSSL result 50,
`X509_V_ERR_APPLICATION_VERIFICATION`. Changing `packet_key`, the advertised
host, configuration pins, and one live pin record did not change that result.

Replacing the callback on one matching live SSL object allowed TLS 1.3 to
complete, but the client closed the socket before sending world application
data. A separate protected initializer return gates the first send and thread
startup. A two-byte `or al, 1` trial at the initializer return branch, combined
with the SSL callback override, allowed the first world credential to be sent.
The actual protected acceptance rules remain unknown.

The successful trial read a 100-byte initial credential. Its selected-character
token occupied bytes 36–99; the first 36 bytes remain opaque. The server
validated the token without logging its contents. The client and emulator then
exchanged the following observed sequence:

| Client request | Emulator response | Observed effect |
| --- | --- | --- |
| Initial 100-byte credential | `PS2C_CharEntryRes` (5) | World authentication completed |
| `PC2S_TimeSyncReq` (20001) | `TimeSyncRes` (1), `MapMoveStartNoti` (175) | Tutorial map load began |
| `PC2S_MapLoadCompleteReq` (20085) | `MapMoveCompleteNoti` (176) with player `UnitNub` | Player appeared at the captured Wild spawn |
| `PC2S_StartPlayingReq` (20086) | `StartPlayingRes` (183) | HUD and standing character were visible |
| World pings | Pongs | Connection remained alive |

The map ID was 8000. The captured spawn coordinates were
`(-795146, 712194, -698778)` millimeters, yaw `158.56723` degrees. The
player nub used Idle state and captured `MoveSpeedRate` stat 84 with value
1000. In a later live session, the user moved in the tutorial world and the
server recorded many `PC2S_MoveReq` (20087) packets. The client visibly moves,
but the server currently decodes and ignores those requests, so authoritative
movement and persistence are not established.

## Launcher implementation

The launcher now starts the server with the opt-in two-second world handshake
delay and tutorial bootstrap packet sequence. It creates a local
`IniBootstrap` archive from the pinned extracted config and passes it to Steam
with `-textconfig -IniBootstrap=...`. Once the client is detected, it starts a
separate world-entry helper before the character-selection key is pressed.

The helper waits for the protected client code to finish initializing before
checking its build signatures. A first trial launched the helper immediately
after Steam started the game and hit a transient signature mismatch; a later
read of the same process matched. The launcher skips its expensive full-process
endpoint scan for clients it launched with the generated local bootstrap.

In a clean launcher-managed restart at 21:24–21:28, the original client made
its local bootstrap and auth requests, then sent the 100-byte world credential.
The server received map-load and start-playing requests and returned the
captured tutorial response sequence. The client displayed the character and
HUD in the world. This trial required no separate manual patch command.

The helper requires the expected game executable, process creation time, code
signatures, a loopback TCP connection to the configured world port, and the
expected world connection/SSL ownership chain. It temporarily changes only the
matching SSL object's callback and the researched two-byte startup branch.
It checks each write by reading it back, restores the callback after the
handshake window, and restores the original startup bytes. The helper does not
load or unload PlaySharp and does not modify game files.

After a completed world connection, the launcher leaves the helper idle while
that TCP connection remains established. It rearms after a disconnect. Helper
output is appended to `out/launcher/world-entry.log` so earlier trial results
remain available when a later trial starts.

The launcher still offers its reversible heap-string endpoint redirect for
clients started outside the generated-bootstrap path. The archive keeps
bootstrap local from startup when Steam forwards the launch arguments. Its
source hash is pinned by `tools/binary_config_codec.py`; the generated archive
and evidence stay under ignored `out/launcher/`.

## Two private clients with named accounts

In a later 2026-10-23 trial, two concurrent processes from the guarded private
client copy used separate named-account grants. The world-entry helper watched
PID 44128 and a second helper targeted PID 26684. Both clients selected newly
created characters. The server completed TLS 1.3 on two world connections and
accepted the credentials for character IDs 3 and 4. Both game windows displayed
a wooded world-start scene at the same time. The helper logs reported restoring
the original startup bytes and SSL callback fields after each connection trial.

The server also logged `World bootstrap diagnostic skipped` for both selected
characters. At the time, `WorldHandshake::configure_bootstrap_character` permitted
the captured tutorial bootstrap only for the Wild class, with map 8000 required
when a saved world position existed. Neither connection received the
`MapMoveStartNoti` or `MapMoveCompleteNoti` bootstrap response, and neither sent
`MapLoadCompleteReq` or `StartPlayingReq` at this checkpoint. The visible terrain
therefore does not establish server map enrollment or nearby-player presence.
This was an earlier checkpoint. A subsequent two-client trial used the added
class-specific tutorial spawns. Both clients completed map loading on map 8000,
the server sent peer-add notifications to each connection, and both game
windows showed the other equipped character nearby. The server logged movement
relays in both directions and changing positions from both clients. Skill
effects, combat, and reconnect restoration remain unverified.

## Hosted world entry correction (2026-10-26)

The previous hosted helper applied only the startup gate. The original client
connected to the selected public world endpoint but rejected TLS before sending
its world credential. Copying the local always-success callback to the public
path would discard certificate verification.

The hosted helper now selects OpenSSL's default certificate verifier while
retaining SSL_VERIFY_PEER. Before changing either field it requires the researched
executable SHA-256 and live code signatures, the selected game-owned TCP peer,
and exactly one native X509 expected hostname matching the selected world DNS
name. Missing or different names, alternative hostname lists, unsupported
inheritance flags, and weakened certificate/hostname options fail closed.
The local helper retains its existing development behavior.

The native parameter layout was correlated with the matching dumped binary and
OpenSSL 1.1.1t. In a read-only original-client sample, SSL.param held one hostname,
host flags 4, default verification flags, and no expected IP. A subsequent fresh
launcher-driven hosted trial passed the exact DNS guard, completed TLS, and
created both world networking threads with verification result zero. The helper
restored the original SSL callback and startup instruction with readback. The
live server accepted the world credential, and the original game displayed the
world, tutorial objective, and a live RTT reading while the player moved.

Seven isolated hostname-policy tests passed. This is one observed hosted entry
on the researched client build, not a capacity, full-gameplay, or unknown-build
compatibility claim. No live server, firewall, certificate, or account settings
were changed. Private process/session details remain outside this reference.

IDA accepted the additional parameter-function renames in memory, but its MCP
instance became unresponsive before a database save could be confirmed. The
static research index and journal explicitly retain that incomplete persistence
gate; no saved-annotation claim is made for that batch.

## Development scope

This is a local development diagnostic for one client build, not a recovered
certificate policy. It temporarily changes the running client's executable
bytes, so a changed game build must fail closed until its signatures and
semantics are revalidated. A newly started client without the launcher helper
still rejects the development world certificate. The emulator implements
tutorial entry and nearby-player add, move, and remove messages; general maps,
authoritative movement, and gameplay simulation remain unfinished.
