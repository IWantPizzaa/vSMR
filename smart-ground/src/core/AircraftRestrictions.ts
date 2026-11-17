import type { RuleSource } from './OperatingRules';
import type { TaxiGraph } from './TaxiGraph';
import type { AircraftProfile, AircraftRouteData, MovementMode, OperatingConfiguration, Route, TaxiEdge } from './types';
import { isUnknownAircraftType, normalizeAircraftProfile } from './AircraftProfile';

export interface AircraftRestrictionContext { airportFlags?: readonly string[] }
export interface AircraftCondition {
  typeCodes?: string[]; excludedTypeCodes?: string[];
  wingspanMetres?: { gt?: number; gte?: number; lt?: number; lte?: number };
  airportFlags?: string[];
}
export interface AircraftRule {
  id: string; configuration: OperatingConfiguration | 'both'; movement?: MovementMode | 'both';
  condition?: AircraftCondition; sourceIds: string[]; reason: string;
}
export interface AircraftEdgeRule extends AircraftRule { edgeIds: string[]; direction: 'closed' | 'forward' | 'reverse' }
export interface AircraftTurnRule extends AircraftRule { viaNodeId: string; fromEdgeId: string; toEdgeId: string }
export interface AircraftAdvisory extends AircraftRule { edgeIds?: string[]; viaNodeId?: string; fromEdgeId?: string; toEdgeId?: string }
export interface AircraftCoverageIssue extends AircraftRule { edgeIds?: string[]; nodeIds?: string[] }
export interface AircraftRestrictionManifest {
  version: string | number; airport: 'LFPG'; dataSha256?: string; sources: RuleSource[];
  edgeRules: AircraftEdgeRule[]; turnRules: AircraftTurnRule[]; advisories?: AircraftAdvisory[];
  coverageIssues?: AircraftCoverageIssue[]; [key: string]: unknown;
}
const strings = (value: unknown): value is string[] => Array.isArray(value) && value.every(x => typeof x === 'string' && x.length > 0);

export function normalizedAirportFlags(context: AircraftRestrictionContext = {}): string[] {
  if (context.airportFlags !== undefined && (!Array.isArray(context.airportFlags) || context.airportFlags.some(x => typeof x !== 'string' || !x))) throw new Error('Airport scenario flags must be non-empty strings');
  return [...new Set(context.airportFlags || [])].sort();
}

