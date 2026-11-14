import { TaxiGraph } from './TaxiGraph';
import type { Aircraft, EdgeOccupancy, NodeOccupancy, PredictedTrajectory } from './types';

/** Continuous edge occupancy, rather than coarse position samples that miss crossings. */
export class TrafficPredictor {
  constructor(readonly graph: TaxiGraph, readonly nodeClearance = 30) {}

  predict(aircraft: readonly Aircraft[], horizon = 120): PredictedTrajectory[] {
    if (!Number.isFinite(horizon) || horizon < 0) throw new Error('Prediction horizon must be finite and nonnegative');
    // A scheduled movement has not entered the simulated movement area yet.
    // In particular, it must not occupy its future gate/exit in predictions.
    return aircraft.filter(a => a.state !== 'scheduled').map(a => this.predictAircraft(a, horizon));
  }

  private predictAircraft(aircraft: Aircraft, horizon: number): PredictedTrajectory {
    const edges: EdgeOccupancy[] = [], nodes: NodeOccupancy[] = [];
    const checkpoint = aircraft.route.operational?.clearanceCheckpoints.find(c => !aircraft.clearedClearanceIds?.includes(c.id));
    const route = aircraft.route;
    const plannedStop = Math.min(route.distance, checkpoint?.distance ?? Infinity, aircraft.yielding?.stopDistance ?? Infinity);
    if (aircraft.state !== 'taxiing' || aircraft.conformance?.status === 'unresolved' || aircraft.conformance?.awaitingApproval) {
      if (aircraft.edgeId) {
        const edge = this.graph.edges.get(aircraft.edgeId);
        if (edge) edges.push({ edgeId: edge.id, from: aircraft.fromNodeId, to: aircraft.toNodeId, start: 0, end: horizon, offset: edge.from === aircraft.fromNodeId ? aircraft.edgeProgress * edge.length : (1 - aircraft.edgeProgress) * edge.length, velocity: 0, stationary: true });
      }
      for (const nodeId of new Set([aircraft.fromNodeId, aircraft.toNodeId])) {
        const node = this.graph.nodes.get(nodeId);
        if (node && Math.hypot(aircraft.x - node.x, aircraft.y - node.y) <= this.nodeClearance) nodes.push({ nodeId, start: 0, end: horizon, time: 0, incoming: aircraft.edgeId, outgoing: null, stationary: true });
      }
      return { aircraft, edges, nodes, horizon };
    }
    const speed = aircraft.speed;
    for (let i = 0; i < route.edgeIds.length; i++) {
      const remainingStart = route.cumulativeDistances[i] - aircraft.distance;
      if (route.cumulativeDistances[i] >= plannedStop - 1e-8) break;
      const remainingEnd = Math.min(route.cumulativeDistances[i + 1], plannedStop) - aircraft.distance;
      if (remainingEnd < -1e-8) continue;
      const start = Math.max(0, remainingStart / speed), end = Math.min(horizon, remainingEnd / speed);
      if (start > horizon) break;
      if (end < start) continue;
      const edge = this.graph.edges.get(route.edgeIds[i])!;
      const offsetAlongRoute = Math.max(0, -remainingStart);
      const forward = edge.from === route.nodeIds[i];
      edges.push({ edgeId: edge.id, from: route.nodeIds[i], to: route.nodeIds[i + 1], start, end, offset: forward ? offsetAlongRoute : edge.length - offsetAlongRoute, velocity: forward ? speed : -speed, stationary: false });
    }
    // An aircraft remains at its destination after arrival; it does not disappear.
    const arrival = (plannedStop - aircraft.distance) / speed;
    if (arrival <= horizon && route.edgeIds.length) {
      const index = Math.max(0, route.cumulativeDistances.findIndex((distance, i) => i > 0 && distance >= plannedStop - 1e-8) - 1);
      const edge = this.graph.edges.get(route.edgeIds[index])!, along = plannedStop - route.cumulativeDistances[index];
      edges.push({ edgeId: edge.id, from: route.nodeIds[index], to: route.nodeIds[index + 1], start: Math.max(0, arrival), end: horizon, offset: edge.from === route.nodeIds[index] ? along : edge.length - along, velocity: 0, stationary: true });
    }
    for (let i = 0; i < route.nodeIds.length; i++) {
      const time = (route.cumulativeDistances[i] - aircraft.distance) / speed;
      const clearanceTime = this.nodeClearance / speed;
      if (route.cumulativeDistances[i] > plannedStop + this.nodeClearance) break;
      if (time + clearanceTime < 0 || time - clearanceTime > horizon) continue;
      const terminal = Math.abs(route.cumulativeDistances[i] - plannedStop) <= this.nodeClearance && arrival <= horizon;
      nodes.push({ nodeId: route.nodeIds[i], time: Math.max(0, terminal ? Math.min(time, arrival) : time), start: Math.max(0, time - clearanceTime), end: terminal ? horizon : Math.min(horizon, time + clearanceTime), incoming: i > 0 ? route.edgeIds[i - 1] : null, outgoing: i < route.edgeIds.length ? route.edgeIds[i] : null, stationary: terminal });
    }
    return { aircraft, edges, nodes, horizon };
  }
}
