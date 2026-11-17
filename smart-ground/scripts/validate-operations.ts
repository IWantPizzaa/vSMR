/** Independent offline validation of the published operational overlay. */
import { strict as assert } from 'node:assert';
import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync } from 'node:fs';
import { performance } from 'node:perf_hooks';
import { AircraftSimulator, ConflictDetector, OperatingRules, RoutePlanner, TaxiGraph } from '../src/core';
import type { AirportData, MovementMode, OperatingConfiguration, RouteOperationalData, TaxiEdge } from '../src/core/types';
import type { OperatingManifest } from '../src/core/OperatingRules';
import { resolveOperatingManifest } from '../src/core/resolveOperatingManifest';

type Phase = 'start' | 'taxi' | 'destination';
type SourceEdge = TaxiEdge & { sourceSegmentId?: string };
const root = new URL('../', import.meta.url);
const bytes = readFileSync(new URL('public/data/lfpg.json', root));
const ruleBytes = readFileSync(new URL('public/data/lfpg-operations.json', root));
const data = JSON.parse(bytes.toString('utf8')) as AirportData;
const baseManifest = JSON.parse(ruleBytes.toString('utf8')) as OperatingManifest;
const manifest = resolveOperatingManifest(baseManifest);
const graph = new TaxiGraph(data), rules = new OperatingRules(graph, manifest);
assert.ok(rules.valid, JSON.stringify(rules.diagnostics));
const hash = (value: Buffer) => createHash('sha256').update(value).digest('hex');
if (typeof manifest.dataSha256 === 'string') assert.equal(manifest.dataSha256, hash(bytes), 'Rule overlay must match its source graph.');
const runwayAxes = new Map<string, { runway: string; dx: number; dy: number }[]>();
for (const edge of graph.edges.values()) {
  if (edge.kind !== 'runway' || !/^(08|09)[LR]\/(26|27)[LR]$/.test(edge.name)) continue;
  const from = graph.nodes.get(edge.from)!, to = graph.nodes.get(edge.to)!;
  for (const id of [edge.from, edge.to]) { const list = runwayAxes.get(id) ?? []; list.push({ runway: edge.name, dx: to.x - from.x, dy: to.y - from.y }); runwayAxes.set(id, list); }
}
let physicalRunwayCrossings = 0;
function verifyPhysicalRunwayProtection(route: import('../src/core/types').Route) {
  let previousCrossingDistance = -Infinity;
  for (let i = 1; i < route.nodeIds.length - 1; i++) {
    const node = graph.nodes.get(route.nodeIds[i])!, before = graph.nodes.get(route.nodeIds[i - 1])!, after = graph.nodes.get(route.nodeIds[i + 1])!;
    const checked = new Set<string>();
    for (const axis of runwayAxes.get(node.id) ?? []) {
      if (checked.has(axis.runway)) continue; checked.add(axis.runway);
      const length = Math.hypot(axis.dx, axis.dy);
      const sideBefore = ((before.x - node.x) * axis.dy - (before.y - node.y) * axis.dx) / length;
      const sideAfter = ((after.x - node.x) * axis.dy - (after.y - node.y) * axis.dx) / length;
      if (sideBefore * sideAfter >= -1e-6) continue;
      const crossingDistance = route.cumulativeDistances[i];
      const protective = route.operational?.clearanceCheckpoints.find(checkpoint => {
        if (checkpoint.distance <= previousCrossingDistance || checkpoint.distance >= crossingDistance - 1e-6) return false;
        const rule = manifest.clearanceRules?.find(r => r.id === checkpoint.ruleId);
        const sourceHold = typeof rule?.sourceHoldingLocationId === 'string' ? data.locations.find(l => l.id === rule.sourceHoldingLocationId) : null;
        return rule?.runway === axis.runway && sourceHold?.nodeId === checkpoint.nodeId && sourceHold.kind === 'holding_point';
      });
      assert.ok(protective, `Unprotected physical runway crossing ${axis.runway} at ${node.id}: ${route.nodeIds[0]} → ${route.nodeIds.at(-1)}`);
      physicalRunwayCrossings++; previousCrossingDistance = crossingDistance;
    }
  }
}

