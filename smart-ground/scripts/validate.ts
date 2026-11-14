/** Offline validation. Run npm run validate; no server or external requests. */
import { strict as assert } from 'node:assert';
import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { performance } from 'node:perf_hooks';
import { AircraftSimulator, ConflictDetector, RoutePlanner, TaxiGraph } from '../src/core/index';
import type { AirportData, AirportSurface, TaxiEdge, TaxiNode } from '../src/core/types';

type Point = [number, number];
type SourceEdge = TaxiEdge & { sourceWayId?: number };
type Building = AirportSurface & { holes?: Point[][]; sourceId?: string };
const root = new URL('../', import.meta.url);
const bytes = readFileSync(new URL('public/data/lfpg.json', root));
const data = JSON.parse(bytes.toString('utf8')) as AirportData;
const graph = new TaxiGraph(data), planner = new RoutePlanner(graph);
const source = JSON.parse(readFileSync(new URL('data-sources/lfpg-osm.json', root), 'utf8')) as { elements: { type: string; id: number; tags?: Record<string, string> }[] };
const sourceWays = new Map(source.elements.filter(e => e.type === 'way').map(e => [e.id, e.tags ?? {}]));
const started = performance.now();
const round = (value: number) => Number(value.toFixed(3));

function cross(a: Point, b: Point): number { return a[0] * b[1] - a[1] * b[0]; }
function segmentIntersection(a: Point, b: Point, c: Point, d: Point): { t: number; u: number; x: number; y: number } | null {
  const r: Point = [b[0] - a[0], b[1] - a[1]], s: Point = [d[0] - c[0], d[1] - c[1]], determinant = cross(r, s);
  if (Math.abs(determinant) < 1e-9) return null;
  const q: Point = [c[0] - a[0], c[1] - a[1]], t = cross(q, s) / determinant, u = cross(q, r) / determinant;
  if (t < -1e-8 || t > 1 + 1e-8 || u < -1e-8 || u > 1 + 1e-8) return null;
  return { t, u, x: a[0] + t * r[0], y: a[1] + t * r[1] };
}
function bounds(points: Point[]): [number, number, number, number] {
  return [Math.min(...points.map(p => p[0])), Math.min(...points.map(p => p[1])), Math.max(...points.map(p => p[0])), Math.max(...points.map(p => p[1]))];
}
function pointInside(point: Point, ring: Point[]): boolean {
  let inside = false;
  for (let i = 0, j = ring.length - 1; i < ring.length; j = i++) {
    const a = ring[i], b = ring[j];
    if ((a[1] > point[1]) !== (b[1] > point[1]) && point[0] < (b[0] - a[0]) * (point[1] - a[1]) / (b[1] - a[1]) + a[0]) inside = !inside;
  }
  return inside;
}
function boundaryDistance(point: Point, ring: Point[]): number {
  let best = Infinity;
  for (let i = 0; i < ring.length; i++) {
    const a = ring[i], b = ring[(i + 1) % ring.length], dx = b[0] - a[0], dy = b[1] - a[1];
    const ratio = Math.max(0, Math.min(1, ((point[0] - a[0]) * dx + (point[1] - a[1]) * dy) / (dx * dx + dy * dy || 1)));
    best = Math.min(best, Math.hypot(point[0] - a[0] - ratio * dx, point[1] - a[1] - ratio * dy));
  }
  return best;
}
function coordinates(node: TaxiNode): Point { return [node.x, node.y]; }

// Check every edge independently of the importer's geometry implementation.
let maxLengthError = 0;
for (const edge of graph.edges.values()) {
  const from = graph.nodes.get(edge.from), to = graph.nodes.get(edge.to);
  assert.ok(from && to, `${edge.id}: missing endpoint`);
  assert.ok(edge.length > 0 && Number.isFinite(edge.length), `${edge.id}: invalid length`);
  const error = Math.abs(edge.length - Math.hypot(from.x - to.x, from.y - to.y));
  maxLengthError = Math.max(maxLengthError, error);
  assert.ok(error < 0.001, `${edge.id}: geometry/weight mismatch ${error}m`);
}
assert.equal(graph.components[0].length, Math.max(...graph.components.map(c => c.length)), 'component 0 must be primary');

