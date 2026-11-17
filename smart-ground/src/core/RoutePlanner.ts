import { TaxiGraph } from './TaxiGraph';
import { OperatingRules } from './OperatingRules';
import { TaxiFlowPolicy, compareFlowCost, type FlowCost, type FlowState } from './TaxiFlowPolicy';
import type { AircraftProfile, AirportLocation, MovementMode, OperatingConfiguration, Route, TaxiEdge } from './types';
import { MAIN_AXIS_CHANGE_METRES } from './RouteQuality';
import { AircraftRestrictions, normalizedAirportFlags, type AircraftRestrictionContext } from './AircraftRestrictions';
import { DEFAULT_AIRCRAFT_PROFILE, normalizeAircraftProfile, sameAircraftProfile } from './AircraftProfile';

export interface RouteRequest { movement?: MovementMode; originId?: string; destinationId?: string; aircraftProfile?: AircraftProfile }

class FlowHeap {
  private values: { id: string; score: FlowCost; cost: FlowCost }[] = [];
  get length() { return this.values.length; }
  push(item: { id: string; score: FlowCost; cost: FlowCost }) {
    this.values.push(item); let index = this.values.length - 1;
    while (index > 0) { const parent = (index - 1) >> 1; if (compareFlowCost(this.values[parent].score, item.score) <= 0) break; this.values[index] = this.values[parent]; index = parent; }
    this.values[index] = item;
  }
  pop() {
    const first = this.values[0], last = this.values.pop()!;
    if (this.values.length) {
      let index = 0;
      while (index * 2 + 1 < this.values.length) {
        let child = index * 2 + 1;
        if (child + 1 < this.values.length && compareFlowCost(this.values[child + 1].score, this.values[child].score) < 0) child++;
        if (compareFlowCost(this.values[child].score, last.score) >= 0) break;
        this.values[index] = this.values[child]; index = child;
      }
      this.values[index] = last;
    }
    return first;
  }
}

/** Binary heap intentionally has no framework/runtime dependencies for C++ parity. */
class MinHeap {
  private values: { id: string; score: number; cost: number }[] = [];
  get length() { return this.values.length; }
  push(item: { id: string; score: number; cost: number }) {
    this.values.push(item);
    let index = this.values.length - 1;
    while (index > 0) {
      const parent = (index - 1) >> 1;
      if (this.values[parent].score <= item.score) break;
      this.values[index] = this.values[parent]; index = parent;
    }
    this.values[index] = item;
  }
  pop() {
    const first = this.values[0], last = this.values.pop()!;
    if (this.values.length) {
      let index = 0;
      while (index * 2 + 1 < this.values.length) {
        let child = index * 2 + 1;
        if (child + 1 < this.values.length && this.values[child + 1].score < this.values[child].score) child++;
        if (this.values[child].score >= last.score) break;
        this.values[index] = this.values[child]; index = child;
      }
      this.values[index] = last;
    }
    return first;
  }
}

export class RoutePlanner {
  operatingRules: OperatingRules | null = null;
  flowPolicy: TaxiFlowPolicy | null = null;
  aircraftRestrictions: AircraftRestrictions | null = null;
  aircraftProfile: AircraftProfile = DEFAULT_AIRCRAFT_PROFILE;
  aircraftRestrictionContext: AircraftRestrictionContext = Object.freeze({ airportFlags: Object.freeze([]) });
  lastFlowFailure: string | null = null;
  lastAircraftFailure: string | null = null;
  configuration: OperatingConfiguration = 'west';
  private aircraftConnectivity: { key: string; rules: OperatingRules | null; flow: TaxiFlowPolicy | null; aircraft: AircraftRestrictions; components: Map<string, string> } | null = null;
  /** An assignment flag alone cannot authorize a soft-flow override. Only
   * geometry constructed and checked by the controller route search is kept. */
  private controllerApprovals = new Set<string>();
  constructor(readonly graph: TaxiGraph, rules?: OperatingRules, configuration: OperatingConfiguration = 'west') {
    if (rules) this.setOperatingRules(rules, configuration);
  }

  setOperatingRules(rules: OperatingRules | null, configuration: OperatingConfiguration): void {
    if (!['east', 'west'].includes(configuration)) throw new Error('Unknown operating configuration');
    if (rules && rules.graph !== this.graph) throw new Error('Operating rules belong to another taxi graph');
    this.operatingRules = rules; this.configuration = configuration;
  }

  setFlowPolicy(policy: TaxiFlowPolicy | null): void {
    if (policy && policy.graph !== this.graph) throw new Error('Taxi flow policy belongs to another graph');
    this.flowPolicy = policy; this.lastFlowFailure = null;
  }

  setAircraftRestrictions(policy: AircraftRestrictions | null): void {
    if (policy && policy.graph !== this.graph) throw new Error('Aircraft restrictions belong to another taxi graph');
    this.aircraftRestrictions = policy; this.lastAircraftFailure = null;
  }
  setAircraftProfile(profile: AircraftProfile): void { this.aircraftProfile = normalizeAircraftProfile(profile); }
  setAircraftRestrictionContext(context: AircraftRestrictionContext): void {
    this.aircraftRestrictionContext = Object.freeze({ airportFlags: Object.freeze(normalizedAirportFlags(context)) });
  }

