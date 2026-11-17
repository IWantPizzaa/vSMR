import { strict as assert } from 'node:assert';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import { AircraftSimulator } from '../src/core/AircraftSimulator';
import { PriorityController } from '../src/core/PriorityController';
import { RoutePlanner } from '../src/core/RoutePlanner';
import { TaxiGraph } from '../src/core/TaxiGraph';
import type { PriorityOperatingRule } from '../src/core/OperatingRules';
import type { Aircraft, AirportData, MovementMode, Route, RouteClearanceCheckpoint } from '../src/core/types';

function fixture() {
  const points: [string, number, number][] = [['W', -200, 0], ['J', 0, 0], ['E', 200, 0], ['N', 0, 200], ['S', 0, -200], ['P', -200, 12], ['Q', 200, 12], ['C', 0, 100]];
  const nodes = points.map(([id, x, y]) => ({ id, x, y, lat: 49, lon: 2 }));
  const byId = new Map(nodes.map(n => [n.id, n]));
  const links = [['W', 'J'], ['J', 'E'], ['N', 'J'], ['J', 'S'], ['P', 'Q'], ['N', 'C'], ['C', 'J']];
  const data: AirportData = { nodes, edges: links.map(([from, to], i) => ({ id: `e${i}`, from, to, length: Math.hypot(byId.get(from)!.x - byId.get(to)!.x, byId.get(from)!.y - byId.get(to)!.y), name: `T${i}`, kind: 'taxiway' })), locations: [], surfaces: [], metadata: {} };
  const graph = new TaxiGraph(data), planner = new RoutePlanner(graph), simulator = new AircraftSimulator(graph, planner);
  planner.setOperatingRules(null, 'east');
  const rule: PriorityOperatingRule = { id: 'give-way-Q-exit', nodeId: 'J', fromEdgeId: 'e0', priorityFromEdgeIds: ['e2', 'e6'], configuration: 'east', sourceIds: ['synthetic-AIP'], reason: 'Q must yield to the mapped runway exit.' };
  function route(nodeIds: string[], edgeIds: string[], movement: MovementMode, checkpoints: RouteClearanceCheckpoint[] = []): Route {
    const cumulativeDistances = [0];
    edgeIds.forEach(id => cumulativeDistances.push(cumulativeDistances.at(-1)! + graph.edges.get(id)!.length));
    return { nodeIds, edgeIds, cumulativeDistances, distance: cumulativeDistances.at(-1)!, operational: { configuration: 'east', movement, ruleIds: [], sourceIds: [], unverifiedEdgeIds: edgeIds, clearanceCheckpoints: checkpoints, advisories: [] } };
  }
  const low = simulator.addRoute(route(['W', 'J', 'E'], ['e0', 'e1'], 'departure'), { id: 'LOW', speed: 10 });
  const high = simulator.addRoute(route(['N', 'J', 'S'], ['e2', 'e3'], 'arrival'), { id: 'HIGH', speed: 10 });
  return { graph, simulator, rule, low, high, route };
}

test('published give-way uses exact branches and returns a stop on the actual low-priority route', () => {
  const { graph, rule, low, high } = fixture();
  const controller = new PriorityController(graph, [rule]);
  const decisions = controller.evaluate([low, high], 'east');
  assert.deepEqual(decisions, [{ aircraftId: 'LOW', priorityAircraftId: 'HIGH', ruleId: rule.id, nodeId: 'J', stopDistance: 155, reason: rule.reason, sourceIds: ['synthetic-AIP'] }]);
  assert.deepEqual(controller.diagnostics, []);
  assert.equal(controller.evaluate([low, high], 'west').length, 0);
});

test('priority ignores unrelated parallel geometry, wrong incoming branch and departure traffic', () => {
  const { graph, simulator, rule, low, high, route } = fixture();
  const controller = new PriorityController(graph, [rule]);
  const parallel = simulator.addRoute(route(['P', 'Q'], ['e4'], 'arrival'), { id: 'PARALLEL', speed: 10 });
  assert.deepEqual(controller.evaluate([low, parallel], 'east'), []);
  const differentBranch = simulator.addRoute(route(['E', 'J', 'S'], ['e1', 'e3'], 'arrival'), { id: 'OTHER', speed: 10 });
  assert.deepEqual(controller.evaluate([low, differentBranch], 'east'), []);
  high.route.operational!.movement = 'departure';
  assert.deepEqual(controller.evaluate([low, high], 'east'), []);
});

