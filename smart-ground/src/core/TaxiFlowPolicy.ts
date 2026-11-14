import type { EdgeOperatingRule, RuleSource } from './OperatingRules';
import type { TaxiGraph } from './TaxiGraph';
import type { MovementMode, OperatingConfiguration, Route, RouteFlowData, TaxiEdge } from './types';
import { MAIN_AXIS_CHANGE_METRES, RouteQuality } from './RouteQuality';

export interface CorridorAxis { id: string; name: string; role: 'main' | 'connector' | 'apron'; edgeIds: string[]; sourceIds: string[] }
export interface CorridorZone { id: string; name: string; standIds: string[]; edgeIds?: string[]; portalEdgeIds?: string[]; sourceIds: string[] }
export interface CorridorSpecialArea { id: string; name: string; kind: 'deicing-pad'; restriction: 'no-normal-transit'; edgeIds: string[]; sourceIds: string[]; reason: string }
export interface NominalFlowPolicy {
  id: string; configuration: OperatingConfiguration | 'both'; movement: MovementMode;
  originZoneIds?: string[]; destinationZoneIds?: string[]; runways?: string[];
  destinationNodeIds?: string[];
  requiredAxisGroups?: string[][]; preferredAxisIds: string[];
  originPortalEdgeIds?: string[]; destinationPortalEdgeIds?: string[];
  firstMainAxisIds?: string[]; lastMainAxisIds?: string[];
  requireMainAxis?: boolean; differentZonesOnly?: boolean;
  sourceIds: string[]; reason: string; priority?: number;
}
export interface CorridorManifest {
  version: string | number; airport: 'LFPG'; dataSha256?: string; sources: RuleSource[];
  axes: CorridorAxis[]; zones: CorridorZone[]; flowPolicies: NominalFlowPolicy[];
  edgeRules?: EdgeOperatingRule[]; specialAreas?: CorridorSpecialArea[]; [key: string]: unknown;
}
export type TaxiFlowManifest = CorridorManifest;
export interface FlowContextOptions {
  configuration: OperatingConfiguration; movement: MovementMode; from: string; to: string;
  originId?: string; destinationId?: string; runway?: string;
}
export interface FlowState { stage: number; mainPhase: 0 | 1 | 2; originExited: boolean; destinationEntered: boolean; lastMainMatches: boolean; lastMainAxisIds: string }
/** Apron transit and re-entry precede bounded manoeuvre cost, then physical distance. */
export type FlowCost = readonly [number, number, number, number];
export interface CompiledTaxiFlow {
  options: FlowContextOptions; policyIds: string[]; sourceIds: string[];
  stages: { id: string; edgeIds: Set<string>; reason: string }[];
  originZones: Set<string>; destinationZones: Set<string>; warnings: string[];
  forbiddenPortalEdgeIds: Set<string>; firstMainAxisIds: Set<string>; lastMainAxisIds: Set<string>;
}

const active = (r: { configuration: string; movement?: string }, configuration: OperatingConfiguration, movement: MovementMode) =>
  (r.configuration === 'both' || r.configuration === configuration) && (!r.movement || r.movement === 'both' || r.movement === movement);

/** Published route patterns are distinct from legal prohibitions and geometry. */
export class TaxiFlowPolicy {
  readonly diagnostics: string[] = [];
  readonly valid: boolean;
  private axes = new Map<string, CorridorAxis>();
  private axesByEdge = new Map<string, CorridorAxis[]>();
  private zonesByStand = new Map<string, Set<string>>();
  private zonesByEdge = new Map<string, Set<string>>();
  private directions = new Map<string, EdgeOperatingRule[]>();
  readonly mainEdgeIds = new Set<string>();
  readonly specialAreaEdgeIds = new Set<string>();
  readonly quality: RouteQuality;

