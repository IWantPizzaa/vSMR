import { strict as assert } from 'node:assert';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import { AircraftRestrictions, AircraftSimulator, OperatingRules, RoutePlanner, TaxiFlowPolicy, TaxiGraph } from '../src/core';
import { resolveOperatingManifest } from '../src/core/resolveOperatingManifest';
import type { AircraftProfile, AirportData, Route } from '../src/core/types';

function fixture(options: { forbiddenTurn?: boolean; narrow?: boolean; checkpointAtA?: boolean; profile?: AircraftProfile; distance?: number; scheduledAt?: number } = {}) {
  const coords: [string, number, number][] = [['S', 0, 0], ['A', 100, 0], ['B', 200, 0], ['C', 300, 0], ['G', 400, 0], ['U', 100, 100], ['V', 200, 100], ['W', 200, 180], ['X', 300, 180], ['F', 200, -100]];
  const nodes = coords.map(([id, x, y]) => ({ id, x, y, lat: 49, lon: 2 })), byId = new Map(nodes.map(node => [node.id, node]));
  const pairs = [['SA', 'S', 'A'], ['AB', 'A', 'B'], ['BC', 'B', 'C'], ['CG', 'C', 'G'], ['AU', 'A', 'U'], ['UV', 'U', 'V'], ['VB', 'V', 'B'], ['UW', 'U', 'W'], ['WX', 'W', 'X'], ['XC', 'X', 'C'], ['BF', 'B', 'F'], ['FC', 'F', 'C']];
  const data: AirportData = { nodes, edges: pairs.map(([id, from, to]) => ({ id, from, to, name: id, kind: 'taxiway', length: Math.hypot(byId.get(to)!.x - byId.get(from)!.x, byId.get(to)!.y - byId.get(from)!.y) })), locations: [{ id: 'stand:S', name: 'S', kind: 'stand', nodeId: 'S', x: 0, y: 0 }, { id: 'hold:G', name: 'G', kind: 'holding_point', nodeId: 'G', x: 400, y: 0 }], surfaces: [], metadata: {} };
  const graph = new TaxiGraph(data), sources = [{ id: 'source', title: 'Independent fixture', url: 'https://example.test/aip' }];
  const rules = new OperatingRules(graph, { version: 1, airport: 'LFPG', effectiveDate: '2026-11-13', sources, edgeRules: pairs.map(([id]) => ({ id: `direction-${id}`, edgeIds: [id], configuration: 'both', direction: 'forward', reason: 'Published direction.', sourceIds: ['source'] })), forbidUTurns: true, restrictParkingToEndpoints: true,
    turnRules: options.forbiddenTurn ? [{ id: 'forbid-SA-AU', viaNodeId: 'A', fromEdgeId: 'SA', toEdgeId: 'AU', configuration: 'both', reason: 'Forbidden junction turn.', sourceIds: ['source'] }] : [],
    clearanceRules: [{ id: 'crossing', nodeId: options.checkpointAtA ? 'A' : 'C', configuration: 'both', kind: 'runway-crossing', sourceIds: ['source'], reason: 'Explicit crossing clearance required.' }],
    runwayAccess: [{ locationId: 'hold:G', runway: '09R', role: 'departure', sourceIds: ['source'] }], runwayUse: { east: { departure: ['09R'], arrival: [] }, west: { departure: ['26L'], arrival: [] } },
  });
  const flow = new TaxiFlowPolicy(graph, { version: 1, airport: 'LFPG', sources, axes: [{ id: 'straight', name: 'MAIN 1', role: 'main', edgeIds: ['AB'], sourceIds: ['source'] }, { id: 'final', name: 'MAIN 2', role: 'main', edgeIds: ['BC', 'CG'], sourceIds: ['source'] }, { id: 'connector', name: 'CONNECTORS', role: 'connector', edgeIds: pairs.map(([id]) => id).filter(id => !['AB', 'BC', 'CG'].includes(id)), sourceIds: ['source'] }], zones: [], flowPolicies: [{ id: 'nominal-straight', configuration: 'both', movement: 'departure', requiredAxisGroups: [['straight']], firstMainAxisIds: ['straight'], preferredAxisIds: ['straight', 'final'], sourceIds: ['source'], reason: 'The standard route uses AB before final.' }] });
  assert.ok(rules.valid, JSON.stringify(rules.diagnostics)); assert.ok(flow.valid, flow.diagnostics.join('; '));
  const planner = new RoutePlanner(graph, rules, 'east'); planner.setFlowPolicy(flow);
  const restrictions = new AircraftRestrictions(graph, { version: 1, airport: 'LFPG', effectiveDate: '2026-11-13', sources, edgeRules: options.narrow ? [{ id: 'narrow-AU', edgeIds: ['AU'], configuration: 'both', direction: 'closed', condition: { wingspanMetres: { gt: 36 } }, sourceIds: ['source'], reason: 'AU <=36m.' }] : [], turnRules: [], advisories: [] });
  assert.ok(restrictions.valid, restrictions.diagnostics.join('; ')); planner.setAircraftRestrictions(restrictions);
  if (options.profile) planner.setAircraftProfile(options.profile);
  const sim = new AircraftSimulator(graph, planner), aircraft = sim.addAircraft('stand:S', 'hold:G', { callsign: 'AFR123', movement: 'departure', speed: 5, distance: options.distance ?? 50, scheduledAt: options.scheduledAt });
  assert.ok(aircraft); assert.deepEqual(aircraft.route.edgeIds, ['SA', 'AB', 'BC', 'CG']);
  return { graph, planner, sim, aircraft, rules };
}

