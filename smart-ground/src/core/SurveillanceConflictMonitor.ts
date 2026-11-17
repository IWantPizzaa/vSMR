import { ConflictDetector } from './ConflictDetector';
import { PhysicalConflictDetector, type PhysicalConflict } from './PhysicalConflictDetector';
import type { TaxiGraph } from './TaxiGraph';
import type { Aircraft, AircraftObservation, Conflict } from './types';

/** Source time is the time of a real radar report, never the render/capture tick. */
export interface SurveillanceTrackQuality {
  aircraftId: string;
  sourceObservedAt: number;
  ageSeconds: number;
  freshness: 'fresh' | 'coasting' | 'stale' | 'lost';
  status: 'tracking' | 'ambiguous' | 'suspected-off-track' | 'confirmed-off-track' | 'stale' | 'lost';
  confidence: number;
  /** Confidence in the raw position, independent of association with a taxiway. */
  positionConfidence?: number;
  uncertaintyMetres: number;
  speedUncertaintyMetresPerSecond: number;
  /** Optional matched/raw estimate, used for an uncertain current footprint. */
  observation?: AircraftObservation;
  speedMetresPerSecond?: number;
  x?: number;
  y?: number;
}

export interface SurveillanceConflict extends Conflict {
  envelope?: PhysicalConflict['envelope'];
  quality: 'confirmed' | 'degraded';
  confidence: number;
  firstDetectedAt: number;
  lastObservedAt: number;
  timeToConflictRange: [number, number];
  reason: string;
}

export interface SurveillanceConflictOptions {
  physicalEnvelopes?: boolean;
  physicalClearanceMetres?: number;
  physicalReleaseClearanceMetres?: number;
  separationMetres?: number;
  releaseSeparationMetres?: number;
  confirmationReports?: number;
  confirmationSeconds?: number;
  clearReports?: number;
  clearSeconds?: number;
  urgentSeconds?: number;
  reliableConfidence?: number;
  staleAfterSeconds?: number;
  lostAfterSeconds?: number;
}

type Quality = SurveillanceTrackQuality & { age: number; reliable: boolean; spatialOnly: boolean; positionReliable: boolean; candidateConfidence: number; limitedHorizon: number };
interface PairState {
  firstDetectedAt: number;
  firstEvidenceAt: number;
  evidenceCount: number;
  lastTimes: [number, number];
  lastPositiveSourceAt: number;
  clearCount: number;
  clearSince: number | null;
  alert?: SurveillanceConflict;
}

/**
 * The exact graph detector remains the geometric reference. This layer adds
 * source-report confirmation and release hysteresis. Uncertainty widens the
 * displayed time interval; it never creates an enormous collision radius.
 * All thresholds are simulator tuning parameters, not ATC separation minima.
 */
export class SurveillanceConflictMonitor {
  private readonly detector: Pick<ConflictDetector, 'detect'>;
  private readonly releaseDetector: Pick<ConflictDetector, 'detect'>;
  private readonly pairs = new Map<string, PairState>();
  private lastNow = -Infinity;
  readonly options: Required<Omit<SurveillanceConflictOptions, 'physicalEnvelopes' | 'physicalClearanceMetres' | 'physicalReleaseClearanceMetres'>>;

  constructor(readonly graph: TaxiGraph, options: SurveillanceConflictOptions = {}) {
    this.options = {
      separationMetres: options.separationMetres ?? 45,
      releaseSeparationMetres: options.releaseSeparationMetres ?? 60,
      confirmationReports: options.confirmationReports ?? 2,
      confirmationSeconds: options.confirmationSeconds ?? 5,
      clearReports: options.clearReports ?? 2,
      clearSeconds: options.clearSeconds ?? 5,
      urgentSeconds: options.urgentSeconds ?? 15,
      reliableConfidence: options.reliableConfidence ?? .65,
      staleAfterSeconds: options.staleAfterSeconds ?? 10,
      lostAfterSeconds: options.lostAfterSeconds ?? 20,
    };
    if (Object.values(this.options).some(value => !Number.isFinite(value) || value < 0)
      || this.options.separationMetres <= 0 || this.options.releaseSeparationMetres < this.options.separationMetres
      || !Number.isInteger(this.options.confirmationReports) || this.options.confirmationReports < 1
      || !Number.isInteger(this.options.clearReports) || this.options.clearReports < 1
      || this.options.reliableConfidence > 1 || this.options.lostAfterSeconds <= this.options.staleAfterSeconds) throw new Error('Invalid surveillance conflict tuning');
    if (options.physicalEnvelopes) {
      const clearance = options.physicalClearanceMetres ?? 2, release = options.physicalReleaseClearanceMetres ?? 5;
      if (!Number.isFinite(release) || release < clearance) throw new Error('Release footprint clearance cannot be smaller than activation clearance');
      this.detector = new PhysicalConflictDetector(graph, { clearanceMetres: clearance });
      this.releaseDetector = new PhysicalConflictDetector(graph, { clearanceMetres: release });
    } else {
      this.detector = new ConflictDetector(graph, this.options.separationMetres);
      this.releaseDetector = new ConflictDetector(graph, this.options.releaseSeparationMetres);
    }
  }