  constructor(readonly graph: TaxiGraph, readonly manifest: CorridorManifest) {
    this.quality = new RouteQuality(graph);
    if (!manifest || manifest.airport !== 'LFPG' || !manifest.version || !Array.isArray(manifest.sources) || !Array.isArray(manifest.axes) || !Array.isArray(manifest.zones) || !Array.isArray(manifest.flowPolicies)) throw new Error('Invalid LFPG corridor manifest');
    const sources = new Set(manifest.sources.map(s => s.id));
    if (sources.size !== manifest.sources.length) this.diagnostics.push('Duplicate corridor source identifier.');
    const checkSources = (ids: string[], subject: string) => {
      if (!Array.isArray(ids) || !ids.length || ids.some(id => !sources.has(id))) this.diagnostics.push(`${subject}: missing source provenance.`);
    };
    for (const axis of manifest.axes) {
      checkSources(axis.sourceIds, axis.id);
      if (this.axes.has(axis.id) || !['main', 'connector', 'apron'].includes(axis.role)) this.diagnostics.push(`${axis.id}: invalid or duplicate axis.`);
      this.axes.set(axis.id, axis);
      for (const id of axis.edgeIds) {
        if (!graph.edges.has(id)) { this.diagnostics.push(`${axis.id}: unknown edge ${id}.`); continue; }
        const list = this.axesByEdge.get(id) || []; list.push(axis); this.axesByEdge.set(id, list);
        if (axis.role === 'main') this.mainEdgeIds.add(id);
      }
    }
    const specialAreaIds = new Set<string>();
    for (const area of manifest.specialAreas || []) {
      checkSources(area.sourceIds, area.id);
      if (specialAreaIds.has(area.id) || area.kind !== 'deicing-pad' || area.restriction !== 'no-normal-transit' || !area.edgeIds.length) this.diagnostics.push(`${area.id}: invalid special area.`);
      specialAreaIds.add(area.id);
      for (const id of area.edgeIds) {
        if (!graph.edges.has(id)) this.diagnostics.push(`${area.id}: unknown special-area edge ${id}.`);
        else this.specialAreaEdgeIds.add(id);
      }
    }
    const zoneIds = new Set<string>();
    for (const zone of manifest.zones) {
      checkSources(zone.sourceIds, zone.id);
      if (zoneIds.has(zone.id)) this.diagnostics.push(`${zone.id}: duplicate apron zone.`);
      zoneIds.add(zone.id);
      for (const standId of zone.standIds) { const list = this.zonesByStand.get(standId) || new Set(); list.add(zone.id); this.zonesByStand.set(standId, list); }
      for (const id of zone.portalEdgeIds || []) if (!graph.edges.has(id)) this.diagnostics.push(`${zone.id}: unknown portal ${id}.`);
      for (const id of zone.edgeIds || []) {
        if (!graph.edges.has(id)) { this.diagnostics.push(`${zone.id}: unknown apron edge ${id}.`); continue; }
        const list = this.zonesByEdge.get(id) || new Set(); list.add(zone.id); this.zonesByEdge.set(id, list);
      }
    }
    const policyIds = new Set<string>();
    for (const policy of manifest.flowPolicies) {
      checkSources(policy.sourceIds, policy.id);
      if (policyIds.has(policy.id) || !['east', 'west', 'both'].includes(policy.configuration) || !['arrival', 'departure', 'transfer'].includes(policy.movement)) this.diagnostics.push(`${policy.id}: invalid nominal flow.`);
      policyIds.add(policy.id);
      for (const zone of [...policy.originZoneIds || [], ...policy.destinationZoneIds || []]) if (!zoneIds.has(zone)) this.diagnostics.push(`${policy.id}: unknown zone ${zone}.`);
      for (const node of policy.destinationNodeIds || []) if (!graph.nodes.has(node)) this.diagnostics.push(`${policy.id}: unknown destination node ${node}.`);
      for (const id of [...policy.preferredAxisIds || [], ...policy.firstMainAxisIds || [], ...policy.lastMainAxisIds || [], ...(policy.requiredAxisGroups || []).flat()]) if (!this.axes.has(id)) this.diagnostics.push(`${policy.id}: unknown corridor axis ${id}.`);
      for (const id of [...policy.originPortalEdgeIds || [], ...policy.destinationPortalEdgeIds || []]) if (!graph.edges.has(id)) this.diagnostics.push(`${policy.id}: unknown portal edge ${id}.`);
    }
    const ruleIds = new Set<string>();
    for (const rule of manifest.edgeRules || []) {
      checkSources(rule.sourceIds, rule.id);
      if (ruleIds.has(rule.id) || !['east', 'west', 'both'].includes(rule.configuration) || !['forward', 'reverse', 'closed'].includes(rule.direction)) this.diagnostics.push(`${rule.id}: invalid corridor direction.`);
      ruleIds.add(rule.id);
      for (const id of rule.edgeIds) {
        if (!graph.edges.has(id)) { this.diagnostics.push(`${rule.id}: unknown direction edge ${id}.`); continue; }
        const rules = this.directions.get(id) || []; rules.push(rule); this.directions.set(id, rules);
      }
    }
    this.valid = this.diagnostics.length === 0;
  }

