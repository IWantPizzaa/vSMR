import { strict as assert } from 'node:assert';
import { test } from 'node:test';
import { AircraftRestrictions, AircraftSimulator, OperatingRules, RoutePlanner, TaxiFlowPolicy, TaxiGraph } from '../src/core';
import type { AircraftObservation, AirportData } from '../src/core';

function fixture(options: { forbiddenRecoveryTurn?: boolean; wideConnector?: boolean; checkpointAtRecoveryPosition?: boolean } = {}) {
  const coordinates: [string, number, number][] = [['S', -100, 0], ['A', 0, 0], ['B', 100, 0], ['C', 200, 0], ['G', 300, 0], ['U', 0, 100], ['V', 100, 100]];
  const nodes = coordinates.map(([id, x, y]) => ({ id, x, y, lat: 49, lon: 2 })), byId = new Map(nodes.map(n => [n.id, n]));
  const pairs = [['SA', 'S', 'A'], ['AB', 'A', 'B'], ['BC', 'B', 'C'], ['CG', 'C', 'G'], ['AU', 'A', 'U'], ['UV', 'U', 'V'], ['VB', 'V', 'B']];
  const data: AirportData = { nodes, edges: pairs.map(([id, from, to]) => ({ id, from, to, name: id, kind: 'taxiway', length: Math.hypot(byId.get(to)!.x - byId.get(from)!.x, byId.get(to)!.y - byId.get(from)!.y) })), locations: [{ ...byId.get('S')!, id: 'stand:S', nodeId: 'S', name: 'S', kind: 'stand' }, { ...byId.get('G')!, id: 'holding:G', nodeId: 'G', name: 'G', kind: 'holding_point' }], surfaces: [], metadata: {} };
  const graph = new TaxiGraph(data), source = { id: 'test-source', title: 'Synthetic source', url: 'https://example.test/source' };
  const rules = new OperatingRules(graph, { version: 1, airport: 'LFPG', effectiveDate: '2026-11-13', sources: [source], edgeRules: data.edges.map(edge => ({ id: `direction-${edge.id}`, edgeIds: [edge.id], configuration: 'both', direction: 'forward', sourceIds: [source.id], reason: 'Source direction.' })), forbidUTurns: true, restrictParkingToEndpoints: true,
    turnRules: options.forbiddenRecoveryTurn ? [{ id: 'prohibited-turn', viaNodeId: 'U', fromEdgeId: 'AU', toEdgeId: 'UV', configuration: 'both', sourceIds: [source.id], reason: 'No turn into the recovery connector.' }] : [],
    runwayUse: { west: { departure: ['26R'], arrival: ['26L'] }, east: { departure: ['08L'], arrival: ['08R'] } }, runwayAccess: [{ locationId: 'holding:G', runway: '26R', role: 'departure', sourceIds: [source.id] }, { locationId: 'holding:G', runway: '08L', role: 'departure', sourceIds: [source.id] }],
    clearanceRules: [{ id: 'crossing-C', nodeId: 'C', configuration: 'both', kind: 'runway-crossing', sourceIds: [source.id], reason: 'Manual runway checkpoint.' }, ...(options.checkpointAtRecoveryPosition ? [{ id: 'crossing-U', nodeId: 'U', configuration: 'both' as const, kind: 'runway-crossing' as const, sourceIds: [source.id], reason: 'Manual checkpoint at the observed recovery position.' }] : [])] });
  assert.ok(rules.valid, JSON.stringify(rules.diagnostics));
  const planner = new RoutePlanner(graph, rules), flow = new TaxiFlowPolicy(graph, { version: 1, airport: 'LFPG', sources: [source], axes: [{ id: 'main', name: 'T', role: 'main', edgeIds: ['BC', 'CG'], sourceIds: [source.id] }], zones: [{ id: 'apron', name: 'Source apron', standIds: ['stand:S'], edgeIds: ['SA'], portalEdgeIds: ['SA'], sourceIds: [source.id] }], flowPolicies: [{ id: 'standard', configuration: 'both', movement: 'departure', originZoneIds: ['apron'], originPortalEdgeIds: ['SA'], requiredAxisGroups: [['main']], preferredAxisIds: ['main'], requireMainAxis: true, sourceIds: [source.id], reason: 'Use the source portal then T.' }] });
  assert.ok(flow.valid); planner.setFlowPolicy(flow);
  planner.setAircraftRestrictions(new AircraftRestrictions(graph, { version: 1, airport: 'LFPG', sources: [source], edgeRules: options.wideConnector ? [{ id: 'span-36', edgeIds: ['UV'], direction: 'closed', configuration: 'both', condition: { wingspanMetres: { gt: 36 } }, sourceIds: [source.id], reason: 'MAX SPAN36.' }] : [], turnRules: [] }));
  const sim = new AircraftSimulator(graph, planner), add = (typeCode = 'A320', wingspanMetres = 35.8) => {
    const aircraft = sim.addAircraft('stand:S', 'holding:G', { callsign: 'AFR123', speed: 5, movement: 'departure', aircraftProfile: { typeCode, wingspanMetres } }); assert.ok(aircraft); return aircraft;
  };
  const observation: AircraftObservation = { edgeId: 'AU', fromNodeId: 'A', toNodeId: 'U', edgeProgress: .25 };
  return { graph, planner, sim, add, observation };
}

