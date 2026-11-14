import { normalizeCallsign, normalizeRunwayName, readAircraftField } from './BridgeSnapshot';
import type { AssignedField, BridgeSnapshot, ReadContext } from './BridgeSnapshot';

export interface VSIDAssignment {
  runway: AssignedField;
  sid: AssignedField;
  cfl: AssignedField;
}

const lfpgRunways = new Set(['08L', '08R', '09L', '09R', '26L', '26R', '27L', '27R']);

/** The published rwy is authoritative; SID/configuration rules never infer it. */
export class VSIDAdapter {
  read(snapshot: BridgeSnapshot, callsign: string, context: ReadContext = { stale: false }): VSIDAssignment {
    const normalized = normalizeCallsign(callsign);
    if (!normalized) return { runway: { value: null, state: 'invalid' }, sid: { value: null, state: 'invalid' }, cfl: { value: null, state: 'invalid' } };
    const runway = readAircraftField(snapshot, 'vsid', 'rwy', normalized, 32, context);
    if (runway.value) {
      runway.value = normalizeRunwayName(runway.value);
      if (runway.value === '---') { runway.value = null; runway.state = 'unset'; }
      else if (!lfpgRunways.has(runway.value)) { runway.value = null; runway.state = 'invalid'; }
    }
    return {
      runway,
      sid: readAircraftField(snapshot, 'vsid', 'sid', normalized, 32, context),
      cfl: readAircraftField(snapshot, 'vsid', 'cfl', normalized, 32, context),
    };
  }
}