  /** Effective permissions shared by route search, simulation and map arrows. */
  allowArc(edge: TaxiEdge, from: string, to: string, movement: MovementMode = 'transfer', profile: AircraftProfile = this.aircraftProfile): boolean {
    if (edge.kind === 'runway') return false;
    if (this.operatingRules && !this.operatingRules.allowArc(edge, from, to, this.configuration, movement, this.flowPolicy ? 'hard' : 'all')) return false;
    if (this.flowPolicy && !this.flowPolicy.allowArc(edge, from, to, this.configuration, movement)) return false;
    return !this.aircraftRestrictions || this.aircraftRestrictions.allowArc(edge, from, to, this.configuration, movement, profile, this.aircraftRestrictionContext);
  }

  allowTurn(previousEdgeId: string | null, viaNodeId: string, nextEdgeId: string, movement: MovementMode = 'transfer', profile: AircraftProfile = this.aircraftProfile): boolean {
    if (this.operatingRules && !this.operatingRules.allowTurn(previousEdgeId, viaNodeId, nextEdgeId, this.configuration, movement)) return false;
    return !this.aircraftRestrictions || this.aircraftRestrictions.allowTurn(previousEdgeId, viaNodeId, nextEdgeId, this.configuration, movement, profile, this.aircraftRestrictionContext);
  }

  get locations(): AirportLocation[] { return this.operatingRules?.locations ?? this.graph.locations; }
  availableLocations(role: 'departure' | 'arrival'): AirportLocation[] {
    if (!this.operatingRules) return this.locations.filter(p => p.kind === 'runway' || p.kind === 'holding_point');
    const ids = new Set(this.operatingRules.runwayAccess(this.configuration, role).map(a => a.locationId));
    return this.locations.filter(p => ids.has(p.id));
  }

  isRouteAllowed(route: Route, movement = route.operational?.movement, references: { originId?: string; destinationId?: string; aircraftProfile?: AircraftProfile } = {}): boolean {
    const controller = route.assignment?.source === 'controller';
    if (controller && !this.controllerApprovals.has(this.controllerKey(route))) return false;
    if (controller) {
      let cursor = -1;
      for (const id of route.assignment!.waypointNodeIds) { cursor = route.nodeIds.indexOf(id, cursor + 1); if (cursor < 0) return false; }
    }
    if (!route.nodeIds.length || route.edgeIds.length + 1 !== route.nodeIds.length || route.cumulativeDistances.length !== route.nodeIds.length
      || !Number.isFinite(route.distance) || !Number.isFinite(route.cumulativeDistances[0]) || Math.abs(route.cumulativeDistances[0]) > 1e-6 || !this.graph.nodes.has(route.nodeIds[0])) return false;
    if (route.operational && route.operational.configuration !== this.configuration) return false;
    if (route.aircraft && (route.aircraft.configuration !== this.configuration || movement !== undefined && route.aircraft.movement !== movement)) return false;
    let profile: AircraftProfile;
    try {
      profile = normalizeAircraftProfile(references.aircraftProfile ?? route.aircraft?.profile ?? this.aircraftProfile);
      if (route.aircraft && !sameAircraftProfile(profile, normalizeAircraftProfile(route.aircraft.profile))) return false;
      if (this.aircraftRestrictions && (!route.aircraft || JSON.stringify(route.aircraft.airportFlags) !== JSON.stringify(normalizedAirportFlags(this.aircraftRestrictionContext)))) return false;
    } catch { return false; }
    if (this.flowPolicy && (!route.flow || route.flow.configuration !== this.configuration || route.flow.movement !== (movement ?? route.flow.movement))) return false;
    if (this.flowPolicy) {
      for (const [id, metadataId, nodeId] of [[references.originId, route.flow?.originId, route.nodeIds[0]], [references.destinationId, route.flow?.destinationId, route.nodeIds.at(-1)!]]) {
        if (id && metadataId && id !== metadataId) return false;
        const reference = id || metadataId;
        if (reference && this.locations.find(l => l.id === reference)?.nodeId !== nodeId) return false;
        if (reference && this.operatingRules && !this.operatingRules.isLocationOpen(reference)) return false;
      }
    }
    const request = { movement: movement ?? route.flow?.movement ?? route.aircraft?.movement, originId: references.originId || route.flow?.originId, destinationId: references.destinationId || route.flow?.destinationId };
    const effectiveMovement = request.movement ?? 'transfer';
    if (this.flowPolicy && this.operatingRules?.manifest.runwayAccess?.length) {
      if (request.movement === 'departure' && request.destinationId && !this.operatingRules.isLocationAllowed(request.destinationId, this.configuration, 'departure')) return false;
      if (request.movement === 'arrival' && request.originId && !this.operatingRules.isLocationAllowed(request.originId, this.configuration, 'arrival')) return false;
    }
    const flow = !controller ? this.flowPolicy?.compile({ ...request, configuration: this.configuration, movement: request.movement ?? 'transfer', from: route.nodeIds[0], to: route.nodeIds.at(-1)!, runway: this.requestRunway(request, route.nodeIds[0], route.nodeIds.at(-1)!) }) : undefined;
    let flowState = this.flowPolicy?.initialState();
    let parkingPhase: 'start' | 'taxi' | 'destination' = 'start';
    let distance = 0;
    for (let i = 0; i < route.edgeIds.length; i++) {
      const edge = this.graph.edges.get(route.edgeIds[i]);
      if (!edge || edge.kind === 'runway' || !((edge.from === route.nodeIds[i] && edge.to === route.nodeIds[i + 1]) || (edge.to === route.nodeIds[i] && edge.from === route.nodeIds[i + 1]))) return false;
      distance += edge.length;
      if (!Number.isFinite(route.cumulativeDistances[i + 1]) || Math.abs(route.cumulativeDistances[i + 1] - distance) > 1e-5) return false;
      if (!this.allowArc(edge, route.nodeIds[i], route.nodeIds[i + 1], effectiveMovement, profile) || !this.allowTurn(i ? route.edgeIds[i - 1] : null, route.nodeIds[i], edge.id, effectiveMovement, profile)) return false;
      if (this.operatingRules) {
        const nextPhase = this.operatingRules.parkingPhase(edge, parkingPhase, route.nodeIds[0], route.nodeIds.at(-1)!);
        if (nextPhase === null) return false; parkingPhase = nextPhase;
      }
      if (this.flowPolicy && flow && flowState) { const next = this.flowPolicy.step(flow, flowState, edge); if (!next) return false; flowState = next.state; }
    }
    return Math.abs(route.distance - distance) < 1e-5 && (!flow || this.flowPolicy!.complete(flow, flowState!));
  }

