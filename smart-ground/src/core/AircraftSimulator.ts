import { TaxiGraph } from './TaxiGraph';
import { RoutePlanner } from './RoutePlanner';
import { PriorityController } from './PriorityController';
import type { Aircraft, AircraftObservation, AircraftOptions, AircraftProfile, AirportLocation, MovementMode, Route, RouteClearanceCheckpoint, RouteDeviationEvent } from './types';
import { normalizeAircraftProfile, sameAircraftProfile } from './AircraftProfile';
import { normalizedAirportFlags } from './AircraftRestrictions';

export interface RouteEditRequest { anchorNodeIndex: number; targetNodeId: string }
export type RouteEditResult = { ok: true; route: Route; waypoints: string[] } | { ok: false; reason: string };
import { routeLegAt, snapshotAssignedRoute } from './RouteConformance';
import { TrafficScheduler } from './TrafficScheduler';

function randomGenerator(seed: number) {
  let state = seed >>> 0;
  return () => { state += 0x6D2B79F5; let t = state; t = Math.imul(t ^ t >>> 15, t | 1); t ^= t + Math.imul(t ^ t >>> 7, t | 61); return ((t ^ t >>> 14) >>> 0) / 4294967296; };
}

export class AircraftSimulator {
  readonly aircraft: Aircraft[] = [];
  time = 0;
  private nextId = 1;
  private nextDeviationId = 1;
  private pacedOriginGaps = new WeakMap<Aircraft, number>();
  private originLastActualRelease = new Map<string, number>();
  readonly planner: RoutePlanner;
  private priorityRulesRef: RoutePlanner['operatingRules'] = null;
  private priorityController: PriorityController | null = null;
  private routeValidation = new WeakMap<Aircraft, { rules: RoutePlanner['operatingRules']; flow: RoutePlanner['flowPolicy']; aircraftRules: RoutePlanner['aircraftRestrictions']; flags: string }>();
  private routeEditHistory = new WeakMap<Aircraft, { route: Route; assignedRoute: Route }[]>();
  constructor(readonly graph: TaxiGraph, planner?: RoutePlanner) { this.planner = planner ?? new RoutePlanner(graph); }

  addAircraft(originId: string, destinationId: string, options: AircraftOptions = {}): Aircraft | null {
    const from = this.planner.locations.find(p => p.id === originId), to = this.planner.locations.find(p => p.id === destinationId);
    if (!from?.nodeId || !to?.nodeId) return null;
    const rules = this.planner.operatingRules, movement = options.movement ?? (rules ? 'departure' : undefined);
    if (rules) {
      if (!rules.isLocationOpen(from.id) || !rules.isLocationOpen(to.id)) return null;
      if (movement === 'departure' && (from.kind !== 'stand' || !rules.isLocationAllowed(to.id, this.planner.configuration, 'departure'))) return null;
      if (movement === 'arrival' && (to.kind !== 'stand' || !rules.isLocationAllowed(from.id, this.planner.configuration, 'arrival'))) return null;
      if (movement === 'transfer' && (from.kind !== 'stand' || to.kind !== 'stand')) return null;
    }
    const aircraftProfile = normalizeAircraftProfile(options.aircraftProfile ?? this.planner.aircraftProfile);
    const route = this.planner.findRoute(from.nodeId, to.nodeId, { movement, originId, destinationId, aircraftProfile });
    if (!route || route.distance <= 0) return null;
    return this.addRoute(route, { ...options, movement, originId, destinationId, aircraftProfile });
  }

  addRoute(route: Route, options: AircraftOptions = {}): Aircraft {
    route = { ...route, assignment: route.assignment ?? { source: 'standard', revision: 0, waypointNodeIds: [] } };
    if (!route.nodeIds.length || route.edgeIds.length !== route.nodeIds.length - 1 || route.cumulativeDistances.length !== route.nodeIds.length) throw new Error('Invalid route');
    for (let i = 0; i < route.edgeIds.length; i++) {
      const edge = this.graph.edges.get(route.edgeIds[i]);
      if (!edge || !((edge.from === route.nodeIds[i] && edge.to === route.nodeIds[i + 1]) || (edge.to === route.nodeIds[i] && edge.from === route.nodeIds[i + 1]))) throw new Error('Route leaves the taxi graph');
    }
    const aircraftProfile = normalizeAircraftProfile(options.aircraftProfile ?? route.aircraft?.profile ?? this.planner.aircraftProfile);
    if (!this.planner.isRouteAllowed(route, options.movement ?? route.operational?.movement, { ...options, aircraftProfile })) throw new Error('Route violates the current operating rules, nominal flow, aircraft profile or configuration');
    route = this.planner.describeRoute(route, options.movement ?? route.operational?.movement, aircraftProfile);
    const serial = this.nextId++, speed = options.speed ?? 7.7;
    if (!Number.isFinite(speed) || speed <= 0) throw new Error('Taxi speed must be positive');
    if (options.distance !== undefined && !Number.isFinite(options.distance)) throw new Error('Route distance must be finite');
    if (options.scheduledAt !== undefined && (!Number.isFinite(options.scheduledAt) || options.scheduledAt < this.time)) throw new Error('Scheduled release must be at or after simulation time');
    const id = options.id ?? `ac-${serial}`;
    if (this.aircraft.some(a => a.id === id)) throw new Error(`Duplicate aircraft ${id}`);
    const aircraft: Aircraft = {
      id, callsign: options.callsign ?? `AFR${String(100 + serial).padStart(3, '0')}`,
      originId: options.originId ?? route.nodeIds[0], destinationId: options.destinationId ?? route.nodeIds.at(-1)!,
      route, aircraftProfile, distance: Math.max(0, Math.min(route.distance, options.distance ?? 0)), speed, state: 'taxiing',
      assignedRoute: snapshotAssignedRoute(route), standardRoute: snapshotAssignedRoute(route), conformance: { status: 'on-route', events: [], assignedNodeIndex: 0 }, scheduledAt: options.scheduledAt,
      x: 0, y: 0, heading: 0, edgeId: null, fromNodeId: route.nodeIds[0], toNodeId: route.nodeIds[0], edgeProgress: 0,
    };
    const pending = this.nextClearance(aircraft);
    if (pending && aircraft.distance > pending.distance + 1e-8) throw new Error('Cannot start beyond an uncleared runway checkpoint');
    this.updatePosition(aircraft);
    if (pending && aircraft.distance >= pending.distance - 1e-8) { aircraft.state = 'holding'; aircraft.waitingClearanceId = pending.id; }
    if (options.scheduledAt !== undefined && options.scheduledAt > this.time) aircraft.state = 'scheduled';
    this.routeValidation.set(aircraft, this.validationContext());
    this.aircraft.push(aircraft); return aircraft;
  }