test('normal assigned travel and manual runway holds never create an off-track alert', () => {
  const { sim, add } = fixture(), aircraft = add();
  for (let i = 0; i < 20; i++) sim.update(5);
  assert.equal(aircraft.state, 'holding'); assert.ok(aircraft.waitingClearanceId);
  assert.equal(aircraft.conformance?.status, 'on-route'); assert.deepEqual(aircraft.conformance?.events, []);
  const position = { edgeId: aircraft.edgeId!, fromNodeId: aircraft.fromNodeId, toNodeId: aircraft.toNodeId, edgeProgress: aircraft.edgeProgress };
  assert.equal(sim.observeAircraft(aircraft.id, position), null);
});

test('a wrong branch identifies the callsign and computes a held, legal main-axis rejoin at the actual position', () => {
  const { sim, planner, add, observation } = fixture(), aircraft = add(), assigned = JSON.stringify(aircraft.assignedRoute);
  const event = sim.observeAircraft(aircraft.id, observation); assert.ok(event);
  assert.equal(event.callsign, 'AFR123'); assert.match(event.reason, /hors itinéraire/);
  assert.equal(event.status, 'recovering'); assert.equal(event.rejoinNodeId, 'B');
  assert.equal(aircraft.state, 'holding'); assert.equal(aircraft.conformance?.awaitingApproval, true);
  assert.equal(aircraft.x, 0); assert.equal(aircraft.y, 25); assert.equal(aircraft.edgeId, 'AU');
  assert.equal(JSON.stringify(aircraft.assignedRoute), assigned); assert.ok(Object.isFrozen(aircraft.assignedRoute?.edgeIds));
  assert.deepEqual(aircraft.route.edgeIds, ['SA', 'AU', 'UV', 'VB', 'BC', 'CG']);
  assert.ok(planner.isRouteAllowed(aircraft.route, 'departure', { originId: aircraft.originId, destinationId: aircraft.destinationId, aircraftProfile: aircraft.aircraftProfile }));
  sim.update(20); assert.equal(aircraft.y, 25, 'Recovery must wait for explicit approval.');
  sim.setHolding(aircraft.id, false); sim.update(56);
  assert.equal(aircraft.conformance?.status, 'on-route'); assert.equal(event.status, 'rejoined');
  sim.update(60); assert.equal(aircraft.state, 'holding'); assert.ok(aircraft.waitingClearanceId, 'Recovery cannot grant a runway crossing.');
  assert.equal(aircraft.toNodeId, 'G'); assert.equal(aircraft.edgeProgress, 0);
  assert.equal(sim.clearRunwayCrossing(aircraft.id), true); sim.update(30); assert.equal(aircraft.state, 'arrived');
});

