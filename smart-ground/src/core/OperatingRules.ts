import type { AirportLocation, MovementMode, OperatingConfiguration, Route, RouteOperationalData, TaxiEdge } from './types';
import type { TaxiGraph } from './TaxiGraph';

export type RuleConfiguration = OperatingConfiguration | 'both';
export type RuleMovement = 'departure' | 'arrival' | 'both';
export interface RuleSource { id: string; title: string; url: string; effectiveDate?: string; sha256?: string; [key: string]: unknown }
export interface EdgeOperatingRule { id: string; edgeIds: string[]; configuration: RuleConfiguration; movement?: RuleMovement; direction: 'forward' | 'reverse' | 'closed'; sourceIds: string[]; reason: string; [key: string]: unknown }
export interface TurnOperatingRule { id: string; viaNodeId: string; fromEdgeId: string; toEdgeId: string; configuration: RuleConfiguration; movement?: RuleMovement; sourceIds: string[]; reason: string; [key: string]: unknown }
export interface ClearanceOperatingRule { id: string; nodeId: string; configuration: RuleConfiguration; movement?: RuleMovement; kind: 'runway-crossing' | 'hold-short'; sourceIds: string[]; reason: string; fromEdgeId?: string; toEdgeId?: string; [key: string]: unknown }
export interface PriorityOperatingRule { id: string; nodeId: string; configuration: RuleConfiguration; movement?: RuleMovement; sourceIds: string[]; reason: string; fromEdgeId?: string; toEdgeId?: string; [key: string]: unknown }
export interface RunwayAccess { locationId: string; runway: string; role: 'departure' | 'arrival'; sourceIds: string[]; pprRequired?: boolean; [key: string]: unknown }
export interface OperatingManifest {
  version: string | number; airport: 'LFPG'; effectiveDate: string; sources: RuleSource[];
  edgeRules: EdgeOperatingRule[]; turnRules?: TurnOperatingRule[];
  allowedTurnRules?: TurnOperatingRule[]; forbidUnmarkedSyntheticTurns?: boolean; forbidUTurns?: boolean; restrictParkingToEndpoints?: boolean;
  clearanceRules?: ClearanceOperatingRule[]; priorityRules?: PriorityOperatingRule[];
  operationalLocations?: AirportLocation[]; runwayAccess?: RunwayAccess[];
  closedLocationIds?: string[];
  approvedPprLocationIds?: string[];
  runwayUse?: Record<OperatingConfiguration, { departure: string[]; arrival: string[] }>;
  [key: string]: unknown;
}
export interface RuleDiagnostic { code: string; severity: 'error' | 'warning'; message: string; ruleId?: string; configuration?: OperatingConfiguration; movement?: MovementMode }

const active = (rule: { configuration: RuleConfiguration; movement?: RuleMovement }, configuration: OperatingConfiguration, movement: MovementMode = 'transfer') => (rule.configuration === 'both' || rule.configuration === configuration) && (!rule.movement || rule.movement === 'both' || rule.movement === movement);
const strings = (value: unknown): value is string[] => Array.isArray(value) && value.every(v => typeof v === 'string' && v.length > 0);
const configurations = new Set(['east', 'west', 'both']);

/** Source-backed constraints are separate from source geometry. Unknown coverage is explicit. */
export class OperatingRules {
  readonly diagnostics: RuleDiagnostic[] = [];
  readonly locations: AirportLocation[];
  readonly valid: boolean;
  private edgeRules = new Map<string, EdgeOperatingRule[]>();
  private turnRules = new Map<string, TurnOperatingRule[]>();
  private allowedTurns = new Map<string, TurnOperatingRule[]>();
  private clearances = new Map<string, ClearanceOperatingRule[]>();
  private priorities = new Map<string, PriorityOperatingRule[]>();
  private accessByLocation = new Map<string, RunwayAccess[]>();
  private parkingComponents = new Map<string, number>();
  private reverseTaxiTurns = new Map<string, boolean>();

