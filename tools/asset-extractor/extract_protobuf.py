"""Recover serialized FileDescriptorProto records embedded in a local PE dump."""

import argparse
import hashlib
import json
import re
from pathlib import Path
from google.protobuf import descriptor_pb2, descriptor_pool, json_format


def varint(data, pos):
    value = 0
    for shift in range(0, 70, 7):
        byte = data[pos]
        pos += 1
        value |= (byte & 127) << shift
        if byte < 128:
            return value, pos
    raise ValueError("overlong varint")


def descriptor_end(data, pos):
    start = pos
    allowed = {
        1: 2,
        2: 2,
        3: 2,
        4: 2,
        5: 2,
        6: 2,
        7: 2,
        8: 2,
        9: 2,
        10: 0,
        11: 0,
        12: 2,
        14: 0,
    }
    while pos < len(data) and pos - start < 8_000_000:
        tag, next_pos = varint(data, pos)
        number, wire = tag >> 3, tag & 7
        if allowed.get(number) != wire:
            break
        if wire == 2:
            size, next_pos = varint(data, next_pos)
            if next_pos + size > len(data):
                break
            value = data[next_pos : next_pos + size]
            pos = next_pos + size
            if number == 12 and value in (b"proto2", b"proto3", b"editions"):
                return pos
        else:
            _, pos = varint(data, next_pos)
    return pos


def enum_source(enum, indent=""):
    lines = [f"{indent}enum {enum.name} {{"]
    if enum.options.allow_alias:
        lines.append(indent + "  option allow_alias = true;")
    lines.extend(f"{indent}  {v.name} = {v.number};" for v in enum.value)
    lines.append(indent + "}")
    return lines


def message_source(msg, syntax, indent=""):
    lines = [f"{indent}message {msg.name} {{"]
    if msg.options.map_entry:
        lines.append(indent + "  option map_entry = true;")
    for enum in msg.enum_type:
        lines.extend(enum_source(enum, indent + "  "))
    map_types = {
        nested.name: nested for nested in msg.nested_type if nested.options.map_entry
    }
    for nested in msg.nested_type:
        if not nested.options.map_entry:
            lines.extend(message_source(nested, syntax, indent + "  "))
    types = descriptor_pb2.FieldDescriptorProto

    def field_source(field, prefix):
        scalar = types.Type.Name(field.type).removeprefix("TYPE_").lower()
        typename = (
            field.type_name
            if field.type in (types.TYPE_MESSAGE, types.TYPE_ENUM, types.TYPE_GROUP)
            else scalar
        )
        label = {
            types.LABEL_OPTIONAL: "optional ",
            types.LABEL_REQUIRED: "required ",
            types.LABEL_REPEATED: "repeated ",
        }[field.label]
        entry = map_types.get(field.type_name.split(".")[-1])
        if entry and field.label == types.LABEL_REPEATED:

            def entry_type(f):
                return (
                    f.type_name
                    if f.type in (types.TYPE_MESSAGE, types.TYPE_ENUM)
                    else types.Type.Name(f.type).removeprefix("TYPE_").lower()
                )

            typename = (
                f"map<{entry_type(entry.field[0])}, {entry_type(entry.field[1])}>"
            )
            label = ""
        if field.HasField("oneof_index") and not field.proto3_optional:
            label = ""
        elif (
            syntax == "proto3"
            and field.label == types.LABEL_OPTIONAL
            and not field.proto3_optional
        ):
            label = ""
        opts = []
        if field.options.HasField("packed"):
            opts.append("packed = " + str(field.options.packed).lower())
        if field.HasField("default_value"):
            default = (
                json.dumps(field.default_value)
                if field.type in (types.TYPE_STRING, types.TYPE_BYTES)
                else field.default_value
            )
            opts.append("default = " + default)
        option = " [" + ", ".join(opts) + "]" if opts else ""
        return f"{prefix}{label}{typename} {field.name} = {field.number}{option};"

    for field in msg.field:
        if not field.HasField("oneof_index") or field.proto3_optional:
            lines.append(field_source(field, indent + "  "))
    for i, oneof in enumerate(msg.oneof_decl):
        fields = [
            f
            for f in msg.field
            if f.HasField("oneof_index")
            and f.oneof_index == i
            and not f.proto3_optional
        ]
        if fields:
            lines.append(f"{indent}  oneof {oneof.name} {{")
            lines.extend(field_source(f, indent + "    ") for f in fields)
            lines.append(indent + "  }")
    for extension in msg.extension_range:
        end = "max" if extension.end == 536870912 else str(extension.end - 1)
        lines.append(f"{indent}  extensions {extension.start} to {end};")
    lines.append(indent + "}")
    return lines