  allowArc(edge: TaxiEdge, from: string, to: string, configuration: OperatingConfiguration, movement: MovementMode): boolean {
    if (!this.valid || this.specialAreaEdgeIds.has(edge.id) || !((edge.from === from && edge.to === to) || (edge.to === from && edge.from === to))) return false;
    return (this.directions.get(edge.id) || []).filter(r => active(r, configuration, movement)).every(r => r.direction !== 'closed' && (r.direction === 'forward' ? edge.from === from : edge.to === from));
  }

  edgeDirection(edgeId: string, configuration: OperatingConfiguration, movement: MovementMode): 'forward' | 'reverse' | 'closed' | null {
    const edge = this.graph.edges.get(edgeId); if (!edge) return null;
    if (this.specialAreaEdgeIds.has(edgeId)) return 'closed';
    const rules = (this.directions.get(edgeId) || []).filter(r => active(r, configuration, movement));
    if (!rules.length) return null;
    const forward = this.allowArc(edge, edge.from, edge.to, configuration, movement), reverse = this.allowArc(edge, edge.to, edge.from, configuration, movement);
    return forward ? reverse ? null : 'forward' : reverse ? 'reverse' : 'closed';
  }
  hasMappedStand(locationId: string): boolean { return !!this.zonesByStand.get(locationId)?.size; }

  private zonesAt(locationId: string | undefined, nodeId: string): Set<string> {
    if (locationId) return new Set(this.zonesByStand.get(locationId) || []);
    const result = new Set<string>();
    for (const location of this.graph.locations) if (location.nodeId === nodeId) for (const zone of this.zonesByStand.get(location.id) || []) result.add(zone);
    return result;
  }

