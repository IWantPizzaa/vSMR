import { strict as assert } from 'node:assert';
import { test } from 'node:test';
import { AircraftRestrictions, AircraftSimulator, DEFAULT_AIRCRAFT_PROFILE, normalizeAircraftProfile, OperatingRules, RoutePlanner, TaxiFlowPolicy, TaxiGraph } from '../src/core';
import type { AircraftProfile, AirportData } from '../src/core';
import type { AircraftRestrictionManifest } from '../src/core/AircraftRestrictions';

const profiles = { small: DEFAULT_AIRCRAFT_PROFILE, a380: { typeCode: 'A388', wingspanMetres: 79.75 }, b744: { typeCode: 'B744', wingspanMetres: 64.92 }, b748: { typeCode: 'B748', wingspanMetres: 68.4 } };
function fixture() {
  const points: [string, number, number][] = [['S', 0, 0], ['A', 100, 0], ['B', 200, 0], ['G', 300, 0], ['U', 100, 100], ['V', 200, 100]];
  const nodes = points.map(([id, x, y]) => ({ id, x, y, lat: 49, lon: 2 })), byId = new Map(nodes.map(node => [node.id, node]));
  const pairs = [['S', 'A'], ['A', 'B'], ['B', 'G'], ['A', 'U'], ['U', 'V'], ['V', 'G']];
  const data: AirportData = { nodes, edges: pairs.map(([from, to], index) => ({ id: `e${index}`, from, to, name: `T${index}`, kind: 'taxiway', length: Math.hypot(byId.get(to)!.x - byId.get(from)!.x, byId.get(to)!.y - byId.get(from)!.y) })), locations: nodes.filter(n => ['S', 'G'].includes(n.id)).map(node => ({ ...node, kind: 'stand', nodeId: node.id, id: `stand:${node.id}`, name: node.id })), surfaces: [], metadata: {} };
  const graph = new TaxiGraph(data);
  const manifest: AircraftRestrictionManifest = { version: 1, airport: 'LFPG', sources: [{ id: 'source', title: 'Synthetic aircraft restriction', url: 'https://example.test/source' }], edgeRules: [], turnRules: [] };
  const planner = new RoutePlanner(graph);
  const install = () => { const policy = new AircraftRestrictions(graph, manifest); assert.ok(policy.valid, policy.diagnostics.join('; ')); planner.setAircraftRestrictions(policy); return policy; };
  const route = (profile = profiles.small, movement: 'departure' | 'arrival' | 'transfer' = 'transfer') => planner.findRoute('S', 'G', { movement, aircraftProfile: profile, originId: 'stand:S', destinationId: 'stand:G' });
  return { data, graph, manifest, planner, install, route };
}

/** Deliberately independent state-space Dijkstra for a >36m forbidden A→B turn. */
function independentTurnDistance(graph: TaxiGraph, span: number) {
  const queue = [{ node: 'S', incoming: '', distance: 0 }], costs = new Map<string, number>();
  while (queue.length) {
    queue.sort((a, b) => a.distance - b.distance); const current = queue.shift()!;
    const key = `${current.node}/${current.incoming}`;
    if ((costs.get(key) ?? Infinity) < current.distance) continue;
    if (current.node === 'G') return current.distance;
    for (const arc of graph.adjacency.get(current.node)!) {
      if (span > 36 && current.node === 'A' && current.incoming === 'e0' && arc.edgeId === 'e1') continue;
      const distance = current.distance + arc.length, nextKey = `${arc.to}/${arc.edgeId}`;
      if (distance >= (costs.get(nextKey) ?? Infinity)) continue;
      costs.set(nextKey, distance); queue.push({ node: arc.to, incoming: arc.edgeId, distance });
    }
  }
  return Infinity;
}