const pose = (a: ReturnType<typeof fixture>['aircraft']) => [a.x, a.y, a.heading, a.distance];

test('dragging a future junction previews and attributes a legal nonstandard route without moving the aircraft', () => {
  const { sim, planner, aircraft } = fixture(), original = JSON.stringify(aircraft.standardRoute), position = pose(aircraft);
  const request = { anchorNodeIndex: 2, targetNodeId: 'U' }, preview = sim.previewAssignedRoute(aircraft.id, request);
  assert.equal(preview.ok, true); assert.deepEqual(pose(aircraft), position); assert.equal(aircraft.state, 'taxiing');
  assert.equal(aircraft.assignedRoute?.assignment?.source, 'standard');
  const edited = sim.editAssignedRoute(aircraft.id, request); assert.ok(edited.ok);
  assert.deepEqual(edited.route.edgeIds, ['SA', 'AU', 'UV', 'VB', 'BC', 'CG']);
  assert.deepEqual(pose(aircraft), position); assert.equal(aircraft.state, 'holding');
  assert.equal(aircraft.assignedRoute?.assignment?.source, 'controller'); assert.equal(aircraft.assignedRoute?.assignment?.revision, 1);
  assert.deepEqual(aircraft.assignedRoute?.assignment?.waypointNodeIds, ['U']); assert.ok(Object.isFrozen(aircraft.assignedRoute?.edgeIds));
  assert.equal(JSON.stringify(aircraft.standardRoute), original); assert.equal(aircraft.route.flow?.status, 'review');
  assert.deepEqual(aircraft.route.flow?.requiredStages, []);
  assert.ok(planner.isRouteAllowed(aircraft.route, 'departure', { originId: 'stand:S', destinationId: 'hold:G', aircraftProfile: aircraft.aircraftProfile }));
  sim.setHolding(aircraft.id, false); sim.update(20);
  assert.equal(aircraft.conformance?.events.length, 0, 'An accepted assignment is now the conformance reference.');
});

test('the controller assignment cannot bypass forbidden seam turns or aircraft limits', () => {
  const forbidden = fixture({ forbiddenTurn: true });
  const denied = forbidden.sim.editAssignedRoute(forbidden.aircraft.id, { anchorNodeIndex: 2, targetNodeId: 'U' });
  assert.equal(denied.ok, false); assert.equal(forbidden.aircraft.assignedRoute?.assignment?.source, 'standard');
  const heavy = fixture({ narrow: true, profile: { typeCode: 'A388', wingspanMetres: 79.75 } });
  heavy.planner.setAircraftProfile({ typeCode: 'A320', wingspanMetres: 35.8 });
  assert.equal(heavy.sim.editAssignedRoute(heavy.aircraft.id, { anchorNodeIndex: 2, targetNodeId: 'U' }).ok, false);
  assert.equal(heavy.aircraft.aircraftProfile?.typeCode, 'A388');
});