  constructor(readonly graph: TaxiGraph, readonly manifest: OperatingManifest) {
    const error = (code: string, message: string, ruleId?: string) => this.diagnostics.push({ code, severity: 'error', message, ruleId });
    if (!manifest || manifest.airport !== 'LFPG' || !manifest.version || typeof manifest.effectiveDate !== 'string' || !Array.isArray(manifest.sources) || !Array.isArray(manifest.edgeRules)) {
      throw new Error('Invalid LFPG operating manifest');
    }
    const sources = new Set<string>();
    for (const source of manifest.sources) {
      if (!source || typeof source.id !== 'string' || !source.id || sources.has(source.id) || typeof source.url !== 'string' || typeof source.title !== 'string') error('invalid-source', 'Missing or duplicate source identifier.');
      else sources.add(source.id);
    }
    const ids = new Set<string>();
    const validateRule = (rule: { id: string; configuration: RuleConfiguration; movement?: RuleMovement; sourceIds: string[]; reason: string }) => {
      if (!rule || typeof rule.id !== 'string' || !rule.id || ids.has(rule.id)) { error('invalid-rule', 'Missing or duplicate rule identifier.', rule?.id); return false; }
      ids.add(rule.id);
      if (!configurations.has(rule.configuration) || (rule.movement !== undefined && !['departure', 'arrival', 'both'].includes(rule.movement)) || typeof rule.reason !== 'string' || !strings(rule.sourceIds) || !rule.sourceIds.length || rule.sourceIds.some(id => !sources.has(id))) {
        error('invalid-rule-provenance', 'Rule has an invalid configuration, reason or source reference.', rule.id); return false;
      }
      return true;
    };
    for (const rule of manifest.edgeRules) {
      if (!validateRule(rule)) continue;
      if (!strings(rule.edgeIds) || !rule.edgeIds.length || !['forward', 'reverse', 'closed'].includes(rule.direction)) { error('invalid-edge-rule', 'Rule must reference edges and a supported direction.', rule.id); continue; }
      for (const id of new Set(rule.edgeIds)) {
        if (!graph.edges.has(id)) { error('unknown-edge', `Rule references missing edge ${id}.`, rule.id); continue; }
        const list = this.edgeRules.get(id) ?? []; list.push(rule); this.edgeRules.set(id, list);
      }
    }
    for (const rule of manifest.turnRules ?? []) {
      if (!validateRule(rule)) continue;
      if (!graph.nodes.has(rule.viaNodeId) || !this.incident(rule.fromEdgeId, rule.viaNodeId) || !this.incident(rule.toEdgeId, rule.viaNodeId)) { error('invalid-turn', 'Turn rule references an unknown or nonincident edge/node.', rule.id); continue; }
      const list = this.turnRules.get(rule.viaNodeId) ?? []; list.push(rule); this.turnRules.set(rule.viaNodeId, list);
    }
    for (const rule of manifest.allowedTurnRules ?? []) {
      if (!validateRule(rule)) continue;
      if (!graph.nodes.has(rule.viaNodeId) || !this.incident(rule.fromEdgeId, rule.viaNodeId) || !this.incident(rule.toEdgeId, rule.viaNodeId)) { error('invalid-allowed-turn', 'Allowed turn references an unknown or nonincident edge/node.', rule.id); continue; }
      const list = this.allowedTurns.get(rule.viaNodeId) ?? []; list.push(rule); this.allowedTurns.set(rule.viaNodeId, list);
    }
    for (const [rules, index] of [[manifest.clearanceRules ?? [], this.clearances], [manifest.priorityRules ?? [], this.priorities]] as const) {
      for (const rule of rules) {
        if (!validateRule(rule)) continue;
        if (!graph.nodes.has(rule.nodeId) || (rule.fromEdgeId && !this.incident(rule.fromEdgeId, rule.nodeId)) || (rule.toEdgeId && !this.incident(rule.toEdgeId, rule.nodeId))) { error('invalid-checkpoint', 'Checkpoint references an unknown or nonincident edge/node.', rule.id); continue; }
        if ('kind' in rule && !['runway-crossing', 'hold-short'].includes(String(rule.kind))) { error('invalid-clearance-kind', 'Unknown clearance kind.', rule.id); continue; }
        // Both indexes contain source-backed node rules; the constructor preserves their own type.
        const list = index.get(rule.nodeId) ?? [];
        (list as (PriorityOperatingRule | ClearanceOperatingRule)[]).push(rule);
        index.set(rule.nodeId, list as ClearanceOperatingRule[] & PriorityOperatingRule[]);
      }
    }
    const locationById = new Map(graph.locations.map(l => [l.id, l]));
    for (const location of manifest.operationalLocations ?? []) {
      if (!location || !location.id || locationById.has(location.id) || !location.nodeId || !graph.nodes.has(location.nodeId) || !Number.isFinite(location.x) || !Number.isFinite(location.y)) { error('invalid-location', `Invalid or duplicate operational location ${location?.id}.`); continue; }
      locationById.set(location.id, location);
    }
    this.locations = [...locationById.values()];
    for (const access of manifest.runwayAccess ?? []) {
      if (!access || !locationById.has(access.locationId) || !['departure', 'arrival'].includes(access.role) || !strings(access.sourceIds) || !access.sourceIds.length || access.sourceIds.some(id => !sources.has(id))) { error('invalid-runway-access', `Invalid runway access ${access?.locationId}.`); continue; }
      const list = this.accessByLocation.get(access.locationId) ?? []; list.push(access); this.accessByLocation.set(access.locationId, list);
    }
    this.valid = !this.diagnostics.some(d => d.severity === 'error');
    let component = 0;
    for (const start of graph.nodes.keys()) {
      if (this.parkingComponents.has(start) || !graph.adjacency.get(start)?.some(a => graph.edges.get(a.edgeId)!.kind === 'parking_position')) continue;
      const queue = [start]; this.parkingComponents.set(start, component);
      for (let i = 0; i < queue.length; i++) for (const arc of graph.adjacency.get(queue[i])!) {
        if (graph.edges.get(arc.edgeId)!.kind !== 'parking_position' || this.parkingComponents.has(arc.to)) continue;
        this.parkingComponents.set(arc.to, component); queue.push(arc.to);
      }
      component++;
    }
    for (const configuration of ['east', 'west'] as const) for (const movement of ['departure', 'arrival', 'transfer'] as const) for (const [id, rules] of this.edgeRules) {
      const directions = new Set(rules.filter(r => active(r, configuration, movement)).map(r => r.direction));
      if (directions.has('forward') && directions.has('reverse') && !directions.has('closed')) this.diagnostics.push({ code: 'conflicting-directions', severity: 'warning', message: `Conflicting directions close edge ${id} in ${configuration}/${movement} context.`, configuration, movement });
    }
  }

