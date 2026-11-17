#!/usr/bin/env python3
"""Map LFPG source arrows to existing, bounded taxiway sections.

This does not create geometry and does not certify that the undated AVISO
overlay is an exhaustive/current operational dataset. Ambiguous assignments
remain explicit. Run after import_lfpg.py; requires Shapely 2.
"""
from __future__ import annotations

from collections import defaultdict
import argparse
import hashlib
import json
import math
from pathlib import Path

from shapely.geometry import LineString
from shapely.strtree import STRtree

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "public/data/lfpg.json"
AVISO = ROOT / "data-sources/lfpg-aviso.geojson"
OUT = ROOT / "public/data/lfpg-directions.json"
ORIGIN = (2.5479, 49.0097)
SCALE_X = 111320 * math.cos(math.radians(ORIGIN[1]))
SOURCE_ID = "aviso-lfpg-local-directions"
MAX_PROJECTION_DISTANCE = 12.0
MAX_CONTINUITY_DISTANCE = 500.0
MAX_CONTINUITY_ANGLE = 35.0


def xy(p):
    return ((p[0] - ORIGIN[0]) * SCALE_X, (p[1] - ORIGIN[1]) * 111320)


def unit(a, b):
    dx, dy = b[0] - a[0], b[1] - a[1]
    length = math.hypot(dx, dy)
    return (dx / length, dy / length) if length else (0.0, 0.0)


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1]


