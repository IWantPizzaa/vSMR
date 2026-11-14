import { TrafficPredictor } from './TrafficPredictor';
import { resolveAircraftGeometry, type AircraftGeometry, type FootprintBox } from './AircraftGeometry';
import type { TaxiGraph } from './TaxiGraph';
import type { Aircraft, Conflict, ConflictType, EdgeOccupancy } from './types';

export interface PhysicalConflictOptions {
  /** Explicit planning margin between envelope surfaces, not position error. */
  clearanceMetres?: number;
  /** Bounds the rotational enclosure error, never the temporal sampling step. */
  maximumRotationDegrees?: number;
}
export interface PhysicalConflict extends Conflict {
  envelope: {
    model: 'oriented-airframe-envelopes'; clearanceMetres: number; rotationErrorMetres: number;
    estimatedDimensions: boolean; timeToEnvelopeOverlap: number | null;
    aircraft: { id: string; lengthMetres: number; wingspanMetres: number; source: AircraftGeometry['source']; shape: AircraftGeometry['model'] }[];
    warnings: string[];
  };
}
interface Bounds { minX: number; minY: number; maxX: number; maxY: number }
interface Motion extends Bounds {
  start: number; end: number; x: number; y: number; vx: number; vy: number;
  body: number; movement: number; rotationPadding: number; stationary: boolean; edgeId: string;
}
interface PredictedFootprint { aircraft: Aircraft; geometry: AircraftGeometry; motions: Motion[]; bounds: Bounds }
interface OrientedBox { x: number; y: number; ux: number; uy: number; vx: number; vy: number; halfLength: number; halfWidth: number }
const wrap = (angle: number) => Math.atan2(Math.sin(angle), Math.cos(angle));
const intersects = (a: Bounds, b: Bounds) => a.minX <= b.maxX && b.minX <= a.maxX && a.minY <= b.maxY && b.minY <= a.maxY;

/** Continuous swept, oriented planning envelopes. Translation intersection is
 * solved analytically by SAT on each piece of the source route. The physical
 * paths can intersect even if two graph edges have no shared node. */
export class PhysicalConflictDetector {
  readonly predictor: TrafficPredictor;
  readonly clearanceMetres: number;
  readonly maximumRotationRadians: number;

  constructor(readonly graph: TaxiGraph, options: PhysicalConflictOptions = {}) {
    this.clearanceMetres = options.clearanceMetres ?? 2;
    const rotation = options.maximumRotationDegrees ?? 3;
    if (!Number.isFinite(this.clearanceMetres) || this.clearanceMetres < 0 || !Number.isFinite(rotation) || rotation <= 0 || rotation > 15) throw new Error('Invalid physical footprint tuning');
    this.maximumRotationRadians = rotation * Math.PI / 180;
    this.predictor = new TrafficPredictor(graph);
  }

