import { strict as assert } from 'node:assert';
import { test } from 'node:test';
import { AircraftSimulator, OperatingRules, RoutePlanner, TaxiFlowPolicy, TaxiGraph } from '../src/core';
import type { AirportData, TaxiEdge } from '../src/core/types';
import type { OperatingManifest } from '../src/core/OperatingRules';
import type { TaxiFlowManifest } from '../src/core/TaxiFlowPolicy';

function fixture(points: [string, number, number][], pairs: [string, string, TaxiEdge['kind']?][]) {
  const nodes = points.map(([id, x, y]) => ({ id, x, y, lat: 49 + y / 111320, lon: 2 + x / 73000 }));
  const nodeMap = new Map(nodes.map(n => [n.id, n]));
  const data: AirportData = { nodes, edges: pairs.map(([from, to, kind], i) => ({ id: `e${i}`, name: `way-${i}`, from, to, kind: kind || 'taxiway', length: Math.hypot(nodeMap.get(from)!.x - nodeMap.get(to)!.x, nodeMap.get(from)!.y - nodeMap.get(to)!.y) })),
    locations: nodes.map(n => ({ ...n, id: `stand:${n.id}`, nodeId: n.id, kind: 'stand', name: n.id })), surfaces: [], metadata: {} };
  const graph = new TaxiGraph(data);
  const operating: OperatingManifest = { version: 1, airport: 'LFPG', effectiveDate: '2026-10-29', sources: [{ id: 'aip', title: 'Synthetic hard restriction source', url: 'https://example.test/aip' }], edgeRules: [], forbidUTurns: true };
  const planner = new RoutePlanner(graph, new OperatingRules(graph, operating), 'west');
  const manifest: TaxiFlowManifest = { version: 1, airport: 'LFPG', sources: [{ id: 'manex', title: 'Synthetic nominal flow source', url: 'https://example.test/manex' }], axes: [], zones: [], flowPolicies: [] };
  const axis = (id: string, edgeIds: string[], role: 'main' | 'connector' | 'apron' = 'main') => manifest.axes.push({ id, name: id, edgeIds, role, sourceIds: ['manex'] });
  const zone = (id: string, standIds: string[], edgeIds: string[]) => manifest.zones.push({ id, name: id, standIds: standIds.map(s => `stand:${s}`), edgeIds, sourceIds: ['manex'] });
  const install = () => { const policy = new TaxiFlowPolicy(graph, manifest); assert.equal(policy.valid, true, policy.diagnostics.join('; ')); planner.setFlowPolicy(policy); return policy; };
  return { data, graph, operating, planner, manifest, axis, zone, install };
}

test('main-axis convergence prevents a shorter transit through an unrelated apron and preserves physical distance', () => {
  const f = fixture([['S', 0, 0], ['P', 100, 0], ['J', 200, 0], ['G', 300, 0], ['U', 0, 100], ['V', 300, 100]], [['S', 'P'], ['P', 'J'], ['J', 'G'], ['S', 'U'], ['U', 'V'], ['V', 'G']]);
  const baseline = f.planner.findRoute('S', 'G')!; assert.equal(baseline.distance, 300);
  f.axis('R', ['e2', 'e4']); f.zone('origin', ['S'], ['e0', 'e3']); f.zone('destination', ['G'], ['e5']); f.zone('unrelated-apron', ['P'], ['e1']);
  f.manifest.flowPolicies.push({ id: 'converge', configuration: 'both', movement: 'transfer', requireMainAxis: true, differentZonesOnly: true, preferredAxisIds: ['R'], sourceIds: ['manex'], reason: 'Use the principal axis between distinct aprons.' });
  f.install(); const route = f.planner.findRoute('S', 'G', { movement: 'transfer', originId: 'stand:S', destinationId: 'stand:G' })!;
  assert.deepEqual(route.nodeIds, ['S', 'U', 'V', 'G']); assert.equal(route.distance, 500); assert.equal(route.flow?.physicalDistance, 500);
  assert.equal(route.flow?.apronTransitMetres, 0); assert.deepEqual(route.flow?.axes, ['R']); assert.ok(f.planner.isRouteAllowed(route));
  // Independently enumerate the two simple routes: both touch R, only the short
  // one spends 100m on the unrelated apron. No arbitrary numeric multiplier.
  const alternatives = [{ ids: ['e0', 'e1', 'e2'], apron: 100, metres: 300 }, { ids: ['e3', 'e4', 'e5'], apron: 0, metres: 500 }].sort((a, b) => a.apron - b.apron || a.metres - b.metres);
  assert.deepEqual(route.edgeIds, alternatives[0].ids);
});

