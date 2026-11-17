/** Offline quality/replay audit of the same planner, defaults and generator as the UI. */
import { strict as assert } from 'node:assert';
import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync } from 'node:fs';
import { performance } from 'node:perf_hooks';
import { AircraftSimulator, OperatingRules, RoutePlanner, TaxiFlowPolicy, TaxiGraph, TrafficPredictor } from '../src/core';
import { resolveOperatingManifest } from '../src/core/resolveOperatingManifest';
import { AircraftRestrictions, type AircraftRestrictionManifest } from '../src/core/AircraftRestrictions';
import { DEFAULT_AIRCRAFT_PROFILE } from '../src/core/AircraftProfile';
import type { AirportData, Route } from '../src/core/types';
import type { OperatingManifest } from '../src/core/OperatingRules';
import type { TaxiFlowManifest } from '../src/core/TaxiFlowPolicy';

const root = new URL('../', import.meta.url), started = performance.now();
const holdingsOnly = process.argv.includes('--holdings-only');
const read = (name: string) => readFileSync(new URL(name, root));
const hash = (bytes: Buffer) => createHash('sha256').update(bytes).digest('hex');
const snapshots = ['public/data/lfpg.json', 'public/data/lfpg-operations.json', 'public/data/lfpg-corridors.json', 'public/data/lfpg-aircraft-restrictions.json'].map(name => ({ name, bytes: read(name) }));
const data = JSON.parse(snapshots[0].bytes.toString()) as AirportData;
const baseOperations = JSON.parse(snapshots[1].bytes.toString()) as OperatingManifest;
const corridors = JSON.parse(snapshots[2].bytes.toString()) as TaxiFlowManifest;
assert.equal(corridors.dataSha256, hash(snapshots[0].bytes));
const graph = new TaxiGraph(data), flow = new TaxiFlowPolicy(graph, corridors);
const aircraftRestrictions = new AircraftRestrictions(graph, JSON.parse(snapshots[3].bytes.toString()) as AircraftRestrictionManifest);
assert.ok(aircraftRestrictions.valid, aircraftRestrictions.diagnostics.join('; '));
assert.ok(flow.valid, flow.diagnostics.join('; '));
assert.ok(flow.specialAreaEdgeIds.size, 'The shipped catalogue must explicitly map actual deicing-pad lanes.');
const runwayAxes = new Map<string, { runway: string; dx: number; dy: number }[]>();
for (const edge of graph.edges.values()) {
  if (edge.kind !== 'runway' || !/^(08|09)[LR]\/(26|27)[LR]$/.test(edge.name)) continue;
  const a = graph.nodes.get(edge.from)!, b = graph.nodes.get(edge.to)!;
  for (const nodeId of [edge.from, edge.to]) { const list = runwayAxes.get(nodeId) || []; list.push({ runway: edge.name, dx: b.x - a.x, dy: b.y - a.y }); runwayAxes.set(nodeId, list); }
}
let routes = 0, positions = 0, crossings = 0;
let holdingTransitionsChecked = 0;
const holdingAnomalies: { from?: string; to?: string; nodeId: string; degrees: number; remainingMetres: number }[] = [];
function verifyHoldingGeometry(route: Route) {
  for (let i = 2; i < route.edgeIds.length; i++) {
    const remaining = route.distance - route.cumulativeDistances[i]; if (remaining > 1000) continue;
    const incoming = graph.edges.get(route.edgeIds[i - 1])!, outgoing = graph.edges.get(route.edgeIds[i])!;
    if (incoming.kind !== 'taxiway' || outgoing.kind !== 'taxiway' || graph.edges.get(route.edgeIds[i - 2])!.kind === 'parking_position') continue;
    const [a, b, c] = route.nodeIds.slice(i - 1, i + 2).map(id => graph.nodes.get(id)!);
    const ux = b.x - a.x, uy = b.y - a.y, vx = c.x - b.x, vy = c.y - b.y;
    const degrees = Math.abs(Math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)) * 180 / Math.PI;
    holdingTransitionsChecked++;
    if (degrees >= 150 - 1e-8) holdingAnomalies.push({ from: route.flow?.originId, to: route.flow?.destinationId, nodeId: b.id, degrees, remainingMetres: remaining });
  }
}
function verifyRoute(route: Route, planner: RoutePlanner, operations: OperatingManifest) {
  assert.ok(planner.isRouteAllowed(route), 'Hard directions, turn restrictions, parking phase and nominal gates must survive replay.');
  assert.ok(route.flow);
  assert.equal(route.aircraft?.profile.typeCode, DEFAULT_AIRCRAFT_PROFILE.typeCode);
  assert.ok(route.edgeIds.every(id => !flow.specialAreaEdgeIds.has(id)), 'Routine taxi must never use a treatment-pad lane.');
  assert.ok(Math.abs(route.distance - route.edgeIds.reduce((total, id) => total + graph.edges.get(id)!.length, 0)) < 1e-5);
  const replay = planner.describeRoute(route);
  for (const key of ['significantTurns', 'mainAxisChanges', 'turnCostMetres', 'curvatureDegrees', 'curvatureCostMetres', 'routingCostMetres'] as const) assert.equal(replay.flow?.[key], route.flow[key], `Quality field ${key} must replay identically.`);
  let previousCrossingDistance = -Infinity;
  for (let i = 1; i < route.nodeIds.length - 1; i++) {
    const node = graph.nodes.get(route.nodeIds[i])!, before = graph.nodes.get(route.nodeIds[i - 1])!, after = graph.nodes.get(route.nodeIds[i + 1])!;
    const checked = new Set<string>();
    for (const axis of runwayAxes.get(node.id) || []) {
      if (checked.has(axis.runway)) continue; checked.add(axis.runway);
      const length = Math.hypot(axis.dx, axis.dy), side = (point: typeof node) => ((point.x - node.x) * axis.dy - (point.y - node.y) * axis.dx) / length;
      if (side(before) * side(after) >= -1e-6) continue;
      const distance = route.cumulativeDistances[i];
      assert.ok(route.operational?.clearanceCheckpoints.some(checkpoint => {
        if (checkpoint.distance <= previousCrossingDistance || checkpoint.distance >= distance - 1e-6) return false;
        const rule = operations.clearanceRules?.find(r => r.id === checkpoint.ruleId);
        const holding = typeof rule?.sourceHoldingLocationId === 'string' ? data.locations.find(l => l.id === rule.sourceHoldingLocationId) : undefined;
        return rule?.runway === axis.runway && holding?.kind === 'holding_point' && holding.nodeId === checkpoint.nodeId;
      }), `No source holding checkpoint before physical crossing ${axis.runway}/${node.id}`);
      previousCrossingDistance = distance; crossings++;
    }
  }
  routes++;
}

