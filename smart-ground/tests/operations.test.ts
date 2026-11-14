import { strict as assert } from 'node:assert';
import { test } from 'node:test';
import { AircraftSimulator, OperatingRules, RoutePlanner, TaxiGraph, TrafficPredictor } from '../src/core';
import type { AirportData, OperatingConfiguration, MovementMode, TaxiEdge } from '../src/core/types';
import type { OperatingManifest } from '../src/core/OperatingRules';
import { resolveOperatingManifest } from '../src/core/resolveOperatingManifest';

function graph(points: [string, number, number][], links: [string, string, TaxiEdge['kind']?][]) {
  const nodes = points.map(([id, x, y]) => ({ id, x, y, lat: 49 + y / 111320, lon: 2 + x / 73000 }));
  const map = new Map(nodes.map(n => [n.id, n]));
  const data: AirportData = { nodes, edges: links.map(([from, to, kind], i) => ({ id: `e${i}`, from, to, length: Math.hypot(map.get(from)!.x - map.get(to)!.x, map.get(from)!.y - map.get(to)!.y), name: `T${i}`, kind: kind ?? 'taxiway' })), locations: nodes.map(n => ({ ...n, name: n.id, nodeId: n.id, kind: 'stand' })), surfaces: [], metadata: {} };
  return new TaxiGraph(data);
}
function manifest(overrides: Partial<OperatingManifest> = {}): OperatingManifest {
  return { version: 1, airport: 'LFPG', effectiveDate: '2026-10-29', sources: [{ id: 'test', title: 'Synthetic regulatory fixture', url: 'https://example.test/fixture' }], edgeRules: [], forbidUTurns: true, ...overrides };
}
const edgeRule = (id: string, edgeIds: string[], direction: 'forward' | 'reverse' | 'closed', configuration: 'east' | 'west' | 'both' = 'both', movement?: 'departure' | 'arrival' | 'both') => ({ id, edgeIds, direction, configuration, movement, sourceIds: ['test'], reason: 'Synthetic source-backed restriction.' });

// Independent linear-priority Dijkstra over (node,incoming) state for comparison.
function directedDijkstra(g: TaxiGraph, rules: OperatingRules, from: string, to: string, configuration: OperatingConfiguration, movement?: MovementMode): number {
  const queue = [{ node: from, incoming: null as string | null, cost: 0 }], best = new Map<string, number>();
  while (queue.length) {
    queue.sort((a, b) => a.cost - b.cost); const current = queue.shift()!;
    const key = JSON.stringify([current.node, current.incoming]);
    if (current.cost >= (best.get(key) ?? Infinity)) continue; best.set(key, current.cost);
    if (current.node === to) return current.cost;
    for (const arc of g.adjacency.get(current.node) ?? []) {
      const edge = g.edges.get(arc.edgeId)!;
      if (edge.kind !== 'runway' && rules.allowArc(edge, current.node, arc.to, configuration, movement) && rules.allowTurn(current.incoming, current.node, edge.id, configuration, movement)) queue.push({ node: arc.to, incoming: edge.id, cost: current.cost + arc.length });
    }
  }
  return Infinity;
}

test('operating directions depend on east/west and departure/arrival without treating opposite flows as contradictions', () => {
  const g = graph([['A', 0, 0], ['B', 100, 0]], [['A', 'B']]);
  const rules = new OperatingRules(g, manifest({ edgeRules: [edgeRule('west-dep', ['e0'], 'reverse', 'west', 'departure'), edgeRule('west-arr', ['e0'], 'forward', 'west', 'arrival'), edgeRule('east-dep', ['e0'], 'forward', 'east', 'departure')] }));
  assert.equal(rules.valid, true); assert.equal(rules.diagnostics.length, 0);
  const p = new RoutePlanner(g, rules, 'west');
  assert.equal(p.findRoute('A', 'B', { movement: 'departure' }), null);
  assert.ok(p.findRoute('A', 'B', { movement: 'arrival' }));
  assert.ok(p.findRoute('B', 'A', { movement: 'departure' }));
  p.setOperatingRules(rules, 'east'); assert.ok(p.findRoute('A', 'B', { movement: 'departure' })); assert.equal(p.findRoute('B', 'A', { movement: 'departure' }), null);
  assert.ok(p.findRoute('B', 'A', { movement: 'transfer' }), 'Transfers only inherit restrictions declared for both movements.');
});