  reset(): void { this.pairs.clear(); this.lastNow = -Infinity; }
  invalidateAircraft(id: string): void { for (const key of this.pairs.keys()) if (key.split('|').includes(id)) this.pairs.delete(key); }

  detect(aircraft: readonly Aircraft[], tracks: readonly SurveillanceTrackQuality[], now: number, horizon = 120): SurveillanceConflict[] {
    if (!Number.isFinite(now) || now < 0 || !Number.isFinite(horizon) || horizon < 0) throw new Error('Surveillance times must be finite and nonnegative');
    if (now < this.lastNow) this.reset(); // A new simulation session is a new source-time domain.
    this.lastNow = now;
    const quality = new Map<string, Quality>();
    for (const track of tracks) {
      if (!track.aircraftId || !Number.isFinite(track.sourceObservedAt) || track.sourceObservedAt > now + .25
        || !Number.isFinite(track.ageSeconds) || track.ageSeconds < 0 || !Number.isFinite(track.confidence)
        || track.confidence < 0 || track.confidence > 1 || !Number.isFinite(track.uncertaintyMetres) || track.uncertaintyMetres < 0
        || track.positionConfidence !== undefined && (!Number.isFinite(track.positionConfidence) || track.positionConfidence < 0 || track.positionConfidence > 1)
        || !Number.isFinite(track.speedUncertaintyMetresPerSecond) || track.speedUncertaintyMetresPerSecond < 0) continue;
      const age = Math.max(track.ageSeconds, now - track.sourceObservedAt);
      if (age > this.options.lostAfterSeconds || track.freshness === 'lost' || track.status === 'lost') continue;
      const unreliableGeometry = track.status === 'ambiguous' || track.status === 'suspected-off-track';
      const spatialOnly = !track.observation && Number.isFinite(track.x) && Number.isFinite(track.y) && track.positionConfidence !== undefined;
      // A failed map association does not invalidate a fresh raw position.
      // This allows only a degraded, short stationary footprint; it does not
      // restore a guessed route, body movement or high association confidence.
      const positionReliable = spatialOnly && age <= Math.min(7, this.options.staleAfterSeconds) && track.freshness === 'fresh'
        && track.status !== 'stale' && track.positionConfidence! >= this.options.reliableConfidence && track.uncertaintyMetres <= 30;
      const reliable = !spatialOnly && !unreliableGeometry && age <= this.options.staleAfterSeconds && track.freshness !== 'stale'
        && track.status !== 'stale' && track.confidence >= this.options.reliableConfidence;
      quality.set(track.aircraftId, { ...track, age, reliable, spatialOnly, positionReliable,
        candidateConfidence: positionReliable ? track.positionConfidence! : track.confidence,
        limitedHorizon: spatialOnly || age > this.options.staleAfterSeconds || track.freshness === 'stale' || track.status === 'stale' ? Math.min(5, horizon) : unreliableGeometry || track.status === 'confirmed-off-track' ? Math.min(10, horizon) : horizon });
    }
    const active = aircraft.filter(a => a.state !== 'scheduled' && quality.has(a.id));
    const byId = new Map(active.map(a => [a.id, a]));
    const projected = active.map(a => {
      const track = quality.get(a.id)!;
      if (track.reliable && track.status !== 'confirmed-off-track') return a;
      // An ambiguous/old target cannot be projected along a confidently chosen
      // assignment. Retain only its current observed graph footprint.
      const observed = track.observation, edge = observed && this.graph.edges.get(observed.edgeId);
      if (!observed || !edge || !((edge.from === observed.fromNodeId && edge.to === observed.toNodeId) || (edge.to === observed.fromNodeId && edge.from === observed.toNodeId))
        || !Number.isFinite(observed.edgeProgress) || observed.edgeProgress < 0 || observed.edgeProgress > 1) {
        const raw = Number.isFinite(track.x) && Number.isFinite(track.y);
        return { ...a, state: 'holding' as const, ...(raw ? { edgeId: null, x: track.x!, y: track.y! } : {}) };
      }
      const from = this.graph.nodes.get(observed.fromNodeId)!, to = this.graph.nodes.get(observed.toNodeId)!;
      if (track.reliable && track.status === 'confirmed-off-track' && Number.isFinite(track.speedMetresPerSecond) && track.speedMetresPerSecond! > .5) {
        // A planned hold is not proof that an observed real aircraft stopped.
        // Forecast measured movement only to this arc's next node; no next
        // turn, route recovery acceptance or runway permission is invented.
        return { ...a, state: 'taxiing' as const, speed: Math.min(30, track.speedMetresPerSecond!),
          route: { nodeIds: [from.id, to.id], edgeIds: [edge.id], cumulativeDistances: [0, edge.length], distance: edge.length },
          distance: edge.length * observed.edgeProgress, conformance: undefined, waitingClearanceId: undefined, clearedClearanceIds: undefined, yielding: undefined,
          edgeId: edge.id, fromNodeId: from.id, toNodeId: to.id, edgeProgress: observed.edgeProgress,
          x: from.x + (to.x - from.x) * observed.edgeProgress, y: from.y + (to.y - from.y) * observed.edgeProgress };
      }
      return { ...a, state: 'holding' as const, edgeId: observed.edgeId, fromNodeId: observed.fromNodeId, toNodeId: observed.toNodeId, edgeProgress: observed.edgeProgress,
        x: from.x + (to.x - from.x) * observed.edgeProgress, y: from.y + (to.y - from.y) * observed.edgeProgress };
    });
    const candidates = this.candidates(this.detector.detect(projected, horizon), quality);
    const releasing = this.candidates(this.releaseDetector.detect(projected, horizon), quality);
    const keys = new Set([...this.pairs.keys(), ...candidates.keys()]);
    const alerts: SurveillanceConflict[] = [];
    for (const key of keys) {
      const ids = key.split('|') as [string, string], first = byId.get(ids[0]), second = byId.get(ids[1]);
      if (!first || !second) { this.pairs.delete(key); continue; }
      const a = quality.get(first.id)!, b = quality.get(second.id)!, times: [number, number] = [a.sourceObservedAt, b.sourceObservedAt];
      const sourceAt = Math.max(...times), raw = candidates.get(key), band = releasing.get(key);
      let state = this.pairs.get(key);
      if (!state && !raw) continue;
      if (!state) {
        state = { firstDetectedAt: now, firstEvidenceAt: sourceAt, evidenceCount: 0, lastTimes: [-Infinity, -Infinity], lastPositiveSourceAt: sourceAt, clearCount: 0, clearSince: null };
        this.pairs.set(key, state);
      }
      const newReport = times.every((time, i) => time >= state!.lastTimes[i]) && times.some((time, i) => time > state!.lastTimes[i]);
      if (raw) {
        if (newReport) { state.evidenceCount++; state.lastTimes = times; state.lastPositiveSourceAt = sourceAt; }
        state.clearCount = 0; state.clearSince = null;
        const reliable = a.reliable && b.reliable;
        const envelope = (raw as Partial<PhysicalConflict>).envelope;
        const immediateSpatial = (a.spatialOnly || b.spatialOnly) && (a.reliable || a.positionReliable) && (b.reliable || b.positionReliable)
          && raw.timeToConflict <= 1e-8 && envelope?.timeToEnvelopeOverlap !== null && envelope?.timeToEnvelopeOverlap !== undefined && envelope.timeToEnvelopeOverlap <= 1e-8;
        const immediate = reliable && raw.timeToConflict <= this.options.urgentSeconds || immediateSpatial;
        const confirmed = state.evidenceCount >= this.options.confirmationReports && sourceAt - state.firstEvidenceAt >= this.options.confirmationSeconds;
        if (state.alert || immediate || confirmed) {
          state.alert = this.describe(raw, first, second, a, b, now, state, reliable ? 'confirmed' : 'degraded', immediateSpatial ? 'Empreintes observées proches hors réseau : association incertaine, aucune trajectoire déduite.' : immediate ? 'Menace proche sur une trajectoire étayée.' : reliable ? 'Conflit confirmé par des mises à jour radar indépendantes.' : 'Position ancienne ou ambiguë : occupation actuelle seulement, prévision limitée.');
        }
      } else if (state.alert) {
        if (band) { state.clearCount = 0; state.clearSince = null; }
        else if (newReport && (a.reliable || a.positionReliable) && (b.reliable || b.positionReliable)) {
          state.clearCount++; state.clearSince ??= sourceAt; state.lastTimes = times;
        }
        if (state.clearCount >= this.options.clearReports && state.clearSince !== null && sourceAt - state.clearSince >= this.options.clearSeconds) { this.pairs.delete(key); continue; }
        const degraded = !a.reliable || !b.reliable;
        const previous = state.alert, elapsed = Math.max(0, now - this.lastAlertUpdate(previous));
        state.alert = { ...previous, timeToConflict: Math.max(0, previous.timeToConflict - elapsed),
          timeToConflictRange: [Math.max(0, previous.timeToConflictRange[0] - elapsed), Math.max(0, previous.timeToConflictRange[1] - elapsed)],
          quality: degraded ? 'degraded' : previous.quality, confidence: Math.min(a.confidence, b.confidence),
          severity: degraded ? 'warning' : previous.severity, lastObservedAt: sourceAt,
          reason: degraded ? 'Association aux voies incertaine ou rapport ancien : alerte conservée jusqu’à confirmation de l’écartement.' : 'Alerte maintenue jusqu’à confirmation de l’écartement.' };
        this.alertUpdatedAt.set(state.alert, now);
      } else if (newReport) {
        // A disappearing single-report candidate must not combine with a later
        // unrelated fluctuation to manufacture a confirmed threat.
        this.pairs.delete(key); continue;
      }
      if (state.alert) alerts.push(state.alert);
    }
    return alerts.sort((a, b) => a.timeToConflict - b.timeToConflict || a.id.localeCompare(b.id));
  }