// A spatial grid checks actual source intersections without making new edges.
const cells = new Map<string, number[]>(), seen = new Set<string>();
const edgeList = [...graph.edges.values()] as SourceEdge[];
const unresolved: { edges: string[]; x: number; y: number }[] = [];
const layerSeparated: { edges: string[]; x: number; y: number }[] = [];
let candidatePairs = 0;
for (let i = 0; i < edgeList.length; i++) {
  const edge = edgeList[i], from = graph.nodes.get(edge.from)!, to = graph.nodes.get(edge.to)!;
  const [minX, minY, maxX, maxY] = bounds([coordinates(from), coordinates(to)]);
  for (let x = Math.floor(minX / 100); x <= Math.floor(maxX / 100); x++) for (let y = Math.floor(minY / 100); y <= Math.floor(maxY / 100); y++) {
    const key = `${x},${y}`, candidates = cells.get(key) ?? [];
    for (const j of candidates) {
      const pair = `${j}:${i}`;
      if (seen.has(pair)) continue; seen.add(pair);
      const other = edgeList[j];
      if (edge.from === other.from || edge.from === other.to || edge.to === other.from || edge.to === other.to) continue;
      candidatePairs++;
      const intersection = segmentIntersection(coordinates(from), coordinates(to), coordinates(graph.nodes.get(other.from)!), coordinates(graph.nodes.get(other.to)!));
      if (!intersection) continue;
      // Numerical intersections within 10 micrometres of an endpoint are already noded.
      const firstEndpoint = intersection.t < 1e-7 || intersection.t > 1 - 1e-7;
      const secondEndpoint = intersection.u < 1e-7 || intersection.u > 1 - 1e-7;
      if (firstEndpoint && secondEndpoint && Math.min(Math.hypot(intersection.x - from.x, intersection.y - from.y), Math.hypot(intersection.x - to.x, intersection.y - to.y)) < 0.00001) continue;
      const entry = { edges: [other.id, edge.id], x: round(intersection.x), y: round(intersection.y) };
      const layer = sourceWays.get(edge.sourceWayId ?? -1)?.layer ?? '0', otherLayer = sourceWays.get(other.sourceWayId ?? -1)?.layer ?? '0';
      (layer !== otherLayer ? layerSeparated : unresolved).push(entry);
    }
    candidates.push(i); cells.set(key, candidates);
  }
}
assert.equal(unresolved.length, 0, `Non-node same-layer crossings remain: ${JSON.stringify(unresolved.slice(0, 4))}`);

// Independent point-in-polygon + boundary-distance sampling (no Shapely dependency).
// We inspect centres at <=0.25m intervals; a >1m run >1m inside a building fails.
const buildings = (data.surfaces as Building[]).filter(s => s.kind === 'building').map(surface => ({ surface, bbox: bounds(surface.points) }));
let buildingCandidatePairs = 0, buildingSamples = 0;
const buildingPenetrations: { edge: string; building: string; interiorMetres: number }[] = [];
for (const edge of edgeList) {
  const from = graph.nodes.get(edge.from)!, to = graph.nodes.get(edge.to)!;
  const [minX, minY, maxX, maxY] = bounds([coordinates(from), coordinates(to)]);
  for (const { surface, bbox } of buildings) {
    if (maxX < bbox[0] || maxY < bbox[1] || minX > bbox[2] || minY > bbox[3]) continue;
    buildingCandidatePairs++;
    const rings = [surface.points, ...(surface.holes ?? [])], cuts = [0, 1];
    for (const ring of rings) for (let i = 0; i < ring.length; i++) {
      const intersection = segmentIntersection(coordinates(from), coordinates(to), ring[i], ring[(i + 1) % ring.length]);
      if (intersection) cuts.push(Math.max(0, Math.min(1, intersection.t)));
    }
    cuts.sort((a, b) => a - b);
    let interior = 0;
    for (let i = 0; i < cuts.length - 1; i++) {
      const start = cuts[i], end = cuts[i + 1], midpoint: Point = [from.x + (to.x - from.x) * (start + end) / 2, from.y + (to.y - from.y) * (start + end) / 2];
      if (!pointInside(midpoint, surface.points) || (surface.holes ?? []).some(hole => pointInside(midpoint, hole))) continue;
      const distance = (end - start) * edge.length, count = Math.max(1, Math.ceil(distance / 0.25)), step = distance / count;
      for (let sample = 0; sample < count; sample++) {
        const ratio = start + (end - start) * (sample + 0.5) / count, position: Point = [from.x + (to.x - from.x) * ratio, from.y + (to.y - from.y) * ratio];
        buildingSamples++;
        if (rings.every(ring => boundaryDistance(position, ring) > 1)) interior += step;
      }
    }
    if (interior > 1.25) buildingPenetrations.push({ edge: edge.id, building: surface.sourceId ?? surface.name ?? 'building', interiorMetres: round(interior) });
  }
}
assert.equal(buildingPenetrations.length, 0, `Routes penetrate buildings: ${JSON.stringify(buildingPenetrations)}`);