test('published egress and ordered corridor stages are enforced on actual edges, not trusted metadata', () => {
  const f = fixture([['S', 0, 0], ['A', 100, 0], ['G', 200, 0], ['P', 0, 100], ['B', 200, 100]], [['S', 'A'], ['A', 'G'], ['S', 'P'], ['P', 'B'], ['B', 'G']]);
  const forbiddenShortcut = f.planner.findRoute('S', 'G')!;
  f.axis('F', ['e3']); f.zone('origin', ['S'], ['e0', 'e2']);
  f.manifest.flowPolicies.push({ id: 'published-egress', configuration: 'both', movement: 'departure', originZoneIds: ['origin'], originPortalEdgeIds: ['e2'], requiredAxisGroups: [['F']], preferredAxisIds: ['F'], sourceIds: ['manex'], reason: 'Apron egress then F.' });
  f.install(); const route = f.planner.findRoute('S', 'G', { movement: 'departure', originId: 'stand:S', destinationId: 'stand:G' })!;
  assert.deepEqual(route.edgeIds, ['e2', 'e3', 'e4']); assert.equal(route.flow?.requiredStages.length, 2);
  assert.equal(f.planner.isRouteAllowed({ ...forbiddenShortcut, flow: route.flow }, 'departure'), false);
  const sim = new AircraftSimulator(f.graph, f.planner);
  assert.throws(() => sim.addRoute({ ...forbiddenShortcut, flow: route.flow }, { originId: 'stand:S', destinationId: 'stand:G', movement: 'departure' }), /nominal flow/);
  assert.equal(f.planner.isRouteAllowed({ ...route, flow: { ...route.flow!, originId: 'unknown' } }, 'departure'), false);
  assert.equal(f.planner.isRouteAllowed(route, 'departure', { originId: 'stand:P', destinationId: 'stand:G' }), false);
});

test('an unavailable nominal portal fails explicitly without a free-routing fallback', () => {
  const f = fixture([['S', 0, 0], ['A', 100, 0], ['G', 200, 0], ['P', 0, 100]], [['S', 'A'], ['A', 'G'], ['S', 'P']]);
  assert.ok(f.planner.findRoute('S', 'G'));
  f.axis('F', ['e0']); f.zone('origin', ['S'], []);
  f.manifest.flowPolicies.push({ id: 'closed-egress', configuration: 'both', movement: 'departure', originZoneIds: ['origin'], originPortalEdgeIds: ['e2'], preferredAxisIds: ['F'], sourceIds: ['manex'], reason: 'Published portal P.' });
  f.install(); assert.equal(f.planner.findRoute('S', 'G', { movement: 'departure', originId: 'stand:S' }), null); assert.match(f.planner.lastFlowFailure || '', /nominal/);
});

test('the wrong first apron portal cannot be laundered by looping through an allowed later portal', () => {
  const f = fixture([['S', 0, 0], ['W', 100, 0], ['P', 100, 100], ['U', 200, 100], ['G', 300, 100]], [['S', 'W'], ['W', 'P'], ['P', 'U'], ['U', 'G'], ['S', 'P']]);
  f.axis('F', ['e3']); f.zone('origin', ['S'], ['e0', 'e1', 'e4']); f.manifest.zones[0].portalEdgeIds = ['e0', 'e2'];
  f.manifest.flowPolicies.push({ id: 'nominal-egress', configuration: 'both', movement: 'departure', originZoneIds: ['origin'], originPortalEdgeIds: ['e2'], requireMainAxis: true, preferredAxisIds: ['F'], sourceIds: ['manex'], reason: 'Use the designated departure portal first.' });
  f.install(); const route = f.planner.findRoute('S', 'G', { movement: 'departure', originId: 'stand:S', destinationId: 'stand:G' })!;
  assert.deepEqual(route.edgeIds, ['e4', 'e2', 'e3']);
  const looped = { ...route, nodeIds: ['S', 'W', 'P', 'U', 'G'], edgeIds: ['e0', 'e1', 'e2', 'e3'], cumulativeDistances: [0, 100, 200, 300, 400], distance: 400 };
  assert.equal(f.planner.isRouteAllowed(looped, 'departure'), false, 'Touching the correct portal later must not undo a wrong apron exit.');
  assert.throws(() => new AircraftSimulator(f.graph, f.planner).addRoute(looped, { movement: 'departure', originId: 'stand:S', destinationId: 'stand:G' }), /nominal flow/);
});

test('new sourced circulation overrides inferred AVISO scope while permanent AIP bans remain hard', () => {
  const f = fixture([['A', 0, 0], ['B', 100, 0]], [['A', 'B']]);
  f.operating.edgeRules.push({ id: 'old-inferred', edgeIds: ['e0'], direction: 'reverse', configuration: 'west', sourceIds: ['aip'], reason: 'Undated inferred scope.', authoritative: false });
  f.planner.setOperatingRules(new OperatingRules(f.graph, f.operating), 'west'); assert.equal(f.planner.findRoute('A', 'B'), null);
  f.axis('N', ['e0']); f.manifest.edgeRules = [{ id: 'source-direction', edgeIds: ['e0'], direction: 'forward', configuration: 'west', sourceIds: ['manex'], reason: 'Published N flow.' }];
  f.install(); const route = f.planner.findRoute('A', 'B', { movement: 'departure', originId: 'stand:A', destinationId: 'stand:B' })!;
  assert.ok(route); assert.deepEqual(route.operational?.ruleIds, []); assert.deepEqual(route.operational?.unverifiedEdgeIds, []); assert.ok(route.flow?.sourceIds.includes('manex'));
  assert.equal(f.planner.allowArc(f.graph.edges.get('e0')!, 'B', 'A', 'departure'), false);
  f.operating.edgeRules.push({ id: 'closed', edgeIds: ['e0'], direction: 'closed', configuration: 'both', sourceIds: ['aip'], reason: 'Published works closure.' });
  f.planner.setOperatingRules(new OperatingRules(f.graph, f.operating), 'west'); assert.equal(f.planner.findRoute('A', 'B', { movement: 'departure' }), null);
});

