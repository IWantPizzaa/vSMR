import { normalizeCallsign, normalizeStandName, readAircraftField } from './BridgeSnapshot';
import type { AssignedField, BridgeSnapshot, ReadContext } from './BridgeSnapshot';

export interface RampAgentAssignment {
  stand: AssignedField;
  remark: AssignedField;
}

/** Read assignments only: no airline/size/priority/stand-block allocation policy. */
export class RampAgentAdapter {
  read(snapshot: BridgeSnapshot, callsign: string, context: ReadContext = { stale: false }): RampAgentAssignment {
    const normalized = normalizeCallsign(callsign);
    if (!normalized) return { stand: { value: null, state: 'invalid' }, remark: { value: null, state: 'invalid' } };
    const stand = readAircraftField(snapshot, 'rampagent', 'stand', normalized, 32, context, true);
    if (stand.value) stand.value = normalizeStandName(stand.value);
    const remark = stand.state === 'value'
      ? readAircraftField(snapshot, 'rampagent', 'remark', normalized, 128, context, true)
      : { value: null, state: stand.state } satisfies AssignedField;
    return { stand, remark };
  }
}