  private incident(edgeId: string, nodeId: string) { const edge = this.graph.edges.get(edgeId); return !!edge && (edge.from === nodeId || edge.to === nodeId); }
  edgeConstraints(edgeId: string, configuration: OperatingConfiguration, movement?: MovementMode, scope: 'all' | 'hard' = 'all') { return (this.edgeRules.get(edgeId) ?? []).filter(rule => active(rule, configuration, movement) && (scope === 'all' || rule.authoritative !== false)); }
  allowArc(edge: TaxiEdge, from: string, to: string, configuration: OperatingConfiguration, movement?: MovementMode, scope: 'all' | 'hard' = 'all'): boolean {
    if (!this.valid || !((edge.from === from && edge.to === to) || (edge.to === from && edge.from === to))) return false;
    return this.edgeConstraints(edge.id, configuration, movement, scope).every(rule => rule.direction !== 'closed' && (rule.direction === 'forward' ? edge.from === from : edge.to === from));
  }
  allowTurn(previousEdgeId: string | null, viaNodeId: string, nextEdgeId: string, configuration: OperatingConfiguration, movement?: MovementMode): boolean {
    if (!this.valid || (this.turnRules.get(viaNodeId) ?? []).some(rule => active(rule, configuration, movement) && rule.fromEdgeId === previousEdgeId && rule.toEdgeId === nextEdgeId)) return false;
    if (this.manifest.forbidUTurns && previousEdgeId === nextEdgeId) return false;
    // Two distinct source edges can still form an immediate near-reversal.
    // Parking lead-ins retain their existing pushback abstraction; this guard
    // applies only to taxiway/taxiway transitions, using incident geometry.
    if (this.manifest.forbidUTurns && previousEdgeId && this.isReverseTaxiTurn(previousEdgeId, viaNodeId, nextEdgeId)) return false;
    if (this.manifest.forbidUnmarkedSyntheticTurns && previousEdgeId && viaNodeId.startsWith('intersection:')) {
      const first = this.graph.edges.get(previousEdgeId) as TaxiEdge & { sourceSegmentId?: string }, second = this.graph.edges.get(nextEdgeId) as TaxiEdge & { sourceSegmentId?: string };
      const sameSourceSegment = previousEdgeId !== nextEdgeId && first.sourceSegmentId && first.sourceSegmentId === second.sourceSegmentId;
      if (!sameSourceSegment && !(this.allowedTurns.get(viaNodeId) ?? []).some(rule => active(rule, configuration, movement) && rule.fromEdgeId === previousEdgeId && rule.toEdgeId === nextEdgeId)) return false;
    }
    return true;
  }
  private isReverseTaxiTurn(previousId: string, viaId: string, nextId: string): boolean {
    const key = `${previousId}|${viaId}|${nextId}`, cached = this.reverseTaxiTurns.get(key);
    if (cached !== undefined) return cached;
    const first = this.graph.edges.get(previousId), second = this.graph.edges.get(nextId);
    if (!first || !second) return true;
    if (first.kind !== 'taxiway' || second.kind !== 'taxiway') return false;
    const via = this.graph.nodes.get(viaId)!, before = this.graph.nodes.get(first.from === viaId ? first.to : first.from)!, after = this.graph.nodes.get(second.from === viaId ? second.to : second.from)!;
    const ux = via.x - before.x, uy = via.y - before.y, vx = after.x - via.x, vy = after.y - via.y;
    const length = Math.hypot(ux, uy) * Math.hypot(vx, vy);
    const reversing = length > 1e-10 && (ux * vx + uy * vy) / length <= -Math.sqrt(3) / 2 + 1e-10;
    this.reverseTaxiTurns.set(key, reversing); return reversing;
  }
  /** Parking lead-ins cannot become an intermediate apron shortcut. */
  parkingPhase(edge: TaxiEdge, phase: 'start' | 'taxi' | 'destination', origin: string, destination: string): 'start' | 'taxi' | 'destination' | null {
    if (!this.manifest.restrictParkingToEndpoints) return phase;
    if (edge.kind !== 'parking_position') return phase === 'destination' ? null : 'taxi';
    const component = this.parkingComponents.get(edge.from);
    if (phase === 'start' && component !== undefined && component === this.parkingComponents.get(origin)) return 'start';
    if (component !== undefined && component === this.parkingComponents.get(destination)) return 'destination';
    return null;
  }
  runwayAccess(configuration: OperatingConfiguration, role: 'departure' | 'arrival'): RunwayAccess[] {
    if (!this.valid) return [];
    const allowed = this.manifest.runwayUse?.[configuration]?.[role];
    return [...this.accessByLocation.values()].flat().filter(access => access.role === role && (!allowed || allowed.includes(access.runway)) && (!access.pprRequired || this.manifest.approvedPprLocationIds?.includes(access.locationId)) && !this.manifest.closedLocationIds?.includes(access.locationId));
  }
  isLocationAllowed(locationId: string, configuration: OperatingConfiguration, role: 'departure' | 'arrival'): boolean { return this.runwayAccess(configuration, role).some(a => a.locationId === locationId); }
  isLocationOpen(locationId: string): boolean { return this.valid && !this.manifest.closedLocationIds?.includes(locationId); }

