import { TaxiGraph } from './TaxiGraph';
import type { AircraftObservation, Route, TaxiEdge } from './types';

/** Adapter epochs are monotonic seconds, not render timestamps. observedAt may
 * be a local receipt epoch when the source does not expose measurement time. */
export interface SurfaceSample {
  aircraftId: string; observedAt: number; x: number; y: number;
  headingTrueDegrees?: number; groundSpeedKnots?: number; receivedAt?: number;
  positionAccuracyMetres?: number;
}
export interface SurfaceTrackingContext {
  clearedClearanceIds?: readonly string[]; minimumAssignedNodeIndex?: number;
}
export interface SurfaceTrackerOptions {
  expectedIntervalSeconds?: number; coastSeconds?: number; lostSeconds?: number;
  confirmationSeconds?: number; evidenceIntervalSeconds?: number;
  positionAccuracyMetres?: number; matchRadiusMetres?: number;
  maximumSurfaceSpeedMetresPerSecond?: number;
}
export type SurfaceTrackStatus = 'tracking' | 'ambiguous' | 'suspected-off-track' | 'confirmed-off-track' | 'stale' | 'lost';
export type SurfaceDeviationKind = 'wrong-branch' | 'wrong-direction' | 'runway-hold' | 'off-network';
export interface SurfaceTrackResult {
  aircraftId: string; accepted: boolean; isNewObservation: boolean; rejectionReason?: string;
  sourceObservedAt: number; ageSeconds: number;
  freshness: 'fresh' | 'coasting' | 'stale' | 'lost'; status: SurfaceTrackStatus;
  confidence: number; positionConfidence?: number; uncertaintyMetres: number; speedUncertaintyMetresPerSecond: number;
  x: number; y: number; /** Radians counterclockwise from east, as in Aircraft. */ heading: number;
  bodyHeading?: number;
  speedMetresPerSecond: number; observation?: AircraftObservation; confirmedObservation?: AircraftObservation;
  routeDistance?: number; observedRouteDistance?: number; evidenceCount: number;
  deviationKind?: SurfaceDeviationKind; acceptedSamples: number;
}
interface Match {
  observation: AircraftObservation; x: number; y: number; heading: number; distance: number; score: number;
  routeDistance?: number; opposite: boolean; ambiguous: boolean;
}
interface Track {
  sample: SurfaceSample; route: Route; context: SurfaceTrackingContext; match?: Match;
  speed: number; accuracy: number; confidence: number; routeDistance?: number;
  status: SurfaceTrackStatus; kind?: SurfaceDeviationKind; count: number; firstEvidenceAt: number;
  lastEvidenceAt: number; normalCount: number; acceptedSamples: number;
}
const KNOT = 0.514444;
const clamp = (x: number, lo: number, hi: number) => Math.max(lo, Math.min(hi, x));
const angularDifference = (a: number, b: number) => Math.abs(Math.atan2(Math.sin(a - b), Math.cos(a - b)));

/** Sparse surface observations and conformance evidence. Motion estimates do
 * not create sensor reports or increment alert evidence. Operational limits
 * constrain projected motion, never the matching of an actually wrong turn. */
export class SurfaceTracker {
  private tracks = new Map<string, Track>();
  private cells = new Map<string, Set<string>>();
  readonly options: Required<SurfaceTrackerOptions>;

  constructor(readonly graph: TaxiGraph, options: SurfaceTrackerOptions = {}) {
    this.options = { expectedIntervalSeconds: 5, coastSeconds: 10, lostSeconds: 20,
      confirmationSeconds: 5, evidenceIntervalSeconds: 3, positionAccuracyMetres: 6,
      matchRadiusMetres: 45, maximumSurfaceSpeedMetresPerSecond: 45, ...options };
    if (Object.values(this.options).some(value => !Number.isFinite(value) || value <= 0)
      || this.options.lostSeconds < this.options.coastSeconds) throw new Error('Invalid surface tracker options');
    for (const edge of graph.edges.values()) {
      const from = graph.nodes.get(edge.from)!, to = graph.nodes.get(edge.to)!;
      for (let x = Math.floor(Math.min(from.x, to.x) / 100); x <= Math.floor(Math.max(from.x, to.x) / 100); x++)
        for (let y = Math.floor(Math.min(from.y, to.y) / 100); y <= Math.floor(Math.max(from.y, to.y) / 100); y++) {
          const key = `${x},${y}`, cell = this.cells.get(key) ?? new Set<string>(); cell.add(edge.id); this.cells.set(key, cell);
        }
    }
  }