/** Independent hard overlay: aircraft conditions never relax operating rules. */
export class AircraftRestrictions {
  readonly diagnostics: string[] = [];
  readonly valid: boolean;
  private edges = new Map<string, AircraftEdgeRule[]>();
  private turns = new Map<string, AircraftTurnRule[]>();
  private profiles = new WeakMap<object, AircraftProfile>();
  constructor(readonly graph: TaxiGraph, readonly manifest: AircraftRestrictionManifest) {
    if (!manifest || manifest.airport !== 'LFPG' || !manifest.version || !Array.isArray(manifest.sources) || !Array.isArray(manifest.edgeRules) || !Array.isArray(manifest.turnRules)) throw new Error('Invalid LFPG aircraft restriction manifest');
    const sourceIds = new Set<string>(), ids = new Set<string>();
    for (const source of manifest.sources) {
      if (!source || typeof source.id !== 'string' || !source.id || typeof source.title !== 'string' || typeof source.url !== 'string' || sourceIds.has(source.id)) this.diagnostics.push('Invalid or duplicate aircraft restriction source.');
      else sourceIds.add(source.id);
    }
    const incident = (edgeId: string, nodeId: string) => { const edge = graph.edges.get(edgeId); return !!edge && (edge.from === nodeId || edge.to === nodeId); };
    const checkRule = (rule: AircraftRule) => {
      if (!rule || typeof rule.id !== 'string' || !rule.id || ids.has(rule.id)) { this.diagnostics.push('Missing or duplicate aircraft rule identifier.'); return false; }
      ids.add(rule.id);
      if (!['east', 'west', 'both'].includes(rule.configuration) || rule.movement !== undefined && !['departure', 'arrival', 'transfer', 'both'].includes(rule.movement)
        || typeof rule.reason !== 'string' || !strings(rule.sourceIds) || !rule.sourceIds.length || rule.sourceIds.some(id => !sourceIds.has(id))) this.diagnostics.push(`${rule.id}: invalid rule context or provenance.`);
      const condition = rule.condition;
      if (condition !== undefined) {
        if (!condition || typeof condition !== 'object' || Array.isArray(condition)) { this.diagnostics.push(`${rule.id}: invalid condition.`); return false; }
        if (Object.keys(condition).some(key => !['typeCodes', 'excludedTypeCodes', 'wingspanMetres', 'airportFlags'].includes(key))) this.diagnostics.push(`${rule.id}: unsupported condition.`);
        for (const key of ['typeCodes', 'excludedTypeCodes', 'airportFlags'] as const) if (condition[key] !== undefined && (!strings(condition[key]) || !condition[key]!.length)) this.diagnostics.push(`${rule.id}: invalid ${key}.`);
        for (const code of [...condition.typeCodes || [], ...condition.excludedTypeCodes || []]) if (!/^[A-Z0-9]{2,8}$/.test(code)) this.diagnostics.push(`${rule.id}: noncanonical aircraft type ${code}.`);
        if (condition.wingspanMetres !== undefined) {
          const bounds = condition.wingspanMetres;
          if (!bounds || typeof bounds !== 'object' || Array.isArray(bounds)) { this.diagnostics.push(`${rule.id}: invalid wingspan bounds.`); return false; }
          if (!Object.keys(bounds).length || Object.keys(bounds).some(key => !['gt', 'gte', 'lt', 'lte'].includes(key))
            || Object.values(bounds).some(value => !Number.isFinite(value) || value < 0) || bounds.gt !== undefined && bounds.gte !== undefined || bounds.lt !== undefined && bounds.lte !== undefined) this.diagnostics.push(`${rule.id}: invalid wingspan bounds.`);
          const lower = bounds.gt ?? bounds.gte ?? -Infinity, upper = bounds.lt ?? bounds.lte ?? Infinity;
          if (lower > upper || lower === upper && (bounds.gt !== undefined || bounds.lt !== undefined)) this.diagnostics.push(`${rule.id}: empty wingspan interval.`);
        }
      }
      return true;
    };
    for (const rule of manifest.edgeRules) {
      if (!checkRule(rule)) continue;
      if (!strings(rule.edgeIds) || !rule.edgeIds.length || !['closed', 'forward', 'reverse'].includes(rule.direction)) { this.diagnostics.push(`${rule.id}: invalid edge restriction.`); continue; }
      for (const id of new Set(rule.edgeIds)) {
        if (!graph.edges.has(id)) { this.diagnostics.push(`${rule.id}: unknown edge ${id}.`); continue; }
        const rules = this.edges.get(id) || []; rules.push(rule); this.edges.set(id, rules);
      }
    }
    for (const rule of manifest.turnRules) {
      if (!checkRule(rule)) continue;
      if (!graph.nodes.has(rule.viaNodeId) || !incident(rule.fromEdgeId, rule.viaNodeId) || !incident(rule.toEdgeId, rule.viaNodeId)) { this.diagnostics.push(`${rule.id}: invalid turn references.`); continue; }
      const rules = this.turns.get(rule.viaNodeId) || []; rules.push(rule); this.turns.set(rule.viaNodeId, rules);
    }
    for (const rule of manifest.advisories || []) {
      if (!checkRule(rule)) continue;
      if (rule.edgeIds && (!strings(rule.edgeIds) || rule.edgeIds.some(id => !graph.edges.has(id)))) this.diagnostics.push(`${rule.id}: unknown advisory edge.`);
      if (rule.viaNodeId && !graph.nodes.has(rule.viaNodeId) || rule.fromEdgeId && (!rule.viaNodeId || !incident(rule.fromEdgeId, rule.viaNodeId)) || rule.toEdgeId && (!rule.viaNodeId || !incident(rule.toEdgeId, rule.viaNodeId))) this.diagnostics.push(`${rule.id}: invalid advisory turn.`);
      if (!rule.edgeIds?.length && !rule.viaNodeId) this.diagnostics.push(`${rule.id}: advisory needs a physical scope.`);
    }
    for (const rule of manifest.coverageIssues || []) {
      if (!checkRule(rule)) continue;
      if (rule.edgeIds && (!strings(rule.edgeIds) || rule.edgeIds.some(id => !graph.edges.has(id)))) this.diagnostics.push(`${rule.id}: unknown coverage edge.`);
      if (rule.nodeIds && (!strings(rule.nodeIds) || rule.nodeIds.some(id => !graph.nodes.has(id)))) this.diagnostics.push(`${rule.id}: unknown coverage node.`);
    }
    this.valid = !this.diagnostics.length;
  }

  private normalizedProfile(profile: AircraftProfile): AircraftProfile | null {
    if (!profile || typeof profile !== 'object') return null;
    const cached = this.profiles.get(profile); if (cached) return cached;
    try { const normalized = normalizeAircraftProfile(profile); if (Object.isFrozen(profile)) this.profiles.set(profile, normalized); this.profiles.set(normalized, normalized); return normalized; }
    catch { return null; }
  }

