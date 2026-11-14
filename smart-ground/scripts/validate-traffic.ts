/** Validate the mixed/staggered generator actually used by the interactive UI. */
import { strict as assert } from 'node:assert';
import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync } from 'node:fs';
import { performance } from 'node:perf_hooks';
import { AIRCRAFT_PROFILES, AircraftRestrictions, AircraftSimulator, ConflictDetector, OperatingRules, RoutePlanner, TaxiFlowPolicy, TaxiGraph, TrafficPredictor } from '../src/core';
import { resolveOperatingManifest } from '../src/core/resolveOperatingManifest';
import type { Aircraft, AirportData } from '../src/core';
import type { AircraftRestrictionManifest } from '../src/core/AircraftRestrictions';
import type { OperatingManifest } from '../src/core/OperatingRules';
import type { TaxiFlowManifest } from '../src/core/TaxiFlowPolicy';

const root = new URL('../', import.meta.url), started = performance.now(), seed = 20261113;
const read = (file: string) => readFileSync(new URL(file, root));
const digest = (bytes: Buffer) => createHash('sha256').update(bytes).digest('hex');
const snapshot = (file: string) => ({ file, bytes: read(file) });
const inputs = ['public/data/lfpg.json', 'public/data/lfpg-operations.json', 'public/data/lfpg-corridors.json', 'public/data/lfpg-aircraft-restrictions.json'].map(snapshot);
const code = ['src/core/AircraftSimulator.ts', 'src/core/RoutePlanner.ts', 'src/core/RouteConformance.ts', 'src/core/TrafficScheduler.ts', 'src/core/TrafficPredictor.ts', 'src/core/ConflictDetector.ts', 'src/core/PriorityController.ts', 'src/core/types.ts', 'scripts/validate-traffic.ts'].map(snapshot);
const data = JSON.parse(inputs[0].bytes.toString('utf8')) as AirportData;
const operations = resolveOperatingManifest(JSON.parse(inputs[1].bytes.toString('utf8')) as OperatingManifest);
const corridors = JSON.parse(inputs[2].bytes.toString('utf8')) as TaxiFlowManifest;
const aircraftManifest = JSON.parse(inputs[3].bytes.toString('utf8')) as AircraftRestrictionManifest;
const graph = new TaxiGraph(data), rules = new OperatingRules(graph, operations), flow = new TaxiFlowPolicy(graph, corridors), aircraftRules = new AircraftRestrictions(graph, aircraftManifest);
assert.ok(rules.valid, JSON.stringify(rules.diagnostics)); assert.ok(flow.valid, flow.diagnostics.join('; ')); assert.ok(aircraftRules.valid, aircraftRules.diagnostics.join('; '));
assert.equal(aircraftManifest.dataSha256, digest(inputs[0].bytes));
assert.deepEqual(operations.activeProfileIds, [], 'Works restrictions must remain absent.');
const requested = 100, frameCount = 120, deltaSeconds = .25, predictionHorizon = 120;
let checkedRoutes = 0, checkedPositions = 0, activePositions = 0, predictionChecks = 0;
const scenarios: { type: string; wingspanMetres: number; configuration: string; generated: number; arrivals: number; departures: number; initiallyActive: number; initiallyScheduled: number; activeAfter30Seconds: number; uniqueOrigins: number; distinctStartTimes: number; lastScheduledAt: number; generationMs: number }[] = [];

function verifyPosition(aircraft: Aircraft): void {
  const from = graph.nodes.get(aircraft.fromNodeId)!, to = graph.nodes.get(aircraft.toNodeId)!;
  assert.ok(Math.abs(aircraft.x - from.x - (to.x - from.x) * aircraft.edgeProgress) < 1e-6);
  assert.ok(Math.abs(aircraft.y - from.y - (to.y - from.y) * aircraft.edgeProgress) < 1e-6);
  assert.equal(aircraft.conformance?.status, 'on-route'); assert.deepEqual(aircraft.conformance?.events, []);
  assert.ok(!aircraft.pendingWrongTurn, 'No random deviation may be armed by the generator.');
  assert.equal(aircraft.clearedClearanceIds?.length || 0, 0, 'The validator never grants crossing clearance.');
  if (aircraft.waitingClearanceId && aircraft.state !== 'scheduled') assert.equal(aircraft.state, 'holding');
  checkedPositions++; if (aircraft.state !== 'scheduled') activePositions++;
}