  describeRoute(route: Route, movement?: MovementMode, aircraftProfile: AircraftProfile = route.aircraft?.profile ?? this.aircraftProfile): Route {
    movement ??= route.flow?.movement ?? route.aircraft?.movement;
    if (this.flowPolicy && route.flow) {
      const options = { movement: movement ?? route.flow.movement, originId: route.flow.originId, destinationId: route.flow.destinationId };
      const context = this.flowPolicy.compile({ ...options, from: route.nodeIds[0], to: route.nodeIds.at(-1)!, configuration: this.configuration, runway: this.requestRunway(options, route.nodeIds[0], route.nodeIds.at(-1)!) });
      const controller = route.assignment?.source === 'controller';
      if (controller) { context.stages = []; context.firstMainAxisIds.clear(); context.lastMainAxisIds.clear(); context.forbiddenPortalEdgeIds.clear(); }
      let state = this.flowPolicy.initialState(), apronTransit = 0, reentries = 0;
      for (const id of route.edgeIds) { const step = this.flowPolicy.step(context, state, this.graph.edges.get(id)!); if (!step) throw new Error('Route uses an inappropriate nominal apron portal'); state = step.state; apronTransit += step.apronTransit; reentries += step.reentries; }
      route = { ...route, flow: this.flowPolicy.describe(route, context, [apronTransit, reentries, 0, route.distance]) };
      if (controller) route.flow = { ...route.flow!, status: 'review', policyIds: [], requiredStages: [], warnings: [...route.flow!.warnings, 'Cheminement attribué par le contrôleur ; les contraintes générales restent appliquées.'] };
    }
    if (this.aircraftRestrictions) route = { ...route, aircraft: this.aircraftRestrictions.describeRoute(route, this.configuration, movement ?? route.operational?.movement ?? 'transfer', aircraftProfile, this.aircraftRestrictionContext) };
    if (!this.operatingRules) return route;
    const operational = this.operatingRules.describeRoute(route, this.configuration, movement, this.flowPolicy ? 'hard' : 'all');
    if (this.flowPolicy) operational.unverifiedEdgeIds = operational.unverifiedEdgeIds.filter(id => this.flowPolicy!.edgeDirection(id, this.configuration, movement ?? 'transfer') === null);
    return { ...route, operational };
  }