test('manual geometry is authorized by the planner rather than by a forged metadata flag', () => {
  const { sim, planner, aircraft } = fixture();
  const forged: Route = { ...aircraft.route, assignment: { source: 'controller', revision: 1, waypointNodeIds: [] } };
  assert.equal(planner.isRouteAllowed(forged, 'departure'), false);
  const edited = sim.editAssignedRoute(aircraft.id, { anchorNodeIndex: 2, targetNodeId: 'U' }); assert.ok(edited.ok);
  const rewritten = { ...edited.route, assignment: { ...edited.route.assignment!, waypointNodeIds: ['V'] } };
  assert.equal(planner.isRouteAllowed(rewritten, 'departure'), false);
});

test('several edits preserve ordered user junctions and undo restores each previous assignment safely', () => {
  const { sim, aircraft } = fixture(), original = JSON.stringify(aircraft.assignedRoute), position = pose(aircraft);
  assert.ok(sim.editAssignedRoute(aircraft.id, { anchorNodeIndex: 2, targetNodeId: 'U' }).ok);
  const first = JSON.stringify(aircraft.assignedRoute);
  assert.ok(sim.editAssignedRoute(aircraft.id, { anchorNodeIndex: aircraft.route.nodeIds.indexOf('C'), targetNodeId: 'F' }).ok);
  assert.deepEqual(aircraft.assignedRoute?.assignment?.waypointNodeIds, ['U', 'F']);
  assert.deepEqual(aircraft.route.edgeIds, ['SA', 'AU', 'UV', 'VB', 'BF', 'FC', 'CG']);
  assert.ok(sim.undoLastRouteEdit(aircraft.id).ok); assert.equal(JSON.stringify(aircraft.assignedRoute), first);
  assert.ok(sim.undoLastRouteEdit(aircraft.id).ok); assert.equal(JSON.stringify(aircraft.assignedRoute), original);
  assert.deepEqual(pose(aircraft), position); assert.equal(aircraft.state, 'holding');
  assert.equal(sim.undoLastRouteEdit(aircraft.id).ok, false);
});

test('dragging an existing manual junction replaces its constraint and undo cannot teleport after entering it', () => {
  const { sim, aircraft } = fixture();
  assert.ok(sim.editAssignedRoute(aircraft.id, { anchorNodeIndex: 2, targetNodeId: 'U' }).ok);
  assert.ok(sim.editAssignedRoute(aircraft.id, { anchorNodeIndex: aircraft.route.nodeIds.indexOf('U'), targetNodeId: 'W' }).ok);
  assert.deepEqual(aircraft.assignedRoute?.assignment?.waypointNodeIds, ['W']);
  assert.deepEqual(aircraft.route.edgeIds, ['SA', 'AU', 'UW', 'WX', 'XC', 'CG']);
  sim.setHolding(aircraft.id, false); sim.update(50); const position = pose(aircraft);
  assert.equal(sim.undoLastRouteEdit(aircraft.id).ok, false, 'The changed UW edge was already entered.');
  assert.deepEqual(pose(aircraft), position);
});

test('editing cancels an untravelled crossing authorization and retains a genuinely passed one', () => {
  const atHold = fixture({ checkpointAtA: true, distance: 100 });
  assert.equal(atHold.sim.clearRunwayCrossing(atHold.aircraft.id), true);
  assert.ok(atHold.sim.editAssignedRoute(atHold.aircraft.id, { anchorNodeIndex: 2, targetNodeId: 'U' }).ok);
  assert.deepEqual(atHold.aircraft.clearedClearanceIds, []); assert.ok(atHold.aircraft.waitingClearanceId);
  atHold.sim.setHolding(atHold.aircraft.id, false); atHold.sim.update(10); assert.equal(atHold.aircraft.distance, 100);
  assert.equal(atHold.sim.clearRunwayCrossing(atHold.aircraft.id), true);
  const passed = fixture({ checkpointAtA: true, distance: 100 });
  assert.equal(passed.sim.clearRunwayCrossing(passed.aircraft.id), true); passed.sim.update(10);
  const cleared = [...passed.aircraft.clearedClearanceIds!], position = pose(passed.aircraft);
  assert.ok(passed.sim.editAssignedRoute(passed.aircraft.id, { anchorNodeIndex: 3, targetNodeId: 'F' }).ok);
  assert.deepEqual(passed.aircraft.clearedClearanceIds, cleared); assert.deepEqual(pose(passed.aircraft), position);
});