/** The oracle derives permissions directly from JSON, independently of OperatingRules. */
class Oracle {
  private masks = new Map<string, number>();
  private turns = new Set<string>();
  private allowedTurns = new Set<string>();
  private parking = new Map<string, number>();
  constructor(readonly configuration: OperatingConfiguration, readonly movement: MovementMode) {
    const applies = (rule: { configuration: string; movement?: string }) => (rule.configuration === 'both' || rule.configuration === configuration) && (!rule.movement || rule.movement === 'both' || rule.movement === movement);
    for (const rule of manifest.edgeRules.filter(applies)) for (const id of rule.edgeIds) this.masks.set(id, (this.masks.get(id) ?? 3) & (rule.direction === 'forward' ? 1 : rule.direction === 'reverse' ? 2 : 0));
    for (const rule of (manifest.turnRules ?? []).filter(applies)) this.turns.add(JSON.stringify([rule.viaNodeId, rule.fromEdgeId, rule.toEdgeId]));
    for (const rule of (manifest.allowedTurnRules ?? []).filter(applies)) this.allowedTurns.add(JSON.stringify([rule.viaNodeId, rule.fromEdgeId, rule.toEdgeId]));
    let serial = 0;
    for (const from of graph.nodes.keys()) {
      if (this.parking.has(from) || !graph.adjacency.get(from)!.some(a => graph.edges.get(a.edgeId)!.kind === 'parking_position')) continue;
      const queue = [from]; this.parking.set(from, serial);
      for (let i = 0; i < queue.length; i++) for (const arc of graph.adjacency.get(queue[i])!) if (graph.edges.get(arc.edgeId)!.kind === 'parking_position' && !this.parking.has(arc.to)) { this.parking.set(arc.to, serial); queue.push(arc.to); }
      serial++;
    }
  }
  allowEdge(id: string, from: string) { const edge = graph.edges.get(id)!; return edge.kind !== 'runway' && !!((this.masks.get(id) ?? 3) & (edge.from === from ? 1 : 2)); }
  allowTurn(incoming: string | null, node: string, next: string) {
    if (!incoming) return true;
    const key = JSON.stringify([node, incoming, next]);
    if (this.turns.has(key) || (manifest.forbidUTurns && incoming === next)) return false;
    if (!manifest.forbidUnmarkedSyntheticTurns || !node.startsWith('intersection:')) return true;
    const first = graph.edges.get(incoming) as SourceEdge, second = graph.edges.get(next) as SourceEdge;
    return incoming !== next && !!first.sourceSegmentId && first.sourceSegmentId === second.sourceSegmentId || this.allowedTurns.has(key);
  }
  phase(id: string, phase: Phase, origin: string, destination: string | null): Phase | null {
    if (!manifest.restrictParkingToEndpoints) return phase;
    const edge = graph.edges.get(id)!;
    if (edge.kind !== 'parking_position') return phase === 'destination' ? null : 'taxi';
    const component = this.parking.get(edge.from);
    if (phase === 'start' && component !== undefined && component === this.parking.get(origin)) return 'start';
    if (destination !== null && component !== undefined && component === this.parking.get(destination)) return 'destination';
    return null;
  }
  hasParking(node: string) { return this.parking.has(node); }
  // Linear-priority Dijkstra is independent of the production binary heap.
  distance(from: string, to: string): number {
    const pending = new Map<string, { node: string; incoming: string | null; phase: Phase; cost: number }>();
    pending.set(JSON.stringify([from, null, 'start']), { node: from, incoming: null, phase: 'start', cost: 0 });
    const visited = new Set<string>();
    while (pending.size) {
      let key = '', best = Infinity;
      for (const [candidate, state] of pending) if (state.cost < best) { key = candidate; best = state.cost; }
      const state = pending.get(key)!; pending.delete(key); if (visited.has(key)) continue; visited.add(key);
      if (state.node === to) return state.cost;
      for (const arc of graph.adjacency.get(state.node) ?? []) {
        if (!this.allowEdge(arc.edgeId, state.node) || !this.allowTurn(state.incoming, state.node, arc.edgeId)) continue;
        const phase = this.phase(arc.edgeId, state.phase, from, to); if (phase === null) continue;
        const nextKey = JSON.stringify([arc.to, arc.edgeId, phase]), cost = state.cost + arc.length;
        if (!visited.has(nextKey) && cost < (pending.get(nextKey)?.cost ?? Infinity)) pending.set(nextKey, { node: arc.to, incoming: arc.edgeId, phase, cost });
      }
    }
    return Infinity;
  }
  reachable(from: string, destination: string | null = null): Set<string> {
    const queue: { node: string; incoming: string | null; phase: Phase }[] = [{ node: from, incoming: null, phase: 'start' }];
    const visited = new Set([JSON.stringify([from, null, 'start'])]), result = new Set([from]);
    for (let i = 0; i < queue.length; i++) {
      const state = queue[i];
      for (const arc of graph.adjacency.get(state.node) ?? []) {
        if (!this.allowEdge(arc.edgeId, state.node) || !this.allowTurn(state.incoming, state.node, arc.edgeId)) continue;
        const phase = this.phase(arc.edgeId, state.phase, from, destination); if (phase === null) continue;
        const key = JSON.stringify([arc.to, arc.edgeId, phase]); if (visited.has(key)) continue;
        visited.add(key); result.add(arc.to); queue.push({ node: arc.to, incoming: arc.edgeId, phase });
      }
    }
    return result;
  }
}