  /** Runway centrelines are display geometry; taxi routes end at taxiway access nodes. */
  findRoute(from: string, to: string, options: RouteRequest = {}): Route | null {
    this.lastFlowFailure = null; this.lastAircraftFailure = null;
    const profile = normalizeAircraftProfile(options.aircraftProfile ?? this.aircraftProfile);
    if (this.aircraftRestrictions && !this.aircraftRestrictions.valid) { this.lastAircraftFailure = 'Le catalogue des restrictions de gabarit ou de virage est invalide.'; this.lastFlowFailure = this.lastAircraftFailure; return null; }
    let constrained: Route | null;
    if (this.flowPolicy) constrained = this.findFlowRoute(from, to, { ...options, aircraftProfile: profile });
    else if (this.operatingRules || this.aircraftRestrictions) constrained = this.findOperationalRoute(from, to, options.movement, profile);
    else constrained = null;
    if (this.flowPolicy || this.operatingRules || this.aircraftRestrictions) {
      if (!constrained && this.aircraftRestrictions) {
        this.lastAircraftFailure = `Aucun parcours compatible pour ${profile.typeCode} (${profile.wingspanMetres} m d’envergure), avec les restrictions de voies, de virage et de configuration sélectionnées.`;
        this.lastFlowFailure = this.lastAircraftFailure + (this.lastFlowFailure ? ` ${this.lastFlowFailure}` : '');
      }
      return constrained;
    }
    if (!this.graph.areConnected(from, to)) return null;
    if (from === to) return { nodeIds: [from], edgeIds: [], distance: 0, cumulativeDistances: [0] };
    const heap = new MinHeap();
    const costs = new Map<string, number>([[from, 0]]);
    const previous = new Map<string, { node: string; edge: string }>();
    heap.push({ id: from, score: this.graph.distance(from, to), cost: 0 });
    while (heap.length) {
      const current = heap.pop();
      if (current.cost !== costs.get(current.id)) continue;
      if (current.id === to) {
        const nodeIds = [to], edgeIds: string[] = [];
        let id = to;
        while (id !== from) { const p = previous.get(id)!; nodeIds.push(p.node); edgeIds.push(p.edge); id = p.node; }
        nodeIds.reverse(); edgeIds.reverse();
        const cumulativeDistances = [0];
        for (const edgeId of edgeIds) cumulativeDistances.push(cumulativeDistances.at(-1)! + this.graph.edges.get(edgeId)!.length);
        return { nodeIds, edgeIds, distance: costs.get(to)!, cumulativeDistances };
      }
      for (const arc of this.graph.adjacency.get(current.id)!) {
        const edge = this.graph.edges.get(arc.edgeId)!;
        if (edge.kind === 'runway') continue;
        const cost = current.cost + arc.length;
        if (cost + 1e-8 >= (costs.get(arc.to) ?? Infinity)) continue;
        costs.set(arc.to, cost); previous.set(arc.to, { node: current.id, edge: arc.edgeId });
        heap.push({ id: arc.to, cost, score: cost + this.graph.distance(arc.to, to) });
      }
    }
    return null;
  }

  private requestRunway(options: RouteRequest, from?: string, to?: string): string | undefined {
    const location = options.movement === 'arrival' ? options.originId : options.destinationId;
    const role = options.movement === 'arrival' ? 'arrival' : 'departure';
    const accesses = this.operatingRules?.manifest.runwayAccess || [];
    if (location) return accesses.find(a => a.locationId === location && a.role === role)?.runway;
    const nodeId = role === 'arrival' ? from : to;
    const ids = new Set(this.locations.filter(l => l.nodeId === nodeId).map(l => l.id));
    const candidates = new Set(accesses.filter(a => a.role === role && ids.has(a.locationId)).map(a => a.runway));
    return candidates.size === 1 ? [...candidates][0] : undefined;
  }

  private controllerKey(route: Route): string {
    return JSON.stringify([route.nodeIds, route.edgeIds, route.assignment, route.flow && [route.flow.configuration, route.flow.movement, route.flow.originId, route.flow.destinationId], route.operational && [route.operational.configuration, route.operational.movement], route.aircraft && [route.aircraft.profile, route.aircraft.configuration, route.aircraft.movement, route.aircraft.airportFlags]]);
  }

  private approveControllerRoute(route: Route, options: RouteRequest): Route | null {
    let candidate: Route;
    try { candidate = this.describeRoute(route, options.movement, options.aircraftProfile); } catch { return null; }
    const key = this.controllerKey(candidate); this.controllerApprovals.add(key);
    if (this.isRouteAllowed(candidate, options.movement, options)) return candidate;
    this.controllerApprovals.delete(key); return null;
  }

