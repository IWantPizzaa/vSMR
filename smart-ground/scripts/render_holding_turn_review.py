#!/usr/bin/env python3
"""Audit source geometry at runway-holding access junctions, before/after."""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import render_route_quality_review as helper
from PIL import Image, ImageDraw

ROOT = helper.ROOT
REPORT = ROOT / 'docs/holding-turn-review-routes.json'
OUTPUT = ROOT / 'docs/lfpg-holding-turn-review.png'
for source in ['src/core/RouteQuality.ts', 'src/core/OperatingRules.ts']:
    if source not in helper.HASH_FILES:
        helper.HASH_FILES.append(source)
CASES = [
    ('east', 'stand:B08', 'operation:departure:09R:D3', 'B08 > 09R / D3'),
    ('east', 'stand:M09', 'operation:departure:08L:T3', 'M09 > 08L / T3'),
    ('east', 'stand:B03', 'operation:departure:09R:D5', 'B03 > 09R / D5'),
]


def local_metrics(route, nodes, edges):
    """Geometry only: heading changes over the final 700 m source chain."""
    points = [(nodes[n]['x'], nodes[n]['y']) for n in route['nodeIds']]
    cumulative = [0.0]
    for index, edge_id in enumerate(route['edgeIds']):
        edge = edges[edge_id]
        if edge['kind'] == 'runway' or {edge['from'], edge['to']} != {route['nodeIds'][index], route['nodeIds'][index + 1]}:
            raise ValueError(f'Not a real taxi source chain: {edge_id}')
        cumulative.append(cumulative[-1] + edge['length'])
    if abs(cumulative[-1] - route['distance']) > .01:
        raise ValueError('Incorrect physical distance')
    local_start = max(0, route['distance'] - 700)
    raw, turns, total, sharp, global_max = [], [], 0.0, [], 0.0
    for index in range(1, len(points) - 1):
        a, b, c = points[index - 1:index + 2]
        u, v = (b[0] - a[0], b[1] - a[1]), (c[0] - b[0], c[1] - b[1])
        angle = math.degrees(math.atan2(u[0] * v[1] - u[1] * v[0], u[0] * v[0] + u[1] * v[1]))
        global_max = max(global_max, abs(angle))
        if abs(angle) > 110:
            sharp.append({'nodeId': route['nodeIds'][index], 'degrees': angle,
                          'incomingEdgeId': route['edgeIds'][index - 1], 'outgoingEdgeId': route['edgeIds'][index]})
        if cumulative[index] >= local_start:
            total += abs(angle)
            raw.append({'nodeId': route['nodeIds'][index], 'degrees': angle, 'remainingMetres': route['distance'] - cumulative[index]})
        a = helper.point_at(points, cumulative, cumulative[index] - 20)
        c = helper.point_at(points, cumulative, cumulative[index] + 20)
        u, v = (b[0] - a[0], b[1] - a[1]), (c[0] - b[0], c[1] - b[1])
        angle = math.degrees(math.atan2(u[0] * v[1] - u[1] * v[0], u[0] * v[0] + u[1] * v[1]))
        if cumulative[index] >= local_start and abs(angle) >= 35:
            turns.append({'nodeId': route['nodeIds'][index], 'degrees': angle, 'remainingMetres': route['distance'] - cumulative[index]})
    runs = []
    for index, edge_id in enumerate(route['edgeIds']):
        edge = edges[edge_id]
        if cumulative[index + 1] < local_start:
            continue
        key = [edge.get('name'), edge.get('sourceWayId')]
        if not runs or runs[-1] != key:
            runs.append(key)
    return {'lastMetres': 700, 'absoluteHeadingChangeDegrees': total,
            'maximumAdjacentDeflectionDegrees': max((abs(t['degrees']) for t in raw), default=0),
            'globalMaximumAdjacentDeflectionDegrees': global_max, 'sourceSharpJunctions': sharp,
            'smoothedDeflections': turns, 'sourceWayRuns': runs,
            'repeatedNodeCount': len(route['nodeIds']) - len(set(route['nodeIds']))}