test('wingspan edge limits preserve exact strict and inclusive bounds', () => {
  const f = fixture();
  f.manifest.edgeRules.push({ id: 'max65', edgeIds: ['e1'], direction: 'closed', configuration: 'both', condition: { wingspanMetres: { gt: 65 } }, sourceIds: ['source'], reason: 'Maximum span 65 m.' });
  const policy = f.install();
  for (const span of [36, 64.92, 65]) assert.deepEqual(f.route({ typeCode: 'TEST', wingspanMetres: span })!.edgeIds, ['e0', 'e1', 'e2']);
  for (const span of [65.0001, 68.4, 79.75]) {
    const route = f.route({ typeCode: 'TEST', wingspanMetres: span })!;
    assert.ok(!route.edgeIds.includes('e1')); assert.ok(f.planner.isRouteAllowed(route)); assert.ok(route.distance > 300);
  }
  const edge = f.graph.edges.get('e1')!;
  assert.equal(policy.allowArc(edge, 'A', 'B', 'west', 'transfer', { typeCode: 'TEST', wingspanMetres: NaN }), false);
  f.manifest.edgeRules[0].condition = { wingspanMetres: { gte: 65 } }; f.install();
  assert.ok(!f.route({ typeCode: 'TEST', wingspanMetres: 65 })!.edgeIds.includes('e1'));
});

test('specific B748 and A388 rules are distinct from B744 and generic span limits', () => {
  const f = fixture();
  f.manifest.edgeRules.push({ id: 'specific', edgeIds: ['e1'], direction: 'closed', configuration: 'both', condition: { typeCodes: ['B748', 'A388'] }, sourceIds: ['source'], reason: 'Type-specific restriction.' });
  f.install();
  assert.ok(f.route(profiles.b744)!.edgeIds.includes('e1'));
  assert.ok(f.route({ typeCode: 'B77W', wingspanMetres: 68.4 })!.edgeIds.includes('e1'));
  for (const profile of [profiles.b748, profiles.a380]) assert.ok(!f.route(profile)!.edgeIds.includes('e1'));
  const unknown = f.route({ typeCode: 'UNKNOWN', wingspanMetres: 35 })!;
  assert.ok(!unknown.edgeIds.includes('e1')); assert.equal(unknown.aircraft?.status, 'review');
});

test('conditional incoming-edge turns agree with independent directed Dijkstra in raw and flow modes', () => {
  const f = fixture();
  f.manifest.turnRules.push({ id: 'wide-turn', fromEdgeId: 'e0', viaNodeId: 'A', toEdgeId: 'e1', configuration: 'both', condition: { wingspanMetres: { gt: 36 } }, sourceIds: ['source'], reason: 'Turn forbidden above 36 m.' });
  f.install();
  for (const flow of [false, true]) {
    f.planner.setFlowPolicy(flow ? new TaxiFlowPolicy(f.graph, { version: 1, airport: 'LFPG', sources: f.manifest.sources, axes: [], zones: [], flowPolicies: [] }) : null);
    for (const wingspanMetres of [35.8, 36, 36.01, 79.75]) {
      const route = f.route({ typeCode: 'TEST', wingspanMetres })!;
      assert.ok(Math.abs(route.distance - independentTurnDistance(f.graph, wingspanMetres)) < 1e-7);
      assert.ok(f.planner.isRouteAllowed(route));
      assert.equal(f.planner.allowTurn('e0', 'A', 'e1', 'transfer', { typeCode: 'TEST', wingspanMetres }), wingspanMetres <= 36);
    }
  }
});

