import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseBridgeImport, RampAgentAdapter, VSIDAdapter } from '../src/adapters/index';
import type { BridgeImportEnvelope } from '../src/adapters/index';

const capturedAt = '2026-11-12T18:00:00.000Z';
const now = Date.parse(capturedAt);
function fixture(): BridgeImportEnvelope {
  return {
    format: 'smart-ground.bridge.v1', airport: 'LFPG', capturedAt, simulated: true,
    snapshot: {
      callsigns: ['AFR123'],
      providers: {
        rampagent: { major: 1, minor: 0, fields: { stand: { AFR123: ' f21 ' }, remark: { AFR123: 'Push W' } } },
        vsid: { major: 1, minor: 1, fields: { rwy: { AFR123: '26R' }, sid: { AFR123: 'ELCOB1B' }, cfl: { AFR123: '100' } } },
      },
    },
  };
}

test('consume actual proxy provider/field/callsign shape and preserve source ownership', () => {
  const result = parseBridgeImport(fixture(), now);
  assert.equal(result.transport, 'file');
  assert.equal(result.simulated, true);
  assert.deepEqual(result.assignments[0], {
    callsign: 'AFR123', stand: 'F21', runway: '26R', remark: 'Push W', sid: 'ELCOB1B', cfl: '100', standState: 'value', runwayState: 'value',
  });
});

test('cleared stand clears its remark; missing rwy never inferred from SID or global rules', () => {
  const input = fixture();
  input.snapshot.providers.rampagent.fields.stand = {};
  input.snapshot.providers.vsid.fields.rwy = {};
  input.snapshot.providers.vsid.fields.rules = { '': 'LFPG:OPPOSING=1;' };
  const result = parseBridgeImport(input, now).assignments[0];
  assert.equal(result.stand, null);
  assert.equal(result.remark, null);
  assert.equal(result.runway, null);
  assert.equal(result.sid, 'ELCOB1B');
  assert.equal(result.runwayState, 'unset');
});

test('unavailable provider, missing field, incompatible schema remain explicit unknowns', () => {
  const input = fixture();
  delete input.snapshot.providers.rampagent;
  input.snapshot.providers.vsid.major = 2;
  const result = parseBridgeImport(input, now).assignments[0];
  assert.equal(result.standState, 'unavailable');
  assert.equal(result.runwayState, 'incompatible');
  const fieldMissing = fixture();
  delete fieldMissing.snapshot.providers.vsid.fields.rwy;
  assert.equal(parseBridgeImport(fieldMissing, now).assignments[0].runwayState, 'unavailable');
});

test('stale and future snapshots withdraw assignments without retaining cached values', () => {
  for (const clock of [now + 15_001, now - 5_001]) {
    const result = parseBridgeImport(fixture(), clock);
    assert.equal(result.stale, true);
    assert.equal(result.assignments[0].stand, null);
    assert.equal(result.assignments[0].runway, null);
    assert.equal(result.assignments[0].standState, 'stale');
  }
  assert.equal(parseBridgeImport(fixture(), now + 15_000).stale, false);
});

test('field caps are bytes, control characters follow provider normalization, unknown LFPG runways reject', () => {
  const input = fixture();
  input.snapshot.providers.rampagent.fields.stand.AFR123 = 'é'.repeat(17);
  input.snapshot.providers.vsid.fields.rwy.AFR123 = '04';
  assert.equal(parseBridgeImport(input, now).assignments[0].standState, 'invalid');
  assert.equal(parseBridgeImport(input, now).assignments[0].runwayState, 'invalid');
  input.snapshot.providers.rampagent.fields.stand.AFR123 = 'A\t01';
  assert.equal(parseBridgeImport(input, now).assignments[0].stand, 'A 01');
  input.snapshot.providers.vsid.fields.rwy.AFR123 = '26\tR';
  assert.equal(parseBridgeImport(input, now).assignments[0].runwayState, 'invalid');
  input.snapshot.providers.rampagent.fields.stand.AFR123 = 'F21\0';
  assert.equal(parseBridgeImport(input, now).assignments[0].standState, 'invalid');
});

test('reject invalid envelope, airports, timestamps, field tables and duplicate/noncanonical callsigns', () => {
  for (const input of [null, {}, { ...fixture(), airport: 'LFPO' }, { ...fixture(), capturedAt: 'yesterday' }, { ...fixture(), simulated: undefined }]) {
    assert.throws(() => parseBridgeImport(input, now));
  }
  const input = fixture();
  input.snapshot.callsigns.push('AFR123');
  assert.throws(() => parseBridgeImport(input, now), /dupliqué/);
  input.snapshot.callsigns = ['afr123'];
  assert.throws(() => parseBridgeImport(input, now), /normalisé/);
});

test('numeric stand padding is preserved and providers have no assignment-policy fallback', () => {
  const input = fixture();
  input.snapshot.providers.rampagent.fields.stand.AFR123 = 'a01';
  assert.equal(new RampAgentAdapter().read(input.snapshot, 'afr123').stand.value, 'A01');
  input.snapshot.providers.vsid.fields = { runway: { AFR123: '27L' } };
  assert.equal(new VSIDAdapter().read(input.snapshot, 'AFR123').runway.state, 'unavailable');
});

test('vSID published no-runway sentinel is unset and bad calendar dates reject', () => {
  const input = fixture();
  input.snapshot.providers.vsid.fields.rwy.AFR123 = '---';
  assert.equal(parseBridgeImport(input, now).assignments[0].runwayState, 'unset');
  assert.throws(() => parseBridgeImport({ ...input, capturedAt: '2026-02-30T18:00:00.000Z' }, now));
  assert.equal(new RampAgentAdapter().read(input.snapshot, ' ').stand.state, 'invalid');
});
