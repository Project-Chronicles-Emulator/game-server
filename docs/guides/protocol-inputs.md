# Build inputs and recovered packet fields


A fresh checkout can compile the server without a client installation or extracted
assets. CMake reads the checked-in `protocol/descriptors.normalized.pb` and runs
`tools/generate_protos.py` with the pinned dependency's `protoc`. The game catalogs
and profiles above are runtime inputs, generated privately for the supported build.
They are not required to link the executable and are not included in releases.

## Complete recovered packet fields

The repository includes both original and normalized client descriptor sets under
`protocol/`. They contain 92 schema files, 1,966 messages (including nested/helper
messages), and 7,362 fields. These are not 1,966 independent network packets.
Normalization changes only `x5msg` syntax to `proto3`; field definitions are preserved.

[`protocol/packet-fields.json`](../../protocol/packet-fields.json) is the complete,
readable protobuf JSON representation of the normalized descriptor set. It retains
field names/numbers/types, labels, nested messages, enums, dependencies and options.
Use `type_name` to follow referenced message/enum definitions. Numeric packet IDs
remain in `protocol/opcodes.json` and `protocol/native-ws-opcodes.json`.

```powershell
python tools/export_protocol_schema.py
python tools/export_protocol_schema.py --check
```

The check validates original-versus-normalized descriptors, resolves dependencies
and round-trips the JSON back to the complete descriptor set. It certifies all fields
in the recovered set, not unknown schemas or another client build. To recover your
own matching descriptors, use stage C above. The packet/feature reference tracks
runtime implementation separately from these recovered field definitions.
