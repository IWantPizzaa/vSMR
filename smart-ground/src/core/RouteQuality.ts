import type { TaxiGraph } from './TaxiGraph';
import type { TaxiEdge } from './types';

/** Metre equivalents keep manoeuvre preferences bounded by an actual detour. */
export const MAIN_AXIS_CHANGE_METRES = 120;
const RIGHT_ANGLE_TURN_METRES = 90;
const TANGENT_SAMPLE_METRES = 40;
const SIGNIFICANT_ANGLE = Math.PI / 6;

export interface JunctionQuality { significantTurns: number; turnCostMetres: number }
export interface CurvatureQuality { degrees: number; costMetres: number }

/** Measures decisions at junctions, never every vertex in a sampled curve. */
export class RouteQuality {
  private tangents = new Map<string, { x: number; y: number }>();
  private decisions = new Map<string, boolean>();
  private curvatures = new Map<string, CurvatureQuality>();
  constructor(readonly graph: TaxiGraph) {}

  /** Total polyline curvature includes a sampled S bend without counting its
   * vertices as separate junction decisions. Straight subdivisions add zero. */
  curvature(incomingId: string | null, via: string, outgoing: TaxiEdge): CurvatureQuality {
    if (!incomingId) return { degrees: 0, costMetres: 0 };
    const key = `${incomingId}|${via}|${outgoing.id}`, cached = this.curvatures.get(key);
    if (cached) return cached;
    const incoming = this.graph.edges.get(incomingId)!, b = this.graph.nodes.get(via)!;
    const a = this.graph.nodes.get(incoming.from === via ? incoming.to : incoming.from)!;
    const c = this.graph.nodes.get(outgoing.from === via ? outgoing.to : outgoing.from)!;
    const ux = b.x - a.x, uy = b.y - a.y, vx = c.x - b.x, vy = c.y - b.y;
    const radians = Math.abs(Math.atan2(ux * vy - uy * vx, ux * vx + uy * vy));
    const result = { degrees: radians * 180 / Math.PI, costMetres: RIGHT_ANGLE_TURN_METRES * radians / (Math.PI / 2) };
    this.curvatures.set(key, result); return result;
  }

  junction(incomingId: string | null, via: string, outgoing: TaxiEdge): JunctionQuality {
    const zero = { significantTurns: 0, turnCostMetres: 0 };
    if (!incomingId || incomingId === outgoing.id) return zero;
    let decision = this.decisions.get(via);
    if (decision === undefined) {
      decision = (this.graph.adjacency.get(via) || []).filter(a => this.graph.edges.get(a.edgeId)!.kind !== 'runway').length >= 3;
      this.decisions.set(via, decision);
    }
    if (!decision) return zero;
    const incoming = this.graph.edges.get(incomingId)!;
    const before = this.tangent(incoming, via), after = this.tangent(outgoing, via);
    const denominator = Math.hypot(before.x, before.y) * Math.hypot(after.x, after.y);
    if (denominator < 1e-8) return zero;
    const angle = Math.acos(Math.max(-1, Math.min(1, -(before.x * after.x + before.y * after.y) / denominator)));
    if (angle + 1e-8 < SIGNIFICANT_ANGLE) return zero;
    return { significantTurns: 1, turnCostMetres: RIGHT_ANGLE_TURN_METRES * angle / (Math.PI / 2) };
  }

  private tangent(first: TaxiEdge, via: string): { x: number; y: number } {
    const key = `${first.id}|${via}`, cached = this.tangents.get(key);
    if (cached) return cached;
    const origin = this.graph.nodes.get(via)!;
    let edge = first, from = via, remaining = TANGENT_SAMPLE_METRES;
    let point = origin;
    const visited = new Set<string>();
    while (!visited.has(edge.id)) {
      visited.add(edge.id);
      const to = edge.from === from ? edge.to : edge.from;
      const a = this.graph.nodes.get(from)!, b = this.graph.nodes.get(to)!;
      const fraction = Math.min(1, remaining / Math.max(edge.length, 1e-8));
      point = { ...b, x: a.x + (b.x - a.x) * fraction, y: a.y + (b.y - a.y) * fraction };
      remaining -= edge.length;
      if (remaining <= 0) break;
      // Continue only along a degree-two curve. Looking through another
      // decision would blend two separate manoeuvres into the same tangent.
      const next = (this.graph.adjacency.get(to) || []).filter(arc => arc.edgeId !== edge.id && this.graph.edges.get(arc.edgeId)!.kind !== 'runway');
      if (next.length !== 1) break;
      from = to; edge = this.graph.edges.get(next[0].edgeId)!;
    }
    const result = { x: point.x - origin.x, y: point.y - origin.y };
    this.tangents.set(key, result); return result;
  }
}
