import { strict as assert } from 'node:assert';
import { test } from 'node:test';
import { AircraftSimulator, SurfaceSurveillance, TaxiGraph } from '../src/core';
import { EuroScopeSurfaceAdapter } from '../src/adapters/EuroScopeSurfaceAdapter';
import type { AirportData, Route } from '../src/core/types';

function fixture(onConfirmed?: ConstructorParameters<typeof SurfaceSurveillance>[1]) {
  const coords: [string, number, number][] = [['S', 0, 0], ['A', 100, 0], ['B', 200, 0], ['C', 300, 0], ['U', 100, 100], ['V', 200, 100]];
  const nodes = coords.map(([id, x, y]) => ({ id, x, y, lat: 49, lon: 2 })), byId = new Map(nodes.map(n => [n.id, n]));
  const edges = [['SA', 'S', 'A'], ['AB', 'A', 'B'], ['BC', 'B', 'C'], ['AU', 'A', 'U'], ['UV', 'U', 'V'], ['VB', 'V', 'B']].map(([id, from, to]) => ({ id, from, to, name: id, kind: 'taxiway' as const, length: Math.hypot(byId.get(to)!.x - byId.get(from)!.x, byId.get(to)!.y - byId.get(from)!.y) }));
  const data: AirportData = { nodes, edges, locations: [], surfaces: [], metadata: {} }, graph = new TaxiGraph(data), simulator = new AircraftSimulator(graph);
  const route: Route = { nodeIds: ['S', 'A', 'B', 'C'], edgeIds: ['SA', 'AB', 'BC'], cumulativeDistances: [0, 100, 200, 300], distance: 300 };
  const aircraft = simulator.addRoute(route, { id: 'AFR123', callsign: 'AFR123', speed: 5, distance: 50 });
  const surveillance = new SurfaceSurveillance(graph, { simulatedNoiseMetres: 0, ...onConfirmed });
  return { graph, simulator, aircraft, surveillance };
}

test('rendered frames produce estimates without inventing extra five-second radar reports', () => {
  const { simulator, aircraft, surveillance } = fixture();
  surveillance.captureSimulation(simulator.aircraft, 0);
  for (let i = 0; i < 19; i++) { simulator.update(.25); surveillance.captureSimulation(simulator.aircraft, simulator.time); }
  assert.equal(surveillance.estimate(aircraft.id, 4.75)?.acceptedSamples, 1);
  assert.equal(surveillance.estimate(aircraft.id, 4.75)?.sourceObservedAt, 0);
  assert.ok(surveillance.estimate(aircraft.id, 4.75)!.x > 70);
  simulator.update(.25); surveillance.captureSimulation(simulator.aircraft, 5);
  assert.equal(surveillance.estimate(aircraft.id, 5)?.acceptedSamples, 2);
  assert.equal(surveillance.estimate(aircraft.id, 5)?.evidenceCount, 0);
  simulator.clear(); const next = simulator.addRoute(aircraft.assignedRoute!, { id: aircraft.id, callsign: aircraft.callsign, speed: 5 });
  surveillance.captureSimulation(simulator.aircraft, 0);
  assert.equal(surveillance.estimate(next.id, 0)?.acceptedSamples, 1, 'A new simulation generation cannot inherit old report timestamps.');
});

test('raw SDK adapter reports confirm one incident, not one event per frame or duplicate callback', () => {
  let notifications = 0;
  const { aircraft, surveillance } = fixture({ onConfirmedDeviation: () => { notifications++; } });
  const adapter = new EuroScopeSurfaceAdapter(); adapter.beginSession(aircraft.id, 'connection-1');
  const feed = (sequence: number, receivedAt: number, x: number, y: number, headingTrueDegrees: number) => {
    const result = adapter.adapt({ aircraftId: aircraft.id, sessionId: 'connection-1', sequence, receivedAt,
      latitude: 49.0097 + y / 111320, longitude: 2.5479 + x / (111320 * Math.cos(49.0097 * Math.PI / 180)),
      reportedGroundSpeedKnots: 10, headingTrueDegrees,
    }, receivedAt);
    if (result.accepted) surveillance.ingest(aircraft, result.sample, receivedAt);
    return result;
  };
  assert.equal(feed(1, 0, 70, 0, 90).accepted, true);
  feed(2, 5, 100, 22, 0); assert.equal(notifications, 0);
  for (let i = 0; i < 40; i++) surveillance.estimate(aircraft.id, 5 + i * .1);
  assert.equal(feed(2, 5, 100, 22, 0).accepted, false); assert.equal(notifications, 0);
  feed(3, 10, 100, 48, 0); assert.equal(notifications, 1);
  feed(4, 15, 100, 72, 0); assert.equal(notifications, 1, 'A continued deviation is one incident.');
});