test('approving a recovery at the current runway checkpoint does not grant the crossing', () => {
  const { sim, add, observation } = fixture({ checkpointAtRecoveryPosition: true }), aircraft = add();
  const event = sim.observeAircraft(aircraft.id, { ...observation, edgeProgress: 1 });
  assert.equal(event?.status, 'recovering');
  assert.equal(aircraft.conformance?.awaitingApproval, true);
  assert.match(aircraft.waitingClearanceId ?? '', /^crossing-U@/);
  const checkpoint = aircraft.waitingClearanceId, position = [aircraft.x, aircraft.y, aircraft.distance];
  sim.update(0); sim.update(1);
  assert.equal(aircraft.conformance?.status, 'recovering', 'The retained observed incoming edge at progress1 is equivalent to the next route edge at progress0.');
  assert.equal(aircraft.conformance?.events.length, 1);
  assert.deepEqual([aircraft.x, aircraft.y, aircraft.distance], position);
  assert.equal(sim.clearRunwayCrossing(aircraft.id), false, 'Crossing approval cannot implicitly approve a newly calculated recovery.');
  sim.setHolding(aircraft.id, false);
  assert.equal(aircraft.conformance?.awaitingApproval, false, 'The explicit recovery action must remain usable while at a checkpoint.');
  assert.equal(aircraft.state, 'holding');
  assert.equal(aircraft.waitingClearanceId, checkpoint);
  assert.deepEqual(aircraft.clearedClearanceIds ?? [], []);
  sim.update(20); assert.deepEqual([aircraft.x, aircraft.y, aircraft.distance], position);
  assert.equal(sim.clearRunwayCrossing(aircraft.id), true);
  sim.update(2); assert.ok(aircraft.distance > position[2]);
});

test('incoming-edge forbidden turns prevent a recovery and ordinary resume cannot bypass the hold', () => {
  const { sim, add, observation } = fixture({ forbiddenRecoveryTurn: true }), aircraft = add();
  const event = sim.observeAircraft(aircraft.id, observation)!;
  assert.equal(event.status, 'unresolved'); assert.equal(aircraft.conformance?.status, 'unresolved');
  sim.setHolding(aircraft.id, false); assert.equal(sim.clearRunwayCrossing(aircraft.id), false); sim.update(100);
  assert.equal(aircraft.state, 'holding'); assert.equal(aircraft.x, 0); assert.equal(aircraft.y, 25);
  sim.observeAircraft(aircraft.id, observation); assert.equal(aircraft.conformance?.events.length, 1, 'The same unresolved position is not an alert storm.');
});

test('wrong-way observations hold at the actual occupied arc rather than legalizing or reversing it', () => {
  const { sim, add } = fixture(), aircraft = add();
  const event = sim.observeAircraft(aircraft.id, { edgeId: 'AB', fromNodeId: 'B', toNodeId: 'A', edgeProgress: .2 })!;
  assert.match(event.reason, /Sens opposé/); assert.equal(event.status, 'unresolved');
  assert.equal(aircraft.x, 80); assert.equal(aircraft.fromNodeId, 'B'); assert.equal(aircraft.toNodeId, 'A');
  sim.update(60); assert.equal(aircraft.x, 80); assert.equal(aircraft.state, 'holding');
});

test('recovery preserves aircraft wingspan instead of searching with the selected smaller profile', () => {
  const small = fixture({ wideConnector: true }), smallAircraft = small.add();
  assert.equal(small.sim.observeAircraft(smallAircraft.id, small.observation)?.status, 'recovering');
  const heavy = fixture({ wideConnector: true }), heavyAircraft = heavy.add('A388', 79.75);
  heavy.planner.setAircraftProfile({ typeCode: 'A320', wingspanMetres: 35.8 });
  assert.equal(heavy.sim.observeAircraft(heavyAircraft.id, heavy.observation)?.status, 'unresolved');
  assert.equal(heavyAircraft.aircraftProfile?.typeCode, 'A388');
});

test('a position beyond an uncleared checkpoint cannot silently become a valid rejoin', () => {
  const { sim, add } = fixture(), aircraft = add();
  const event = sim.observeAircraft(aircraft.id, { edgeId: 'CG', fromNodeId: 'C', toNodeId: 'G', edgeProgress: .5 })!;
  assert.equal(event.status, 'unresolved'); assert.match(event.reason, /point d’attente non autorisé/);
  assert.equal(aircraft.x, 250); assert.equal(aircraft.state, 'holding');
});

