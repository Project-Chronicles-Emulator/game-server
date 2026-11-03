"""Export the level trigger volumes of one world from the verified client data.

Source chain, all read through the hash-checked Source:
    Maps/<World>/DataLayers/{Runtime,Static}/<Layer>/level_trigger_volume.json
    Map.runtimeDataLayerPath -> which maps list a Runtime layer

The export carries no player, session or capture values. The wire id of a volume is the
little-endian split of its GUID, the same pair the client sends as trigger_id_x/y in
PC2S_LevelTriggerReq (20084). The arrival position of a map move is not derived: a
channelSession volume names a destination map and a spawnPositionId, and no table or layer file
in the export resolves that id, so it is reported as authored and nothing more.
"""

import argparse
import hashlib
import json
import sys
from pathlib import Path

from tutorial_spawns import MAP_ROOT, wire_guid


FORMAT_VERSION = 1
DEFAULT_WORLD = "ArcheWorld"
DEFAULT_MAP_IDS = (9011, 9012)
LAYER_GROUPS = ("Runtime", "Static")
TRIGGER_FILE = "level_trigger_volume.json"

# Destination blocks a volume can carry; a volume with none of them is a cutscene, doodad or
# dialogue volume whose task lives in the LevelTriggerVolume catalog table.
KIND_CHANNEL_SESSION = "channelSession"
KIND_CHANNEL = "channel"
KIND_PORTAL = "portal"
KIND_NORMAL = "normal"
KIND_NONE = "none"
KIND_PRIORITY = (KIND_CHANNEL_SESSION, KIND_CHANNEL, KIND_PORTAL, KIND_NORMAL)

SHAPE_BOX = "box"
SHAPE_SPHERE = "sphere"
SHAPES = (SHAPE_BOX, SHAPE_SPHERE)

RULES = {
    "wire_id": "trigger_id_wire is [x, y], decimal uint64 strings: the little-endian halves of "
               "the volume GUID, as sent in PC2S_LevelTriggerReq. Verified against the captured "
               "requests by the tests.",
    "kind": "channelSession: destination map and spawnPositionId authored. channel: only a map "
            "id, and the one observed transition of this kind went to a different map than the "
            "authored id (Candidate: region id; the rule is Unknown). portal: a Portal row id. "
            "normal: a trigger action, no map. none: no destination block.",
    "arrival_position": "Not derived. spawn_position_id is reported as authored (0 for most "
                        "volumes) and no table or layer file resolves it: Unknown.",
    "geometry": "Shapes and numbers exactly as authored in the layer file; the unit is "
                "centimeters like the NPC placements (Candidate: not checked against a capture).",
    "membership": "listed_by_map_ids is exact: the Map rows whose runtimeDataLayerPath names the "
                  "Runtime layer. Static volumes are never listed, and the volumes of the "
                  "captured 9012 transitions sit in layers that no 9011/9012 row lists "
                  "(Observed), so use is_requested_map_layer only as a filter, not as proof "
                  "that a volume belongs to a map.",
}


def layer_names(map_row):
    return [name for name in map_row["runtimeDataLayerPath"].strip("[]").split(",") if name]


def world_name(map_row):
    return map_row["world"].rsplit("/", 1)[1].split(".")[0]


def maps_listing_layers(source, world):
    listing = {}
    for row in source.table("Map"):
        if world_name(row) != world:
            continue
        for layer in layer_names(row):
            listing.setdefault(layer, set()).add(int(row["id"]))
    return listing


def volume_kind(volume, context):
    kinds = [kind for kind in KIND_PRIORITY if kind in volume]
    if len(kinds) > 1:
        raise ValueError(f"Trigger volume has several destination blocks {kinds}: {context}")
    return kinds[0] if kinds else KIND_NONE


def volume_shape(volume, context):
    shapes = [shape for shape in SHAPES if shape in volume]
    if len(shapes) != 1:
        raise ValueError(f"Trigger volume needs exactly one shape, found {shapes}: {context}")
    return shapes[0], volume[shapes[0]]