  previewAssignedRoute(id: string, request: RouteEditRequest): RouteEditResult {
    const aircraft = this.aircraft.find(a => a.id === id);
    if (!aircraft) return { ok: false, reason: 'Avion introuvable.' };
    if (aircraft.state === 'arrived' || aircraft.conformance?.status && aircraft.conformance.status !== 'on-route') return { ok: false, reason: 'Le tracé ne peut être modifié pendant un écart ou après l’arrivée.' };
    const route = aircraft.route, anchor = request.anchorNodeIndex;
    if (!Number.isInteger(anchor) || anchor <= 0 || anchor >= route.nodeIds.length - 1 || route.cumulativeDistances[anchor] <= aircraft.distance + 1e-8) return { ok: false, reason: 'Choisissez un carrefour encore à parcourir ; origine, destination et trajet déjà parcouru restent fixes.' };
    const target = this.graph.nodes.get(request.targetNodeId), arcs = this.graph.adjacency.get(request.targetNodeId) || [];
    const taxi = arcs.filter(arc => this.graph.edges.get(arc.edgeId)?.kind === 'taxiway');
    if (!target || taxi.length < 2 || arcs.length < 3 && new Set(taxi.map(arc => this.graph.edges.get(arc.edgeId)!.name)).size < 2) return { ok: false, reason: 'Déposez le point sur une intersection réelle de taxiways.' };
    if (route.nodeIds[anchor] === target.id) return { ok: false, reason: 'Le carrefour choisi est inchangé.' };
    const currentLeg = routeLegAt(route, aircraft.distance), atNode = aircraft.distance === route.cumulativeDistances[currentLeg];
    const fixedNodeCount = Math.min(route.nodeIds.length, currentLeg + (atNode ? 1 : 2));
    if (route.nodeIds.slice(0, fixedNodeCount).includes(target.id)) return { ok: false, reason: 'Le raccordement ne peut pas revenir sur le trajet déjà parcouru ou déplacer l’avion.' };
    const waypoints = [...(aircraft.assignedRoute?.assignment?.waypointNodeIds ?? [])];
    const replacement = waypoints.indexOf(route.nodeIds[anchor]);
    if (waypoints.includes(target.id) && replacement !== waypoints.indexOf(target.id)) return { ok: false, reason: 'Cette intersection est déjà imposée dans le tracé attribué.' };
    if (replacement >= 0) waypoints[replacement] = target.id;
    else {
      const next = waypoints.findIndex(nodeId => route.nodeIds.indexOf(nodeId) > anchor);
      waypoints.splice(next < 0 ? waypoints.length : next, 0, target.id);
    }
    const movement = route.operational?.movement ?? route.flow?.movement ?? route.aircraft?.movement ?? 'transfer';
    const candidate = this.planner.findControllerRoute(route, fixedNodeCount, waypoints, { movement, originId: aircraft.originId, destinationId: aircraft.destinationId, aircraftProfile: aircraft.aircraftProfile }, (aircraft.assignedRoute?.assignment?.revision ?? 0) + 1);
    if (!candidate) return { ok: false, reason: 'Aucun raccordement autorisé par cette intersection : sens, virages, gabarit et accès restent appliqués.' };
    const passed = this.retainedClearances(aircraft, candidate);
    if (candidate.operational?.clearanceCheckpoints.some(point => point.distance < aircraft.distance - 1e-8 && !passed.includes(point.id))) return { ok: false, reason: 'Le tracé imposerait de passer un point d’attente sans autorisation.' };
    return { ok: true, route: candidate, waypoints };
  }

  editAssignedRoute(id: string, request: RouteEditRequest): RouteEditResult {
    const result = this.previewAssignedRoute(id, request); if (!result.ok) return result;
    const aircraft = this.aircraft.find(a => a.id === id)!;
    const history = this.routeEditHistory.get(aircraft) ?? [];
    history.push({ route: snapshotAssignedRoute(aircraft.route), assignedRoute: snapshotAssignedRoute(aircraft.assignedRoute ?? aircraft.route) });
    this.routeEditHistory.set(aircraft, history);
    this.commitRouteAssignment(aircraft, result.route, snapshotAssignedRoute(result.route));
    return result;
  }