test('scenario flags activate only their own conditions and preserve existing aircraft profiles', () => {
  const f = fixture();
  f.manifest.edgeRules.push({ id: 'occupied-pad', edgeIds: ['e1'], direction: 'closed', configuration: 'both', condition: { wingspanMetres: { gt: 36 }, airportFlags: ['southwest-deicing-occupied'] }, sourceIds: ['source'], reason: 'Conditional SW pad clearance.' });
  f.install(); const sim = new AircraftSimulator(f.graph, f.planner);
  f.planner.setAircraftProfile(profiles.b744);
  const old = sim.addAircraft('stand:S', 'stand:G', { movement: 'transfer' })!;
  f.planner.setAircraftProfile(profiles.a380);
  assert.equal(old.aircraftProfile?.typeCode, 'B744'); assert.ok(f.planner.isRouteAllowed(old.route));
  f.planner.setAircraftRestrictionContext({ airportFlags: ['southwest-deicing-occupied'] });
  assert.equal(f.planner.isRouteAllowed(old.route), false);
  sim.update(1); assert.equal(old.state, 'holding'); sim.setHolding(old.id, false); assert.equal(old.state, 'holding');
  const fresh = sim.addAircraft('stand:S', 'stand:G', { movement: 'transfer', aircraftProfile: profiles.a380 })!;
  assert.equal(fresh.aircraftProfile?.typeCode, 'A388'); assert.ok(!fresh.route.edgeIds.includes('e1'));
});

test('profile and scenario metadata cannot launder a forbidden route during replay or injection', () => {
  const f = fixture();
  f.manifest.edgeRules.push({ id: 'max65', edgeIds: ['e1'], direction: 'closed', configuration: 'both', condition: { wingspanMetres: { gt: 65 } }, sourceIds: ['source'], reason: 'Span limit.' });
  f.install(); const small = f.route(profiles.small)!, large = f.route(profiles.a380)!;
  assert.equal(f.planner.isRouteAllowed(small, 'transfer', { aircraftProfile: profiles.a380 }), false);
  const forged = { ...small, aircraft: large.aircraft };
  assert.equal(f.planner.isRouteAllowed(forged), false);
  assert.throws(() => new AircraftSimulator(f.graph, f.planner).addRoute(forged, { aircraftProfile: profiles.a380 }), /aircraft profile/);
  const missing = { ...small, aircraft: undefined }; assert.equal(f.planner.isRouteAllowed(missing), false);
  assert.equal(f.planner.isRouteAllowed({ ...small, distance: small.distance + 1 }), false);
  assert.equal(f.planner.isRouteAllowed({ ...small, aircraft: { ...small.aircraft!, airportFlags: ['colored-dual-axis'] } }), false);
});

test('movement, configuration and permanently closed operating edges remain hard alongside aircraft limits', () => {
  const f = fixture();
  const rules = new OperatingRules(f.graph, { version: 1, airport: 'LFPG', effectiveDate: '2026-10-29', sources: f.manifest.sources, edgeRules: [{ id: 'permanent', edgeIds: ['e3'], direction: 'closed', configuration: 'both', sourceIds: ['source'], reason: 'Closed.' }] });
  f.planner.setOperatingRules(rules, 'west');
  f.manifest.edgeRules.push({ id: 'arrival', edgeIds: ['e1'], direction: 'closed', configuration: 'east', movement: 'arrival', sourceIds: ['source'], reason: 'Arrival-only restriction.' });
  f.install(); assert.ok(f.route(profiles.small, 'arrival'));
  f.planner.setOperatingRules(rules, 'east'); assert.equal(f.route(profiles.small, 'arrival'), null);
  const departure = f.route(profiles.small, 'departure')!; assert.ok(departure);
  assert.equal(f.planner.isRouteAllowed(departure, 'arrival'), false);
  f.planner.setOperatingRules(rules, 'west'); assert.equal(f.planner.isRouteAllowed(departure), false);
});

test('oversteering is a nonblocking scoped advisory and unresolved coverage stays reviewable', () => {
  const f = fixture();
  f.manifest.advisories = [{ id: 'oversteering', viaNodeId: 'A', fromEdgeId: 'e0', toEdgeId: 'e1', configuration: 'both', condition: { typeCodes: ['A388'] }, sourceIds: ['source'], reason: 'Oversteering required.' }];
  f.install(); const route = f.route(profiles.a380)!;
  assert.deepEqual(route.edgeIds, ['e0', 'e1', 'e2']); assert.equal(route.aircraft?.advisories[0].id, 'oversteering'); assert.equal(route.aircraft?.status, 'compatible');
  f.manifest.coverageIssues = [{ id: 'unmapped', edgeIds: ['e1'], configuration: 'both', condition: { typeCodes: ['A388'] }, sourceIds: ['source'], reason: 'A source restriction here needs mapping review.' }];
  f.install(); assert.equal(f.route(profiles.a380)!.aircraft?.status, 'review'); assert.equal(f.route(profiles.b744)!.aircraft?.status, 'compatible');
});

