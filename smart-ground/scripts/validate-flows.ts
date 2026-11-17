/** Offline validation of the actual main-axis routing mode used by the UI. */
import { strict as assert } from 'node:assert';
import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync } from 'node:fs';
import { performance } from 'node:perf_hooks';
import { AircraftSimulator, ConflictDetector, OperatingRules, RoutePlanner, TaxiFlowPolicy, TaxiGraph, TrafficPredictor } from '../src/core';
import type { AirportData, MovementMode, OperatingConfiguration, Route } from '../src/core/types';
import type { OperatingManifest } from '../src/core/OperatingRules';
import type { TaxiFlowManifest } from '../src/core/TaxiFlowPolicy';
import { resolveOperatingManifest } from '../src/core/resolveOperatingManifest';
import { AircraftRestrictions, type AircraftRestrictionManifest } from '../src/core/AircraftRestrictions';
import { DEFAULT_AIRCRAFT_PROFILE } from '../src/core/AircraftProfile';

const root = new URL('../', import.meta.url), started = performance.now();
const input = (name: string) => readFileSync(new URL(name, root));
const hash = (bytes: Buffer) => createHash('sha256').update(bytes).digest('hex');
const dataBytes = input('public/data/lfpg.json'), operationBytes = input('public/data/lfpg-operations.json'), corridorBytes = input('public/data/lfpg-corridors.json');
const aircraftBytes = input('public/data/lfpg-aircraft-restrictions.json');
const data = JSON.parse(dataBytes.toString('utf8')) as AirportData;
const operations = resolveOperatingManifest(JSON.parse(operationBytes.toString('utf8')) as OperatingManifest);
const manifest = JSON.parse(corridorBytes.toString('utf8')) as TaxiFlowManifest;
assert.equal(manifest.dataSha256, hash(dataBytes), 'The corridor catalog must refer to the exact geographic snapshot.');
const graph = new TaxiGraph(data), rules = new OperatingRules(graph, operations), flow = new TaxiFlowPolicy(graph, manifest);
const aircraftRestrictions = new AircraftRestrictions(graph, JSON.parse(aircraftBytes.toString('utf8')) as AircraftRestrictionManifest);
assert.ok(aircraftRestrictions.valid, aircraftRestrictions.diagnostics.join('; '));
assert.ok(rules.valid, JSON.stringify(rules.diagnostics)); assert.ok(flow.valid, flow.diagnostics.join('; '));
const runwayAxes = new Map<string, { runway: string; dx: number; dy: number }[]>();
for (const edge of graph.edges.values()) {
  if (edge.kind !== 'runway' || !/^(08|09)[LR]\/(26|27)[LR]$/.test(edge.name)) continue;
  const from = graph.nodes.get(edge.from)!, to = graph.nodes.get(edge.to)!;
  for (const id of [edge.from, edge.to]) { const axes = runwayAxes.get(id) || []; axes.push({ runway: edge.name, dx: to.x - from.x, dy: to.y - from.y }); runwayAxes.set(id, axes); }
}
let crossings = 0, checkedRoutes = 0, checkedPositions = 0;
function verifyRoute(route: Route, planner: RoutePlanner) {
  assert.ok(planner.isRouteAllowed(route), 'Actual portal, source restrictions and direction must survive route replay.');
  assert.ok(route.flow, 'The current UI mode must actually invoke the corridor planner.');
  assert.equal(route.aircraft?.profile.typeCode, DEFAULT_AIRCRAFT_PROFILE.typeCode, 'The current UI baseline must also use its declared A320 profile.');
  assert.deepEqual(route.aircraft?.airportFlags, Array.isArray(operations.activeProfileIds) ? [...operations.activeProfileIds].sort() : []);
  assert.ok(route.edgeIds.every(id => !flow.specialAreaEdgeIds.has(id)), 'Routine traffic must stay outside deicing treatment lanes.');
  assert.ok(Math.abs(route.distance - route.edgeIds.reduce((sum, id) => sum + graph.edges.get(id)!.length, 0)) < .00001);
  let previousCrossingDistance = -Infinity;
  for (let i = 1; i < route.nodeIds.length - 1; i++) {
    const node = graph.nodes.get(route.nodeIds[i])!, before = graph.nodes.get(route.nodeIds[i - 1])!, after = graph.nodes.get(route.nodeIds[i + 1])!;
    const checked = new Set<string>();
    for (const axis of runwayAxes.get(node.id) || []) {
      if (checked.has(axis.runway)) continue; checked.add(axis.runway);
      const length = Math.hypot(axis.dx, axis.dy);
      const beforeSide = ((before.x - node.x) * axis.dy - (before.y - node.y) * axis.dx) / length;
      const afterSide = ((after.x - node.x) * axis.dy - (after.y - node.y) * axis.dx) / length;
      if (beforeSide * afterSide >= -1e-6) continue;
      const distance = route.cumulativeDistances[i];
      const protection = route.operational?.clearanceCheckpoints.find(checkpoint => {
        if (checkpoint.distance <= previousCrossingDistance || checkpoint.distance >= distance - 1e-6) return false;
        const rule = operations.clearanceRules?.find(r => r.id === checkpoint.ruleId);
        const sourceHold = typeof rule?.sourceHoldingLocationId === 'string' ? data.locations.find(l => l.id === rule.sourceHoldingLocationId) : null;
        return rule?.runway === axis.runway && sourceHold?.kind === 'holding_point' && sourceHold.nodeId === checkpoint.nodeId;
      });
      assert.ok(protection, `Unprotected runway crossing on NEW flow route: ${axis.runway} / ${node.id} / ${route.flow.originId} → ${route.flow.destinationId}`);
      crossings++; previousCrossingDistance = distance;
    }
  }
  checkedRoutes++;
}
const comparisons: Record<string, unknown>[] = [], review: Record<string, unknown>[] = [], scenarios: Record<string, unknown>[] = [];
for (const configuration of ['east', 'west'] as const) {
  const planner = new RoutePlanner(graph, rules, configuration); planner.setFlowPolicy(flow);
  planner.setAircraftRestrictions(aircraftRestrictions); planner.setAircraftProfile(DEFAULT_AIRCRAFT_PROFILE);
  planner.setAircraftRestrictionContext({ airportFlags: Array.isArray(operations.activeProfileIds) ? operations.activeProfileIds.filter((id): id is string => typeof id === 'string') : [] });
  const baseline = new RoutePlanner(graph, rules, configuration);
  const endpoints = (role: 'arrival' | 'departure') => {
    const seen = new Set<string>();
    return rules.runwayAccess(configuration, role).filter(a => !seen.has(a.runway) && !!seen.add(a.runway)).map(a => a.locationId);
  };
  const cases: { from: string; to: string; movement: MovementMode }[] = [];
  for (const stand of ['A01', 'B24', 'F02', 'J14', 'I24', 'N41']) for (const to of endpoints('departure')) cases.push({ from: `stand:${stand}`, to, movement: 'departure' });
  for (const from of endpoints('arrival')) for (const stand of ['A07', 'F02', 'J14', 'I24']) cases.push({ from, to: `stand:${stand}`, movement: 'arrival' });
  for (const [from, to] of [['B24', 'F02'], ['F02', 'B24'], ['I24', 'J14'], ['F02', 'A01']]) cases.push({ from: `stand:${from}`, to: `stand:${to}`, movement: 'transfer' });
  for (const item of cases) {
    const from = planner.locations.find(l => l.id === item.from), to = planner.locations.find(l => l.id === item.to);
    if (!from?.nodeId || !to?.nodeId || !rules.isLocationOpen(from.id) || !rules.isLocationOpen(to.id)) continue;
    const request = { movement: item.movement, originId: from.id, destinationId: to.id };
    const route = planner.findRoute(from.nodeId, to.nodeId, request), old = baseline.findRoute(from.nodeId, to.nodeId, request);
    const oldConformsDirections = !!old && old.edgeIds.every((id, i) => planner.allowArc(graph.edges.get(id)!, old.nodeIds[i], old.nodeIds[i + 1], item.movement));
    comparisons.push({ configuration, ...item, beforeMetres: old?.distance ?? null, beforeObeysCorrectedDirections: oldConformsDirections, beforeNodeIds: old?.nodeIds, beforeEdgeIds: old?.edgeIds, afterMetres: route?.distance ?? null, axes: route?.flow?.axes, apronTransitMetres: route?.flow?.apronTransitMetres, mainAxisReentries: route?.flow?.mainAxisReentries, significantTurns: route?.flow?.significantTurns, mainAxisChanges: route?.flow?.mainAxisChanges, failure: route ? undefined : planner.lastFlowFailure });
    if (route) { verifyRoute(route, planner); review.push({ configuration, movement: item.movement, from: from.id, to: to.id, fromName: from.name, toName: to.name, distance: route.distance, nodeIds: route.nodeIds, edgeIds: route.edgeIds, operational: route.operational, flow: route.flow, before: old ? { distance: old.distance, nodeIds: old.nodeIds, edgeIds: old.edgeIds, conformsCorrectedDirections: oldConformsDirections } : null }); }
  }
  for (const movement of ['departure', 'arrival', 'transfer'] as const) for (const count of [10, 25, 50, 100]) {
    const simulator = new AircraftSimulator(graph, planner), detector = new ConflictDetector(graph), startGeneration = performance.now();
    const actual = simulator.generate(count, 20261112, { movement }), generationMs = performance.now() - startGeneration;
    assert.equal(actual, count, `${configuration}/${movement}: unable to generate requested traffic.`);
    for (const aircraft of simulator.aircraft) { verifyRoute(aircraft.route, planner); assert.equal(aircraft.route.flow?.status, 'nominal', 'Automatic traffic must not silently include a review-required route.'); }
    let updateMs = 0, predictionMs = 0, maxPredictionMs = 0;
    for (let step = 0; step < 600; step++) {
      const t = performance.now(); simulator.update(.25); updateMs += performance.now() - t;
      for (const a of simulator.aircraft) {
        const from = graph.nodes.get(a.fromNodeId)!, to = graph.nodes.get(a.toNodeId)!;
        assert.ok(Math.abs(a.x - from.x - (to.x - from.x) * a.edgeProgress) < 1e-6);
        assert.ok(Math.abs(a.y - from.y - (to.y - from.y) * a.edgeProgress) < 1e-6); checkedPositions++;
        if (a.waitingClearanceId) assert.equal(a.state, 'holding');
      }
      if (step % 20 === 0) { const t = performance.now(); detector.detect(simulator.aircraft, 120); const elapsed = performance.now() - t; predictionMs += elapsed; maxPredictionMs = Math.max(maxPredictionMs, elapsed); }
    }
    // Pending crossings remain closed in prediction as well as in the actual motion.
    for (const a of simulator.aircraft.filter(a => a.waitingClearanceId)) {
      const prediction = new TrafficPredictor(graph).predict([a], 300)[0];
      assert.ok(prediction.edges.every(e => e.stationary));
    }
    scenarios.push({ configuration, movement, requested: count, generated: actual, nominalRoutes: simulator.aircraft.filter(a => a.route.flow?.status === 'nominal').length, reviewRoutes: simulator.aircraft.filter(a => a.route.flow?.status === 'review').length, generationMs: +generationMs.toFixed(3), updateMeanMs: +(updateMs / 600).toFixed(3), predictionMeanMs: +(predictionMs / 30).toFixed(3), predictionMaxMs: +maxPredictionMs.toFixed(3), positionsChecked: actual * 600, awaitingClearance: simulator.aircraft.filter(a => a.waitingClearanceId).length });
  }
}
for (const [file, snapshot] of [['public/data/lfpg.json', dataBytes], ['public/data/lfpg-operations.json', operationBytes], ['public/data/lfpg-corridors.json', corridorBytes], ['public/data/lfpg-aircraft-restrictions.json', aircraftBytes]] as const) assert.equal(hash(input(file)), hash(snapshot), `${file} changed during validation.`);
const report = { airport: 'LFPG', dataSha256: hash(dataBytes), rulesSha256: hash(operationBytes), corridorsSha256: hash(corridorBytes), aircraftSha256: hash(aircraftBytes), aircraftProfile: DEFAULT_AIRCRAFT_PROFILE, aircraftFlags: operations.activeProfileIds, activeProfileIds: operations.activeProfileIds, comparisons, scenarios, checkedRoutes, checkedPositions, physicalRunwayCrossingsChecked: crossings, unprotectedPhysicalRunwayCrossings: 0,
  objective: ['unrelated apron transit metres', 'main-axis reentries', 'physical distance + bounded total-curvature and main-axis-change metre equivalents', 'actual physical distance'], elapsedMs: +(performance.now() - started).toFixed(3), limitations: ['Nominal flows may differ from a coordinated ATC clearance.', 'No claim that the supplied network contains every real painted turn or current NOTAM.', 'Timings are Node.js algorithm measurements and exclude Canvas.'] };
