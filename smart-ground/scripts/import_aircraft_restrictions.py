#!/usr/bin/env python3
"""Compile source-scoped LFPG wingspan/type restrictions on the immutable graph.

The AIP colour charts are read visually. Their names, source-way identities and
published endpoints are recorded explicitly; no coloured raster proximity mask
or invented graph connection is used to determine permission.
"""
from __future__ import annotations

import hashlib
import heapq
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AD2 = 'sia-lfpg-ad2-20261029'
GMC07 = 'sia-lfpg-gmc07-20261029'
GMC08 = 'sia-lfpg-gmc08-20261029'


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    graph_path = ROOT / 'public/data/lfpg.json'
    graph = json.loads(graph_path.read_text(encoding='utf-8'))
    corridor = json.loads((ROOT / 'public/data/lfpg-corridors.json').read_text(encoding='utf-8'))
    operating = json.loads((ROOT / 'public/data/lfpg-operations.json').read_text(encoding='utf-8'))
    nodes = {node['id']: node for node in graph['nodes']}
    edges = {edge['id']: edge for edge in graph['edges']}
    taxi = [edge for edge in graph['edges'] if edge['kind'] == 'taxiway']
    axes = {axis['id']: axis for axis in corridor['axes']}
    zones = {zone['id']: zone for zone in corridor['zones']}
    main_ids = {edge for axis in corridor['axes'] if axis['role'] == 'main' for edge in axis['edgeIds']}
    sources = [source for source in operating['sources'] if source['id'] in {AD2, 'osm-lfpg-holding-20261112', 'aviso-lfpg-local-directions', 'sia-sup188-2026'}]
    for number, source_id in [(7, GMC07), (8, GMC08)]:
        path = ROOT / f'data-sources/aircraft/lfpg-gmc-{number:02d}-20261029.pdf'
        sources.append({'id': source_id, 'title': f'SIA LFPG GMC {number:02d} - A380 / B748 / A124 / C5', 'url': f'https://www.sia.aviation-civile.gouv.fr/media/dvd/eAIP_29_OCT_2026/FRANCE/AIRAC-2026-10-29/html/eAIP/Cartes/LFPG/AD_2_LFPG_GMC_{number:02d}.pdf', 'file': path.relative_to(ROOT).as_posix(), 'sha256': sha(path), 'effectiveDate': '2026-06-11', 'edition': 'AIRAC 29 OCT 2026', 'capturedOn': '2026-11-13'})
    easa_path = ROOT / 'data-sources/aircraft/easa-aerodrome-reference-code-202603.html'
    sources.append({'id': 'easa-aerodrome-reference-code-202603', 'title': 'EASA CS ADR-DSN.A.005 Table A-1, revision March 2026', 'url': 'https://www.easa.europa.eu/en/document-library/easy-access-rules/online-publications/easy-access-rules-aerodromes-regulation-eu?erules-id=ERULES-1963177438-2162', 'file': easa_path.relative_to(ROOT).as_posix(), 'sha256': sha(easa_path), 'effectiveDate': '2026-03-13', 'capturedOn': '2026-11-13'})
    rules: list[dict] = []
    mapping: list[dict] = []
    unmapped: list[dict] = []

    def named(name: str) -> list[dict]:
        return [edge for edge in taxi if edge['name'] == name]

    def ways(*ids: int) -> list[dict]:
        return [edge for edge in taxi if edge.get('sourceWayId') in ids]

    def axis(name: str) -> list[dict]:
        return [edges[edge] for edge in axes[name]['edgeIds'] if edges[edge]['kind'] == 'taxiway']

    def rule(rule_id: str, selected: list[dict], reason: str, *, condition: dict, source_ids: list[str] | None = None, scope: str, clause: str = '20.3.3.2.2', configuration: str = 'both') -> None:
        selected = list({edge['id']: edge for edge in selected}.values())
        if not selected:
            unmapped.append({'id': rule_id, 'clause': clause, 'reason': reason, 'mappingStatus': 'no-verified-existing-source-chain'})
            return
        ids = sorted(edge['id'] for edge in selected)
        rules.append({'id': rule_id, 'edgeIds': ids, 'configuration': configuration, 'direction': 'closed', 'condition': condition, 'sourceIds': source_ids or [AD2], 'reason': reason, 'clause': clause, 'scope': scope})
        mapping.append({'id': rule_id, 'sourceWayIds': sorted({edge['sourceWayId'] for edge in selected}), 'edgeCount': len(selected), 'scope': scope, 'clause': clause})

    def max_span(rule_id: str, selected: list[dict], metres: float, description: str, *, flags: list[str] | None = None, scope: str) -> None:
        condition: dict = {'wingspanMetres': {'gt': metres}}
        if flags:
            condition['airportFlags'] = flags
        rule(rule_id, selected, f'{description} : envergure maximale {metres:g} m incluse (interdit au-dessus).', condition=condition, scope=scope)

    max_span('aip-wingspan-A3-61', named('A3'), 61, 'A3', scope='Entire source C-chain 172859840, not the outer A ring or its feeders')
    max_span('aip-wingspan-G1-33', ways(59465748), 33, 'G1', scope='OSM G1W source chain under the AVISO G1 label, not G2 or G3')
    max_span('aip-wingspan-G2-36', ways(59465750), 36, 'G2', scope='OSM G2W source chain under the AVISO G2 label')
    max_span('aip-wingspan-C2-36', ways(179615757), 36, 'C2', scope='Existing unnamed source 179615757 under AVISO C2 label; C main excluded')
    for name in ['E1', 'E2', 'E10', 'E11', 'E13', 'P4', 'P5', 'BM1']:
        max_span(f'aip-wingspan-{name}-36', axis(name) if name in axes else named(name), 36, name, scope=f'Published {name} source footprint only')
    # E12 is also a stand name in terminal2E. Only the taxiway in Juliett is
    # selected, never the unrelated E12 parking lead.
    max_span('aip-wingspan-E12-36', ways(152634782), 36, 'E12 de Juliett', scope='Eastern J-area taxiway E12 way152634782 only; unrelated stand E12 parking way1531265018 is explicitly excluded')
    for name in ['A4', 'G3', 'G4', 'P3', 'TB2']:
        max_span(f'aip-colored-axis-{name}-36', axis(name) if name in axes else named(name), 36, f'{name}, axes orange/bleu selectionnes', flags=['colored-dual-axis'], scope=f'Only the orange/blue operating mode on the published {name} footprint; single central OSM line is not relabelled as two parallel paths')
    max_span('aip-wingspan-EM23-52', named('EM23'), 52, 'EM23', scope='Named source chains180375120/237392456')
    en81 = next(location for location in graph['locations'] if location['id'] == 'stand:EN81')
    en81_leads = [edge for edge in graph['edges'] if edge['kind'] == 'parking_position' and en81['nodeId'] in [edge['from'], edge['to']]]
    en81_join = {edge['to'] if edge['from'] == en81['nodeId'] else edge['from'] for edge in en81_leads}
    assert len(en81_join) == 1
    en81_y = nodes[next(iter(en81_join))]['y']
    em5_south = [edge for edge in named('EM5') if max(nodes[edge[key]]['y'] for key in ['from', 'to']) <= en81_y + .01]
    max_span('aip-wingspan-EM5-from-EN81-36', em5_south, 36, 'EM5 depuis EN81 vers les postes EN82/EN83', scope=f'Southward existing EM5 chain at and beyond the exact EN81 parking-lead junction {next(iter(en81_join))}; north part is not extended arbitrarily')
    max_span('aip-wingspan-FN4-36', named('FN4'), 36, 'FN4', scope='FN4 chain59465739; the additional actual taxi mass >80t clause is recorded separately, not inferred from MTOW')
    max_span('aip-wingspan-WT-36', [edges['osm:229282487:3']], 36, 'WT', scope='Only the 9.7 m central common stem between source nodes2379042571/2379042572 under the WT label; approach curves and W4 runway segments remain outside this restriction')

    # Text AD2.20.3.3.2.2 explicitly delegates >65m restrictions to GMC07/08.
    # The orange shading is the same physical restriction in both configurations;
    # green is A380-only and must not be inherited by B748/B744/custom 66m.
    heavy_condition = {'wingspanMetres': {'gt': 65}}
    chart_sources = [AD2, GMC07, GMC08]
    for name in ['A3', 'A4', 'G3', 'G4', 'TB2', 'P1', 'P2', 'P3', 'P4', 'P5']:
        rule(f'gmc-codeF-{name}', axis(name) if name in axes else named(name), f'GMC07/08 : {name} figure en orange, interdit aux avions d’envergure superieure a 65 m.', condition=heavy_condition, source_ids=chart_sources, clause='20.3.3.3.2 / GMC07-08 orange', scope=f'Chart-shaded named {name} chain; exact mapped corridor source IDs')
    for name in ['G2W', 'E1', 'E2', 'E3', 'E4', 'E9', 'E10', 'E11', 'E13', 'BM1', 'BM2', 'BM3', 'BM5', 'BM6', 'BD7', 'BD8', 'EM2', 'EM23', 'FN2', 'FN3', 'FN4', 'Y2', 'Z5', 'V4', 'V5', 'C1']:
        rule(f'gmc-codeF-{name}', named(name), f'GMC07/08 : {name} figure en orange, interdit aux avions d’envergure superieure a 65 m.', condition=heavy_condition, source_ids=chart_sources, clause='20.3.3.3.2 / GMC07-08 orange', scope=f'Published chart {name} footprint on named existing source chains')
    rule('gmc-codeF-E12-Juliett', ways(152634782), 'GMC07/08 : E12 de Juliett interdit au-dessus de 65 m d’envergure.', condition=heavy_condition, source_ids=chart_sources, clause='20.3.3.3.2 / GMC07-08 orange', scope='Eastern J-area E12 taxiway only; not the unrelated terminal2E stand E12 parking lead')
    rule('gmc-codeF-E-west-GE1', ways(222948626), 'GMC07/08 : E entre FN2 et le carrefour B/GE1 interdit au-dessus de 65 m d’envergure.', condition=heavy_condition, source_ids=chart_sources, clause='20.3.3.3.2 / GMC07-08 orange; HS7E/HS10W', scope='Horizontal E source222948626 only; the eastern E chain170450475 stays outside this restriction')
    rule('gmc-codeF-M', named('M'), 'GMC07/08 : M interdit en roulage autonome au-dessus de 65 m d’envergure.', condition=heavy_condition, source_ids=chart_sources, clause='20.3.3.3.2 / GMC07-08 orange', scope='M source156849409/222939611, including western EM1-labelled part; towing approval is not autonomous-taxi authorization')
    bd1_joint = {edge[key] for edge in named('B') for key in ['from', 'to']} & {edge[key] for edge in named('BD1') for key in ['from', 'to']}
    assert len(bd1_joint) == 1
    bd1_x = nodes[next(iter(bd1_joint))]['x']
    b_west = [edge for edge in named('B') if max(nodes[edge[key]]['x'] for key in ['from', 'to']) <= bd1_x + 1e-6]
    rule('gmc-codeF-B-FedEx', b_west, 'GMC07/08 : B entre BM2/BM3 et BD1 interdit en roulage autonome au-dessus de 65 m d’envergure.', condition=heavy_condition, source_ids=chart_sources, clause='20.3.3.3.2 / GMC07-08 orange', scope='Existing western B chain before its BD1 junction; published eastern B main corridor is explicitly retained')
    md2_south = {edge for item in operating['edgeRules'] if item['id'].startswith('aip-md2-away-runway:') for edge in item['edgeIds']}
    rule('gmc-codeF-MD2-south-B', [edges[edge] for edge in md2_south], 'GMC07/08 : partie MD2 entre B et M interdite au-dessus de 65 m d’envergure.', condition=heavy_condition, source_ids=chart_sources, clause='20.3.3.3.2 / GMC07-08 orange', scope='Exact existing B-to-M source subchain used by the independently sourced MD2 no-entry rule; D-to-B portion remains outside this rule')
    for zone_id in ['terminal:H', 'apron:J']:
        selected = [edges[edge] for edge in zones[zone_id]['edgeIds'] if edges[edge]['kind'] == 'taxiway' and edge not in main_ids]
        rule(f'gmc-codeF-{zone_id.split(":")[-1]}-apron', selected, f'GMC07/08 : voies de desserte de l’aire {zone_id.split(":")[-1]} en orange, interdites au-dessus de 65 m d’envergure.', condition=heavy_condition, source_ids=chart_sources, clause='20.3.3.3.2 / GMC07-08 orange', scope=f'Existing local taxiway component {zone_id}; published principal axes are explicitly excluded from apron closure')
    # The orange S portions terminate at actual V3/V4 and V5/S6 junctions.
    # Neither the white S1/S2/S3 frontage nor S6-to-S9 is closed wholesale.
    s_orange = [edges['osm:222951672:0'], edges['osm:515164109:0']] + ways(515164108, 515164111)
    rule('gmc-codeF-S-partial', s_orange, 'GMC07/08 : deux portions de S en orange entre V3/V4 et V5/S6, interdites au-dessus de 65 m.', condition=heavy_condition, source_ids=chart_sources, clause='20.3.3.3.2 / GMC07-08 orange', scope='S left:222951672:0+515164111; S right:515164108+515164109:0. White S west of V3 and east of S6, V2 and the main V3 diagonal are preserved.')
    rule('gmc-codeF-V3-runway-branch', ways(158959145), 'GMC07/08 : raccord complementaire V3 vers la piste 08R/26L interdit au-dessus de 65 m ; la diagonale V3 principale reste disponible.', condition=heavy_condition, source_ids=chart_sources, clause='20.3.3.3.2 / GMC07-08 orange', scope='Seven-edge existing branch158959145 from V3 node1710701966 to RWY node1710701947, no intermediate alternative. Source orange is its runway-end portion; the white main V3 chain158959143 is not included.')
    # The chart legend names these types explicitly. Retain that independent
    # type restriction as well as the general >65m rule delegated by AD2, so a
    # falsely understated custom span cannot erase a published type prohibition.
    orange_ids = {edge for item in rules if item['id'].startswith('gmc-codeF-') for edge in item['edgeIds']}
    rule('gmc-codeF-explicit-types', [edges[edge] for edge in orange_ids], 'GMC07/08 : emprise orange interdite aux types A380, B747-8, A124 et famille C5 explicitement cites dans la legende.', condition={'typeCodes': ['A388', 'B748', 'A124', 'C5', 'C5A', 'C5B', 'C5C', 'C5M']}, source_ids=[GMC07, GMC08], clause='GMC07/08 orange legend', scope='Same reviewed orange source chains, type condition independent of declared wingspan; B744 is deliberately not included. C5A/B/C/M are accepted user family aliases of chart C5, not claims of separate ICAO designators.')

    # Green A380-only U/C cargo footprint. The eastern UT1-to-N/F frontage stays
    # available; no aircraft-family rule is extrapolated to B747-400/B747-8.
    ut1_nodes = {edge[key] for edge in named('UT1') for key in ['from', 'to']}
    green_boundaries = {}
    for name in ['U', 'C']:
        junctions = ut1_nodes & {edge[key] for edge in named(name) for key in ['from', 'to']}
        assert len(junctions) == 1, ('UT1 source boundary', name, junctions)
        green_boundaries[name] = next(iter(junctions))
    u_cargo = [edge for edge in named('U') if max(nodes[edge[key]]['x'] for key in ['from', 'to']) <= nodes[green_boundaries['U']]['x'] + 1e-6]
    c_cargo = [edge for edge in named('C') if max(nodes[edge[key]]['x'] for key in ['from', 'to']) <= nodes[green_boundaries['C']]['x'] + 1e-6]
    a380_cargo = u_cargo + c_cargo + ways(179602433, 180964947) + [edge for name in ['UC1', 'UC2', 'UC3', 'UC4', 'UC5'] for edge in named(name)]
    rule('gmc-A380-only-west-cargo-U-C', a380_cargo, 'GMC07/08 : emprise cargo U/C/UC1–UC5 en vert, interdite uniquement a l’A380.', condition={'typeCodes': ['A388']}, source_ids=[GMC07, GMC08], clause='GMC07/08 green legend', scope=f'Western U/C source chains and transverse UC links up to the exact UT1 west-mouth source nodes {green_boundaries}; UT1 and east-of-UT1 U/C frontage are not closed by type')

    # Conditional T between T1 and T2 is compiled only if both exact source
    # junctions are available. This never becomes an unconditional 36m closure.
    t_ids = set(axes['T']['edgeIds'])
    t_adjacency: dict[str, list[tuple[str, str, float]]] = {}
    for edge_id in t_ids:
        edge = edges[edge_id]
        for a, b in [(edge['from'], edge['to']), (edge['to'], edge['from'])]:
            t_adjacency.setdefault(a, []).append((b, edge_id, edge['length']))
    def t_junctions(name: str) -> set[str]:
        candidates = {edge[key] for edge in named(name) for key in ['from', 'to']}
        return candidates & t_adjacency.keys()
    def path(start: str, target: str) -> list[dict]:
        best = {start: 0.0}; previous: dict[str, tuple[str, str]] = {}; heap = [(0.0, start)]
        while heap:
            distance, node = heapq.heappop(heap)
            if distance != best[node]:
                continue
            if node == target:
                result = []
                while node != start:
                    node, edge_id = previous[node]; result.append(edges[edge_id])
                return result
            for next_node, edge_id, length in t_adjacency.get(node, []):
                candidate = distance + length
                if candidate < best.get(next_node, float('inf')):
                    best[next_node] = candidate; previous[next_node] = (node, edge_id); heapq.heappush(heap, (candidate, next_node))
        return []
    candidates = [selected for a in t_junctions('T1') for b in t_junctions('T2') if (selected := path(a, b))]
    if candidates:
        selected = min(candidates, key=lambda selected: sum(edge['length'] for edge in selected))
        max_span('aip-wingspan-T-T1-T2-occupied-SW-36', selected, 36, 'T entre T1 et T2 si une baie SW1–SW4 est occupee par un avion en attente', flags=['southwest-deicing-occupied'], scope='T-source path between the genuine source T1 and T2 junctions, activated only by the explicit SW-pad scenario flag')
    else:
        unmapped.append({'id': 'T-T1-T2-occupied-SW', 'clause': '20.3.3.2.2', 'reason': 'No exact T1/T2-to-T main junction pair in the pinned graph; no broad arbitrary T closure created.'})

    # Construction-only stand limits (SUP188 E15/E34 code C) are omitted
    # by request; permanent type, span, turn and scenario rules stay intact.

    turns_path = ROOT / 'data-sources/aircraft/turn-restrictions.json'
    turn_data = json.loads(turns_path.read_text(encoding='utf-8')) if turns_path.exists() else {}
    known_source_ids = {source['id'] for source in sources}
    for source in turn_data.get('sources', []):
        if source['id'] not in known_source_ids:
            sources.append(source); known_source_ids.add(source['id'])
    turn_rules = turn_data.get('turnRules', [])
    advisories = turn_data.get('advisories', [])
    unmapped += turn_data.get('coverage', {}).get('unmapped', [])
    unmapped += [
        {'id': 'FN4-actual-mass-80t', 'clause': '20.3.3.2.2', 'reason': 'The additional FN4 actual mass >80t restriction is not inferred from a profile MTOW; actual taxi mass is outside this wingspan catalogue.'},
        {'id': 'obstacle-clearance-not-wingspan', 'clause': '20.3.3.2.1 / 20.3.3.3.4', 'reason': 'Obstacle clearance strip widths and the list of TWYs narrower than23m are not converted into fictitious wingspan limits. Wingtip clearance, oversteering and obstacle height require a separate physical model.'},
        {'id': 'towing-A380-B748', 'clause': '20.3.3.3.6', 'reason': 'Towing approvals on M/B/BM3/BM5/BM6/EM7/EM8 do not authorize autonomous taxi and do not bypass route exclusions. Towing remains a separate unmodelled mode.'},
        {'id': 'engine-and-bridge-cooccupancy', 'clause': '20.3.3.3 / 20.3.3.4', 'reason': 'Engine thrust procedures and simultaneous bridge occupancy cannot be established by a static wingspan check; no such authorization is implied.'},
    ]
    manifest = {'version': 1, 'airport': 'LFPG', 'effectiveDate': '2026-10-29', 'reviewDate': '2026-11-13', 'dataSha256': sha(graph_path), 'sources': sources, 'edgeRules': rules, 'turnRules': turn_rules, 'advisories': advisories, 'airportFlags': [{'id': 'colored-dual-axis', 'default': False, 'reason': 'Use of orange/blue dual taxilane axes instead of the central yellow line.'}, {'id': 'southwest-deicing-occupied', 'default': False, 'reason': 'An aircraft is waiting on SW1–SW4 deicing bays.'}], 'mapping': mapping, 'coverage': {'completeIRLEquivalence': False, 'mappedEdgeRules': len(rules), 'mappedTurnRules': len(turn_rules), 'turnMapping': turn_data.get('coverage', {}), 'typeFamilyAliases': {'C5': ['C5A', 'C5B', 'C5C', 'C5M']}, 'unmapped': unmapped, 'limitations': ['The chart is schematic; only reviewed existing source chains are selected.', 'B747-400 and B747-8 are distinct profiles. The >65m rule does not apply to B747-400 merely because its family name is B747.', 'Orange/blue lane mode enforces its published 36m restriction but does not create two offset physical tracks in the single-centreline graph.', 'Stand aircraft-capacity compatibility is not inferred from a terminal name.', 'Current NOTAM and real controller clearances are not downloaded in real time.']}}
    source_ids = {source['id'] for source in sources}
    assert len(source_ids) == len(sources)
    for item in rules:
        assert item['edgeIds'] and len(item['edgeIds']) == len(set(item['edgeIds']))
        assert all(edge in edges for edge in item['edgeIds'])
        assert all(source in source_ids for source in item['sourceIds'])
    out = ROOT / 'public/data/lfpg-aircraft-restrictions.json'
    out.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'file': str(out), 'edgeRules': len(rules), 'turnRules': len(turn_rules), 'uniqueRestrictedEdges': len({edge for item in rules for edge in item['edgeIds']}), 'advisories': len(advisories), 'unmapped': len(unmapped), 'sha256': sha(out)}, indent=2))


if __name__ == '__main__':
    main()