def load(capture):
    if REPORT.exists():
        report = helper.read(REPORT)
    else:
        report = {'baseline': helper.read(ROOT / 'tmp/holding-turn-baseline.json')}
    if capture:
        report['current'] = helper.capture(report['baseline'])
    for path in helper.HASH_FILES:
        if report['current']['hashes'].get(path) != helper.sha(ROOT / path):
            raise ValueError(f'Stale current trace: {path}')
    operations = helper.read(ROOT / 'public/data/lfpg-operations.json')
    expected_profiles = sorted(profile['id'] for profile in operations.get('temporaryProfiles', []) if profile.get('defaultEnabled'))
    if sorted(report['current'].get('activeProfileIds', [])) != expected_profiles:
        raise ValueError('Trace did not use the actual simulator default works profiles.')
    if report['current'].get('aircraftProfile') != {'typeCode': 'A320', 'wingspanMetres': 35.8} or sorted(report['current'].get('airportFlags', [])) != expected_profiles:
        raise ValueError('Current trace must use the default A320 profile and effective works-profile flags.')
    verified = {}
    for catalog in [operations, helper.read(ROOT / 'public/data/lfpg-corridors.json'), helper.read(ROOT / 'public/data/lfpg-aircraft-restrictions.json')]:
        for source in catalog.get('sources', []):
            if not source.get('sha256'):
                continue
            path = Path(source['path']) if source.get('path') else (ROOT / source['file']).resolve()
            if not path.exists() or helper.sha(path) != source['sha256']:
                raise ValueError(f'Source snapshot mismatch: {source.get("id")}')
            verified[source['id']] = source['sha256']
    report['verifiedCurrentSources'] = verified
    airport = helper.read(ROOT / 'public/data/lfpg.json')
    if report['baseline']['hashes']['public/data/lfpg.json'] != helper.sha(ROOT / 'public/data/lfpg.json'):
        raise ValueError('Geometry changed: do not compare routes against a different source graph.')
    nodes = {n['id']: n for n in airport['nodes']}
    edges = {e['id']: e for e in airport['edges']}
    for snapshot in ['baseline', 'current']:
        for item in report[snapshot]['routes']:
            if item.get('route'):
                item['holdingGeometry'] = local_metrics(item['route'], nodes, edges)
        captured = [item for item in report[snapshot]['routes'] if item.get('route')]
        report[snapshot]['auditSummary'] = {
            'requestedRoutes': len(report[snapshot]['routes']), 'capturedRoutes': len(captured),
            'routesWithSourceDeflectionOver110Degrees': sum(bool(item['holdingGeometry']['sourceSharpJunctions']) for item in captured),
            'routesWithRepeatedNodes': sum(bool(item['holdingGeometry']['repeatedNodeCount']) for item in captured),
            'globalMaximumAdjacentDeflectionDegrees': max((item['holdingGeometry']['globalMaximumAdjacentDeflectionDegrees'] for item in captured), default=0)}
    report['method'] = {'localScopeMetres': 700, 'tangentMetres': 20, 'source': 'Actual oriented edge traces; no generated turn-quality metadata used.'}
    REPORT.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    return airport, nodes, edges, report


