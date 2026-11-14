/**
 * Deterministic demand for a mixed simulation session. These intervals are
 * simulator pacing parameters, not airport capacity or separation standards.
 * Actual activation still needs an unoccupied source in AircraftSimulator.
 */
export interface TrafficScheduleOptions {
  /** Absolute simulation time, in seconds. */
  startTime?: number;
  /** Time from the first batch to the last nominal release. */
  windowSeconds?: number;
  /** Initial aircraft allowed to start immediately when their sources differ. */
  initialActive?: number;
  /** Earliest gap between releases from the same physical origin node. */
  sameOriginGapSeconds?: number;
}

export interface TrafficScheduleSlot {
  readonly index: number;
  readonly movement: 'arrival' | 'departure';
  /** Nominal absolute start, before repeated-origin spacing. */
  readonly targetAt: number;
}

function randomGenerator(seed: number): () => number {
  let state = seed >>> 0;
  return () => {
    state += 0x6D2B79F5;
    let value = state;
    value = Math.imul(value ^ value >>> 15, value | 1);
    value ^= value + Math.imul(value ^ value >>> 7, value | 61);
    return ((value ^ value >>> 14) >>> 0) / 4294967296;
  };
}

export class TrafficScheduler {
  readonly slots: readonly TrafficScheduleSlot[];
  readonly startTime: number;
  readonly windowSeconds: number;
  readonly initialActive: number;
  readonly sameOriginGapSeconds: number;
  private readonly originLastRelease = new Map<string, number>();
  private readonly allocations = new Map<number, { originKey: string; scheduledAt: number }>();

  constructor(readonly count: number, seed = 42, options: TrafficScheduleOptions = {}) {
    if (!Number.isInteger(count) || count < 0 || count > 100) throw new Error('Traffic count must be an integer from 0 to 100');
    if (!Number.isInteger(seed) || !Number.isFinite(seed)) throw new Error('Traffic seed must be a finite integer');
    this.startTime = options.startTime ?? 0;
    this.windowSeconds = options.windowSeconds ?? Math.max(180, count * 12);
    this.initialActive = options.initialActive ?? Math.min(2, count);
    this.sameOriginGapSeconds = options.sameOriginGapSeconds ?? 90;
    if (!Number.isFinite(this.startTime) || this.startTime < 0) throw new Error('Traffic start time must be finite and nonnegative');
    if (!Number.isFinite(this.windowSeconds) || this.windowSeconds < 0) throw new Error('Traffic window must be finite and nonnegative');
    if (!Number.isInteger(this.initialActive) || this.initialActive < 0 || this.initialActive > count) throw new Error('Initial active count must be an integer within the traffic count');
    if (!Number.isFinite(this.sameOriginGapSeconds) || this.sameOriginGapSeconds < 0) throw new Error('Repeated-origin gap must be finite and nonnegative');

    const random = randomGenerator(seed);
    const movements: ('arrival' | 'departure')[] = [];
    // Each successive pair contains both movements, including the first batch.
    // A failed route search can retry the same successful-aircraft index without
    // silently converting unavailable arrivals into extra departures.
    for (let index = 0; index < count; index += 2) {
      const first = random() < .5 ? 'arrival' : 'departure';
      movements.push(first);
      if (index + 1 < count) movements.push(first === 'arrival' ? 'departure' : 'arrival');
    }
    const weights = Array.from({ length: count - this.initialActive }, () => .8 + random() * .4);
    const weightTotal = weights.reduce((total, weight) => total + weight, 0);
    let elapsedWeight = 0;
    this.slots = Object.freeze(movements.map((movement, index) => {
      if (index >= this.initialActive) elapsedWeight += weights[index - this.initialActive];
      const targetAt = this.startTime + (index < this.initialActive ? 0 : elapsedWeight / weightTotal * this.windowSeconds);
      return Object.freeze({ index, movement, targetAt });
    }));
  }

  movementAt(index: number): TrafficScheduleSlot['movement'] {
    return this.slot(index).movement;
  }

  /**
   * Commit only after a compatible route has been found. Call in successful
   * aircraft order; use the origin node ID (not a stand name) to merge aliases.
   * Repeating the same allocation is idempotent and consumes no extra gap.
   */
  allocate(index: number, originKey: string): number {
    const slot = this.slot(index);
    if (typeof originKey !== 'string' || !originKey.trim()) throw new Error('A physical origin key is required');
    const previous = this.allocations.get(index);
    if (previous) {
      if (previous.originKey !== originKey) throw new Error('A traffic slot cannot be reassigned to another origin');
      return previous.scheduledAt;
    }
    if (index !== this.allocations.size) throw new Error('Traffic slots must be allocated in successful aircraft order');
    const originLast = this.originLastRelease.get(originKey);
    const scheduledAt = Math.max(slot.targetAt, originLast === undefined ? this.startTime : originLast + this.sameOriginGapSeconds);
    this.originLastRelease.set(originKey, scheduledAt);
    this.allocations.set(index, { originKey, scheduledAt });
    return scheduledAt;
  }

  private slot(index: number): TrafficScheduleSlot {
    if (!Number.isInteger(index) || index < 0 || index >= this.count) throw new Error('Traffic slot index is outside the schedule');
    return this.slots[index];
  }
}
