#!/usr/bin/env python3
"""Validate offline geometry, source lineage, connectivity and stand coverage."""
from collections import Counter, defaultdict
import hashlib
import heapq
import json
import math
from pathlib import Path

from shapely.geometry import LineString, Polygon
from shapely.strtree import STRtree

from import_lfpg import ROOT, SOURCES, OUT, xy


def main() -> None:
    airport = json.loads((OUT / "lfpg.json").read_text(encoding="utf-8"))
    nodes = {node["id"]: node for node in airport["nodes"]}
    edges = airport["edges"]
    osm = json.loads((SOURCES / "lfpg-osm.json").read_text(encoding="utf-8"))
    source_nodes = {node["id"]: node for node in osm["elements"] if node["type"] == "node"}
    source_ways = {way["id"]: way for way in osm["elements"] if way["type"] == "way"}
    shapes, parent_lengths = [], defaultdict(float)
    adjacency = defaultdict(list)
    for edge in edges:
        assert edge["from"] in nodes and edge["to"] in nodes, f"Missing endpoint: {edge['id']}"
        start, end = nodes[edge["from"]], nodes[edge["to"]]
        shape = LineString([(start["x"], start["y"]), (end["x"], end["y"])])
        assert edge["length"] > 0 and math.isfinite(edge["length"])
        assert abs(shape.length - edge["length"]) < 0.00001, f"Inconsistent metric: {edge['id']}"
        parent = edge.get("sourceSegmentId", edge["id"])
        _, way_id, index = parent.split(":")
        way = source_ways[int(way_id)]
        original_nodes = [source_nodes[n] for n in way["nodes"][int(index):int(index) + 2]]
        original = LineString([xy(node["lon"], node["lat"]) for node in original_nodes])
        assert shape.difference(original.buffer(0.00001)).is_empty, f"Invented geometry: {edge['id']}"
        parent_lengths[parent] += edge["length"]
        assert parent_lengths[parent] <= original.length + 0.00001, f"Source length duplicated: {parent}"
        if edge["kind"] != "runway":
            adjacency[edge["from"]].append((edge["to"], edge["length"]))
            adjacency[edge["to"]].append((edge["from"], edge["length"]))
        shapes.append(shape)
    for parent, imported_length in parent_lengths.items():
        _, way_id, index = parent.split(":")
        original_nodes = [source_nodes[n] for n in source_ways[int(way_id)]["nodes"][int(index):int(index) + 2]]
        original = LineString([xy(node["lon"], node["lat"]) for node in original_nodes])
        assert abs(imported_length - original.length) < 0.00001, f"Truncated source geometry: {parent}"

    buildings = [Polygon(surface["points"], surface.get("holes", [])).buffer(0).buffer(-1) for surface in airport["surfaces"] if surface["kind"] == "building"]
    tree = STRtree(buildings)
    building_violations = []
    for edge, shape in zip(edges, shapes):
        for index in tree.query(shape):
            if shape.intersection(buildings[int(index)]).length > 1.00001:
                building_violations.append(edge["id"])
    assert not building_violations, f"Aircraft could traverse building interiors: {building_violations}"

    # Every source annotation survives even when there is no safe route for it.
    names = {location["name"] for location in airport["locations"] if location["kind"] == "stand"}
    aviso_names = {label["name"].strip().upper() for label in airport["labels"] if label["kind"] == "stand"}
    ramp_names = set(json.loads((SOURCES / "lfpg-ramp-agent.json").read_text(encoding="utf-8"))["Stands"])
    assert aviso_names <= names, f"Missing AVISO stands: {aviso_names - names}"
    assert ramp_names <= names, f"Missing Ramp Agent stands: {ramp_names - names}"
    runways = [location for location in airport["locations"] if location["kind"] == "runway"]
    assert len(runways) == 8 and all(location["nodeId"] in adjacency for location in runways)
    by_runway = {location["name"]: location for location in runways}
    for west, east in [("08L", "26R"), ("08R", "26L"), ("09L", "27R"), ("09R", "27L")]:
        assert by_runway[west]["x"] < by_runway[east]["x"], f"Physical runway ends reversed: {west}/{east}"
        for name, opposite in [(west, east), (east, west)]:
            location = by_runway[name]
            threshold = location["thresholdPosition"]
            other = by_runway[opposite]["thresholdPosition"]
            assert math.hypot(location["x"] - threshold["x"], location["y"] - threshold["y"]) < math.hypot(location["x"] - other["x"], location["y"] - other["y"]), f"Wrong named threshold association: {name}"
    primary_stands = [location for location in airport["locations"] if location["kind"] == "stand" and location.get("taxiComponent") == 0]
    visited = set()
    stack = [runways[0]["nodeId"]]
    while stack:
        node = stack.pop()
        if node in visited:
            continue
        visited.add(node)
        stack.extend(neighbour for neighbour, _ in adjacency[node] if neighbour not in visited)
    assert all(stand["nodeId"] in visited for stand in primary_stands)
    assert all(runway["nodeId"] in visited for runway in runways)

    # One source-faithful shortest route to each runway from spread-out stands.
    routes = []
    for index, runway in enumerate(runways):
        stand = primary_stands[(index * 53) % len(primary_stands)]
        source, target = stand["nodeId"], runway["nodeId"]
        queue = [(0.0, source)]
        distance = {source: 0.0}
        while queue:
            cost, node = heapq.heappop(queue)
            if cost != distance[node]:
                continue
            if node == target:
                break
            for neighbour, length in adjacency[node]:
                candidate = cost + length
                if candidate < distance.get(neighbour, math.inf):
                    distance[neighbour] = candidate
                    heapq.heappush(queue, (candidate, neighbour))
        assert target in distance, f"Unreachable runway {runway['name']} from {stand['name']}"
        routes.append({"stand": stand["name"], "runway": runway["name"], "distanceMetres": round(distance[target], 1)})

    report = {"passed": True, "datasetSha256": hashlib.sha256((OUT / "lfpg.json").read_bytes()).hexdigest(), "nodesChecked": len(nodes), "edgesChecked": len(edges), "sourceSegmentsChecked": len(parent_lengths), "inventedGeometrySegments": 0, "buildingInteriorViolations": 0, "avisoStandLabelsPreserved": len(aviso_names), "rampStandIdsPreserved": len(ramp_names), "primaryStandsReachableWithoutRunwayTravel": len(primary_stands), "reachableRunwayEnds": len(runways), "physicalRunwayEndOrientationChecked": True, "sampleShortestRoutes": routes}
    (OUT / "geometry-validation.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