test('profiles reject ambiguous, invalid or downsized known types while declared custom ICAO types remain usable', () => {
  assert.throws(() => normalizeAircraftProfile({ typeCode: 'B747', wingspanMetres: 64.92 }), /variant/);
  assert.throws(() => normalizeAircraftProfile({ typeCode: 'A388', wingspanMetres: 35.8 }), /published/);
  assert.throws(() => normalizeAircraftProfile({ typeCode: 'A320', wingspanMetres: Infinity }), /finite/);
  assert.equal(normalizeAircraftProfile({ typeCode: 'a380', wingspanMetres: 79.75 }).typeCode, 'A388');
  assert.equal(normalizeAircraftProfile({ typeCode: 'B744', wingspanMetres: 64.44 }).wingspanMetres, 64.44);
  assert.equal(normalizeAircraftProfile({ typeCode: 'A320', wingspanMetres: 34.1 }).wingspanMetres, 34.1);
  assert.equal(normalizeAircraftProfile({ typeCode: 'B77W', wingspanMetres: 64.8 }).typeCode, 'B77W');
  const f = fixture(); f.manifest.turnRules.push({ id: 'unknown', fromEdgeId: 'e0', viaNodeId: 'missing', toEdgeId: 'e1', configuration: 'both', sourceIds: ['source'], reason: 'Bad mapping.' });
  const invalid = new AircraftRestrictions(f.graph, f.manifest); assert.equal(invalid.valid, false); f.planner.setAircraftRestrictions(invalid); assert.equal(f.route(profiles.small), null);
});

test('100 generated aircraft retain their explicit profile and each replay uses that profile', () => {
  const f = fixture();
  f.manifest.edgeRules.push({ id: 'large', edgeIds: ['e1'], direction: 'closed', configuration: 'both', condition: { typeCodes: ['A388'] }, sourceIds: ['source'], reason: 'Large type.' });
  f.install(); const sim = new AircraftSimulator(f.graph, f.planner);
  assert.equal(sim.generate(100, 17, { aircraftProfile: profiles.a380 }), 100);
  assert.ok(sim.aircraft.every(a => a.aircraftProfile?.typeCode === 'A388' && !a.route.edgeIds.includes('e1') && f.planner.isRouteAllowed(a.route, undefined, { aircraftProfile: a.aircraftProfile })));
  f.planner.setAircraftProfile(profiles.small); sim.update(1);
  assert.ok(sim.aircraft.every(a => a.aircraftProfile?.typeCode === 'A388'));
});

test('an inaccessible raw stand does not prevent reusing the few heavy-compatible origins', () => {
  const f = fixture(), b = f.graph.nodes.get('B')!;
  f.data.locations.push({ ...b, id: 'stand:B', nodeId: 'B', name: 'B', kind: 'stand' });
  f.manifest.edgeRules.push({ id: 'isolate-B', edgeIds: ['e1', 'e2'], direction: 'closed', configuration: 'both', condition: { typeCodes: ['A388'] }, sourceIds: ['source'], reason: 'The middle stand is inaccessible to this type.' });
  f.install(); const sim = new AircraftSimulator(f.graph, f.planner);
  assert.equal(sim.generate(100, 190, { aircraftProfile: profiles.a380 }), 100);
  assert.ok(sim.aircraft.every(a => a.originId !== 'stand:B' && a.destinationId !== 'stand:B'));
  const previous = sim.aircraft.map(a => [a.originId, a.destinationId, a.speed]);
  assert.equal(sim.generate(100, 190, { aircraftProfile: profiles.a380 }), 100);
  assert.deepEqual(sim.aircraft.map(a => [a.originId, a.destinationId, a.speed]), previous);
});