test('scheduled traffic keeps its release and passed points or unresolved aircraft cannot be dragged', () => {
  const future = fixture({ distance: 0, scheduledAt: 90 });
  assert.ok(future.sim.editAssignedRoute(future.aircraft.id, { anchorNodeIndex: 2, targetNodeId: 'U' }).ok);
  assert.equal(future.aircraft.state, 'scheduled'); assert.equal(future.aircraft.scheduledAt, 90);
  const current = fixture({ distance: 150 });
  assert.equal(current.sim.editAssignedRoute(current.aircraft.id, { anchorNodeIndex: 1, targetNodeId: 'U' }).ok, false);
  current.sim.observeAircraft(current.aircraft.id, { edgeId: 'AB', fromNodeId: 'B', toNodeId: 'A', edgeProgress: .25 });
  assert.equal(current.aircraft.conformance?.status, 'unresolved');
  assert.equal(current.sim.editAssignedRoute(current.aircraft.id, { anchorNodeIndex: 3, targetNodeId: 'F' }).ok, false);
});

test('recovery after a controller edit rejoins that assignment and retains its passed manual junction', () => {
  const { sim, planner, aircraft } = fixture();
  assert.ok(sim.editAssignedRoute(aircraft.id, { anchorNodeIndex: 2, targetNodeId: 'U' }).ok);
  const assignment = JSON.stringify(aircraft.assignedRoute), standard = JSON.stringify(aircraft.standardRoute);
  const event = sim.observeAircraft(aircraft.id, { edgeId: 'UW', fromNodeId: 'U', toNodeId: 'W', edgeProgress: .25 });
  assert.equal(event?.status, 'recovering');
  assert.equal(aircraft.route.assignment?.source, 'controller'); assert.deepEqual(aircraft.route.assignment?.waypointNodeIds, ['U']);
  assert.ok(planner.isRouteAllowed(aircraft.route, 'departure', { originId: aircraft.originId, destinationId: aircraft.destinationId, aircraftProfile: aircraft.aircraftProfile }));
  assert.equal(JSON.stringify(aircraft.assignedRoute), assignment); assert.equal(JSON.stringify(aircraft.standardRoute), standard);
  sim.setHolding(aircraft.id, false); sim.update(100);
  assert.equal(aircraft.conformance?.status, 'on-route'); assert.equal(aircraft.state, 'holding'); assert.ok(aircraft.waitingClearanceId);
  assert.equal(aircraft.conformance?.events.length, 1);
});

test('the real LFPG M09 to T3 route accepts the neighboring actual junction and preserves its standard reference', () => {
  const root = new URL('../public/data/', import.meta.url), read = (file: string) => JSON.parse(readFileSync(new URL(file, root), 'utf8'));
  const graph = new TaxiGraph(read('lfpg.json')), operations = resolveOperatingManifest(read('lfpg-operations.json'));
  const planner = new RoutePlanner(graph, new OperatingRules(graph, operations), 'east');
  planner.setFlowPolicy(new TaxiFlowPolicy(graph, read('lfpg-corridors.json'))); planner.setAircraftRestrictions(new AircraftRestrictions(graph, read('lfpg-aircraft-restrictions.json')));
  const sim = new AircraftSimulator(graph, planner), aircraft = sim.addAircraft('stand:M09', 'operation:departure:08L:T3', { movement: 'departure' }); assert.ok(aircraft);
  const before = JSON.stringify(aircraft.standardRoute), position = [aircraft.x, aircraft.y, aircraft.distance];
  assert.equal(aircraft.route.nodeIds[37], 'osm:2318941305');
  const result = sim.editAssignedRoute(aircraft.id, { anchorNodeIndex: 37, targetNodeId: 'osm:8968901466' }); assert.ok(result.ok);
  assert.ok(result.route.nodeIds.includes('osm:8968901466')); assert.notDeepEqual(result.route.edgeIds, aircraft.standardRoute?.edgeIds);
  assert.deepEqual([aircraft.x, aircraft.y, aircraft.distance], position); assert.equal(JSON.stringify(aircraft.standardRoute), before);
  assert.ok(planner.isRouteAllowed(result.route, 'departure', { originId: aircraft.originId, destinationId: aircraft.destinationId, aircraftProfile: aircraft.aircraftProfile }));
  assert.ok(sim.undoLastRouteEdit(aircraft.id).ok); assert.equal(JSON.stringify(aircraft.assignedRoute), before);
});
