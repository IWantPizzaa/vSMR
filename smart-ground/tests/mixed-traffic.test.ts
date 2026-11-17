import { strict as assert } from 'node:assert';
import { test } from 'node:test';
import { AircraftSimulator, OperatingRules, RoutePlanner, TaxiGraph, TrafficPredictor } from '../src/core';
import type { AirportData, TaxiEdge } from '../src/core/types';

function fixture(checkpointAtOrigin = false) {
  const nodes = [['S1', 0, 0], ['S2', 0, 200], ['J1', 100, 0], ['J2', 100, 200], ['D', 1000, 0], ['A', 1000, 200]].map(([id, x, y]) => ({ id: String(id), x: Number(x), y: Number(y), lat: 49, lon: 2 }));
  const byId = new Map(nodes.map(node => [node.id, node]));
  const edges = [['S1', 'J1'], ['S2', 'J2'], ['J1', 'D'], ['J2', 'A'], ['J1', 'J2'], ['D', 'A']].map(([from, to], index): TaxiEdge => ({ id: `e${index}`, from, to, length: Math.hypot(byId.get(from)!.x - byId.get(to)!.x, byId.get(from)!.y - byId.get(to)!.y), name: 'T', kind: index < 2 ? 'parking_position' : 'taxiway' }));
  const data: AirportData = { nodes, edges, surfaces: [], metadata: {}, locations: [
    { ...byId.get('S1')!, id: 'stand:S1', nodeId: 'S1', name: 'S1', kind: 'stand' },
    { ...byId.get('S2')!, id: 'stand:S2', nodeId: 'S2', name: 'S2', kind: 'stand' },
    { ...byId.get('D')!, id: 'departure:D', nodeId: 'D', name: 'D', kind: 'holding_point' },
    { ...byId.get('A')!, id: 'arrival:A', nodeId: 'A', name: 'A', kind: 'holding_point' },
  ] };
  const graph = new TaxiGraph(data), source = { id: 'source', title: 'Test topology', url: 'https://example.test/topology' };
  const rules = new OperatingRules(graph, { version: 1, airport: 'LFPG', effectiveDate: '2026-11-13', sources: [source], edgeRules: [], forbidUTurns: true, restrictParkingToEndpoints: true,
    runwayUse: { west: { departure: ['26R'], arrival: ['26L'] }, east: { departure: ['08L'], arrival: ['08R'] } },
    runwayAccess: [{ locationId: 'departure:D', runway: '26R', role: 'departure', sourceIds: ['source'] }, { locationId: 'arrival:A', runway: '26L', role: 'arrival', sourceIds: ['source'] }],
    clearanceRules: checkpointAtOrigin ? [{ id: 'hold-A', nodeId: 'A', configuration: 'both', kind: 'runway-crossing', sourceIds: ['source'], reason: 'Manual crossing at arrival start.' }] : [],
  });
  assert.ok(rules.valid, JSON.stringify(rules.diagnostics));
  const planner = new RoutePlanner(graph, rules), sim = new AircraftSimulator(graph, planner);
  return { graph, planner, sim };
}

test('generateMixed produces deterministic balanced movement metadata, unique callsigns and a staggered queue', () => {
  const first = fixture(), second = fixture();
  assert.equal(first.sim.generateMixed(10, 321), 10); assert.equal(second.sim.generateMixed(10, 321), 10);
  const summary = (sim: AircraftSimulator) => sim.aircraft.map(a => [a.callsign, a.originId, a.destinationId, a.route.operational?.movement, a.scheduledAt, a.state]);
  assert.deepEqual(summary(first.sim), summary(second.sim));
  assert.equal(new Set(first.sim.aircraft.map(a => a.callsign)).size, 10);
  assert.equal(first.sim.aircraft.filter(a => a.route.operational?.movement === 'arrival').length, 5);
  assert.equal(first.sim.aircraft.filter(a => a.route.operational?.movement === 'departure').length, 5);
  assert.ok(first.sim.aircraft.filter(a => a.state !== 'scheduled').length <= 2);
  assert.ok(new Set(first.sim.aircraft.map(a => a.scheduledAt)).size > 2);
  const lastAt = new Map<string, number>();
  for (const aircraft of first.sim.aircraft) {
    const origin = aircraft.route.nodeIds[0], at = aircraft.scheduledAt!;
    if (lastAt.has(origin)) assert.ok(at - lastAt.get(origin)! >= 90);
    lastAt.set(origin, at);
    assert.ok(first.planner.isRouteAllowed(aircraft.route, aircraft.route.operational?.movement, { originId: aircraft.originId, destinationId: aircraft.destinationId, aircraftProfile: aircraft.aircraftProfile }));
  }
  const predicted = new TrafficPredictor(first.graph).predict(first.sim.aircraft, 300);
  assert.equal(predicted.length, first.sim.aircraft.filter(a => a.state !== 'scheduled').length);
  assert.ok(predicted.every(p => p.aircraft.state !== 'scheduled'));
});

