#!/usr/bin/env python3
"""Build source-backed LFPG operating overlays without changing geographic data.

Uses the pinned AIP / GMC sources downloaded in data-sources/operations and the
supplemental AVISO direction mapping. Python standard library only. No network
or nearest-neighbour connector is used by this build.
"""
from __future__ import annotations

from collections import defaultdict
import hashlib
import heapq
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'public/data/lfpg-operations.json'
SOURCE = ROOT / 'data-sources/operations'
HTML_URL = 'https://www.sia.aviation-civile.gouv.fr/media/dvd/eAIP_29_OCT_2026/FRANCE/AIRAC-2026-10-29/html/eAIP/'
AD = 'sia-lfpg-ad2-20261029'
GMC_E = 'sia-lfpg-gmc01-20261029'
GMC_W = 'sia-lfpg-gmc02-20261029'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def xy(lon, lat):
    return ((lon - 2.5479) * 111320 * math.cos(math.radians(49.0097)), (lat - 49.0097) * 111320)


def main():
    data_file = ROOT / 'public/data/lfpg.json'
    data = json.loads(data_file.read_text(encoding='utf-8'))
    osm = json.loads((ROOT / 'data-sources/lfpg-osm.json').read_text(encoding='utf-8'))
    aviso = json.loads((ROOT / 'data-sources/lfpg-aviso.geojson').read_text(encoding='utf-8'))
    directions = json.loads((ROOT / 'public/data/lfpg-directions.json').read_text(encoding='utf-8'))
    nodes = {n['id']: n for n in data['nodes']}
    edges = {e['id']: e for e in data['edges']}
    adjacent = defaultdict(list)
    for edge in edges.values():
        adjacent[edge['from']].append(edge)
        adjacent[edge['to']].append(edge)
    node_tags = defaultdict(dict)
    for item in osm['elements']:
        if item['type'] == 'node':
            node_tags[f"osm:{item['id']}"].update(item.get('tags', {}))
    axes = {}
    for runway in aviso['metadata']['runways']:
        axes['/'.join(runway['ends'])] = [xy(*p) for p in runway['thresholds']]

    def axis_distance(node_id, runway):
        node = nodes[node_id]
        (ax, ay), (bx, by) = axes[runway]
        dx, dy = bx - ax, by - ay
        # Signed distance to the source runway's infinite centreline, positive north.
        return (dx * (node['y'] - ay) - dy * (node['x'] - ax)) / math.hypot(dx, dy)

    def named(name):
        return [e for e in edges.values() if e['name'] == name and e['kind'] == 'taxiway']

    def source_edges(way_ids):
        return [e for e in edges.values() if e.get('sourceWayId') in way_ids]

    def restricted_path(start, goal, allowed):
        index = defaultdict(list)
        for e in allowed:
            index[e['from']].append((e['to'], e))
            index[e['to']].append((e['from'], e))
        queue = [(0, start)]
        costs, previous = {start: 0}, {}
        while queue:
            cost, node = heapq.heappop(queue)
            if cost != costs[node]:
                continue
            if node == goal:
                result = []
                while node != start:
                    before, edge = previous[node]
                    result.append(edge)
                    node = before
                return list(reversed(result))
            for other, edge in index[node]:
                value = cost + edge['length']
                if value < costs.get(other, math.inf):
                    costs[other] = value
                    previous[other] = (node, edge)
                    heapq.heappush(queue, (value, other))
        return []

    sources = [
        {'id': AD, 'title': 'SIA LFPG AD 2.20, édition AIRAC 29 OCT 2026', 'url': HTML_URL + 'FR-AD-2.LFPG-fr-FR.html', 'effectiveDate': '2026-10-29', 'file': 'data-sources/operations/lfpg-ad2-20261029.html'},
        {'id': GMC_E, 'title': 'SIA AD 2 LFPG GMC 01, face EST', 'url': HTML_URL + 'Cartes/LFPG/AD_2_LFPG_GMC_01.pdf', 'effectiveDate': '2026-06-11', 'cycleDate': '2026-10-29', 'file': 'data-sources/operations/lfpg-gmc-01-20261029.pdf'},
        {'id': GMC_W, 'title': 'SIA AD 2 LFPG GMC 02, face OUEST', 'url': HTML_URL + 'Cartes/LFPG/AD_2_LFPG_GMC_02.pdf', 'effectiveDate': '2026-06-11', 'cycleDate': '2026-10-29', 'file': 'data-sources/operations/lfpg-gmc-02-20261029.pdf'},
        {'id': 'sia-lfpg-adc02-20261029', 'title': 'SIA AD 2 LFPG ADC 02, balisage et entrées interdites', 'url': HTML_URL + 'Cartes/LFPG/AD_2_LFPG_ADC_02.pdf', 'effectiveDate': '2025-10-02', 'cycleDate': '2026-10-29', 'file': 'data-sources/operations/lfpg-adc-02-20261029.pdf'},
        {'id': 'sia-sup151-2026', 'title': 'SIA SUP AIP 151/26, travaux A et B', 'url': 'https://www.sia.aviation-civile.gouv.fr/media/store/documents/file/l/f/lf_sup_a_2026_151_fr.pdf', 'effectiveDate': '2026-08-06', 'file': 'data-sources/operations/lf-sup-2026-151-fr.pdf'},
        {'id': 'sia-sup188-2026', 'title': 'SIA SUP AIP 188/26, travaux P3 et TE3', 'url': 'https://www.sia.aviation-civile.gouv.fr/media/store/documents/file/l/f/lf_sup_a_2026_188_fr.pdf', 'effectiveDate': '2026-10-01', 'file': 'data-sources/operations/lf-sup-2026-188-fr.pdf'},
        {'id': 'osm-lfpg-holding-20261112', 'title': 'OSM LFPG : points holding_position et topologie conservés', 'url': 'https://www.openstreetmap.org/copyright', 'effectiveDate': '2026-11-12', 'file': 'data-sources/lfpg-osm.json', 'license': 'ODbL-1.0'},
    ]
    for source in sources:
        source['sha256'] = sha(ROOT / source['file'])
    known_sources = {s['id'] for s in sources}
    sources.extend(s for s in directions['sources'] if s['id'] not in known_sources)

    permanent = []
    ledger = []
    unmapped = []
    def add_rule(id, selected, direction, reason, section, configuration='both'):
        if not selected:
            unmapped.append({'id': id, 'reason': 'No source edge matching the published scope.', 'section': section})
            return
        permanent.append({'id': id, 'edgeIds': [e['id'] for e in selected], 'configuration': configuration, 'direction': direction, 'sourceIds': [AD], 'reason': reason, 'authoritative': True, 'section': section})

    tow = [e for e in edges.values() if e['kind'] == 'taxiway' and e['name'] in {f'EM{i}' for i in range(3, 9)}]
    add_rule('aip-tow-only-em3-em8', tow, 'closed', 'EM3 à EM8 : circulation uniquement en tractage, moteurs éteints. Le prototype simule le roulage autonome.', '20.3.3.2.2')
    ledger.append({'id': 'tow-only', 'section': '20.3.3.2.2', 'status': 'enforced', 'description': 'EM3–EM8 exclus du roulage autonome ; le tractage n’est pas simulé.'})

    no_entry = {'09L/27R': ['Z3', 'Z5', 'Z6'], '09R/27L': [f'Y{i}' for i in range(1, 7)], '08L/26R': ['WT'] + [f'W{i}' for i in range(1, 7)], '08R/26L': ['V3', 'V4', 'V5', 'V6']}
    # Published no-entry bars apply towards a runway, not towards an arbitrary
    # cardinal direction. Orient each original connector by distance along that
    # named connector from a genuine shared runway node.
    for runway, names in no_entry.items():
        for name in names:
            continuation_ways = {'Z3': [223198024], 'Z5': [157174701], 'Z6': [157174628]}
            selected = named(name) + source_edges(continuation_ways.get(name, []))
            incident = defaultdict(list)
            for e in selected:
                incident[e['from']].append(e)
                incident[e['to']].append(e)
            seeds = [id for id in incident if any(e['kind'] == 'runway' for e in adjacent[id]) and abs(axis_distance(id, runway)) < 12]
            if not seeds:
                unmapped.append({'id': f'no-entry:{name}', 'reason': 'No genuine runway/connector shared node in source.', 'section': '20.3.2.1.7'})
                continue
            distances = {id: 0.0 for id in seeds}
            queue = [(0, id) for id in seeds]
            heapq.heapify(queue)
            while queue:
                value, id = heapq.heappop(queue)
                if value != distances[id]:
                    continue
                for e in incident[id]:
                    other = e['to'] if e['from'] == id else e['from']
                    cost = value + e['length']
                    if cost < distances.get(other, math.inf):
                        distances[other] = cost
                        heapq.heappush(queue, (cost, other))
            for e in selected:
                before, after = distances.get(e['from']), distances.get(e['to'])
                if before is None or after is None or abs(before - after) < 1e-8:
                    unmapped.append({'id': f"no-entry:{name}:{e['id']}", 'reason': 'Connector component cannot be oriented from its source runway.', 'section': '20.3.2.1.7'})
                    continue
                direction = 'forward' if before < after else 'reverse'
                add_rule(f"aip-no-entry:{e['id']}", [e], direction, f'{name} : sortie de piste uniquement ; entrée vers {runway} interdite H24.', '20.3.2.1.7 / 20.3.3.2.3')
    ledger.append({'id': 'runway-no-entry', 'section': '20.3.2.1.7 / 20.3.3.2.3', 'status': 'enforced', 'description': 'Z3/Z5/Z6 ; Y1–Y6 ; WT/W1–W6 ; V3–V6 utilisables dans le sens sortie, jamais pour entrer sur la piste.'})
    # MD2 source way continues north of B to D, outside this published restriction.
    # Restrict only its B -> M/BM5 side. The exact boundary is the shared B node.
    md2 = restricted_path('intersection:29', 'osm:13993315190', named('MD2'))
    for e in md2:
        direction = 'forward' if nodes[e['from']]['y'] > nodes[e['to']]['y'] else 'reverse'
        add_rule(f"aip-md2-away-runway:{e['id']}", [e], direction, 'MD2 entre B et M : entrée vers la piste interdite, sortie vers M seulement.', '20.3.3.2.3')
    ledger.append({'id': 'md2-no-entry', 'section': '20.3.3.2.3', 'status': 'enforced', 'description': 'MD2 : interdiction du sens vers la piste sur la partie au sud de B ; la portion B–D n’est pas étendue arbitrairement.'})

    # Real source holding nodes are selected by source runway / ILS type and
    # existing connector adjacency. No synthetic holding geometry is created.
    holds = [l for l in data['locations'] if l['kind'] == 'holding_point' and l['nodeId'] and node_tags[l['nodeId']].get('holding_position:type') == 'runway']
    locations, access, clearances = [], [], []
    runway_defs = {
        '09R': ('09R/27L', -1, 'departure', ['D1', 'D3', 'D4', 'D5', 'D6']),
        '27L': ('09R/27L', -1, 'departure', ['Q2', 'Q3', 'Q4', 'Q5', 'Q6']),
        '08L': ('08L/26R', 1, 'departure', ['T1', 'T2', 'T3', 'T4', 'T5', 'T6']),
        '26R': ('08L/26R', 1, 'departure', ['T9', 'T10', 'T11', 'T12', 'R1']),
        '09L': ('09R/27L', 1, 'arrival', ['K6', 'K7', 'K8']),
        '27R': ('09R/27L', 1, 'arrival', ['K1', 'K2', 'K3']),
        '08R': ('08L/26R', -1, 'arrival', ['S6', 'S7', 'S9']),
        '26L': ('08L/26R', -1, 'arrival', ['S1', 'S2', 'S3']),
    }
    for runway, (axis, side, role, connectors) in runway_defs.items():
        for connector in connectors:
            matches = [h for h in holds if any(e['name'] == connector for e in adjacent[h['nodeId']]) and axis_distance(h['nodeId'], axis) * side > 30]
            if not matches:
                unmapped.append({'id': f'access:{runway}:{connector}', 'reason': 'No normal/CAT I OSM holding node with verified connector adjacency on the required side.', 'section': '20.3.2.1.6 / GMC 01–02'})
                continue
            hold = min(matches, key=lambda h: abs(axis_distance(h['nodeId'], axis)))
            location_id = f'operation:{role}:{runway}:{connector}'
            configuration = 'east' if runway.startswith(('08', '09')) else 'west'
            locations.append({'id': location_id, 'name': f'{runway} · {connector} CAT I', 'nodeId': hold['nodeId'], 'kind': 'holding_point', 'x': hold['x'], 'y': hold['y'], 'lat': hold['lat'], 'lon': hold['lon'], 'source': hold['source'], 'sourceHoldingLocationId': hold['id'], 'runway': runway, 'connector': connector, 'role': role, 'configuration': configuration, 'positionType': 'real_source_holding_point', 'description': 'Point d’attente avant piste ; aucune entrée ou traversée automatique.' if role == 'departure' else 'Arrivée après dégagement de la piste extérieure, avant traversée de la piste intérieure.'})
            ppr = role == 'departure' and (runway, connector) in {('09R', 'D1'), ('27L', 'Q6'), ('08L', 'T1'), ('08L', 'T2'), ('26R', 'R1')}
            access.append({'locationId': location_id, 'runway': runway, 'role': role, 'configuration': configuration, 'connector': connector, 'crossingRunway': axis if role == 'arrival' else None, 'sourceIds': [AD, GMC_E if configuration == 'east' else GMC_W, 'osm-lfpg-holding-20261112'], 'pprRequired': ppr, 'pprReason': 'AD 2.13 : usage très long courrier, PPR avec préavis de 30 minutes sur la première fréquence contactée.' if ppr else None})
    for h in holds:
        axis = min(axes, key=lambda r: abs(axis_distance(h['nodeId'], r)))
        here = abs(axis_distance(h['nodeId'], axis))
        if not 30 < here < 250:
            continue
        for e in adjacent[h['nodeId']]:
            if e['kind'] != 'taxiway':
                continue
            other = e['to'] if e['from'] == h['nodeId'] else e['from']
            if abs(axis_distance(other, axis)) < here - .01:
                clearances.append({'id': f"aip-clearance:{h['nodeId']}:{e['id']}", 'nodeId': h['nodeId'], 'toEdgeId': e['id'], 'configuration': 'both', 'kind': 'runway-crossing', 'sourceIds': [AD, 'osm-lfpg-holding-20261112'], 'reason': f'{axis} : maintenir le point d’attente réel avant entrée/traversée, puis attendre la décision manuelle du contrôleur.', 'runway': axis, 'sourceHoldingLocationId': h['id']})

    priorities = []
    priority_defs = [('east', 'Q', ['Q2', 'Q3', 'Q4']), ('east', 'T', ['T9', 'T10', 'T11']), ('west', 'D', ['D4', 'D5', 'D6']), ('west', 'T', ['T4', 'T5', 'T6'])]
    for configuration, yielding, priority in priority_defs:
        for id, incident in adjacent.items():
            names = {e['name'] for e in incident}
            if yielding not in names or not names.intersection(priority):
                continue
            for incoming in incident:
                if incoming['name'] == yielding:
                    priorities.append({'id': f'aip-yield:{configuration}:{id}:{incoming["id"]}', 'nodeId': id, 'fromEdgeId': incoming['id'], 'priorityFromEdgeIds': [e['id'] for e in incident if e['name'] in priority], 'configuration': configuration, 'sourceIds': [AD, GMC_E if configuration == 'east' else GMC_W], 'reason': f"Sur {yielding}, céder la priorité aux arrivées sortant par {'/'.join(sorted(names.intersection(priority)))}. Aucune clairance de piste délivrée automatiquement.", 'yieldTaxiway': yielding, 'priorityTaxiways': sorted(names.intersection(priority))})

    # Preserve source geometry; overlay verified *local* name correspondences only.
    # OSM P3 at x~3450 is a different, wrongly named feature and is NOT closed.
    p3_ways = {179460847, 277202903, 277202904, 1525559583, 1525559584, 1075534451}
    te3_ways = {179458124, 179458113}
    overrides = {e['id']: 'P3' + (' · axe bleu' if 'bleu' in e['name'] else ' · axe orange' if 'orange' in e['name'] else '') for e in source_edges(p3_ways)}
    overrides.update({e['id']: 'TE3' for e in source_edges(te3_ways)})
    for name, way in [('Z3', 223198024), ('Z5', 157174701), ('Z6', 157174628)]:
        overrides.update({e['id']: name for e in source_edges([way])})
    # Construction-only closures and stand exclusions are omitted by request.
    # Keep the independently verified P3/TE3 name correspondences above.

    official_scope = {id for r in permanent for id in r['edgeIds']}
    supplemental = []
    superseded = 0
    for r in directions['edgeRules']:
        remaining = [id for id in r['edgeIds'] if id not in official_scope]
        superseded += len(r['edgeIds']) - len(remaining)
        if remaining:
            supplemental.append({**r, 'edgeIds': remaining})
    ledger.extend([
        {'id': 'circulation-config', 'section': '20.3.3.1 / GMC01–02', 'status': 'partial', 'description': 'Sens est/ouest issus des flèches AVISO avec contexte ARR/DEP des GMC actuelles ; calage limité et non certification exhaustive.'},
        {'id': 'runway-use', 'section': '20.3.2.1', 'status': 'enforced-standard-profile', 'description': 'Pistes intérieures préférentielles départs, extérieures préférentielles arrivées. Il s’agit d’un profil standard, pas d’une interdiction réelle de l’usage opposé sur clairance.'},
        {'id': 'long-tora-ppr', 'section': 'AD 2.13', 'status': 'enforced', 'description': 'Accès longs D1/Q6/T1/T2/R1 soumis à PPR, préavis30minutes ; désactivés sans approbation explicite. R1 en plus indisponible lorsque le dégivrage26R est en service (état non connecté).'},
        {'id': 'closed-source-taxiways', 'section': 'ADC02 / 20.3.2.1.1.1', 'status': 'absent-source-already-unroutable', 'description': 'D7/Q1/Y1A et Z4 fermés sur les sources publiées ; aucun axe routable portant ces noms dans le jeu inclus.'},
        {'id': 'hold-and-cross', 'section': '20.3.2.3 / 20.3.2.4', 'status': 'enforced', 'description': 'Points d’attente source avant entrée/traversée ; aucune autorisation n’est émise automatiquement.'},
        {'id': 'priority', 'section': '20.3.2.5.2 / 20.3.2.5.3', 'status': 'partial', 'description': 'Priorités sur Q envers Q2/Q3/Q4 et T envers T9/T10/T11 face est ; sur D envers D4/D5/D6 et T envers T4/T5/T6 face ouest. Arrêt et reprise automatiques aux 15 approches dont les branches sont appariées ; les autres jonctions restent non prouvées. Aucune clairance de piste délivrée automatiquement.'},
        {'id': 't1-between-satellites', 'section': '20.3.3.2.3', 'status': 'partial', 'description': 'Interdiction du transit par les lignes de stands, des demi-tours et des jonctions artificielles sans marquage prouvé ; contrôle exhaustif des mouvements A↔A3 entre satellites non disponible.'},
        {'id': 'unmarked-turns', 'section': '20.3.3.2.4.2', 'status': 'conservative-partial', 'description': 'Virages non marqués interdits en AIP. Les virages aux intersections synthétiques non prouvés sont rejetés ; la topologie OSM n’est pas une preuve de marquage peint.'},
        {'id': 't1-green-red', 'section': '20.3.4.3', 'status': 'advisory', 'description': 'GREEN/RED PUSH/PULL T1 sont des options de repoussage/tractage ; les flèches colorées ne sont pas imposées simultanément comme sens uniques.'},
        {'id': 'lvp', 'section': '20.3.2.1.6 / 20.3.6', 'status': 'unmapped', 'description': 'LVP non simulé : pas de bascule CATIII ni d’état des barres/RWSL en temps réel.'},
        {'id': 'deicing', 'section': '20.3.7 / GMC04–06', 'status': 'advisory', 'description': 'Entrée, circulation et sortie des baies de dégivrage sous clairance et gestionnaire ; processus non simulé.'},
        {'id': 'ice-shedding', 'section': '20.3.8', 'status': 'advisory', 'description': 'Points IS nécessitent clairance SOL ; pas de mise en puissance automatique.'},
        {'id': 'aircraft-restrictions', 'section': '20.3.3.2.1 / 20.3.3.2.2 / 20.3.3.3 / 20.3.3.4', 'status': 'excluded-by-request', 'description': 'Envergure, masse, type A388/B748/etc et restrictions moteur dépendantes du gabarit volontairement hors périmètre.'},
        {'id': 'temporary-closures', 'section': 'SUP151/26 / SUP188/26', 'status': 'excluded-by-request', 'description': 'Les fermetures de voies et de postes liées aux travaux SUP151/26 et SUP188/26 sont exclues à la demande de l’utilisateur.'},
    ])
    manifest = {'version': 1, 'airport': 'LFPG', 'effectiveDate': '2026-10-29', 'reviewDate': '2026-11-12', 'dataSha256': sha(data_file), 'sources': sources,
        'edgeRules': permanent + supplemental, 'turnRules': [], 'forbidUnmarkedSyntheticTurns': True, 'forbidUTurns': True, 'restrictParkingToEndpoints': True,
        'clearanceRules': clearances, 'priorityRules': priorities, 'operationalLocations': locations, 'runwayAccess': access,
        'runwayUse': {'east': {'departure': ['09R', '08L'], 'arrival': ['09L', '08R']}, 'west': {'departure': ['27L', '26R'], 'arrival': ['27R', '26L']}},
        'edgeNameOverrides': overrides, 'temporaryProfiles': [], 'ruleLedger': ledger,
        'coverage': {'completeIRLEquivalence': False, 'officialPermanentRules': len(permanent), 'supplementalDirectionRules': len(supplemental), 'supersededSupplementalEdges': superseded, 'realHoldingPointLocations': len(locations), 'clearanceCheckpoints': len(clearances), 'priorityCheckpoints': len(priorities), 'directionCoverage': directions['summary'], 'unmapped': unmapped, 'limitations': ['NOTAM temps réel absent.', 'Les GMC sont schématiques : ni la topologie OSM ni les flèches AVISO non datées ne prouvent tous les marquages et limites opérationnelles.', 'Les règles non prouvées restent identifiées ; aucune liaison ni restriction géographique fictive n’est ajoutée.', 'Une arrivée commence après dégagement extérieur avant la traversée intérieure, pas sur le seuil de la piste.', 'Le profil de piste est préférentiel et standard ; les exceptions sur clairance ne sont pas exhaustivement modélisées.']},
        'aliasDiagnostics': [{'sourceWayIds': sorted(p3_ways), 'currentName': 'P3', 'sourceIds': [GMC_E, GMC_W, 'sia-sup188-2026'], 'evidence': 'Axes du triple taxilane au sud/est du terminal2E, libellés AVISO P3 ; OSM E0 Axe central/orange/bleu obsolètes.'}, {'sourceWayIds': sorted(te3_ways), 'currentName': 'TE3', 'sourceIds': [GMC_E, GMC_W, 'sia-sup188-2026'], 'evidence': 'Liaison entre R et P2 au sud de P3 ; AVISO TE3, source OSM RP5/unnamed.'}, {'sourceWayIds': [170974835], 'status': 'unresolved-name-disagreement', 'evidence': 'Le ref OSM P3 est situé environ850m à l’est du P3 AVISO/AIP : ce ref ne sert pas à appliquer SUP188.'}]}
    assert all(r['edgeIds'] for r in manifest['edgeRules'])
    assert len({l['id'] for l in locations}) == len(locations)
    OUT.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'officialEdgeRules': len(permanent), 'supplementalDirectionRules': len(supplemental), 'holdingLocations': len(locations), 'clearances': len(clearances), 'priorities': len(priorities), 'unmapped': len(unmapped), 'constructionProfiles': 0}, indent=2))


if __name__ == '__main__':
    main()
