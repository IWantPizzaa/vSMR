import { strict as assert } from 'node:assert';
import { test } from 'node:test';
import { TrafficScheduler } from '../src/core/TrafficScheduler';

test('mixed schedules are seeded, balanced and spread out while the initial batch contains both movements', () => {
  const schedule = new TrafficScheduler(100, 20261113);
  assert.deepEqual(schedule.slots, new TrafficScheduler(100, 20261113).slots);
  assert.notDeepEqual(schedule.slots, new TrafficScheduler(100, 20261114).slots);
  assert.equal(schedule.slots.filter(slot => slot.movement === 'arrival').length, 50);
  assert.equal(schedule.slots.filter(slot => slot.movement === 'departure').length, 50);
  assert.equal(new Set(schedule.slots.slice(0, 2).map(slot => slot.movement)).size, 2);
  assert.equal(schedule.slots.filter(slot => slot.targetAt === 0).length, 2);
  for (let index = 2; index < schedule.count; index++) assert.ok(schedule.slots[index].targetAt > schedule.slots[index - 1].targetAt);
  assert.ok(Math.abs(schedule.slots.at(-1)!.targetAt - 1200) < 1e-8, '100 movements are nominally released across twenty simulation minutes.');
});

test('odd and small traffic counts retain a mix without inventing extra traffic', () => {
  for (const count of [0, 1, 2, 3, 9, 25]) {
    const schedule = new TrafficScheduler(count, 4321);
    assert.equal(schedule.slots.length, count);
    const arrivals = schedule.slots.filter(slot => slot.movement === 'arrival').length;
    assert.ok(Math.abs(arrivals - (count - arrivals)) <= 1);
    for (let index = 0; index < count; index++) assert.equal(schedule.movementAt(index), schedule.slots[index].movement);
  }
});

test('reused physical origins are staggered even across stand aliases and allocations are idempotent', () => {
  const schedule = new TrafficScheduler(6, 77, { startTime: 300, windowSeconds: 60 });
  const first = schedule.allocate(0, 'osm:shared-stand-node');
  assert.equal(first, 300);
  assert.equal(schedule.allocate(0, 'osm:shared-stand-node'), first);
  assert.equal(schedule.allocate(1, 'osm:shared-stand-node'), 390, 'The second immediate slot is delayed when both stand aliases refer to the same node.');
  const other = schedule.allocate(2, 'osm:other-origin');
  assert.ok(other > 300 && other < 390, 'A blocked origin does not delay an unrelated stand.');
  assert.equal(schedule.allocate(3, 'osm:shared-stand-node'), 480);
  assert.ok(schedule.allocate(4, 'osm:other-origin') >= other + 90);
  assert.throws(() => schedule.allocate(0, 'osm:changed-origin'), /cannot be reassigned/);
});

test('custom pacing is explicit and malformed schedule inputs fail rather than starting all aircraft', () => {
  const schedule = new TrafficScheduler(4, 1, { startTime: 20, initialActive: 0, windowSeconds: 240, sameOriginGapSeconds: 120 });
  assert.ok(schedule.slots.every(slot => slot.targetAt > 20));
  assert.ok(Math.abs(schedule.slots.at(-1)!.targetAt - 260) < 1e-8);
  assert.throws(() => schedule.allocate(1, 'node'), /successful aircraft order/);
  assert.throws(() => schedule.allocate(0, ''), /physical origin/);
  for (const index of [-1, .5, 4, NaN]) assert.throws(() => schedule.movementAt(index), /outside the schedule/);
  for (const count of [-1, .5, 101, Infinity]) assert.throws(() => new TrafficScheduler(count));
  assert.throws(() => new TrafficScheduler(1, NaN));
  assert.throws(() => new TrafficScheduler(1, 42, { startTime: -1 }));
  assert.throws(() => new TrafficScheduler(1, 42, { initialActive: 2 }));
  assert.throws(() => new TrafficScheduler(1, 42, { windowSeconds: Infinity }));
  assert.throws(() => new TrafficScheduler(1, 42, { sameOriginGapSeconds: -1 }));
});