  /** Undo never moves an aircraft back onto an old branch. Once the changed
   * portion was entered, a new explicit forward assignment is required. */
  undoLastRouteEdit(id: string): RouteEditResult {
    const aircraft = this.aircraft.find(a => a.id === id), history = aircraft && this.routeEditHistory.get(aircraft), previous = history?.at(-1);
    if (!aircraft || !previous) return { ok: false, reason: 'Aucune modification de tracé à annuler.' };
    if (aircraft.state === 'arrived' || aircraft.conformance?.status && aircraft.conformance.status !== 'on-route') return { ok: false, reason: 'Le tracé ne peut être rétabli pendant un écart ou après l’arrivée.' };
    const leg = routeLegAt(aircraft.route, aircraft.distance), atNode = aircraft.distance === aircraft.route.cumulativeDistances[leg];
    const count = Math.min(aircraft.route.nodeIds.length, leg + (atNode ? 1 : 2));
    if (previous.route.nodeIds.length < count || aircraft.route.nodeIds.slice(0, count).some((id, i) => previous.route.nodeIds[i] !== id)
      || aircraft.route.edgeIds.slice(0, count - 1).some((id, i) => previous.route.edgeIds[i] !== id)) return { ok: false, reason: 'L’avion a déjà emprunté la branche modifiée ; annuler le replacerait sur un autre trajet.' };
    const movement = previous.route.operational?.movement ?? previous.route.flow?.movement ?? previous.route.aircraft?.movement;
    if (!this.planner.isRouteAllowed(previous.route, movement, { originId: aircraft.originId, destinationId: aircraft.destinationId, aircraftProfile: aircraft.aircraftProfile })) return { ok: false, reason: 'Le précédent tracé n’est plus autorisé dans le contexte actuel.' };
    const passed = this.retainedClearances(aircraft, previous.route);
    if (previous.route.operational?.clearanceCheckpoints.some(point => point.distance < aircraft.distance - 1e-8 && !passed.includes(point.id))) return { ok: false, reason: 'Le précédent tracé ne conserve pas les autorisations déjà nécessaires.' };
    history!.pop();
    this.commitRouteAssignment(aircraft, previous.route, previous.assignedRoute);
    return { ok: true, route: previous.route, waypoints: [...(previous.assignedRoute.assignment?.waypointNodeIds ?? [])] };
  }

  private commitRouteAssignment(aircraft: Aircraft, route: Route, assigned: Route): void {
    const pose = { x: aircraft.x, y: aircraft.y, heading: aircraft.heading }, scheduled = aircraft.state === 'scheduled';
    aircraft.standardRoute ??= snapshotAssignedRoute(aircraft.assignedRoute ?? aircraft.route);
    aircraft.clearedClearanceIds = this.retainedClearances(aircraft, route);
    aircraft.route = route; aircraft.assignedRoute = assigned;
    const leg = routeLegAt(route, aircraft.distance);
    const cursor = assigned.edgeIds.findIndex((id, i) => id === route.edgeIds[leg] && assigned.nodeIds[i] === route.nodeIds[leg] && assigned.nodeIds[i + 1] === route.nodeIds[leg + 1]);
    aircraft.conformance = { status: 'on-route', events: [...(aircraft.conformance?.events || [])], assignedNodeIndex: Math.max(0, cursor) };
    delete aircraft.yielding; delete aircraft.pendingWrongTurn; delete aircraft.waitingClearanceId;
    this.updatePosition(aircraft); Object.assign(aircraft, pose);
    aircraft.state = scheduled ? 'scheduled' : 'holding';
    const pending = this.nextClearance(aircraft);
    if (pending && pending.distance <= aircraft.distance + 1e-8) aircraft.waitingClearanceId = pending.id;
    this.routeValidation.set(aircraft, this.validationContext());
  }

  private retainedClearances(aircraft: Aircraft, candidate: Route): string[] {
    return (aircraft.route.operational?.clearanceCheckpoints || []).filter(old => old.distance < aircraft.distance - 1e-8 && aircraft.clearedClearanceIds?.includes(old.id)
      && candidate.operational?.clearanceCheckpoints.some(point => point.id === old.id && point.ruleId === old.ruleId && point.nodeId === old.nodeId && point.distance === old.distance)).map(point => point.id);
  }