test('forbidden turns require incoming-edge A* state and agree with independent directed Dijkstra', () => {
  const g = graph([['A', 0, 0], ['B', 100, 0], ['C', 200, 0], ['D', 100, 100], ['E', 0, 100]], [['A', 'B'], ['B', 'C'], ['A', 'E'], ['E', 'D'], ['D', 'B']]);
  const rules = new OperatingRules(g, manifest({ turnRules: [{ id: 'no-through-B', viaNodeId: 'B', fromEdgeId: 'e0', toEdgeId: 'e1', configuration: 'both', sourceIds: ['test'], reason: 'No marked turn.' }] }));
  const p = new RoutePlanner(g, rules, 'west');
  assert.deepEqual(p.findRoute('A', 'C')!.nodeIds, ['A', 'E', 'D', 'B', 'C']);
  for (const from of g.nodes.keys()) for (const to of g.nodes.keys()) {
    const expected = directedDijkstra(g, rules, from, to, 'west'), actual = p.findRoute(from, to);
    if (expected === Infinity) assert.equal(actual, null); else { assert.ok(actual); assert.ok(Math.abs(actual.distance - expected) < 1e-8); }
  }
});

test('conflicting rules fail closed per edge, malformed references invalidate routing, and provenance exposes unknown coverage', () => {
  const g = graph([['A', 0, 0], ['B', 100, 0], ['C', 200, 0]], [['A', 'B'], ['B', 'C']]);
  const conflict = new OperatingRules(g, manifest({ edgeRules: [edgeRule('one', ['e0'], 'forward'), edgeRule('two', ['e0'], 'reverse')] }));
  assert.equal(new RoutePlanner(g, conflict).findRoute('A', 'C'), null); assert.ok(conflict.diagnostics.some(d => d.code === 'conflicting-directions'));
  const missing = new OperatingRules(g, manifest({ edgeRules: [edgeRule('bad', ['missing'], 'closed')] }));
  assert.equal(missing.valid, false); assert.equal(new RoutePlanner(g, missing).findRoute('B', 'C'), null); assert.ok(missing.diagnostics.some(d => d.code === 'unknown-edge'));
  const rules = new OperatingRules(g, manifest({ edgeRules: [edgeRule('one', ['e0'], 'forward')] }));
  const route = new RoutePlanner(g, rules, 'east').findRoute('A', 'C')!;
  assert.deepEqual(route.operational?.ruleIds, ['one']); assert.deepEqual(route.operational?.sourceIds, ['test']); assert.deepEqual(route.operational?.unverifiedEdgeIds, ['e1']);
});

test('synthetic geometric crossings preserve source-segment continuity and reject unmarked turns unless explicitly allowed', () => {
  const g = graph([['W', -100, 0], ['intersection:0', 0, 0], ['E', 100, 0], ['S', 0, -100], ['N', 0, 100]], [['W', 'intersection:0'], ['intersection:0', 'E'], ['S', 'intersection:0'], ['intersection:0', 'N']]);
  for (const id of ['e0', 'e1']) Object.assign(g.edges.get(id)!, { sourceSegmentId: 'horizontal' });
  for (const id of ['e2', 'e3']) Object.assign(g.edges.get(id)!, { sourceSegmentId: 'vertical' });
  const restrictions = manifest({ forbidUnmarkedSyntheticTurns: true });
  const p = new RoutePlanner(g, new OperatingRules(g, restrictions));
  assert.ok(p.findRoute('W', 'E')); assert.equal(p.findRoute('W', 'N'), null);
  restrictions.allowedTurnRules = [{ id: 'marked', viaNodeId: 'intersection:0', fromEdgeId: 'e0', toEdgeId: 'e3', configuration: 'both', sourceIds: ['test'], reason: 'Explicit marked movement.' }];
  p.setOperatingRules(new OperatingRules(g, restrictions), 'west'); assert.ok(p.findRoute('W', 'N')); assert.equal(p.findRoute('N', 'W'), null);
});

