#!/usr/bin/env python3
"""Render validated operational LFPG routes as four static geographic panels.

Offline only. Requires Pillow. Refuses stale route/catalog/data hashes and
profiles that differ from the simulator's published defaults. This is not a
browser screenshot or a certificate of complete IRL operating equivalence.
"""
from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
COLORS = ["#63d6f4", "#d4b2ff", "#e8d76f"]


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def applies(rule, configuration, movement):
    return rule["configuration"] in ("both", configuration) and rule.get("movement", "both") in ("both", movement)


def validate_inputs():
    data_path = ROOT / "public/data/lfpg.json"
    rules_path = ROOT / "public/data/lfpg-operations.json"
    review = json.loads((ROOT / "docs/operating-review-routes.json").read_text(encoding="utf-8"))
    airport = json.loads(data_path.read_text(encoding="utf-8"))
    catalog = json.loads(rules_path.read_text(encoding="utf-8"))
    if review.get("dataSha256") != sha(data_path) or catalog.get("dataSha256") != sha(data_path):
        raise ValueError("Geographic dataset hash mismatch; regenerate the operational overlay and review routes.")
    if review.get("rulesSha256") != sha(rules_path):
        raise ValueError("Stale operational route report: catalog SHA-256 changed. Run npm run validate:operations first.")
    expected_profiles = sorted(p["id"] for p in catalog.get("temporaryProfiles", []) if p.get("defaultEnabled"))
    if sorted(review.get("activeProfileIds", [])) != expected_profiles:
        raise ValueError("Route profiles differ from the simulator defaults. Regenerate the operational review.")
    source_paths = set()
    for source in [*airport["metadata"].get("sources", []), *catalog["sources"]]:
        if not source.get("file") or not source.get("sha256"):
            continue
        path = (ROOT / source["file"]).resolve()
        if not path.is_relative_to(ROOT.resolve()) or not path.exists() or sha(path) != source["sha256"]:
            raise ValueError(f"Source snapshot hash mismatch: {source['file']}")
        source_paths.add(path)
    selected = [p for p in catalog.get("temporaryProfiles", []) if p["id"] in expected_profiles]
    rules = [*catalog["edgeRules"], *[r for p in selected for r in p.get("edgeRules", [])]]
    closed_locations = set(catalog.get("closedLocationIds", []))
    closed_locations.update(location for p in selected for location in p.get("closedLocationIds", []))
    nodes = {n["id"]: n for n in airport["nodes"]}
    edges = {e["id"]: e for e in airport["edges"]}
    for route in review["routes"]:
        if len(route["nodeIds"]) != len(route["edgeIds"]) + 1:
            raise ValueError("Malformed route node/edge cardinality.")
        if route["from"] in closed_locations or route["to"] in closed_locations:
            raise ValueError("The report contains a route from/to a closed stand.")
        total = 0
        for i, edge_id in enumerate(route["edgeIds"]):
            edge = edges.get(edge_id)
            if not edge or edge["kind"] == "runway" or {edge["from"], edge["to"]} != {route["nodeIds"][i], route["nodeIds"][i + 1]}:
                raise ValueError(f"Route is not a source-faithful taxi path: {edge_id}")
            total += edge["length"]
            for rule in rules:
                if not applies(rule, route["configuration"], route["movement"]) or edge_id not in rule["edgeIds"]:
                    continue
                permitted = rule["direction"] != "closed" and (edge["from"] == route["nodeIds"][i] if rule["direction"] == "forward" else edge["to"] == route["nodeIds"][i])
                if not permitted:
                    raise ValueError(f"Route violates an active source-backed edge rule: {rule['id']}")
        if abs(total - route["distance"]) > 1e-5:
            raise ValueError("Route distance is not equal to its source edge lengths.")
        if route.get("operational") is not None:
            expected_checkpoints = set()
            for i, node_id in enumerate(route["nodeIds"]):
                incoming = route["edgeIds"][i - 1] if i else None
                outgoing = route["edgeIds"][i] if i < len(route["edgeIds"]) else None
                for rule in catalog.get("clearanceRules", []):
                    if rule["nodeId"] == node_id and applies(rule, route["configuration"], route["movement"]) and (not rule.get("fromEdgeId") or rule["fromEdgeId"] == incoming) and (not rule.get("toEdgeId") or rule["toEdgeId"] == outgoing):
                        expected_checkpoints.add((rule["id"], i))
            reported_checkpoints = {(c["ruleId"], c["nodeIndex"]) for c in route["operational"]["clearanceCheckpoints"]}
            if reported_checkpoints != expected_checkpoints:
                raise ValueError("Reported runway checkpoints do not match source-backed route intersections.")
    for configuration in ("east", "west"):
        for movement in ("departure", "arrival"):
            if len([r for r in review["routes"] if r["configuration"] == configuration and r["movement"] == movement]) < 2:
                raise ValueError(f"Need at least two reviewed routes for {configuration}/{movement}.")
    return airport, catalog, review, rules, len(source_paths)


