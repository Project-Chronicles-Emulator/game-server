# Native packet capture and character evidence

`tools/capture_native_session.py` takes a bounded, read-only slice of the
original client's `X5Packet.log`. The client writes decrypted **world
application** frames in both directions. The tool classifies each frame using
`protocol/opcodes.json`, writes a small metadata index, and keeps byte-exact
frames in a native-log-compatible binary file for `chronicles-replay`.

The raw binary may include private character or session values. Capture under
ignored `Emulator/out/`; do not commit or share it. The index and console output
contain only timing, direction, opcode, packet name, and frame length. The
client's HTTP authentication and front WebSocket character creation are outside
this logger.

## Capture a run

Start the **official** client using its official bootstrap configuration. The
emulator launch archive and local world-entry helper are only for local runs.
Wait until `X5Packet.log` exists, then arm the capture before entering the world
or performing the action:

```powershell
cd 'D:\PROJETOS\Archeage Emulator\Emulator'
python tools/capture_native_session.py out/captures/official-entry-001 --label official-entry --seconds 90
python tools/capture_native_session.py out/captures/official-walk-001 --label official-walk --seconds 45
python tools/capture_native_session.py out/captures/official-skills-001 --label official-skills --seconds 45
```

Use a new output directory for each phase. The capture fails if the game
rotates or rewrites the log during the phase; start a new phase after a restart.
It excludes the older complete records present before arming, but retains a
partial initial record when that record finishes during the phase. It excludes
an incomplete final record and reports its byte count.

Decode a private capture with the existing replay binary:

```powershell
& out/build/windows/bin/Release/chronicles-replay.exe `
  out/captures/official-walk-001/native-session.bin protocol `
  --jsonl out/captures/official-walk-001/decoded.jsonl
```

The client's original logger was previously validated against 17,055 complete
records, including movement-related server notifications, skill requests,
equipment updates, and world notifications. That corpus does not contain a
client `MoveReq`; the local tutorial trial supplied the first observed
`MoveReq` in this work. The older corpus is under
`G:\Dumps\Archeage\ClientData_2026-10-23\LivePackets\` and is not copied into
source.

An official BR realm entry on 2026-10-23 was captured at
`out/captures/official-entry-20261023-01/`. It contains 528 complete world
frames (12 PC2S, 516 PS2C) across 65 opcodes. Offline replay decoded and
round-tripped all 528. The observed entry order was `CharEntryRes`,
`TimeSyncReq`, `MapMoveStartNoti`, `TimeSyncRes`, world character state including
`WorldCharItemsNoti`, `MapLoadCompleteReq`, then `MapMoveCompleteNoti`. Five
client `StartSkillReq` frames and three `StartX5RootMotionReq` frames were also
captured. The run did not produce a client `MoveReq`, so movement persistence
still needs a separate live test.

A later complete tutorial run on 2026-10-24 covered two instanced sections and
entry into the shared area. See [official-tutorial-trace.md](official-tutorial-trace.md)
for the verified map sequence, packet counts, and implementation boundary.

## Appearance and persistence evidence

The earlier official `PS2C_WorldCharItemsNoti` and `MapMoveCompleteNoti` for a
level-one Magic character have nine starter items: weapon, four armor pieces,
three costume pieces, and bag. The WorldCharItems notification precedes the
client's `MapLoadCompleteReq`. The map-complete `PlayerNub` repeats the equipped
item CMS IDs and carries face/body customization. The client asset catalog
`Character.jsonl` identifies Wild as Nuian Female and its base clothes pack
`473`; `ClothesPack.jsonl` maps that pack to model IDs 87, 88, and 89. The
`Item.jsonl` entries for those models are Wild starter costume IDs 10130202,
10130203, and 10130204. Wild's level-one hand axe is item 11102302. These
catalog-derived Wild IDs are an inference until a Wild official creation and
world-entry capture verifies them.

The emulator now sends saved class-specific item instances and copies the lobby
customization into its world `PlayerNub`. New Fight, Wild, and Adamant
characters receive their catalog-linked weapon, three-piece base costume, and
bag. New Magic characters receive the nine-item set observed in the official
capture, including fabric armor. The client tables do not prove which armor
material Fight, Wild, or Adamant start with, so no armor IDs are assigned to
those new characters. Older Wild fabric armor from the version 3 backfill is
preserved. The server saves a valid
last movement position every two seconds and on connection close, then uses
that position on the next tutorial entry. This is local development progress;
quest, inventory changes, combat, and other map persistence still need their
own observed official packets and handlers.

The lobby create/list response also carries each character's equipped item IDs
in slot-indexed `equip_cms_ids`, with the main weapon at slot zero. The mapping
follows the recovered `EX5EquipSlotType` enum and passes a focused create/list
test. In the fresh local trial, the original client rendered the saved
character clothed at selection and in the tutorial world; a sheathed weapon
was visible in the world. This establishes the local appearance path for that
character, while the exact Wild starter set still awaits an official Wild capture.

Character creation uses a separate front WebSocket envelope. The emulator
already parses `CharCreateReq`, persists its serialized customization, and
returns it in lobby character responses. A local creation can therefore check
the end-to-end appearance path without recording an official account's front
WebSocket token. Capturing **official** character creation still needs a
separate, authenticated front-transport observation method; this world logger
does not claim to provide it.