test('temporal occupancy avoids a stop when the two crossing times are safely separated', () => {
  const { graph, rule, low, high } = fixture();
  const controller = new PriorityController(graph, [rule]);
  high.speed = 2; // 100 s until the node; LOW clears near 20 s.
  assert.deepEqual(controller.evaluate([low, high], 'east'), []);
});

test('manual holds do not resume or claim future priority; automatic yield is re-evaluated prospectively', () => {
  const { graph, rule, low, high } = fixture();
  const controller = new PriorityController(graph, [rule]);
  low.state = 'holding';
  assert.deepEqual(controller.evaluate([low, high], 'east'), []);
  low.yielding = { ruleId: rule.id, priorityAircraftId: high.id, nodeId: 'J', reason: rule.reason, sourceIds: rule.sourceIds };
  assert.equal(controller.evaluate([low, high], 'east').length, 1);
  assert.equal(low.state, 'holding', 'The pure controller must never mutate a manual or automatic hold.');
  high.state = 'holding';
  assert.deepEqual(controller.evaluate([low, high], 'east'), []);
  assert.equal(low.state, 'holding', 'Simulator integration owns the decision to resume an automatic yield.');
});

test('a priority arrival behind an uncleared runway crossing cannot reserve a downstream junction', () => {
  const { graph, simulator, rule, low, route } = fixture();
  const controller = new PriorityController(graph, [rule]);
  const checkpoint: RouteClearanceCheckpoint = { id: 'cross-C', ruleId: 'cross-C', nodeId: 'C', nodeIndex: 1, distance: 100, kind: 'runway-crossing', sourceIds: ['synthetic-AIP'], reason: 'Explicit runway clearance required.' };
  const blocked = simulator.addRoute(route(['N', 'C', 'J', 'S'], ['e5', 'e6', 'e3'], 'arrival', [checkpoint]), { id: 'BLOCKED', speed: 10 });
  assert.deepEqual(controller.evaluate([low, blocked], 'east'), []);
  blocked.clearedClearanceIds = ['cross-C'];
  assert.equal(controller.evaluate([low, blocked], 'east').length, 1);
  blocked.state = 'holding'; blocked.waitingClearanceId = 'cross-C';
  assert.deepEqual(controller.evaluate([low, blocked], 'east'), []);
});

test('controller never rewinds an aircraft already closer than the stop buffer and does not affect passed nodes', () => {
  const { graph, rule, low, high } = fixture();
  const controller = new PriorityController(graph, [rule]);
  low.distance = 190; high.distance = 190;
  assert.equal(controller.evaluate([low, high], 'east')[0].stopDistance, 190);
  low.distance = 210;
  assert.deepEqual(controller.evaluate([low, high], 'east'), []);
});

test('unproven or malformed priority mapping remains advisory without inventing branches', () => {
  const { graph, rule, low, high } = fixture();
  const advisory = { ...rule }; delete advisory.priorityFromEdgeIds;
  const controller = new PriorityController(graph, [advisory, { ...rule, id: 'bad-edge', priorityFromEdgeIds: ['unknown'] }]);
  assert.equal(controller.diagnostics.length, 2);
  assert.deepEqual(controller.evaluate([low, high], 'east'), []);
  assert.throws(() => new PriorityController(graph, [rule], { stopBuffer: -1 }));
});

test('real LFPG right-of-way rules map to source-backed incident branches', () => {
  const data = JSON.parse(readFileSync(new URL('../public/data/lfpg.json', import.meta.url), 'utf8')) as AirportData;
  const manifest = JSON.parse(readFileSync(new URL('../public/data/lfpg-operations.json', import.meta.url), 'utf8')) as { priorityRules: PriorityOperatingRule[] };
  const controller = new PriorityController(new TaxiGraph(data), manifest.priorityRules);
  assert.ok(manifest.priorityRules.length >= 12);
  assert.deepEqual(controller.diagnostics, []);
  assert.ok(manifest.priorityRules.every(rule => Array.isArray(rule.priorityFromEdgeIds) && rule.priorityFromEdgeIds.length > 0 && rule.sourceIds.some(id => id.startsWith('sia-lfpg-'))));
});
