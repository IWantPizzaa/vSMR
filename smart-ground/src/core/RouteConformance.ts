import type { Route } from './types';

/** Independent assignment snapshot; an observed excursion never rewrites it. */
export function snapshotAssignedRoute(route: Route): Route {
  const copy = JSON.parse(JSON.stringify(route)) as Route;
  const freeze = (value: unknown) => {
    if (!value || typeof value !== 'object' || Object.isFrozen(value)) return;
    Object.values(value).forEach(freeze); Object.freeze(value);
  };
  freeze(copy); return copy;
}

export function routeLegAt(route: Route, distance: number): number {
  let lo = 0, hi = route.edgeIds.length - 1;
  while (lo < hi) { const mid = (lo + hi) >> 1; if (route.cumulativeDistances[mid + 1] <= distance) lo = mid + 1; else hi = mid; }
  return Math.max(0, lo);
}
