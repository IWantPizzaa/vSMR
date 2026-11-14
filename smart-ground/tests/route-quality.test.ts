import { strict as assert } from 'node:assert';
import { test } from 'node:test';
import { AircraftSimulator, OperatingRules, RoutePlanner, TaxiFlowPolicy, TaxiGraph } from '../src/core';
import { RouteQuality } from '../src/core/RouteQuality';
import type { AirportData } from '../src/core/types';
import type { TaxiFlowManifest } from '../src/core/TaxiFlowPolicy';

function fixture(points: [string, number, number][], pairs: [string, string][]) {
  const nodes = points.map(([id, x, y]) => ({ id, x, y, lat: 49, lon: 2 })), byId = new Map(nodes.map(n => [n.id, n]));
  const data: AirportData = { nodes, edges: pairs.map(([from, to], index) => ({ id: `e${index}`, name: `e${index}`, from, to, kind: 'taxiway', length: Math.hypot(byId.get(to)!.x - byId.get(from)!.x, byId.get(to)!.y - byId.get(from)!.y) })),
    locations: nodes.map(n => ({ ...n, id: `stand:${n.id}`, nodeId: n.id, name: n.id, kind: 'stand' })), surfaces: [], metadata: {} };
  const graph = new TaxiGraph(data);
  const rules = new OperatingRules(graph, { version: 1, airport: 'LFPG', effectiveDate: '2026-10-29', sources: [{ id: 'source', title: 'Synthetic source', url: 'https://example.test/source' }], edgeRules: [], forbidUTurns: true });
  const manifest: TaxiFlowManifest = { version: 1, airport: 'LFPG', sources: rules.manifest.sources, axes: [], zones: [], flowPolicies: [] };
  const planner = new RoutePlanner(graph, rules, 'west');
  const install = () => { const policy = new TaxiFlowPolicy(graph, manifest); assert.ok(policy.valid, policy.diagnostics.join(';')); planner.setFlowPolicy(policy); return policy; };
  return { data, graph, rules, manifest, planner, install };
}

test('a tiny shortcut across parallel main axes loses to continuous use of the source axis', () => {
  const f = fixture([['S', 0, 0], ['A', 100, 0], ['B', 150, 15], ['C', 250, 15], ['D', 300, 0], ['G', 400, 0], ['X', 150, -5], ['Y', 250, -5]],
    [['S', 'A'], ['A', 'B'], ['B', 'C'], ['C', 'D'], ['D', 'G'], ['A', 'X'], ['X', 'Y'], ['Y', 'D']]);
  const shortest = f.planner.findRoute('S', 'G')!;
  assert.deepEqual(shortest.edgeIds, ['e0', 'e5', 'e6', 'e7', 'e4']);
  f.manifest.axes = [{ id: 'N', name: 'N', role: 'main', edgeIds: ['e0', 'e1', 'e2', 'e3', 'e4'], sourceIds: ['source'] },
    { id: 'F', name: 'F', role: 'main', edgeIds: ['e6'], sourceIds: ['source'] },
    { id: 'RN', name: 'RN', role: 'connector', edgeIds: ['e5', 'e7'], sourceIds: ['source'] }];
  f.install(); const route = f.planner.findRoute('S', 'G')!;
  assert.deepEqual(route.edgeIds, ['e0', 'e1', 'e2', 'e3', 'e4']);
  assert.deepEqual(route.flow?.axes, ['N']); assert.equal(route.flow?.mainAxisChanges, 0);
  assert.ok(route.distance > shortest.distance); assert.ok(route.distance - shortest.distance < 5);
  assert.equal(route.flow?.physicalDistance, route.distance); assert.ok(f.planner.isRouteAllowed(route));
  const replay = f.planner.describeRoute(route); assert.equal(replay.flow?.routingCostMetres, route.flow?.routingCostMetres);
});

