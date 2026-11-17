#!/usr/bin/env python3
"""Compile exact source-node turn restrictions, without changing geography.

The current AIP's dangerous-turn figure is an inline image in AD2, not GMC03.
The selected incident branches below are independently reviewed against that
figure. A named pair alone never closes all similarly named taxiways.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'data-sources/aircraft/turn-restrictions.json'
AD2 = 'sia-lfpg-ad2-20261029'
MAP = 'sia-lfpg-forbidden-turn-map-20261029'
MARKINGS = 'sia-lfpg-unmarked-turn-figure-20261029'
BASE_URL = 'https://www.sia.aviation-civile.gouv.fr/media/dvd/eAIP_29_OCT_2026/FRANCE/AIRAC-2026-10-29/html/eAIP/Documents_AIP/MIA/AD/LFPG/'


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    source_file = ROOT / 'public/data/lfpg.json'
    data = json.loads(source_file.read_text(encoding='utf-8'))
    nodes = {node['id']: node for node in data['nodes']}
    edges = {edge['id']: edge for edge in data['edges']}
    operations = json.loads((ROOT / 'public/data/lfpg-operations.json').read_text(encoding='utf-8'))
    sources = [next(source for source in operations['sources'] if source['id'] == AD2)]
    for source_id, filename, title in [
        (MAP, 'viragedangereux_1025.png', 'SIA LFPG AD2.20.3.3.2.4.3 - carte des virages interdits les plus dangereux'),
        (MARKINGS, 'LFPG.20_croquis_virages_interdits.png', 'SIA LFPG AD2.20.3.3.2.4.2 - presence ou absence de ligne axiale courbe'),
    ]:
        path = ROOT / 'data-sources/aircraft' / filename
        sources.append({'id': source_id, 'title': title, 'url': BASE_URL + filename,
                        'file': path.relative_to(ROOT).as_posix(), 'sha256': sha(path),
                        'effectiveDate': '2026-10-29', 'capturedOn': '2026-11-13'})

    rules, groups, advisories = [], [], []

    def incident(edge_id: str, via: str) -> None:
        assert edge_id in edges and via in [edges[edge_id]['from'], edges[edge_id]['to']], (edge_id, via)

    def add(group: str, via: str, incoming: str, outgoing: str, reason: str, condition=None, source_ids=None):
        assert via in nodes
        incident(incoming, via); incident(outgoing, via)
        assert incoming != outgoing
        item = {'id': f'aircraft-turn:{group}:{len(rules):03d}', 'viaNodeId': via,
                'fromEdgeId': incoming, 'toEdgeId': outgoing, 'configuration': 'both',
                'sourceIds': source_ids or [AD2, MAP], 'reason': reason,
                'mappingGroup': group}
        if condition:
            item['condition'] = condition
        rules.append(item)

    def pairs(group: str, via: str, first: list[str], second: list[str], reason: str,
              reciprocal=True, condition=None, source_ids=None):
        begin = len(rules)
        for incoming in first:
            for outgoing in second:
                add(group, via, incoming, outgoing, reason, condition, source_ids)
                if reciprocal:
                    add(group, via, outgoing, incoming, reason, condition, source_ids)
        groups.append({'id': group, 'viaNodeId': via, 'firstEdgeIds': first, 'secondEdgeIds': second,
                       'reciprocal': reciprocal, 'condition': condition,
                       'ruleIds': [r['id'] for r in rules[begin:]], 'sourceIds': source_ids or [AD2, MAP],
                       'sourceWayIds': sorted({edges[e]['sourceWayId'] for e in first + second})})

    pairs('MD2-D', 'osm:1690796501', ['osm:222939612:0'],
          ['osm:156840791:43', 'osm:156840791:44'],
          'Virages directs MD2 vers D et D vers MD2 interdits au croisement non marque ; la continuation MD2/D5 et les raccordements courbes distincts sont conserves.')
    pairs('MD1-B', 'intersection:27', ['osm:156840762:9:part0', 'osm:156840762:9:part1'],
          ['osm:156840795:38:part1', 'osm:156840795:38:part2'],
          'Virages directs MD1/B interdits au croisement ; les deux lignes sources restent franchissables dans leur propre continuite.')
    pairs('MD2-B', 'intersection:29', ['osm:222939612:3:part0', 'osm:222939612:3:part1'],
          ['osm:156840795:38:part0', 'osm:156840795:38:part1'],
          'Virages directs MD2/B interdits au croisement non marque.')
    pairs('MD2-M', 'osm:1690901700', ['osm:222939599:0:part1'],
          ['osm:156849409:0', 'osm:222939611:0'],
          'Virages directs MD2/M interdits. Le court prolongement sud de MD2 porte le nom BM5 dans OSM ; seule cette transition terminale est concernee, pas toute BM5.')
    pairs('MD1-BM6', 'osm:1690902025', ['osm:156840762:4', 'osm:156840762:5'],
          ['osm:156849441:5', 'osm:156849441:6'],
          'Virages directs MD1/BM6 interdits au croisement des deux axes sources.')
    pairs('BD7-BD8', 'osm:1690795982', ['osm:156840739:7', 'osm:156840739:8'],
          ['osm:222939638:2', 'osm:222939638:3'],
          'Virages directs BD7/BD8 interdits au croisement ; les courbes propres de chaque raccordement restent utilisables.')
    pairs('B-A-west', 'osm:1690796506', ['osm:156840795:49'], ['osm:157180716:0'],
          'Branche B/A en retour de cap interdite au carrefour occidental du Terminal 1 ; la continuation tangentielle vers B est conservee.')
    pairs('A-B-east', 'osm:1279879806', ['osm:112636526:56'], ['osm:156840795:55'],
          'Branche A/B en retour de cap interdite au carrefour oriental du Terminal 1 ; la continuation tangentielle vers B ouest est conservee.')
    pairs('Y4-QD1', 'osm:8949536314', ['osm:967374627:0'], ['osm:967374610:0'],
          'Virage Y4 vers QD1 interdit dans ce sens. Le prolongement Y4 est non nomme et la courbe QD1 porte le nom Q dans OSM.', reciprocal=False)
    pairs('FN2-F', 'osm:1712613144', ['osm:222934041:0'],
          ['osm:172796083:13', 'osm:172796083:14'],
          'Virages directs FN2/F interdits ; la continuation E/FN2 et les courbes marquees distinctes restent disponibles.')
    pairs('FN3-F-west-junction', 'osm:43210348', ['osm:59465741:6'],
          ['osm:172796083:16', 'osm:172796083:17:part0'],
          'Virages directs FN3/F interdits ; le raccordement G3/F est distinct et conserve.')
    pairs('FN3-F-source-crossing', 'intersection:3', ['osm:59465743:10:part0', 'osm:59465743:10:part1'],
          ['osm:172796083:17:part0', 'osm:172796083:17:part1'],
          'Virages directs FN3/F interdits au second croisement source rapproche ; aucune fermeture de FN3 ni de F.')

    # The eastern A3 endpoint adjoins the A bridge approach. Do not copy this
    # restriction onto the western A3 access, which is a separate physical gate.
    pairs('A-bridge-A3-east-span36', 'osm:1279879907', ['osm:112642878:57'], ['osm:172859840:109'],
          'Envergure superieure a 36 m : virage entre le pont de A et A3 interdit dans les deux sens. La limite generale A3 de 61 m est une restriction separee.',
          condition={'wingspanMetres': {'gt': 36}}, source_ids=[AD2])
    pairs('A-DA1-span36', 'osm:1693909137', ['osm:112642878:0', 'osm:157180716:26'], ['osm:157174702:0'],
          'Envergure superieure a 36 m : virage depuis A vers DA1 interdit. Le texte ne ferme pas automatiquement le mouvement inverse DA1 vers A.',
          reciprocal=False, condition={'wingspanMetres': {'gt': 36}}, source_ids=[AD2])

    # A380 TA3 -> R east is an oversteering advisory, not a prohibited turn.
    curve = [edge for edge in edges.values() if edge.get('sourceWayId') == 1527163957]
    for incoming in curve:
        for via in [incoming['from'], incoming['to']]:
            for outgoing in edges.values():
                if outgoing['name'] != 'R' or via not in [outgoing['from'], outgoing['to']]:
                    continue
                other = outgoing['to'] if outgoing['from'] == via else outgoing['from']
                if nodes[other]['x'] > nodes[via]['x']:
                    advisories.append({'id': 'aircraft-advisory:A388-TA3-R-east-oversteer', 'configuration': 'both',
                                       'viaNodeId': via, 'fromEdgeId': incoming['id'], 'toEdgeId': outgoing['id'],
                                       'condition': {'typeCodes': ['A388']}, 'sourceIds': [AD2],
                                       'reason': 'A380 : pratiquer l’oversteering dans le virage TA3 vers R cap a l’est ; obstacle a 47,5 m du rayon de virage (AD2.20.3.3.3.4). Cette precaution ne ferme pas le raccordement.'})
    assert len(advisories) == 1, advisories
    assert len({(rule['viaNodeId'], rule['fromEdgeId'], rule['toEdgeId']) for rule in rules}) == len(rules)
    result = {'version': 1, 'airport': 'LFPG', 'dataSha256': sha(source_file), 'reviewDate': '2026-11-13',
              'sources': sources, 'edgeRules': [], 'turnRules': rules, 'advisories': advisories, 'groups': groups,
              'coverage': {'unconditionalDirectedTurns': sum('condition' not in rule for rule in rules),
                           'conditionalDirectedTurns': sum('condition' in rule for rule in rules),
                           'genericUnmarkedTurns': 'The AIP rule applies everywhere, but this layer digitizes the specifically illustrated dangerous junctions. Existing synthetic-crossing continuity and no-U-turn controls remain independent.',
                           'A3BridgeScope': 'Eastern bridge-side approach only (A segment57 / A3 segment109); the western A3 access is not treated as the bridge without source proof.',
                           'MD2southScope': 'The short source continuation222939599 is tagged BM5; only its terminal junction with M is mapped as the prohibited MD2/M crossing.'}}
    OUT.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'output': str(OUT), 'groups': len(groups), 'turnRules': len(rules), 'advisories': len(advisories)}, indent=2))


if __name__ == '__main__':
    main()