test('parking lines serve origin/destination stands but cannot become intermediate shortcut', () => {
  const g = graph([['A', 0, 0], ['B', 100, 0], ['C', 200, 0], ['D', 100, 100], ['S', 100, -20]], [['A', 'B', 'parking_position'], ['B', 'C', 'parking_position'], ['A', 'D'], ['D', 'C'], ['S', 'B', 'parking_position']]);
  const p = new RoutePlanner(g, new OperatingRules(g, manifest({ restrictParkingToEndpoints: true })));
  assert.deepEqual(p.findRoute('A', 'C')!.nodeIds, ['A', 'B', 'C'], 'Origin and destination genuinely share a parking chain.');
  // Add through route with endpoints outside the parking component.
  const h = graph([['X', -100, 0], ['A', 0, 0], ['B', 100, 0], ['C', 200, 0], ['Y', 300, 0], ['D', 100, 150]], [['X', 'A'], ['A', 'B', 'parking_position'], ['B', 'C', 'parking_position'], ['C', 'Y'], ['A', 'D'], ['D', 'C']]);
  const q = new RoutePlanner(h, new OperatingRules(h, manifest({ restrictParkingToEndpoints: true })));
  assert.deepEqual(q.findRoute('X', 'Y')!.nodeIds, ['X', 'A', 'D', 'C', 'Y']); assert.ok(q.findRoute('B', 'Y'));
});

test('runway crossings stop at sourced checkpoints, cannot be resumed without manual release, and predictions do not cross pending clearance', () => {
  const g = graph([['A', 0, 0], ['B', 100, 0], ['C', 200, 0]], [['A', 'B'], ['B', 'C']]);
  const rules = new OperatingRules(g, manifest({ clearanceRules: [{ id: 'cross', nodeId: 'B', configuration: 'both', kind: 'runway-crossing', sourceIds: ['test'], reason: 'Hold before runway.' }] }));
  const p = new RoutePlanner(g, rules), sim = new AircraftSimulator(g, p), a = sim.addRoute(p.findRoute('A', 'C')!, { speed: 10 });
  const prediction = new TrafficPredictor(g).predict([a], 60)[0];
  assert.ok(prediction.edges.every(e => e.edgeId === 'e0')); assert.ok(prediction.nodes.some(n => n.nodeId === 'B' && n.stationary));
  sim.update(100); assert.equal(a.distance, 100); assert.equal(a.state, 'holding'); assert.equal(a.waitingClearanceId, 'cross@1');
  sim.setHolding(a.id, false); sim.update(100); assert.equal(a.distance, 100);
  assert.equal(sim.clearRunwayCrossing(a.id), true); sim.update(100); assert.equal(a.distance, 200); assert.equal(a.state, 'arrived');
});

test('operational generator uses active runway accesses and excludes closed stands and PPR-only accesses', () => {
  const g = graph([['A', 0, 0], ['B', 100, 0], ['C', 200, 0], ['D', 300, 0]], [['A', 'B'], ['B', 'C'], ['C', 'D']]);
  const rules = new OperatingRules(g, manifest({
    operationalLocations: [{ ...g.nodes.get('D')!, id: 'hold27', name: '27L holding', nodeId: 'D', kind: 'holding_point' }, { ...g.nodes.get('C')!, id: 'exit27', name: '27R exit', nodeId: 'C', kind: 'holding_point' }, { ...g.nodes.get('B')!, id: 'ppr', name: 'PPR', nodeId: 'B', kind: 'holding_point' }],
    closedLocationIds: ['A'], runwayUse: { west: { departure: ['27L'], arrival: ['27R'] }, east: { departure: ['09R'], arrival: ['09L'] } },
    runwayAccess: [{ locationId: 'hold27', runway: '27L', role: 'departure', sourceIds: ['test'] }, { locationId: 'exit27', runway: '27R', role: 'arrival', sourceIds: ['test'] }, { locationId: 'ppr', runway: '27L', role: 'departure', sourceIds: ['test'], pprRequired: true }],
  }));
  const p = new RoutePlanner(g, rules), sim = new AircraftSimulator(g, p);
  assert.deepEqual(p.availableLocations('departure').map(l => l.id), ['hold27']);
  const approved = new OperatingRules(g, { ...rules.manifest, approvedPprLocationIds: ['ppr'] });
  assert.deepEqual(approved.runwayAccess('west', 'departure').map(a => a.locationId), ['hold27', 'ppr']);
  assert.equal(sim.addAircraft('A', 'hold27', { movement: 'departure' }), null);
  assert.equal(sim.addAircraft('B', 'D', { movement: 'departure' }), null, 'Legacy locations cannot impersonate runway accesses.');
  assert.ok(sim.generate(3, 42, { movement: 'departure' }) > 0); assert.ok(sim.aircraft.every(a => a.destinationId === 'hold27' && a.originId !== 'A'));
  assert.ok(sim.generate(3, 42, { movement: 'arrival' }) > 0); assert.ok(sim.aircraft.every(a => a.originId === 'exit27' && a.destinationId !== 'A'));
  const old = p.findRoute('B', 'D', { movement: 'departure' })!; p.setOperatingRules(rules, 'east');
  assert.throws(() => sim.addRoute(old), /configuration/); assert.equal(sim.generate(3, 42), 0);
});