for (const profile of AIRCRAFT_PROFILES) for (const configuration of ['east', 'west'] as const) {
  const planner = new RoutePlanner(graph, rules, configuration); planner.setFlowPolicy(flow); planner.setAircraftRestrictions(aircraftRules); planner.setAircraftProfile(profile);
  const sim = new AircraftSimulator(graph, planner), predictor = new TrafficPredictor(graph), detector = new ConflictDetector(graph), generationStarted = performance.now();
  const generated = sim.generateMixed(requested, seed, { aircraftProfile: profile }), generationMs = performance.now() - generationStarted;
  assert.equal(generated, requested, `${profile.typeCode}/${configuration}: mixed traffic requires 100 compatible routes.`);
  const arrivals = sim.aircraft.filter(a => a.route.operational?.movement === 'arrival').length, departures = sim.aircraft.filter(a => a.route.operational?.movement === 'departure').length;
  assert.equal(arrivals, 50); assert.equal(departures, 50); assert.equal(new Set(sim.aircraft.map(a => a.callsign)).size, generated);
  const initiallyActive = sim.aircraft.filter(a => a.state !== 'scheduled').length;
  assert.ok(initiallyActive <= 2); assert.ok(initiallyActive > 0);
  const distinctStartTimes = new Set(sim.aircraft.map(a => a.scheduledAt)).size; assert.ok(distinctStartTimes > 2);
  const origins = new Map<string, number>(), assignments = new Map(sim.aircraft.map(a => [a.id, a.assignedRoute]));
  for (const aircraft of sim.aircraft) {
    assert.ok(aircraft.scheduledAt !== undefined && Number.isFinite(aircraft.scheduledAt) && aircraft.scheduledAt >= 0);
    const origin = aircraft.route.nodeIds[0], previousAt = origins.get(origin);
    if (previousAt !== undefined) assert.ok(aircraft.scheduledAt - previousAt >= 90 - 1e-8, 'Repeated physical origins must retain their pacing gap.');
    origins.set(origin, aircraft.scheduledAt);
    assert.deepEqual(aircraft.aircraftProfile, profile); assert.deepEqual(aircraft.route.aircraft?.profile, profile);
    assert.ok(planner.isRouteAllowed(aircraft.route, aircraft.route.operational?.movement, { originId: aircraft.originId, destinationId: aircraft.destinationId, aircraftProfile: profile }), `${aircraft.callsign}: full route replay failed.`);
    assert.equal(aircraft.route.flow?.status, 'nominal'); assert.equal(aircraft.route.aircraft?.status, 'compatible');
    assert.ok(aircraft.route.edgeIds.every(id => !flow.specialAreaEdgeIds.has(id)), 'A mixed route cannot transit an excluded deicing bay.');
    assert.deepEqual(aircraft.assignedRoute?.edgeIds, aircraft.route.edgeIds); assert.ok(Object.isFrozen(aircraft.assignedRoute?.edgeIds));
    checkedRoutes++;
  }
  for (let frame = 0; frame < frameCount; frame++) {
    sim.update(deltaSeconds);
    const predictions = predictor.predict(sim.aircraft, predictionHorizon), pending = new Set(sim.aircraft.filter(a => a.state === 'scheduled').map(a => a.id));
    assert.equal(predictions.length, generated - pending.size); assert.ok(predictions.every(p => !pending.has(p.aircraft.id)));
    const conflicts = detector.detect(sim.aircraft, predictionHorizon); assert.ok(conflicts.every(c => c.aircraftIds.every(id => !pending.has(id))));
    predictionChecks++;
    for (const aircraft of sim.aircraft) { assert.equal(aircraft.assignedRoute, assignments.get(aircraft.id)); verifyPosition(aircraft); }
  }
  const activeAfter30Seconds = sim.aircraft.filter(a => a.state !== 'scheduled').length;
  scenarios.push({ type: profile.typeCode, wingspanMetres: profile.wingspanMetres, configuration, generated, arrivals, departures, initiallyActive, initiallyScheduled: generated - initiallyActive, activeAfter30Seconds, uniqueOrigins: origins.size, distinctStartTimes, lastScheduledAt: Math.max(...sim.aircraft.map(a => a.scheduledAt!)), generationMs: +generationMs.toFixed(1) });
  console.log(`${profile.typeCode}/${configuration}: ${generated} compatible mixed routes (${arrivals} arrivals/${departures} departures), ${initiallyActive} initially active, ${generated - initiallyActive} scheduled.`);
}