  detect(aircraft: readonly Aircraft[], horizon = 120): PhysicalConflict[] {
    const predicted: PredictedFootprint[] = this.predictor.predict(aircraft, horizon).map(trajectory => {
      const geometry = resolveAircraftGeometry(trajectory.aircraft.aircraftProfile);
      const motions = this.motionPieces(trajectory.aircraft, trajectory.edges, geometry, horizon);
      const bounds = motions.reduce<Bounds>((total, item) => ({ minX: Math.min(total.minX, item.minX), minY: Math.min(total.minY, item.minY), maxX: Math.max(total.maxX, item.maxX), maxY: Math.max(total.maxY, item.maxY) }), { minX: Infinity, minY: Infinity, maxX: -Infinity, maxY: -Infinity });
      return { aircraft: trajectory.aircraft, geometry, motions, bounds };
    });
    const conflicts: PhysicalConflict[] = [];
    for (let i = 0; i < predicted.length; i++) for (let j = i + 1; j < predicted.length; j++) {
      const first = predicted[i], second = predicted[j];
      if (!intersects(first.bounds, second.bounds)) continue;
      let firstHit: { time: number; a: Motion; b: Motion } | null = null, contact = Infinity;
      let startB = 0;
      for (const a of first.motions) {
        while (startB < second.motions.length && second.motions[startB].end < a.start - 1e-9) startB++;
        for (let k = startB; k < second.motions.length && second.motions[k].start <= a.end + 1e-9; k++) {
          const b = second.motions[k];
          if (!intersects(a, b)) continue;
          const start = Math.max(a.start, b.start), end = Math.min(a.end, b.end); if (end < start - 1e-9) continue;
          const time = this.overlapTime(a, b, first.geometry, second.geometry, start, end, this.clearanceMetres);
          if (time === null) continue;
          if (!firstHit || time < firstHit.time) firstHit = { time, a, b };
          const physical = this.clearanceMetres === 0 ? time : this.overlapTime(a, b, first.geometry, second.geometry, start, end, 0);
          if (physical !== null) contact = Math.min(contact, physical);
        }
      }
      if (!firstHit) continue;
      const { time, a, b } = firstHit, ids = [first.aircraft.id, second.aircraft.id].sort() as [string, string];
      const type: ConflictType = a.stationary || b.stationary ? 'blockage'
        : Math.abs(wrap(a.movement - b.movement)) > Math.PI * .75 ? 'head-on'
        : Math.abs(wrap(a.movement - b.movement)) < Math.PI * .2 ? 'overtaking' : 'crossing';
      const pa = this.position(a, time), pb = this.position(b, time);
      const edgeA = this.graph.edges.get(a.edgeId), edgeB = this.graph.edges.get(b.edgeId);
      const estimatedDimensions = first.geometry.source === 'estimated-length' || second.geometry.source === 'estimated-length';
      conflicts.push({ id: `${ids.join('|')}|physical`, aircraftIds: ids, type,
        severity: !estimatedDimensions && time <= 30 ? 'critical' : 'warning', timeToConflict: Math.max(0, time),
        x: (pa.x + pb.x) / 2, y: (pa.y + pb.y) / 2,
        location: a.edgeId === b.edgeId ? edgeA?.name || a.edgeId : `${edgeA?.name || a.edgeId} / ${edgeB?.name || b.edgeId}`,
        message: `Enveloppes orientées · ${first.aircraft.callsign} / ${second.aircraft.callsign}`,
        envelope: { model: 'oriented-airframe-envelopes', clearanceMetres: this.clearanceMetres,
          rotationErrorMetres: Math.max(a.rotationPadding, b.rotationPadding), estimatedDimensions,
          timeToEnvelopeOverlap: Number.isFinite(contact) ? contact : null,
          aircraft: [first, second].map(p => ({ id: p.aircraft.id, lengthMetres: p.geometry.lengthMetres, wingspanMetres: p.geometry.wingspanMetres, source: p.geometry.source, shape: p.geometry.model })),
          warnings: [...new Set([...first.geometry.warnings, ...second.geometry.warnings])],
        },
      });
    }
    return conflicts.sort((a, b) => a.timeToConflict - b.timeToConflict || a.id.localeCompare(b.id));
  }

  private motionPieces(aircraft: Aircraft, occupancy: EdgeOccupancy[], geometry: AircraftGeometry, horizon: number): Motion[] {
    const motions: Motion[] = [];
    const firstMoving = occupancy.find(item => !item.stationary && Math.abs(item.velocity) > 1e-9);
    const movingHeading = firstMoving ? this.arcHeading(firstMoving) : aircraft.heading;
    const bodyNow = Number.isFinite(aircraft.bodyHeading) ? aircraft.bodyHeading! : aircraft.heading;
    const offset = wrap(bodyNow - movingHeading);
    let previousBody = bodyNow;
    for (const item of occupancy) {
      const edge = this.graph.edges.get(item.edgeId); if (!edge) continue;
      const from = this.graph.nodes.get(edge.from)!, to = this.graph.nodes.get(edge.to)!;
      const x = from.x + (to.x - from.x) * item.offset / edge.length, y = from.y + (to.y - from.y) * item.offset / edge.length;
      const vx = (to.x - from.x) * item.velocity / edge.length, vy = (to.y - from.y) * item.velocity / edge.length;
      const movement = item.stationary ? aircraft.heading : Math.atan2(vy, vx);
      const body = item.stationary ? previousBody : movement + offset;
      const delta = wrap(body - previousBody);
      if (Math.abs(delta) > 1e-8) {
        // The source polyline can contain a sharp, instantaneous tangent change.
        // Enclose its rotation at that node rather than skipping the wing sweep.
        // Each <=3deg rotational interval has a rigorous radius-based padding.
        const count = Math.ceil(Math.abs(delta) / this.maximumRotationRadians), step = delta / count;
        const rotationPadding = 2 * geometry.radiusMetres * Math.sin(Math.abs(step) / 4);
        for (let i = 0; i < count; i++) motions.push(this.bound({ start: item.start, end: item.start, x, y, vx: 0, vy: 0,
          body: previousBody + step * (i + .5), movement, rotationPadding, stationary: false, edgeId: item.edgeId }, geometry));
      }
      motions.push(this.bound({ start: item.start, end: item.end, x, y, vx, vy, body, movement, rotationPadding: 0, stationary: item.stationary, edgeId: item.edgeId }, geometry));
      previousBody = body;
    }
    if (!motions.length && Number.isFinite(aircraft.x) && Number.isFinite(aircraft.y)) {
      // An unassigned or off-network surface report still occupies real space.
      // No next direction is invented without matched route geometry.
      motions.push(this.bound({ start: 0, end: horizon, x: aircraft.x, y: aircraft.y,
        vx: 0, vy: 0, body: bodyNow, movement: aircraft.heading, rotationPadding: 0, stationary: true, edgeId: aircraft.edgeId ?? 'position observée' }, geometry));
    }
    return motions.sort((a, b) => a.start - b.start || a.end - b.end);
  }