  private matches(rule: AircraftRule, configuration: OperatingConfiguration, movement: MovementMode, profile: AircraftProfile, context: AircraftRestrictionContext): boolean {
    if ((rule.configuration !== 'both' && rule.configuration !== configuration) || rule.movement && rule.movement !== 'both' && rule.movement !== movement) return false;
    const condition = rule.condition; if (!condition) return true;
    const unknown = isUnknownAircraftType(profile);
    if (condition.typeCodes && !unknown && !condition.typeCodes.includes(profile.typeCode)) return false;
    // An unidentified type cannot claim a type-specific exemption.
    if (condition.excludedTypeCodes && !unknown && condition.excludedTypeCodes.includes(profile.typeCode)) return false;
    const span = profile.wingspanMetres, bounds = condition.wingspanMetres;
    if (bounds && (bounds.gt !== undefined && span <= bounds.gt || bounds.gte !== undefined && span < bounds.gte || bounds.lt !== undefined && span >= bounds.lt || bounds.lte !== undefined && span > bounds.lte)) return false;
    const flags = context.airportFlags || [];
    return !condition.airportFlags || condition.airportFlags.every(flag => flags.includes(flag));
  }

  edgeConstraints(edgeId: string, configuration: OperatingConfiguration, movement: MovementMode, profile: AircraftProfile, context: AircraftRestrictionContext = {}): AircraftEdgeRule[] {
    const normalized = this.normalizedProfile(profile); if (!normalized) return [];
    return (this.edges.get(edgeId) || []).filter(rule => this.matches(rule, configuration, movement, normalized, context));
  }
  allowArc(edge: TaxiEdge, from: string, to: string, configuration: OperatingConfiguration, movement: MovementMode, profile: AircraftProfile, context: AircraftRestrictionContext = {}): boolean {
    if (!this.valid || !this.normalizedProfile(profile) || !((edge.from === from && edge.to === to) || (edge.to === from && edge.from === to))) return false;
    return this.edgeConstraints(edge.id, configuration, movement, profile, context).every(rule => rule.direction !== 'closed' && (rule.direction === 'forward' ? edge.from === from : edge.to === from));
  }
  turnConstraints(previousEdgeId: string | null, viaNodeId: string, nextEdgeId: string, configuration: OperatingConfiguration, movement: MovementMode, profile: AircraftProfile, context: AircraftRestrictionContext = {}): AircraftTurnRule[] {
    const normalized = this.normalizedProfile(profile);
    return previousEdgeId && normalized ? (this.turns.get(viaNodeId) || []).filter(rule => rule.fromEdgeId === previousEdgeId && rule.toEdgeId === nextEdgeId && this.matches(rule, configuration, movement, normalized, context)) : [];
  }
  allowTurn(previousEdgeId: string | null, viaNodeId: string, nextEdgeId: string, configuration: OperatingConfiguration, movement: MovementMode, profile: AircraftProfile, context: AircraftRestrictionContext = {}): boolean {
    return this.valid && !!this.normalizedProfile(profile) && !this.turnConstraints(previousEdgeId, viaNodeId, nextEdgeId, configuration, movement, profile, context).length;
  }

  describeRoute(route: Route, configuration: OperatingConfiguration, movement: MovementMode, aircraftProfile: AircraftProfile, context: AircraftRestrictionContext = {}): AircraftRouteData {
    const profile = normalizeAircraftProfile(aircraftProfile), applied = new Map<string, AircraftRule>(), advisories = new Map<string, AircraftAdvisory>();
    for (let i = 0; i < route.edgeIds.length; i++) {
      for (const rule of this.edgeConstraints(route.edgeIds[i], configuration, movement, profile, context)) applied.set(rule.id, rule);
      for (const rule of this.turnConstraints(i ? route.edgeIds[i - 1] : null, route.nodeIds[i], route.edgeIds[i], configuration, movement, profile, context)) applied.set(rule.id, rule);
      for (const rule of this.manifest.advisories || []) if (this.matches(rule, configuration, movement, profile, context)
        && (rule.edgeIds?.includes(route.edgeIds[i]) || rule.viaNodeId === route.nodeIds[i] && (!rule.fromEdgeId || route.edgeIds[i - 1] === rule.fromEdgeId) && (!rule.toEdgeId || route.edgeIds[i] === rule.toEdgeId))) advisories.set(rule.id, rule);
    }
    const warnings = isUnknownAircraftType(profile) ? ['Type d’appareil non identifié : les restrictions spécifiques sont appliquées de manière conservatrice ; vérifier la compatibilité du type réel.'] : [];
    const sourceIds = new Set([...applied.values(), ...advisories.values()].flatMap(rule => rule.sourceIds));
    for (const issue of this.manifest.coverageIssues || []) if (this.matches(issue, configuration, movement, profile, context)
      && (!issue.edgeIds?.length && !issue.nodeIds?.length || issue.edgeIds?.some(id => route.edgeIds.includes(id)) || issue.nodeIds?.some(id => route.nodeIds.includes(id)))) {
      warnings.push(issue.reason); issue.sourceIds.forEach(id => sourceIds.add(id));
    }
    return { profile, configuration, movement, status: warnings.length ? 'review' : 'compatible', ruleIds: [...applied.keys()], sourceIds: [...sourceIds],
      advisories: [...advisories.values()].map(rule => ({ id: rule.id, reason: rule.reason, sourceIds: rule.sourceIds })), warnings, airportFlags: normalizedAirportFlags(context) };
  }
}