test('same-apron local transfer does not require a main-axis detour and declared shared connectors stay neutral', () => {
  const f = fixture([['S', 0, 0], ['A', 100, 0], ['G', 200, 0], ['N', 100, 200]], [['S', 'A'], ['A', 'G'], ['S', 'N'], ['N', 'G']]);
  f.axis('N', ['e2', 'e3']); f.axis('G3', ['e0', 'e1'], 'connector');
  f.zone('T2-north', ['S', 'G'], ['e0', 'e1']); f.zone('unrelated-overlap', ['A'], ['e0']);
  f.manifest.flowPolicies.push({ id: 'cross-apron', configuration: 'both', movement: 'transfer', differentZonesOnly: true, requireMainAxis: true, preferredAxisIds: ['N'], sourceIds: ['manex'], reason: 'Use main axes between distinct areas.' });
  f.manifest.flowPolicies.push({ id: 'local-T2', configuration: 'both', movement: 'transfer', originZoneIds: ['T2-north'], destinationZoneIds: ['T2-north'], preferredAxisIds: ['G3'], sourceIds: ['manex'], reason: 'Local G3/G4 connection permitted.' });
  f.install(); const route = f.planner.findRoute('S', 'G', { movement: 'transfer', originId: 'stand:S', destinationId: 'stand:G' })!;
  assert.deepEqual(route.edgeIds, ['e0', 'e1']); assert.equal(route.distance, 200); assert.equal(route.flow?.apronTransitMetres, 0); assert.equal(route.flow?.mainAxisReentries, 0); assert.deepEqual(route.flow?.requiredStages, []);
});

test('unknown policy references invalidate flow routing and stale configuration cannot be simulated', () => {
  const f = fixture([['A', 0, 0], ['B', 100, 0]], [['A', 'B']]); f.axis('N', ['e0']); f.install();
  const route = f.planner.findRoute('A', 'B', { movement: 'transfer', originId: 'stand:A', destinationId: 'stand:B' })!;
  f.planner.setOperatingRules(new OperatingRules(f.graph, f.operating), 'east');
  assert.equal(f.planner.isRouteAllowed(route), false); assert.throws(() => new AircraftSimulator(f.graph, f.planner).addRoute(route), /configuration/);
  f.manifest.axes[0].edgeIds.push('missing'); const broken = new TaxiFlowPolicy(f.graph, f.manifest); assert.equal(broken.valid, false);
  f.planner.setFlowPolicy(broken); assert.equal(f.planner.findRoute('A', 'B'), null); assert.ok(f.planner.lastFlowFailure);
});

test('a generic convergence policy cannot silently label an unmapped stand as a fully nominal apron route', () => {
  const f = fixture([['S', 0, 0], ['G', 100, 0]], [['S', 'G']]); f.axis('N', ['e0']);
  f.manifest.flowPolicies.push({ id: 'generic', configuration: 'both', movement: 'departure', requireMainAxis: true, preferredAxisIds: ['N'], sourceIds: ['manex'], reason: 'Generic principal-axis convergence.' });
  f.install(); const route = f.planner.findRoute('S', 'G', { movement: 'departure', originId: 'stand:S', destinationId: 'stand:G' })!;
  assert.ok(route); assert.equal(route.flow?.status, 'review'); assert.match(route.flow!.warnings.join(' '), /aire du stand de départ.*pas appariée/);
});

test('automatic traffic uses mapped nominal routes while a manual unmapped request remains explicitly reviewable', () => {
  const f = fixture([['S', 0, 0], ['G', 100, 0], ['H', 200, 0]], [['S', 'G'], ['G', 'H']]);
  f.axis('R', ['e0', 'e1']); f.zone('source', ['S'], []); f.zone('destination', ['G'], []);
  f.manifest.flowPolicies.push({ id: 'generic', configuration: 'both', movement: 'transfer', differentZonesOnly: true, requireMainAxis: true, preferredAxisIds: ['R'], sourceIds: ['manex'], reason: 'Join a principal axis between mapped aprons.' });
  f.install(); const manual = f.planner.findRoute('S', 'H', { movement: 'transfer', originId: 'stand:S', destinationId: 'stand:H' })!;
  assert.equal(manual.flow?.status, 'review');
  const simulator = new AircraftSimulator(f.graph, f.planner); assert.equal(simulator.generate(10, 42, { movement: 'transfer' }), 10);
  assert.ok(simulator.aircraft.every(a => a.route.flow?.status === 'nominal' && a.originId !== 'stand:H' && a.destinationId !== 'stand:H'));
});