def main():
    airport, catalog, review, active_rules, source_count = validate_inputs()
    width, height = 3600, 2360
    image = Image.new("RGB", (width, height), "#0c1921")
    draw = ImageDraw.Draw(image)
    font_path = Path("C:/Windows/Fonts/segoeui.ttf")
    if not font_path.exists():
        font_path = Path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")
    def font(size):
        return ImageFont.truetype(str(font_path), size) if font_path.exists() else ImageFont.load_default()
    def text(position, message, size=23, fill="#d8e8ec", **kwargs):
        draw.text(position, message, font=font(size), fill=fill, **kwargs)
    def outlined(position, message, size=20, fill="#d8e8ec"):
        text(position, message, size, fill, stroke_width=3, stroke_fill="#101f29")
    nodes = {n["id"]: n for n in airport["nodes"]}
    locations = {l["id"]: l for l in [*airport["locations"], *catalog.get("operationalLocations", [])]}
    edges = {e["id"]: e for e in airport["edges"]}
    fills = {"terrain": "#1e2929", "grass": "#18322e", "taxiway": "#25404d", "apron": "#263844", "runway": "#10151c", "building": "#4a5b68", "closure": "#5b3b3d", "stopbar": "#714e4f"}
    order = {"terrain": 0, "grass": 1, "apron": 2, "taxiway": 3, "runway": 4, "building": 5, "closure": 6, "stopbar": 7}
    positions = [(n["x"], n["y"]) for n in nodes.values()]
    positions += [tuple(p) for s in airport["surfaces"] for p in s["points"]]
    min_x, max_x = min(p[0] for p in positions), max(p[0] for p in positions)
    min_y, max_y = min(p[1] for p in positions), max(p[1] for p in positions)

    text((65, 32), "SMART GROUND / LFPG", 41, "#e5f0f3")
    text((67, 94), "Revue géographique des parcours opérationnels — 4 contextes de roulage", 28, "#a7c1cc")
    text((2670, 45), "ÉDITION AIP : 29 OCT 2026", 24, "#a1bac2")
    profile_labels = []
    for profile in catalog.get("temporaryProfiles", []):
        label = profile["id"].replace("sup", "SUP ").replace("-2026", "").replace("-phase2", " phase 2")
        profile_labels.append(f"{label} {'activé' if profile['id'] in review['activeProfileIds'] else 'désactivé'}")
    text((2670, 87), " · ".join(profile_labels), 21, "#cfbd8b")
    text((67, 143), "Parcours A* validés. Départs arrêtés au point d’attente ; traversées des arrivées sur confirmation manuelle.", 23, "#8aaab5")

    panel_width, panel_height = 1705, 895
    for row, configuration in enumerate(("east", "west")):
        for column, movement in enumerate(("departure", "arrival")):
            left, top = 65 + column * 1765, 212 + row * 950
            draw.rectangle((left, top, left + panel_width, top + panel_height), fill="#101f29", outline="#29404d", width=2)
            label = f"FACE {'EST' if configuration == 'east' else 'OUEST'} / {'DÉPARTS' if movement == 'departure' else 'ARRIVÉES'}"
            text((left + 25, top + 19), label, 28, "#dfeef0")
            use = catalog["runwayUse"][configuration][movement]
            text((left + 25, top + 63), "Pistes : " + " / ".join(use), 20, "#88bcb8")
            map_left, map_top, map_width, map_height = left + 18, top + 110, panel_width - 36, 600
            ppm = min((map_width - 100) / (max_x - min_x), (map_height - 45) / (max_y - min_y))
            offset_x = map_left + (map_width - (max_x - min_x) * ppm) / 2
            offset_y = map_top + (map_height - (max_y - min_y) * ppm) / 2
            def project(x, y):
                return (round(offset_x + (x - min_x) * ppm), round(offset_y + (max_y - y) * ppm))
            def node_point(node_id):
                node = nodes[node_id]
                return project(node["x"], node["y"])
            for surface in sorted(airport["surfaces"], key=lambda s: order.get(s["kind"], 3)):
                polygon = [project(*point) for point in surface["points"]]
                if len(polygon) >= 3:
                    draw.polygon(polygon, fill=fills.get(surface["kind"], "#263844"))
            for edge in airport["edges"]:
                if edge["kind"] != "runway":
                    draw.line((node_point(edge["from"]), node_point(edge["to"])), fill="#48616e", width=1)
            closed = {edge_id for rule in active_rules if rule["direction"] == "closed" and applies(rule, configuration, movement) for edge_id in rule["edgeIds"]}
            for edge_id in closed:
                edge = edges[edge_id]
                draw.line((node_point(edge["from"]), node_point(edge["to"])), fill="#db9065", width=3)
            routes = [r for r in review["routes"] if r["configuration"] == configuration and r["movement"] == movement][:3]
            placed = []
            def marker_label(point, message, color):
                px, py = point
                for dx, dy in [(10, -25), (10, 8), (-100, -25), (-100, 10), (12, -52), (12, 35)]:
                    box = draw.textbbox((px + dx, py + dy), message, font=font(19), stroke_width=2)
                    if box[0] < map_left or box[2] > map_left + map_width or box[1] < map_top - 10 or box[3] > map_top + map_height:
                        continue
                    if any(box[0] < q[2] + 7 and box[2] > q[0] - 7 and box[1] < q[3] + 5 and box[3] > q[1] - 5 for q in placed):
                        continue
                    placed.append(box)
                    outlined((px + dx, py + dy), message, 19, color)
                    return
                # The textual route legend always preserves endpoint labels even if crowded on the map.
            checkpoint_count = 0
            for ordinal, route in enumerate(routes):
                color = COLORS[ordinal]
                route_points = [node_point(node_id) for node_id in route["nodeIds"]]
                draw.line(route_points, fill=color, width=4, joint="curve")
                cumulative = [0.0]
                for edge_id in route["edgeIds"]:
                    cumulative.append(cumulative[-1] + edges[edge_id]["length"])
                # Arrowheads follow source segments at fractions of route length.
                for fraction in (.25, .6, .85):
                    target = route["distance"] * fraction
                    for i in range(1, len(cumulative)):
                        if cumulative[i] < target:
                            continue
                        a, b = route_points[i - 1], route_points[i]
                        theta = math.atan2(b[1] - a[1], b[0] - a[0])
                        t = (target - cumulative[i - 1]) / max(1e-9, cumulative[i] - cumulative[i - 1])
                        p = (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)
                        wings = [(p[0] - 10 * math.cos(theta + d), p[1] - 10 * math.sin(theta + d)) for d in (-.55, .55)]
                        draw.line((wings[0], p, wings[1]), fill=color, width=3)
                        break
                for index, node_id in enumerate(route["nodeIds"]):
                    incoming = route["edgeIds"][index - 1] if index else None
                    outgoing = route["edgeIds"][index] if index < len(route["edgeIds"]) else None
                    checkpoints = [rule for rule in catalog.get("clearanceRules", []) if rule["nodeId"] == node_id and applies(rule, configuration, movement)
                                   and (not rule.get("fromEdgeId") or rule["fromEdgeId"] == incoming)
                                   and (not rule.get("toEdgeId") or rule["toEdgeId"] == outgoing)]
                    if checkpoints:
                        checkpoint_count += 1
                        x, y = node_point(node_id)
                        draw.polygon([(x, y - 8), (x + 8, y), (x, y + 8), (x - 8, y)], fill="#18232b", outline="#f0ad7b", width=2)
                for location_id, point in [(route["from"], route_points[0]), (route["to"], route_points[-1])]:
                    px, py = point
                    draw.ellipse((px - 5, py - 5, px + 5, py + 5), fill=color, outline="#0d1922", width=2)
                    location = locations.get(location_id)
                    name = location["name"] if location else location_id
                    marker_label(point, name, color)
                legend_y = top + 735 + ordinal * 42
                draw.line((left + 26, legend_y + 15, left + 65, legend_y + 15), fill=color, width=4)
                start = locations.get(route["from"], {}).get("name", route["from"])
                end = locations.get(route["to"], {}).get("name", route["to"])
                text((left + 83, legend_y), f"{start} → {end}", 20, color)
                text((left + panel_width - 170, legend_y), f"{route['distance'] / 1000:.2f} km", 21, "#b5c9d0")
            text((left + panel_width - 340, top + 67), f"{checkpoint_count} point(s) à confirmer", 18, "#d8b089")
            # Metric scale and north indication are independent of the page layout.
            sx, sy = map_left + 30, map_top + map_height - 20
            draw.line((sx, sy, sx + 1000 * ppm, sy), fill="#95afb9", width=2)
            text((sx, sy - 28), "1 km", 17, "#95afb9")
            text((map_left + 25, map_top + 5), "N ↑", 19, "#9eb7bf")

    draw.polygon([(79, 2161), (91, 2173), (79, 2185), (67, 2173)], fill="#18232b", outline="#f0ad7b", width=2)
    text((108, 2158), "Action manuelle au point d’attente / traversée", 24, "#d5b38d")
    draw.line((790, 2173, 832, 2173), fill="#db9065", width=4)
    text((850, 2158), "Voie fermée au roulage autonome ou travaux actifs", 24, "#d5b38d")
    text((67, 2207), "Validation géographique et algorithmique ; aucune équivalence IRL exhaustive ni couverture NOTAM temps réel n’est revendiquée.", 23, "#9bb8c4")
    text((67, 2254), f"Empreintes vérifiées : géographie {review['dataSha256'][:16]}…  ·  catalogue {review['rulesSha256'][:16]}…  ·  {source_count} instantanés source contrôlés", 21, "#7f9faa")
    text((67, 2300), "© OpenStreetMap contributors (ODbL) · AVISO local GNG (GPL-3.0) · cartes SIA référencées dans le catalogue. Rendu de données, pas une capture navigateur.", 20, "#6e929f")
    destination = ROOT / "docs/lfpg-operating-review.png"
    image.save(destination)
    print(destination)
    print(f"Validated hashes and default profiles {review['activeProfileIds']}; rendered 12 routes in four panels.")


if __name__ == "__main__":
    main()
