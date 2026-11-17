import { strict as assert } from 'node:assert';
import { test } from 'node:test';
import { AircraftSimulator } from '../src/core/AircraftSimulator';
import { ConflictDetector } from '../src/core/ConflictDetector';
import { RoutePlanner } from '../src/core/RoutePlanner';
import { SurveillanceConflictMonitor, type SurveillanceTrackQuality } from '../src/core/SurveillanceConflictMonitor';
import { TaxiGraph } from '../src/core/TaxiGraph';
import type { Aircraft, AirportData } from '../src/core/types';

function fixture(crossing = false) {
  const points: [string, number, number][] = crossing ? [['W', -500, 0], ['C', 0, 0], ['E', 500, 0], ['S', 0, -500], ['N', 0, 500]] : [['A', 0, 0], ['B', 1000, 0]];
  const links = crossing ? [['W', 'C'], ['C', 'E'], ['S', 'C'], ['C', 'N']] : [['A', 'B']];
  const nodes = points.map(([id, x, y]) => ({ id, x, y, lat: 49, lon: 2 })), lookup = new Map(nodes.map(n => [n.id, n]));
  const data: AirportData = { nodes, edges: links.map(([from, to], i) => ({ id: `e${i}`, from, to, name: `T${i}`, kind: 'taxiway', length: Math.hypot(lookup.get(from)!.x - lookup.get(to)!.x, lookup.get(from)!.y - lookup.get(to)!.y) })), locations: [], surfaces: [], metadata: {} };
  const graph = new TaxiGraph(data), simulator = new AircraftSimulator(graph), planner = new RoutePlanner(graph);
  const monitor = new SurveillanceConflictMonitor(graph);
  const add = (id: string, from: string, to: string, speed = 10, distance = 0) => simulator.addRoute(planner.findRoute(from, to)!, { id, callsign: id, speed, distance });
  return { graph, simulator, add, monitor };
}

function qualities(aircraft: readonly Aircraft[], sourceObservedAt: number, now = sourceObservedAt, patch: Partial<SurveillanceTrackQuality> = {}): SurveillanceTrackQuality[] {
  return aircraft.map(a => ({ aircraftId: a.id, sourceObservedAt, ageSeconds: now - sourceObservedAt, freshness: 'fresh', status: 'tracking', confidence: .95, uncertaintyMetres: 5, speedUncertaintyMetresPerSecond: .5, ...patch }));
}

test('a nonurgent collision requires two independent radar reports, not repeated render frames', () => {
  const { simulator, add, monitor } = fixture();
  add('EAST', 'A', 'B'); add('WEST', 'B', 'A');
  assert.deepEqual(monitor.detect(simulator.aircraft, qualities(simulator.aircraft, 0), 0, 80), []);
  for (let frame = 1; frame < 20; frame++) assert.deepEqual(monitor.detect(simulator.aircraft, qualities(simulator.aircraft, 0, frame / 5), frame / 5, 80), []);
  simulator.update(5);
  const alerts = monitor.detect(simulator.aircraft, qualities(simulator.aircraft, 5), 5, 80);
  assert.equal(alerts.length, 1); assert.equal(alerts[0].type, 'head-on'); assert.equal(alerts[0].quality, 'confirmed');
  assert.ok(Math.abs(alerts[0].timeToConflict - 42.75) < 1e-8);
  assert.ok(alerts[0].timeToConflictRange[0] < alerts[0].timeToConflict && alerts[0].timeToConflictRange[1] > alerts[0].timeToConflict);
});

test('a credible urgent head-on and crossing are reported on the first report', () => {
  const head = fixture(); head.add('EAST', 'A', 'B', 10, 350); head.add('WEST', 'B', 'A', 10, 350);
  const h = head.monitor.detect(head.simulator.aircraft, qualities(head.simulator.aircraft, 0), 0, 80)[0];
  assert.equal(h.type, 'head-on'); assert.equal(h.severity, 'critical'); assert.ok(h.timeToConflict < 15);
  const cross = fixture(true); cross.add('EAST', 'W', 'E', 10, 400); cross.add('NORTH', 'S', 'N', 10, 400);
  const c = cross.monitor.detect(cross.simulator.aircraft, qualities(cross.simulator.aircraft, 0), 0, 80)[0];
  assert.equal(c.type, 'crossing'); assert.equal(c.severity, 'critical');
});

