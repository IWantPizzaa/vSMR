#!/usr/bin/env python3
"""Build the offline LFPG data set from pinned OSM, AVISO and Ramp Agent sources.

No nearest-neighbour connectors are created. Graph edges are original OSM
segments, joined only by OSM node identity or identical source coordinates.
Requires Python 3.10+ and Shapely 2 (pip install -r scripts/requirements-data.txt).
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import shutil
import urllib.parse
import urllib.request

from shapely.geometry import LineString, Polygon
from shapely.strtree import STRtree

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / "data-sources"
OUT = ROOT / "public" / "data"
ORIGIN = (2.5479, 49.0097)
SCALE_X = 111320 * math.cos(math.radians(ORIGIN[1]))
RAMP_COMMIT = "63513c01e3989d6f1f02b3fa366201939be3cb5c"
RAMP_URL = f"https://raw.githubusercontent.com/vaccfr/RampAgent-Config/{RAMP_COMMIT}/airports/LFPG.json"
QUERY = '[out:json][timeout:180];(way["aeroway"~"^(taxiway|parking_position|runway)$"](48.983,2.485,49.032,2.623);node["aeroway"~"^(parking_position|holding_position)$"](48.983,2.485,49.032,2.623););out body;>;out skel qt;'


def xy(lon: float, lat: float) -> tuple[float, float]:
    return ((lon - ORIGIN[0]) * SCALE_X, (lat - ORIGIN[1]) * 111320)


def point(lon: float, lat: float) -> dict:
    x, y = xy(lon, lat)
    return {"x": x, "y": y, "lat": lat, "lon": lon}


def node_true_intersections(nodes: dict[str, dict], edges: list[dict], ways: list[dict]) -> tuple[list[dict], list[dict], list[dict]]:
    """Split existing segments at actual intersections; never draw new links.

    Differently tagged OSM layers remain separate. Shared source nodes already
    describe junctions and require no extra work. Micrometre coordinate keys
    absorb floating-point intersection arithmetic only (not mapping gaps).
    """
    shapes = [LineString([(nodes[e["from"]]["x"], nodes[e["from"]]["y"]), (nodes[e["to"]]["x"], nodes[e["to"]]["y"])]) for e in edges]
    tree = STRtree(shapes)
    way_layers = {w["id"]: w["tags"].get("layer", "0") for w in ways}
    coordinate_nodes = {(round(n["x"], 6), round(n["y"], 6)): n["id"] for n in nodes.values()}
    splits: dict[int, dict[str, float]] = defaultdict(dict)
    reports, separated = [], []
    for i, shape in enumerate(shapes):
        for j in sorted(int(value) for value in tree.query(shape) if int(value) > i):
            a, b = edges[i], edges[j]
            if {a["from"], a["to"]} & {b["from"], b["to"]}:
                continue
            intersection = shape.intersection(shapes[j])
            if intersection.is_empty:
                continue
            if way_layers[a["sourceWayId"]] != way_layers[b["sourceWayId"]]:
                separated.append({"edges": [a["id"], b["id"]], "reason": "different_source_layers"})
                continue
            if intersection.geom_type == "Point":
                points = [intersection]
            elif intersection.geom_type == "MultiPoint":
                points = list(intersection.geoms)
            elif intersection.geom_type == "LineString":
                points = list(intersection.boundary.geoms)
            else:
                continue
            for position in points:
                key = (round(position.x, 6), round(position.y, 6))
                node_id = coordinate_nodes.get(key)
                if node_id is None:
                    node_id = f"intersection:{len(reports)}"
                    coordinate_nodes[key] = node_id
                    nodes[node_id] = {"id": node_id, "x": position.x, "y": position.y, "lon": ORIGIN[0] + position.x / SCALE_X, "lat": ORIGIN[1] + position.y / 111320, "source": "exact_intersection_of_osm_source_segments"}
                for index in [i, j]:
                    distance = shapes[index].project(position)
                    if 0.000001 < distance < shapes[index].length - 0.000001:
                        splits[index][node_id] = distance
                reports.append({"nodeId": node_id, "x": position.x, "y": position.y, "sourceSegments": [a["id"], b["id"]]})
    result = []
    for i, edge in enumerate(edges):
        if i not in splits:
            result.append(edge)
            continue
        points = [(edge["from"], 0), *sorted(splits[i].items(), key=lambda pair: pair[1]), (edge["to"], shapes[i].length)]
        for part, ((start, start_distance), (end, end_distance)) in enumerate(zip(points, points[1:])):
            if start == end or end_distance - start_distance <= 0.000001:
                continue
            result.append({**edge, "id": f"{edge['id']}:part{part}", "sourceSegmentId": edge["id"], "from": start, "to": end, "length": end_distance - start_distance})
    return result, reports, separated


def download(url: str, destination: Path, data: bytes | None = None) -> None:
    request = urllib.request.Request(url, data=data, headers={"User-Agent": "SmartGround-LFPG-offline-import/1.0"})
    with urllib.request.urlopen(request, timeout=210) as response:
        raw = response.read()
    json.loads(raw)  # Do not replace a valid snapshot with an HTML error page.
    destination.write_bytes(raw)


def components(node_ids: list[str], edges: list[dict], allow_runways: bool = True) -> tuple[list[dict], dict[str, int]]:
    adjacency: dict[str, set[str]] = defaultdict(set)
    for edge in edges:
        if not allow_runways and edge["kind"] == "runway":
            continue
        adjacency[edge["from"]].add(edge["to"])
        adjacency[edge["to"]].add(edge["from"])
    unseen = set(node_ids)
    groups = []
    while unseen:
        stack = [min(unseen)]
        unseen.remove(stack[0])
        group = []
        while stack:
            node = stack.pop()
            group.append(node)
            for neighbour in adjacency[node]:
                if neighbour in unseen:
                    unseen.remove(neighbour)
                    stack.append(neighbour)
        groups.append(sorted(group))
    groups.sort(key=lambda group: (-len(group), group[0]))
    index = {node: i for i, group in enumerate(groups) for node in group}
    reports = [{"id": i, "nodeCount": len(group), "exampleNodes": group[:8]} for i, group in enumerate(groups)]
    return reports, index


def build() -> dict:
    osm_path = SOURCES / "lfpg-osm.json"
    aviso_path = SOURCES / "lfpg-aviso.geojson"
    ramp_path = SOURCES / "lfpg-ramp-agent.json"
    osm = json.loads(osm_path.read_text(encoding="utf-8"))
    aviso = json.loads(aviso_path.read_text(encoding="utf-8"))
    ramp = json.loads(ramp_path.read_text(encoding="utf-8"))
    # Recursive Overpass output can repeat a tagged node later as a skeleton.
    # Merge records so the skeleton never erases parking/holding tags.
    source_nodes = {}
    for element in osm["elements"]:
        if element["type"] == "node":
            previous = source_nodes.get(element["id"], {})
            source_nodes[element["id"]] = {**previous, **element, "tags": {**previous.get("tags", {}), **element.get("tags", {})}}
    ways = [e for e in osm["elements"] if e["type"] == "way"]
    source_kind_counts = Counter(w["tags"]["aeroway"] for w in ways)

    # AVISO provides display surfaces and independent building-interior checks.
    surfaces, labels, reference_lines = [], [], []
    building_polygons = []
    surface_kinds = {"Buildings": "building", "Runways": "runway", "Taxiway Orange": "taxiway", "Hard Surface 2": "apron", "Hard Surface 3": "apron", "Hard Surface 4": "apron", "Grass": "grass", "Grass runways": "grass", "Terrain 1": "terrain", "Closure areas": "closure", "Stopbars": "stopbar"}
    for feature in aviso["features"]:
        props, geometry = feature["properties"], feature["geometry"]
        category = props.get("category", "")
        if geometry["type"] == "Polygon":
            rings = [[tuple(round(value, 3) for value in xy(*position)) for position in ring] for ring in geometry["coordinates"]]
            surfaces.append({"kind": surface_kinds.get(category, "apron"), "name": props.get("name", category), "points": rings[0], "holes": rings[1:], "sourceId": feature["id"]})
            if category == "Buildings":
                polygon = Polygon(rings[0], rings[1:])
                if not polygon.is_valid:
                    polygon = polygon.buffer(0)
                # One metre inset avoids falsely rejecting edge-touching lead-ins.
                interior = polygon.buffer(-1)
                if not interior.is_empty:
                    building_polygons.append(interior)
        elif geometry["type"] == "Point":
            labels.append({"name": props.get("text-field", props.get("name", "")), "kind": {"Gates": "stand", "Terminaux": "terminal", "Taxiways": "taxiway"}.get(category, "annotation"), **point(*geometry["coordinates"])})
        elif geometry["type"] == "MultiLineString" and props.get("geometry_role") != "directional_arrows":
            # Never treat AVISO limit lines or arbitrary annotations as taxi routes.
            for i, line in enumerate(geometry["coordinates"]):
                reference_lines.append({"id": f"{feature['id']}:{i}", "kind": "aviso_reference", "name": props.get("name", ""), "points": [[round(v, 3) for v in xy(*p)] for p in line], "routingExcluded": True})
    building_index = STRtree(building_polygons)

    # Coordinate identity is exact to source precision, never a proximity join.
    used_source_ids = {node for way in ways for node in way["nodes"]}
    canonical: dict[tuple[float, float], str] = {}
    node_id_map: dict[int, str] = {}
    nodes_by_id = {}
    identical_merges = 0
    for source_id in sorted(used_source_ids):
        source = source_nodes[source_id]
        coord = (source["lon"], source["lat"])
        if coord in canonical:
            node_id_map[source_id] = canonical[coord]
            identical_merges += 1
        else:
            node_id = f"osm:{source_id}"
            canonical[coord] = node_id
            node_id_map[source_id] = node_id
            nodes_by_id[node_id] = {"id": node_id, **point(*coord)}
    edges, rejected_segments, duplicate_segments = [], [], 0
    edge_keys = set()
    node_kinds: dict[str, set[str]] = defaultdict(set)
    adjacency: dict[str, set[str]] = defaultdict(set)
    way_graph_nodes: dict[int, list[str]] = {}
    for way in ways:
        kind = way["tags"]["aeroway"]
        way_graph_nodes[way["id"]] = [node_id_map[n] for n in way["nodes"]]
        for i, (a, b) in enumerate(zip(way["nodes"], way["nodes"][1:])):
            start, end = node_id_map[a], node_id_map[b]
            pa, pb = nodes_by_id[start], nodes_by_id[end]
            length = math.hypot(pa["x"] - pb["x"], pa["y"] - pb["y"])
            if length < 0.001:
                continue
            key = (min(start, end), max(start, end), kind)
            if key in edge_keys:
                duplicate_segments += 1
                continue
            edge_keys.add(key)
            shape = LineString([(pa["x"], pa["y"]), (pb["x"], pb["y"])])
            penetration = max((shape.intersection(building_polygons[int(index)]).length for index in building_index.query(shape)), default=0)
            name = way["tags"].get("ref", way["tags"].get("name", ""))
            if penetration > 1:
                rejected_segments.append({"wayId": way["id"], "segment": i, "kind": kind, "name": name, "from": start, "to": end, "buildingInteriorMetres": round(penetration, 2), "reason": "intersects_aviso_building_interior"})
                reference_lines.append({"id": f"excluded-osm:{way['id']}:{i}", "kind": "excluded", "name": name, "points": [[pa["x"], pa["y"]], [pb["x"], pb["y"]]], "routingExcluded": True})
                continue
            edge = {"id": f"osm:{way['id']}:{i}", "from": start, "to": end, "length": length, "name": name or ("Stand lead-in" if kind == "parking_position" else "Taxiway" if kind == "taxiway" else "Runway"), "kind": kind, "sourceWayId": way["id"]}
            edges.append(edge)
            adjacency[start].add(end)
            adjacency[end].add(start)
            node_kinds[start].add(kind)
            node_kinds[end].add(kind)

    edges, intersection_reports, separated_crossings = node_true_intersections(nodes_by_id, edges, ways)
    # Rebuild incidence after noding, before determining stand-side endpoints.
    adjacency = defaultdict(set)
    node_kinds = defaultdict(set)
    for edge in edges:
        adjacency[edge["from"]].add(edge["to"])
        adjacency[edge["to"]].add(edge["from"])
        node_kinds[edge["from"]].add(edge["kind"])
        node_kinds[edge["to"]].add(edge["kind"])

    locations: dict[str, dict] = {}
    candidates: dict[str, list[tuple[str, str]]] = defaultdict(list)
    # A named OSM stand point inside a source way is the strongest stand position.
    for source in source_nodes.values():
        tags = source.get("tags", {})
        if tags.get("aeroway") == "parking_position" and tags.get("ref") and source["id"] in node_id_map:
            candidates[tags["ref"].strip().upper()].append((node_id_map[source["id"]], f"OSM node {source['id']}"))
    for way in ways:
        tags = way["tags"]
        if tags["aeroway"] != "parking_position" or not tags.get("ref"):
            continue
        ends = [way_graph_nodes[way["id"]][0], way_graph_nodes[way["id"]][-1]]
        # Select the source endpoint that is least attached to the taxi network.
        terminal = min(ends, key=lambda n: ("taxiway" in node_kinds[n], len(adjacency[n])))
        candidates[tags["ref"].strip().upper()].append((terminal, f"OSM parking way {way['id']} endpoint"))

    def add_stand(name: str, lon: float, lat: float, source: str, allow_unmatched: bool = True) -> None:
        name = name.strip().upper()
        if not name:
            return
        position = point(lon, lat)
        matching = candidates.get(name, [])
        candidate = min(matching, key=lambda entry: math.hypot(nodes_by_id[entry[0]]["x"] - position["x"], nodes_by_id[entry[0]]["y"] - position["y"]), default=None)
        match_distance = math.inf if candidate is None else math.hypot(nodes_by_id[candidate[0]]["x"] - position["x"], nodes_by_id[candidate[0]]["y"] - position["y"])
        # A name match is insufficient if the two source records disagree spatially.
        routable = candidate is not None and match_distance <= 60 and bool(adjacency[candidate[0]])
        entry = {"id": f"stand:{name}", "name": name, "kind": "stand", "nodeId": candidate[0] if routable else None, **({k: v for k, v in nodes_by_id[candidate[0]].items() if k != "id"} if routable else position), "source": source, "sourcePosition": position, "sources": [source], "matchDistanceMetres": round(match_distance, 2) if routable else None}
        if routable:
            entry["graphSource"] = candidate[1]
        else:
            entry["unavailableReason"] = "No spatially consistent, identically named OSM stand lead-in; no connector invented."
        previous = locations.get(entry["id"])
        if previous:
            previous["sources"].append(source)
            if previous["nodeId"] is None and routable:
                entry["sources"] = previous["sources"]
                locations[entry["id"]] = entry
        elif allow_unmatched or routable:
            locations[entry["id"]] = entry

    # Use independently tagged stand points, then add every AVISO and Ramp record.
    for source in source_nodes.values():
        tags = source.get("tags", {})
        if tags.get("aeroway") == "parking_position" and tags.get("ref"):
            add_stand(tags["ref"], source["lon"], source["lat"], f"OSM node {source['id']}")
    for name, matches in candidates.items():
        if f"stand:{name}" not in locations:
            node = nodes_by_id[matches[0][0]]
            add_stand(name, node["lon"], node["lat"], matches[0][1])
    for label in labels:
        if label["kind"] == "stand":
            add_stand(label["name"], label["lon"], label["lat"], "vSMR bundled AVISO gate label")
    for name, stand in ramp["Stands"].items():
        lat, lon, *_ = map(float, stand["Coordinates"].split(":"))
        add_stand(name, lon, lat, f"Ramp Agent LFPG config {RAMP_COMMIT}")
        locations[f"stand:{name.strip().upper()}"]["terminal"] = stand.get("Terminal", "")

    for source in source_nodes.values():
        tags = source.get("tags", {})
        if tags.get("aeroway") != "holding_position":
            continue
        node_id = node_id_map.get(source["id"])
        locations[f"hold:{source['id']}"] = {"id": f"hold:{source['id']}", "name": tags.get("ref", tags.get("name", f"HP {source['id']}")), "kind": "holding_point", "nodeId": node_id if node_id and adjacency[node_id] else None, **point(source["lon"], source["lat"]), "source": f"OSM holding_position node {source['id']}"}

    runway_access = []
    # OSM way direction is unrelated to the order of runway reference names.
    # Use the explicitly named AVISO thresholds to identify physical ends.
    named_thresholds = {name: position for runway in aviso["metadata"]["runways"] for name, position in zip(runway["ends"], runway["thresholds"])}
    for way in ways:
        tags = way["tags"]
        if tags["aeroway"] != "runway" or "/" not in tags.get("ref", ""):
            continue
        names = tags["ref"].split("/")
        source_order = way_graph_nodes[way["id"]]
        access = [node for node in source_order if "taxiway" in node_kinds[node]]
        for name in names:
            # This is an actual taxiway/runway intersection, NOT a runway threshold.
            if name not in named_thresholds:
                raise ValueError(f"No explicitly named source threshold for {name}")
            threshold = point(*named_thresholds[name])
            node_id = min(access, key=lambda n: math.hypot(nodes_by_id[n]["x"] - threshold["x"], nodes_by_id[n]["y"] - threshold["y"]), default=None)
            node = nodes_by_id[node_id] if node_id else threshold
            location = {"id": f"runway:{name}", "name": name, "kind": "runway", "nodeId": node_id, **{k: v for k, v in node.items() if k != "id"}, "source": f"OSM runway {way['id']} / shared taxiway node", "accessType": "runway_taxiway_intersection", "thresholdPosition": threshold, "thresholdSource": "explicitly named AVISO metadata runway threshold"}
            locations[location["id"]] = location
            runway_access.append({"runway": name, "nodeId": node_id, "type": location["accessType"]})

    nodes = list(nodes_by_id.values())
    component_reports, node_component = components(list(nodes_by_id), edges)
    taxi_components, taxi_index = components(list(nodes_by_id), edges, allow_runways=False)
    for location in locations.values():
        location["component"] = node_component.get(location["nodeId"])
        location["taxiComponent"] = taxi_index.get(location["nodeId"])
    stands = [location for location in locations.values() if location["kind"] == "stand"]
    unavailable = [{"name": s["name"], "reason": s.get("unavailableReason", "outside primary connected component"), "sources": s["sources"]} for s in stands if s["nodeId"] is None or s["component"] != 0]
    sources = [
        {"name": "OpenStreetMap contributors", "url": "https://www.openstreetmap.org/copyright", "license": "ODbL-1.0", "file": "data-sources/lfpg-osm.json", "sha256": hashlib.sha256(osm_path.read_bytes()).hexdigest(), "timestamp": osm.get("osm3s", {}).get("timestamp_osm_base"), "query": QUERY, "endpoint": "https://overpass-api.de/api/interpreter"},
        {"name": "Bundled vSMR LFPG AVISO / local GNG", "url": "https://github.com/IWantPizzaa/vSMR", "license": aviso["metadata"].get("geometry_license", "GPL-3.0"), "file": "data-sources/lfpg-aviso.geojson", "sha256": hashlib.sha256(aviso_path.read_bytes()).hexdigest()},
        {"name": "French vACC Ramp Agent LFPG configuration", "url": RAMP_URL, "commit": RAMP_COMMIT, "file": "data-sources/lfpg-ramp-agent.json", "sha256": hashlib.sha256(ramp_path.read_bytes()).hexdigest(), "license": "No explicit license found upstream at pinned revision; local requested prototype; redistribution license unresolved"},
    ]
    diagnostics = {"components": component_reports, "taxiOnlyComponents": taxi_components, "primaryComponentNodes": component_reports[0]["nodeCount"], "unavailableStands": unavailable, "buildingInteriorRejectedSegments": rejected_segments, "identicalCoordinateNodeMerges": identical_merges, "duplicateSegments": duplicate_segments, "inventedConnectors": 0, "runwayAccess": runway_access, "coordinateJoinToleranceMetres": 0, "exactIntersectionSplits": intersection_reports, "separatedLayerCrossings": separated_crossings}
    counts = {"nodes": len(nodes), "edges": len(edges), "sourceWays": dict(source_kind_counts), "stands": len(stands), "routableStands": sum(s["nodeId"] is not None for s in stands), "primaryComponentStands": sum(s["nodeId"] is not None and s["component"] == 0 for s in stands), "unavailableStands": len(unavailable), "holdingPoints": sum(l["kind"] == "holding_point" for l in locations.values()), "runwayEnds": len(runway_access), "surfaces": len(surfaces), "avisoGateLabels": sum(l["kind"] == "stand" for l in labels), "rampAgentStands": len(ramp["Stands"]), "referenceLines": len(reference_lines)}
    return {"nodes": nodes, "edges": edges, "locations": sorted(locations.values(), key=lambda l: (l["kind"], l["name"])), "surfaces": surfaces, "labels": labels, "referenceLines": reference_lines, "metadata": {"airport": "LFPG", "name": "Paris Charles de Gaulle", "schemaVersion": 1, "origin": {"lon": ORIGIN[0], "lat": ORIGIN[1]}, "projection": "Local equirectangular metres: x=(lon-2.5479)*111320*cos(49.0097deg), y=(lat-49.0097)*111320", "sources": sources, "counts": counts, "diagnostics": diagnostics, "limitations": ["Source snapshot is a simulator data set, not an operational or certified airport chart.", "No invented connectors: unavailable stands and disconnected source components remain explicit.", "AVISO gate labels are annotations; they are not used to invent stand lead-ins.", "Named stand records map to an existing, identically named OSM endpoint only within 60m; original source positions and match distances are retained.", "Runway destinations are existing runway/taxiway intersection nodes, not runway thresholds or ATC clearance limits.", "All source runway edges are retained, but planners should exclude runway travel for ordinary taxi routes.", "AVISO nonrouting reference lines include boundaries, stand markings and restriction lines; they do not extend the routing graph.", "A source parking segment crossing an AVISO building interior is excluded from routing and retained as reference geometry."]}}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--refresh-osm", action="store_true", help="Replace the OSM snapshot with a fresh Overpass response")
    parser.add_argument("--refresh-ramp", action="store_true", help="Fetch the pinned Ramp Agent LFPG configuration")
    parser.add_argument("--refresh-aviso", action="store_true", help="Copy the current bundled LFPG AVISO into the pinned snapshot")
    args = parser.parse_args()
    SOURCES.mkdir(exist_ok=True)
    OUT.mkdir(parents=True, exist_ok=True)
    if args.refresh_osm or not (SOURCES / "lfpg-osm.json").exists():
        download("https://overpass-api.de/api/interpreter", SOURCES / "lfpg-osm.json", urllib.parse.urlencode({"data": QUERY}).encode())
    if args.refresh_ramp or not (SOURCES / "lfpg-ramp-agent.json").exists():
        download(RAMP_URL, SOURCES / "lfpg-ramp-agent.json")
    if args.refresh_aviso or not (SOURCES / "lfpg-aviso.geojson").exists():
        shutil.copyfile(ROOT.parent / "vSMR" / "data" / "AVISO" / "LFPG.geojson", SOURCES / "lfpg-aviso.geojson")
    data = build()
    (OUT / "lfpg.json").write_text(json.dumps(data, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    report = {"counts": data["metadata"]["counts"], "sources": data["metadata"]["sources"], "diagnostics": data["metadata"]["diagnostics"], "limitations": data["metadata"]["limitations"]}
    (OUT / "connectivity-report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report["counts"], indent=2))


if __name__ == "__main__":
    main()