  compile(options: FlowContextOptions): CompiledTaxiFlow {
    const originZones = this.zonesAt(options.originId, options.from), destinationZones = this.zonesAt(options.destinationId, options.to);
    const sameZone = [...originZones].some(id => destinationZones.has(id));
    const policies = this.manifest.flowPolicies.filter(p => active(p, options.configuration, options.movement)
      && (!p.originZoneIds?.length || p.originZoneIds.some(id => originZones.has(id)))
      && (!p.destinationZoneIds?.length || p.destinationZoneIds.some(id => destinationZones.has(id)))
      && (!p.destinationNodeIds?.length || p.destinationNodeIds.includes(options.to))
      && (!p.runways?.length || !!options.runway && p.runways.includes(options.runway))
      && (!p.differentZonesOnly || originZones.size > 0 && destinationZones.size > 0 && !sameZone))
      .sort((a, b) => (b.priority || 0) - (a.priority || 0) || a.id.localeCompare(b.id));
    const stages: CompiledTaxiFlow['stages'] = [];
    const add = (id: string, edges: Iterable<string>, reason: string) => {
      const edgeIds = new Set(edges);
      if (!edgeIds.size) return;
      const previous = stages.at(-1);
      if (!previous || previous.edgeIds.size !== edgeIds.size || ![...edgeIds].every(e => previous.edgeIds.has(e))) stages.push({ id, edgeIds, reason });
    };
    policies.forEach(p => { if (p.originPortalEdgeIds?.length) add(`${p.id}:egress`, p.originPortalEdgeIds, p.reason); });
    policies.forEach(p => (p.requiredAxisGroups || []).forEach((group, i) => add(`${p.id}:axis-${i}`, group.flatMap(id => this.axes.get(id)!.edgeIds), p.reason)));
    if (policies.some(p => p.requireMainAxis) && !stages.some(s => [...s.edgeIds].every(id => this.mainEdgeIds.has(id)))) {
      const preferredMain = policies.flatMap(p => p.preferredAxisIds).flatMap(id => this.axes.get(id)!.role === 'main' ? this.axes.get(id)!.edgeIds : []);
      add('main-axis-convergence', preferredMain.length ? preferredMain : this.mainEdgeIds, 'Rejoindre un axe principal avant la desserte finale.');
    }
    policies.forEach(p => { if (p.destinationPortalEdgeIds?.length) add(`${p.id}:ingress`, p.destinationPortalEdgeIds, p.reason); });
    const forbiddenPortalEdgeIds = new Set<string>();
    const zoneById = new Map(this.manifest.zones.map(z => [z.id, z]));
    for (const p of policies) for (const [allowed, zoneIds] of [[p.originPortalEdgeIds, p.originZoneIds || [...originZones]], [p.destinationPortalEdgeIds, p.destinationZoneIds || [...destinationZones]]] as const) {
      if (!allowed?.length) continue;
      for (const zoneId of zoneIds) for (const id of zoneById.get(zoneId)?.portalEdgeIds || []) if (!allowed.includes(id)) forbiddenPortalEdgeIds.add(id);
    }
    const warnings = policies.length ? [] : ['Aucun cheminement nominal MANEX ne correspond exactement à ces références ; seule la préférence de continuité des axes peut être évaluée.'];
    const sourceIsStand = options.originId ? this.graph.locations.some(l => l.id === options.originId && l.kind === 'stand') : this.graph.locations.some(l => l.nodeId === options.from && l.kind === 'stand');
    const destinationIsStand = options.destinationId ? this.graph.locations.some(l => l.id === options.destinationId && l.kind === 'stand') : this.graph.locations.some(l => l.nodeId === options.to && l.kind === 'stand');
    if (sourceIsStand && !originZones.size) warnings.push('L’aire du stand de départ n’est pas appariée au MANEX ; son raccordement local reste à vérifier.');
    if (destinationIsStand && !destinationZones.size) warnings.push('L’aire du stand d’arrivée n’est pas appariée au MANEX ; son raccordement local reste à vérifier.');
    return { options, policyIds: policies.map(p => p.id), sourceIds: [...new Set(policies.flatMap(p => p.sourceIds))], stages, originZones, destinationZones,
      forbiddenPortalEdgeIds, firstMainAxisIds: new Set(policies.flatMap(p => p.firstMainAxisIds || [])), lastMainAxisIds: new Set(policies.flatMap(p => p.lastMainAxisIds || [])),
      warnings };
  }

  initialState(): FlowState { return { stage: 0, mainPhase: 0, originExited: false, destinationEntered: false, lastMainMatches: false, lastMainAxisIds: '' }; }
  complete(context: CompiledTaxiFlow, state: FlowState) { return state.stage === context.stages.length && (!context.lastMainAxisIds.size || state.lastMainMatches); }

