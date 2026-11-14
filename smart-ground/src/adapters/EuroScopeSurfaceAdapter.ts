import type { SurfaceSample } from '../core/SurfaceTracker';

/** Owned primitives copied by ONE native radar-update ingestion callback.
 * receivedAt is local monotonic receipt time; SDK GetReceivedTime() is an AGE,
 * not this timestamp. Neither a redraw nor consumedAt creates a new report. */
export interface EuroScopeSurfaceReport {
  aircraftId: string;
  sessionId: string;
  sequence: number;
  receivedAt: number;
  latitude: number;
  longitude: number;
  reportedGroundSpeedKnots?: number;
  /** GetGS(): repeats the reported speed, or calculated fallback when it is 0. */
  effectiveGroundSpeedKnots?: number;
  /** Only GetReportedHeadingTrueNorth(), not a magnetic heading/course. */
  headingTrueDegrees?: number;
}

export interface EuroScopeSurfaceProjection {
  latitude: number;
  longitude: number;
  /** Same scale as the pinned LFPG graph, configurable for another graph. */
  metresPerDegree?: number;
}

export type EuroScopeSurfaceRejection = 'invalid-identity' | 'inactive-session' | 'session-mismatch'
  | 'invalid-sequence' | 'duplicate-or-out-of-order-sequence' | 'invalid-time' | 'future-receipt'
  | 'nonadvancing-receipt' | 'invalid-position' | 'invalid-speed' | 'invalid-heading';

export type EuroScopeSurfaceAdaptation = {
  accepted: true;
  sample: SurfaceSample;
  sessionId: string;
  sequence: number;
  speedSource: 'reported' | 'effective-fallback' | 'unavailable';
} | { accepted: false; reason: EuroScopeSurfaceRejection };

interface Session { id: string; sequence?: number; receivedAt?: number }

const validIdentity = (value: unknown): value is string => typeof value === 'string'
  && value.length > 0 && value.length <= 128 && value.trim() === value && !/[\x00-\x1f\x7f]/.test(value);
const nonnegative = (value: unknown): value is number => typeof value === 'number' && Number.isFinite(value) && value >= 0;

/** SDK-free bridge boundary. Session changes are explicit so a delayed callback
 * from an old connection cannot overwrite the current aircraft's sensor state. */
export class EuroScopeSurfaceAdapter {
  private sessions = new Map<string, Session>();
  readonly projection: Readonly<Required<EuroScopeSurfaceProjection>>;
  private readonly eastScale: number;

  constructor(projection: EuroScopeSurfaceProjection = { latitude: 49.0097, longitude: 2.5479 }) {
    const origin = { ...projection, metresPerDegree: projection.metresPerDegree ?? 111320 };
    if (!Number.isFinite(origin.latitude) || Math.abs(origin.latitude) >= 90
      || !Number.isFinite(origin.longitude) || Math.abs(origin.longitude) > 180
      || !Number.isFinite(origin.metresPerDegree) || origin.metresPerDegree <= 0) throw new Error('Invalid surface projection origin/scale');
    this.projection = Object.freeze(origin);
    this.eastScale = origin.metresPerDegree * Math.cos(origin.latitude * Math.PI / 180);
  }

  /** Returns true for a new generation. The owner must also remove this ID from
   * SurfaceTracker/alert history when it starts a different tracking session. */
  beginSession(aircraftId: string, sessionId: string): boolean {
    if (!validIdentity(aircraftId) || !validIdentity(sessionId)) throw new Error('Invalid surface tracking identity');
    if (this.sessions.get(aircraftId)?.id === sessionId) return false;
    this.sessions.set(aircraftId, { id: sessionId }); return true;
  }

  endSession(aircraftId: string, sessionId: string): boolean {
    if (this.sessions.get(aircraftId)?.id !== sessionId) return false;
    return this.sessions.delete(aircraftId);
  }

  clear(): void { this.sessions.clear(); }

  adapt(report: EuroScopeSurfaceReport, consumedAt: number): EuroScopeSurfaceAdaptation {
    const reject = (reason: EuroScopeSurfaceRejection): EuroScopeSurfaceAdaptation => ({ accepted: false, reason });
    if (!report || !validIdentity(report.aircraftId) || !validIdentity(report.sessionId)) return reject('invalid-identity');
    const session = this.sessions.get(report.aircraftId);
    if (!session) return reject('inactive-session');
    if (session.id !== report.sessionId) return reject('session-mismatch');
    if (!Number.isSafeInteger(report.sequence) || report.sequence < 0) return reject('invalid-sequence');
    if (session.sequence !== undefined && report.sequence <= session.sequence) return reject('duplicate-or-out-of-order-sequence');
    if (!nonnegative(report.receivedAt) || !nonnegative(consumedAt)) return reject('invalid-time');
    if (report.receivedAt > consumedAt) return reject('future-receipt');
    if (session.receivedAt !== undefined && report.receivedAt <= session.receivedAt) return reject('nonadvancing-receipt');
    if (!Number.isFinite(report.latitude) || Math.abs(report.latitude) > 90
      || !Number.isFinite(report.longitude) || Math.abs(report.longitude) > 180) return reject('invalid-position');
    if (report.reportedGroundSpeedKnots !== undefined && !nonnegative(report.reportedGroundSpeedKnots)
      || report.effectiveGroundSpeedKnots !== undefined && !nonnegative(report.effectiveGroundSpeedKnots)) return reject('invalid-speed');
    if (report.headingTrueDegrees !== undefined && (!Number.isFinite(report.headingTrueDegrees)
      || report.headingTrueDegrees < 0 || report.headingTrueDegrees >= 360)) return reject('invalid-heading');

    let groundSpeedKnots = report.reportedGroundSpeedKnots;
    let speedSource: 'reported' | 'effective-fallback' | 'unavailable' = groundSpeedKnots === undefined ? 'unavailable' : 'reported';
    // Preserve GetGS's dependency instead of treating it as another sensor.
    // Without the raw reported value, fallback provenance cannot be verified.
    if (groundSpeedKnots === 0 && report.effectiveGroundSpeedKnots !== undefined) {
      groundSpeedKnots = report.effectiveGroundSpeedKnots; speedSource = 'effective-fallback';
    }
    const sample: SurfaceSample = {
      aircraftId: report.aircraftId, observedAt: report.receivedAt, receivedAt: report.receivedAt,
      x: (report.longitude - this.projection.longitude) * this.eastScale,
      y: (report.latitude - this.projection.latitude) * this.projection.metresPerDegree,
      ...(groundSpeedKnots === undefined ? {} : { groundSpeedKnots }),
      ...(report.headingTrueDegrees === undefined ? {} : { headingTrueDegrees: report.headingTrueDegrees }),
    };
    session.sequence = report.sequence; session.receivedAt = report.receivedAt;
    return { accepted: true, sample, sequence: report.sequence, sessionId: report.sessionId, speedSource };
  }
}
