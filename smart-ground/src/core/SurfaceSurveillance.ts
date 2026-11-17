import { TaxiGraph } from './TaxiGraph';
import { SurfaceTracker, type SurfaceSample, type SurfaceTrackResult, type SurfaceTrackerOptions } from './SurfaceTracker';
import { SurveillanceConflictMonitor, type SurveillanceConflict, type SurveillanceTrackQuality } from './SurveillanceConflictMonitor';
import type { Aircraft, AircraftObservation, Route } from './types';

export interface SurfaceSurveillanceOptions {
  reportIntervalSeconds?: number;
  simulatedNoiseMetres?: number;
  tracker?: SurfaceTrackerOptions;
  onConfirmedDeviation?: (aircraft: Aircraft, observation: AircraftObservation, result: SurfaceTrackResult) => void;
}

/** Boundary between independent reports and continuously rendered estimates.
 * Simulation truth is a sample source, never extra evidence between reports.
 * A native adapter can call ingest() instead of captureSimulation(). */
export class SurfaceSurveillance {
  readonly tracker: SurfaceTracker;
  readonly conflicts: SurveillanceConflictMonitor;
  readonly reportIntervalSeconds: number;
  readonly simulatedNoiseMetres: number;
  private sources = new Map<string, Aircraft>();
  private lastSimulatedReports = new Map<string, number>();
  private confirmedIncidents = new Set<string>();
  private lastTime = -Infinity;

  constructor(readonly graph: TaxiGraph, readonly options: SurfaceSurveillanceOptions = {}) {
    this.reportIntervalSeconds = options.reportIntervalSeconds ?? 5;
    this.simulatedNoiseMetres = options.simulatedNoiseMetres ?? 2;
    if (!Number.isFinite(this.reportIntervalSeconds) || this.reportIntervalSeconds <= 0 || !Number.isFinite(this.simulatedNoiseMetres) || this.simulatedNoiseMetres < 0) throw new Error('Invalid surveillance sampling options');
    this.tracker = new SurfaceTracker(graph, { expectedIntervalSeconds: this.reportIntervalSeconds, ...options.tracker });
    this.conflicts = new SurveillanceConflictMonitor(graph, { physicalEnvelopes: true, physicalClearanceMetres: 2, physicalReleaseClearanceMetres: 5 });
  }

  reset(): void {
    this.tracker.clear(); this.conflicts.reset(); this.sources.clear(); this.lastSimulatedReports.clear(); this.confirmedIncidents.clear(); this.lastTime = -Infinity;
  }

  referenceRoute(aircraft: Aircraft): Route {
    return aircraft.conformance?.status === 'recovering' ? aircraft.route : aircraft.assignedRoute ?? aircraft.route;
  }

  ingest(aircraft: Aircraft, sample: SurfaceSample, now: number): SurfaceTrackResult {
    if (sample.aircraftId !== aircraft.id || aircraft.state === 'scheduled') throw new Error('A surface report must identify an active aircraft');
    const result = this.tracker.ingest(sample, this.referenceRoute(aircraft), now, {
      clearedClearanceIds: aircraft.clearedClearanceIds,
    });
    if (result.accepted && result.isNewObservation) {
      if (result.status === 'tracking' || result.status === 'lost') this.confirmedIncidents.delete(aircraft.id);
      if (result.confirmedObservation && !this.confirmedIncidents.has(aircraft.id)) {
        this.confirmedIncidents.add(aircraft.id);
        this.options.onConfirmedDeviation?.(aircraft, result.confirmedObservation, result);
      }
    }
    return result;
  }

