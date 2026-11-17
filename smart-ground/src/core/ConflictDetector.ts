import { TaxiGraph } from './TaxiGraph';
import { TrafficPredictor } from './TrafficPredictor';
import type { Aircraft, Conflict, ConflictType, EdgeOccupancy, NodeOccupancy, PredictedTrajectory } from './types';

const labels: Record<ConflictType, string> = { 'head-on': 'Face-à-face', crossing: 'Croisement', overtaking: 'Rattrapage', blockage: 'Blocage' };

export class ConflictDetector {
  readonly predictor: TrafficPredictor;
  constructor(readonly graph: TaxiGraph, readonly separation = 45) { this.predictor = new TrafficPredictor(graph); }

  detect(aircraft: readonly Aircraft[], horizon = 120): Conflict[] {
    const predictions = this.predictor.predict(aircraft, horizon);
    const edgeIndex = new Map<string, { trajectory: PredictedTrajectory; occupancy: EdgeOccupancy }[]>();
    const nodeIndex = new Map<string, { trajectory: PredictedTrajectory; occupancy: NodeOccupancy }[]>();
    for (const trajectory of predictions) {
      for (const occupancy of trajectory.edges) {
        const list = edgeIndex.get(occupancy.edgeId) ?? []; list.push({ trajectory, occupancy }); edgeIndex.set(occupancy.edgeId, list);
      }
      for (const occupancy of trajectory.nodes) {
        const list = nodeIndex.get(occupancy.nodeId) ?? []; list.push({ trajectory, occupancy }); nodeIndex.set(occupancy.nodeId, list);
      }
    }
    const byPair = new Map<string, Conflict>();
    const record = (first: Aircraft, second: Aircraft, type: ConflictType, time: number, x: number, y: number, location: string) => {
      const ids = [first.id, second.id].sort() as [string, string], key = `${ids[0]}|${ids[1]}`;
      const candidate: Conflict = { id: `${key}|${type}`, type, severity: time <= 30 ? 'critical' : 'warning', aircraftIds: ids, timeToConflict: Math.max(0, time), x, y, location, message: `${labels[type]} · ${first.callsign} / ${second.callsign}` };
      const previous = byPair.get(key);
      if (!previous || time < previous.timeToConflict - 1e-6 || (Math.abs(time - previous.timeToConflict) < 1e-6 && type === 'blockage')) byPair.set(key, candidate);
    };
    for (const [edgeId, list] of edgeIndex) {
      const edge = this.graph.edges.get(edgeId)!, from = this.graph.nodes.get(edge.from)!, to = this.graph.nodes.get(edge.to)!;
      for (let i = 0; i < list.length; i++) for (let j = i + 1; j < list.length; j++) {
        const a = list[i], b = list[j];
        if (a.trajectory.aircraft.id === b.trajectory.aircraft.id) continue;
        const start = Math.max(a.occupancy.start, b.occupancy.start), end = Math.min(a.occupancy.end, b.occupancy.end);
        if (end < start) continue;
        const positionA = a.occupancy.offset + a.occupancy.velocity * (start - a.occupancy.start);
        const positionB = b.occupancy.offset + b.occupancy.velocity * (start - b.occupancy.start);
        const difference = positionA - positionB, relativeSpeed = a.occupancy.velocity - b.occupancy.velocity;
        const closest = relativeSpeed === 0 ? start : Math.max(start, Math.min(end, start - difference / relativeSpeed));
        if (Math.abs(difference + relativeSpeed * (closest - start)) > this.separation) continue;
        const entry = Math.abs(difference) <= this.separation ? start : start + (Math.abs(difference) - this.separation) / Math.abs(relativeSpeed);
        if (!Number.isFinite(entry) || entry > end + 1e-8) continue;
        const type: ConflictType = a.occupancy.stationary || b.occupancy.stationary ? 'blockage' : a.occupancy.velocity * b.occupancy.velocity < 0 ? 'head-on' : 'overtaking';
        const pa = a.occupancy.offset + a.occupancy.velocity * (entry - a.occupancy.start);
        const pb = b.occupancy.offset + b.occupancy.velocity * (entry - b.occupancy.start);
        const ratio = Math.max(0, Math.min(1, (pa + pb) / (2 * edge.length)));
        record(a.trajectory.aircraft, b.trajectory.aircraft, type, entry, from.x + (to.x - from.x) * ratio, from.y + (to.y - from.y) * ratio, edge.name || edge.id);
      }
    }
    for (const [nodeId, list] of nodeIndex) {
      const node = this.graph.nodes.get(nodeId)!;
      for (let i = 0; i < list.length; i++) for (let j = i + 1; j < list.length; j++) {
        const a = list[i], b = list[j], first = a.occupancy, second = b.occupancy;
        if (a.trajectory.aircraft.id === b.trajectory.aircraft.id) continue;
        const start = Math.max(first.start, second.start), end = Math.min(first.end, second.end);
        if (end < start) continue;
        // Shared-edge following and head-on encounters are classified continuously above.
        if (!first.stationary && !second.stationary && first.incoming && first.incoming === second.incoming) continue;
        if (!first.stationary && !second.stationary && first.incoming === second.outgoing && second.incoming === first.outgoing) continue;
        const type: ConflictType = first.stationary || second.stationary ? 'blockage' : 'crossing';
        record(a.trajectory.aircraft, b.trajectory.aircraft, type, start, node.x, node.y, `Intersection ${node.id}`);
      }
    }
    return [...byPair.values()].sort((a, b) => a.timeToConflict - b.timeToConflict || a.id.localeCompare(b.id));
  }
}
