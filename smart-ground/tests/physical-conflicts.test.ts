import { strict as assert } from 'node:assert';
import { test } from 'node:test';
import { AircraftSimulator } from '../src/core/AircraftSimulator';
import { normalizeAircraftProfile, sameAircraftProfile } from '../src/core/AircraftProfile';
import { footprintPolygons, resolveAircraftGeometry } from '../src/core/AircraftGeometry';
import { PhysicalConflictDetector } from '../src/core/PhysicalConflictDetector';
import { SurveillanceConflictMonitor, type SurveillanceTrackQuality } from '../src/core/SurveillanceConflictMonitor';
import { TaxiGraph } from '../src/core/TaxiGraph';
import type { Aircraft, AircraftProfile, AirportData, Route } from '../src/core/types';

const A320: AircraftProfile = { typeCode: 'A320', wingspanMetres: 35.8 };
const A388: AircraftProfile = { typeCode: 'A388', wingspanMetres: 79.75 };
const TINY: AircraftProfile = { typeCode: 'TEST', wingspanMetres: 4, lengthMetres: 4 };
interface Flight { points: [number, number][]; profile?: AircraftProfile; speed?: number; distance?: number; holding?: boolean; bodyHeading?: number }
function fixture(flights: Flight[]) {
  const data: AirportData = { nodes: [], edges: [], locations: [], surfaces: [], metadata: {} }, routes: Route[] = [];
  flights.forEach((flight, i) => {
    const nodeIds = flight.points.map(([x, y], j) => { const id = `${i}:${j}`; data.nodes.push({ id, x, y, lat: 49, lon: 2 }); return id; });
    const edgeIds: string[] = [], cumulativeDistances = [0];
    flight.points.slice(1).forEach(([x, y], j) => { const id = `${i}:e${j}`, previous = flight.points[j], length = Math.hypot(x - previous[0], y - previous[1]); edgeIds.push(id); cumulativeDistances.push(cumulativeDistances.at(-1)! + length); data.edges.push({ id, from: nodeIds[j], to: nodeIds[j + 1], name: `T${i}`, kind: 'taxiway', length }); });
    routes.push({ nodeIds, edgeIds, cumulativeDistances, distance: cumulativeDistances.at(-1)! });
  });
  const graph = new TaxiGraph(data), sim = new AircraftSimulator(graph);
  const aircraft = flights.map((flight, i) => {
    const a = sim.addRoute(routes[i], { id: `AC${i}`, callsign: `TEST${i}`, speed: flight.speed ?? 8, distance: flight.distance ?? 0, aircraftProfile: flight.profile ?? A320 });
    if (flight.holding) a.state = 'holding'; if (flight.bodyHeading !== undefined) a.bodyHeading = flight.bodyHeading; return a;
  });
  return { graph, sim, aircraft };
}
function quality(aircraft: Aircraft[], sourceObservedAt: number, uncertaintyMetres = 4): SurveillanceTrackQuality[] {
  return aircraft.map(a => ({ aircraftId: a.id, sourceObservedAt, ageSeconds: 0, freshness: 'fresh', status: 'tracking', confidence: .95, uncertaintyMetres, speedUncertaintyMetresPerSecond: .5 }));
}

