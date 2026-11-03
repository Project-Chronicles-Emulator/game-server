"""Writes a CUE4Parse-readable .usmap (version 3: long names, large enums, no compression) from decoded UHT tables."""
import struct
import sys

import uht_decoder as uht

USMAP_TYPES = {"ByteProperty": 0, "BoolProperty": 1, "IntProperty": 2, "FloatProperty": 3, "ObjectProperty": 4,
               "NameProperty": 5, "DelegateProperty": 6, "DoubleProperty": 7, "ArrayProperty": 8, "StructProperty": 9,
               "StrProperty": 10, "TextProperty": 11, "InterfaceProperty": 12, "MulticastDelegateProperty": 13,
               "WeakObjectProperty": 14, "LazyObjectProperty": 15, "AssetObjectProperty": 16,
               "SoftObjectProperty": 17, "UInt64Property": 18, "UInt32Property": 19, "UInt16Property": 20,
               "Int64Property": 21, "Int16Property": 22, "Int8Property": 23, "MapProperty": 24, "SetProperty": 25,
               "EnumProperty": 26, "FieldPathProperty": 27}
UHT_TO_USMAP = {"ClassProperty": "ObjectProperty", "SoftClassProperty": "SoftObjectProperty",
                "MulticastInlineDelegateProperty": "MulticastDelegateProperty",
                "MulticastSparseDelegateProperty": "MulticastDelegateProperty"}


class Names:
    def __init__(self):
        self.index = {}
        self.list = []

    def add(self, text):
        if text not in self.index:
            self.index[text] = len(self.list)
            self.list.append(text)
        return self.index[text]


def top_level_properties(properties):
    """UHT lists inner properties (array/set element, map key/value, enum underlying) immediately BEFORE their
    container; walk backwards, attach the inner entries to the container, return declaration order."""
    result = []
    i = len(properties) - 1
    while i >= 0:
        prop = properties[i]
        kind = prop["type"]
        consumed = 0
        inner = []
        if kind in ("ArrayProperty", "SetProperty", "EnumProperty"):
            consumed = 1
        elif kind == "MapProperty":
            consumed = 2
        for k in range(consumed):
            if i - 1 - k >= 0:
                inner.append(properties[i - 1 - k])
        prop = dict(prop, inner=inner)
        result.append(prop)
        i -= 1 + consumed
    result.reverse()
    return result


def write_type(out, names, prop, enum_name_of, struct_name_of):
    kind = UHT_TO_USMAP.get(prop["type"], prop["type"])
    out.append(struct.pack("<B", USMAP_TYPES.get(kind, 4)))
    if kind == "EnumProperty":
        underlying = prop["inner"][0] if prop["inner"] else {"type": "ByteProperty"}
        out.append(struct.pack("<B", USMAP_TYPES.get(UHT_TO_USMAP.get(underlying["type"], underlying["type"]), 0)))
        out.append(struct.pack("<I", names.add(enum_name_of(prop.get("link")))))
    elif kind == "StructProperty":
        out.append(struct.pack("<I", names.add(struct_name_of(prop.get("link")))))
    elif kind in ("ArrayProperty", "SetProperty"):
        inner = prop["inner"][0] if prop["inner"] else {"type": "ObjectProperty", "inner": [], "link": None}
        write_type(out, names, dict(inner, inner=inner.get("inner", [])), enum_name_of, struct_name_of)
    elif kind == "MapProperty":
        value = prop["inner"][0] if prop["inner"] else {"type": "ObjectProperty", "inner": [], "link": None}
        key = prop["inner"][1] if len(prop["inner"]) > 1 else {"type": "NameProperty", "inner": [], "link": None}
        write_type(out, names, dict(key, inner=key.get("inner", [])), enum_name_of, struct_name_of)
        write_type(out, names, dict(value, inner=value.get("inner", [])), enum_name_of, struct_name_of)


def build(registry, path):
    image = registry.image
    names = Names()
    enum_cache = {fva: registry.enum_schema(fva) for fva in registry.enums}
    struct_cache = {fva: registry.struct_schema(fva) for fva in registry.structs}
    enum_name_of = lambda link: (enum_cache[link]["name"] if link in enum_cache else "None")
    struct_name_of = lambda link: (struct_cache[link]["name"] if link in struct_cache else "None")

    enum_blob = []
    for fva, enum in enum_cache.items():
        enum_blob.append(struct.pack("<IH", names.add(enum["name"]), len(enum["values"])))
        for entry_name, _ in enum["values"]:
            enum_blob.append(struct.pack("<I", names.add(entry_name or "None")))

    schema_blob = []
    schema_count = 0

    def add_schema(name, super_name, properties):
        nonlocal schema_count
        props = top_level_properties(properties)
        body = []
        body.append(struct.pack("<I", names.add(name)))
        body.append(struct.pack("<I", names.add(super_name) if super_name else 0xFFFFFFFF))
        body.append(struct.pack("<HH", len(props), len(props)))
        for index, prop in enumerate(props):
            body.append(struct.pack("<HB", index, max(1, prop["arrayDim"])))
            body.append(struct.pack("<I", names.add(prop["name"])))
            write_type(body, names, prop, enum_name_of, struct_name_of)
        schema_blob.extend(body)
        schema_count += 1

    class_super = {}
    for fva in registry.classes:
        schema = registry.class_schema(fva)
        parent = next((d for d in schema["dependencies"][:1] if d in registry.classes), None)
        class_super[fva] = registry.class_names.get(parent)
    for fva in registry.classes:
        schema = registry.class_schema(fva)
        short = (schema["name"] or "Unknown")[1:]
        super_short = class_super[fva][1:] if class_super[fva] else None
        add_schema(short, super_short, schema["properties"])
    for fva, schema in struct_cache.items():
        parent = schema["super"]
        add_schema(schema["name"], struct_cache[parent]["name"] if parent in struct_cache else None, schema["properties"])

    name_blob = [struct.pack("<I", len(names.list))]
    for text in names.list:
        raw = text.encode("utf-8")[:65535]
        name_blob.append(struct.pack("<H", len(raw)) + raw)
    payload = b"".join(name_blob) + struct.pack("<I", len(enum_cache)) + b"".join(enum_blob) \
        + struct.pack("<I", schema_count) + b"".join(schema_blob)
    header = struct.pack("<HBIBII", 0x30C4, 3, 0, 0, len(payload), len(payload))
    with open(path, "wb") as handle:
        handle.write(header + payload)
    return {"names": len(names.list), "enums": len(enum_cache), "schemas": schema_count, "bytes": len(header) + len(payload)}


if __name__ == "__main__":
    image = uht.Image(sys.argv[1])
    print(build(uht.Registry(image), sys.argv[2]))