  update(deltaSeconds: number): void {
    if (!Number.isFinite(deltaSeconds) || deltaSeconds < 0) throw new Error('Simulation delta must be finite and nonnegative');
    // Evaluate traffic priority before advancing; bounded steps preserve right-of-way
    // even when a caller advances many seconds in one update.
    if ((this.planner.operatingRules?.manifest.priorityRules?.length || this.aircraft.some(a => a.state === 'scheduled' || a.pendingWrongTurn)) && deltaSeconds > 1) {
      let remaining = deltaSeconds;
      while (remaining > 1e-8) { const step = Math.min(1, remaining); this.update(step); remaining -= step; }
      return;
    }
    const previousTime = this.time;
    this.time += deltaSeconds;
    // Observe before the integrator writes fresh route positions. A changed
    // graph-matched surface track must not be silently snapped back to plan.
    for (const aircraft of this.aircraft) {
      if (aircraft.state === 'scheduled' || aircraft.state === 'arrived' || aircraft.conformance?.status === 'unresolved' || !aircraft.edgeId) continue;
      const index = routeLegAt(aircraft.route, aircraft.distance), expected = this.graph.edges.get(aircraft.route.edgeIds[index]);
      const observed = this.graph.edges.get(aircraft.edgeId);
      if (!expected || !observed || !Number.isFinite(aircraft.edgeProgress) || aircraft.edgeProgress < 0 || aircraft.edgeProgress > 1) continue;
      const previousEndpoint = aircraft.edgeProgress === 1 && index > 0 && aircraft.distance === aircraft.route.cumulativeDistances[index] && observed.id === aircraft.route.edgeIds[index - 1] && aircraft.fromNodeId === aircraft.route.nodeIds[index - 1] && aircraft.toNodeId === aircraft.route.nodeIds[index];
      const assignment = aircraft.assignedRoute, assignedCursor = aircraft.conformance?.assignedNodeIndex ?? 0;
      const conformingToAssignment = aircraft.conformance?.status !== 'on-route' || !assignment || assignment.edgeIds.some((id, i) => (i >= assignedCursor || aircraft.edgeProgress === 1 && i === assignedCursor - 1) && id === observed.id && assignment.nodeIds[i] === aircraft.fromNodeId && assignment.nodeIds[i + 1] === aircraft.toNodeId);
      if (!conformingToAssignment || !previousEndpoint && (observed.id !== expected.id || aircraft.fromNodeId !== aircraft.route.nodeIds[index] || aircraft.toNodeId !== aircraft.route.nodeIds[index + 1])) {
        this.observeAircraft(aircraft.id, { edgeId: observed.id, fromNodeId: aircraft.fromNodeId, toNodeId: aircraft.toNodeId, edgeProgress: aircraft.edgeProgress });
      }
    }
    const currentContext = this.validationContext();
    for (const aircraft of this.aircraft) {
      if (aircraft.conformance?.status === 'unresolved' || aircraft.conformance?.awaitingApproval) { aircraft.state = 'holding'; delete aircraft.yielding; }
      const previous = this.routeValidation.get(aircraft);
      const profileChanged = !!aircraft.route.aircraft && (!aircraft.aircraftProfile || !sameAircraftProfile(aircraft.aircraftProfile, aircraft.route.aircraft.profile));
      if (profileChanged || !previous || previous.rules !== currentContext.rules || previous.flow !== currentContext.flow || previous.aircraftRules !== currentContext.aircraftRules || previous.flags !== currentContext.flags) {
        if (!this.isAircraftRouteAllowed(aircraft)) aircraft.state = 'holding';
        else aircraft.route = this.planner.describeRoute(aircraft.route, aircraft.route.operational?.movement ?? aircraft.route.flow?.movement ?? aircraft.route.aircraft?.movement, aircraft.aircraftProfile);
        this.routeValidation.set(aircraft, currentContext);
      }
    }
    if (this.priorityRulesRef !== this.planner.operatingRules) {
      this.priorityRulesRef = this.planner.operatingRules;
      this.priorityController = this.priorityRulesRef?.manifest.priorityRules?.length ? new PriorityController(this.graph, this.priorityRulesRef.manifest.priorityRules) : null;
    }
    const decisions = new Map((this.priorityController?.evaluate(this.aircraft, this.planner.configuration) ?? []).map(decision => [decision.aircraftId, decision]));
    for (const aircraft of this.aircraft) {
      const decision = decisions.get(aircraft.id);
      if (decision) {
        aircraft.yielding = { ruleId: decision.ruleId, priorityAircraftId: decision.priorityAircraftId, nodeId: decision.nodeId, reason: decision.reason, sourceIds: decision.sourceIds, stopDistance: decision.stopDistance };
      } else if (aircraft.yielding) {
        delete aircraft.yielding;
        if (aircraft.state === 'holding' && !aircraft.waitingClearanceId && !aircraft.conformance?.awaitingApproval && aircraft.conformance?.status !== 'unresolved' && aircraft.route.operational?.configuration === this.planner.configuration && this.isAircraftRouteAllowed(aircraft)) aircraft.state = 'taxiing';
      }
    }
    for (const aircraft of this.aircraft) {
      let activeDelta = deltaSeconds;
      if (aircraft.state === 'scheduled') {
        const origin = aircraft.route.nodeIds[0], gap = this.pacedOriginGaps.get(aircraft), lastRelease = this.originLastActualRelease.get(origin);
        if ((aircraft.scheduledAt ?? Infinity) > this.time || gap !== undefined && lastRelease !== undefined && this.time < lastRelease + gap
          || !this.isAircraftRouteAllowed(aircraft) || !this.originIsClear(aircraft)) continue;
        const pending = this.nextClearance(aircraft);
        aircraft.state = pending && pending.distance <= aircraft.distance + 1e-8 ? 'holding' : 'taxiing';
        if (aircraft.state === 'holding' && pending) aircraft.waitingClearanceId = pending.id;
        if (gap !== undefined) this.originLastActualRelease.set(origin, this.time);
        activeDelta = Math.max(0, this.time - Math.max(previousTime, aircraft.scheduledAt ?? this.time));
      }
      if (aircraft.state !== 'taxiing') continue;
      if (aircraft.conformance?.status === 'unresolved' || aircraft.conformance?.awaitingApproval) { aircraft.state = 'holding'; continue; }
      if (aircraft.route.operational && aircraft.route.operational.configuration !== this.planner.configuration) { aircraft.state = 'holding'; continue; }
      const pending = this.nextClearance(aircraft), target = Math.min(aircraft.route.distance, aircraft.distance + aircraft.speed * activeDelta);
      const wrongTurn = aircraft.pendingWrongTurn ? this.nextWrongTurn(aircraft, target) : null;
      if (wrongTurn && (!pending || wrongTurn.distance < pending.distance - 1e-8) && wrongTurn.distance < (aircraft.yielding?.stopDistance ?? Infinity)) {
        aircraft.distance = wrongTurn.distance; this.updatePosition(aircraft); delete aircraft.pendingWrongTurn;
        this.observeAircraft(aircraft.id, wrongTurn.observation); continue;
      }
      aircraft.distance = Math.min(target, pending?.distance ?? Infinity, aircraft.yielding?.stopDistance ?? Infinity);
      this.updatePosition(aircraft);
      if (pending && aircraft.distance >= pending.distance - 1e-8) { aircraft.state = 'holding'; aircraft.waitingClearanceId = pending.id; }
      else if (aircraft.yielding?.stopDistance !== undefined && aircraft.distance >= aircraft.yielding.stopDistance - 1e-8) aircraft.state = 'holding';
    }
  }