const scenarios: { works: string; configuration: string; movement: string; generated: number; generationMs: number; turns: number; changes: number; positions: number }[] = [];
const b12: { works: string; access: string; metres: number; axes: string; turns: number; changes: number }[] = [];
const holdingSweeps: { works: string; configuration: string; stands: number; accesses: number; routes: number }[] = [];
for (const works of ['default', 'none'] as const) {
  const operations = resolveOperatingManifest(baseOperations, works === 'none' ? { enabledProfileIds: [] } : {}), rules = new OperatingRules(graph, operations);
  assert.ok(rules.valid, JSON.stringify(rules.diagnostics));
  for (const configuration of ['east', 'west'] as const) {
    const planner = new RoutePlanner(graph, rules, configuration); planner.setFlowPolicy(flow);
    planner.setAircraftRestrictions(aircraftRestrictions); planner.setAircraftProfile(DEFAULT_AIRCRAFT_PROFILE);
    planner.setAircraftRestrictionContext({ airportFlags: Array.isArray(operations.activeProfileIds) ? operations.activeProfileIds.filter((id): id is string => typeof id === 'string') : [] });
    const stands = ['B08', 'M09', 'B03', 'F02', 'A39', 'B12', 'J14', 'I24'].map(id => planner.locations.find(l => l.id === `stand:${id}`)!)
      .filter(l => l?.nodeId && rules.isLocationOpen(l.id));
    const accesses = planner.availableLocations('departure').filter(l => l.nodeId);
    let holdingRoutes = 0;
    for (const from of stands) for (const to of accesses) {
      const route = planner.findRoute(from.nodeId!, to.nodeId!, { movement: 'departure', originId: from.id, destinationId: to.id });
      assert.ok(route, `${works}/${configuration}: ${from.id} to ${to.id}: ${planner.lastFlowFailure}`);
      verifyRoute(route, planner, operations); verifyHoldingGeometry(route); holdingRoutes++;
    }
    holdingSweeps.push({ works, configuration, stands: stands.length, accesses: accesses.length, routes: holdingRoutes });
    if (holdingsOnly) continue;
    if (configuration === 'west') {
      const from = planner.locations.find(l => l.id === 'stand:B12')!;
      for (const suffix of ['T9', 'T10', 'T11', 'T12']) {
        const access = rules.runwayAccess('west', 'departure').find(a => a.runway === '26R' && a.locationId.endsWith(`:${suffix}`));
        assert.ok(access, `Missing 26R/${suffix}`);
        const to = planner.locations.find(l => l.id === access.locationId)!;
        const route = planner.findRoute(from.nodeId!, to.nodeId!, { movement: 'departure', originId: from.id, destinationId: to.id });
        assert.ok(route, `B12 to ${suffix}: ${planner.lastFlowFailure}`); verifyRoute(route, planner, operations);
        // Independent geometry labels, rather than trusting the planner's axes list.
        assert.ok(route.edgeIds.some(id => graph.edges.get(id)!.name === 'G3'), 'B12 must use its actual G3 apron egress.');
        assert.ok(route.edgeIds.some(id => graph.edges.get(id)!.name === 'F'), 'Westbound nominal departure follows southbound F.');
        assert.ok(route.edgeIds.some(id => graph.edges.get(id)!.name === (suffix === 'T9' ? 'RT11' : 'T')), '26R access uses the continuous T corridor or its direct RT11/T9 connector.');
        b12.push({ works, access: suffix, metres: +route.distance.toFixed(1), axes: route.flow!.axes.join(' → '), turns: route.flow!.significantTurns!, changes: route.flow!.mainAxisChanges! });
      }
    }
    for (const movement of ['departure', 'arrival', 'transfer'] as const) {
      const sim = new AircraftSimulator(graph, planner), generationStart = performance.now();
      const generated = sim.generate(100, 20261112, { movement }), generationMs = performance.now() - generationStart;
      assert.equal(generated, 100, `${works}/${configuration}/${movement}`);
      for (const aircraft of sim.aircraft) { verifyRoute(aircraft.route, planner, operations); assert.equal(aircraft.route.flow?.status, 'nominal'); }
      for (let step = 0; step < 600; step++) {
        sim.update(.25);
        for (const aircraft of sim.aircraft) {
          const from = graph.nodes.get(aircraft.fromNodeId)!, to = graph.nodes.get(aircraft.toNodeId)!;
          assert.ok(Math.abs(aircraft.x - from.x - (to.x - from.x) * aircraft.edgeProgress) < 1e-6);
          assert.ok(Math.abs(aircraft.y - from.y - (to.y - from.y) * aircraft.edgeProgress) < 1e-6);
          if (aircraft.waitingClearanceId) assert.equal(aircraft.state, 'holding');
          positions++;
        }
      }
      for (const aircraft of sim.aircraft.filter(a => a.waitingClearanceId)) assert.ok(new TrafficPredictor(graph).predict([aircraft], 300)[0].edges.every(e => e.stationary));
      const item = { works, configuration, movement, generated, generationMs: +generationMs.toFixed(1), turns: sim.aircraft.reduce((total, a) => total + a.route.flow!.significantTurns!, 0), changes: sim.aircraft.reduce((total, a) => total + a.route.flow!.mainAxisChanges!, 0), positions: 60000 };
      scenarios.push(item); console.log(JSON.stringify(item));
    }
  }
}
for (const { name, bytes } of snapshots) assert.equal(hash(read(name)), hash(bytes), `${name} changed during validation.`);
assert.equal(holdingAnomalies.length, 0, `Immediate taxiway reversals in the final 1000m: ${JSON.stringify(holdingAnomalies)}`);
const report = `# LFPG routine route quality validation

Generated with \`npm run validate:route-quality\`. The audit uses the production planner, all four offline catalogues and the declared A320 (${DEFAULT_AIRCRAFT_PROFILE.wingspanMetres}m) profile, with default SUP188 works and with all optional works profiles disabled. It verifies ${routes} routes, ${positions.toLocaleString('en-US')} exact-edge positions and ${crossings} source-protected physical runway crossings. No routine route uses any of the ${flow.specialAreaEdgeIds.size} catalogued treatment-pad edges. All generated traffic is nominal; manual runway crossing authority is preserved.

## Routing model

Hard AIP restrictions, current works, turn prohibitions, stand access phases and ordered nominal corridor gates are applied before route comparison. Routine taxi excludes the explicit, source-mapped treatment-pad lanes; published connectors next to those lanes remain available.

Among feasible routes, the search compares this tuple: foreign/returned apron transit metres; main-axis reentries after apron transit; physical metres plus bounded manoeuvre equivalents; physical metres. An actual change to a different named principal axis adds 120 equivalent metres, including changes across neutral connectors. Total absolute heading variation along the actual source polyline adds 90 equivalent metres per 90° of curvature. Thus a sampled S bend pays for both opposing curves even if no curve vertex is a branching decision. Splitting a straight source segment adds zero curvature; it cannot multiply the cost. A sufficiently shorter legal route can outweigh these finite preferences. These are simulator route-quality preferences, not published ATC minima.

The separate junction counter keeps its existing meaning: choices at branching nodes of at least 30°, measured with 40m tangents along adjoining degree-two curves. Neither that count nor its diagnostic \`turnCostMetres\` is added again to the search cost. \`curvatureDegrees\` and \`curvatureCostMetres\` expose the actual curve contribution. Source-scoped holding-approach profiles match exact physical destination nodes; they are nominal simulator routing preferences, with AIP restrictions and special-area exclusions still applied independently.

The existing no-U-turn option also rejects an immediate deflection of at least 150° between two distinct taxiway edges, using the incident source segments. This is a geometric interpretation of that simulator option, not a new AIP restriction or an aircraft-size limit. Ordinary right-angle junctions and sampled curves remain usable. Parking lead-ins keep their existing pushback abstraction.

The physical distance and simulator interpolation remain the original source-edge lengths. Quality fields are independently reconstructed during route description and checked against the stored result. Synthetic tests cover parallel-axis shortcuts, a justified shorter alternative, densification invariance, and rejection of pad transit during replay/injection.

## Holding access sweep

Eight source origins (B08, M09, B03, F02, A39, B12, J14, I24) are checked against every currently open departure access, excluding PPR accesses unless approved by the production defaults. Each route is checked for endpoint-specific nominal approach gates, source orientation, prohibited turns, pad exclusions, physical length and protected runway crossings.

The final 1000m additionally contains ${holdingTransitionsChecked} independently measured taxiway-to-taxiway transitions, at least two segments beyond a parking lead: zero immediate deflections of 150° or more. This is a scoped holding-approach audit, not a claim that every branch in the entire airport has been certified. For the short sweep alone, run \`npm run validate:route-quality -- --holdings-only\`.

| Works | Configuration | Open source stands | Active accesses | Valid routes |
| --- | --- | ---: | ---: | ---: |
${holdingSweeps.map(r => `| ${r.works} | ${r.configuration} | ${r.stands} | ${r.accesses} | ${r.routes} |`).join('\n')}

## B12 departures to 26R

The source GMC direction on N remains northbound in west configuration. B12 follows its G3 egress and the legal southbound F corridor toward R/T. T9 is reached directly from R via RT11; T10–T12 use T. There is no requirement to touch T and return to the direct T9 connector. The user's requested continuity is preserved without reversing a source-backed one-way axis.

| Works | Access | Metres | Principal axes | Junction turns | Axis changes |
| --- | --- | ---: | --- | ---: | ---: |
${b12.map(r => `| ${r.works} | ${r.access} | ${r.metres} | ${r.axes} | ${r.turns} | ${r.changes} |`).join('\n')}

## Automatic traffic

Each row checks 100 aircraft for 600 motion updates (0.25s each); 60,000 exact-edge positions. Every route is replayed through effective restrictions and physical runway-crossing checkpoints are checked using source holding locations. Pending runway clearances also stop future trajectory predictions.

| Works | Configuration | Movement | Aircraft | Generation ms | Junction turns, total | Axis changes, total |
| --- | --- | --- | ---: | ---: | ---: | ---: |
${scenarios.map(r => `| ${r.works} | ${r.configuration} | ${r.movement} | ${r.generated} | ${r.generationMs} | ${r.turns} | ${r.changes} |`).join('\n')}

## Reproducibility

Seed: 20261112. Node.js: ${process.version}. Elapsed: ${((performance.now() - started) / 1000).toFixed(2)}s. Timings measure algorithms in Node.js and exclude Canvas rendering.

${snapshots.map(s => `- \`${s.name}\`: \`${hash(s.bytes)}\``).join('\n')}

The source snapshot still contains geographic gaps and does not model every current NOTAM or coordinated ATC deviation. The audit demonstrates consistency against the shipped source overlay; it does not certify every real-world taxi clearance.
`;
if (!holdingsOnly) writeFileSync(new URL('docs/ROUTE-QUALITY-VALIDATION.md', root), report);
else console.log(JSON.stringify({ holdingSweeps, holdingTransitionsChecked, holdingAnomalies }));
console.log(`Route quality validated: ${routes} routes; ${positions} positions; ${crossings} protected physical crossings; ${flow.specialAreaEdgeIds.size} treatment-pad edges excluded.`);
