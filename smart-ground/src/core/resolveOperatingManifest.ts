import type { OperatingManifest } from './OperatingRules';

export interface OperatingWorksProfile {
  id: string;
  title: string;
  validFrom: string;
  validUntil: string;
  requiresNotamConfirmation: boolean;
  defaultEnabled: boolean;
  edgeRules?: OperatingManifest['edgeRules'];
  closedLocationIds?: string[];
  notes?: string[] | string;
  sourceIds: string[];
}
export interface ProfiledOperatingManifest extends OperatingManifest {
  temporaryProfiles?: OperatingWorksProfile[];
}

/** Shared by the UI, tests and validator: published defaults are part of the scenario. */
export function resolveOperatingManifest(base: ProfiledOperatingManifest, options: {
  enabledProfileIds?: readonly string[];
  approvedPprLocationIds?: readonly string[];
} = {}): OperatingManifest {
  const profiles = base.temporaryProfiles || [];
  const enabled = new Set(options.enabledProfileIds ?? profiles.filter(p => p.defaultEnabled).map(p => p.id));
  for (const id of enabled) if (!profiles.some(p => p.id === id)) throw new Error(`Unknown operating works profile: ${id}`);
  const selected = profiles.filter(p => enabled.has(p.id));
  return {
    ...base,
    edgeRules: [...base.edgeRules, ...selected.flatMap(p => p.edgeRules || [])],
    closedLocationIds: [...new Set([...(base.closedLocationIds || []), ...selected.flatMap(p => p.closedLocationIds || [])])],
    approvedPprLocationIds: [...(options.approvedPprLocationIds || [])],
    activeProfileIds: [...enabled].sort(),
  };
}