  setHolding(id: string, holding: boolean): void {
    const aircraft = this.aircraft.find(a => a.id === id);
    if (!aircraft || aircraft.state === 'scheduled' || aircraft.conformance?.status === 'unresolved') return;
    if (!holding && aircraft.conformance?.awaitingApproval) {
      if (!this.isAircraftRouteAllowed(aircraft)) return;
      aircraft.conformance.awaitingApproval = false;
    }
    if (aircraft && holding) delete aircraft.yielding; // An explicit hold remains manual.
    if (aircraft && aircraft.state !== 'arrived' && !aircraft.waitingClearanceId && (!aircraft.route.operational || aircraft.route.operational.configuration === this.planner.configuration) && (holding || this.isAircraftRouteAllowed(aircraft))) aircraft.state = holding ? 'holding' : 'taxiing';
  }
  clearRunwayCrossing(id: string): boolean {
    const aircraft = this.aircraft.find(a => a.id === id);
    if (!aircraft?.waitingClearanceId || aircraft.state === 'scheduled' || aircraft.conformance?.status === 'unresolved' || aircraft.conformance?.awaitingApproval || aircraft.route.operational?.configuration !== this.planner.configuration || !this.isAircraftRouteAllowed(aircraft)) return false;
    aircraft.clearedClearanceIds = [...(aircraft.clearedClearanceIds ?? []), aircraft.waitingClearanceId];
    delete aircraft.waitingClearanceId; aircraft.state = 'taxiing'; return true;
  }
  private nextClearance(aircraft: Aircraft): RouteClearanceCheckpoint | undefined {
    return aircraft.route.operational?.clearanceCheckpoints.find(checkpoint => !aircraft.clearedClearanceIds?.includes(checkpoint.id));
  }
  private validationContext() { return { rules: this.planner.operatingRules, flow: this.planner.flowPolicy, aircraftRules: this.planner.aircraftRestrictions, flags: JSON.stringify(normalizedAirportFlags(this.planner.aircraftRestrictionContext)) }; }
  private isAircraftRouteAllowed(aircraft: Aircraft): boolean {
    return this.planner.isRouteAllowed(aircraft.route, aircraft.route.operational?.movement ?? aircraft.route.flow?.movement ?? aircraft.route.aircraft?.movement, { originId: aircraft.originId, destinationId: aircraft.destinationId, aircraftProfile: aircraft.aircraftProfile });
  }
  removeAircraft(id: string): void { const index = this.aircraft.findIndex(a => a.id === id); if (index >= 0) this.aircraft.splice(index, 1); }
  clear(): void { this.aircraft.length = 0; this.time = 0; this.nextId = 1; this.nextDeviationId = 1; this.originLastActualRelease.clear(); }

  private originIsClear(candidate: Aircraft): boolean {
    const origin = this.graph.nodes.get(candidate.route.nodeIds[0])!;
    return !this.aircraft.some(other => other !== candidate && other.state !== 'scheduled' && Math.hypot(other.x - origin.x, other.y - origin.y) < 60);
  }

  /** An explicit test action. Normal traffic never makes random wrong turns. */
  simulateWrongTurn(id: string): boolean {
    const aircraft = this.aircraft.find(a => a.id === id);
    if (!aircraft || aircraft.state === 'scheduled' || aircraft.state === 'arrived' || aircraft.conformance?.status !== 'on-route' || !this.nextWrongTurn(aircraft, Infinity)) return false;
    aircraft.pendingWrongTurn = true; return true;
  }

  private nextWrongTurn(aircraft: Aircraft, maximumDistance: number): { distance: number; observation: AircraftObservation } | null {
    const route = aircraft.route, movement = route.operational?.movement ?? route.flow?.movement ?? route.aircraft?.movement ?? 'transfer';
    const pending = this.nextClearance(aircraft);
    for (let i = 1; i < route.edgeIds.length; i++) {
      const distance = route.cumulativeDistances[i];
      if (distance < aircraft.distance - 1e-8 || distance > maximumDistance + 1e-8 || pending && distance >= pending.distance - 1e-8) continue;
      const node = route.nodeIds[i], incoming = route.edgeIds[i - 1], assigned = route.edgeIds[i];
      const alternative = (this.graph.adjacency.get(node) ?? []).find(arc => arc.edgeId !== incoming && arc.edgeId !== assigned && this.graph.edges.get(arc.edgeId)!.kind === 'taxiway'
        && this.planner.allowArc(this.graph.edges.get(arc.edgeId)!, node, arc.to, movement, aircraft.aircraftProfile)
        && this.planner.allowTurn(incoming, node, arc.edgeId, movement, aircraft.aircraftProfile));
      if (alternative) return { distance, observation: { edgeId: alternative.edgeId, fromNodeId: node, toNodeId: alternative.to, edgeProgress: 0 } };
    }
    return null;
  }