  clear(): void { this.tracks.clear(); }
  remove(aircraftId: string): void { this.tracks.delete(aircraftId); }

  /** An ATC assignment is new intent, not a new sensor measurement. Preserve
   * the last receipt epoch, position and report count while remapping it. */
  setReferenceRoute(aircraftId: string, route: Route, context: SurfaceTrackingContext = {}): void {
    const track = this.tracks.get(aircraftId); if (!track) return;
    track.route = route; track.context = { ...context };
    const observation = track.match?.observation;
    const index = observation ? route.edgeIds.findIndex((id, i) => id === observation.edgeId && route.nodeIds[i] === observation.fromNodeId && route.nodeIds[i + 1] === observation.toNodeId) : -1;
    const distance = index >= 0 && observation ? route.cumulativeDistances[index] + observation.edgeProgress * this.graph.edges.get(observation.edgeId)!.length : undefined;
    if (track.match) track.match = { ...track.match, routeDistance: distance, opposite: false, ambiguous: track.match.ambiguous || distance === undefined };
    track.routeDistance = distance; track.count = 0; track.normalCount = 0; track.kind = undefined;
    track.firstEvidenceAt = track.sample.observedAt; track.lastEvidenceAt = track.sample.observedAt;
    track.status = track.match && !track.match.ambiguous && distance !== undefined ? 'tracking' : 'ambiguous';
  }

