import { strict as assert } from 'node:assert';
import { test } from 'node:test';
import { readFileSync, existsSync } from 'node:fs';
import { performance } from 'node:perf_hooks';
import { AircraftSimulator, ConflictDetector, RoutePlanner, TaxiGraph, TrafficPredictor } from '../src/core/index';
import type { AirportData, TaxiEdge } from '../src/core/types';

function airport(points: [string, number, number][], links: [string, string, string?][]): AirportData {
  const nodes = points.map(([id, x, y]) => ({ id, x, y, lat: 49 + y / 111320, lon: 2 + x / 73000 }));
  const lookup = new Map(nodes.map(n => [n.id, n]));
  const edges: TaxiEdge[] = links.map(([from, to, name], i) => ({ id: `e${i}`, from, to, length: Math.hypot(lookup.get(from)!.x - lookup.get(to)!.x, lookup.get(from)!.y - lookup.get(to)!.y), name: name ?? `T${i}`, kind: 'taxiway' }));
  return { nodes, edges, locations: nodes.map((n, i) => ({ ...n, name: n.id, nodeId: n.id, kind: i === nodes.length - 1 ? 'runway' : 'stand' })), surfaces: [], metadata: {} };
}
const straight = () => new TaxiGraph(airport([['A', 0, 0], ['B', 1000, 0]], [['A', 'B', 'ALPHA']]));
const cross = () => new TaxiGraph(airport([['W', -500, 0], ['C', 0, 0], ['E', 500, 0], ['S', 0, -500], ['N', 0, 500]], [['W', 'C'], ['C', 'E'], ['S', 'C'], ['C', 'N']]));

function dijkstra(graph: TaxiGraph, from: string, to: string): number {
  const costs = new Map<string, number>([[from, 0]]), visited = new Set<string>();
  while (true) {
    let current: string | null = null, best = Infinity;
    for (const [id, cost] of costs) if (!visited.has(id) && cost < best) { current = id; best = cost; }
    if (!current) return Infinity;
    if (current === to) return best;
    visited.add(current);
    for (const arc of graph.adjacency.get(current)!) {
      const edge = graph.edges.get(arc.edgeId)!;
      if (edge.kind === 'runway') continue;
      if (best + arc.length < (costs.get(arc.to) ?? Infinity)) costs.set(arc.to, best + arc.length);
    }
  }
}

test('A* finds the exact shortest path, handles disconnected and identical nodes', () => {
  const graph = new TaxiGraph(airport([['A', 0, 0], ['B', 100, 0], ['C', 100, 100], ['D', 0, 100], ['I', 900, 900]], [['A', 'B'], ['B', 'C'], ['A', 'D'], ['D', 'C'], ['A', 'C']]));
  const planner = new RoutePlanner(graph);
  for (const from of graph.nodes.keys()) for (const to of graph.nodes.keys()) {
    const route = planner.findRoute(from, to), expected = dijkstra(graph, from, to);
    if (expected === Infinity) assert.equal(route, null);
    else { assert.ok(route); assert.ok(Math.abs(route.distance - expected) < 1e-7); }
  }
  assert.equal(graph.components.length, 2);
  assert.equal(planner.findRoute('unknown', 'A'), null);
  assert.equal(planner.findRoute('A', 'C')!.edgeIds.length, 1);
});

test('A* never uses a runway as a taxi shortcut, including terminal segments', () => {
  const data = airport([['A', 0, 0], ['B', 50, 0], ['C', 100, 0], ['D', 150, 0], ['E', 75, 100]], [['A', 'B'], ['B', 'C'], ['C', 'D'], ['A', 'E'], ['E', 'D']]);
  data.edges[1].kind = 'runway';
  const graph = new TaxiGraph(data), planner = new RoutePlanner(graph);
  assert.deepEqual(planner.findRoute('A', 'D')!.nodeIds, ['A', 'E', 'D']);
  assert.deepEqual(planner.findRoute('A', 'C')!.nodeIds, ['A', 'E', 'D', 'C']);
});