test('published lengths override rounded native entries, native types are available and unknown lengths stay explicit', () => {
  for (const [typeCode, lengthMetres] of [['A320', 37.57], ['A388', 72.73], ['B744', 70.67], ['B748', 76.25]] as const) {
    const geometry = resolveAircraftGeometry({ typeCode, wingspanMetres: 80 });
    assert.equal(geometry.lengthMetres, lengthMetres); assert.equal(geometry.source, 'published-dimensions');
    assert.equal(geometry.model, 'body-wing-tail-envelope'); assert.equal(geometry.boxes.length, 3);
  }
  assert.equal(resolveAircraftGeometry({ ...A388, lengthMetres: 1 }).lengthMetres, 72.73, 'A declaration cannot shrink a known airframe.');
  assert.ok(resolveAircraftGeometry({ ...A388, lengthMetres: 1 }).warnings.some(warning => warning.includes('enveloppe conservée')));
  assert.equal(resolveAircraftGeometry({ typeCode: 'A20N', wingspanMetres: 35.8 }).lengthMetres, 37.57);
  const fallback = resolveAircraftGeometry({ typeCode: 'CUSTOM', wingspanMetres: 20 });
  assert.equal(fallback.source, 'estimated-length'); assert.ok(fallback.warnings.some(w => w.includes('Longueur inconnue')));
  assert.equal(resolveAircraftGeometry({ typeCode: 'CUSTOM', wingspanMetres: 20, lengthMetres: 25 }).source, 'declared-dimensions');
  assert.deepEqual(normalizeAircraftProfile(A320), A320);
  assert.equal(normalizeAircraftProfile({ ...A320, lengthMetres: 38 }).lengthMetres, 38);
  assert.throws(() => normalizeAircraftProfile({ ...A320, lengthMetres: 0 }));
  assert.equal(sameAircraftProfile(A320, { ...A320, lengthMetres: 38 }), false);
});

test('head-on contact uses actual nose-to-tail length and separates contact from the chosen clearance margin', () => {
  const { graph, aircraft } = fixture([
    { points: [[0, 0], [1000, 0]], distance: 100, speed: 10 },
    { points: [[1000, 0], [0, 0]], distance: 100, speed: 10 },
  ]);
  const conflict = new PhysicalConflictDetector(graph, { clearanceMetres: 2 }).detect(aircraft, 60)[0];
  assert.equal(conflict.type, 'head-on');
  assert.ok(Math.abs(conflict.timeToConflict - (800 - 37.57 - 2) / 20) < 1e-8);
  assert.ok(Math.abs(conflict.envelope.timeToEnvelopeOverlap! - (800 - 37.57) / 20) < 1e-8);
  assert.equal(conflict.envelope.clearanceMetres, 2); assert.equal(conflict.envelope.estimatedDimensions, false);
});

test('continuous swept intersection catches a physical crossing between five-second reports without a shared graph node', () => {
  const { graph, aircraft } = fixture([
    { points: [[-100, 0], [100, 0]], speed: 40, profile: TINY },
    { points: [[0, -100], [0, 100]], speed: 40, profile: TINY },
  ]);
  assert.equal(graph.areConnected(aircraft[0].fromNodeId, aircraft[1].fromNodeId), false);
  const conflicts = new PhysicalConflictDetector(graph, { clearanceMetres: 0 }).detect(aircraft, 5);
  assert.equal(conflicts.length, 1); assert.equal(conflicts[0].type, 'crossing');
  assert.ok(Math.abs(conflicts[0].timeToConflict - 2.4) < 1e-8);
});

test('parallel aircraft do not inherit a circular 45m envelope and wingtip margins depend on each span', () => {
  const small = fixture([{ points: [[0, 0], [1000, 0]] }, { points: [[0, 40], [1000, 40]] }]);
  assert.deepEqual(new PhysicalConflictDetector(small.graph).detect(small.aircraft, 60), []);
  const large = fixture([{ points: [[0, 0], [1000, 0]], profile: A388 }, { points: [[0, 80], [1000, 80]], profile: A388 }]);
  const warning = new PhysicalConflictDetector(large.graph, { clearanceMetres: 2 }).detect(large.aircraft, 60)[0];
  assert.equal(warning.timeToConflict, 0); assert.equal(warning.envelope.timeToEnvelopeOverlap, null, 'A small wingtip clearance is not reported as an actual envelope overlap.');
  assert.deepEqual(new PhysicalConflictDetector(large.graph, { clearanceMetres: 0 }).detect(large.aircraft, 60), []);
});