const started = performance.now();
const stands = rules.locations.filter(l => l.kind === 'stand' && l.nodeId && rules.isLocationOpen(l.id));
const matrix: Record<string, { access: string; runway: string; reachableStands: number; unreachableStandIds: string[] }[]> = {};
const comparisons: { configuration: string; movement: string; from: string; to: string; distance: number | null; oracleDistance: number | null }[] = [];
const reviewRoutes: { configuration: string; movement: string; from: string; to: string; fromName: string; toName: string; distance: number; nodeIds: string[]; edgeIds: string[]; operational?: RouteOperationalData }[] = [];
const scenarios: Record<string, unknown>[] = [];
let inspectedRoutes = 0, inspectedPositions = 0;
for (const configuration of ['east', 'west'] as const) {
  const planner = new RoutePlanner(graph, rules, configuration), oracle = new Oracle(configuration, 'departure');
  const accesses = rules.runwayAccess(configuration, 'departure').map(a => ({ access: a, location: rules.locations.find(l => l.id === a.locationId)! }));
  const rows = accesses.map(({ access }) => ({ access: access.locationId, runway: access.runway, reachableStands: 0, unreachableStandIds: [] as string[] }));
  for (const stand of stands) {
    const reachable = oracle.reachable(stand.nodeId!);
    accesses.forEach(({ location }, index) => {
      const connected = oracle.hasParking(location.nodeId!) ? oracle.reachable(stand.nodeId!, location.nodeId!).has(location.nodeId!) : reachable.has(location.nodeId!);
      if (connected) rows[index].reachableStands++; else rows[index].unreachableStandIds.push(stand.id);
    });
  }
  matrix[configuration] = rows;
  for (const movement of ['departure', 'arrival', 'transfer'] as const) {
    const oracle = new Oracle(configuration, movement);
    const origins = movement === 'arrival' ? planner.availableLocations('arrival') : stands;
    const destinations = movement === 'departure' ? planner.availableLocations('departure') : stands;
    // Spread the chosen stands over the imported list; do not assert every source is routable.
    for (let i = 0; i < 12 && origins.length && destinations.length; i++) {
      const from = origins[Math.floor(i * origins.length / 12)], to = destinations[(i * 37 + 5) % destinations.length];
      if (!from.nodeId || !to.nodeId) continue;
      const route = planner.findRoute(from.nodeId, to.nodeId, { movement }), expected = oracle.distance(from.nodeId, to.nodeId);
      assert.equal(!!route, Number.isFinite(expected), `${configuration}/${movement} ${from.id} → ${to.id}`);
      if (route) {
        assert.ok(Math.abs(route.distance - expected) < 1e-6);
        assert.ok(planner.isRouteAllowed(route, movement)); verifyPhysicalRunwayProtection(route); inspectedRoutes++;
        if (route.distance > 0) reviewRoutes.push({ configuration, movement, from: from.id, to: to.id, fromName: from.name, toName: to.name, distance: route.distance, nodeIds: route.nodeIds, edgeIds: route.edgeIds, operational: route.operational });
      }
      comparisons.push({ configuration, movement, from: from.id, to: to.id, distance: route ? Number(route.distance.toFixed(3)) : null, oracleDistance: Number.isFinite(expected) ? Number(expected.toFixed(3)) : null });
    }
    for (const count of [10, 25, 50, 100]) {
      const sim = new AircraftSimulator(graph, planner), t = performance.now(), actual = sim.generate(count, 20261112, { movement });
      const generationMs = performance.now() - t, detector = new ConflictDetector(graph); let predictionMs = 0, peakPredictionMs = 0, updateMs = 0, peakUpdateMs = 0;
      for (const aircraft of sim.aircraft) { assert.ok(planner.isRouteAllowed(aircraft.route, movement)); verifyPhysicalRunwayProtection(aircraft.route); inspectedRoutes++; }
      for (let frame = 0; frame < 600; frame++) {
        const updateStarted = performance.now(); sim.update(.25); const elapsedUpdate = performance.now() - updateStarted;
        updateMs += elapsedUpdate; peakUpdateMs = Math.max(peakUpdateMs, elapsedUpdate);
        for (const aircraft of sim.aircraft) {
          const from = graph.nodes.get(aircraft.fromNodeId)!, to = graph.nodes.get(aircraft.toNodeId)!;
          assert.ok(Math.abs(aircraft.x - from.x - (to.x - from.x) * aircraft.edgeProgress) < 1e-6);
          assert.ok(Math.abs(aircraft.y - from.y - (to.y - from.y) * aircraft.edgeProgress) < 1e-6); inspectedPositions++;
          if (aircraft.waitingClearanceId) assert.equal(aircraft.state, 'holding');
        }
        if (frame % 20 === 0) { const t = performance.now(); detector.detect(sim.aircraft, 120); const ms = performance.now() - t; predictionMs += ms; peakPredictionMs = Math.max(peakPredictionMs, ms); }
      }
      scenarios.push({ configuration, movement, requested: count, generated: actual, generationMs: Number(generationMs.toFixed(3)), updateMeanMs: Number((updateMs / 600).toFixed(3)), updateMaxMs: Number(peakUpdateMs.toFixed(3)), predictionMeanMs: Number((predictionMs / 30).toFixed(3)), predictionMaxMs: Number(peakPredictionMs.toFixed(3)), positionsChecked: actual * 600, awaitingClearance: sim.aircraft.filter(a => a.waitingClearanceId).length });
    }
  }
}
const report = {
  airport: 'LFPG', dataSha256: hash(bytes), rulesSha256: hash(ruleBytes), effectiveDate: manifest.effectiveDate,
  activeProfileIds: manifest.activeProfileIds, closedLocationIds: manifest.closedLocationIds,
  diagnostics: rules.diagnostics, policy: { forbidUTurns: manifest.forbidUTurns, forbidUnmarkedSyntheticTurns: manifest.forbidUnmarkedSyntheticTurns, restrictParkingToEndpoints: manifest.restrictParkingToEndpoints, unknownArcsRemainExplicitlyUnverified: true },
  standsConsidered: stands.length, departureReachability: matrix, comparisons, scenarios, inspectedRoutes, inspectedPositions,
  physicalRunwayCrossingsChecked: physicalRunwayCrossings, unprotectedPhysicalRunwayCrossings: 0,
  elapsedMs: Number((performance.now() - started).toFixed(3)), limitations: ['Reachability follows the sourced overlay and conservative synthetic-turn policy, not undocumented ATC clearances.', 'No claim of complete current NOTAM coverage or certified IRL routing.', 'Node.js algorithm timings exclude Canvas rendering.'],
};
assert.equal(hash(readFileSync(new URL('public/data/lfpg-operations.json', root))), hash(ruleBytes), 'Operating sources changed during validation; rerun against the final snapshot.');
const selectedReviewRoutes: typeof reviewRoutes = [];
for (const configuration of ['east', 'west']) for (const movement of ['departure', 'arrival', 'transfer']) {
  const candidates = reviewRoutes.filter(r => r.configuration === configuration && r.movement === movement);
  const selected: typeof reviewRoutes = [], runways = new Set<string>();
  for (const route of candidates) {
    const runway = manifest.runwayAccess?.find(a => a.locationId === (movement === 'arrival' ? route.from : route.to))?.runway ?? 'transfer';
    if (!runways.has(runway)) { selected.push(route); runways.add(runway); }
  }
  for (const route of candidates) if (selected.length < 3 && !selected.includes(route)) selected.push(route);
  selectedReviewRoutes.push(...selected.slice(0, 3));
}
writeFileSync(new URL('docs/operations-validation-results.json', root), JSON.stringify(report, null, 2) + '\n');
writeFileSync(new URL('docs/operating-review-routes.json', root), JSON.stringify({ dataSha256: hash(bytes), rulesSha256: hash(ruleBytes), activeProfileIds: manifest.activeProfileIds, routes: selectedReviewRoutes }, null, 2) + '\n');
console.log(`LFPG operations validated: ${comparisons.length} independent directed Dijkstra comparisons, ${inspectedRoutes} checked routes, ${inspectedPositions} exact-edge positions.`);
for (const [configuration, rows] of Object.entries(matrix)) console.log(`${configuration}: ${rows.map(r => `${r.access}=${r.reachableStands}/${stands.length}`).join(', ')}`);
for (const scenario of scenarios.filter(s => s.requested === 100)) console.log(JSON.stringify(scenario));