  ingest(sample: SurfaceSample, route: Route, now = sample.receivedAt ?? sample.observedAt, context: SurfaceTrackingContext = {}): SurfaceTrackResult {
    this.validateSample(sample, now);
    const previous = this.tracks.get(sample.aircraftId);
    const reject = (reason: string): SurfaceTrackResult => {
      const result = previous ? this.estimate(sample.aircraftId, now)! : this.unmatchedResult(sample, now);
      return { ...result, accepted: false, isNewObservation: false, rejectionReason: reason };
    };
    if (sample.observedAt > now + .5) return reject('future-observation');
    if (now - sample.observedAt > this.options.lostSeconds) return reject('expired-observation');
    if (previous && sample.observedAt <= previous.sample.observedAt) return reject('duplicate-or-out-of-order');
    const accuracy = clamp(sample.positionAccuracyMetres ?? this.options.positionAccuracyMetres, 1, 30);
    const dt = previous ? sample.observedAt - previous.sample.observedAt : 0;
    const displacement = previous ? Math.hypot(sample.x - previous.sample.x, sample.y - previous.sample.y) : 0;
    if (previous && displacement > this.options.maximumSurfaceSpeedMetresPerSecond * dt + 4 * (accuracy + previous.accuracy)) return reject('implausible-position-jump');
    const reportedSpeed = sample.groundSpeedKnots === undefined ? undefined : sample.groundSpeedKnots * KNOT;
    const measuredSpeed = dt > 0 && displacement > Math.max(8, accuracy * 2) ? displacement / dt : undefined;
    const course = measuredSpeed !== undefined && dt <= this.options.coastSeconds + this.options.expectedIntervalSeconds
      ? Math.atan2(sample.y - previous!.sample.y, sample.x - previous!.sample.x) : undefined;
    const rawSpeed = reportedSpeed ?? measuredSpeed ?? previous?.speed ?? 0;
    // Position-derived motion is corroboration, not a second independent speed sensor.
    const observedSpeed = reportedSpeed !== undefined && measuredSpeed !== undefined ? .7 * reportedSpeed + .3 * measuredSpeed : rawSpeed;
    const speed = rawSpeed < .5 && measuredSpeed === undefined ? 0 : previous ? .65 * observedSpeed + .35 * previous.speed : observedSpeed;
    const heading = course ?? (sample.headingTrueDegrees === undefined ? undefined : (90 - sample.headingTrueDegrees) * Math.PI / 180);
    const match = this.match(sample, route, context, previous, dt, speed, heading, accuracy);
    let confidence = match ? clamp(.98 - match.distance / 65 - (match.ambiguous ? .55 : 0), .1, .98) : .1;
    if (sample.receivedAt !== undefined) confidence *= clamp(1 - (sample.receivedAt - sample.observedAt) / 30, .35, 1);
    let kind: SurfaceDeviationKind | undefined;
    const pending = route.operational?.clearanceCheckpoints.find(point => !context.clearedClearanceIds?.includes(point.id));
    if (!match) kind = 'off-network';
    else if (!match.ambiguous) {
      if (match.opposite && speed >= 1.5) kind = 'wrong-direction';
      else if (match.routeDistance === undefined && speed >= 1.5) kind = 'wrong-branch';
      else if (pending && match.routeDistance !== undefined && match.routeDistance > pending.distance + Math.max(8, accuracy * 2)) kind = 'runway-hold';
    }
    // A stopped body heading does not prove reversal; an ambiguous intersection
    // does not prove a wrong branch. Each evidence vote is a distinct report.
    const continuous = previous && dt <= this.options.coastSeconds + this.options.expectedIntervalSeconds && previous.kind === kind;
    let count = kind ? continuous ? previous.count : 1 : 0;
    let firstEvidenceAt = kind ? continuous ? previous.firstEvidenceAt : sample.observedAt : sample.observedAt;
    let lastEvidenceAt = kind ? continuous ? previous.lastEvidenceAt : sample.observedAt : sample.observedAt;
    if (kind && continuous && sample.observedAt - previous.lastEvidenceAt >= this.options.evidenceIntervalSeconds) { count++; lastEvidenceAt = sample.observedAt; }
    let status: SurfaceTrackStatus = match?.ambiguous ? 'ambiguous' : 'tracking';
    if (kind) status = count >= 2 && sample.observedAt - firstEvidenceAt >= this.options.confirmationSeconds ? 'confirmed-off-track' : 'suspected-off-track';
    let normalCount = kind || match?.ambiguous ? 0 : (previous?.normalCount ?? 0) + 1;
    if (!kind && previous?.status === 'confirmed-off-track' && normalCount < 2) status = 'suspected-off-track';
    let routeDistance = match?.routeDistance;
    if (routeDistance !== undefined && previous?.route === route && previous.routeDistance !== undefined && dt <= this.options.coastSeconds) {
      const prediction = previous.routeDistance + previous.speed * dt;
      // A measurement wins over extrapolation; smoothing remains on the same
      // assigned geometry and is bounded to a single accuracy envelope.
      routeDistance = clamp(.75 * routeDistance + .25 * prediction, match!.routeDistance! - accuracy, match!.routeDistance! + accuracy);
    }
    const track: Track = { sample: { ...sample }, route, context: { ...context }, match, speed, accuracy, confidence,
      routeDistance, status, kind, count, firstEvidenceAt, lastEvidenceAt, normalCount, acceptedSamples: (previous?.acceptedSamples ?? 0) + 1 };
    this.tracks.set(sample.aircraftId, track);
    const result = this.estimate(sample.aircraftId, now)!;
    return { ...result, isNewObservation: true,
      ...(status === 'confirmed-off-track' && match ? { confirmedObservation: { ...match.observation } } : {}) };
  }