  /** Graph-matched observation. Position stays at the observed source geometry;
   * a recovery is held for explicit review, never an automatic clearance. */
  observeAircraft(id: string, observation: AircraftObservation): RouteDeviationEvent | null {
    const aircraft = this.aircraft.find(a => a.id === id);
    if (!aircraft) throw new Error(`Unknown aircraft ${id}`);
    if (aircraft.state === 'scheduled') throw new Error('A scheduled aircraft has no observed surface track yet');
    const edge = this.graph.edges.get(observation.edgeId);
    if (!edge || !Number.isFinite(observation.edgeProgress) || observation.edgeProgress < 0 || observation.edgeProgress > 1
      || !((edge.from === observation.fromNodeId && edge.to === observation.toNodeId) || (edge.to === observation.fromNodeId && edge.from === observation.toNodeId))) throw new Error('Observation must lie on one directed source edge');
    const route = aircraft.route, currentLeg = routeLegAt(route, aircraft.distance);
    aircraft.assignedRoute ??= snapshotAssignedRoute(route);
    aircraft.conformance ??= { status: 'on-route', events: [], assignedNodeIndex: 0 };
    const pending = this.nextClearance(aircraft);
    const matching = route.edgeIds.findIndex((edgeId, i) => (i >= currentLeg || observation.edgeProgress === 1 && i === currentLeg - 1 && aircraft.distance === route.cumulativeDistances[currentLeg]) && edgeId === observation.edgeId && route.nodeIds[i] === observation.fromNodeId && route.nodeIds[i + 1] === observation.toNodeId);
    const assigned = aircraft.assignedRoute;
    const assignmentMatches = aircraft.conformance.status !== 'on-route' || assigned.edgeIds.some((edgeId, i) => (i >= aircraft.conformance!.assignedNodeIndex || observation.edgeProgress === 1 && i === aircraft.conformance!.assignedNodeIndex - 1) && edgeId === observation.edgeId && assigned.nodeIds[i] === observation.fromNodeId && assigned.nodeIds[i + 1] === observation.toNodeId);
    const observedDistance = matching < 0 ? NaN : route.cumulativeDistances[matching] + edge.length * observation.edgeProgress;
    if (assignmentMatches && matching >= 0 && observedDistance >= aircraft.distance - .25 && (!pending || observedDistance <= pending.distance + 1e-8) && aircraft.conformance.status !== 'unresolved') {
      aircraft.distance = Math.max(0, observedDistance); this.updatePosition(aircraft); return null;
    }
    const expectedEdgeId = aircraft.conformance.status === 'on-route' ? assigned.edgeIds[aircraft.conformance.assignedNodeIndex] ?? null : route.edgeIds[currentLeg] ?? null;
    const opposite = route.edgeIds.some((edgeId, i) => i >= currentLeg && edgeId === observation.edgeId && route.nodeIds[i] === observation.toNodeId && route.nodeIds[i + 1] === observation.fromNodeId);
    const reason = matching >= 0 && pending && observedDistance > pending.distance + 1e-8 ? 'Position observée au-delà d’un point d’attente non autorisé.'
      : opposite ? `Sens opposé à l’itinéraire assigné sur ${edge.name || edge.id}.`
      : `Virage ou voie hors itinéraire assigné : ${edge.name || edge.id}.`;
    const from = this.graph.nodes.get(observation.fromNodeId)!, to = this.graph.nodes.get(observation.toNodeId)!;
    aircraft.x = from.x + (to.x - from.x) * observation.edgeProgress; aircraft.y = from.y + (to.y - from.y) * observation.edgeProgress;
    aircraft.heading = Math.atan2(to.y - from.y, to.x - from.x); aircraft.edgeId = edge.id; aircraft.fromNodeId = from.id; aircraft.toNodeId = to.id; aircraft.edgeProgress = observation.edgeProgress;
    aircraft.state = 'holding'; delete aircraft.yielding; delete aircraft.pendingWrongTurn;
    const prior = aircraft.conformance.events.at(-1);
    if (aircraft.conformance.status === 'unresolved' && prior && JSON.stringify(prior.observed) === JSON.stringify(observation)) return prior;
    const event: RouteDeviationEvent = { id: `deviation-${this.nextDeviationId++}`, aircraftId: aircraft.id, callsign: aircraft.callsign, time: this.time, reason, status: 'unresolved', observed: { ...observation }, expectedEdgeId };
    aircraft.conformance.events.push(event); aircraft.conformance.status = 'unresolved'; aircraft.conformance.awaitingApproval = true;
    delete aircraft.conformance.rejoinNodeId; delete aircraft.conformance.rejoinDistance;
    const divergenceIndex = route.nodeIds.findIndex((nodeId, i) => i >= currentLeg && nodeId === observation.fromNodeId);
    if (divergenceIndex < 0 || matching >= 0 && pending && observedDistance > pending.distance + 1e-8) return event;
    const prefixNodes = [...route.nodeIds.slice(0, divergenceIndex + 1), observation.toNodeId];
    const prefixEdges = [...route.edgeIds.slice(0, divergenceIndex), observation.edgeId], cumulativeDistances = [0];
    for (const id of prefixEdges) cumulativeDistances.push(cumulativeDistances.at(-1)! + this.graph.edges.get(id)!.length);
    const prefix: Route = { nodeIds: prefixNodes, edgeIds: prefixEdges, cumulativeDistances, distance: cumulativeDistances.at(-1)! };
    const profile = aircraft.aircraftProfile, movement = route.operational?.movement ?? route.flow?.movement ?? route.aircraft?.movement ?? 'transfer';
    const recovered = this.planner.findRecoveryRoute(prefix, aircraft.assignedRoute, aircraft.conformance.assignedNodeIndex + 1, { movement, originId: aircraft.originId, destinationId: aircraft.destinationId, aircraftProfile: profile });
    if (!recovered) return event;
    const distance = cumulativeDistances[divergenceIndex] + edge.length * observation.edgeProgress;
    const uncleared = recovered.route.operational?.clearanceCheckpoints.find(checkpoint => !aircraft.clearedClearanceIds?.includes(checkpoint.id));
    if (uncleared && uncleared.distance < distance - 1e-8) return event;
    aircraft.route = recovered.route; aircraft.distance = distance;
    delete aircraft.waitingClearanceId;
    if (uncleared && uncleared.distance <= distance + 1e-8) aircraft.waitingClearanceId = uncleared.id;
    aircraft.conformance.status = 'recovering'; aircraft.conformance.rejoinNodeId = recovered.rejoinNodeId; aircraft.conformance.rejoinDistance = recovered.rejoinDistance;
    event.status = 'recovering'; event.rejoinNodeId = recovered.rejoinNodeId;
    this.routeValidation.set(aircraft, this.validationContext());
    // Do not call updatePosition here: its exact-node boundary convention could
    // replace the observed wrong branch with the following segment at progress0.
    return event;
  }

