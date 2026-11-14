import { strict as assert } from 'node:assert';
import { test } from 'node:test';
import { SurfaceTracker, type SurfaceSample } from '../src/core/SurfaceTracker';
import { TaxiGraph } from '../src/core/TaxiGraph';
import type { AirportData, Route } from '../src/core/types';

function fixture() {
  const coordinates: [string, number, number][] = [['S', 0, 0], ['A', 100, 0], ['B', 200, 0], ['C', 300, 0], ['U', 100, 100], ['V', 200, 100], ['P', 0, 12], ['Q', 200, 12]];
  const nodes = coordinates.map(([id, x, y]) => ({ id, x, y, lat: 49, lon: 2 })), byId = new Map(nodes.map(node => [node.id, node]));
  const pairs = [['SA', 'S', 'A'], ['AB', 'A', 'B'], ['BC', 'B', 'C'], ['AU', 'A', 'U'], ['UV', 'U', 'V'], ['VB', 'V', 'B'], ['PQ', 'P', 'Q']];
  const data: AirportData = { nodes, edges: pairs.map(([id, from, to]) => ({ id, from, to, name: id, kind: 'taxiway', length: Math.hypot(byId.get(to)!.x - byId.get(from)!.x, byId.get(to)!.y - byId.get(from)!.y) })), locations: [], surfaces: [], metadata: {} };
  const graph = new TaxiGraph(data), tracker = new SurfaceTracker(graph);
  function route(nodeIds = ['S', 'A', 'B', 'C'], edgeIds = ['SA', 'AB', 'BC']): Route {
    const cumulativeDistances = [0]; for (const id of edgeIds) cumulativeDistances.push(cumulativeDistances.at(-1)! + graph.edges.get(id)!.length);
    return { nodeIds, edgeIds, cumulativeDistances, distance: cumulativeDistances.at(-1)! };
  }
  const sample = (observedAt: number, x: number, y = 0, headingTrueDegrees = 90, groundSpeedKnots = 10): SurfaceSample => ({ aircraftId: 'AFR123', observedAt, x, y, headingTrueDegrees, groundSpeedKnots });
  return { graph, tracker, route, sample };
}

test('five-second noisy reports remain on their assigned route without frame-based evidence', () => {
  const { tracker, route, sample } = fixture(), assignment = route();
  let state = 42;
  for (let i = 0; i < 18; i++) {
    state = Math.imul(state, 1664525) + 1013904223 | 0;
    const noise = ((state >>> 0) / 4294967296 - .5) * 4;
    const report = tracker.ingest(sample(i * 5, 10 + i * 12, noise, 90 + noise, 4.7), assignment);
    assert.notEqual(report.status, 'confirmed-off-track'); assert.equal(report.evidenceCount, 0);
    for (let frame = 1; frame < 20; frame++) {
      const estimate = tracker.estimate('AFR123', i * 5 + frame * .25)!;
      assert.equal(estimate.sourceObservedAt, i * 5); assert.equal(estimate.evidenceCount, 0); assert.equal(estimate.confirmedObservation, undefined);
    }
  }
});

test('a sparse report may skip several source segments through a normal turn', () => {
  const { tracker, route, sample } = fixture(), assignment = route(['S', 'A', 'U', 'V'], ['SA', 'AU', 'UV']);
  tracker.ingest(sample(0, 85, 0), assignment);
  const report = tracker.ingest(sample(5, 100, 40, 0), assignment);
  assert.equal(report.status, 'tracking'); assert.equal(report.evidenceCount, 0);
  assert.equal(report.observation?.edgeId, 'AU');
  const estimate = tracker.estimate('AFR123', 9)!;
  assert.equal(estimate.x, 100); assert.ok(estimate.y > 40); assert.ok(estimate.y < 100);
});

test('a sustained wrong branch needs two independent clear reports five seconds apart', () => {
  const { tracker, route, sample } = fixture(), assignment = route();
  tracker.ingest(sample(0, 70), assignment);
  const first = tracker.ingest(sample(5, 100, 22, 0), assignment);
  assert.equal(first.status, 'suspected-off-track'); assert.equal(first.evidenceCount, 1); assert.equal(first.confirmedObservation, undefined);
  for (let i = 1; i < 100; i++) assert.equal(tracker.estimate('AFR123', 5 + i * .04)!.evidenceCount, 1);
  const duplicate = tracker.ingest(sample(5, 100, 23, 0), assignment, 6);
  assert.equal(duplicate.accepted, false); assert.equal(duplicate.evidenceCount, 1);
  const confirmed = tracker.ingest(sample(10, 100, 48, 0), assignment);
  assert.equal(confirmed.status, 'confirmed-off-track'); assert.equal(confirmed.deviationKind, 'wrong-branch');
  assert.equal(confirmed.confirmedObservation?.edgeId, 'AU'); assert.equal(confirmed.evidenceCount, 2);
  assert.equal(assignment.edgeIds.includes('AU'), false, 'Matching must not rewrite the assignment.');
});

