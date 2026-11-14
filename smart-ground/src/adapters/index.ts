import { validateEnvelope } from './BridgeSnapshot';
import type { FieldState } from './BridgeSnapshot';
import { RampAgentAdapter } from './RampAgentAdapter';
import { VSIDAdapter } from './VSIDAdapter';

export { RampAgentAdapter } from './RampAgentAdapter';
export { VSIDAdapter } from './VSIDAdapter';
export * from './EuroScopeSurfaceAdapter';
export { normalizeCallsign, normalizeStandName, normalizeRunwayName } from './BridgeSnapshot';
export type { BridgeImportEnvelope, BridgeProvider, BridgeSnapshot, AssignedField, FieldState } from './BridgeSnapshot';

export interface ImportedAssignment {
  callsign: string;
  stand: string | null;
  runway: string | null;
  remark: string | null;
  sid: string | null;
  cfl: string | null;
  standState: FieldState;
  runwayState: FieldState;
}

export interface ImportedAssignments {
  airport: 'LFPG';
  transport: 'file';
  capturedAt: string;
  simulated: boolean;
  stale: boolean;
  assignments: ImportedAssignment[];
  diagnostics: string[];
}

/** A local file never establishes a live/authenticated EuroScope connection. */
export function parseBridgeImport(input: unknown, nowMs = Date.now(), maxAgeMs = 15_000): ImportedAssignments {
  if (!Number.isFinite(nowMs) || !Number.isFinite(maxAgeMs) || maxAgeMs < 0) throw new Error('Paramètres de fraîcheur invalides.');
  const envelope = validateEnvelope(input);
  const capturedMs = Date.parse(envelope.capturedAt);
  const ageMs = nowMs - capturedMs;
  const stale = ageMs > maxAgeMs || ageMs < -5_000;
  const diagnostics: string[] = [];
  if (ageMs < -5_000) diagnostics.push('Horodatage futur : vérifier les horloges. Affectations inutilisables.');
  else if (stale) diagnostics.push('Instantané périmé : les affectations sont retirées, sans conserver les anciennes valeurs.');
  if (envelope.simulated) diagnostics.push('Données de démonstration déclarées ; aucune connexion EuroScope active.');
  else diagnostics.push('Fichier importé ; provenance déclarée, sans connexion EuroScope active.');
  const ramp = new RampAgentAdapter();
  const vsid = new VSIDAdapter();
  const assignments = envelope.snapshot.callsigns.map(callsign => {
    const rampData = ramp.read(envelope.snapshot, callsign, { stale });
    const vsidData = vsid.read(envelope.snapshot, callsign, { stale });
    return {
      callsign,
      stand: rampData.stand.value,
      runway: vsidData.runway.value,
      remark: rampData.remark.value,
      sid: vsidData.sid.value,
      cfl: vsidData.cfl.value,
      standState: rampData.stand.state,
      runwayState: vsidData.runway.state,
    };
  });
  for (const id of ['rampagent', 'vsid']) {
    const provider = envelope.snapshot.providers[id];
    if (!provider) diagnostics.push(`Fournisseur ${id} absent.`);
    else if (provider.major !== 1) diagnostics.push(`Schéma majeur ${provider.major} du fournisseur ${id} incompatible.`);
  }
  const invalidCount = assignments.filter(a => a.standState === 'invalid' || a.runwayState === 'invalid').length;
  if (invalidCount) diagnostics.push(`${invalidCount} indicatif(s) avec un stand ou une piste invalide.`);
  return { airport: 'LFPG', transport: 'file', capturedAt: envelope.capturedAt, simulated: envelope.simulated, stale, assignments, diagnostics };
}