def build_volume(volume, layer, group, listing, requested_map_ids):
    context = f"volume {volume.get('id')} in {group}/{layer}"
    guid = volume["id"]
    kind = volume_kind(volume, context)
    shape, geometry = volume_shape(volume, context)
    listed_by = sorted(listing.get(layer, ())) if group == "Runtime" else []

    entry = {
        "trigger_id_wire": wire_guid(guid),
        "trigger_guid": guid,
        "is_activate": bool(volume["isActivate"]),
        "kind": kind,
        "destination_map_id": None,
        "spawn_position_id": None,
        "shape": shape,
        "geometry": geometry,
        "source_layer": layer,
        "layer_group": group,
        "listed_by_map_ids": listed_by,
        "is_requested_map_layer": bool(set(listed_by) & set(requested_map_ids)),
    }
    if kind in (KIND_CHANNEL_SESSION, KIND_CHANNEL):
        entry["destination_map_id"] = int(volume[kind]["mapId"])
    if kind == KIND_CHANNEL_SESSION:
        entry["spawn_position_id"] = int(volume[kind]["spawnPositionId"])
    if kind == KIND_PORTAL:
        entry["portal_cms_id"] = int(volume[kind]["cmsId"])
    if kind == KIND_NORMAL:
        entry["action_type"] = volume[kind]["actionType"]
    return entry


def layer_files(source, world):
    """Yield (group, layer, raw relative path) for every trigger file of the world, sorted."""
    prefix = (MAP_ROOT / world / "DataLayers").as_posix() + "/"
    found = []
    for path in source.raw_files:
        if not path.startswith(prefix) or not path.endswith("/" + TRIGGER_FILE):
            continue
        parts = path[len(prefix):].split("/")
        if len(parts) != 3 or parts[0] not in LAYER_GROUPS:
            raise ValueError(f"Unexpected trigger file location: {path}")
        found.append((parts[0], parts[1], path))
    return sorted(found)


def build_trigger_export(source, world=DEFAULT_WORLD, requested_map_ids=DEFAULT_MAP_IDS):
    listing = maps_listing_layers(source, world)
    volumes = []
    seen_guids = {}
    files = layer_files(source, world)
    if not files:
        raise ValueError(f"No {TRIGGER_FILE} is present for world {world}")

    for group, layer, path in files:
        document = json.loads(source.read(Path("raw") / path))
        for volume in document["triggerVolumes"]:
            entry = build_volume(volume, layer, group, listing, requested_map_ids)
            guid = entry["trigger_guid"]
            if guid in seen_guids:
                raise ValueError(f"Duplicate trigger GUID {guid} in {group}/{layer} "
                                 f"and {seen_guids[guid]}")
            seen_guids[guid] = f"{group}/{layer}"
            volumes.append(entry)

    volumes.sort(key=lambda item: (item["layer_group"], item["source_layer"],
                                   item["trigger_guid"]))
    return {
        "format_version": FORMAT_VERSION,
        "world": world,
        "requested_map_ids": sorted(requested_map_ids),
        "rules": RULES,
        "layer_files": len(files),
        "volume_total": len(volumes),
        "counts": summarize(volumes, requested_map_ids),
        "volumes": volumes,
    }


def summarize(volumes, requested_map_ids):
    by_kind = {}
    for volume in volumes:
        by_kind[volume["kind"]] = by_kind.get(volume["kind"], 0) + 1

    requested = [volume for volume in volumes if volume["is_requested_map_layer"]]
    to_requested = [volume for volume in volumes
                    if volume["destination_map_id"] in requested_map_ids]
    return {
        "by_kind": dict(sorted(by_kind.items())),
        "inactive": sum(1 for volume in volumes if not volume["is_activate"]),
        "in_requested_map_layers": len(requested),
        "destination_is_requested_map": len(to_requested),
        "destination_is_requested_map_by_kind": count_kinds(to_requested),
    }


def count_kinds(volumes):
    counts = {}
    for volume in volumes:
        counts[volume["kind"]] = counts.get(volume["kind"], 0) + 1
    return dict(sorted(counts.items()))


def render(document):
    return json.dumps(document, indent=2, sort_keys=False) + "\n"


def write_document(path, document):
    path.parent.mkdir(parents=True, exist_ok=True)
    data = render(document).encode("utf-8")
    path.write_bytes(data)
    return hashlib.sha256(data).hexdigest()


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("data/generated"))
    parser.add_argument("--world", default=DEFAULT_WORLD)
    parser.add_argument("--map-id", type=int, action="append", dest="map_ids")
    parser.add_argument("--output-dir", type=Path, default=Path("data/generated/triggers"))
    options = parser.parse_args(arguments)

    from build_tutorial_data import Source

    source = Source(options.source)
    document = build_trigger_export(
        source, options.world, tuple(options.map_ids or DEFAULT_MAP_IDS))
    document["source_sha256"] = {name: digest for name, digest in sorted(source.sources.items())}
    output = options.output_dir / f"{options.world}-level-triggers.json"
    digest = write_document(output, document)
    print(json.dumps({
        "output": output.as_posix(),
        "sha256": digest,
        "layer_files": document["layer_files"],
        "volumes": document["volume_total"],
        "counts": document["counts"],
    }))
    return 0


if __name__ == "__main__":
    sys.exit(main())