def render(airport, nodes, edges, report):
    w, top, row_h, gap = 2400, 132, 580, 26
    image = Image.new('RGB', (w, top + len(CASES) * row_h + 92), '#101e24')
    draw = ImageDraw.Draw(image)
    draw.text((34, 21), "LFPG - acces aux points d'attente : geometrie reelle", font=helper.font(38, True), fill='#eef7f5')
    draw.text((34, 74), 'Orange : historique | Vert : actuel A320 (35.8 m), quatre catalogues | Derniers 700 m, meme echelle, geometrie source', font=helper.font(23), fill='#becfd2')
    panel_w = (w - 3 * gap) // 2
    for row, (config, origin, dest, title) in enumerate(CASES):
        items = [next(item for item in report[s]['routes'] if (item['configuration'], item['from'], item['to']) == (config, origin, dest)) for s in ['baseline', 'current']]
        paths = []
        for item in items:
            route = item['route']
            cumulative = [0.0]
            for edge_id in route['edgeIds']:
                cumulative.append(cumulative[-1] + edges[edge_id]['length'])
            selected = [node for index, node in enumerate(route['nodeIds']) if cumulative[index] >= route['distance'] - 700]
            # Include one node before the local interval to show continuity.
            index = route['nodeIds'].index(selected[0])
            paths.append(route['nodeIds'][max(0, index - 1):])
        union = [nodes[n] for path in paths for n in path]
        bounds = (min(n['x'] for n in union) - 85, max(n['x'] for n in union) + 85,
                  min(n['y'] for n in union) - 75, max(n['y'] for n in union) + 75)
        for col, item in enumerate(items):
            x0, y0 = gap + col * (panel_w + gap), top + row * row_h
            draw.rounded_rectangle((x0, y0, x0 + panel_w, y0 + row_h - 17), radius=12, fill='#192c31', outline='#385159', width=2)
            metric = item['holdingGeometry']
            draw.text((x0 + 18, y0 + 13), f'{"AVANT" if col == 0 else "ACTUEL"} | FACE EST | {title}', font=helper.font(26, True), fill='#eaf4f3')
            draw.text((x0 + 18, y0 + 51), f'Distance totale : {item["route"]["distance"]:.0f} m | Rotation cumulee locale : {metric["absoluteHeadingChangeDegrees"]:.0f} deg', font=helper.font(22), fill='#c8d8da')
            plot = (x0 + 30, y0 + 101, x0 + panel_w - 32, y0 + row_h - 45)
            scale = min((plot[2] - plot[0]) / (bounds[1] - bounds[0]), (plot[3] - plot[1]) / (bounds[3] - bounds[2]))
            def project(p):
                return (plot[0] + plot[2]) / 2 + (p['x'] - (bounds[0] + bounds[1]) / 2) * scale, (plot[1] + plot[3]) / 2 - (p['y'] - (bounds[2] + bounds[3]) / 2) * scale
            for edge in airport['edges']:
                a, b = nodes[edge['from']], nodes[edge['to']]
                if not all(bounds[0] <= p['x'] <= bounds[1] and bounds[2] <= p['y'] <= bounds[3] for p in [a, b]):
                    continue
                draw.line([project(a), project(b)], fill='#829497' if edge['kind'] == 'runway' else '#476169', width=4 if edge['kind'] == 'runway' else 2)
            color = '#f5ac64' if col == 0 else '#b6e977'
            draw.line([project(nodes[n]) for n in paths[col]], fill=color, width=7, joint='curve')
            labels = {}
            for edge_id in item['route']['edgeIds']:
                edge = edges[edge_id]
                if edge['from'] not in paths[col] or edge.get('name') == 'Taxiway':
                    continue
                labels.setdefault(edge.get('name'), []).append(nodes[edge['from']])
            for label, candidates in labels.items():
                px, py = project(candidates[len(candidates) // 2])
                draw.text((px + 8, py - 27), label, font=helper.font(22, True), fill='#e8f1f0', stroke_width=3, stroke_fill='#192c31')
            px, py = project(nodes[item['route']['nodeIds'][-1]])
            draw.ellipse((px - 12, py - 12, px + 12, py + 12), fill=color, outline='#eff9ee', width=2)
            draw.text((px + 18, py - 13), 'ATTENTE', font=helper.font(19, True), fill=color)
            nx, ny = plot[2] - 8, plot[1] + 7
            draw.line((nx, ny + 28, nx, ny), fill='#e2edef', width=2)
            draw.polygon([(nx, ny), (nx - 5, ny + 9), (nx + 5, ny + 9)], fill='#e2edef')
            draw.text((nx - 7, ny + 33), 'N', font=helper.font(18), fill='#e2edef')
    before, current = (report[s]['auditSummary'] for s in ['baseline', 'current'])
    draw.text((34, image.height - 81), f'Audit etendu : {current["capturedRoutes"]} trajets | Jonctions >110 deg : {before["routesWithSourceDeflectionOver110Degrees"]} avant / {current["routesWithSourceDeflectionOver110Degrees"]} actuel | Trajets avec noeud repete : {before["routesWithRepeatedNodes"]} avant / {current["routesWithRepeatedNodes"]} actuel', font=helper.font(22), fill='#d1dec2')
    draw.text((34, image.height - 44), 'Audit geographique statique | Traces exactes et hashes : holding-turn-review-routes.json | Pas une capture navigateur.', font=helper.font(22), fill='#aabfc5')
    image.save(OUTPUT)
    print(OUTPUT)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--no-capture', action='store_true')
    args = parser.parse_args()
    render(*load(not args.no_capture))
