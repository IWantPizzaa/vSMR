import { TrafficPredictor } from './TrafficPredictor';
import type { PriorityOperatingRule } from './OperatingRules';
import type { TaxiGraph } from './TaxiGraph';
import type { Aircraft, MovementMode, OperatingConfiguration } from './types';

export interface YieldDecision {
  aircraftId: string;
  priorityAircraftId: string;
  ruleId: string;
  nodeId: string;
  /** Absolute distance along the yielding aircraft's route, never a geographic shortcut. */
  stopDistance: number;
  reason: string;
  sourceIds: string[];
}

export interface PriorityControllerOptions {
  /** Simulator parameters, not published operational separation standards. */
  timeGap?: number;
  stopBuffer?: number;
  horizon?: number;
}

/**
 * Applies a published give-way rule only to its exact incoming graph branches.
 * It makes no runway-clearance decision. TrafficPredictor stops before every
 * pending runway checkpoint, so a blocked future exit cannot claim priority.
 */
export class PriorityController {
  readonly diagnostics: string[] = [];
  readonly predictor: TrafficPredictor;
  readonly timeGap: number;
  readonly stopBuffer: number;
  readonly horizon: number;
  private rulesByNode = new Map<string, (PriorityOperatingRule & { priorityFromEdgeIds: string[] })[]>();

  constructor(readonly graph: TaxiGraph, rules: readonly PriorityOperatingRule[], options: PriorityControllerOptions = {}) {
    this.timeGap = options.timeGap ?? 12;
    this.stopBuffer = options.stopBuffer ?? 45;
    this.horizon = options.horizon ?? 120;
    if (!Number.isFinite(this.timeGap) || this.timeGap < 0 || !Number.isFinite(this.stopBuffer) || this.stopBuffer <= 0 || !Number.isFinite(this.horizon) || this.horizon <= 0) throw new Error('Invalid priority controller parameters');
    this.predictor = new TrafficPredictor(graph);
    const incident = (edgeId: string, nodeId: string) => {
      const edge = graph.edges.get(edgeId);
      return !!edge && (edge.from === nodeId || edge.to === nodeId);
    };
    for (const rule of rules) {
      const priorityEdges = rule.priorityFromEdgeIds;
      if (!graph.nodes.has(rule.nodeId) || !rule.fromEdgeId || !incident(rule.fromEdgeId, rule.nodeId) || !Array.isArray(priorityEdges) || !priorityEdges.length || !priorityEdges.every(id => typeof id === 'string' && incident(id, rule.nodeId) && id !== rule.fromEdgeId)) {
        this.diagnostics.push(`${rule.id}: priority remains advisory because exact incident branches are not mapped.`);
        continue;
      }
      const mapped = { ...rule, priorityFromEdgeIds: [...new Set(priorityEdges as string[])] };
      const list = this.rulesByNode.get(rule.nodeId) ?? [];
      list.push(mapped); this.rulesByNode.set(rule.nodeId, list);
    }
  }

  evaluate(aircraft: readonly Aircraft[], configuration: OperatingConfiguration, movement?: MovementMode): YieldDecision[] {
    // An automatically yielding aircraft is evaluated as if it resumed now.
    // A manual hold or uncleared crossing is never released by this controller.
    const prospective = aircraft.filter(a => a.state !== 'scheduled').map(a => (a.state === 'taxiing' || a.state === 'holding') && a.conformance?.status !== 'unresolved' && !a.conformance?.awaitingApproval && a.yielding && !a.waitingClearanceId && a.route.operational?.configuration === configuration
      ? { ...a, state: 'taxiing' as const, yielding: undefined } : a);
    const trajectories = this.predictor.predict(prospective, this.horizon);
    const priorityAtNode = new Map<string, { aircraft: Aircraft; occupancy: typeof trajectories[number]['nodes'][number] }[]>();
    for (const trajectory of trajectories) {
      const high = trajectory.aircraft;
      if (high.state !== 'taxiing' || high.conformance?.status === 'unresolved' || high.conformance?.awaitingApproval || high.waitingClearanceId || high.route.operational?.configuration !== configuration || high.route.operational.movement !== 'arrival') continue;
      for (const occupancy of trajectory.nodes) {
        if (!this.rulesByNode.has(occupancy.nodeId) || !occupancy.incoming) continue;
        const list = priorityAtNode.get(occupancy.nodeId) ?? [];
        list.push({ aircraft: high, occupancy }); priorityAtNode.set(occupancy.nodeId, list);
      }
    }
    const result: YieldDecision[] = [];
    for (const trajectory of trajectories) {
      const low = trajectory.aircraft;
      if (low.state !== 'taxiing' || low.conformance?.status === 'unresolved' || low.conformance?.awaitingApproval || low.waitingClearanceId || low.route.operational?.configuration !== configuration) continue;
      const lowMovement = low.route.operational.movement ?? movement ?? 'transfer';
      let best: YieldDecision | null = null;
      for (let i = 1; i < low.route.nodeIds.length; i++) {
        const nodeId = low.route.nodeIds[i], distance = low.route.cumulativeDistances[i];
        const nodeRules = this.rulesByNode.get(nodeId);
        if (!nodeRules) continue;
        if (distance < low.distance - 1e-8) continue;
        const incoming = low.route.edgeIds[i - 1], outgoing = low.route.edgeIds[i] ?? null;
        const lowOccupancy = trajectory.nodes.find(n => n.nodeId === nodeId && n.incoming === incoming && Math.abs(n.time - Math.max(0, (distance - low.distance) / low.speed)) < 1e-6);
        if (!lowOccupancy) continue; // Beyond the horizon or an uncleared runway crossing.
        for (const rule of nodeRules) {
          if ((rule.configuration !== 'both' && rule.configuration !== configuration) || (rule.movement && rule.movement !== 'both' && rule.movement !== lowMovement) || rule.fromEdgeId !== incoming || (rule.toEdgeId && rule.toEdgeId !== outgoing)) continue;
          for (const candidate of priorityAtNode.get(nodeId) ?? []) {
            if (candidate.aircraft.id === low.id || !candidate.occupancy.incoming || !rule.priorityFromEdgeIds.includes(candidate.occupancy.incoming)) continue;
            const highOccupancy = candidate.occupancy;
            if (lowOccupancy.start > highOccupancy.end + this.timeGap || highOccupancy.start > lowOccupancy.end + this.timeGap) continue;
            const decision: YieldDecision = { aircraftId: low.id, priorityAircraftId: candidate.aircraft.id, ruleId: rule.id, nodeId,
              stopDistance: Math.max(low.distance, distance - this.stopBuffer), reason: rule.reason, sourceIds: [...rule.sourceIds] };
            if (!best || decision.stopDistance < best.stopDistance || (decision.stopDistance === best.stopDistance && decision.priorityAircraftId.localeCompare(best.priorityAircraftId) < 0)) best = decision;
          }
        }
      }
      if (best) result.push(best);
    }
    return result.sort((a, b) => a.aircraftId.localeCompare(b.aircraftId));
  }
}
