/** JSON shape emitted by vSMR's existing ProxyBridgeSnapshot, not an ESB ABI. */
export interface BridgeProvider {
  major: number;
  minor: number;
  fields: Record<string, Record<string, unknown>>;
}

export interface BridgeSnapshot {
  callsigns: string[];
  providers: Record<string, BridgeProvider>;
}

/** Transport metadata belongs to Smart Ground; providers do not publish it. */
export interface BridgeImportEnvelope {
  format: 'smart-ground.bridge.v1';
  airport: 'LFPG';
  capturedAt: string;
  simulated: boolean;
  snapshot: BridgeSnapshot;
}

export type FieldState = 'value' | 'unset' | 'unavailable' | 'incompatible' | 'stale' | 'invalid';
export interface AssignedField {
  value: string | null;
  state: FieldState;
}

export interface ReadContext {
  stale: boolean;
}

export function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

export function normalizeCallsign(value: string): string {
  const result = value.trim().toUpperCase();
  return /^[A-Z0-9][A-Z0-9_-]{0,63}$/.test(result) ? result : '';
}

/** Preserve numeric padding and stand suffixes; fuzzy stand matching is unsafe. */
export function normalizeStandName(value: string): string {
  return value.trim().toUpperCase();
}

export function normalizeRunwayName(value: string): string {
  return value.trim().toUpperCase();
}

export function readAircraftField(
  snapshot: BridgeSnapshot,
  providerId: string,
  field: string,
  callsign: string,
  maximumBytes: number,
  context: ReadContext,
  sanitizeControls = false,
): AssignedField {
  if (context.stale) return { value: null, state: 'stale' };
  const provider = Object.hasOwn(snapshot.providers, providerId) ? snapshot.providers[providerId] : undefined;
  if (!provider) return { value: null, state: 'unavailable' };
  if (provider.major !== 1) return { value: null, state: 'incompatible' };
  const values = provider.fields[field];
  if (!values) return { value: null, state: 'unavailable' };
  if (!Object.hasOwn(values, callsign)) return { value: null, state: 'unset' };
  const raw = values[callsign];
  if (typeof raw !== 'string' || raw.includes('\0') || new TextEncoder().encode(raw).length > maximumBytes) {
    return { value: null, state: 'invalid' };
  }
  let value = raw.trim();
  if (sanitizeControls) value = value.replace(/[\x01-\x1f\x7f]/g, ' ').trim();
  else if (/[\x00-\x1f\x7f]/.test(value)) return { value: null, state: 'invalid' };
  return value ? { value, state: 'value' } : { value: null, state: 'unset' };
}

/** Validate bounded JSON before any assignment reaches the graph or the UI. */
export function validateEnvelope(input: unknown): BridgeImportEnvelope {
  if (!isRecord(input) || input.format !== 'smart-ground.bridge.v1') {
    throw new Error('Format attendu : smart-ground.bridge.v1.');
  }
  if (input.airport !== 'LFPG') throw new Error('Ce simulateur accepte uniquement les instantanés LFPG.');
  if (typeof input.simulated !== 'boolean') throw new Error('Le champ simulated doit être explicite.');
  if (typeof input.capturedAt !== 'string' || !/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d{1,3})?Z$/.test(input.capturedAt) || !Number.isFinite(Date.parse(input.capturedAt)) || new Date(input.capturedAt).toISOString().slice(0, 19) !== input.capturedAt.slice(0, 19)) {
    throw new Error('capturedAt doit être une date UTC ISO 8601 valide.');
  }
  const snapshot = input.snapshot;
  if (!isRecord(snapshot) || !Array.isArray(snapshot.callsigns) || snapshot.callsigns.length > 4096 || !isRecord(snapshot.providers)) {
    throw new Error('Instantané bridge invalide (callsigns/providers).');
  }
  const normalized = new Set<string>();
  for (const callsign of snapshot.callsigns) {
    if (typeof callsign !== 'string' || callsign !== normalizeCallsign(callsign) || normalized.has(callsign)) {
      throw new Error('Indicatif invalide, non normalisé ou dupliqué dans callsigns.');
    }
    normalized.add(callsign);
  }
  if (Object.keys(snapshot.providers).length > 32) throw new Error('Trop de fournisseurs dans cet instantané.');
  for (const provider of Object.values(snapshot.providers)) {
    if (!isRecord(provider) || !Number.isSafeInteger(provider.major) || (provider.major as number) < 0 || !Number.isSafeInteger(provider.minor) || (provider.minor as number) < 0 || !isRecord(provider.fields) || Object.keys(provider.fields).length > 64) {
      throw new Error('Déclaration de fournisseur bridge invalide.');
    }
    for (const field of Object.values(provider.fields)) {
      if (!isRecord(field) || Object.keys(field).length > 4096) throw new Error('Table de valeurs bridge invalide.');
    }
  }
  return input as unknown as BridgeImportEnvelope;
}