/** A separate, deliberately simple Dijkstra implementation, one-to-all. */
function referenceDistances(from: string): Map<string, number> {
  const distances = new Map<string, number>([[from, 0]]), visited = new Set<string>(), frontier = new Set<string>([from]);
  while (frontier.size) {
    let id = '', cost = Infinity;
    for (const candidate of frontier) if (distances.get(candidate)! < cost) { id = candidate; cost = distances.get(candidate)!; }
    frontier.delete(id); visited.add(id);
    for (const arc of graph.adjacency.get(id)!) {
      if (graph.edges.get(arc.edgeId)!.kind === 'runway' || visited.has(arc.to)) continue;
      const candidate = cost + arc.length;
      if (candidate < (distances.get(arc.to) ?? Infinity)) { distances.set(arc.to, candidate); frontier.add(arc.to); }
    }
  }
  return distances;
}
const runways = graph.locations.filter(p => p.kind === 'runway' && p.nodeId);
assert.equal(runways.length, 8);
for (const [west, east] of [['08L', '26R'], ['08R', '26L'], ['09L', '27R'], ['09R', '27L']]) {
  assert.ok(runways.find(p => p.name === west)!.x < runways.find(p => p.name === east)!.x, `${west}/${east}: runway names must match geographical direction, independently of source way point order`);
}
const representativeNames = ['A01', 'C03', 'E16', 'F09', 'I24', 'N41', 'R 11', 'Y06/Y07'];
const representatives = representativeNames.map(name => graph.locations.find(p => p.kind === 'stand' && p.name === name && p.nodeId)).filter(p => p !== undefined);
assert.equal(representatives.length, representativeNames.length);
const routes: { origin: string; destination: string; distanceMetres: number; edges: number; differenceFromDijkstra: number }[] = [];
for (const stand of representatives) {
  const expected = referenceDistances(stand.nodeId!);
  for (const runway of runways) {
    const route = planner.findRoute(stand.nodeId!, runway.nodeId!);
    assert.ok(route, `${stand.name} → ${runway.name}: no taxi route`);
    const difference = Math.abs(route.distance - expected.get(runway.nodeId!)!);
    assert.ok(difference < 1e-5, `${stand.name} → ${runway.name}: A* differs from Dijkstra`);
    assert.ok(route.edgeIds.every(id => graph.edges.get(id)!.kind !== 'runway'));
    routes.push({ origin: stand.name, destination: runway.name, distanceMetres: round(route.distance), edges: route.edgeIds.length, differenceFromDijkstra: round(difference) });
  }
}
const reviewedRoutes: { origin: string; destination: string; differenceMetres: number }[] = [];
const reviewPath = new URL('docs/review-routes.json', root);
if (existsSync(reviewPath)) {
  const reviews = JSON.parse(readFileSync(reviewPath, 'utf8')) as { originId: string; destinationId: string; length: number }[];
  for (const review of reviews) {
    const from = graph.locations.find(p => p.id === review.originId), to = graph.locations.find(p => p.id === review.destinationId);
    assert.ok(from?.nodeId && to?.nodeId);
    const route = planner.findRoute(from.nodeId, to.nodeId); assert.ok(route);
    const difference = Math.abs(route.distance - review.length); assert.ok(difference < 1e-5, `${from.name} → ${to.name}: differs from independently reviewed route`);
    reviewedRoutes.push({ origin: from.name, destination: to.name, differenceMetres: round(difference) });
  }
}