  step(context: CompiledTaxiFlow, state: FlowState, edge: TaxiEdge): { state: FlowState; apronTransit: number; reentries: number; axisChanges: number } | null {
    if (context.forbiddenPortalEdgeIds.has(edge.id) || this.specialAreaEdgeIds.has(edge.id)) return null;
    let stage = state.stage;
    if (context.stages[stage]?.edgeIds.has(edge.id)) stage++;
    const zones = this.zonesByEdge.get(edge.id) || new Set<string>();
    const inOrigin = [...zones].some(id => context.originZones.has(id)), inDestination = [...zones].some(id => context.destinationZones.has(id));
    const isMain = this.mainEdgeIds.has(edge.id);
    const declaredAxes = this.axesByEdge.get(edge.id) || [];
    if (isMain && state.mainPhase === 0 && context.firstMainAxisIds.size && !declaredAxes.some(a => context.firstMainAxisIds.has(a.id))) return null;
    const isConnector = declaredAxes.some(a => a.role === 'connector');
    // Corridors and their junction connectors are not apron transit, even where
    // a broad apron source polygon touches them.
    const isApron = !isMain && !isConnector && (zones.size > 0 || edge.kind === 'parking_position' || declaredAxes.some(a => a.role === 'apron'));
    const foreign = isApron && zones.size > 0 && !inOrigin && !inDestination;
    const originReturn = isApron && inOrigin && state.originExited && !inDestination;
    const destinationEscape = state.destinationEntered && isApron && !inDestination && !inOrigin;
    const apronTransit = foreign || originReturn || destinationEscape ? edge.length : 0;
    const reentries = isMain && state.mainPhase === 2 ? 1 : 0;
    const mainAxes = declaredAxes.filter(a => a.role === 'main').map(a => a.id).sort();
    const previousAxes = state.lastMainAxisIds ? state.lastMainAxisIds.split('|') : [];
    const axisChanges = mainAxes.length && previousAxes.length && !mainAxes.some(id => previousAxes.includes(id)) ? 1 : 0;
    const mainPhase = isMain ? 1 : isApron && state.mainPhase > 0 ? 2 : state.mainPhase;
    return { state: { stage, mainPhase,
      originExited: state.originExited || context.originZones.size > 0 && (isMain || isConnector || zones.size > 0 && !inOrigin),
      destinationEntered: state.destinationEntered || isApron && inDestination,
      lastMainMatches: isMain && context.lastMainAxisIds.size ? declaredAxes.some(a => context.lastMainAxisIds.has(a.id)) : state.lastMainMatches,
      lastMainAxisIds: mainAxes.length ? mainAxes.join('|') : state.lastMainAxisIds }, apronTransit, reentries, axisChanges };
  }

  describe(route: Route, context: CompiledTaxiFlow, cost: FlowCost): RouteFlowData {
    const axes: string[] = [];
    for (const id of route.edgeIds) for (const axis of this.axesByEdge.get(id) || []) if (axis.role === 'main' && axes.at(-1) !== axis.name) axes.push(axis.name);
    const warnings = [...context.warnings];
    if (cost[0] > .001) warnings.push('Le parcours traverse une aire intermédiaire ou réentre dans une aire déjà quittée ; coordination ATC à vérifier.');
    if (cost[1] > 0) warnings.push('Le parcours quitte puis rejoint un axe principal ; vérifier le raccordement nominal.');
    const applicableDirections = route.edgeIds.flatMap(id => (this.directions.get(id) || []).filter(r => active(r, context.options.configuration, context.options.movement)));
    let significantTurns = 0, mainAxisChanges = 0, turnCostMetres = 0, curvatureDegrees = 0, curvatureCostMetres = 0, state = this.initialState();
    route.edgeIds.forEach((id, index) => {
      const edge = this.graph.edges.get(id)!, step = this.step(context, state, edge);
      if (step) { mainAxisChanges += step.axisChanges; state = step.state; }
      const quality = this.quality.junction(index ? route.edgeIds[index - 1] : null, route.nodeIds[index], edge);
      significantTurns += quality.significantTurns; turnCostMetres += quality.turnCostMetres;
      const curvature = this.quality.curvature(index ? route.edgeIds[index - 1] : null, route.nodeIds[index], edge);
      curvatureDegrees += curvature.degrees; curvatureCostMetres += curvature.costMetres;
    });
    return { status: warnings.length ? 'review' : 'nominal', policyIds: context.policyIds, sourceIds: [...new Set([...context.sourceIds, ...applicableDirections.flatMap(r => r.sourceIds)])],
      axes, apronTransitMetres: cost[0], mainAxisReentries: cost[1], physicalDistance: route.distance,
      significantTurns, mainAxisChanges, turnCostMetres, curvatureDegrees, curvatureCostMetres,
      routingCostMetres: route.distance + curvatureCostMetres + mainAxisChanges * MAIN_AXIS_CHANGE_METRES,
      originId: context.options.originId, destinationId: context.options.destinationId,
      configuration: context.options.configuration, movement: context.options.movement, warnings, requiredStages: context.stages.map(s => s.id) };
  }
}

export function compareFlowCost(a: FlowCost, b: FlowCost): number {
  for (let i = 0; i < a.length; i++) { const difference = a[i] - b[i]; if (Math.abs(difference) > 1e-7) return difference; }
  return 0;
}