test('aircraft interpolate along exact edges, hold, resume, and clamp a large time step', () => {
  const graph = cross(), simulator = new AircraftSimulator(graph), route = new RoutePlanner(graph).findRoute('W', 'N')!;
  const aircraft = simulator.addRoute(route, { speed: 10 });
  simulator.update(25); assert.deepEqual([aircraft.x, aircraft.y], [-250, 0]);
  simulator.setHolding(aircraft.id, true); simulator.update(60); assert.equal(aircraft.distance, 250);
  simulator.setHolding(aircraft.id, false); simulator.update(50); assert.deepEqual([aircraft.x, aircraft.y], [0, 250]);
  simulator.update(1e6); assert.deepEqual([aircraft.x, aircraft.y], [0, 500]); assert.equal(aircraft.state, 'arrived'); assert.equal(aircraft.distance, route.distance);
  assert.throws(() => simulator.update(-1)); assert.throws(() => simulator.update(Infinity));
});

test('continuous predictions detect a future head-on between sparse sample times', () => {
  const graph = straight(), simulator = new AircraftSimulator(graph), planner = simulator.planner;
  simulator.addRoute(planner.findRoute('A', 'B')!, { speed: 10, id: 'east' });
  simulator.addRoute(planner.findRoute('B', 'A')!, { speed: 10, id: 'west' });
  const conflict = new ConflictDetector(graph).detect(simulator.aircraft, 80)[0];
  assert.equal(conflict.type, 'head-on'); assert.ok(Math.abs(conflict.timeToConflict - 47.75) < 1e-7); assert.equal(conflict.location, 'ALPHA');
  assert.equal(new ConflictDetector(graph).detect(simulator.aircraft, 40).length, 0);
});

test('same shared intersection generates a crossing alert, separated arrivals do not', () => {
  const graph = cross(), simulator = new AircraftSimulator(graph);
  simulator.addRoute(simulator.planner.findRoute('W', 'E')!, { speed: 10 });
  const second = simulator.addRoute(simulator.planner.findRoute('S', 'N')!, { speed: 10 });
  const detector = new ConflictDetector(graph), conflicts = detector.detect(simulator.aircraft, 80);
  assert.equal(conflicts[0].type, 'crossing'); assert.equal(conflicts[0].timeToConflict, 47); assert.deepEqual([conflicts[0].x, conflicts[0].y], [0, 0]);
  simulator.removeAircraft(second.id); simulator.addRoute(simulator.planner.findRoute('S', 'N')!, { speed: 10, distance: 490 });
  assert.equal(detector.detect(simulator.aircraft, 80).length, 0);
});

test('different same-direction speeds predict overtaking', () => {
  const graph = straight(), simulator = new AircraftSimulator(graph), route = simulator.planner.findRoute('A', 'B')!;
  simulator.addRoute(route, { speed: 10, id: 'fast' }); simulator.addRoute(route, { speed: 5, distance: 300, id: 'slow' });
  const conflicts = new ConflictDetector(graph).detect(simulator.aircraft, 80);
  assert.equal(conflicts[0].type, 'overtaking'); assert.equal(conflicts[0].timeToConflict, 51);
});

test('held aircraft occupy their current edge and create a future blockage', () => {
  const graph = straight(), simulator = new AircraftSimulator(graph), route = simulator.planner.findRoute('A', 'B')!;
  simulator.addRoute(route, { speed: 10, id: 'moving' });
  const held = simulator.addRoute(route, { speed: 10, distance: 500, id: 'held' }); simulator.setHolding(held.id, true);
  const prediction = new TrafficPredictor(graph).predict([held], 80)[0];
  assert.equal(prediction.edges.length, 1); assert.equal(prediction.edges[0].velocity, 0);
  const conflict = new ConflictDetector(graph).detect(simulator.aircraft, 80)[0];
  assert.equal(conflict.type, 'blockage'); assert.equal(conflict.timeToConflict, 45.5);
});

test('nearby parallel lanes and disconnected geometric crossings do not collide', () => {
  const graph = new TaxiGraph(airport([['A', 0, 0], ['B', 1000, 0], ['C', 0, 10], ['D', 1000, 10], ['E', 500, -500], ['F', 500, 500]], [['A', 'B'], ['C', 'D'], ['E', 'F']]));
  const simulator = new AircraftSimulator(graph);
  for (const [from, to] of [['A', 'B'], ['C', 'D'], ['E', 'F']]) simulator.addRoute(simulator.planner.findRoute(from, to)!, { speed: 10 });
  assert.equal(new ConflictDetector(graph).detect(simulator.aircraft, 100).length, 0);
});

