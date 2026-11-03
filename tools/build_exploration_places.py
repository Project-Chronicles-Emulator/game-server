"""Build a searchable exploration guide from hash-verified client map exports.

Positions are static authored anchors, not observed safe player landing positions.
Bounding-box matches are explicitly approximate: grid masks and collision are not modeled.
"""

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path

from build_tutorial_data import BUILD_ID, SOURCE_SHA256, Source, write_profile
from tutorial_spawns import MAP_ROOT, position_mm


SUPPORTED_WORLD = "ArcheWorld"
ANCHOR_FILES = {"discovered_point.json", "npc_spawner.json", "tradingpost.json"}


def position_meters(point):
    return [value / 1000 for value in position_mm(point)]


def world_name(map_row):
    return map_row["world"].rsplit("/", 1)[0].removeprefix("/Game/Maps/")


def category(zone):
    key = zone["zoneKey"].lower()
    if "dungeon" in key:
        return "Dungeon"
    if "housing" in key:
        return "Housing"
    if zone.get("village") == "True" or "village" in key:
        return "Village"
    if "poi" in key:
        return "Landmark"
    if key.startswith("zone_") and key.count("_") == 1:
        return "Region"
    return "Other named area"


def collect_anchors(source, maps):
    layer_maps = defaultdict(set)
    for row in maps:
        for layer in row.get("runtimeDataLayerPath", "[]").strip("[]").split(","):
            if layer:
                layer_maps[(world_name(row), layer.strip())].add(int(row["id"]))

    anchors = []
    boundaries = defaultdict(dict)
    for relative in sorted(source.raw_files):
        path = Path(relative)
        if not path.is_relative_to(MAP_ROOT):
            continue
        parts = path.relative_to(MAP_ROOT).parts
        if path.name == "zone.json":
            world = "/".join(parts[:-1])
            document = json.loads(source.read(Path("raw") / path))
            for zone in document.get("zones", []):
                boundaries[world][zone["key"]] = zone["boundary"]
            continue
        if path.name not in ANCHOR_FILES or "DataLayers" not in parts:
            continue

        layer_index = parts.index("DataLayers")
        world = "/".join(parts[:layer_index])
        layer = parts[-2]
        map_ids = sorted(layer_maps.get((world, layer), set()))
        document = json.loads(source.read(Path("raw") / path))

        def add(point, kind, identity, zone_key=""):
            anchors.append({
                "world": world,
                "position_m": position_meters(point),
                "kind": kind,
                "identity": str(identity),
                "zone_key": zone_key,
                "map_ids": map_ids,
                "source": (Path("raw") / path).as_posix(),
            })

        for point in document.get("discoveredPoints", []):
            add(point["position"], "discovery", point["id"], point["zoneKey"])
        for post in document.get("tradingPosts", []):
            add(post["tradeWagonParkingPos"], "trade parking", post["cmsId"])
        for sector in document.get("spawnerSectors", []):
            for spawner in sector["spawners"]:
                for index, point in enumerate(spawner.get("pointList", {}).get("points", [])):
                    add(point, "NPC spawn", f"{spawner['id']}:{index}")

    return anchors, boundaries


def inside_bounds(anchor, bounds):
    x, y, _ = anchor["position_m"]
    return (bounds["minX"] / 100 <= x <= bounds["maxX"] / 100
            and bounds["minY"] / 100 <= y <= bounds["maxY"] / 100)


def choose_anchor(zone, anchors, boundaries, associated_maps):
    exact = [anchor for anchor in anchors if anchor["zone_key"] == zone["zoneKey"]]
    if exact:
        return exact[0], "authored discovery point"

    # A rectangle alone supplies no height. Choose an actual authored point inside it;
    # this is a heuristic navigation starting point, not proof of polygon membership.
    candidates = []
    for world, zones in boundaries.items():
        bounds = zones.get(zone["zoneKey"])
        if not bounds:
            continue
        center_x = (bounds["minX"] + bounds["maxX"]) / 200
        center_y = (bounds["minY"] + bounds["maxY"]) / 200
        for anchor in anchors:
            if anchor["world"] == world and inside_bounds(anchor, bounds):
                x, y, _ = anchor["position_m"]
                distance = (x - center_x) ** 2 + (y - center_y) ** 2
                candidates.append((distance, anchor["source"], anchor["identity"], anchor))
    if candidates:
        candidates.sort(key=lambda item: item[:3])
        return candidates[0][3], "approximate anchor inside 2D bounds"

    map_ids = {int(row["id"]) for row in associated_maps}
    candidates = [anchor for anchor in anchors if map_ids.intersection(anchor["map_ids"])]
    if candidates:
        return candidates[0], "approximate anchor in associated map layer"
    return None, "no verified 3D anchor"