  describeRoute(route: Route, configuration: OperatingConfiguration, movement?: MovementMode, scope: 'all' | 'hard' = 'all'): RouteOperationalData {
    const applied = new Map<string, { id: string; sourceIds: string[] }>();
    const unverifiedEdgeIds: string[] = [], clearanceCheckpoints: RouteOperationalData['clearanceCheckpoints'] = [], advisories: RouteOperationalData['advisories'] = [];
    route.edgeIds.forEach(id => {
      const constraints = this.edgeConstraints(id, configuration, movement, scope);
      if (!constraints.length) unverifiedEdgeIds.push(id);
      constraints.forEach(rule => applied.set(rule.id, rule));
    });
    route.nodeIds.forEach((nodeId, i) => {
      const incoming = i > 0 ? route.edgeIds[i - 1] : null, outgoing = route.edgeIds[i] ?? null;
      (this.turnRules.get(nodeId) ?? []).filter(rule => active(rule, configuration, movement) && rule.fromEdgeId === incoming).forEach(rule => applied.set(rule.id, rule));
      (this.allowedTurns.get(nodeId) ?? []).filter(rule => active(rule, configuration, movement) && rule.fromEdgeId === incoming && rule.toEdgeId === outgoing).forEach(rule => applied.set(rule.id, rule));
      for (const rule of this.clearances.get(nodeId) ?? []) {
        if (!active(rule, configuration, movement) || (rule.fromEdgeId && rule.fromEdgeId !== incoming) || (rule.toEdgeId && rule.toEdgeId !== outgoing)) continue;
        applied.set(rule.id, rule);
        clearanceCheckpoints.push({ id: `${rule.id}@${i}`, ruleId: rule.id, nodeId, nodeIndex: i, distance: route.cumulativeDistances[i], kind: rule.kind, reason: rule.reason, sourceIds: rule.sourceIds });
      }
      for (const rule of this.priorities.get(nodeId) ?? []) {
        if (!active(rule, configuration, movement) || (rule.fromEdgeId && rule.fromEdgeId !== incoming) || (rule.toEdgeId && rule.toEdgeId !== outgoing)) continue;
        applied.set(rule.id, rule); advisories.push({ id: rule.id, nodeId, reason: rule.reason, sourceIds: rule.sourceIds });
      }
    });
    return { configuration, movement, ruleIds: [...applied.keys()].sort(), sourceIds: [...new Set([...applied.values()].flatMap(rule => rule.sourceIds))].sort(), unverifiedEdgeIds, clearanceCheckpoints, advisories };
  }
}