test('selected works profiles share UI defaults, preserve source references and reject unknown profiles', () => {
  const base = manifest({ temporaryProfiles: [
    { id: 'published', defaultEnabled: true, sourceIds: ['test'], edgeRules: [edgeRule('published-closure', ['e0'], 'closed')], closedLocationIds: ['A'] },
    { id: 'notam-dependent', defaultEnabled: false, sourceIds: ['test'], edgeRules: [edgeRule('notam-closure', ['e1'], 'closed')], closedLocationIds: ['B'] },
  ] });
  const defaults = resolveOperatingManifest(base);
  assert.deepEqual(defaults.activeProfileIds, ['published']); assert.deepEqual(defaults.closedLocationIds, ['A']); assert.equal(defaults.edgeRules.length, 1);
  const explicit = resolveOperatingManifest(base, { enabledProfileIds: ['notam-dependent'], approvedPprLocationIds: ['PPR'] });
  assert.deepEqual(explicit.activeProfileIds, ['notam-dependent']); assert.deepEqual(explicit.closedLocationIds, ['B']); assert.deepEqual(explicit.approvedPprLocationIds, ['PPR']); assert.deepEqual(explicit.edgeRules[0].sourceIds, ['test']);
  assert.throws(() => resolveOperatingManifest(base, { enabledProfileIds: ['unknown'] }), /Unknown operating works profile/);
});

test('automatic right-of-way holds before the junction and resumes after priority traffic while a manual hold persists', () => {
  const g = graph([['W', -200, 0], ['J', 0, 0], ['E', 200, 0], ['N', 0, 200], ['S', 0, -200]], [['W', 'J'], ['J', 'E'], ['N', 'J'], ['J', 'S']]);
  const rules = new OperatingRules(g, manifest({ priorityRules: [{ id: 'yield', nodeId: 'J', fromEdgeId: 'e0', priorityFromEdgeIds: ['e2'], configuration: 'east', sourceIds: ['test'], reason: 'Give way to arrival.' }] }));
  const p = new RoutePlanner(g, rules, 'east'), sim = new AircraftSimulator(g, p);
  const low = sim.addRoute(p.findRoute('W', 'E', { movement: 'departure' })!, { speed: 10 });
  const high = sim.addRoute(p.findRoute('N', 'S', { movement: 'arrival' })!, { speed: 10 });
  sim.update(1);
  const futureYield = new TrafficPredictor(g).predict([low], 120)[0]; assert.ok(futureYield.edges.every(e => e.edgeId === 'e0')); assert.ok(futureYield.edges.some(e => e.stationary && e.offset === 155));
  sim.update(15); assert.equal(low.distance, 155); assert.equal(low.state, 'holding'); assert.equal(low.yielding?.priorityAircraftId, high.id);
  const prediction = new TrafficPredictor(g).predict([low], 120)[0]; assert.ok(prediction.edges.every(e => e.edgeId === 'e0'));
  sim.update(20); assert.ok(low.distance > 155); assert.equal(low.yielding, undefined);
  sim.setHolding(low.id, true); const manualDistance = low.distance; sim.update(60); assert.equal(low.distance, manualDistance); assert.equal(low.state, 'holding');
  sim.clear();
  const held = sim.addRoute(p.findRoute('W', 'E', { movement: 'departure' })!, { speed: 10 });
  sim.addRoute(p.findRoute('N', 'S', { movement: 'arrival' })!, { speed: 10 });
  sim.update(16); assert.ok(held.yielding); sim.setHolding(held.id, true); assert.equal(held.yielding, undefined);
  sim.update(60); assert.equal(held.distance, 155); assert.equal(held.state, 'holding', 'An explicit hold during automatic yielding remains manual after priority traffic clears.');
});