test('turn and axis costs remain bounded, so a very long detour is not preferred to two necessary changes', () => {
  const f = fixture([['S', 0, 0], ['A', 100, 0], ['B', 100, 500], ['C', 300, 500], ['D', 300, 0], ['G', 400, 0], ['X', 150, -5], ['Y', 250, -5]],
    [['S', 'A'], ['A', 'B'], ['B', 'C'], ['C', 'D'], ['D', 'G'], ['A', 'X'], ['X', 'Y'], ['Y', 'D']]);
  f.manifest.axes = [{ id: 'N', name: 'N', role: 'main', edgeIds: ['e0', 'e1', 'e2', 'e3', 'e4'], sourceIds: ['source'] },
    { id: 'F', name: 'F', role: 'main', edgeIds: ['e6'], sourceIds: ['source'] }, { id: 'RN', name: 'RN', role: 'connector', edgeIds: ['e5', 'e7'], sourceIds: ['source'] }];
  f.install(); const route = f.planner.findRoute('S', 'G')!;
  assert.deepEqual(route.edgeIds, ['e0', 'e5', 'e6', 'e7', 'e4']); assert.equal(route.flow?.mainAxisChanges, 2);
  assert.ok(route.distance < 410); assert.ok(route.flow!.routingCostMetres! < 700);
});

test('densifying a turning curve does not invent additional manoeuvres or alter a junction tangent', () => {
  const points: [string, number, number][] = [['S', -100, 0], ['J', 0, 0], ['A', 10, 0], ['B', 20, 5], ['C', 30, 15], ['D', 35, 30], ['G', 35, 100], ['W', 100, 0]];
  const pairs: [string, string][] = [['S', 'J'], ['J', 'A'], ['A', 'B'], ['B', 'C'], ['C', 'D'], ['D', 'G'], ['J', 'W']];
  const coarse = fixture(points, pairs), densePoints = [...points], densePairs: [string, string][] = [];
  for (const [a, b] of pairs) {
    const start = points.find(p => p[0] === a)!, end = points.find(p => p[0] === b)!;
    const id = `${a}-${b}`; densePoints.push([id, (start[1] + end[1]) / 2, (start[2] + end[2]) / 2]); densePairs.push([a, id], [id, b]);
  }
  const dense = fixture(densePoints, densePairs);
  function measure(f: ReturnType<typeof fixture>) {
    const route = f.planner.findRoute('S', 'G')!, quality = new RouteQuality(f.graph);
    return route.edgeIds.reduce((sum, id, i) => { const edge = f.graph.edges.get(id)!, incoming = i ? route.edgeIds[i - 1] : null;
      const q = quality.junction(incoming, route.nodeIds[i], edge), curvature = quality.curvature(incoming, route.nodeIds[i], edge);
      return { turns: sum.turns + q.significantTurns, cost: sum.cost + q.turnCostMetres, curvature: sum.curvature + curvature.degrees };
    }, { turns: 0, cost: 0, curvature: 0 });
  }
  const a = measure(coarse), b = measure(dense);
  assert.equal(a.turns, 1); assert.equal(b.turns, 1); assert.ok(Math.abs(a.cost - b.cost) < 1e-8);
  assert.ok(Math.abs(a.curvature - b.curvature) < 1e-8, 'Collinear subdivisions must not multiply or change total curvature.');
});

test('a sampled S bend has a finite curvature cost even when no curve vertex is a junction', () => {
  const f = fixture([['S', 0, 0], ['A', 100, 0], ['B', 150, 50], ['C', 200, 0], ['D', 300, 0], ['G', 400, 0], ['U', 200, 100]],
    [['S', 'A'], ['A', 'B'], ['B', 'C'], ['C', 'D'], ['D', 'G'], ['S', 'U'], ['U', 'G']]);
  const shortest = f.planner.findRoute('S', 'G')!;
  assert.deepEqual(shortest.edgeIds, ['e0', 'e1', 'e2', 'e3', 'e4']);
  f.install(); const route = f.planner.findRoute('S', 'G')!;
  assert.deepEqual(route.edgeIds, ['e5', 'e6'], 'Choose a small distance increase over the otherwise invisible S bend.');
  assert.equal(route.flow?.significantTurns, 0, 'Degree-two curve vertices remain separate from the junction counter.');
  assert.ok(Math.abs(route.flow!.curvatureDegrees! - 53.13010235415598) < 1e-8);
  assert.equal(route.flow?.curvatureCostMetres, route.flow?.curvatureDegrees, '90 equivalent metres per 90 degrees.');
  assert.ok(Math.abs(route.flow!.routingCostMetres! - route.distance - route.flow!.curvatureCostMetres!) < 1e-8, 'Junction diagnostics must not be added a second time.');
});