test('live measured motion is independent of a planned hold and stale reports stop receiving confident forecasts', () => {
  const { aircraft, surveillance } = fixture(); aircraft.state = 'holding';
  surveillance.ingest(aircraft, { aircraftId: aircraft.id, observedAt: 0, x: 50, y: 0, groundSpeedKnots: 10, headingTrueDegrees: 90 }, 0);
  const prediction = surveillance.predictionFleet([aircraft], 2);
  assert.equal(prediction.aircraft[0].state, 'taxiing'); assert.ok(prediction.aircraft[0].speed > 5);
  assert.equal(aircraft.state, 'holding', 'Safety forecasting cannot change a planned hold or issue a control command.');
  assert.equal(surveillance.estimate(aircraft.id, 11)?.freshness, 'stale');
  assert.equal(surveillance.estimate(aircraft.id, 21)?.freshness, 'lost');
  assert.deepEqual(surveillance.detect([aircraft], 21), []);
});

test('only a confirmed raw deviation reaches the existing recovery planner', () => {
  const { graph, simulator, aircraft } = fixture();
  const surveillance = new SurfaceSurveillance(graph, { onConfirmedDeviation: (target, observation) => simulator.observeAircraft(target.id, observation) });
  const sample = (observedAt: number, x: number, y: number, headingTrueDegrees: number) => ({ aircraftId: aircraft.id, observedAt, x, y, headingTrueDegrees, groundSpeedKnots: 10 });
  surveillance.ingest(aircraft, sample(0, 70, 0, 90), 0);
  surveillance.ingest(aircraft, sample(5, 100, 22, 0), 5);
  assert.equal(aircraft.conformance?.events.length, 0);
  surveillance.ingest(aircraft, sample(10, 100, 48, 0), 10);
  assert.equal(aircraft.conformance?.events.length, 1); assert.equal(aircraft.conformance?.status, 'recovering');
  assert.equal(aircraft.state, 'holding'); assert.equal(aircraft.conformance?.awaitingApproval, true);
  assert.equal(aircraft.assignedRoute?.edgeIds.includes('AU'), false);
  assert.equal(aircraft.route.edgeIds.includes('AU'), true);
});

test('changing an assigned route preserves sensor receipt time instead of inventing a new report', () => {
  const { simulator, aircraft, surveillance } = fixture();
  surveillance.captureSimulation([aircraft], 0);
  const before = surveillance.estimate(aircraft.id, 3)!;
  const result = simulator.editAssignedRoute(aircraft.id, { anchorNodeIndex: 1, targetNodeId: 'U' });
  assert.equal(result.ok, true);
  surveillance.assignmentChanged(aircraft);
  const after = surveillance.estimate(aircraft.id, 3)!;
  assert.equal(after.sourceObservedAt, before.sourceObservedAt); assert.equal(after.acceptedSamples, before.acceptedSamples);
  assert.equal(after.ageSeconds, 3); assert.equal(after.evidenceCount, 0);
  assert.equal(after.status, 'tracking');
});

test('stopped body orientation survives the radar pipeline independently of assigned movement direction', () => {
  const { aircraft, surveillance } = fixture(); aircraft.state = 'holding';
  surveillance.ingest(aircraft, { aircraftId: aircraft.id, observedAt: 0, x: 50, y: 0, groundSpeedKnots: 0, headingTrueDegrees: 0 }, 0);
  const fleet = surveillance.predictionFleet([aircraft], 3);
  assert.equal(fleet.aircraft[0].heading, 0, 'The assigned taxiway runs east.');
  assert.equal(fleet.aircraft[0].bodyHeading, Math.PI / 2, 'The measured airframe is oriented north at a stop.');
});

test('an unmatched report occupies its raw position rather than an obsolete assigned graph edge', () => {
  const { aircraft, surveillance } = fixture();
  const result = surveillance.ingest(aircraft, { aircraftId: aircraft.id, observedAt: 0, x: 1000, y: 1000, groundSpeedKnots: 0, headingTrueDegrees: 90 }, 0);
  assert.equal(result.observation, undefined); assert.ok(result.confidence < .25);
  assert.ok(result.positionConfidence! > .8, 'Poor lane association must not erase a fresh bounded position report.');
  const fleet = surveillance.predictionFleet([aircraft], 0);
  assert.equal(fleet.aircraft[0].edgeId, null); assert.equal(fleet.aircraft[0].x, 1000); assert.equal(fleet.aircraft[0].y, 1000);
  assert.equal(aircraft.edgeId, 'SA'); assert.equal(aircraft.x, 50);
});