  /** A controller's explicit junction choices replace soft nominal portals and
   * stages, while every physical arc, turn, parking and runway rule remains.
   * The fixed prefix includes the current edge whenever already entered. */
  findControllerRoute(base: Route, fixedNodeCount: number, waypointNodeIds: readonly string[], options: RouteRequest, revision: number): Route | null {
    if (!this.isRouteAllowed(base, options.movement, options) || !Number.isInteger(fixedNodeCount) || fixedNodeCount < 1 || fixedNodeCount > base.nodeIds.length
      || !Number.isInteger(revision) || revision < 1 || waypointNodeIds.some(id => !this.graph.nodes.has(id)) || new Set(waypointNodeIds).size !== waypointNodeIds.length) return null;
    const movement = options.movement ?? base.operational?.movement ?? base.flow?.movement ?? 'transfer';
    const profile = normalizeAircraftProfile(options.aircraftProfile ?? base.aircraft?.profile ?? this.aircraftProfile);
    const origin = base.nodeIds[0], destination = base.nodeIds.at(-1)!, nodes = base.nodeIds.slice(0, fixedNodeCount), edges = base.edgeIds.slice(0, fixedNodeCount - 1), from = nodes.at(-1)!;
    const remainingWaypoints = waypointNodeIds.filter(id => !nodes.includes(id));
    const rules = this.operatingRules, policy = this.flowPolicy;
    const context = policy?.compile({ ...options, movement, from: origin, to: destination, configuration: this.configuration, runway: this.requestRunway({ ...options, movement }, origin, destination) });
    if (context) { context.stages = []; context.firstMainAxisIds.clear(); context.lastMainAxisIds.clear(); context.forbiddenPortalEdgeIds.clear(); }
    type State = { node: string; incoming: string | null; parking: 'start' | 'taxi' | 'destination'; stage: number; flow: FlowState | undefined };
    let parking: State['parking'] = 'start', flow = policy?.initialState();
    for (let i = 0; i < edges.length; i++) {
      const edge = this.graph.edges.get(edges[i])!;
      if (!this.allowArc(edge, nodes[i], nodes[i + 1], movement, profile) || !this.allowTurn(i ? edges[i - 1] : null, nodes[i], edge.id, movement, profile)) return null;
      const phase: State['parking'] | null = rules?.parkingPhase(edge, parking, origin, destination) ?? (rules ? null : parking); if (phase === null) return null; parking = phase;
      if (policy && context && flow) { const step = policy.step(context, flow, edge); if (!step) return null; flow = step.state; }
    }
    const keyOf = (state: State) => JSON.stringify([state.node, state.incoming, state.parking, state.stage, state.flow]);
    const initial: State = { node: from, incoming: edges.at(-1) ?? null, parking, stage: 0, flow }, first = keyOf(initial);
    const states = new Map([[first, initial]]), costs = new Map<string, FlowCost>([[first, [0, 0, 0, 0]]]);
    const previous = new Map<string, { key: string; edge: string }>(), heap = new FlowHeap();
    heap.push({ id: first, cost: [0, 0, 0, 0], score: [0, 0, 0, 0] });
    const fixedPast = new Set(nodes.slice(0, -1));
    let expanded = 0;
    while (heap.length && expanded++ < 120000) {
      const current = heap.pop(); if (compareFlowCost(current.cost, costs.get(current.id)!) !== 0) continue;
      const state = states.get(current.id)!;
      if (state.node === destination && state.stage === remainingWaypoints.length) {
        const suffixNodes = [state.node], suffixEdges: string[] = []; let key = current.id;
        while (key !== first) { const p = previous.get(key)!; suffixEdges.push(p.edge); suffixNodes.push(states.get(p.key)!.node); key = p.key; }
        suffixNodes.reverse(); suffixEdges.reverse();
        const nodeIds = [...nodes, ...suffixNodes.slice(1)], edgeIds = [...edges, ...suffixEdges], cumulativeDistances = [0];
        for (const id of edgeIds) cumulativeDistances.push(cumulativeDistances.at(-1)! + this.graph.edges.get(id)!.length);
        const candidate: Route = { ...base, nodeIds, edgeIds, cumulativeDistances, distance: cumulativeDistances.at(-1)!, assignment: { source: 'controller', revision, waypointNodeIds: [...waypointNodeIds] } };
        const approved = this.approveControllerRoute(candidate, { ...options, movement, aircraftProfile: profile }); if (approved) return approved;
      }
      if (state.node === destination) continue;
      for (const arc of this.graph.adjacency.get(state.node) ?? []) {
        const edge = this.graph.edges.get(arc.edgeId)!;
        if (arc.edgeId === state.incoming || fixedPast.has(arc.to) || !this.allowArc(edge, state.node, arc.to, movement, profile) || !this.allowTurn(state.incoming, state.node, edge.id, movement, profile)) continue;
        const nextParking = rules?.parkingPhase(edge, state.parking, origin, destination) ?? (rules ? null : state.parking); if (nextParking === null) continue;
        const step = policy && context && state.flow ? policy.step(context, state.flow, edge) : undefined; if (policy && !step) continue;
        const stage = state.stage + (arc.to === remainingWaypoints[state.stage] ? 1 : 0);
        if (remainingWaypoints.slice(stage + 1).includes(arc.to)) continue;
        const next: State = { node: arc.to, incoming: edge.id, parking: nextParking, stage, flow: step?.state }, key = keyOf(next);
        const cost: FlowCost = [current.cost[0] + (step?.apronTransit ?? 0), current.cost[1] + (step?.reentries ?? 0), current.cost[2] + arc.length + (policy?.quality.curvature(state.incoming, state.node, edge).costMetres ?? 0) + (step?.axisChanges ?? 0) * MAIN_AXIS_CHANGE_METRES, current.cost[3] + arc.length];
        const old = costs.get(key); if (old && compareFlowCost(cost, old) >= 0) continue;
        states.set(key, next); costs.set(key, cost); previous.set(key, { key: current.id, edge: edge.id }); heap.push({ id: key, cost, score: cost });
      }
    }
    return null;
  }