test('body, wing and tail envelopes avoid alarms caused only by the empty corners of an overall bounding box', () => {
  const { graph, aircraft } = fixture([{ points: [[0, 0], [100, 0]], holding: true }, { points: [[30, 30], [130, 30]], holding: true }]);
  assert.deepEqual(new PhysicalConflictDetector(graph, { clearanceMetres: 0 }).detect(aircraft, 10), []);
  aircraft.forEach(a => a.aircraftProfile = { typeCode: 'CUSTOM', lengthMetres: 37.57, wingspanMetres: 35.8 });
  assert.equal(new PhysicalConflictDetector(graph, { clearanceMetres: 0 }).detect(aircraft, 10).length, 1, 'Unknown detailed shapes retain explicit conservative oriented bounds.');
});

test('pushback preserves fuselage orientation separately from the opposite direction of movement', () => {
  const { graph, aircraft } = fixture([
    { points: [[0, 0], [-100, 0]], bodyHeading: 0 },
    { points: [[18, 7], [30, 7]], holding: true, profile: { typeCode: 'TEST', lengthMetres: 1, wingspanMetres: 1 } },
  ]);
  assert.deepEqual(new PhysicalConflictDetector(graph, { clearanceMetres: 0 }).detect(aircraft, 5), []);
  const tail = footprintPolygons(aircraft[0])[2]; assert.ok(tail.every(point => point.x < 0));
  delete aircraft[0].bodyHeading;
  assert.equal(new PhysicalConflictDetector(graph, { clearanceMetres: 0 }).detect(aircraft, 0).length, 1, 'Using the motion heading as body heading would falsely put the tail in front during pushback.');
});

test('a route turn encloses the intermediate nose and wing rotation instead of sampling only its endpoints', () => {
  const { graph, aircraft } = fixture([
    { points: [[-100, 0], [0, 0], [0, 100]], speed: 10, bodyHeading: 0 },
    { points: [[13, 13], [20, 13]], holding: true, profile: { typeCode: 'TEST', lengthMetres: 1, wingspanMetres: 1 } },
  ]);
  const conflict = new PhysicalConflictDetector(graph, { clearanceMetres: 0 }).detect(aircraft, 10.1)[0];
  assert.ok(conflict); assert.equal(conflict.timeToConflict, 10);
  assert.ok(conflict.envelope.rotationErrorMetres > 0 && conflict.envelope.rotationErrorMetres < 1);
});

test('physical projections stop at uncleared checkpoints and stationary/off-network reports retain their footprint', () => {
  const { graph, aircraft } = fixture([{ points: [[-100, 0], [-50, 0], [100, 0]], speed: 10 }, { points: [[50, 0], [100, 0]], holding: true, profile: TINY }]);
  aircraft[0].route.operational = { configuration: 'west', movement: 'departure', ruleIds: [], sourceIds: [], unverifiedEdgeIds: [], advisories: [], clearanceCheckpoints: [{ id: 'hold', ruleId: 'rule', nodeId: '0:1', nodeIndex: 1, distance: 50, kind: 'runway-crossing', reason: 'Test hold.', sourceIds: ['test'] }] };
  assert.deepEqual(new PhysicalConflictDetector(graph).detect(aircraft, 60), []);
  aircraft[0].clearedClearanceIds = ['hold']; assert.equal(new PhysicalConflictDetector(graph).detect(aircraft, 60).length, 1);
  aircraft.forEach((a, i) => { a.edgeId = null; a.state = 'holding'; a.x = 100 + i * 10; a.y = 100; });
  assert.equal(new PhysicalConflictDetector(graph).detect(aircraft, 60).length, 1);
});