test('speed noise does not accumulate isolated possible conflicts into a confirmed alarm', () => {
  const { simulator, add, monitor } = fixture();
  const fast = add('FAST', 'A', 'B', 7.5), slow = add('SLOW', 'A', 'B', 5, 300);
  for (const [time, speed] of [[0, 7.5], [5, 8.5], [10, 7.5], [15, 8.5]] as const) {
    fast.speed = speed;
    assert.deepEqual(monitor.detect([fast, slow], qualities([fast, slow], time), time, 80), []);
  }
  fast.speed = 10;
  assert.equal(monitor.detect([fast, slow], qualities([fast, slow], 20), 20, 80).length, 1, 'The second sustained positive sample confirms the actual faster following movement.');
});

test('release hysteresis preserves a stable pair ID across position and stop jitter', () => {
  const { simulator, add, monitor } = fixture();
  const first = add('FIRST', 'A', 'B', 10, 100), second = add('SECOND', 'A', 'B', 10, 140);
  first.state = 'holding'; second.state = 'holding';
  const initial = monitor.detect(simulator.aircraft, qualities(simulator.aircraft, 0), 0, 80)[0]; assert.ok(initial);
  const setSecond = (distance: number) => { second.distance = distance; second.x = distance; second.edgeProgress = distance / 1000; };
  setSecond(150);
  const band = monitor.detect(simulator.aircraft, qualities(simulator.aircraft, 5), 5, 80)[0]; assert.equal(band.id, initial.id);
  setSecond(145); const threshold = monitor.detect(simulator.aircraft, qualities(simulator.aircraft, 10), 10, 80)[0]; assert.equal(threshold.id, initial.id);
  setSecond(180);
  assert.equal(monitor.detect(simulator.aircraft, qualities(simulator.aircraft, 15), 15, 80).length, 1);
  for (let frame = 1; frame < 10; frame++) assert.equal(monitor.detect(simulator.aircraft, qualities(simulator.aircraft, 15, 15 + frame / 3), 15 + frame / 3, 80).length, 1, 'Frame redraws cannot supply a second clear report.');
  assert.deepEqual(monitor.detect(simulator.aircraft, qualities(simulator.aircraft, 20), 20, 80), []);
});

test('stale and lost reports cannot sustain a confident moving prediction indefinitely', () => {
  const { simulator, add, monitor } = fixture(); add('EAST', 'A', 'B', 10, 350); add('WEST', 'B', 'A', 10, 350);
  assert.equal(monitor.detect(simulator.aircraft, qualities(simulator.aircraft, 0), 0, 80)[0].quality, 'confirmed');
  const stale = monitor.detect(simulator.aircraft, qualities(simulator.aircraft, 0, 11, { freshness: 'stale', status: 'stale', confidence: .5 }), 11, 80)[0];
  assert.equal(stale.quality, 'degraded'); assert.equal(stale.severity, 'warning');
  assert.deepEqual(monitor.detect(simulator.aircraft, qualities(simulator.aircraft, 0, 21, { freshness: 'lost', status: 'lost' }), 21, 80), []);
  assert.deepEqual(monitor.detect(simulator.aircraft, [], 22, 80), []);
});

test('ambiguous geometry is a limited observed footprint, not a confident assigned-route threat', () => {
  const { simulator, add, monitor } = fixture(); add('EAST', 'A', 'B'); add('WEST', 'B', 'A');
  assert.ok(new ConflictDetector(simulator.graph).detect(simulator.aircraft, 80).length);
  for (const time of [0, 5, 10]) assert.deepEqual(monitor.detect(simulator.aircraft, qualities(simulator.aircraft, time, time, { status: 'ambiguous', confidence: .4, uncertaintyMetres: 40 }), time, 80), []);
  const near = fixture(); near.add('ONE', 'A', 'B', 10, 100); near.add('TWO', 'A', 'B', 10, 140);
  assert.deepEqual(near.monitor.detect(near.simulator.aircraft, qualities(near.simulator.aircraft, 0, 0, { status: 'ambiguous', confidence: .4 }), 0, 80), []);
  const warning = near.monitor.detect(near.simulator.aircraft, qualities(near.simulator.aircraft, 5, 5, { status: 'ambiguous', confidence: .4 }), 5, 80)[0];
  assert.equal(warning.quality, 'degraded'); assert.equal(warning.severity, 'warning');
});