  /** Join a downstream directed suffix of the assignment. Prefix is actual,
   * already travelled geometry including the observed current edge. Keeping
   * that history lets the ordinary full replay check every flow stage, marked
   * turn, aircraft restriction and runway checkpoint without a recovery bypass. */
  findRecoveryRoute(prefix: Route, assigned: Route, firstAssignedNode: number, options: RouteRequest): { route: Route; rejoinNodeId: string; rejoinDistance: number } | null {
    const movement = options.movement ?? 'transfer', profile = normalizeAircraftProfile(options.aircraftProfile ?? this.aircraftProfile);
    if (!prefix.edgeIds.length || !this.isRouteAllowed(assigned, movement, options)) return null;
    const from = prefix.nodeIds.at(-1)!, destination = assigned.nodeIds.at(-1)!, origin = prefix.nodeIds[0];
    const policy = this.flowPolicy, rules = this.operatingRules;
    const context = policy?.compile({ ...options, from: origin, to: destination, configuration: this.configuration, movement, runway: this.requestRunway(options, origin, destination) });
    const controller = assigned.assignment?.source === 'controller';
    if (controller && context) { context.stages = []; context.firstMainAxisIds.clear(); context.lastMainAxisIds.clear(); context.forbiddenPortalEdgeIds.clear(); }
    type State = { node: string; incoming: string | null; parking: 'start' | 'taxi' | 'destination'; flow: FlowState | undefined };
    let flow = policy?.initialState(), parking: State['parking'] = 'start';
    for (let i = 0; i < prefix.edgeIds.length; i++) {
      const edge = this.graph.edges.get(prefix.edgeIds[i]);
      if (!edge || !this.allowArc(edge, prefix.nodeIds[i], prefix.nodeIds[i + 1], movement, profile)
        || !this.allowTurn(i ? prefix.edgeIds[i - 1] : null, prefix.nodeIds[i], edge.id, movement, profile)) return null;
      const phase: State['parking'] | null = rules ? rules.parkingPhase(edge, parking, origin, destination) : parking;
      if (phase === null) return null; parking = phase;
      if (policy && context && flow) { const next = policy.step(context, flow, edge); if (!next) return null; flow = next.state; }
    }
    const keyOf = (s: State) => JSON.stringify([s.node, s.incoming, s.parking, s.flow]);
    const initial: State = { node: from, incoming: prefix.edgeIds.at(-1)!, parking, flow }, first = keyOf(initial);
    const states = new Map<string, State>([[first, initial]]), costs = new Map<string, FlowCost>([[first, [0, 0, 0, 0]]]);
    const previous = new Map<string, { key: string; edge: string }>(), heap = new FlowHeap();
    heap.push({ id: first, cost: [0, 0, 0, 0], score: [0, 0, 0, 0] });
    const targets = new Map<string, number[]>();
    const nextWaypoint = controller ? Math.min(...(assigned.assignment?.waypointNodeIds ?? []).filter(id => !prefix.nodeIds.includes(id)).map(id => assigned.nodeIds.indexOf(id)).filter(i => i >= firstAssignedNode)) : Infinity;
    const downstream = assigned.nodeIds.map((_, i) => i).filter(i => i >= Math.max(1, firstAssignedNode) && i <= nextWaypoint);
    const mainTargets = downstream.filter(i => policy?.mainEdgeIds.has(assigned.edgeIds[i]) || policy?.mainEdgeIds.has(assigned.edgeIds[i - 1]));
    for (const i of !controller && mainTargets.length ? mainTargets : downstream) {
      // Prefer a true main-axis rejoin where one exists ahead; final local
      // approach nodes remain possible when the aircraft is already beyond it.
      const nodeId = assigned.nodeIds[i], list = targets.get(nodeId) ?? []; list.push(i); targets.set(nodeId, list);
    }
    let expanded = 0;
    while (heap.length && expanded++ < 80000) {
      const current = heap.pop(); if (compareFlowCost(current.cost, costs.get(current.id)!) !== 0) continue;
      const state = states.get(current.id)!;
      for (const suffixIndex of targets.get(state.node) ?? []) {
        const connectorEdges: string[] = [], connectorNodes = [state.node]; let key = current.id;
        while (key !== first) { const p = previous.get(key)!; connectorEdges.push(p.edge); connectorNodes.push(states.get(p.key)!.node); key = p.key; }
        connectorEdges.reverse(); connectorNodes.reverse();
        const nodeIds = [...prefix.nodeIds, ...connectorNodes.slice(1), ...assigned.nodeIds.slice(suffixIndex + 1)];
        const edgeIds = [...prefix.edgeIds, ...connectorEdges, ...assigned.edgeIds.slice(suffixIndex)];
        const cumulativeDistances = [0]; for (const id of edgeIds) cumulativeDistances.push(cumulativeDistances.at(-1)! + this.graph.edges.get(id)!.length);
        let candidate: Route = { nodeIds, edgeIds, cumulativeDistances, distance: cumulativeDistances.at(-1)!, flow: assigned.flow, aircraft: assigned.aircraft, operational: assigned.operational, assignment: assigned.assignment };
        if (controller) { const approved = this.approveControllerRoute(candidate, options); if (!approved) continue; candidate = approved; }
        try { candidate = this.describeRoute(candidate, movement, profile); } catch { continue; }
        if (!this.isRouteAllowed(candidate, movement, options)) continue;
        return { route: candidate, rejoinNodeId: state.node, rejoinDistance: prefix.distance + current.cost[3] };
      }
      for (const arc of this.graph.adjacency.get(state.node) ?? []) {
        const edge = this.graph.edges.get(arc.edgeId)!;
        // A recovery connector cannot pass through a stand, reverse its incoming
        // edge or borrow the previous route history to make a shortcut.
        if (edge.kind === 'parking_position' || edge.id === state.incoming || prefix.nodeIds.slice(0, -1).includes(arc.to)
          || !this.allowArc(edge, state.node, arc.to, movement, profile) || !this.allowTurn(state.incoming, state.node, edge.id, movement, profile)) continue;
        const nextParking = rules?.parkingPhase(edge, state.parking, origin, destination) ?? (rules ? null : state.parking); if (nextParking === null) continue;
        const step = policy && context && state.flow ? policy.step(context, state.flow, edge) : undefined; if (policy && !step) continue;
        const next: State = { node: arc.to, incoming: edge.id, parking: nextParking, flow: step?.state };
        const cost: FlowCost = [current.cost[0] + (step?.apronTransit ?? 0), current.cost[1] + (step?.reentries ?? 0), current.cost[2] + arc.length + (policy?.quality.curvature(state.incoming, state.node, edge).costMetres ?? 0) + (step?.axisChanges ?? 0) * MAIN_AXIS_CHANGE_METRES, current.cost[3] + arc.length];
        const key = keyOf(next), old = costs.get(key); if (old && compareFlowCost(cost, old) >= 0) continue;
        states.set(key, next); costs.set(key, cost); previous.set(key, { key: current.id, edge: edge.id }); heap.push({ id: key, cost, score: cost });
      }
    }
    return null;
  }