  captureSimulation(aircraft: readonly Aircraft[], now: number): void {
    if (!Number.isFinite(now) || now < 0) throw new Error('Source time must be finite and nonnegative');
    if (now < this.lastTime || this.sources.size > 0 && aircraft.length > 0 && !aircraft.some(a => this.sources.get(a.id) === a)) this.reset();
    this.lastTime = now;
    const ids = new Set(aircraft.map(a => a.id));
    for (const id of this.sources.keys()) if (!ids.has(id)) { this.sources.delete(id); this.tracker.remove(id); this.lastSimulatedReports.delete(id); this.confirmedIncidents.delete(id); }
    for (const a of aircraft) {
      if (this.sources.has(a.id) && this.sources.get(a.id) !== a) { this.tracker.remove(a.id); this.lastSimulatedReports.delete(a.id); this.confirmedIncidents.delete(a.id); }
      this.sources.set(a.id, a);
      if (a.state === 'scheduled') continue;
      const previous = this.lastSimulatedReports.get(a.id);
      if (previous !== undefined && now - previous < this.reportIntervalSeconds - 1e-8) continue;
      const report = Math.floor(now / this.reportIntervalSeconds), hash = [...a.id].reduce((value, char) => (value * 31 + char.charCodeAt(0)) >>> 0, 7);
      const noise = (axis: number) => this.simulatedNoiseMetres * Math.sin((hash % 997 + report * 17 + axis * 31) * 1.618);
      this.ingest(a, { aircraftId: a.id, observedAt: now, receivedAt: now,
        x: a.x + noise(0), y: a.y + noise(1),
        headingTrueDegrees: ((90 - (a.bodyHeading ?? a.heading) * 180 / Math.PI) % 360 + 360) % 360,
        groundSpeedKnots: a.state === 'taxiing' ? a.speed / .514444 : 0,
        positionAccuracyMetres: Math.max(3, this.simulatedNoiseMetres * 2),
      }, now);
      this.lastSimulatedReports.set(a.id, now);
    }
  }

  estimate(id: string, now: number): SurfaceTrackResult | null { return this.tracker.estimate(id, now); }

  assignmentChanged(aircraft: Aircraft): void {
    this.tracker.setReferenceRoute(aircraft.id, this.referenceRoute(aircraft), { clearedClearanceIds: aircraft.clearedClearanceIds });
    this.confirmedIncidents.delete(aircraft.id);
    this.conflicts.invalidateAircraft(aircraft.id);
  }

  /** Prediction copies use measured motion, not a command to stop as evidence
   * that a live target stopped. The source and its route are never modified. */
  predictionFleet(aircraft: readonly Aircraft[], now: number): { aircraft: Aircraft[]; tracks: SurveillanceTrackQuality[] } {
    const fleet: Aircraft[] = [], tracks: SurveillanceTrackQuality[] = [];
    for (const source of aircraft) {
      if (source.state === 'scheduled') continue;
      const result = this.estimate(source.id, now); if (!result) continue;
      const observed = result.observation;
      const moving = result.speedMetresPerSecond > .5;
      const trustedProjection = result.status === 'tracking' && result.freshness !== 'stale' && result.freshness !== 'lost';
      const route = this.referenceRoute(source);
      const distance = result.routeDistance ?? result.observedRouteDistance ?? source.distance;
      const plannedHold = source.conformance?.status === 'unresolved' || source.conformance?.awaitingApproval;
      tracks.push({ ...result, status: plannedHold && result.status === 'tracking' ? 'confirmed-off-track' : result.status });
      fleet.push({ ...source, route, distance, x: result.x, y: result.y, heading: result.heading, bodyHeading: result.bodyHeading,
        speed: Math.max(.1, result.speedMetresPerSecond), state: moving && trustedProjection ? 'taxiing' : 'holding',
        conformance: moving ? undefined : source.conformance, yielding: undefined,
        ...(observed ? { edgeId: observed.edgeId, fromNodeId: observed.fromNodeId, toNodeId: observed.toNodeId, edgeProgress: observed.edgeProgress } : { edgeId: null }),
      });
    }
    return { aircraft: fleet, tracks };
  }

  detect(aircraft: readonly Aircraft[], now: number, horizon = 120): SurveillanceConflict[] {
    const fleet = this.predictionFleet(aircraft, now);
    return this.conflicts.detect(fleet.aircraft, fleet.tracks, now, horizon);
  }
}