test('a holding-approach profile matches its physical destination node and validates that reference', () => {
  const f = fixture([['S', 0, 0], ['A', 100, 0], ['G', 200, 0]], [['S', 'A'], ['A', 'G']]);
  f.manifest.axes = [{ id: 'D', name: 'D', role: 'main', edgeIds: ['e0'], sourceIds: ['source'] }];
  f.manifest.flowPolicies = [{ id: 'holding-D', configuration: 'east', movement: 'departure', destinationNodeIds: ['G'], lastMainAxisIds: ['D'], preferredAxisIds: ['D'], sourceIds: ['source'], reason: 'Source-backed D holding approach.' }];
  const policy = f.install();
  assert.deepEqual(policy.compile({ configuration: 'east', movement: 'departure', from: 'S', to: 'G' }).policyIds, ['holding-D']);
  assert.deepEqual(policy.compile({ configuration: 'east', movement: 'departure', from: 'S', to: 'A' }).policyIds, []);
  assert.deepEqual(policy.compile({ configuration: 'west', movement: 'departure', from: 'S', to: 'G' }).policyIds, []);
  f.manifest.flowPolicies[0].destinationNodeIds!.push('missing-node');
  assert.equal(new TaxiFlowPolicy(f.graph, f.manifest).valid, false);
});

test('a source-scoped outbound portal cannot be satisfied by crossing the same edge inward', () => {
  const f = fixture([['S', 0, 0], ['A', 100, 0], ['P', 100, 100], ['G', 200, 100], ['B', 0, 100]],
    [['S', 'A'], ['A', 'P'], ['S', 'B'], ['B', 'P'], ['P', 'G'], ['A', 'G']]);
  f.manifest.flowPolicies = [{ id: 'outbound', configuration: 'both', movement: 'departure', originPortalEdgeIds: ['e1'], preferredAxisIds: [], sourceIds: ['source'], reason: 'Cross the source apron portal outward.' }];
  f.manifest.edgeRules = [{ id: 'outbound-direction', configuration: 'both', movement: 'departure', edgeIds: ['e1'], direction: 'forward', sourceIds: ['source'], reason: 'Portal from apron to main circulation, as physically stored.' }];
  f.install(); const route = f.planner.findRoute('S', 'G', { movement: 'departure' })!;
  assert.deepEqual(route.edgeIds, ['e0', 'e1', 'e4']);
  const inward = { ...route, nodeIds: ['S', 'B', 'P', 'A', 'G'], edgeIds: ['e2', 'e3', 'e1', 'e5'], cumulativeDistances: [0, 100, 200, 300, 300 + Math.hypot(100, 100)], distance: 300 + Math.hypot(100, 100) };
  assert.equal(f.planner.isRouteAllowed(inward, 'departure'), false, 'An ordered portal stage alone must not override its source direction.');
  assert.equal(f.planner.allowArc(f.graph.edges.get('e1')!, 'P', 'A', 'arrival'), true, 'The departure-specific nominal gate does not invent an arrival closure.');
});

test('special-area exclusions apply to search, effective permissions, replay and aircraft injection', () => {
  const f = fixture([['S', 0, 0], ['P', 100, 0], ['G', 200, 0], ['U', 0, 100], ['V', 200, 100]], [['S', 'P'], ['P', 'G'], ['S', 'U'], ['U', 'V'], ['V', 'G']]);
  const padShortcut = f.planner.findRoute('S', 'G')!;
  f.manifest.specialAreas = [{ id: 'pad-13', name: 'DE-ICE 13', kind: 'deicing-pad', restriction: 'no-normal-transit', edgeIds: ['e1'], sourceIds: ['source'], reason: 'Treatment lane, outside routine taxi.' }];
  const policy = f.install(), route = f.planner.findRoute('S', 'G')!;
  assert.deepEqual(route.edgeIds, ['e2', 'e3', 'e4']); assert.equal(policy.specialAreaEdgeIds.has('e1'), true);
  assert.equal(policy.edgeDirection('e1', 'west', 'transfer'), 'closed');
  assert.equal(f.planner.allowArc(f.graph.edges.get('e1')!, 'P', 'G'), false); assert.equal(f.planner.allowArc(f.graph.edges.get('e1')!, 'G', 'P'), false);
  const forged = { ...padShortcut, flow: route.flow };
  assert.equal(f.planner.isRouteAllowed(forged), false);
  assert.throws(() => new AircraftSimulator(f.graph, f.planner).addRoute(forged), /nominal flow/);
  f.manifest.specialAreas[0].edgeIds.push('unknown'); assert.equal(new TaxiFlowPolicy(f.graph, f.manifest).valid, false);
});