def place_record(zone, anchor, confidence, associated_maps, zone_names):
    command = None
    if anchor and anchor["world"] == SUPPORTED_WORLD:
        coordinates = " ".join(f"{value:.3f}" for value in anchor["position_m"])
        command = "/gm tp " + coordinates
    origin = zone.get("originZoneKey", "")
    return {
        "zone_id": int(zone["id"]),
        "zone_key": zone["zoneKey"],
        "name": zone["name"].strip(),
        "region": zone_names.get(origin, origin) or zone["name"].strip(),
        "category": category(zone),
        "catalog_active": zone.get("activation") == "True",
        "confidence": confidence,
        "world": anchor["world"] if anchor else "",
        "position_m": anchor["position_m"] if anchor else None,
        "source": anchor["source"] if anchor else "",
        "anchor_kind": anchor["kind"] if anchor else "",
        "anchor_id": anchor["identity"] if anchor else "",
        "map_ids": sorted({int(row["id"]) for row in associated_maps}),
        "command": command,
    }


def build(source):
    zones = source.table("Zone")
    maps = source.table("Map")
    names = {zone["zoneKey"]: zone["name"] for zone in zones}
    anchors, boundaries = collect_anchors(source, maps)
    places = []
    for zone in sorted(zones, key=lambda row: (row["zoneKey"], int(row["id"]))):
        if not zone["name"].strip():
            continue
        associated = [row for row in maps if row.get("FK_zoneKey") == zone["zoneKey"]]
        anchor, confidence = choose_anchor(zone, anchors, boundaries, associated)
        places.append(place_record(zone, anchor, confidence, associated, names))

    posts = {int(row["id"]): row for row in source.table("TradingPost")}
    seen_posts = set()
    for anchor in anchors:
        if anchor["kind"] != "trade parking" or anchor["identity"] in seen_posts:
            continue
        seen_posts.add(anchor["identity"])
        post = posts.get(int(anchor["identity"]), {})
        label = post.get("name") or post.get("comment") or "Trading post " + anchor["identity"]
        zone = {"id": "0", "zoneKey": "TradingPost_" + anchor["identity"], "name": label}
        record = place_record(zone, anchor, "authored trade wagon parking", [], names)
        record["zone_id"] = None
        record["category"] = "Trading post"
        places.append(record)

    return {
        "format_version": 1,
        "build_id": BUILD_ID,
        "source_executable_dump_sha256": SOURCE_SHA256,
        "coordinate_unit": "meters",
        "teleport_requirement": "Current map must be ArcheWorld; use /gm tp_map 9012 first.",
        "validation": "Static export only; landing safety and original-client traversal unverified.",
        "counts": dict(sorted(Counter(place["confidence"] for place in places).items())),
        "places": places,
        "sources": dict(sorted(source.sources.items())),
    }


def render_html(profile):
    template = Path(__file__).with_name("exploration_guide.html").read_text(encoding="utf-8")
    # JSON is embedded as inert script data. Escape '<' so catalog text cannot close the tag.
    data = json.dumps(profile, ensure_ascii=True).replace("<", "\\u003c")
    return template.replace("__EXPLORATION_DATA__", data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("data/generated"))
    parser.add_argument("--output", type=Path,
                        default=Path("data/generated/exploration/places.json"))
    parser.add_argument("--html", type=Path, default=Path("docs/exploration-guide.html"))
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    profile = build(Source(args.source))
    page = render_html(profile)
    if args.check:
        if json.loads(args.output.read_text(encoding="utf-8")) != profile:
            raise SystemExit("Exploration JSON is stale; regenerate the guide")
        if args.html.read_text(encoding="utf-8") != page:
            raise SystemExit("Exploration HTML is stale; regenerate the guide")
    else:
        write_profile(args.output, profile)
        args.html.parent.mkdir(parents=True, exist_ok=True)
        args.html.write_text(page, encoding="utf-8")
    print(json.dumps({
        "places": len(profile["places"]),
        "named_zones": sum(place["zone_id"] is not None for place in profile["places"]),
        "teleport_commands": sum(place["command"] is not None for place in profile["places"]),
        "counts": profile["counts"],
    }))


if __name__ == "__main__":
    main()