  /** Balanced, paced traffic for the interactive simulation; homogeneous
   * generate() remains available for reproducible operational validators. */
  generateMixed(count: number, seed = 42, options: { aircraftProfile?: AircraftProfile; windowSeconds?: number } = {}): number {
    if (!Number.isInteger(count) || count < 0 || count > 100) throw new Error('Traffic count must be an integer from 0 to 100');
    this.clear();
    const scheduler = new TrafficScheduler(count, seed, { windowSeconds: options.windowSeconds }), random = randomGenerator(seed);
    const profile = normalizeAircraftProfile(options.aircraftProfile ?? this.planner.aircraftProfile);
    const locations = this.planner.locations.filter((l): l is AirportLocation & { nodeId: string } => !!l.nodeId && (!this.planner.operatingRules || this.planner.operatingRules.isLocationOpen(l.id)));
    const stands = locations.filter(l => l.kind === 'stand' && (!this.planner.flowPolicy || this.planner.flowPolicy.hasMappedStand(l.id)));
    const accesses = { departure: this.planner.availableLocations('departure').filter((l): l is AirportLocation & { nodeId: string } => !!l.nodeId), arrival: this.planner.availableLocations('arrival').filter((l): l is AirportLocation & { nodeId: string } => !!l.nodeId) };
    const remaining = { departure: [...new Map(stands.map(l => [l.nodeId, l])).values()], arrival: [...new Map(accesses.arrival.map(l => [l.nodeId, l])).values()] };
    const failed = new Set<string>();
    for (let attempt = 0; this.aircraft.length < count && attempt < Math.max(100, count * 120); attempt++) {
      const index = this.aircraft.length, movement = scheduler.movementAt(index), origins = movement === 'departure' ? stands : accesses.arrival, targets = movement === 'departure' ? accesses.departure : stands;
      if (!origins.length || !targets.length) break;
      const pool = remaining[movement], from = pool.length ? pool.splice(Math.floor(random() * pool.length), 1)[0] : origins[Math.floor(random() * origins.length)];
      const candidates = targets.filter(to => to.nodeId !== from.nodeId && this.graph.areConnected(from.nodeId, to.nodeId)); if (!candidates.length) continue;
      const to = candidates[Math.floor(random() * candidates.length)], key = `${movement}|${from.id}|${to.id}`; if (failed.has(key)) continue;
      const prefix = ['AFR', 'EZY', 'DLH', 'BAW', 'KLM', 'VLG'][Math.floor(random() * 6)];
      const aircraft = this.addAircraft(from.id, to.id, { movement, aircraftProfile: profile, speed: 5 + random() * 4, callsign: `${prefix}${1000 + index}` });
      if (!aircraft) { failed.add(key); continue; }
      if (aircraft.route.flow?.status === 'review' || aircraft.route.aircraft?.status === 'review') { this.removeAircraft(aircraft.id); failed.add(key); continue; }
      aircraft.scheduledAt = scheduler.allocate(index, from.nodeId);
      this.pacedOriginGaps.set(aircraft, scheduler.sameOriginGapSeconds);
      if (aircraft.scheduledAt > this.time || !this.originIsClear(aircraft)) aircraft.state = 'scheduled';
      else this.originLastActualRelease.set(from.nodeId, this.time);
    }
    return this.aircraft.length;
  }

