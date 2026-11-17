import { NATIVE_AIRCRAFT_LENGTHS } from './AircraftDimensions';
import type { Aircraft, AircraftProfile } from './types';

export interface FootprintPoint { x: number; y: number }
export interface FootprintBox { x: number; y: number; halfLength: number; halfWidth: number }
export interface AircraftGeometry {
  lengthMetres: number; wingspanMetres: number;
  source: 'published-dimensions' | 'native-catalogue' | 'declared-dimensions' | 'estimated-length';
  model: 'body-wing-tail-envelope' | 'oriented-bounds';
  boxes: readonly FootprintBox[]; radiusMetres: number; warnings: string[];
}

/** Reviewed primary planning pages already stored in data-sources/aircraft-profiles.
 * A388 ACP p29; B744 Boeing RevF p31; B748 Boeing RevE p15; A320 EASA.A.064 p37.
 * These are overall lengths/maximum fuselage widths, not detailed 3D surfaces. */
const PRIMARY = Object.freeze({
  A320: { length: 37.57, body: 3.95 },
  A388: { length: 72.73, body: 7.14 },
  B744: { length: 70.67, body: 6.50 },
  B748: { length: 76.25, body: 6.50 },
});

export function resolveAircraftGeometry(profile?: AircraftProfile): AircraftGeometry {
  const code = profile?.typeCode?.toUpperCase() ?? 'UNKNOWN';
  const known = PRIMARY[code as keyof typeof PRIMARY], nativeLength = NATIVE_AIRCRAFT_LENGTHS[code];
  const span = profile?.wingspanMetres;
  const wingspanMetres = Number.isFinite(span) && span! > 0 ? span! : 80;
  const declared = profile?.lengthMetres;
  const validDeclared = Number.isFinite(declared) && declared! > 0;
  const knownLength = known?.length ?? nativeLength;
  const source: AircraftGeometry['source'] = known ? 'published-dimensions' : nativeLength && (!validDeclared || declared! < nativeLength) ? 'native-catalogue' : validDeclared ? 'declared-dimensions' : 'estimated-length';
  // Known airframes cannot shrink by declaring a shorter length. The fallback
  // for an unknown type is explicit and intentionally only a review envelope.
  const lengthMetres = validDeclared ? Math.max(knownLength ?? 0, declared!) : knownLength ?? Math.max(20, wingspanMetres * 1.5);
  const warnings: string[] = [];
  if (validDeclared && knownLength !== undefined && declared! < knownLength) warnings.push(`Longueur déclarée inférieure à la dimension connue : enveloppe conservée à ${knownLength} m.`);
  let boxes: FootprintBox[], model: AircraftGeometry['model'];
  if (known) {
    model = 'body-wing-tail-envelope';
    // Conservative planning envelopes around fuselage, main wings/engines and
    // horizontal tail. Broad chord bands deliberately contain empty areas;
    // they are not advertised as an exact manufacturer airframe silhouette.
    boxes = [
      { x: 0, y: 0, halfLength: lengthMetres / 2, halfWidth: known.body / 2 },
      { x: -.025 * lengthMetres, y: 0, halfLength: .295 * lengthMetres, halfWidth: wingspanMetres / 2 },
      { x: -.37 * lengthMetres, y: 0, halfLength: .13 * lengthMetres, halfWidth: .225 * wingspanMetres },
    ];
  } else {
    model = 'oriented-bounds';
    boxes = [{ x: 0, y: 0, halfLength: lengthMetres / 2, halfWidth: wingspanMetres / 2 }];
    warnings.push('Enveloppe rectangulaire orientée : silhouette détaillée du type non disponible.');
  }
  if (source === 'estimated-length') warnings.push('Longueur inconnue : estimation conservatrice à vérifier, pas une dimension publiée.');
  if (!profile || !Number.isFinite(span) || span! <= 0) warnings.push('Envergure inconnue : enveloppe de repli à vérifier.');
  const radiusMetres = Math.max(...boxes.map(box => Math.hypot(Math.abs(box.x) + box.halfLength, Math.abs(box.y) + box.halfWidth)));
  return { lengthMetres, wingspanMetres, source, model, boxes, radiusMetres, warnings };
}

export function footprintPolygons(aircraft: Pick<Aircraft, 'x' | 'y' | 'heading' | 'bodyHeading' | 'aircraftProfile'>, clearanceMetres = 0): FootprintPoint[][] {
  if (!Number.isFinite(clearanceMetres) || clearanceMetres < 0) throw new Error('Footprint clearance must be nonnegative');
  const angle = Number.isFinite(aircraft.bodyHeading) ? aircraft.bodyHeading! : aircraft.heading, c = Math.cos(angle), s = Math.sin(angle);
  return resolveAircraftGeometry(aircraft.aircraftProfile).boxes.map(box => {
    const length = box.halfLength + clearanceMetres, width = box.halfWidth + clearanceMetres;
    return [[-length, -width], [length, -width], [length, width], [-length, width]].map(([x, y]) => ({
      x: aircraft.x + (box.x + x) * c - (box.y + y) * s,
      y: aircraft.y + (box.x + x) * s + (box.y + y) * c,
    }));
  });
}
