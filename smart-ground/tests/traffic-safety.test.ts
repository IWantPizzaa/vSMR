import { strict as assert } from 'node:assert';
import { test } from 'node:test';
import { AircraftSimulator } from '../src/core/AircraftSimulator';
import { ConflictDetector } from '../src/core/ConflictDetector';
import { PriorityController } from '../src/core/PriorityController';
import { TaxiGraph } from '../src/core/TaxiGraph';
import { TrafficPredictor } from '../src/core/TrafficPredictor';
import type { PriorityOperatingRule } from '../src/core/OperatingRules';
import type { AirportData, MovementMode, Route } from '../src/core/types';

function fixture() {
  const points: [string, number, number][] = [['W', -200, 0], ['J', 0, 0], ['E', 200, 0], ['N', 0, 200], ['S', 0, -200]];
  const nodes = points.map(([id, x, y]) => ({ id, x, y, lat: 49, lon: 2 }));
  const links = [['W', 'J'], ['J', 'E'], ['N', 'J'], ['J', 'S']];
  const data: AirportData = { nodes, edges: links.map(([from, to], i) => ({ id: `e${i}`, from, to, length: 200, name: `T${i}`, kind: 'taxiway' })), locations: [], surfaces: [], metadata: {} };
  const graph = new TaxiGraph(data), simulator = new AircraftSimulator(graph);
  simulator.planner.setOperatingRules(null, 'east');
  function route(nodeIds: string[], edgeIds: string[], movement: MovementMode): Route {
    return { nodeIds, edgeIds, cumulativeDistances: [0, 200, 400], distance: 400, operational: { configuration: 'east', movement, ruleIds: [], sourceIds: [], unverifiedEdgeIds: edgeIds, clearanceCheckpoints: [], advisories: [] } };
  }
  const low = simulator.addRoute(route(['W', 'J', 'E'], ['e0', 'e1'], 'departure'), { id: 'LOW', speed: 10 });
  const high = simulator.addRoute(route(['N', 'J', 'S'], ['e2', 'e3'], 'arrival'), { id: 'HIGH', speed: 10 });
  const rule: PriorityOperatingRule = { id: 'give-way', nodeId: 'J', fromEdgeId: 'e0', priorityFromEdgeIds: ['e2'], configuration: 'east', sourceIds: ['test-source'], reason: 'Mapped give-way.' };
  const yielding = { ruleId: rule.id, priorityAircraftId: high.id, nodeId: 'J', reason: rule.reason, sourceIds: rule.sourceIds };
  return { graph, low, high, rule, yielding };
}

test('scheduled traffic neither occupies its future origin nor claims priority before activation', () => {
  const { graph, low, high, rule, yielding } = fixture();
  assert.ok(new ConflictDetector(graph).detect([low, high], 40).length);
  assert.equal(new PriorityController(graph, [rule]).evaluate([low, high], 'east').length, 1);
  high.state = 'scheduled'; high.scheduledAt = 60; high.yielding = yielding;
  assert.deepEqual(new TrafficPredictor(graph).predict([low, high], 120).map(t => t.aircraft.id), ['LOW']);
  assert.deepEqual(new ConflictDetector(graph).detect([low, high], 120), []);
  assert.deepEqual(new PriorityController(graph, [rule]).evaluate([low, high], 'east'), []);
  high.state = 'taxiing'; low.state = 'scheduled'; low.scheduledAt = 60; low.yielding = yielding;
  assert.deepEqual(new PriorityController(graph, [rule]).evaluate([low, high], 'east'), [], 'A stale yielding flag must not activate pending traffic prospectively.');
});

test('unresolved off-route traffic blocks its observed edge without projecting the obsolete assigned route', () => {
  const { graph, low, high, rule, yielding } = fixture();
  // LOW was assigned W-J-E but is actually halfway down the N-J branch.
  low.edgeId = 'e2'; low.fromNodeId = 'N'; low.toNodeId = 'J'; low.edgeProgress = .5;
  low.x = 0; low.y = 100; low.distance = 100;
  low.conformance = { status: 'unresolved', events: [], assignedNodeIndex: 0, awaitingApproval: true };
  low.state = 'taxiing'; low.yielding = yielding; // Defensive against stale state.
  const prediction = new TrafficPredictor(graph).predict([low], 40)[0];
  assert.deepEqual(prediction.edges.map(edge => edge.edgeId), ['e2']);
  assert.ok(prediction.edges.every(edge => edge.stationary && edge.velocity === 0));
  assert.equal(prediction.edges[0].offset, 100);
  const conflicts = new ConflictDetector(graph).detect([low, high], 40);
  assert.equal(conflicts[0].type, 'blockage');
  assert.equal(conflicts[0].location, 'T2');
  assert.equal(conflicts[0].timeToConflict, 5.5);
  assert.deepEqual(new PriorityController(graph, [rule]).evaluate([low, high], 'east'), []);
});

test('a calculated recovery awaiting approval stays stationary and cannot be released by priority', () => {
  const { graph, low, high, rule, yielding } = fixture();
  low.conformance = { status: 'recovering', events: [], assignedNodeIndex: 0, awaitingApproval: true };
  low.state = 'holding'; low.yielding = yielding;
  assert.deepEqual(new PriorityController(graph, [rule]).evaluate([low, high], 'east'), []);
  assert.ok(new TrafficPredictor(graph).predict([low], 40)[0].edges.every(edge => edge.stationary));
  low.state = 'taxiing';
  assert.deepEqual(new PriorityController(graph, [rule]).evaluate([low, high], 'east'), []);
  assert.ok(new TrafficPredictor(graph).predict([low], 40)[0].edges.every(edge => edge.stationary));
  low.conformance.awaitingApproval = false; low.state = 'holding';
  assert.equal(new PriorityController(graph, [rule]).evaluate([low, high], 'east').length, 1, 'After approval, an ordinary automatic yield can be evaluated and resumed again.');
});