def render_review(airport, output):
    """Render a data review, never a browser screenshot."""
    from PIL import Image, ImageDraw, ImageFont
    image = Image.new("RGB", (2500, 1700), "#0d1b22")
    draw = ImageDraw.Draw(image)
    font_path = Path("C:/Windows/Fonts/segoeui.ttf")
    def font(size):
        return ImageFont.truetype(str(font_path), size) if font_path.exists() else ImageFont.load_default()
    nodes = {n["id"]: n for n in airport["nodes"]}
    all_points = [(n["x"], n["y"]) for n in airport["nodes"]]
    all_points += [tuple(point) for s in airport["surfaces"] for point in s["points"]]
    minimum_x, maximum_x = min(p[0] for p in all_points), max(p[0] for p in all_points)
    minimum_y, maximum_y = min(p[1] for p in all_points), max(p[1] for p in all_points)
    scale = min(2310 / (maximum_x - minimum_x), 590 / (maximum_y - minimum_y))
    draw.text((70, 25), "SMART GROUND / LFPG - revue des directions source", fill="#e0eeee", font=font(32))
    draw.text((70, 74), "Arrows AVISO locales non datees - contexte ARR/DEP relu sur GMC 01/02 (cycle 29 OCT 2026)", fill="#90a9b0", font=font(20))
    palette = {"both": "#b3db83", "departure": "#77cff0", "arrival": "#eb8cb7"}
    for panel, configuration in enumerate(("east", "west")):
        top = 130 + panel * 720
        draw.text((70, top), "FACE EST" if configuration == "east" else "FACE OUEST", fill="#e0eeee", font=font(25))
        summary = output["summary"][configuration]
        draw.text((350, top + 5), f"{summary['matchedCenterlineArrows']}/{summary['centerlineArrows']} fleches centrales projetees - {summary['directedEdges']} segments contraints", fill="#9eb0b4", font=font(18))
        offset_x = (2500 - (maximum_x - minimum_x) * scale) / 2
        offset_y = top + 65
        def project(p):
            return (round(offset_x + (p[0] - minimum_x) * scale), round(offset_y + (maximum_y - p[1]) * scale))
        for surface in airport["surfaces"]:
            color = {"building": "#34434b", "runway": "#56606a", "grass": "#152d2c"}.get(surface["kind"], "#1b2b32")
            draw.polygon([project(p) for p in surface["points"]], fill=color)
        for edge in airport["edges"]:
            if edge["kind"] == "runway":
                continue
            draw.line([project((nodes[edge[n]]["x"], nodes[edge[n]]["y"])) for n in ("from", "to")], fill="#40535b", width=1)
        rule_lookup = {r["edgeIds"][0]: r for r in output["edgeRules"] if r["configuration"] == configuration}
        for edge in airport["edges"]:
            rule = rule_lookup.get(edge["id"])
            if rule:
                draw.line([project((nodes[edge[n]]["x"], nodes[edge[n]]["y"])) for n in ("from", "to")], fill=palette[rule["movement"]], width=2)
        for arrow in output["arrows"]:
            if arrow["configuration"] != configuration:
                continue
            color = "#778486" if arrow.get("advisoryOnly") else palette.get(arrow.get("movement"), "#efab6b")
            draw.line([project(p) for p in arrow["points"]], fill=color, width=3)
            if "matchedEdgeId" not in arrow and not arrow.get("advisoryOnly"):
                x, y = project((arrow["x"], arrow["y"]))
                draw.ellipse((x - 8, y - 8, x + 8, y + 8), outline="#efab6b", width=2)
    legend = [("#b3db83", "Direction de base"), ("#77cff0", "Depart"), ("#eb8cb7", "Arrivee"), ("#778486", "T1 option ATC"), ("#efab6b", "Projection absente")]
    for i, (color, text) in enumerate(legend):
        x = 70 + i * 470
        draw.line((x, 1590, x + 35, 1590), fill=color, width=4)
        draw.text((x + 48, 1575), text, fill="#bcced0", font=font(19))
    draw.text((70, 1630), "Controle de donnees geographiques. Les zones sans fleche ne sont pas certifiees libres de restrictions. Aucun connecteur ajoute.", fill="#849ea8", font=font(19))
    destination = ROOT / "docs/lfpg-direction-review.png"
    image.save(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--render-review", action="store_true", help="Also render a static geographic data review (requires Pillow).")
    arguments = parser.parse_args()
    airport = json.loads(DATA.read_text(encoding="utf-8"))
    aviso = json.loads(AVISO.read_text(encoding="utf-8"))
    nodes = {n["id"]: n for n in airport["nodes"]}
    edges = [e for e in airport["edges"] if e["kind"] == "taxiway"]
    edge_by_id = {e["id"]: e for e in edges}
    geometry = [LineString([(nodes[e["from"]]["x"], nodes[e["from"]]["y"]),
                            (nodes[e["to"]]["x"], nodes[e["to"]]["y"])]) for e in edges]
    index = STRtree(geometry)
    adjacency = defaultdict(list)
    for edge in edges:
        adjacency[edge["from"]].append(edge)
        adjacency[edge["to"]].append(edge)

    def position(node_id):
        return (nodes[node_id]["x"], nodes[node_id]["y"])

    def scope(seed, forward):
        """Continue the same source lane, never jumping to nearby geometry.

        The explicit maximum is a model scope, not a claimed AIP boundary.
        Named branches/ambiguous continuations stop propagation. Unnamed
        connectors only continue within their original OSM source way.
        """
        assigned = {seed["id"]: "forward" if forward else "reverse"}
        ends = [(seed["to"], seed["from"], True), (seed["from"], seed["to"], False)]
        for node_id, previous_id, downstream in ends:
            current = seed
            direction_vector = unit(position(previous_id), position(node_id))
            travelled = seed["length"] / 2
            while travelled < MAX_CONTINUITY_DISTANCE:
                candidates = []
                for candidate in adjacency[node_id]:
                    if candidate["id"] in assigned:
                        continue
                    named = seed["name"] not in ("Taxiway", "", "taxiway")
                    if named and candidate["name"] != seed["name"]:
                        continue
                    if not named and candidate.get("sourceWayId") != seed.get("sourceWayId"):
                        continue
                    other = candidate["to"] if candidate["from"] == node_id else candidate["from"]
                    vector = unit(position(node_id), position(other))
                    cosine = dot(direction_vector, vector)
                    if cosine < math.cos(math.radians(MAX_CONTINUITY_ANGLE)):
                        continue
                    candidates.append((cosine, candidate, other, vector))
                # Do not decide between same-name alternative lanes or branches.
                if len(candidates) != 1:
                    break
                _, candidate, other, vector = candidates[0]
                if travelled + candidate["length"] > MAX_CONTINUITY_DISTANCE:
                    break
                travel_from = node_id if downstream else other
                travel_to = other if downstream else node_id
                # The walk starts in stored seed direction, then flips for reverse arrows.
                allowed_from, allowed_to = (travel_from, travel_to) if forward else (travel_to, travel_from)
                assigned[candidate["id"]] = "forward" if candidate["from"] == allowed_from else "reverse"
                travelled += candidate["length"]
                current, previous_id, node_id, direction_vector = candidate, node_id, other, vector
        return assigned

    arrows, unmatched, advisory, claims = [], [], [], defaultdict(list)
    for feature in aviso["features"]:
        props = feature["properties"]
        if props.get("geometry_role") != "directional_arrows":
            continue
        configuration = "east" if "East" in props["name"] else "west"
        palette = "centerline" if "centerline" in props["category"] else "green" if "green" in props["category"] else "brown"
        for ordinal, coordinates in enumerate(feature["geometry"]["coordinates"]):
            arrow_id = f"{configuration}:{feature['id']}:{ordinal}"
            # Source arrows contain shaft, wing 1, repeated tip, wing 2.
            points = [xy(p) for p in coordinates]
            arrow = {"id": arrow_id, "configuration": configuration, "palette": palette,
                     "sourceId": SOURCE_ID, "points": [[round(x, 3), round(y, 3)] for x, y in points],
                     "shaft": [[round(x, 3), round(y, 3)] for x, y in points[:-3]],
                     "x": round(points[0][0], 3), "y": round(points[0][1], 3)}
            arrows.append(arrow)
            if palette != "centerline":
                arrow["advisoryOnly"] = True
                advisory.append({"id": arrow_id, "reason": "T1_optional_GREEN_RED_push_pull_not_a_mandatory_default_flow"})
                continue
            if len(points) < 5 or math.dist(points[-2], points[-4]) > 0.1:
                unmatched.append({"id": arrow_id, "reason": "unrecognised_arrowhead_structure"})
                continue
            shaft = LineString(points[:-3])
            midpoint = shaft.interpolate(.5, normalized=True)
            tangent = unit(shaft.interpolate(max(0, shaft.length / 2 - 1)).coords[0],
                           shaft.interpolate(min(shaft.length, shaft.length / 2 + 1)).coords[0])
            arrow.update({"x": round(midpoint.x, 3), "y": round(midpoint.y, 3)})
            candidates = []
            for i in index.query(midpoint.buffer(MAX_PROJECTION_DISTANCE)):
                i = int(i)
                distance = geometry[i].distance(midpoint)
                alignment = abs(dot(tangent, unit(geometry[i].coords[0], geometry[i].coords[-1])))
                if distance <= MAX_PROJECTION_DISTANCE and alignment >= math.cos(math.radians(35)):
                    candidates.append((distance + (1 - alignment) * 10, distance, alignment, i))
            candidates.sort()
            if not candidates:
                unmatched.append({"id": arrow_id, "reason": "no_aligned_existing_taxiway_within_12m"})
                continue
            best = candidates[0]
            # Equal-distance distinct parallel lane candidates are not silently resolved.
            ambiguity = [c for c in candidates[1:] if c[0] - best[0] < 0.5
                         and not ({edges[c[3]]["from"], edges[c[3]]["to"]}
                                  & {edges[best[3]]["from"], edges[best[3]]["to"]})]
            if ambiguity:
                unmatched.append({"id": arrow_id, "reason": "ambiguous_nearby_lanes", "candidates": [edges[c[3]]["id"] for c in [best, *ambiguity]]})
                continue
            edge = edges[best[3]]
            forward = dot(tangent, unit(geometry[best[3]].coords[0], geometry[best[3]].coords[-1])) > 0
            arrow.update({"matchedEdgeId": edge["id"], "taxiway": edge["name"],
                          "projectionDistance": round(best[1], 3), "alignment": round(best[2], 5)})
            # GMC charts distinguish ARR and DEP on the same E alignment.
            # The undated generic E overlay must not impose its departure flow on arrivals.
            movement = "departure" if edge["name"] == "E" else "both"
            if configuration == "west" and edge["name"] in ("T", "R"):
                # GMC 02 uses blue eastbound DEP and pink westbound ARR here.
                movement = "departure" if points[len(points) - 4][0] > points[0][0] else "arrival"
            arrow["movement"] = movement
            assigned = scope(edge, forward)
            arrow["scopeEdgeIds"] = sorted(assigned)
            for edge_id, direction in assigned.items():
                claims[(configuration, movement, edge_id)].append({"arrowId": arrow_id, "direction": direction})

    conflicts, rules = [], []
    for (configuration, movement, edge_id), evidence in sorted(claims.items()):
        directions = {item["direction"] for item in evidence}
        if len(directions) != 1:
            conflicts.append({"configuration": configuration, "movement": movement, "edgeId": edge_id,
                              "reason": "opposing_source_arrow_scopes_no_rule_applied", "evidence": evidence})
            continue
        edge = edge_by_id[edge_id]
        role_source = f"sia-lfpg-gmc0{1 if configuration == 'east' else 2}-20261029" if movement != "both" else None
        rules.append({"id": f"arrow:{configuration}:{movement}:{edge_id}", "edgeIds": [edge_id],
                      "configuration": configuration, "movement": movement, "direction": directions.pop(),
                      "sourceIds": [SOURCE_ID, *([role_source] if role_source else [])], "arrowIds": sorted({item["arrowId"] for item in evidence}),
                      "reason": f"Sens de la couche AVISO {configuration}, continuité locale de {edge['name']}.",
                      "mapping": "bounded_same_lane_continuity", "authoritative": False})

    summary = {}
    for configuration in ["east", "west"]:
        selected = [r for r in rules if r["configuration"] == configuration]
        selected_edges = {r["edgeIds"][0] for r in selected}
        summary[configuration] = {"sourceArrows": sum(a["configuration"] == configuration for a in arrows),
                                  "centerlineArrows": sum(a["configuration"] == configuration and a["palette"] == "centerline" for a in arrows),
                                  "matchedCenterlineArrows": sum(a["configuration"] == configuration and "matchedEdgeId" in a for a in arrows),
                                  "directedEdges": len(selected_edges),
                                  "directedMetres": round(sum(edge_by_id[e]["length"] for e in selected_edges), 1),
                                  "unverifiedTaxiwayEdges": len(edges) - len(selected_edges)}
    sources = [{"id": SOURCE_ID, "title": "LFPG AVISO local GNG - East/West Arrows",
                           "url": "https://github.com/IWantPizzaa/vSMR", "file": "data-sources/lfpg-aviso.geojson",
                           "sha256": hashlib.sha256(AVISO.read_bytes()).hexdigest(), "license": "GPL-3.0",
                           "effectiveDate": None, "status": "supplemental_undated_direction_overlay"}]
    for number in (1, 2):
        chart = ROOT / f"data-sources/operations/lfpg-gmc-0{number}-20261029.pdf"
        sources.append({"id": f"sia-lfpg-gmc0{number}-20261029", "title": f"SIA AD 2 LFPG GMC 0{number}",
                        "url": f"https://www.sia.aviation-civile.gouv.fr/media/dvd/eAIP_29_OCT_2026/FRANCE/AIRAC-2026-10-29/html/eAIP/Cartes/LFPG/AD_2_LFPG_GMC_0{number}.pdf",
                        "file": chart.relative_to(ROOT).as_posix(), "effectiveDate": "2026-06-11", "cycleDate": "2026-10-29",
                        "sha256": hashlib.sha256(chart.read_bytes()).hexdigest() if chart.exists() else None,
                        "usage": "Visual ARR/DEP context check for E and west R/T; not a georeferenced source for the AVISO projections."})
    verified = {configuration: {r["edgeIds"][0] for r in rules if r["configuration"] == configuration} for configuration in ("east", "west")}
    output = {"version": 1, "airport": "LFPG", "dataSha256": hashlib.sha256(DATA.read_bytes()).hexdigest(),
              "sources": sources,
              "mappingParameters": {"projectionDistanceMetres": MAX_PROJECTION_DISTANCE,
                                    "maximumSameLanePropagationMetresPerSide": MAX_CONTINUITY_DISTANCE,
                                    "maximumContinuationAngleDegrees": MAX_CONTINUITY_ANGLE},
              "summary": summary, "edgeRules": rules, "arrows": arrows,
              "unverifiedEdgeIds": {configuration: sorted(e["id"] for e in edges if e["id"] not in verified[configuration])
                                    for configuration in ("east", "west")},
              "diagnostics": {"unmatchedArrows": unmatched, "optionalT1Arrows": advisory,
                              "opposingScopes": conflicts,
                              "limitations": ["Undated AVISO evidence is not a current operational certification.",
                                              "Propagation is bounded model inference along the same connected lane, not an AIP boundary.",
                                              "Unmatched/contradictory arrows never create connections or closed edges.",
                                              "T1 GREEN/RED push/pull variants remain advisory, not simultaneous prohibitions.",
                                              "E departure guidance does not constrain arrivals; current chart movement-specific rules take precedence."]}}
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(output, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if arguments.render_review:
        render_review(airport, output)
    print(json.dumps({"summary": summary, "unmatched": len(unmatched), "conflictingEdges": len(conflicts), "rules": len(rules)}, indent=2))


if __name__ == "__main__":
    main()