test('a delayed movement never advances before release and large and small updates agree', () => {
  const first = fixture(), second = fixture();
  const add = (sim: AircraftSimulator) => sim.addAircraft('stand:S1', 'departure:D', { movement: 'departure', speed: 10, scheduledAt: 10 })!;
  const a = add(first.sim), b = add(second.sim);
  first.sim.setHolding(a.id, false); assert.equal(a.state, 'scheduled');
  first.sim.update(9); assert.equal(a.distance, 0); assert.equal(a.state, 'scheduled');
  first.sim.update(11);
  for (let step = 0; step < 80; step++) second.sim.update(.25);
  assert.equal(a.distance, 100); assert.equal(b.distance, 100);
  assert.equal(a.state, 'taxiing'); assert.deepEqual([a.x, a.y], [b.x, b.y]);
  assert.deepEqual(a.conformance?.events, []); assert.deepEqual(b.conformance?.events, []);
});

test('scheduled reuse of an occupied origin waits for actual clearance of its initial area', () => {
  const { sim } = fixture();
  const first = sim.addAircraft('stand:S1', 'departure:D', { movement: 'departure', speed: 10 })!;
  sim.setHolding(first.id, true);
  const pending = sim.addAircraft('stand:S1', 'departure:D', { movement: 'departure', speed: 10, scheduledAt: 5 })!;
  sim.update(10); assert.equal(pending.state, 'scheduled'); assert.equal(pending.distance, 0);
  sim.setHolding(first.id, false); sim.update(5);
  assert.equal(pending.state, 'scheduled'); assert.equal(pending.distance, 0, 'A predecessor still within 60m keeps the origin occupied.');
  sim.update(2);
  assert.equal(pending.state, 'taxiing');
  assert.ok(first.distance >= 60);
  assert.ok(pending.distance <= 20, 'A late release must not accumulate distance for time spent waiting on its source.');
});

test('overdue generated arrivals retain the 90-second actual release gap after an occupied origin delays the queue', () => {
  const { sim } = fixture();
  assert.equal(sim.generateMixed(6, 321, { windowSeconds: 60 }), 6);
  const arrivals = sim.aircraft.filter(a => a.route.operational?.movement === 'arrival');
  assert.equal(arrivals.length, 3); assert.ok(arrivals.every(a => a.route.nodeIds[0] === 'A'));
  const [first, second, third] = arrivals;
  assert.notEqual(first.state, 'scheduled'); sim.setHolding(first.id, true);
  sim.update(220);
  assert.equal(second.state, 'scheduled'); assert.equal(third.state, 'scheduled');
  assert.ok(second.scheduledAt! < sim.time && third.scheduledAt! < sim.time);
  sim.setHolding(first.id, false);
  for (let step = 0; step < 100 && second.state === 'scheduled'; step++) sim.update(1);
  assert.notEqual(second.state, 'scheduled'); const secondReleasedAt = sim.time;
  sim.update(30); assert.equal(third.state, 'scheduled', 'Nominal slots are overdue, but actual arrivals must not bunch when the origin clears.');
  for (let step = 0; step < 100 && third.state === 'scheduled'; step++) sim.update(1);
  assert.notEqual(third.state, 'scheduled'); assert.ok(sim.time - secondReleasedAt >= 90);
});

test('scheduled arrivals cannot acquire crossing clearance or be resumed before their release', () => {
  const { sim } = fixture(true);
  const aircraft = sim.addAircraft('arrival:A', 'stand:S1', { movement: 'arrival', scheduledAt: 10, speed: 10 })!;
  assert.equal(aircraft.state, 'scheduled'); assert.ok(aircraft.waitingClearanceId);
  assert.equal(sim.clearRunwayCrossing(aircraft.id), false);
  sim.setHolding(aircraft.id, false); sim.update(9); assert.equal(aircraft.state, 'scheduled'); assert.equal(aircraft.distance, 0);
  sim.update(1); assert.equal(aircraft.state, 'holding'); assert.equal(aircraft.distance, 0);
  sim.setHolding(aircraft.id, false); sim.update(5); assert.equal(aircraft.distance, 0);
  assert.equal(sim.clearRunwayCrossing(aircraft.id), true); sim.update(1); assert.equal(aircraft.distance, 10);
});

test('explicit legacy generation remains immediate and homogeneous for operational validators', () => {
  const { sim } = fixture();
  assert.equal(sim.generate(6, 12, { movement: 'departure' }), 6);
  assert.ok(sim.aircraft.every(a => a.state !== 'scheduled' && a.scheduledAt === undefined && a.route.operational?.movement === 'departure'));
});