test('moving backward along an assigned source way confirms a wrong direction', () => {
  const { tracker, route, sample } = fixture(), assignment = route();
  tracker.ingest(sample(0, 180), assignment);
  assert.equal(tracker.ingest(sample(5, 150, 0, 270), assignment).status, 'suspected-off-track');
  const report = tracker.ingest(sample(10, 120, 0, 270), assignment);
  assert.equal(report.status, 'confirmed-off-track'); assert.equal(report.deviationKind, 'wrong-direction');
  assert.equal(report.confirmedObservation?.fromNodeId, 'B'); assert.equal(report.confirmedObservation?.toNodeId, 'A');
});

test('body-heading reversals at a stop and pushback do not independently prove wrong-way motion', () => {
  const { tracker, route, sample } = fixture(), assignment = route();
  tracker.ingest(sample(0, 150, 0, 90, 0), assignment);
  for (let i = 1; i <= 5; i++) {
    const report = tracker.ingest(sample(i * 5, 150 + i % 2, i % 2, i % 2 ? 270 : 0, 0), assignment);
    assert.equal(report.evidenceCount, 0); assert.notEqual(report.status, 'confirmed-off-track');
    assert.equal(report.speedMetresPerSecond, 0);
  }
  const pushback = new SurfaceTracker(fixture().graph);
  pushback.ingest(sample(0, 20, 0, 270, 6), assignment);
  const moving = pushback.ingest(sample(5, 38, 0, 270, 6), assignment);
  assert.notEqual(moving.status, 'confirmed-off-track', 'Geometric displacement wins over opposite body heading.');
});

test('near-parallel tracks and a junction remain ambiguous until the physical branch is established', () => {
  const { tracker, route, sample } = fixture(), assignment = route();
  const between = tracker.ingest(sample(0, 50, 6), assignment);
  assert.equal(between.status, 'ambiguous'); assert.ok(between.confidence < .6);
  const junction = tracker.ingest(sample(5, 100, 3, 0), assignment);
  assert.notEqual(junction.status, 'confirmed-off-track'); assert.equal(junction.evidenceCount, 0);
});

test('ambiguous and suspected estimates keep the last measured footprint instead of committing a branch', () => {
  const { tracker, route, sample } = fixture(), assignment = route();
  const ambiguous = tracker.ingest(sample(0, 50, 6), assignment);
  assert.equal(ambiguous.status, 'ambiguous');
  for (const time of [1, 3, 5]) {
    const estimate = tracker.estimate('AFR123', time)!;
    assert.equal(estimate.x, ambiguous.x); assert.equal(estimate.y, ambiguous.y);
    assert.deepEqual(estimate.observation, ambiguous.observation); assert.equal(estimate.evidenceCount, 0);
  }
  tracker.ingest(sample(5, 70), assignment);
  const suspect = tracker.ingest(sample(10, 100, 22, 0), assignment);
  assert.equal(suspect.status, 'suspected-off-track'); assert.equal(suspect.evidenceCount, 1);
  for (const time of [11, 13, 15]) {
    const estimate = tracker.estimate('AFR123', time)!;
    assert.equal(estimate.x, suspect.x); assert.equal(estimate.y, suspect.y);
    assert.deepEqual(estimate.observation, suspect.observation); assert.equal(estimate.evidenceCount, 1);
    assert.equal(estimate.confirmedObservation, undefined);
  }
});

