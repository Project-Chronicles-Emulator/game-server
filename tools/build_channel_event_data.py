"""Export channel-event map bindings and tribute altar placements from verified client data.

A map's channel event is Map.FK_zoneKey -> Zone.FK_channelEvent -> ChannelEvent. A tribute altar
is a Doodad with propertyTypeCase Tribute whose phase flow offers an interaction that opens the
channel-event offering UI (InteractionUseOpenUi OpenSharedOfferingForChannelEvent). Altars are
placed from the runtime data-layer spot objects of every map that loads them.

The objectives, tasks and tribute targets of channel events are server-only data: the client
ChannelEvent table holds only IDs and reward links, so none of them is exported here.
"""

import argparse
import json
from pathlib import Path

from build_tutorial_data import BUILD_ID, SOURCE_SHA256, Source, fragment
from build_tutorial_data import spot_unit_id, write_profile
from tutorial_spawns import MAP_ROOT, position_mm


TRIBUTE_PROPERTY = "Tribute"
CHANNEL_EVENT_OFFERING = "OpenSharedOfferingForChannelEvent"


def channel_events(source):
    """Channel event of every map whose zone names one."""
    events = {row["id"]: row for row in source.table("ChannelEvent")}
    zones = {}
    for zone in source.table("Zone"):
        if zone["FK_channelEvent"]:
            if zone["FK_channelEvent"] not in events:
                raise ValueError(f"Zone {zone['zoneKey']} names an unknown channel event")
            zones[zone["zoneKey"]] = zone

    bindings = []
    for map_row in source.table("Map"):
        zone = zones.get(map_row["FK_zoneKey"])
        if not zone:
            continue
        event = events[zone["FK_channelEvent"]]
        bindings.append({
            "map_id": int(map_row["id"]),
            "zone_key": zone["zoneKey"],
            "channel_event_id": int(event["id"]),
            "reward_id": int(event["FK_ChannelEventReward"]) if event["FK_ChannelEventReward"]
            else None,
        })
    return sorted(bindings, key=lambda row: row["map_id"])


def channel_offering_interactions(source):
    return {int(row["FK_interactionId"]) for row in source.table("InteractionUseOpenUi")
            if row["openUiType"] == CHANNEL_EVENT_OFFERING}


def tribute_doodads(source):
    """Doodad CMS ID -> offered item, for altars that open the channel-event offering UI."""
    offerings = channel_offering_interactions(source)
    candidates = {row["id"]: row for row in source.table("Doodad")
                  if row["propertyTypeCase"] == TRIBUTE_PROPERTY}
    interactions = {}
    for flow in source.table("DoodadPhaseFlow"):
        if flow["FK_doodad"] in candidates and flow["triggerTypeCase"] == "Interaction":
            interactions.setdefault(flow["FK_doodad"], set()).add(
                fragment(flow["triggerType"])["id"])

    altars = {}
    for doodad_id, doodad in candidates.items():
        if not interactions.get(doodad_id, set()) & offerings:
            continue
        item_cms_id = fragment(doodad["propertyType"]).get("itemCmsId")
        if not isinstance(item_cms_id, int) or item_cms_id <= 0:
            raise ValueError(f"Tribute doodad {doodad_id} names no offered item")
        source.row("Item", item_cms_id)
        altars[int(doodad_id)] = item_cms_id
    return altars


def map_layers(map_row):
    world = map_row["world"].rsplit("/", 1)[0].removeprefix("/Game/Maps/")
    layers = map_row["runtimeDataLayerPath"].strip("[]")
    return [MAP_ROOT / world / "DataLayers/Runtime" / layer / "spot_object.json"
            for layer in layers.split(",") if layer]


def altar_placements(source, altars):
    cached_spots = {}
    placements = {}
    for map_row in source.table("Map"):
        for relative in map_layers(map_row):
            if relative.as_posix() not in source.raw_files:
                continue
            if relative not in cached_spots:
                cached_spots[relative] = json.loads(source.read(Path("raw") / relative))

            for sector in cached_spots[relative]["sectors"]:
                for spot in sector["spotObjects"]:
                    for kind in spot["types"]:
                        doodad_cms_id = kind.get("doodad", {}).get("cmsId")
                        if doodad_cms_id not in altars:
                            continue
                        placement = {
                            "map_id": int(map_row["id"]),
                            "unit_id": spot_unit_id(sector["sectorIndex"], spot["id"]),
                            "doodad_cms_id": doodad_cms_id,
                            "item_cms_id": altars[doodad_cms_id],
                            "position": position_mm(spot["position"]),
                        }
                        key = (placement["map_id"], placement["unit_id"])
                        if key in placements and placements[key] != placement:
                            raise ValueError(f"Conflicting tribute altar placement: {key}")
                        placements[key] = placement
    return sorted(placements.values(), key=lambda row: (row["map_id"], int(row["unit_id"])))


def build(source):
    altars = tribute_doodads(source)
    return {
        "format_version": 1,
        "build_id": BUILD_ID,
        "source_executable_dump_sha256": SOURCE_SHA256,
        "channel_events": channel_events(source),
        "tribute_altars": altar_placements(source, altars),
        "sources": dict(source.sources),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("data/generated"))
    parser.add_argument("--output", type=Path,
                        default=Path("data/generated/channel-events/definitions.json"))
    args = parser.parse_args()
    profile = build(Source(args.source))
    write_profile(args.output, profile)
    print(json.dumps({"channel_event_maps": len(profile["channel_events"]),
                      "tribute_altars": len(profile["tribute_altars"])}))


if __name__ == "__main__":
    main()