test('prediction obeys runway checkpoints and excludes future scheduled traffic', () => {
  const { simulator, add, monitor } = fixture();
  const first = add('FIRST', 'A', 'B'), second = add('SECOND', 'B', 'A');
  first.route.operational = { configuration: 'east', ruleIds: [], sourceIds: [], unverifiedEdgeIds: [], advisories: [], clearanceCheckpoints: [{ id: 'hold', ruleId: 'hold', nodeId: 'A', nodeIndex: 0, distance: 100, kind: 'hold-short', reason: 'Stop', sourceIds: [] }] };
  second.route.operational = { configuration: 'east', ruleIds: [], sourceIds: [], unverifiedEdgeIds: [], advisories: [], clearanceCheckpoints: [{ id: 'hold2', ruleId: 'hold2', nodeId: 'B', nodeIndex: 0, distance: 100, kind: 'hold-short', reason: 'Stop', sourceIds: [] }] };
  assert.deepEqual(monitor.detect(simulator.aircraft, qualities(simulator.aircraft, 0), 0, 120), []);
  first.route.operational = undefined; second.route.operational = undefined;
  second.state = 'scheduled'; second.scheduledAt = 120;
  for (const time of [5, 10]) assert.deepEqual(monitor.detect(simulator.aircraft, qualities(simulator.aircraft, time), time, 120), []);
});

test('bounded uncertainty changes the time range without inflating disconnected parallel tracks', () => {
  const { simulator, add, monitor } = fixture(); add('EAST', 'A', 'B', 10, 350); add('WEST', 'B', 'A', 10, 350);
  const alert = monitor.detect(simulator.aircraft, qualities(simulator.aircraft, 0, 0, { uncertaintyMetres: 10000, speedUncertaintyMetresPerSecond: 10000 }), 0, 80)[0];
  assert.ok(alert.timeToConflict - alert.timeToConflictRange[0] <= 12);
  assert.ok(alert.timeToConflictRange[1] - alert.timeToConflict <= 12);
  assert.throws(() => monitor.detect(simulator.aircraft, [], NaN));
  monitor.reset(); assert.deepEqual(monitor.detect([], [], 0), []);
  const nodes = [ ['A', 0, 0], ['B', 1000, 0], ['C', 0, 10], ['D', 1000, 10] ] as const;
  const data: AirportData = { nodes: nodes.map(([id, x, y]) => ({ id, x, y, lat: 49, lon: 2 })),
    edges: [{ id: 'parallel-1', from: 'A', to: 'B', name: 'N', kind: 'taxiway', length: 1000 }, { id: 'parallel-2', from: 'C', to: 'D', name: 'F', kind: 'taxiway', length: 1000 }], locations: [], surfaces: [], metadata: {} };
  const graph = new TaxiGraph(data), parallel = new AircraftSimulator(graph), parallelMonitor = new SurveillanceConflictMonitor(graph);
  parallel.addRoute(parallel.planner.findRoute('A', 'B')!, { id: 'N', speed: 10, distance: 300 });
  parallel.addRoute(parallel.planner.findRoute('C', 'D')!, { id: 'F', speed: 10, distance: 300 });
  for (const time of [0, 5]) assert.deepEqual(parallelMonitor.detect(parallel.aircraft, qualities(parallel.aircraft, time, time, { uncertaintyMetres: 25 }), time, 80), [], 'Position uncertainty must not fuse nearby disconnected lanes into a collision.');
});

test('an observed moving off-track target is not assumed stopped by a planned recovery hold', () => {
  const { simulator, add, monitor } = fixture();
  const first = add('EAST', 'A', 'B', 10, 400), second = add('WEST', 'B', 'A', 10, 400);
  first.state = 'holding'; first.conformance = { status: 'unresolved', awaitingApproval: true, events: [], assignedNodeIndex: 0 };
  const data = qualities(simulator.aircraft, 0);
  data[0] = { ...data[0], status: 'confirmed-off-track', speedMetresPerSecond: 10, observation: { edgeId: first.edgeId!, fromNodeId: first.fromNodeId, toNodeId: first.toNodeId, edgeProgress: first.edgeProgress } };
  // Stationary-plan reference misses this encounter within10s; the measured
  // motion reaches it in7.75s, without using any turn beyond its current arc.
  assert.deepEqual(new ConflictDetector(simulator.graph).detect([first, second], 10), []);
  const conflict = monitor.detect(simulator.aircraft, data, 0, 10)[0];
  assert.equal(conflict.type, 'head-on'); assert.ok(Math.abs(conflict.timeToConflict - 7.75) < 1e-8);
  assert.equal(first.state, 'holding'); assert.equal(first.conformance.awaitingApproval, true, 'The forecast clone must not approve or move the real simulation target.');
});