  private readonly alertUpdatedAt = new WeakMap<SurveillanceConflict, number>();
  private lastAlertUpdate(alert: SurveillanceConflict): number { return this.alertUpdatedAt.get(alert) ?? alert.firstDetectedAt; }

  private candidates(conflicts: Conflict[], quality: Map<string, Quality>): Map<string, Conflict> {
    return new Map(conflicts.filter(conflict => {
      const a = quality.get(conflict.aircraftIds[0]), b = quality.get(conflict.aircraftIds[1]);
      return a && b && conflict.timeToConflict <= Math.min(a.limitedHorizon, b.limitedHorizon) && Math.min(a.candidateConfidence, b.candidateConfidence) >= .25;
    }).map(conflict => [conflict.aircraftIds.slice().sort().join('|'), conflict]));
  }

  private describe(raw: Conflict, first: Aircraft, second: Aircraft, a: Quality, b: Quality, now: number, state: PairState, quality: SurveillanceConflict['quality'], reason: string): SurveillanceConflict {
    const velocity = (aircraft: Aircraft) => aircraft.state === 'taxiing' && aircraft.conformance?.status !== 'unresolved' && !aircraft.conformance?.awaitingApproval
      ? [Math.cos(aircraft.heading) * aircraft.speed, Math.sin(aircraft.heading) * aircraft.speed] : [0, 0];
    const va = velocity(first), vb = velocity(second), relativeSpeed = Math.max(1, Math.hypot(va[0] - vb[0], va[1] - vb[1]));
    const timingMargin = Math.min(12, (Math.min(40, a.uncertaintyMetres + b.uncertaintyMetres)
      + Math.min(30, raw.timeToConflict) * Math.min(4, a.speedUncertaintyMetresPerSecond + b.speedUncertaintyMetresPerSecond)) / relativeSpeed);
    const alert: SurveillanceConflict = { ...raw, id: `surveillance:${raw.aircraftIds.slice().sort().join('|')}`,
      severity: quality === 'degraded' ? 'warning' : raw.severity, quality, confidence: Math.min(a.confidence, b.confidence), firstDetectedAt: state.firstDetectedAt,
      lastObservedAt: Math.max(a.sourceObservedAt, b.sourceObservedAt), timeToConflictRange: [Math.max(0, raw.timeToConflict - timingMargin), raw.timeToConflict + timingMargin], reason };
    this.alertUpdatedAt.set(alert, now); return alert;
  }
}