test('the physical surveillance option preserves geometry metadata and keeps uncertainty separate from collision radius', () => {
  const { graph, aircraft } = fixture([{ points: [[0, 0], [1000, 0]] }, { points: [[0, 40], [1000, 40]] }]);
  const monitor = new SurveillanceConflictMonitor(graph, { physicalEnvelopes: true });
  assert.deepEqual(monitor.detect(aircraft, quality(aircraft, 0, 100), 0), [], 'Large uncertainty changes timing confidence, not airframe span.');
  const headOn = fixture([{ points: [[0, 0], [1000, 0]], speed: 5 }, { points: [[100, 0], [-900, 0]], speed: 5 }]);
  const alert = new SurveillanceConflictMonitor(headOn.graph, { physicalEnvelopes: true }).detect(headOn.aircraft, quality(headOn.aircraft, 0), 0)[0];
  assert.ok(alert.envelope); assert.equal(alert.envelope.model, 'oriented-airframe-envelopes');
});

test('assignment invalidation drops only the edited aircraft alert memory', () => {
  const { graph, aircraft } = fixture([
    { points: [[0, 0], [1000, 0]], speed: 5 }, { points: [[1000, 0], [0, 0]], speed: 5 },
    { points: [[0, 200], [1000, 200]], speed: 5 }, { points: [[1000, 200], [0, 200]], speed: 5 },
  ]);
  const monitor = new SurveillanceConflictMonitor(graph, { physicalEnvelopes: true });
  assert.equal(monitor.detect(aircraft, quality(aircraft, 0), 0, 120).length, 0);
  assert.equal(monitor.detect(aircraft, quality(aircraft, 5), 5, 120).length, 2);
  monitor.invalidateAircraft('AC0');
  const retained = monitor.detect(aircraft, quality(aircraft, 5), 5, 120);
  assert.equal(retained.length, 1); assert.deepEqual(retained[0].aircraftIds, ['AC2', 'AC3']);
});

test('fresh unmatched reports occupy their actual raw footprint and produce only degraded spatial warnings', () => {
  const { graph, aircraft } = fixture([{ points: [[0, 0], [1000, 0]] }, { points: [[1000, 200], [0, 200]] }]);
  const initial = aircraft.map(a => [a.x, a.y, a.edgeId]);
  const reports = (at: number, apart = false): SurveillanceTrackQuality[] => quality(aircraft, at).map((q, i) => ({ ...q,
    confidence: .1, positionConfidence: .9, uncertaintyMetres: 26, status: 'ambiguous', observation: undefined,
    x: 100 + i * 10, y: apart && i ? 1200 : 1000,
  }));
  const monitor = new SurveillanceConflictMonitor(graph, { physicalEnvelopes: true });
  const current = monitor.detect(aircraft, reports(0), 0);
  assert.equal(current.length, 1); assert.equal(current[0].timeToConflict, 0);
  assert.equal(current[0].severity, 'warning'); assert.equal(current[0].quality, 'degraded');
  assert.equal(current[0].confidence, .1, 'Position evidence must not become high taxiway-association confidence.');
  assert.equal(current[0].x, 105); assert.equal(current[0].y, 1000, 'An old matched edge cannot snap the raw footprint back to the taxiway.');
  assert.match(current[0].reason, /hors réseau/); assert.deepEqual(aircraft.map(a => [a.x, a.y, a.edgeId]), initial);
  assert.equal(monitor.detect(aircraft, reports(5, true), 5).length, 1, 'The first independent clear report retains release hysteresis.');
  assert.equal(monitor.detect(aircraft, reports(10, true), 10).length, 0, 'Two fresh separated raw reports can clear an off-network warning.');
  const old = reports(0).map(q => ({ ...q, ageSeconds: 11, status: 'stale' as const, freshness: 'stale' as const }));
  assert.deepEqual(new SurveillanceConflictMonitor(graph, { physicalEnvelopes: true }).detect(aircraft, old, 11), []);
  const uncertain = reports(0).map(q => ({ ...q, uncertaintyMetres: 75 }));
  assert.deepEqual(new SurveillanceConflictMonitor(graph, { physicalEnvelopes: true }).detect(aircraft, uncertain, 0), [], 'An unbounded raw location cannot manufacture an immediate confident footprint.');
});