  /** Replaces traffic. Disconnected locations are preserved in data but never spawned. */
  generate(count: number, seed = 42, options: { movement?: MovementMode; aircraftProfile?: AircraftProfile } = {}): number {
    if (!Number.isInteger(count) || count < 0 || count > 100) throw new Error('Traffic count must be an integer from 0 to 100');
    this.clear();
    const aircraftProfile = normalizeAircraftProfile(options.aircraftProfile ?? this.planner.aircraftProfile);
    const random = randomGenerator(seed);
    const locations = this.planner.locations.filter((p): p is AirportLocation & { nodeId: string } => !!p.nodeId && !!this.graph.adjacency.get(p.nodeId)?.length && (!this.planner.operatingRules || this.planner.operatingRules.isLocationOpen(p.id)));
    const stands = locations.filter(p => p.kind === 'stand' && (!this.planner.flowPolicy || this.planner.flowPolicy.hasMappedStand(p.id)));
    const movement = options.movement ?? (this.planner.operatingRules ? 'departure' : undefined);
    const accesses = this.planner.operatingRules ? this.planner.availableLocations(movement === 'arrival' ? 'arrival' : 'departure').filter((p): p is AirportLocation & { nodeId: string } => !!p.nodeId) : [];
    const starts = movement === 'arrival' && this.planner.operatingRules ? accesses : stands.length ? stands : locations;
    const runways = locations.filter(p => p.kind === 'runway' || p.kind === 'holding_point');
    // Try each source origin once before reusing compatible ones. Restricted
    // heavy types may have fewer than 100 usable stands; inaccessible stands
    // must not prevent reuse of the stands that actually passed route replay.
    const remainingStarts = [...new Map(starts.map(location => [location.nodeId, location])).values()];
    const failedPairs = new Set<string>();
    for (let attempt = 0; this.aircraft.length < count && attempt < Math.max(100, count * 80); attempt++) {
      if (starts.length < 2 && locations.length < 2) break;
      const from = remainingStarts.length ? remainingStarts.splice(Math.floor(random() * remainingStarts.length), 1)[0] : starts[Math.floor(random() * starts.length)];
      if (!from) continue;
      const targets = this.planner.operatingRules ? movement === 'departure' ? accesses : stands : (runways.length && random() < 0.7 ? runways : locations);
      const candidates = targets.filter(p => p.nodeId !== from.nodeId && !!p.nodeId && this.graph.areConnected(from.nodeId, p.nodeId));
      if (!candidates.length) continue;
      const to = candidates[Math.floor(random() * candidates.length)];
      const pairKey = `${from.id}|${to.id}`;
      if (failedPairs.has(pairKey)) continue;
      const aircraft = this.addAircraft(from.id, to.id, { movement, aircraftProfile, speed: 5 + random() * 4, callsign: `${['AFR', 'EZY', 'DLH', 'BAW', 'KLM', 'VLG'][Math.floor(random() * 6)]}${100 + Math.floor(random() * 8900)}` });
      if (aircraft?.route.flow?.status === 'review' || aircraft?.route.aircraft?.status === 'review') { this.removeAircraft(aircraft.id); failedPairs.add(pairKey); continue; }
      if (!aircraft) failedPairs.add(pairKey);
    }
    return this.aircraft.length;
  }

  private updatePosition(aircraft: Aircraft): void {
    const route = aircraft.route;
    if (!route.edgeIds.length) {
      const node = this.graph.nodes.get(route.nodeIds[0])!; aircraft.x = node.x; aircraft.y = node.y; aircraft.state = 'arrived'; return;
    }
    // A binary search supports large time steps without skipping geometric segments.
    let lo = 0, hi = route.edgeIds.length - 1;
    while (lo < hi) { const mid = (lo + hi) >> 1; if (route.cumulativeDistances[mid + 1] <= aircraft.distance) lo = mid + 1; else hi = mid; }
    const index = lo, edge = this.graph.edges.get(route.edgeIds[index])!;
    const from = this.graph.nodes.get(route.nodeIds[index])!, to = this.graph.nodes.get(route.nodeIds[index + 1])!;
    const progress = Math.max(0, Math.min(1, (aircraft.distance - route.cumulativeDistances[index]) / edge.length));
    aircraft.x = from.x + (to.x - from.x) * progress; aircraft.y = from.y + (to.y - from.y) * progress;
    aircraft.heading = Math.atan2(to.y - from.y, to.x - from.x); aircraft.edgeId = edge.id;
    aircraft.fromNodeId = from.id; aircraft.toNodeId = to.id; aircraft.edgeProgress = progress;
    if (aircraft.assignedRoute && aircraft.conformance) {
      const assigned = aircraft.assignedRoute;
      const indexOnAssignment = assigned.edgeIds.findIndex((id, i) => i >= aircraft.conformance!.assignedNodeIndex && id === edge.id && assigned.nodeIds[i] === from.id && assigned.nodeIds[i + 1] === to.id);
      if (indexOnAssignment >= 0) aircraft.conformance.assignedNodeIndex = indexOnAssignment;
      if (aircraft.conformance.status === 'recovering' && aircraft.conformance.rejoinDistance !== undefined && aircraft.distance >= aircraft.conformance.rejoinDistance) {
        aircraft.conformance.status = 'on-route'; aircraft.conformance.awaitingApproval = false;
        const event = aircraft.conformance.events.at(-1); if (event) event.status = 'rejoined';
      }
    }
    if (aircraft.distance >= route.distance - 1e-8) aircraft.state = 'arrived';
  }
}
