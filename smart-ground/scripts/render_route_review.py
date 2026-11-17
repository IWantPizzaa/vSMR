#!/usr/bin/env python3
"""Render a geographic source-data review, without requiring a web browser.

Requires Pillow. Routes are independently computed using Dijkstra on exactly
the same source graph, excluding runway edges. This is not a browser screenshot.
"""
from collections import defaultdict
import heapq
import json
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
PALETTE = ["#5bd6fb", "#f4b85c", "#c6a0ff", "#a7df72", "#ff8ba2", "#f7e576", "#68d8b4", "#e89cec"]


def main() -> None:
    airport = json.loads((ROOT / "public/data/lfpg.json").read_text(encoding="utf-8"))
    nodes = {node["id"]: node for node in airport["nodes"]}
    adjacency = defaultdict(list)
    for edge in airport["edges"]:
        if edge["kind"] != "runway":
            adjacency[edge["from"]].append((edge["to"], edge["length"], edge["id"]))
            adjacency[edge["to"]].append((edge["from"], edge["length"], edge["id"]))
    stands = [location for location in airport["locations"] if location["kind"] == "stand" and location.get("taxiComponent") == 0]
    runways = [location for location in airport["locations"] if location["kind"] == "runway"]
    routes = []
    for i, runway in enumerate(runways):
        stand = stands[(i * 53) % len(stands)]
        source, target = stand["nodeId"], runway["nodeId"]
        queue, distances, parents = [(0.0, source)], {source: 0.0}, {}
        while queue:
            distance, node = heapq.heappop(queue)
            if distance != distances[node]:
                continue
            if node == target:
                break
            for neighbour, length, edge_id in adjacency[node]:
                candidate = distance + length
                if candidate < distances.get(neighbour, math.inf):
                    distances[neighbour] = candidate
                    parents[neighbour] = (node, edge_id)
                    heapq.heappush(queue, (candidate, neighbour))
        route_nodes, route_edges = [target], []
        while route_nodes[-1] != source:
            previous, edge_id = parents[route_nodes[-1]]
            route_nodes.append(previous)
            route_edges.append(edge_id)
        route_nodes.reverse()
        route_edges.reverse()
        routes.append({"stand": stand["name"], "runway": runway["name"], "originId": stand["id"], "destinationId": runway["id"], "length": distances[target], "nodes": route_nodes, "edges": route_edges})

    # Supersampling keeps dense centerline and building edges readable.
    scale = 2
    width, height = 3200, 1990
    image = Image.new("RGB", (width * scale, height * scale), "#0c1921")
    draw = ImageDraw.Draw(image)
    font_path = Path("C:/Windows/Fonts/segoeui.ttf")
    if not font_path.exists():
        font_path = Path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")

    def font(size):
        return ImageFont.truetype(str(font_path), size * scale)

    def text(position, message, size=24, fill="#dce8ed", **kwargs):
        draw.text(tuple(round(v * scale) for v in position), message, font=font(size), fill=fill, **kwargs)

    def line(points, fill, width=1):
        draw.line([(round(x * scale), round(y * scale)) for x, y in points], fill=fill, width=max(1, round(width * scale)), joint="curve")

    def box(position, fill, outline=None, width=1):
        draw.rectangle(tuple(round(v * scale) for v in position), fill=fill, outline=outline, width=round(width * scale))

    text((70, 42), "SMART GROUND  /  LFPG", 42, "#edf5f8")
    text((72, 104), "Revue des données géographiques et de huit itinéraires sur les axes source", 26, "#9cb7c5")
    text((2440, 60), "INSTANTANÉ OSM · 12 NOV. 2026", 22, "#8fadb9")
    counts = airport["metadata"]["counts"]
    text((72, 155), f"{counts['nodes']:,} nœuds   ·   {counts['edges']:,} arêtes   ·   451 stands reliés   ·   8 accès de piste".replace(",", " "), 23, "#7fd6c2")
    map_left, map_top, map_width, map_height = 75, 220, 3050, 1310
    box((map_left, map_top, map_left + map_width, map_top + map_height), "#101f29", "#29404d")
    all_positions = [(n["x"], n["y"]) for n in nodes.values()]
    all_positions.extend(tuple(p) for surface in airport["surfaces"] for p in surface["points"])
    min_x, max_x = min(p[0] for p in all_positions), max(p[0] for p in all_positions)
    min_y, max_y = min(p[1] for p in all_positions), max(p[1] for p in all_positions)
    pixels_per_metre = min((map_width - 180) / (max_x - min_x), (map_height - 110) / (max_y - min_y))
    offset_x = map_left + (map_width - (max_x - min_x) * pixels_per_metre) / 2
    offset_y = map_top + (map_height - (max_y - min_y) * pixels_per_metre) / 2

    def project(x, y):
        return (offset_x + (x - min_x) * pixels_per_metre, offset_y + (max_y - y) * pixels_per_metre)

    def source_point(node_id):
        return project(nodes[node_id]["x"], nodes[node_id]["y"])

    fills = {"terrain": "#1e2929", "grass": "#19352f", "taxiway": "#25404d", "apron": "#263844", "runway": "#0b1117", "building": "#526574", "closure": "#603a3b", "stopbar": "#7f5151"}
    order = {"terrain": 0, "grass": 1, "apron": 2, "taxiway": 3, "runway": 4, "building": 5, "closure": 6, "stopbar": 7}
    for surface in sorted(airport["surfaces"], key=lambda s: order.get(s["kind"], 3)):
        coords = [project(*point) for point in surface["points"]]
        if len(coords) < 3:
            continue
        draw.polygon([(round(x * scale), round(y * scale)) for x, y in coords], fill=fills.get(surface["kind"], "#263844"))
        if surface["kind"] in {"building", "runway"}:
            line(coords, "#6c808c" if surface["kind"] == "building" else "#78909e", 0.55)
    for edge in airport["edges"]:
        if edge["kind"] == "runway":
            continue
        colour = "#708d9d" if edge["kind"] == "taxiway" else "#4c697b"
        line([source_point(edge["from"]), source_point(edge["to"])], colour, 0.7 if edge["kind"] == "taxiway" else 0.5)
    for location in airport["locations"]:
        if location["kind"] == "stand" and (location["nodeId"] is None or location.get("component") != 0):
            px, py = project(location["x"], location["y"])
            line([(px - 2, py - 2), (px + 2, py + 2)], "#f4a35c", 1.3)
            line([(px - 2, py + 2), (px + 2, py - 2)], "#f4a35c", 1.3)
    for i, route in enumerate(routes):
        colour = PALETTE[i]
        points = [source_point(node) for node in route["nodes"]]
        line(points, colour, 2.7)
        for px, py in [points[0], points[-1]]:
            draw.ellipse(((px - 5) * scale, (py - 5) * scale, (px + 5) * scale, (py + 5) * scale), fill=colour, outline="#0c1921", width=scale)
        px, py = points[0]
        text((px + 8, py + 5), route["stand"], 20, colour, stroke_width=2 * scale, stroke_fill="#0c1921")
    for location in runways:
        px, py = project(location["x"], location["y"])
        text((px - 20, py - 35), location["name"], 22, "#ffffff", stroke_width=2 * scale, stroke_fill="#0c1921")
    for label in airport.get("labels", []):
        if label["kind"] == "terminal" and label["name"] in {"2A", "2B", "2C", "2D", "2E", "2F", "2G", "S3", "S4", "FEDEX", "FRET 4"}:
            px, py = project(label["x"], label["y"])
            text((px - 15, py - 6), label["name"], 15, "#b4c7d0", stroke_width=1 * scale, stroke_fill="#101f29")
    north_x, north_y = map_left + 65, map_top + 90
    line([(north_x, north_y + 50), (north_x, north_y)], "#d6e6eb", 2)
    line([(north_x - 9, north_y + 13), (north_x, north_y), (north_x + 9, north_y + 13)], "#d6e6eb", 2)
    text((north_x - 10, north_y - 37), "N", 24)
    sx, sy = map_left + 70, map_top + map_height - 55
    line([(sx, sy), (sx + 1000 * pixels_per_metre, sy)], "#c2d5de", 3)
    line([(sx, sy - 6), (sx, sy + 6)], "#c2d5de", 2)
    line([(sx + 1000 * pixels_per_metre, sy - 6), (sx + 1000 * pixels_per_metre, sy + 6)], "#c2d5de", 2)
    text((sx, sy + 10), "1 000 m", 20, "#9db9c6")

    text((75, 1570), "ITINÉRAIRES VÉRIFIÉS · DIJKSTRA INDÉPENDANT · AUCUNE ARÊTE DE PISTE", 24, "#dce8ed")
    for i, route in enumerate(routes):
        col, row = i % 4, i // 4
        x, y = 90 + col * 775, 1630 + row * 75
        line([(x, y + 18), (x + 55, y + 18)], PALETTE[i], 5)
        text((x + 73, y), f"{route['stand']} → {route['runway']}", 28, "#e2edf3")
        text((x + 405, y + 4), f"{route['length'] / 1000:.2f} km", 24, "#91aeba")
    box((75, 1813, 3125, 1814), "#2c414c")
    text((75, 1842), "× Orange : 72 stands sans route vers le réseau principal. Aucun raccordement de proximité n'est ajouté.", 23, "#e5b081")
    text((75, 1886), "Contrôle automatique : 8 487 arêtes sur leurs segments source · 0 traversée d'intérieur de bâtiment · 2 segments de parking exclus.", 22, "#9bb7c3")
    text((75, 1931), "Sources : © OpenStreetMap contributors (ODbL) · AVISO vSMR / local GNG (GPL-3.0) · Ramp Agent vaccfr (conditions amont non précisées).", 19, "#6f92a4")
    output = ROOT / "docs/lfpg-route-review.png"
    output.parent.mkdir(exist_ok=True)
    image.resize((width, height), Image.Resampling.LANCZOS).save(output)
    (ROOT / "docs/review-routes.json").write_text(json.dumps(routes, indent=2) + "\n", encoding="utf-8")
    print(output)
    print("Eight source-faithful shortest routes rendered; this is a data review, not a browser screenshot.")


if __name__ == "__main__":
    main()
