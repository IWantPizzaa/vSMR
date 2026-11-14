#!/usr/bin/env python3
"""Render the frozen source-edge overlay, not a browser screenshot or route."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    graph_path = ROOT / 'public/data/lfpg.json'
    catalog_path = ROOT / 'public/data/lfpg-aircraft-restrictions.json'
    graph = json.loads(graph_path.read_text(encoding='utf-8'))
    catalog = json.loads(catalog_path.read_text(encoding='utf-8'))
    assert catalog['dataSha256'] == digest(graph_path)
    for source in catalog['sources']:
        if source.get('file') and source.get('sha256'):
            assert digest(ROOT / source['file']) == source['sha256'], source['id']
    nodes = {node['id']: node for node in graph['nodes']}
    edges = {edge['id']: edge for edge in graph['edges']}
    font_path = Path('C:/Windows/Fonts/segoeui.ttf')
    bold_path = Path('C:/Windows/Fonts/segoeuib.ttf')
    def font(size: int, bold: bool = False):
        return ImageFont.truetype(str(bold_path if bold else font_path), size)
    profiles = [('A320', 35.8), ('B744', 64.92), ('B748', 68.4), ('A388', 79.75)]
    def matches(item: dict, code: str, span: float, flags: tuple[str, ...] = ()) -> bool:
        c = item.get('condition', {})
        if c.get('typeCodes') and code not in c['typeCodes']:
            return False
        if code in c.get('excludedTypeCodes', []):
            return False
        limits = c.get('wingspanMetres', {})
        if 'gt' in limits and span <= limits['gt'] or 'gte' in limits and span < limits['gte'] or 'lt' in limits and span >= limits['lt'] or 'lte' in limits and span > limits['lte']:
            return False
        return all(flag in flags for flag in c.get('airportFlags', []))
    image = Image.new('RGB', (2300, 1620), '#0f1c22')
    draw = ImageDraw.Draw(image)
    draw.text((35, 24), 'LFPG - restrictions de gabarit sur les voies source', font=font(36, True), fill='#e7eeee')
    draw.text((35, 78), 'Sans restrictions de travaux ; axes centraux ; SW libres | Orange : interdit | Vert : A380 seul | Rouge : virage interdit', font=font(21), fill='#b4c4c8')
    boxes = [(25, 130, 1135, 800), (1160, 130, 2270, 800), (25, 825, 1135, 1495), (1160, 825, 2270, 1495)]
    for (code, span), box in zip(profiles, boxes):
        x0, y0, x1, y1 = box
        draw.rounded_rectangle(box, radius=8, fill='#172b32', outline='#3b555f', width=2)
        base_flags = ()
        active = [rule for rule in catalog['edgeRules'] if matches(rule, code, span, base_flags)]
        blocked = {edge for rule in active for edge in rule['edgeIds']}
        green = {edge for rule in active if rule['id'] == 'gmc-A380-only-west-cargo-U-C' for edge in rule['edgeIds']}
        turns = {rule['viaNodeId'] for rule in catalog['turnRules'] if matches(rule, code, span)}
        draw.text((x0 + 18, y0 + 14), f'{code} - {span:g} m', font=font(27, True), fill='#e2eeee')
        draw.text((x0 + 18, y0 + 52), f'{len(blocked)} aretes interdites ; {len(turns)} jonctions avec au moins un virage interdit', font=font(18), fill='#bacacd')
        scale = min((x1 - x0 - 55) / 8750, (y1 - y0 - 145) / 4700)
        def xy(node_id: str):
            node = nodes[node_id]
            return (x0 + 25 + (node['x'] + 4300) * scale, y1 - 38 - (node['y'] + 2400) * scale)
        for edge in edges.values():
            color = '#3b525b' if edge['kind'] != 'runway' else '#718087'
            draw.line([xy(edge['from']), xy(edge['to'])], fill=color, width=1 if edge['kind'] != 'runway' else 3)
        for edge_id in blocked:
            edge = edges[edge_id]
            draw.line([xy(edge['from']), xy(edge['to'])], fill='#7cc064' if edge_id in green else '#ffc167', width=3)
        for node_id in turns:
            x, y = xy(node_id)
            draw.ellipse((x - 3, y - 3, x + 3, y + 3), outline='#ff8c85', width=2)
        for label, x, y in [('N', 425, 350), ('F', 775, 350), ('R', 2350, -1060), ('T', 2350, -1190), ('E', 1850, -100), ('D', -1050, 1070), ('B', 980, 1050), ('U/C', -900, -1650)]:
            px = x0 + 25 + (x + 4300) * scale
            py = y1 - 38 - (y + 2400) * scale
            draw.text((px, py), label, font=font(15, True), fill='#b0c2c7')
        extra_flags = base_flags + ('colored-dual-axis', 'southwest-deicing-occupied')
        with_flags = {edge for rule in catalog['edgeRules'] if matches(rule, code, span, extra_flags) for edge in rule['edgeIds']}
        draw.text((x0 + 18, y1 - 29), f'Options axes colores + SW occupees : {len(with_flags - blocked)} aretes supplementaires fermees', font=font(15), fill='#a7b8bc')
    draw.text((35, 1517), 'Audit de donnees geographiques, sans geometrie ajoutee ni capture navigateur. Les voies blanches de V2/V3 et de S sont preservees.', font=font(20), fill='#b2c6c9')
    draw.text((35, 1552), f'Graphe {digest(graph_path)[:16]}... | Catalogue {digest(catalog_path)[:16]}... | GMC07/08 11 JUN 2026, AIRAC 29 OCT 2026', font=font(18), fill='#8ea7ae')
    out = ROOT / 'docs/lfpg-aircraft-restrictions-review.png'
    image.save(out)
    print(out)


if __name__ == '__main__':
    main()