  estimate(aircraftId: string, now: number): SurfaceTrackResult | null {
    if (!Number.isFinite(now)) throw new Error('Surface estimate time must be finite');
    const track = this.tracks.get(aircraftId); if (!track) return null;
    const age = Math.max(0, now - track.sample.observedAt);
    // No candidate branch is a motion authority while the association is still
    // ambiguous or only suspected. Keep the last measured footprint; neither
    // interpolation nor its smoothed prior may commit the next intersection.
    const uncertainAssociation = track.status === 'ambiguous' || track.status === 'suspected-off-track';
    const motionAge = uncertainAssociation ? 0 : Math.min(age, this.options.coastSeconds);
    const freshness = age > this.options.lostSeconds ? 'lost' : age > this.options.coastSeconds ? 'stale'
      : age > this.options.expectedIntervalSeconds + 2 ? 'coasting' : 'fresh';
    let x = track.match?.x ?? track.sample.x, y = track.match?.y ?? track.sample.y, heading = track.match?.heading ?? 0;
    let observation = track.match?.observation ? { ...track.match.observation } : undefined;
    let routeDistance = uncertainAssociation ? track.match?.routeDistance : track.routeDistance;
    if (routeDistance !== undefined) {
      const pending = track.route.operational?.clearanceCheckpoints.find(point => !track.context.clearedClearanceIds?.includes(point.id));
      const stop = pending ? Math.max(pending.distance, track.match!.routeDistance!) : track.route.distance;
      routeDistance = clamp(routeDistance + track.speed * motionAge, 0, stop);
      const point = this.routePoint(track.route, routeDistance); if (point) { x = point.x; y = point.y; heading = point.heading; observation = point.observation; }
    } else if (track.match) {
      const edge = this.graph.edges.get(track.match.observation.edgeId)!;
      const from = this.graph.nodes.get(track.match.observation.fromNodeId)!, to = this.graph.nodes.get(track.match.observation.toNodeId)!;
      const progress = clamp(track.match.observation.edgeProgress + track.speed * motionAge / edge.length, 0, 1);
      x = from.x + (to.x - from.x) * progress; y = from.y + (to.y - from.y) * progress;
      observation = { ...track.match.observation, edgeProgress: progress };
    }
    const uncertaintyMetres = Math.min(75, track.accuracy + age * (.55 + track.speed * .15) + (!track.match ? 20 : track.match.ambiguous ? 12 : 0));
    const measuredBody = track.sample.headingTrueDegrees === undefined ? undefined : (90 - track.sample.headingTrueDegrees) * Math.PI / 180;
    // Position receipt/accuracy and lane association are different questions.
    // This is a model score, not a covariance supplied by the radar SDK.
    const receiptQuality = track.sample.receivedAt === undefined ? 1 : clamp(1 - (track.sample.receivedAt - track.sample.observedAt) / 30, .2, 1);
    const positionConfidence = clamp((1 - track.accuracy / 80) * Math.max(0, 1 - age / 30) * receiptQuality, 0, 1);
    const rotation = track.match && track.speed > .5 && !uncertainAssociation ? Math.atan2(Math.sin(heading - track.match.heading), Math.cos(heading - track.match.heading)) : 0;
    return { aircraftId, accepted: true, isNewObservation: false, sourceObservedAt: track.sample.observedAt, ageSeconds: age, freshness,
      status: freshness === 'stale' || freshness === 'lost' ? freshness : track.status,
      confidence: clamp(track.confidence * (freshness === 'lost' ? .05 : freshness === 'stale' ? .3 : 1 - age / 60), 0, 1),
      positionConfidence,
      uncertaintyMetres, speedUncertaintyMetresPerSecond: .6 + track.speed * .15 + Math.min(age, 20) * .12,
      x, y, heading, bodyHeading: measuredBody === undefined ? undefined : measuredBody + rotation, speedMetresPerSecond: freshness === 'stale' || freshness === 'lost' ? 0 : track.speed,
      observation, routeDistance, observedRouteDistance: track.match?.routeDistance,
      evidenceCount: track.count, deviationKind: track.kind, acceptedSamples: track.acceptedSamples };
  }

