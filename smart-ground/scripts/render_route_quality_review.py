#!/usr/bin/env python3
"""Capture and plot real source-edge taxi routes before/after the quality changes.

This is an independent geographic review, not a Canvas/browser screenshot.  The
initial baseline was recorded before the turn-cost/deicing changes.  Re-running
captures the current planner and rejects stale geometry, catalogs or sources.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import tempfile

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
REPORT = ROOT / 'docs/route-quality-review-routes.json'
OUTPUT = ROOT / 'docs/lfpg-route-quality-review.png'
HASH_FILES = ['public/data/lfpg.json', 'public/data/lfpg-operations.json', 'public/data/lfpg-corridors.json',
              'public/data/lfpg-aircraft-restrictions.json', 'src/core/RoutePlanner.ts',
              'src/core/TaxiFlowPolicy.ts', 'src/core/RouteQuality.ts', 'src/core/OperatingRules.ts',
              'src/core/AircraftProfile.ts', 'src/core/AircraftRestrictions.ts']
CASES = [
    ('west', 'stand:B12', 'operation:departure:26R:T9', 'B12 > 26R / T9', 'Actuel : G3, F sud, R puis RT11; N reste vers le nord (GMC02).'),
    ('east', 'stand:F02', 'operation:departure:09R:D3', 'F02 > 09R / D3', 'Continuer sur F puis D; eviter les pads ROMEO.'),
    ('west', 'stand:A39', 'operation:departure:27L:Q4', 'A39 > 27L / Q4', 'Sortie P1, axe N puis B/Q; pas de raccourci par les bays.'),
    ('west', 'stand:J14', 'operation:departure:27L:Q4', 'J14 > 27L / Q4', 'Conserver Q au lieu de Q-B-Q quand la continuite est ouverte.'),
]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def capture(baseline):
    """Run the shipped planner, never synthesize route geometry in the renderer."""
    source = r'''
import {readFileSync,writeFileSync} from 'node:fs';
import {TaxiGraph} from '../src/core/TaxiGraph';
import {OperatingRules} from '../src/core/OperatingRules';
import {resolveOperatingManifest} from '../src/core/resolveOperatingManifest';
import {TaxiFlowPolicy} from '../src/core/TaxiFlowPolicy';
import {RoutePlanner} from '../src/core/RoutePlanner';
import {AircraftRestrictions} from '../src/core/AircraftRestrictions';
import {DEFAULT_AIRCRAFT_PROFILE} from '../src/core/AircraftProfile';
const root=new URL('../',import.meta.url);
const read=(p:string)=>JSON.parse(readFileSync(new URL(p,root),'utf8'));
const data=read('public/data/lfpg.json'),ops=resolveOperatingManifest(read('public/data/lfpg-operations.json')),flow=read('public/data/lfpg-corridors.json');
const aircraft=read('public/data/lfpg-aircraft-restrictions.json');
const activeProfileIds=Array.isArray(ops.activeProfileIds)?ops.activeProfileIds.filter((id):id is string=>typeof id==='string'):[];
const input=JSON.parse(readFileSync(process.argv[2],'utf8'));
const routes=input.routes.map((item:any)=>{
 const graph=new TaxiGraph(data),rules=new OperatingRules(graph,ops),planner=new RoutePlanner(graph,rules,item.configuration);
 planner.setFlowPolicy(new TaxiFlowPolicy(graph,flow));
 planner.setAircraftRestrictions(new AircraftRestrictions(graph,aircraft));
 planner.setAircraftProfile(DEFAULT_AIRCRAFT_PROFILE);
 planner.setAircraftRestrictionContext({airportFlags:activeProfileIds});
 const a=planner.locations.find(l=>l.id===item.from),b=planner.locations.find(l=>l.id===item.to);
 const route=a?.nodeId&&b?.nodeId?planner.findRoute(a.nodeId,b.nodeId,{movement:item.movement,originId:item.from,destinationId:item.to}):null;
 if(route&&!planner.isRouteAllowed(route,item.movement,{originId:item.from,destinationId:item.to}))throw Error('Actual route replay failed');
 return {configuration:item.configuration,movement:item.movement,from:item.from,to:item.to,...(route?{route}:{failure:planner.lastFlowFailure||'missing endpoint'})};
});
writeFileSync(process.argv[3],JSON.stringify({routes,activeProfileIds,aircraftProfile:DEFAULT_AIRCRAFT_PROFILE,airportFlags:activeProfileIds},null,2));
'''
    temp = ROOT / 'tmp'
    temp.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='route-quality-', dir=temp) as directory:
        folder = Path(directory)
        # Imports are relative to tmp/, so keep the source at that level.
        handle = tempfile.NamedTemporaryFile(suffix='.ts', prefix='route-quality-', dir=temp, delete=False)
        script = Path(handle.name)
        handle.close()
        try:
            script.write_text(source, encoding='utf-8')
            input_path, output_path = folder / 'input.json', folder / 'output.json'
            input_path.write_text(json.dumps(baseline), encoding='utf-8')
            subprocess.run(['npm.cmd', 'exec', 'tsx', str(script), str(input_path), str(output_path)], cwd=ROOT, check=True, capture_output=True, text=True)
            current = read(output_path)
        finally:
            script.unlink(missing_ok=True)
    current['hashes'] = {name: sha(ROOT / name) for name in HASH_FILES}
    return current


def point_at(points, distances, target):
    if target <= 0:
        return points[0]
    if target >= distances[-1]:
        return points[-1]
    for i in range(1, len(points)):
        if distances[i] >= target:
            span = distances[i] - distances[i - 1]
            ratio = (target - distances[i - 1]) / span if span else 0
            return tuple(points[i - 1][j] + ratio * (points[i][j] - points[i - 1][j]) for j in (0, 1))
    raise AssertionError('Unreachable distance sample')


def metrics(route, nodes, edges, degree):
    """Independent turn audit: real branching nodes, 40 m route tangents.

    Degree-two densification points never count.  Candidate deflections >=35deg
    less than 65 m apart form one maneuver.  This does not duplicate the cost
    implementation or claim to measure steering angle/ATC clearance count.
    """
    points = [(nodes[n]['x'], nodes[n]['y']) for n in route['nodeIds']]
    distances = [0.0]
    for i, edge_id in enumerate(route['edgeIds']):
        edge = edges[edge_id]
        if edge['kind'] == 'runway' or {edge['from'], edge['to']} != {route['nodeIds'][i], route['nodeIds'][i + 1]}:
            raise ValueError(f'Path is not a real taxi-edge chain: {edge_id}')
        distances.append(distances[-1] + edge['length'])
    if abs(distances[-1] - route['distance']) > .01:
        raise ValueError('Physical route length differs from source-edge sum')
    turns = []
    for i, node_id in enumerate(route['nodeIds'][1:-1], 1):
        if degree.get(node_id, 0) < 3:
            continue
        p = points[i]
        before = point_at(points, distances, distances[i] - 40)
        after = point_at(points, distances, distances[i] + 40)
        incoming, outgoing = (p[0] - before[0], p[1] - before[1]), (after[0] - p[0], after[1] - p[1])
        cross = incoming[0] * outgoing[1] - incoming[1] * outgoing[0]
        dot = incoming[0] * outgoing[0] + incoming[1] * outgoing[1]
        angle = math.degrees(math.atan2(cross, dot))
        if abs(angle) < 35:
            continue
        turn = {'nodeId': node_id, 'metresFromStart': round(distances[i], 3), 'angleDegrees': round(angle, 3)}
        if turns and distances[i] - turns[-1]['metresFromStart'] < 65:
            if abs(angle) > abs(turns[-1]['angleDegrees']):
                turns[-1] = turn
        else:
            turns.append(turn)
    # Pad identities are independently transcribed source centerlines, not a
    # copy of the generated special-area manifest.
    pad_ways = {112642886, 622439721, 622439717, 222981758, 1092616455}
    pad_metres = sum(edges[e]['length'] for e in route['edgeIds'] if edges[e].get('sourceWayId') in pad_ways)
    reversals = sum(1 for a, b in zip(turns, turns[1:]) if a['angleDegrees'] * b['angleDegrees'] < 0 and b['metresFromStart'] - a['metresFromStart'] < 220)
    primary_names = {'N', 'F', 'R', 'T', 'E', 'B', 'D', 'Q', 'U', 'C', 'M'}
    main_runs = []
    for edge_id in route['edgeIds']:
        name = edges[edge_id].get('name')
        # GMC01: this real 21 m R-named feeder is a connector U-to-N, not a
        # traversal of the principal R frontage. Keep the physical source name.
        is_main = name in primary_names and edge_id != 'osm:59465744:0'
        if is_main and (not main_runs or main_runs[-1] != name):
            main_runs.append(name)
    return {'physicalDistance': distances[-1], 'branchTurnClusters': len(turns), 'nearbyOppositeTurnPairs': reversals,
            'screenPadCenterlineMetres': pad_metres, 'sourceMainAxisRuns': main_runs,
            'sourceMainAxisChanges': max(0, len(main_runs) - 1),
            'repeatedNodes': len(route['nodeIds']) - len(set(route['nodeIds'])), 'turns': turns}


def prepare(do_capture):
    if REPORT.exists():
        report = read(REPORT)
    else:
        baseline_path = ROOT / 'tmp/user-route-baseline.json'
        if not baseline_path.exists():
            raise ValueError('Missing frozen baseline: never reconstruct a before route with the changed planner.')
        report = {'description': 'Actual MANEX-mode baseline before this user-requested quality change; current trace is replayed against shipped rules.',
                  'baseline': read(baseline_path)}
    if do_capture:
        report['current'] = capture(report['baseline'])
    for name in HASH_FILES:
        if report['current']['hashes'].get(name) != sha(ROOT / name):
            raise ValueError(f'Stale current route trace for {name}; rerun without --no-capture')
    airport = read(ROOT / 'public/data/lfpg.json')
    if report['baseline']['hashes']['public/data/lfpg.json'] != sha(ROOT / 'public/data/lfpg.json'):
        raise ValueError('Baseline and current traces must use the same immutable source geometry.')
    nodes, edges = {p['id']: p for p in airport['nodes']}, {e['id']: e for e in airport['edges']}
    adjacency = {}
    for edge in airport['edges']:
        if edge['kind'] == 'runway':
            continue
        adjacency.setdefault(edge['from'], set()).add(edge['to'])
        adjacency.setdefault(edge['to'], set()).add(edge['from'])
    degree = {key: len(value) for key, value in adjacency.items()}
    report['metricDefinition'] = {'lookaheadMetres': 40, 'minimumBranchDegree': 3, 'minimumDeflectionDegrees': 35,
                                  'clusterSeparationMetres': 65, 'oppositePairSeparationMetres': 220,
                                  'sourcePadWays': [112642886, 622439721, 622439717, 222981758, 1092616455]}
    for snapshot in ['baseline', 'current']:
        for item in report[snapshot]['routes']:
            if item.get('route'):
                item['independentGeometryMetrics'] = metrics(item['route'], nodes, edges, degree)
    b12 = next(item['route'] for item in report['current']['routes'] if item['from'] == 'stand:B12' and item['to'] == 'operation:departure:26R:T9')
    g3_west, f_south = 0.0, 0.0
    for index, edge_id in enumerate(b12['edgeIds']):
        edge = edges[edge_id]
        before, after = nodes[b12['nodeIds'][index]], nodes[b12['nodeIds'][index + 1]]
        if edge.get('sourceWayId') == 159161262:
            g3_west += before['x'] - after['x']
        if edge.get('sourceWayId') in {172796083, 222939600}:
            f_south += before['y'] - after['y']
    if g3_west <= 400 or f_south <= 300:
        raise ValueError('The current B12 review path does not match the source-backed G3-west / F-south caption; do not publish a misleading plot.')
    expected = sorted(p['id'] for p in read(ROOT / 'public/data/lfpg-operations.json').get('temporaryProfiles', []) if p.get('defaultEnabled'))
    if sorted(report['current'].get('activeProfileIds', [])) != expected:
        raise ValueError('Trace did not use simulator default works profiles.')
    if report['current'].get('aircraftProfile') != {'typeCode': 'A320', 'wingspanMetres': 35.8} or sorted(report['current'].get('airportFlags', [])) != expected:
        raise ValueError('Current trace must use the default A320 profile and effective works-profile flags.')
    # Hash every available source backing the three overlaid restriction sets.
    checked = {}
    for filename in ['lfpg-operations.json', 'lfpg-corridors.json', 'lfpg-aircraft-restrictions.json']:
        for source in read(ROOT / 'public/data' / filename)['sources']:
            if not source.get('sha256'):
                continue
            path = Path(source['path']) if source.get('path') else (ROOT / source['file']).resolve()
            if not path.exists() or sha(path) != source['sha256']:
                raise ValueError(f'Source hash mismatch: {source.get("id")}')
            checked[source['id']] = source['sha256']
    report['verifiedCurrentSources'] = [{'id': source_id, 'sha256': digest} for source_id, digest in checked.items()]
    REPORT.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    return airport, nodes, edges, report


def font(size, bold=False):
    directory = Path('C:/Windows/Fonts')
    filename = directory / ('arialbd.ttf' if bold else 'arial.ttf')
    return ImageFont.truetype(str(filename), size)


def render(airport, nodes, edges, report):
    width, top, row_h, gutter = 2400, 158, 525, 26
    image = Image.new('RGB', (width, top + len(CASES) * row_h + 70), '#101e24')
    draw = ImageDraw.Draw(image)
    draw.text((34, 20), 'LFPG - revue des roulages sur la geometrie source', font=font(39, True), fill='#eef7f5')
    draw.text((34, 75), 'Orange : historique   |   Vert : actuel A320 (35.8 m), quatre catalogues   |   Cercles : choix aux embranchements', font=font(22), fill='#c2d1d2')
    draw.text((34, 111), 'Face Ouest : N reste nominalement vers le nord; G3 > F sud > R/T respecte GMC02. Aucune geometrie de trajet inventee.', font=font(22), fill='#d4dfb2')
    panel_w = (width - gutter * 3) // 2
    for row, (configuration, origin, destination, title, note) in enumerate(CASES):
        items = [next(item for item in report[s]['routes'] if (item['configuration'], item['from'], item['to']) == (configuration, origin, destination)) for s in ['baseline', 'current']]
        if not all(item.get('route') for item in items):
            raise ValueError(f'Missing concrete route for review: {title}')
        union = [nodes[n] for item in items for n in item['route']['nodeIds']]
        min_x, max_x = min(n['x'] for n in union), max(n['x'] for n in union)
        min_y, max_y = min(n['y'] for n in union), max(n['y'] for n in union)
        margin = 125
        bounds = min_x - margin, max_x + margin, min_y - margin, max_y + margin
        for col, item in enumerate(items):
            x0, y0 = gutter + col * (panel_w + gutter), top + row * row_h
            box = (x0, y0, x0 + panel_w, y0 + row_h - 17)
            draw.rounded_rectangle(box, radius=12, fill='#192c31', outline='#385159', width=2)
            prefix = 'AVANT' if col == 0 else 'ACTUEL'
            draw.text((x0 + 18, y0 + 12), f'{prefix} | FACE {"EST" if configuration == "east" else "OUEST"} | {title}', font=font(25, True), fill='#eaf3f3')
            metric = item['independentGeometryMetrics']
            line = f'{metric["physicalDistance"]:,.0f} m  |  {metric["sourceMainAxisChanges"]} changements d\'axe  |  {metric["branchTurnClusters"]} choix de virage  |  {metric["screenPadCenterlineMetres"]:.0f} m RN/RS/QB5-6'.replace(',', ' ')
            draw.text((x0 + 18, y0 + 47), line, font=font(21), fill='#c9d8db')
            draw.text((x0 + 18, y0 + 78), note, font=font(20), fill='#a8c0c6')
            plot = (x0 + 24, y0 + 117, x0 + panel_w - 24, y0 + row_h - 43)
            scale = min((plot[2] - plot[0]) / (bounds[1] - bounds[0]), (plot[3] - plot[1]) / (bounds[3] - bounds[2]))
            cx, cy = (plot[0] + plot[2]) / 2, (plot[1] + plot[3]) / 2
            def project(p):
                return cx + (p['x'] - (bounds[0] + bounds[1]) / 2) * scale, cy - (p['y'] - (bounds[2] + bounds[3]) / 2) * scale
            for edge in airport['edges']:
                a, b = nodes[edge['from']], nodes[edge['to']]
                if not all(bounds[0] <= p['x'] <= bounds[1] and bounds[2] <= p['y'] <= bounds[3] for p in [a, b]):
                    continue
                draw.line([project(a), project(b)], fill='#63787a' if edge['kind'] == 'runway' else '#344b50', width=3 if edge['kind'] == 'runway' else 1)
            color = '#f5ac64' if col == 0 else '#b6e977'
            draw.line([project(nodes[n]) for n in item['route']['nodeIds']], fill=color, width=5, joint='curve')
            label_points = {}
            for i, edge_id in enumerate(item['route']['edgeIds']):
                name = edges[edge_id].get('name')
                if name not in {'N', 'F', 'R', 'T', 'E', 'B', 'D', 'Q', 'G3', 'P4'}:
                    continue
                label_points.setdefault(name, []).append(nodes[item['route']['nodeIds'][i]])
            for name, candidates in label_points.items():
                px, py = project(candidates[len(candidates) // 2])
                label_box = draw.textbbox((px + 7, py - 27), name, font=font(18, True))
                draw.rectangle((label_box[0] - 3, label_box[1] - 2, label_box[2] + 3, label_box[3] + 2), fill='#192c31')
                draw.text((px + 7, py - 27), name, font=font(18, True), fill='#e7efec')
            for turn in metric['turns']:
                px, py = project(nodes[turn['nodeId']])
                draw.ellipse((px - 5, py - 5, px + 5, py + 5), outline='#ffffff', width=1)
            for label, node_id in [('A', item['route']['nodeIds'][0]), ('B', item['route']['nodeIds'][-1])]:
                px, py = project(nodes[node_id])
                draw.ellipse((px - 12, py - 12, px + 12, py + 12), fill=color, outline='#eef7ee', width=1)
                draw.text((px - 7, py - 10), label, font=font(17, True), fill='#18302b')
            nx, ny = plot[2] - 12, plot[1] + 13
            draw.line((nx, ny + 28, nx, ny + 4), fill='#e2edef', width=2)
            draw.polygon([(nx, ny), (nx - 4, ny + 8), (nx + 4, ny + 8)], fill='#e2edef')
            draw.text((nx - 6, ny + 32), 'N', font=font(16), fill='#e2edef')
    draw.text((34, image.height - 56), 'Audit geographique statique, pas une capture navigateur. Traces exactes, hashes et methode : route-quality-review-routes.json / ROUTE-QUALITY-SOURCES.md', font=font(21), fill='#a8bfc5')
    image.save(OUTPUT)
    print(f'{OUTPUT}: {image.width} x {image.height}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--no-capture', action='store_true', help='Render only if the captured current hashes still match.')
    args = parser.parse_args()
    render(*prepare(not args.no_capture))
