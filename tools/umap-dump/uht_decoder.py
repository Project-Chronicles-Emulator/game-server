"""Static decoder of Unreal Header Tool (UHT) registration tables in a module image dump (read-only).

Input: a PE image dump whose file offsets equal RVAs (e.g. dumps/ArcheAgeChronicles.memdump.exe, image base 0x140000000).
It finds every Z_Construct_* singleton function by its fixed prologue
    sub rsp,28h ; mov rax,[rip+s] ; test rax,rax ; jnz ; lea rdx,[rip+params] ; lea rcx,[rip+singleton] ; call Construct*
and classifies it by the callee (ConstructUClass / ConstructUScriptStruct / ConstructUEnum / other).
Nothing is written to the dump; output goes to a caller-chosen directory.
"""
import json
import mmap
import re
import struct
import sys

IMAGE_BASE = 0x140000000

PROLOGUE = re.compile(
    rb"\x48\x83\xec\x28\x48\x8b\x05....\x48\x85\xc0\x75\x1a\x48\x8d\x15(....)\x48\x8d\x0d(....)\xe8(....)",
    re.S,
)


class Image:
    def __init__(self, path):
        self.file = open(path, "rb")
        self.data = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)
        self.size = len(self.data)

    def contains(self, va):
        return IMAGE_BASE <= va < IMAGE_BASE + self.size

    def u64(self, va):
        return struct.unpack_from("<Q", self.data, va - IMAGE_BASE)[0]

    def u32(self, va):
        return struct.unpack_from("<I", self.data, va - IMAGE_BASE)[0]

    def i32(self, va):
        return struct.unpack_from("<i", self.data, va - IMAGE_BASE)[0]

    def u16(self, va):
        return struct.unpack_from("<H", self.data, va - IMAGE_BASE)[0]

    def cstr(self, va, limit=160):
        if not self.contains(va):
            return None
        start = va - IMAGE_BASE
        end = self.data.find(b"\0", start, start + limit)
        return self.data[start:end].decode("ascii", "replace") if end > start else ""

    def wstr(self, va, limit=160):
        if not self.contains(va):
            return None
        start = va - IMAGE_BASE
        chars = []
        for index in range(limit):
            code = struct.unpack_from("<H", self.data, start + 2 * index)[0]
            if code == 0:
                break
            chars.append(chr(code))
        return "".join(chars)


def find_construct_functions(image):
    """Return {function_va: (params_va, singleton_va, callee_va)} for every singleton-construct function."""
    functions = {}
    for match in PROLOGUE.finditer(image.data):
        function_va = IMAGE_BASE + match.start()
        params = function_va + 16 + 7 + struct.unpack("<i", match.group(1))[0]
        singleton = function_va + 23 + 7 + struct.unpack("<i", match.group(2))[0]
        callee = function_va + 30 + 5 + struct.unpack("<i", match.group(3))[0]
        functions[function_va] = (params, singleton, callee)
    return functions


def main():
    image = Image(sys.argv[1])
    functions = find_construct_functions(image)
    callees = {}
    for params, singleton, callee in functions.values():
        callees[callee] = callees.get(callee, 0) + 1
    print(json.dumps({"constructFunctions": len(functions),
                      "byCallee": {hex(k): v for k, v in sorted(callees.items(), key=lambda e: -e[1])[:8]}}))


if __name__ == "__main__":
    main()


# ---------------------------------------------------------------- schema decoding ----------------------------------

CLASS_CALLEE = 0x141BCBBA0
STRUCT_CALLEE = 0x141BCC030
ENUM_CALLEE = 0x141BCBD50
TEXT_END = 0x140000000 + 154800128  # end of .text; code pointers are below this address

TYPE_NAMES = {0: "ByteProperty", 1: "Int8Property", 2: "Int16Property", 3: "IntProperty", 4: "Int64Property",
              5: "UInt16Property", 6: "UInt32Property", 7: "UInt64Property", 0xA: "FloatProperty",
              0xB: "DoubleProperty", 0xC: "BoolProperty", 0xD: "SoftClassProperty", 0xE: "WeakObjectProperty",
              0xF: "LazyObjectProperty", 0x10: "SoftObjectProperty", 0x11: "ClassProperty", 0x12: "ObjectProperty",
              0x13: "InterfaceProperty", 0x14: "NameProperty", 0x15: "StrProperty", 0x16: "ArrayProperty",
              0x17: "MapProperty", 0x18: "SetProperty", 0x19: "StructProperty", 0x1A: "DelegateProperty",
              0x1B: "MulticastInlineDelegateProperty", 0x1C: "MulticastSparseDelegateProperty", 0x1D: "TextProperty",
              0x1E: "EnumProperty", 0x1F: "FieldPathProperty", 0x20: "DoubleProperty"}