test('short contiguous vertices of a curved wrong branch are not confused with parallel lanes', () => {
  const { graph, route, sample } = fixture();
  const nodes = graph.data.nodes.filter(node => node.id !== 'U'), edges = graph.data.edges.filter(edge => !['AU', 'UV', 'VB'].includes(edge.id));
  let previous = 'A';
  for (let i = 1; i <= 50; i++) {
    const id = `short-${i}`, point = { id, x: 100 + Math.sin(i / 6) * .2, y: i * 2, lat: 49, lon: 2 };
    const from = nodes.find(node => node.id === previous)!; nodes.push(point);
    edges.push({ id: `curve-${i}`, from: previous, to: id, name: 'wrong curve', kind: 'taxiway', length: Math.hypot(point.x - from.x, point.y - from.y) }); previous = id;
  }
  const tracker = new SurfaceTracker(new TaxiGraph({ ...graph.data, nodes, edges })), assignment = route();
  tracker.ingest(sample(0, 70), assignment);
  const first = tracker.ingest(sample(5, 100, 21.2, 0), assignment);
  const second = tracker.ingest(sample(10, 100, 47.1, 0), assignment);
  assert.equal(first.status, 'suspected-off-track'); assert.equal(second.status, 'confirmed-off-track');
  assert.match(second.confirmedObservation?.edgeId ?? '', /^curve-/);
});

test('coasting is bounded and stale tracks freeze rather than travelling indefinitely', () => {
  const { tracker, route, sample } = fixture(); tracker.ingest(sample(0, 20), route());
  assert.equal(tracker.estimate('AFR123', 8)!.freshness, 'coasting');
  const lastMotion = tracker.estimate('AFR123', 10)!, stale = tracker.estimate('AFR123', 15)!, lost = tracker.estimate('AFR123', 25)!;
  assert.equal(stale.freshness, 'stale'); assert.equal(lost.freshness, 'lost');
  assert.equal(stale.x, lastMotion.x); assert.equal(lost.x, lastMotion.x); assert.equal(stale.speedMetresPerSecond, 0);
  assert.ok(lost.uncertaintyMetres > lastMotion.uncertaintyMetres); assert.ok(lost.confidence < stale.confidence);
});

test('extrapolation stops before an uncleared runway checkpoint but actual crossing evidence is retained', () => {
  const { tracker, route, sample } = fixture(), assignment = route();
  assignment.operational = { configuration: 'east', movement: 'departure', ruleIds: [], sourceIds: [], unverifiedEdgeIds: [], advisories: [], clearanceCheckpoints: [{ id: 'hold-A', ruleId: 'A', nodeId: 'A', nodeIndex: 1, distance: 100, kind: 'runway-crossing', reason: 'Manual crossing', sourceIds: [] }] };
  tracker.ingest(sample(0, 80), assignment);
  assert.equal(tracker.estimate('AFR123', 10)!.x, 100);
  assert.equal(tracker.ingest(sample(5, 125), assignment).status, 'suspected-off-track');
  const actual = tracker.ingest(sample(10, 150), assignment);
  assert.equal(actual.deviationKind, 'runway-hold'); assert.equal(actual.status, 'confirmed-off-track');
  assert.ok(actual.x >= 145, 'The actual report is not snapped backward to the hold.');
  assert.equal(actual.confirmedObservation?.edgeId, 'AB');
});

test('duplicates, reordered packets, future epochs and jumps cannot freshen a track or add votes', () => {
  const { tracker, route, sample } = fixture(), assignment = route(); tracker.ingest(sample(5, 20), assignment);
  assert.equal(tracker.ingest(sample(5, 22), assignment, 6).rejectionReason, 'duplicate-or-out-of-order');
  assert.equal(tracker.ingest(sample(4, 22), assignment, 6).accepted, false);
  assert.equal(tracker.ingest(sample(20, 25), assignment, 10).rejectionReason, 'future-observation');
  assert.equal(tracker.ingest(sample(10, 5000), assignment).rejectionReason, 'implausible-position-jump');
  const estimate = tracker.estimate('AFR123', 11)!; assert.equal(estimate.sourceObservedAt, 5); assert.equal(estimate.acceptedSamples, 1);
  assert.throws(() => tracker.ingest({ ...sample(15, 30), headingTrueDegrees: NaN }, assignment), /Invalid/);
});

test('a long report gap resets corroboration, while a normal delayed report can still rejoin the route', () => {
  const { tracker, route, sample } = fixture(), assignment = route();
  tracker.ingest(sample(0, 100, 20, 0), assignment);
  const afterLoss = tracker.ingest(sample(20, 100, 70, 0), assignment);
  assert.notEqual(afterLoss.status, 'confirmed-off-track'); assert.equal(afterLoss.evidenceCount, 1);
  const normal = tracker.ingest(sample(40, 200, 0, 90), assignment);
  assert.notEqual(normal.status, 'confirmed-off-track'); assert.equal(normal.evidenceCount, 0);
});