writeFileSync(new URL('docs/flow-validation-results.json', root), JSON.stringify(report, null, 2) + '\n');
const reviewPairs = new Set(['stand:B24|operation:departure:27L:Q4', 'stand:F02|operation:departure:09R:D3', 'operation:arrival:27R:K3|stand:F02', 'operation:arrival:09L:K6|stand:F02']);
const selected = review.filter(r => reviewPairs.has(`${r.from}|${r.to}`));
for (const configuration of ['east', 'west']) for (const movement of ['departure', 'arrival']) {
  const existing = selected.filter(r => r.configuration === configuration && r.movement === movement);
  for (const candidate of review.filter(r => r.configuration === configuration && r.movement === movement)) {
    if (existing.length >= 3) break;
    if (!selected.includes(candidate)) { selected.push(candidate); existing.push(candidate); }
  }
}
writeFileSync(new URL('docs/flow-review-routes.json', root), JSON.stringify({ dataSha256: report.dataSha256, rulesSha256: report.rulesSha256, corridorsSha256: report.corridorsSha256, aircraftSha256: report.aircraftSha256, aircraftProfile: report.aircraftProfile, activeProfileIds: report.activeProfileIds, routes: selected }, null, 2) + '\n');
console.log(`Flow mode validated: ${checkedRoutes} routes, ${checkedPositions} exact-edge positions, ${crossings} physically protected runway crossings.`);
for (const item of scenarios.filter(s => s.requested === 100)) console.log(JSON.stringify(item));