LINK_TYPES = {0xD: 0x38, 0xE: 0x38, 0xF: 0x38, 0x10: 0x38, 0x11: 0x38, 0x12: 0x38, 0x13: 0x38, 0x19: 0x38,
              0x1E: 0x38, 0x1A: 0x38, 0x1B: 0x38, 0x1C: 0x38}


def read_property(image, entry_va):
    name = image.cstr(image.u64(entry_va), 100)
    if not name or not name.replace("_", "").isalnum():
        return None
    tag = image.u32(entry_va + 0x18)
    kind = tag & 0x3F
    prop = {"name": name, "propertyFlags": image.u64(entry_va + 0x10), "tag": tag, "type": TYPE_NAMES.get(kind, "Unknown%x" % kind),
            "arrayDim": image.u16(entry_va + 0x30), "offset": image.u16(entry_va + 0x32), "link": None}
    if kind in LINK_TYPES:
        link = image.u64(entry_va + LINK_TYPES[kind])
        prop["link"] = link if link and image.contains(link) else None
    return prop


def read_property_array(image, array_va, limit=400):
    properties = []
    for index in range(limit):
        entry = image.u64(array_va + 8 * index)
        if not image.contains(entry) or entry < TEXT_END:
            break
        prop = read_property(image, entry)
        if prop is None:
            break
        properties.append(prop)
    return properties


class Registry:
    def __init__(self, image):
        self.image = image
        self.functions = find_construct_functions(image)
        self.classes = {fva: p for fva, (p, s, c) in self.functions.items() if c == CLASS_CALLEE}
        self.structs = {fva: p for fva, (p, s, c) in self.functions.items() if c == STRUCT_CALLEE}
        self.enums = {fva: p for fva, (p, s, c) in self.functions.items() if c == ENUM_CALLEE}
        self.class_names = self._class_names()

    def _class_names(self):
        image = self.image
        no_register = {image.u64(params): fva for fva, params in self.classes.items()}
        names = {}
        start = 154800128
        count = (44828276 // 8) * 8
        view = memoryview(image.data)[start:start + count].cast("Q")
        for index, value in enumerate(view):
            fva = no_register.get(value)
            if fva is None:
                continue
            # entry layout seen: {Z_Construct, NoRegister thunk, name wstr, info, ...}; the thunk follows the construct pointer
            pass
        zc_by_thunk = {}
        for fva, params in self.classes.items():
            zc_by_thunk[image.u64(params)] = fva
        zc_set = set(self.classes)
        for index in range(len(view) - 3):
            value = view[index]
            if value in zc_set and view[index + 1] == image.u64(self.classes[value]):
                name = image.wstr(view[index + 2], 80)
                if name and name.isidentifier():
                    names.setdefault(value, name)
        return names

    def class_schema(self, fva):
        image = self.image
        params = self.classes[fva]
        property_array = image.u64(params + 0x28)
        dependencies = []
        dep_array = image.u64(params + 0x18)
        for index in range(64):
            dep = image.u64(dep_array + 8 * index) if dep_array else 0
            if dep not in self.functions:
                break
            dependencies.append(dep)
        return {"function": fva, "name": self.class_names.get(fva), "params": params,
                "properties": read_property_array(image, property_array) if property_array else [], "dependencies": dependencies}

    def struct_schema(self, fva):
        image = self.image
        params = self.structs[fva]
        count = image.u16(params + 0x28)
        array = image.u64(params + 0x20)
        props = read_property_array(image, array, count) if array else []
        return {"function": fva, "name": image.cstr(image.u64(params + 0x18), 100), "super": image.u64(params + 8) or None,
                "numProperties": count, "properties": props}

    def enum_schema(self, fva):
        image = self.image
        params = self.enums[fva]
        array = image.u64(params + 0x20)
        count = image.u16(params + 0x2C)
        values = []
        for index in range(count):
            entry = array + 16 * index
            values.append((image.cstr(image.u64(entry), 120), image.u64(entry + 8)))
        return {"function": fva, "name": image.cstr(image.u64(params + 0x18), 100), "values": values}