  /** An undirected over-approximation only rejects disconnected profile
   * regions. Directions, turn state, apron gates and clearance rules still
   * belong to the actual search and replay; this cannot invent a route. */
  private connectedForAircraft(from: string, to: string, movement: MovementMode, profile: AircraftProfile): boolean {
    if (!this.aircraftRestrictions) return true;
    const key = JSON.stringify([this.configuration, movement, profile.typeCode, profile.wingspanMetres, normalizedAirportFlags(this.aircraftRestrictionContext)]);
    let cache = this.aircraftConnectivity;
    if (!cache || cache.key !== key || cache.rules !== this.operatingRules || cache.flow !== this.flowPolicy || cache.aircraft !== this.aircraftRestrictions) {
      const components = new Map([...this.graph.nodes.keys()].map(id => [id, id]));
      const root = (id: string): string => {
        let current = id;
        while (components.get(current)! !== current) current = components.get(current)!;
        while (components.get(id)! !== id) { const next = components.get(id)!; components.set(id, current); id = next; }
        return current;
      };
      for (const edge of this.graph.edges.values()) {
        if (!this.allowArc(edge, edge.from, edge.to, movement, profile) && !this.allowArc(edge, edge.to, edge.from, movement, profile)) continue;
        const first = root(edge.from), second = root(edge.to); if (first !== second) components.set(first, second);
      }
      for (const id of components.keys()) components.set(id, root(id));
      cache = { key, rules: this.operatingRules, flow: this.flowPolicy, aircraft: this.aircraftRestrictions, components }; this.aircraftConnectivity = cache;
    }
    return cache.components.has(from) && cache.components.get(from) === cache.components.get(to);
  }