test('the explicit wrong-turn demo travels normally to its junction before deviating', () => {
  const { sim, add } = fixture(), aircraft = add();
  assert.equal(sim.simulateWrongTurn(aircraft.id), true);
  sim.update(19.8); assert.equal(aircraft.x, -1); assert.equal(aircraft.conformance?.events.length, 0);
  sim.update(.3); assert.equal(aircraft.x, 0); assert.equal(aircraft.y, 0); assert.equal(aircraft.edgeId, 'AU');
  assert.equal(aircraft.conformance?.events.length, 1); assert.equal(aircraft.conformance?.status, 'recovering');
});

test('periodic monitoring catches a changed graph-matched observed arc before integration overwrites it', () => {
  const { sim, add } = fixture(), aircraft = add();
  Object.assign(aircraft, { edgeId: 'AU', fromNodeId: 'A', toNodeId: 'U', edgeProgress: .1, x: 0, y: 10 });
  sim.update(.1);
  assert.equal(aircraft.state, 'holding'); assert.equal(aircraft.conformance?.events.length, 1);
  assert.equal(aircraft.x, 0); assert.equal(aircraft.y, 10);
});

test('replacing the active route does not authorize a different path from the immutable assignment', () => {
  const { sim, planner, graph, add } = fixture(), aircraft = add(), assigned = JSON.stringify(aircraft.assignedRoute);
  const nodeIds = ['S', 'A', 'U', 'V', 'B', 'C', 'G'], edgeIds = ['SA', 'AU', 'UV', 'VB', 'BC', 'CG'], cumulativeDistances = [0];
  for (const edgeId of edgeIds) cumulativeDistances.push(cumulativeDistances.at(-1)! + graph.edges.get(edgeId)!.length);
  const replacement = planner.describeRoute({ ...aircraft.route, nodeIds, edgeIds, cumulativeDistances, distance: cumulativeDistances.at(-1)! }, 'departure', aircraft.aircraftProfile);
  assert.ok(planner.isRouteAllowed(replacement, 'departure'));
  aircraft.route = replacement; aircraft.distance = 125;
  Object.assign(aircraft, { edgeId: 'AU', fromNodeId: 'A', toNodeId: 'U', edgeProgress: .25, x: 0, y: 25 });
  sim.update(0);
  assert.equal(aircraft.conformance?.events.length, 1); assert.equal(aircraft.state, 'holding');
  assert.equal(JSON.stringify(aircraft.assignedRoute), assigned);
});

test('positions immediately before a node use the same boundary convention as the integrator', () => {
  const { sim, add } = fixture(), aircraft = add(), distance = 100 - 5e-9;
  aircraft.distance = distance;
  Object.assign(aircraft, { edgeId: 'SA', fromNodeId: 'S', toNodeId: 'A', edgeProgress: distance / 100, x: -100 + distance, y: 0 });
  sim.update(0);
  assert.equal(aircraft.edgeId, 'SA'); assert.equal(aircraft.conformance?.events.length, 0);
});

test('recovery completes exactly at its rejoin node rather than while still on the connector', () => {
  const { sim, add, observation } = fixture(), aircraft = add();
  sim.observeAircraft(aircraft.id, observation); sim.setHolding(aircraft.id, false);
  const rejoinDistance = aircraft.conformance!.rejoinDistance!;
  sim.update((rejoinDistance - aircraft.distance - 5e-9) / aircraft.speed);
  assert.equal(aircraft.edgeId, 'VB'); assert.equal(aircraft.conformance?.status, 'recovering');
  sim.update(0); assert.equal(aircraft.conformance?.events.length, 1);
  sim.update(1e-8); assert.equal(aircraft.edgeId, 'BC'); assert.equal(aircraft.conformance?.status, 'on-route');
  sim.update(0); assert.equal(aircraft.conformance?.events.length, 1);
});
