/** Reproducible raw five-second reports, without using simulator edge IDs as observations. */
import { strict as assert } from 'node:assert';
import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync } from 'node:fs';
import { performance } from 'node:perf_hooks';
import { AircraftSimulator, RoutePlanner, OperatingRules, AircraftRestrictions, TaxiFlowPolicy, TaxiGraph } from '../src/core';
import { resolveOperatingManifest } from '../src/core/resolveOperatingManifest';
import { SurfaceTracker, type SurfaceTrackResult } from '../src/core/SurfaceTracker';
import type { Aircraft, AircraftProfile, MovementMode, OperatingConfiguration } from '../src/core/types';

const root = new URL('../', import.meta.url), started = performance.now(), seed = 20261113;
const read = (file: string) => readFileSync(new URL(file, root));
const digest = (bytes: Buffer) => createHash('sha256').update(bytes).digest('hex');
const snapshot = (file: string) => ({ file, bytes: read(file) });
const inputs = ['public/data/lfpg.json', 'public/data/lfpg-operations.json', 'public/data/lfpg-corridors.json', 'public/data/lfpg-aircraft-restrictions.json'].map(snapshot);
const code = ['src/core/SurfaceTracker.ts', 'src/core/TaxiGraph.ts', 'src/core/AircraftSimulator.ts', 'src/core/RoutePlanner.ts', 'src/core/OperatingRules.ts', 'src/core/AircraftRestrictions.ts', 'src/core/TaxiFlowPolicy.ts', 'scripts/validate-surveillance.ts'].map(snapshot);
const graph = new TaxiGraph(JSON.parse(inputs[0].bytes.toString('utf8')));
const operations = resolveOperatingManifest(JSON.parse(inputs[1].bytes.toString('utf8')));
const rules = new OperatingRules(graph, operations), flow = new TaxiFlowPolicy(graph, JSON.parse(inputs[2].bytes.toString('utf8'))), aircraftRules = new AircraftRestrictions(graph, JSON.parse(inputs[3].bytes.toString('utf8')));
assert.ok(rules.valid); assert.ok(flow.valid); assert.ok(aircraftRules.valid);
let randomState = seed;
const random = () => { randomState = Math.imul(randomState, 1664525) + 1013904223 | 0; return (randomState >>> 0) / 4294967296; };
const profiles: Record<string, AircraftProfile> = { A320: { typeCode: 'A320', wingspanMetres: 35.8 }, A388: { typeCode: 'A388', wingspanMetres: 79.75 }, B744: { typeCode: 'B744', wingspanMetres: 64.92 }, B748: { typeCode: 'B748', wingspanMetres: 68.4 } };
const cases: [string, OperatingConfiguration, string, string, MovementMode][] = [
  ['A320', 'east', 'stand:B08', 'operation:departure:09R:D3', 'departure'],
  ['A320', 'east', 'stand:M09', 'operation:departure:08L:T3', 'departure'],
  ['A320', 'east', 'stand:B03', 'operation:departure:09R:D5', 'departure'],
  ['A320', 'east', 'stand:A01', 'operation:departure:08L:T4', 'departure'],
  ['A320', 'west', 'stand:B12', 'operation:departure:26R:T9', 'departure'],
  ['A320', 'west', 'stand:F02', 'operation:departure:26R:T11', 'departure'],
  ['A320', 'west', 'operation:arrival:27R:K3', 'stand:I24', 'arrival'],
  ['A320', 'east', 'operation:arrival:09L:K6', 'stand:F02', 'arrival'],
  ['A388', 'east', 'stand:K09', 'operation:departure:08L:T4', 'departure'],
  ['A388', 'east', 'stand:W6/W7', 'operation:departure:09R:D4', 'departure'],
  ['A388', 'west', 'stand:Y08', 'operation:departure:26R:T11', 'departure'],
  ['A388', 'west', 'stand:W6/W7', 'operation:departure:27L:Q5', 'departure'],
  ['B744', 'east', 'stand:K09', 'operation:departure:08L:T4', 'departure'],
  ['B744', 'west', 'stand:Y08', 'operation:departure:26R:T11', 'departure'],
  ['B748', 'east', 'stand:K09', 'operation:departure:08L:T4', 'departure'],
  ['B748', 'west', 'stand:Y08', 'operation:departure:26R:T11', 'departure'],
];
function simulator(configuration: OperatingConfiguration, profile: AircraftProfile) {
  const planner = new RoutePlanner(graph, rules, configuration); planner.setFlowPolicy(flow); planner.setAircraftRestrictions(aircraftRules); planner.setAircraftProfile(profile);
  return new AircraftSimulator(graph, planner);
}
function sample(aircraft: Aircraft, time: number, noise = 3) {
  return { aircraftId: aircraft.id, observedAt: time, x: aircraft.x + (random() - .5) * noise * 2, y: aircraft.y + (random() - .5) * noise * 2,
    headingTrueDegrees: ((90 - aircraft.heading * 180 / Math.PI + (random() - .5) * 6) % 360 + 360) % 360,
    groundSpeedKnots: aircraft.state === 'taxiing' ? aircraft.speed / .514444 + (random() - .5) * .5 : 0 };
}
let reportCount = 0, estimatesChecked = 0, explicitCrossings = 0;
const scenarios: { type: string; configuration: OperatingConfiguration; from: string; to: string; movement: MovementMode; reports: number; ambiguous: number; suspected: number; falseConfirmed: number; seconds: number }[] = [];
for (const [type, configuration, from, to, movement] of cases) {
  const sim = simulator(configuration, profiles[type]), tracker = new SurfaceTracker(graph);
  const aircraft = sim.addAircraft(from, to, { callsign: 'AUDIT1', movement, aircraftProfile: profiles[type] });
  assert.ok(aircraft, `${type}/${configuration}/${from}: original route must be compatible.`);
  let reports = 0, ambiguous = 0, suspected = 0;
  for (let report = 0; report < 1000; report++) {
    const result = tracker.ingest(sample(aircraft, sim.time), aircraft.assignedRoute!, sim.time, { clearedClearanceIds: aircraft.clearedClearanceIds });
    assert.equal(result.accepted, true); assert.notEqual(result.status, 'confirmed-off-track', `${type}/${from} at ${sim.time}: false confirmed deviation ${result.deviationKind}.`);
    reports++; reportCount++;
    if (result.status === 'ambiguous') ambiguous++; if (result.status === 'suspected-off-track') suspected++;
    for (let frame = 1; frame < 20; frame++) {
      const estimate: SurfaceTrackResult = tracker.estimate(aircraft.id, sim.time + frame * .25)!;
      assert.equal(estimate.sourceObservedAt, sim.time); assert.equal(estimate.evidenceCount, result.evidenceCount);
      assert.equal(estimate.confirmedObservation, undefined, 'Render interpolation must not create a new source observation.'); estimatesChecked++;
    }
    if (aircraft.state === 'arrived') break;
    if (aircraft.waitingClearanceId) {
      // Explicit oracle action solely to complete the assigned routes. The
      // tracker itself has no clearance-granting function.
      assert.equal(sim.clearRunwayCrossing(aircraft.id), true); explicitCrossings++;
    }
    sim.update(5);
  }
  assert.equal(aircraft.state, 'arrived');
  scenarios.push({ type, configuration, from, to, movement, reports, ambiguous, suspected, falseConfirmed: 0, seconds: sim.time });
  console.log(`${type}/${configuration}/${from}: ${reports} noisy five-second reports, no false confirmed deviation.`);
}