  /** Hard flow gates precede lexicographic apron cost and bounded manoeuvre preferences. */
  private findFlowRoute(from: string, to: string, options: RouteRequest): Route | null {
    const policy = this.flowPolicy!, movement = options.movement ?? 'transfer', rules = this.operatingRules;
    const profile = options.aircraftProfile ?? this.aircraftProfile;
    if (!policy.valid || rules && !rules.valid || !this.graph.areConnected(from, to) || !this.connectedForAircraft(from, to, movement, profile)) {
      this.lastFlowFailure = 'Le graphe ou les contraintes de cheminement ne permettent pas de construire ce parcours.'; return null;
    }
    const context = policy.compile({ ...options, from, to, configuration: this.configuration, movement, runway: this.requestRunway(options, from, to) });
    type State = { node: string; incoming: string | null; parking: 'start' | 'taxi' | 'destination'; flow: FlowState };
    const keyOf = (s: State) => JSON.stringify([s.node, s.incoming, s.parking, s.flow.stage, s.flow.mainPhase, Number(s.flow.originExited), Number(s.flow.destinationEntered), Number(s.flow.lastMainMatches), s.flow.lastMainAxisIds]);
    const initial: State = { node: from, incoming: null, parking: 'start', flow: policy.initialState() }, initialKey = keyOf(initial);
    const states = new Map<string, State>([[initialKey, initial]]), costs = new Map<string, FlowCost>([[initialKey, [0, 0, 0, 0]]]);
    const previous = new Map<string, { key: string; edge: string }>(), heap = new FlowHeap();
    heap.push({ id: initialKey, cost: [0, 0, 0, 0], score: [0, 0, this.graph.distance(from, to), this.graph.distance(from, to)] });
    while (heap.length) {
      const current = heap.pop(); if (compareFlowCost(current.cost, costs.get(current.id)!) !== 0) continue;
      const state = states.get(current.id)!;
      if (state.node === to && policy.complete(context, state.flow)) {
        const nodeIds = [to], edgeIds: string[] = []; let key = current.id;
        while (key !== initialKey) { const p = previous.get(key)!; edgeIds.push(p.edge); nodeIds.push(states.get(p.key)!.node); key = p.key; }
        nodeIds.reverse(); edgeIds.reverse(); const cumulativeDistances = [0];
        for (const id of edgeIds) cumulativeDistances.push(cumulativeDistances.at(-1)! + this.graph.edges.get(id)!.length);
        const route: Route = { nodeIds, edgeIds, cumulativeDistances, distance: cumulativeDistances.at(-1)! };
        route.flow = policy.describe(route, context, current.cost);
        return this.describeRoute(route, movement, profile);
      }
      for (const arc of this.graph.adjacency.get(state.node)!) {
        const edge = this.graph.edges.get(arc.edgeId)!;
        if (!this.allowArc(edge, state.node, arc.to, movement, profile) || !this.allowTurn(state.incoming, state.node, edge.id, movement, profile)) continue;
        const parking = rules ? rules.parkingPhase(edge, state.parking, from, to) : state.parking; if (parking === null) continue;
        const step = policy.step(context, state.flow, edge);
        if (!step || arc.to === to && !policy.complete(context, step.state)) continue;
        const next: State = { node: arc.to, incoming: edge.id, parking, flow: step.state }, key = keyOf(next);
        const curvature = policy.quality.curvature(state.incoming, state.node, edge);
        const cost: FlowCost = [current.cost[0] + step.apronTransit, current.cost[1] + step.reentries,
          current.cost[2] + arc.length + curvature.costMetres + step.axisChanges * MAIN_AXIS_CHANGE_METRES, current.cost[3] + arc.length];
        const old = costs.get(key); if (old && compareFlowCost(cost, old) >= 0) continue;
        states.set(key, next); costs.set(key, cost); previous.set(key, { key: current.id, edge: edge.id });
        heap.push({ id: key, cost, score: [cost[0], cost[1], cost[2] + this.graph.distance(arc.to, to), cost[3] + this.graph.distance(arc.to, to)] });
      }
    }
    this.lastFlowFailure = context.stages.length
      ? `Aucun parcours nominal n’atteint les raccordements publiés (${context.stages.map(s => s.reason).filter((s, i, a) => a.indexOf(s) === i).join(' ')}). Les interdictions et fermetures restent appliquées.`
      : 'Aucun parcours cohérent disponible avec les contraintes et fermetures sélectionnées.';
    return null;
  }

  /** Previous edge is part of search state, so a forbidden turn cannot be bypassed by node-only A*. */
  private findOperationalRoute(from: string, to: string, movement?: MovementMode, profile: AircraftProfile = this.aircraftProfile): Route | null {
    const rules = this.operatingRules;
    if (rules && !rules.valid || !this.graph.areConnected(from, to) || !this.connectedForAircraft(from, to, movement ?? 'transfer', profile)) return null;
    if (from === to) return this.describeRoute({ nodeIds: [from], edgeIds: [], distance: 0, cumulativeDistances: [0] }, movement, profile);
    const initialKey = JSON.stringify([from, null, 'start']);
    const states = new Map<string, { node: string; incoming: string | null; phase: 'start' | 'taxi' | 'destination' }>([[initialKey, { node: from, incoming: null, phase: 'start' }]]);
    const costs = new Map<string, number>([[initialKey, 0]]), previous = new Map<string, { key: string; edge: string }>();
    const heap = new MinHeap(); heap.push({ id: initialKey, score: this.graph.distance(from, to), cost: 0 });
    while (heap.length) {
      const current = heap.pop(); if (current.cost !== costs.get(current.id)) continue;
      const state = states.get(current.id)!;
      if (state.node === to) {
        const nodeIds = [to], edgeIds: string[] = []; let key = current.id;
        while (key !== initialKey) { const p = previous.get(key)!; edgeIds.push(p.edge); nodeIds.push(states.get(p.key)!.node); key = p.key; }
        nodeIds.reverse(); edgeIds.reverse(); const cumulativeDistances = [0];
        for (const id of edgeIds) cumulativeDistances.push(cumulativeDistances.at(-1)! + this.graph.edges.get(id)!.length);
        return this.describeRoute({ nodeIds, edgeIds, distance: current.cost, cumulativeDistances }, movement, profile);
      }
      for (const arc of this.graph.adjacency.get(state.node)!) {
        const edge = this.graph.edges.get(arc.edgeId)!;
        if (!this.allowArc(edge, state.node, arc.to, movement ?? 'transfer', profile) || !this.allowTurn(state.incoming, state.node, arc.edgeId, movement ?? 'transfer', profile)) continue;
        const phase = rules ? rules.parkingPhase(edge, state.phase, from, to) : state.phase; if (phase === null) continue;
        const key = JSON.stringify([arc.to, arc.edgeId, phase]), cost = current.cost + arc.length;
        if (cost + 1e-8 >= (costs.get(key) ?? Infinity)) continue;
        states.set(key, { node: arc.to, incoming: arc.edgeId, phase }); costs.set(key, cost); previous.set(key, { key: current.id, edge: arc.edgeId });
        heap.push({ id: key, score: cost + this.graph.distance(arc.to, to), cost });
      }
    }
    return null;
  }
}
