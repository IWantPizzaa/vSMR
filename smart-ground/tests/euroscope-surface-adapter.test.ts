import { strict as assert } from 'node:assert';
import { test } from 'node:test';
import { EuroScopeSurfaceAdapter, type EuroScopeSurfaceReport } from '../src/adapters/EuroScopeSurfaceAdapter';

const report = (changes: Partial<EuroScopeSurfaceReport> = {}): EuroScopeSurfaceReport => ({
  aircraftId: 'AFR123', sessionId: 'connection-1', sequence: 1, receivedAt: 10,
  latitude: 49.0097, longitude: 2.5479, reportedGroundSpeedKnots: 15, headingTrueDegrees: 0, ...changes,
});
function fixture() {
  const adapter = new EuroScopeSurfaceAdapter(); adapter.beginSession('AFR123', 'connection-1'); return adapter;
}

test('EuroScope primitive reports use the exact pinned LFPG projection and preserve valid north heading', () => {
  const adapter = fixture(), atOrigin = adapter.adapt(report(), 12); assert.ok(atOrigin.accepted);
  assert.equal(atOrigin.sample.x, 0); assert.equal(atOrigin.sample.y, 0); assert.equal(atOrigin.sample.headingTrueDegrees, 0);
  assert.equal(atOrigin.sample.groundSpeedKnots, 15); assert.equal(atOrigin.speedSource, 'reported');
  const moved = adapter.adapt(report({ sequence: 2, receivedAt: 15, latitude: 49.0107, longitude: 2.5489 }), 15); assert.ok(moved.accepted);
  assert.ok(Math.abs(moved.sample.x - .001 * 111320 * Math.cos(49.0097 * Math.PI / 180)) < 1e-6);
  assert.ok(Math.abs(moved.sample.y - 111.32) < 1e-6);
});

test('render consumption never refreshes an old source receipt or adds duplicate evidence', () => {
  const adapter = fixture(), first = adapter.adapt(report(), 15); assert.ok(first.accepted);
  assert.equal(first.sample.observedAt, 10); assert.equal(first.sample.receivedAt, 10);
  assert.deepEqual(adapter.adapt(report(), 20), { accepted: false, reason: 'duplicate-or-out-of-order-sequence' });
  assert.deepEqual(adapter.adapt(report({ sequence: 0, receivedAt: 18 }), 20), { accepted: false, reason: 'duplicate-or-out-of-order-sequence' });
  assert.deepEqual(adapter.adapt(report({ sequence: 2, receivedAt: 10 }), 20), { accepted: false, reason: 'nonadvancing-receipt' });
  const next = adapter.adapt(report({ sequence: 2, receivedAt: 15 }), 20); assert.ok(next.accepted); assert.equal(next.sample.observedAt, 15);
});

test('session replacement is explicit and rejects delayed reports from an old connection', () => {
  const adapter = fixture(); assert.ok(adapter.adapt(report(), 10).accepted);
  assert.equal(adapter.beginSession('AFR123', 'connection-1'), false);
  assert.equal(adapter.adapt(report(), 10).accepted, false, 'Repeated beginSession must not reset a report sequence.');
  assert.deepEqual(adapter.adapt(report({ sessionId: 'connection-2', sequence: 0, receivedAt: 20 }), 20), { accepted: false, reason: 'session-mismatch' });
  assert.equal(adapter.beginSession('AFR123', 'connection-2'), true);
  assert.ok(adapter.adapt(report({ sessionId: 'connection-2', sequence: 0, receivedAt: 20 }), 20).accepted);
  assert.deepEqual(adapter.adapt(report({ sequence: 50, receivedAt: 25 }), 25), { accepted: false, reason: 'session-mismatch' });
  assert.equal(adapter.endSession('AFR123', 'connection-1'), false);
  assert.equal(adapter.endSession('AFR123', 'connection-2'), true);
  assert.deepEqual(adapter.adapt(report({ sessionId: 'connection-2', receivedAt: 30 }), 30), { accepted: false, reason: 'inactive-session' });
});

test('GetGS is a conditional fallback rather than an independent speed measurement', () => {
  const adapter = fixture();
  const reported = adapter.adapt(report({ effectiveGroundSpeedKnots: 25 }), 10); assert.ok(reported.accepted);
  assert.equal(reported.sample.groundSpeedKnots, 15); assert.equal(reported.speedSource, 'reported');
  const fallback = adapter.adapt(report({ sequence: 2, receivedAt: 15, reportedGroundSpeedKnots: 0, effectiveGroundSpeedKnots: 3 }), 15); assert.ok(fallback.accepted);
  assert.equal(fallback.sample.groundSpeedKnots, 3); assert.equal(fallback.speedSource, 'effective-fallback');
  const missing = adapter.adapt(report({ sequence: 3, receivedAt: 20, reportedGroundSpeedKnots: undefined, effectiveGroundSpeedKnots: 3 }), 20); assert.ok(missing.accepted);
  assert.equal(missing.sample.groundSpeedKnots, undefined); assert.equal(missing.speedSource, 'unavailable');
  const stopped = adapter.adapt(report({ sequence: 4, receivedAt: 25, reportedGroundSpeedKnots: 0 }), 25); assert.ok(stopped.accepted);
  assert.equal(stopped.sample.groundSpeedKnots, 0);
});

test('invalid fields are rejected without consuming the good callback sequence', () => {
  const cases: [Partial<EuroScopeSurfaceReport>, string][] = [
    [{ latitude: 91 }, 'invalid-position'], [{ longitude: -181 }, 'invalid-position'], [{ latitude: NaN }, 'invalid-position'],
    [{ receivedAt: -1 }, 'invalid-time'], [{ receivedAt: Infinity }, 'invalid-time'], [{ receivedAt: 11 }, 'future-receipt'],
    [{ reportedGroundSpeedKnots: -1 }, 'invalid-speed'], [{ effectiveGroundSpeedKnots: NaN }, 'invalid-speed'],
    [{ headingTrueDegrees: 360 }, 'invalid-heading'], [{ headingTrueDegrees: -1 }, 'invalid-heading'],
    [{ sequence: .5 }, 'invalid-sequence'], [{ sequence: Number.MAX_SAFE_INTEGER + 1 }, 'invalid-sequence'],
  ];
  for (const [changes, reason] of cases) {
    const adapter = fixture(); assert.deepEqual(adapter.adapt(report(changes), 10), { accepted: false, reason });
    assert.ok(adapter.adapt(report(), 10).accepted, reason);
  }
  assert.deepEqual(fixture().adapt(report(), NaN), { accepted: false, reason: 'invalid-time' });
});

test('projection configuration and independent aircraft/session histories remain separate', () => {
  const adapter = new EuroScopeSurfaceAdapter({ latitude: 0, longitude: 0, metresPerDegree: 100000 });
  adapter.beginSession('AFR123', 'connection-1'); adapter.beginSession('EZY456', 'connection-1');
  const first = adapter.adapt(report({ latitude: .001, longitude: -.002 }), 10); assert.ok(first.accepted);
  assert.equal(first.sample.x, -200); assert.equal(first.sample.y, 100);
  assert.ok(adapter.adapt(report({ aircraftId: 'EZY456' }), 10).accepted);
  adapter.clear(); assert.deepEqual(adapter.adapt(report(), 10), { accepted: false, reason: 'inactive-session' });
  assert.throws(() => new EuroScopeSurfaceAdapter({ latitude: 90, longitude: 0 }), /Invalid surface projection/);
  assert.throws(() => adapter.beginSession('', 'connection-1'), /Invalid surface tracking identity/);
});