  private arcHeading(item: EdgeOccupancy): number {
    const from = this.graph.nodes.get(item.from)!, to = this.graph.nodes.get(item.to)!;
    return Math.atan2(to.y - from.y, to.x - from.x);
  }
  private position(motion: Motion, time: number) { return { x: motion.x + motion.vx * (time - motion.start), y: motion.y + motion.vy * (time - motion.start) }; }
  private bound(motion: Omit<Motion, keyof Bounds>, geometry: AircraftGeometry): Motion {
    const endX = motion.x + motion.vx * (motion.end - motion.start), endY = motion.y + motion.vy * (motion.end - motion.start);
    const radius = geometry.radiusMetres + this.clearanceMetres + motion.rotationPadding;
    return { ...motion, minX: Math.min(motion.x, endX) - radius, maxX: Math.max(motion.x, endX) + radius,
      minY: Math.min(motion.y, endY) - radius, maxY: Math.max(motion.y, endY) + radius };
  }
  private box(motion: Motion, shape: FootprintBox, time: number, padding: number): OrientedBox {
    const position = this.position(motion, time), ux = Math.cos(motion.body), uy = Math.sin(motion.body);
    return { x: position.x + shape.x * ux - shape.y * uy, y: position.y + shape.x * uy + shape.y * ux,
      ux, uy, vx: -uy, vy: ux, halfLength: shape.halfLength + padding, halfWidth: shape.halfWidth + padding };
  }
  private overlapTime(a: Motion, b: Motion, ga: AircraftGeometry, gb: AircraftGeometry, start: number, end: number, clearance: number): number | null {
    let earliest = Infinity;
    for (const shapeA of ga.boxes) for (const shapeB of gb.boxes) {
      const first = this.box(a, shapeA, start, clearance / 2 + a.rotationPadding), second = this.box(b, shapeB, start, clearance / 2 + b.rotationPadding);
      let entry = 0, exit = Math.max(0, end - start);
      for (const [x, y] of [[first.ux, first.uy], [first.vx, first.vy], [second.ux, second.uy], [second.vx, second.vy]]) {
        const radius = first.halfLength * Math.abs(first.ux * x + first.uy * y) + first.halfWidth * Math.abs(first.vx * x + first.vy * y)
          + second.halfLength * Math.abs(second.ux * x + second.uy * y) + second.halfWidth * Math.abs(second.vx * x + second.vy * y);
        const separation = (first.x - second.x) * x + (first.y - second.y) * y, velocity = (a.vx - b.vx) * x + (a.vy - b.vy) * y;
        if (Math.abs(velocity) < 1e-12) { if (Math.abs(separation) > radius + 1e-9) { entry = Infinity; break; } }
        else {
          const t1 = (-radius - separation) / velocity, t2 = (radius - separation) / velocity;
          entry = Math.max(entry, Math.min(t1, t2)); exit = Math.min(exit, Math.max(t1, t2));
          if (entry > exit + 1e-9) { entry = Infinity; break; }
        }
      }
      if (Number.isFinite(entry) && entry <= exit + 1e-9) earliest = Math.min(earliest, start + Math.max(0, entry));
    }
    return Number.isFinite(earliest) ? earliest : null;
  }
}