test('future destination occupants remain blockers after arrival', () => {
  const graph = straight(), simulator = new AircraftSimulator(graph), route = simulator.planner.findRoute('A', 'B')!;
  const parked = simulator.addRoute(route, { speed: 10, distance: 1000 });
  simulator.addRoute(route, { speed: 10, distance: 700 });
  assert.equal(parked.state, 'arrived');
  const conflict = new ConflictDetector(graph).detect(simulator.aircraft, 60)[0];
  assert.equal(conflict.type, 'blockage'); assert.equal(conflict.timeToConflict, 25.5);
});

test('100-aircraft generation, motion and prediction are deterministic and bounded', () => {
  const points: [string, number, number][] = Array.from({ length: 112 }, (_, i) => [`N${i}`, i * 100, 0]);
  const links: [string, string][] = Array.from({ length: 111 }, (_, i) => [`N${i}`, `N${i + 1}`]);
  const graph = new TaxiGraph(airport(points, links)), simulator = new AircraftSimulator(graph);
  assert.equal(simulator.generate(100, 1949), 100);
  const first = simulator.aircraft.map(a => [a.originId, a.destinationId, a.speed]);
  simulator.generate(100, 1949); assert.deepEqual(simulator.aircraft.map(a => [a.originId, a.destinationId, a.speed]), first);
  const start = performance.now();
  for (let i = 0; i < 100; i++) simulator.update(0.25);
  const conflicts = new ConflictDetector(graph).detect(simulator.aircraft, 120);
  const elapsed = performance.now() - start;
  assert.ok(elapsed < 4000, `100-aircraft update/prediction took ${elapsed.toFixed(1)} ms`);
  assert.ok(conflicts.every(c => Number.isFinite(c.timeToConflict) && c.timeToConflict >= 0 && c.timeToConflict <= 120));
  for (const a of simulator.aircraft) {
    assert.ok(a.distance >= 0 && a.distance <= a.route.distance);
    assert.ok(a.edgeId && graph.edges.has(a.edgeId));
    assert.equal(a.y, 0);
  }
  console.info(`Synthetic 100-aircraft: ${elapsed.toFixed(1)} ms for 100 updates + prediction; ${conflicts.length} conflict pairs`);
});

test('LFPG real graph: topology, A* distances, 100-aircraft traversal and prediction', { skip: !existsSync(new URL('../public/data/lfpg.json', import.meta.url)) }, () => {
  const data = JSON.parse(readFileSync(new URL('../public/data/lfpg.json', import.meta.url), 'utf8')) as AirportData;
  const graph = new TaxiGraph(data), simulator = new AircraftSimulator(graph);
  const started = performance.now();
  assert.equal(simulator.generate(100, 20261112), 100, 'LFPG must support 100 routed aircraft');
  const generationMs = performance.now() - started;
  for (const a of simulator.aircraft.slice(0, 12)) assert.ok(Math.abs(dijkstra(graph, a.route.nodeIds[0], a.route.nodeIds.at(-1)!) - a.route.distance) < 1e-5);
  const stepStart = performance.now();
  for (let i = 0; i < 240; i++) {
    simulator.update(0.25);
    for (const a of simulator.aircraft) {
      assert.ok(Number.isFinite(a.x) && Number.isFinite(a.y));
      assert.ok(a.edgeId && graph.edges.has(a.edgeId));
      const from = graph.nodes.get(a.fromNodeId)!, to = graph.nodes.get(a.toNodeId)!;
      assert.ok(Math.abs(a.x - (from.x + (to.x - from.x) * a.edgeProgress)) < 1e-7);
      assert.ok(Math.abs(a.y - (from.y + (to.y - from.y) * a.edgeProgress)) < 1e-7);
    }
  }
  const motionMs = performance.now() - stepStart, predictStart = performance.now();
  const conflicts = new ConflictDetector(graph).detect(simulator.aircraft, 120), predictionMs = performance.now() - predictStart;
  assert.ok(generationMs < 10000); assert.ok(predictionMs < 4000);
  assert.ok(conflicts.every(c => Number.isFinite(c.x) && Number.isFinite(c.y) && c.timeToConflict >= 0 && c.timeToConflict <= 120));
  console.info(`LFPG: ${graph.nodes.size} nodes / ${graph.edges.size} edges; generate100 ${generationMs.toFixed(1)} ms; 240×100 motion ${motionMs.toFixed(1)} ms; prediction ${predictionMs.toFixed(1)} ms; ${conflicts.length} conflict pairs`);
});