// Real M09/T3 wrong-U branch at a slow2m/s: actual recovery geometry supplies
// raw position/heading/GS; its simulator edge ID never enters the tracker.
const faultSim = simulator('east', profiles.A320), fault = faultSim.addAircraft('stand:M09', 'operation:departure:08L:T3', { callsign: 'WRONG1', movement: 'departure', speed: 2 })!;
assert.ok(fault); assert.equal(faultSim.simulateWrongTurn(fault.id), true);
while (!fault.conformance?.events.length && faultSim.time < 3000) faultSim.update(.25);
assert.equal(fault.conformance?.status, 'recovering');
const faultTracker = new SurfaceTracker(graph), faultAt = faultSim.time;
faultTracker.ingest(sample(fault, faultSim.time, 0), fault.assignedRoute!, faultSim.time);
faultSim.setHolding(fault.id, false);
const faultReports: { time: number; status: string; evidence: number; edgeId?: string }[] = [];
let confirmedAt: number | undefined;
for (let i = 0; i < 12; i++) {
  faultSim.update(5);
  const result = faultTracker.ingest(sample(fault, faultSim.time, 0), fault.assignedRoute!, faultSim.time);
  faultReports.push({ time: faultSim.time, status: result.status, evidence: result.evidenceCount, edgeId: result.observation?.edgeId });
  if (result.status === 'confirmed-off-track') {
    assert.ok(result.confirmedObservation); assert.ok(result.evidenceCount >= 2); confirmedAt = faultSim.time;
    const count = result.evidenceCount;
    for (let frame = 0; frame < 20; frame++) assert.equal(faultTracker.estimate(fault.id, faultSim.time + frame * .1)!.evidenceCount, count);
    break;
  }
}
assert.ok(confirmedAt !== undefined, 'A sustained real wrong-U branch must be detected from independent reports.');
assert.equal(faultTracker.estimate(fault.id, confirmedAt + 25)!.freshness, 'lost');
for (const item of [...inputs, ...code]) assert.equal(digest(read(item.file)), digest(item.bytes), `${item.file} changed during validation.`);
const report = { generatedAt: new Date().toISOString(), seed, inputSha256: Object.fromEntries(inputs.map(i => [i.file, digest(i.bytes)])), codeSha256: Object.fromEntries(code.map(i => [i.file, digest(i.bytes)])), normalRoutes: scenarios.length, normalReports: reportCount, estimatesChecked, falseConfirmedDeviations: 0, sourceIntervalSeconds: 5, positionNoiseMetres: 3, headingNoiseDegrees: 3, speedNoiseKnots: .25, explicitOracleCrossings: explicitCrossings, scenarios, sustainedWrongBranch: { callsign: fault.callsign, faultAt, confirmedAt, secondsUntilConfirmed: confirmedAt - faultAt, reports: faultReports }, elapsedMs: +(performance.now() - started).toFixed(1) };
writeFileSync(new URL('docs/surveillance-validation-results.json', root), JSON.stringify(report, null, 2) + '\n');
writeFileSync(new URL('docs/SURVEILLANCE-VALIDATION.md', root), `# Validation des observations sol espacees

Commande : \`npm.cmd run validate:surveillance\`. Les ${scenarios.length} parcours LFPG couvrent les deux configurations, des arrivees et departs, A320, A388, B744 et B748. Le suivi recoit seulement position x/y, cap vrai, vitesse sol et horodatage independant toutes les cinq secondes ; les identifiants de segment du simulateur ne lui sont pas transmis.

Les **${reportCount} observations bruitees** (position ±3 m par axe, cap ±3°, vitesse ±0,25 kt) ne produisent **aucun faux ecart confirme**. ${estimatesChecked} estimations intermediaires ne creent ni nouvel horodatage source ni vote de confirmation. Les intersections et voies paralleles peuvent rester ambigues ; un ecart isole n'est pas transforme en certitude. Les ${explicitCrossings} traversees necessaires au parcours complet sont autorisees explicitement par l'oracle de test ; le suivi ne delivre aucune clairance.

Une mauvaise branche U reelle depuis M09 vers T3 est confirmee apres ${confirmedAt - faultAt} secondes, sur au moins deux rapports independants et non par repetition des images. La vitesse de2m/s de cet oracle rend la branche durablement observable. L'extrapolation est bornee a dix secondes, puis figee et signalee perimee ; apres vingt secondes le suivi est perdu. Une projection ne franchit pas un point d'attente non autorise, mais une mesure effectivement recue au-dela reste disponible comme preuve de deviation.

Un detour commis puis corrige entierement entre deux messages n'est pas observable avec ces seules donnees. La cadence de cinq secondes est ici un scenario de simulation, pas une promesse SDK. Quand EuroScope ne donne pas l'heure de mesure, l'adaptateur doit employer l'epoque de reception independante et conserver l'age connu ; chaque image de rendu n'est pas une nouvelle observation. Le bruit et les seuils sont des parametres a calibrer sur des enregistrements reels.

| Profil | Configuration | Origine → destination | Rapports | Ambigus | Suspects isoles | Faux confirmes |
| --- | --- | --- | ---: | ---: | ---: | ---: |
${scenarios.map(s => `| ${s.type} | ${s.configuration} | ${s.from} → ${s.to} | ${s.reports} | ${s.ambiguous} | ${s.suspected} | 0 |`).join('\n')}

Resultats detaillees : [surveillance-validation-results.json](surveillance-validation-results.json). Les tests unitaires couvrent aussi les doublons, paquets reordonnes, cap d'un avion arrete, repoussage, virages normaux entre deux rapports et saut de position incoherent.

## Empreintes

${[...inputs, ...code].map(item => `- \`${item.file}\` : \`${digest(item.bytes)}\``).join('\n')}
`, 'utf8');
console.log(`Surface surveillance validated: ${scenarios.length} routes, ${reportCount} noisy independent reports, ${estimatesChecked} render estimates, zero false confirmed deviations; actual wrong-U confirmed in ${confirmedAt - faultAt}s.`);