const scenarios = [];
for (const count of [10, 25, 50, 100]) {
  const simulator = new AircraftSimulator(graph), detector = new ConflictDetector(graph), generationStarted = performance.now();
  assert.equal(simulator.generate(count, 20261112), count);
  const generationMs = performance.now() - generationStarted;
  const predictionTimes: number[] = [], conflictsByType: Record<string, number> = {};
  let maxConflicts = 0, positionChecks = 0;
  const motionStarted = performance.now();
  for (let step = 0; step < 600; step++) {
    simulator.update(0.25);
    for (const aircraft of simulator.aircraft) {
      assert.ok(aircraft.distance >= 0 && aircraft.distance <= aircraft.route.distance);
      const from = graph.nodes.get(aircraft.fromNodeId)!, to = graph.nodes.get(aircraft.toNodeId)!;
      assert.ok(Math.abs(aircraft.x - from.x - (to.x - from.x) * aircraft.edgeProgress) < 1e-6);
      assert.ok(Math.abs(aircraft.y - from.y - (to.y - from.y) * aircraft.edgeProgress) < 1e-6);
      assert.ok(aircraft.edgeId && graph.edges.get(aircraft.edgeId)!.kind !== 'runway');
      positionChecks++;
    }
    if (step % 20 === 0) {
      const predictionStarted = performance.now(), conflicts = detector.detect(simulator.aircraft, 120);
      predictionTimes.push(performance.now() - predictionStarted);
      maxConflicts = Math.max(maxConflicts, conflicts.length);
      for (const conflict of conflicts) { assert.ok(conflict.timeToConflict >= 0 && conflict.timeToConflict <= 120); conflictsByType[conflict.type] = (conflictsByType[conflict.type] ?? 0) + 1; }
    }
  }
  scenarios.push({ aircraft: count, seed: 20261112, seconds: simulator.time, stepSeconds: 0.25, positionChecks, arrived: simulator.aircraft.filter(a => a.state === 'arrived').length, maxConflictPairs: maxConflicts, conflictObservationsByType: conflictsByType, generationMs: round(generationMs), motionAndPredictionMs: round(performance.now() - motionStarted), predictionMeanMs: round(predictionTimes.reduce((a, b) => a + b, 0) / predictionTimes.length), predictionMaxMs: round(Math.max(...predictionTimes)) });
}

const report = {
  airport: 'LFPG', dataSha256: createHash('sha256').update(bytes).digest('hex'),
  units: { distance: 'metres', time: 'simulation seconds', benchmark: 'wall-clock milliseconds' },
  topology: { nodes: graph.nodes.size, edges: graph.edges.size, components: graph.components.map((c, id) => ({ id, nodes: c.length })), primaryComponentIsZero: true, edgeKinds: Object.fromEntries(['taxiway', 'parking_position', 'runway'].map(kind => [kind, edgeList.filter(e => e.kind === kind).length])), maxEdgeLengthErrorMetres: maxLengthError, sourceCounts: data.metadata.counts },
  geometry: { intersectionCandidatePairs: candidatePairs, unresolvedSameLayerIntersections: unresolved, intentionallySeparatedLayerCrossings: layerSeparated, buildingPolygons: buildings.length, buildingCandidatePairs, buildingSamples, buildingInteriorPenetrations: buildingPenetrations, buildingCheck: 'Independent point-in-polygon and distance-to-boundary samples at <=0.25m along exact inside intervals; fails >1.25m of segment >1m inside a building. Thin sub-sample penetrations remain below this check resolution.' },
  routes, reviewedRoutes, scenarios,
  interpretation: ['Conflicts are predictions under constant taxi speed and the manually selected hold state; no ATC clearances or automatic conflict resolution.', '45m edge separation and 30m node clearance are simplified simulator parameters, not operational separation standards.', 'Disconnected components, unavailable stands and layer-separated crossings remain explicit; validation does not invent geometry.'],
  totalValidationMs: round(performance.now() - started),
};
mkdirSync(new URL('docs/', root), { recursive: true });
writeFileSync(new URL('docs/validation-results.json', root), `${JSON.stringify(report, null, 2)}\n`);
console.info(`LFPG validated: ${graph.nodes.size} nodes, ${graph.edges.size} edges, ${graph.components.length} components; ${routes.length} A*/Dijkstra route comparisons.`);
console.info(`Geometry: ${unresolved.length} unresolved same-layer intersections; ${buildingPenetrations.length} building penetrations; ${buildingSamples} independent building samples.`);
for (const scenario of scenarios) console.info(`${scenario.aircraft} aircraft: ${scenario.positionChecks} positions; generate ${scenario.generationMs}ms, prediction mean/max ${scenario.predictionMeanMs}/${scenario.predictionMaxMs}ms.`);
console.info('Report: docs/validation-results.json');