test('flow-only replay preserves movement-specific restrictions and rejects forged physical distances', () => {
  const f = fixture([['S', 0, 0], ['G', 100, 0]], [['S', 'G']]);
  f.rules.manifest.edgeRules.push({ id: 'departure-direction', edgeIds: ['e0'], direction: 'forward', configuration: 'both', movement: 'departure', sourceIds: ['source'], reason: 'Synthetic movement-specific direction.' });
  f.planner.setOperatingRules(new OperatingRules(f.graph, f.rules.manifest), 'west'); f.install();
  const route = f.planner.findRoute('S', 'G', { movement: 'departure' })!;
  const flowOnly = { ...route, operational: undefined };
  assert.ok(f.planner.isRouteAllowed(flowOnly)); assert.equal(f.planner.describeRoute(flowOnly).operational?.movement, 'departure');
  assert.equal(f.planner.isRouteAllowed({ ...flowOnly, nodeIds: ['G', 'S'] }), false);
  assert.equal(f.planner.isRouteAllowed({ ...flowOnly, distance: 90 }), false);
  assert.equal(f.planner.isRouteAllowed({ ...flowOnly, cumulativeDistances: [0, 90] }), false);
});

test('no-U-turn geometry rejects near-reversals across distinct taxi edges and matches directed Dijkstra', () => {
  const f = fixture([['S', 0, 0], ['J', 100, 0], ['A', 10, 20], ['U', 100, 100]], [['S', 'J'], ['J', 'A'], ['J', 'U'], ['U', 'A']]);
  assert.equal(f.rules.allowTurn('e0', 'J', 'e1', 'west'), false, 'The 167-degree branch reverses heading despite using a different edge ID.');
  assert.equal(f.rules.allowTurn('e0', 'J', 'e2', 'west'), true, 'An ordinary right-angle source junction stays available.');
  const route = f.planner.findRoute('S', 'A')!;
  assert.deepEqual(route.nodeIds, ['S', 'J', 'U', 'A']);
  assert.equal(f.planner.isRouteAllowed({ ...route, nodeIds: ['S', 'J', 'A'], edgeIds: ['e0', 'e1'], distance: 100 + Math.hypot(90, 20), cumulativeDistances: [0, 100, 100 + Math.hypot(90, 20)] }), false);
  function dijkstra(from: string, to: string) {
    const queue = [{ node: from, previous: null as string | null, cost: 0 }], visited = new Set<string>();
    while (queue.length) {
      queue.sort((a, b) => a.cost - b.cost); const current = queue.shift()!, key = `${current.node}|${current.previous}`;
      if (visited.has(key)) continue; visited.add(key); if (current.node === to) return current.cost;
      for (const arc of f.graph.adjacency.get(current.node)!) {
        if (arc.edgeId === current.previous) continue;
        if (current.previous) {
          const edge = f.graph.edges.get(current.previous)!, a = f.graph.nodes.get(edge.from === current.node ? edge.to : edge.from)!, b = f.graph.nodes.get(current.node)!, c = f.graph.nodes.get(arc.to)!;
          const ux = b.x - a.x, uy = b.y - a.y, vx = c.x - b.x, vy = c.y - b.y;
          const angle = Math.abs(Math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)) * 180 / Math.PI;
          if (angle >= 150 - 1e-8) continue;
        }
        queue.push({ node: arc.to, previous: arc.edgeId, cost: current.cost + arc.length });
      }
    }
    return Infinity;
  }
  for (const from of f.graph.nodes.keys()) for (const to of f.graph.nodes.keys()) {
    const expected = dijkstra(from, to), actual = f.planner.findRoute(from, to);
    if (expected === Infinity) assert.equal(actual, null); else { assert.ok(actual); assert.ok(Math.abs(actual.distance - expected) < 1e-8); }
  }
  f.graph.edges.get('e1')!.kind = 'parking_position';
  assert.equal(new OperatingRules(f.graph, f.rules.manifest).allowTurn('e0', 'J', 'e1', 'west'), true, 'Do not invent a pushback geometry rule for parking leads.');
});