  private validateSample(sample: SurfaceSample, now: number): void {
    if (!sample || typeof sample.aircraftId !== 'string' || !sample.aircraftId || ![sample.observedAt, sample.x, sample.y, now].every(Number.isFinite)
      || sample.observedAt < 0 || sample.receivedAt !== undefined && (!Number.isFinite(sample.receivedAt) || sample.receivedAt < sample.observedAt)
      || sample.headingTrueDegrees !== undefined && (!Number.isFinite(sample.headingTrueDegrees) || sample.headingTrueDegrees < 0 || sample.headingTrueDegrees >= 360)
      || sample.groundSpeedKnots !== undefined && (!Number.isFinite(sample.groundSpeedKnots) || sample.groundSpeedKnots < 0 || sample.groundSpeedKnots * KNOT > this.options.maximumSurfaceSpeedMetresPerSecond)
      || sample.positionAccuracyMetres !== undefined && (!Number.isFinite(sample.positionAccuracyMetres) || sample.positionAccuracyMetres <= 0)) throw new Error('Invalid timestamped surface sample');
  }

  private unmatchedResult(sample: SurfaceSample, now: number): SurfaceTrackResult {
    return { aircraftId: sample.aircraftId, accepted: false, isNewObservation: false, sourceObservedAt: sample.observedAt,
      ageSeconds: Math.max(0, now - sample.observedAt), freshness: 'lost', status: 'lost', confidence: 0,
      uncertaintyMetres: 75, speedUncertaintyMetresPerSecond: 3, x: sample.x, y: sample.y, heading: 0,
      speedMetresPerSecond: 0, evidenceCount: 0, acceptedSamples: 0 };
  }