for (const item of [...inputs, ...code]) assert.equal(digest(read(item.file)), digest(item.bytes), `${item.file} changed during validation.`);
const report = { generatedAt: new Date().toISOString(), seed, inputSha256: Object.fromEntries(inputs.map(i => [i.file, digest(i.bytes)])), codeSha256: Object.fromEntries(code.map(i => [i.file, digest(i.bytes)])), checkedRoutes, checkedPositions, activePositionsChecked: activePositions, predictionChecks, unexpectedDeviations: 0, framesPerScenario: frameCount, deltaSeconds, simulatedSecondsPerScenario: frameCount * deltaSeconds, runwayClearancesGranted: 0, scenarios, elapsedMs: +(performance.now() - started).toFixed(1) };
writeFileSync(new URL('docs/traffic-validation-results.json', root), JSON.stringify(report, null, 2) + '\n');
writeFileSync(new URL('docs/TRAFFIC-VALIDATION.md', root), `# Validation du trafic mixte et des mises en mouvement

Commande : \`npm.cmd run validate:traffic\`. Les huit scénarios utilisent le générateur mixte de l’interface, les quatre profils A320/A388/B744/B748 et les deux configurations. Chacun produit 100 itinéraires compatibles : 50 arrivées et 50 départs, avec des indicatifs uniques et au plus deux avions actifs initialement.

Les ${checkedRoutes} itinéraires passent le rejeu complet des règles, des cheminements et des restrictions aéronefs. Les origines physiques réutilisées sont espacées d’au moins 90 secondes dans la programmation. Les avions futurs restent absents des prédictions et des conflits actifs jusqu’à leur activation. Les ${checkedPositions.toLocaleString('fr-FR')} positions contrôlées suivent exactement leur segment, sans erreur de cheminement injectée ni fausse alerte hors itinéraire. Aucune clairance de traversée n’est accordée.

Le contrôle de progression couvre les ${frameCount * deltaSeconds} premières secondes simulées de chaque scénario (${frameCount} pas de ${deltaSeconds} s), et vérifie ${predictionChecks} ensembles de prédictions. Il ne prétend pas dérouler les vingt minutes du programme ni résoudre tous les conflits. Les tests d’intégration séparés couvrent les grands pas de temps, les origines occupées, le maintien de l’écart réel de 90 secondes après un retard et les autorisations de piste refusées avant activation. Le rythme de génération est un paramètre de simulation, pas une séparation ATC.

| Profil | Configuration | Arrivées / départs | Actifs au début | Programmés | Actifs après 30 s | Origines physiques | Horaires distincts |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
${scenarios.map(s => `| ${s.type} · ${s.wingspanMetres} m | ${s.configuration === 'east' ? 'Face est' : 'Face ouest'} | ${s.arrivals} / ${s.departures} | ${s.initiallyActive} | ${s.initiallyScheduled} | ${s.activeAfter30Seconds} | ${s.uniqueOrigins} | ${s.distinctStartTimes} |`).join('\n')}

Graine : ${seed}. Données et code sont figés durant la validation ; leurs empreintes sont conservées dans [traffic-validation-results.json](traffic-validation-results.json). Les quatre catalogues sont inchangés. Les travaux restent exclus.

${inputs.map(i => `- \`${i.file}\` : \`${digest(i.bytes)}\``).join('\n')}
`, 'utf8');
console.log(`Mixed traffic validated: ${checkedRoutes} legal routes, ${checkedPositions} exact positions, ${predictionChecks} prediction checks, no unexpected deviations.`);
