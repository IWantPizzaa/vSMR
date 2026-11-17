import type { AircraftProfile } from './types';

/** Published fixed-wing spans, in metres; custom profiles remain explicit. */
export const AIRCRAFT_PROFILES: readonly Readonly<AircraftProfile>[] = Object.freeze([
  Object.freeze({ typeCode: 'A320', wingspanMetres: 35.8 }),
  Object.freeze({ typeCode: 'A388', wingspanMetres: 79.75 }),
  Object.freeze({ typeCode: 'B744', wingspanMetres: 64.92 }),
  Object.freeze({ typeCode: 'B748', wingspanMetres: 68.4 }),
]);
export const DEFAULT_AIRCRAFT_PROFILE = AIRCRAFT_PROFILES[0];
/** Structural variants can legitimately be below the conservative presets. */
export const MINIMUM_KNOWN_WINGSPANS: Readonly<Record<string, number>> = Object.freeze({ A320: 34.1, A388: 79.75, B744: 64.44, B748: 68.4 });

/** An unidentified custom aircraft cannot evade type-specific restrictions. */
export function normalizeAircraftProfile(value: AircraftProfile): AircraftProfile {
  if (!value || typeof value.typeCode !== 'string' || !Number.isFinite(value.wingspanMetres) || value.wingspanMetres <= 0) throw new Error('Aircraft type and a positive finite wingspan are required');
  let typeCode = value.typeCode.trim().toUpperCase() || 'UNKNOWN';
  if (typeCode === 'B747') throw new Error('Choose the B747 variant: B744 or B748');
  if (typeCode === 'A380') typeCode = 'A388';
  if (!/^[A-Z0-9]{2,8}$/.test(typeCode)) throw new Error('Aircraft type must be a short ICAO designator or UNKNOWN');
  const minimum = MINIMUM_KNOWN_WINGSPANS[typeCode];
  if (minimum !== undefined && value.wingspanMetres < minimum - 1e-7) throw new Error(`Wingspan cannot be smaller than the published ${typeCode} minimum (${minimum} m)`);
  if (value.lengthMetres !== undefined && (!Number.isFinite(value.lengthMetres) || value.lengthMetres <= 0)) throw new Error('Aircraft length must be positive when supplied');
  return Object.freeze({ typeCode, wingspanMetres: value.wingspanMetres, ...(value.lengthMetres === undefined ? {} : { lengthMetres: value.lengthMetres }) });
}

export function sameAircraftProfile(first: AircraftProfile, second: AircraftProfile): boolean {
  return first.typeCode === second.typeCode && first.wingspanMetres === second.wingspanMetres && first.lengthMetres === second.lengthMetres;
}

export function isUnknownAircraftType(profile: AircraftProfile): boolean { return ['UNKNOWN', 'CUSTOM', 'ZZZZ'].includes(profile.typeCode); }