def source(proto):
    syntax = "proto3" if proto.syntax == "x5msg" else proto.syntax or "proto2"
    lines = [
        "// Reconstructed from the embedded descriptor. Use descriptors.pb for authoritative metadata.",
        f"// Original syntax: {proto.syntax or 'proto2'}. x5msg is normalized for tooling; validate payloads separately.",
        f'syntax = "{syntax}";',
    ]
    if proto.package:
        lines.append(f"package {proto.package};")
    for dep in proto.dependency:
        lines.append(f'import "{dep}";')
    for enum in proto.enum_type:
        lines.extend(enum_source(enum))
    for msg in proto.message_type:
        lines.extend(message_source(msg, syntax))
    for svc in proto.service:
        lines.append(f"service {svc.name} {{")
        for method in svc.method:
            request = ("stream " if method.client_streaming else "") + method.input_type
            response = (
                "stream " if method.server_streaming else ""
            ) + method.output_type
            lines.append(f"  rpc {method.name} ({request}) returns ({response});")
        lines.append("}")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    data = args.binary.read_bytes()
    args.output.mkdir(parents=True, exist_ok=True)
    records = {}
    for hit in re.finditer(
        rb"\x0a([\x01-\x7f])([a-zA-Z_][a-zA-Z0-9_./-]*\.proto)", data
    ):
        if hit[1][0] != len(hit[2]):
            continue
        start = hit.start()
        end = descriptor_end(data, start)
        raw = data[start:end]
        proto = descriptor_pb2.FileDescriptorProto()
        try:
            proto.ParseFromString(raw)
        except Exception:
            continue
        if proto.name.encode() != hit[2] or not (
            proto.message_type or proto.enum_type or proto.service
        ):
            continue
        if proto.name in records:
            if records[proto.name][1] != raw:
                raise ValueError("conflicting descriptors: " + proto.name)
            continue
        records[proto.name] = (proto, raw, start)

    pool = descriptor_pool.DescriptorPool()
    pending = dict(records)
    loaded = []
    errors = {}
    while pending:
        progress = False
        for name, (proto, raw, _) in list(pending.items()):
            compatible = descriptor_pb2.FileDescriptorProto()
            compatible.CopyFrom(proto)
            if compatible.syntax == "x5msg":
                compatible.syntax = "proto3"
            try:
                pool.Add(compatible)
            except Exception as exc:
                errors[name] = str(exc)
                continue
            loaded.append(name)
            del pending[name]
            errors.pop(name, None)
            progress = True
        if not progress:
            break
    desc_set = descriptor_pb2.FileDescriptorSet()
    normalized_set = descriptor_pb2.FileDescriptorSet()
    manifest = []
    messages = []

    def catalog(msg, parent, filename):
        full = parent + "." + msg.name if parent else msg.name
        messages.append(
            {
                "name": full,
                "file": filename,
                "fields": [
                    json_format.MessageToDict(f, preserving_proto_field_name=True)
                    for f in msg.field
                ],
            }
        )
        for nested in msg.nested_type:
            catalog(nested, full, filename)

    for name in sorted(records):
        proto, raw, offset = records[name]
        desc_set.file.add().CopyFrom(proto)
        normalized = normalized_set.file.add()
        normalized.CopyFrom(proto)
        if normalized.syntax == "x5msg":
            normalized.syntax = "proto3"
        dest = args.output / "proto" / name
        if ".." in Path(name).parts or Path(name).is_absolute():
            raise ValueError(name)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(source(proto), encoding="utf-8")
        meta = args.output / "descriptors" / (name + ".json")
        meta.parent.mkdir(parents=True, exist_ok=True)
        meta.write_text(
            json_format.MessageToJson(proto, preserving_proto_field_name=True),
            encoding="utf-8",
        )
        manifest.append(
            {
                "name": name,
                "package": proto.package,
                "file_offset": hex(offset),
                "size": len(raw),
                "sha256": hashlib.sha256(raw).hexdigest(),
                "messages": len(proto.message_type),
                "enums": len(proto.enum_type),
                "dependencies": list(proto.dependency),
                "pool_validated": name in loaded,
            }
        )
        for msg in proto.message_type:
            catalog(msg, proto.package, name)
    (args.output / "descriptors.pb").write_bytes(desc_set.SerializeToString())
    (args.output / "descriptors.normalized.pb").write_bytes(
        normalized_set.SerializeToString()
    )
    (args.output / "manifest.json").write_text(
        json.dumps(
            {
                "source": str(args.binary),
                "source_sha256": hashlib.sha256(data).hexdigest(),
                "files": manifest,
                "validation_errors": errors,
            },
            indent=2,
        ),
        encoding="utf-8",
    )
    with (args.output / "messages.jsonl").open("w", encoding="utf-8") as f:
        for msg in messages:
            f.write(json.dumps(msg, ensure_ascii=False) + "\n")
    print(
        json.dumps(
            {
                "files": len(records),
                "messages_including_nested": len(messages),
                "pool_validated": len(loaded),
                "unresolved": errors,
            },
            indent=2,
        )
    )
    if pending:
        raise SystemExit(2)


if __name__ == "__main__":
    main()