  private match(sample: SurfaceSample, route: Route, context: SurfaceTrackingContext, previous: Track | undefined, dt: number, speed: number, heading: number | undefined, accuracy: number): Match | undefined {
    const radius = Math.max(this.options.matchRadiusMetres, accuracy * 3), ids = new Set<string>();
    for (let x = Math.floor((sample.x - radius) / 100); x <= Math.floor((sample.x + radius) / 100); x++)
      for (let y = Math.floor((sample.y - radius) / 100); y <= Math.floor((sample.y + radius) / 100); y++) for (const id of this.cells.get(`${x},${y}`) ?? []) ids.add(id);
    const candidates: Match[] = [], firstNode = context.minimumAssignedNodeIndex ?? 0;
    for (const id of ids) {
      const edge = this.graph.edges.get(id)!, a = this.graph.nodes.get(edge.from)!, b = this.graph.nodes.get(edge.to)!;
      const dx = b.x - a.x, dy = b.y - a.y, denominator = dx * dx + dy * dy;
      const fraction = clamp(((sample.x - a.x) * dx + (sample.y - a.y) * dy) / denominator, 0, 1);
      const x = a.x + fraction * dx, y = a.y + fraction * dy, distance = Math.hypot(sample.x - x, sample.y - y);
      if (distance > radius) continue;
      for (const reverse of [false, true]) {
        const fromNodeId = reverse ? edge.to : edge.from, toNodeId = reverse ? edge.from : edge.to, progress = reverse ? 1 - fraction : fraction;
        const direction = Math.atan2(reverse ? -dy : dy, reverse ? -dx : dx);
        const routeIndex = route.edgeIds.findIndex((routeId, i) => i >= firstNode && routeId === id && route.nodeIds[i] === fromNodeId && route.nodeIds[i + 1] === toNodeId);
        const opposite = routeIndex < 0 && route.edgeIds.some((routeId, i) => i >= firstNode && routeId === id && route.nodeIds[i] === toNodeId && route.nodeIds[i + 1] === fromNodeId);
        const routeDistance = routeIndex < 0 ? undefined : route.cumulativeDistances[routeIndex] + progress * edge.length;
        const headingPenalty = heading !== undefined && speed >= 1.5 ? 16 * angularDifference(heading, direction) / Math.PI : 0;
        let continuity = 0;
        if (routeDistance !== undefined && previous?.route === route && previous.routeDistance !== undefined && dt <= this.options.coastSeconds + this.options.expectedIntervalSeconds) {
          continuity = Math.min(8, Math.abs(routeDistance - previous.routeDistance - previous.speed * dt) * .06);
        }
        // One metre of assignment preference resolves coincident stop headings,
        // but cannot hide a geometrically clear departure onto another branch.
        const score = distance + headingPenalty + continuity - (routeIndex >= 0 ? 1 : 0);
        candidates.push({ observation: { edgeId: id, fromNodeId, toNodeId, edgeProgress: progress }, x, y, heading: direction, distance, score, routeDistance, opposite, ambiguous: false });
      }
    }
    candidates.sort((a, b) => a.score - b.score || a.observation.edgeId.localeCompare(b.observation.edgeId));
    const best = candidates[0]; if (!best) return undefined;
    const corridor = new Set<string>([best.observation.edgeId]), queue: { edgeId: string; distance: number }[] = [{ edgeId: best.observation.edgeId, distance: 0 }];
    for (let i = 0; i < queue.length && i < 128; i++) {
      const current = this.graph.edges.get(queue[i].edgeId)!;
      for (const node of [current.from, current.to]) {
        const adjacent = this.graph.adjacency.get(node) ?? []; if (adjacent.length !== 2) continue;
        for (const arc of adjacent) {
          if (corridor.has(arc.edgeId)) continue;
          const edge = this.graph.edges.get(arc.edgeId)!, a = this.graph.nodes.get(edge.from)!, b = this.graph.nodes.get(edge.to)!;
          const angle = angularDifference(Math.atan2(b.y - a.y, b.x - a.x), best.heading);
          if (Math.min(angle, Math.PI - angle) > Math.PI * 35 / 180) continue;
          corridor.add(edge.id);
          const distance = queue[i].distance + edge.length;
          if (distance < Math.max(15, accuracy * 3)) queue.push({ edgeId: edge.id, distance });
        }
      }
    }
    const alternate = candidates.find(candidate => {
      if (corridor.has(candidate.observation.edgeId) || Math.hypot(candidate.x - best.x, candidate.y - best.y) <= .5) return false;
      // Consecutive vertices of one curved lane are one physical hypothesis.
      // Parallel disconnected lanes and two outgoing branches at a fork remain
      // alternatives, even if both have a similar bearing.
      const contiguous = candidate.observation.toNodeId === best.observation.fromNodeId
        || best.observation.toNodeId === candidate.observation.fromNodeId;
      return !contiguous || angularDifference(candidate.heading, best.heading) > Math.PI * 35 / 180;
    });
    if (alternate && alternate.score - best.score < Math.max(4, accuracy * .7)) best.ambiguous = true;
    // Near a branch point the next outgoing arc is not established yet.
    const nearNode = Math.min(best.observation.edgeProgress, 1 - best.observation.edgeProgress) * this.graph.edges.get(best.observation.edgeId)!.length;
    const nearestNode = best.observation.edgeProgress < .5 ? best.observation.fromNodeId : best.observation.toNodeId;
    if (best.routeDistance === undefined && nearNode < Math.max(8, accuracy * 1.5) && (this.graph.adjacency.get(nearestNode)?.length ?? 0) > 2) best.ambiguous = true;
    if (speed < 1.5 && best.routeDistance === undefined) best.ambiguous = true;
    return best;
  }

  private routePoint(route: Route, distance: number): { x: number; y: number; heading: number; observation: AircraftObservation } | undefined {
    if (!route.edgeIds.length) return undefined;
    let i = 0; while (i < route.edgeIds.length - 1 && route.cumulativeDistances[i + 1] <= distance) i++;
    const edge: TaxiEdge = this.graph.edges.get(route.edgeIds[i])!, from = this.graph.nodes.get(route.nodeIds[i])!, to = this.graph.nodes.get(route.nodeIds[i + 1])!;
    const progress = clamp((distance - route.cumulativeDistances[i]) / edge.length, 0, 1);
    return { x: from.x + (to.x - from.x) * progress, y: from.y + (to.y - from.y) * progress,
      heading: Math.atan2(to.y - from.y, to.x - from.x), observation: { edgeId: edge.id, fromNodeId: from.id, toNodeId: to.id, edgeProgress: progress } };
  }
}
