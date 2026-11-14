/** Offline audit of the profile-aware production planner and simulator. */
import { strict as assert } from 'node:assert';
import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync } from 'node:fs';
import { performance } from 'node:perf_hooks';
import { AIRCRAFT_PROFILES, AircraftRestrictions, AircraftSimulator, OperatingRules, RoutePlanner, TaxiFlowPolicy, TaxiGraph, TrafficPredictor } from '../src/core';
import { resolveOperatingManifest } from '../src/core/resolveOperatingManifest';
import type { AircraftProfile, AirportData, MovementMode, OperatingConfiguration, Route } from '../src/core';
import type { AircraftRestrictionManifest } from '../src/core/AircraftRestrictions';
import type { OperatingManifest } from '../src/core/OperatingRules';
import type { TaxiFlowManifest } from '../src/core/TaxiFlowPolicy';

const root = new URL('../', import.meta.url), started = performance.now();
const probe = process.argv.includes('--probe'), requested = probe ? 10 : 100, steps = probe ? 1 : 240;
const read = (file: string) => readFileSync(new URL(file, root));
const digest = (bytes: Buffer) => createHash('sha256').update(bytes).digest('hex');
const snapshots = ['public/data/lfpg.json', 'public/data/lfpg-operations.json', 'public/data/lfpg-corridors.json', 'public/data/lfpg-aircraft-restrictions.json'].map(file => ({ file, bytes: read(file) }));
const data = JSON.parse(snapshots[0].bytes.toString('utf8')) as AirportData;
const baseOperations = JSON.parse(snapshots[1].bytes.toString('utf8')) as OperatingManifest;
const operations = resolveOperatingManifest(baseOperations);
const activeProfileIds = (operations.activeProfileIds || []) as string[];
assert.ok(Array.isArray(activeProfileIds) && activeProfileIds.every(id => typeof id === 'string'), 'Active works profiles must be valid aircraft scenario flags.');
const corridors = JSON.parse(snapshots[2].bytes.toString('utf8')) as TaxiFlowManifest;
const manifest = JSON.parse(snapshots[3].bytes.toString('utf8')) as AircraftRestrictionManifest;
assert.equal(manifest.dataSha256, digest(snapshots[0].bytes));
const graph = new TaxiGraph(data), rules = new OperatingRules(graph, operations), flow = new TaxiFlowPolicy(graph, corridors), aircraftRules = new AircraftRestrictions(graph, manifest);
assert.ok(rules.valid, JSON.stringify(rules.diagnostics)); assert.ok(flow.valid, flow.diagnostics.join('; ')); assert.ok(aircraftRules.valid, aircraftRules.diagnostics.join('; '));
assert.ok(manifest.edgeRules.length && manifest.turnRules.length, 'Both edge and turn restrictions must actually be shipped.');
const runwayAxes = new Map<string, { runway: string; dx: number; dy: number }[]>();
for (const edge of graph.edges.values()) {
  if (edge.kind !== 'runway' || !/^(08|09)[LR]\/(26|27)[LR]$/.test(edge.name)) continue;
  const from = graph.nodes.get(edge.from)!, to = graph.nodes.get(edge.to)!;
  for (const id of [edge.from, edge.to]) { const list = runwayAxes.get(id) || []; list.push({ runway: edge.name, dx: to.x - from.x, dy: to.y - from.y }); runwayAxes.set(id, list); }
}
let checkedRoutes = 0, checkedPositions = 0, crossings = 0;
function verifyRoute(route: Route, planner: RoutePlanner, profile: AircraftProfile) {
  assert.ok(route.aircraft, 'A profile-aware route must retain the applied profile.');
  assert.deepEqual(route.aircraft.profile, profile);
  assert.ok(planner.isRouteAllowed(route, undefined, { aircraftProfile: profile }));
  assert.ok(Math.abs(route.distance - route.edgeIds.reduce((sum, id) => sum + graph.edges.get(id)!.length, 0)) < 1e-5);
  assert.ok(route.edgeIds.every(id => !flow.specialAreaEdgeIds.has(id)));
  let previousCrossingDistance = -Infinity;
  for (let i = 0; i < route.edgeIds.length; i++) {
    const edge = graph.edges.get(route.edgeIds[i])!;
    const movement: MovementMode = route.aircraft.movement;
    assert.ok(aircraftRules.allowArc(edge, route.nodeIds[i], route.nodeIds[i + 1], planner.configuration, movement, profile, planner.aircraftRestrictionContext));
    assert.ok(aircraftRules.allowTurn(i ? route.edgeIds[i - 1] : null, route.nodeIds[i], edge.id, planner.configuration, movement, profile, planner.aircraftRestrictionContext));
  }
  for (let i = 1; i < route.nodeIds.length - 1; i++) {
    const node = graph.nodes.get(route.nodeIds[i])!, before = graph.nodes.get(route.nodeIds[i - 1])!, after = graph.nodes.get(route.nodeIds[i + 1])!, seen = new Set<string>();
    for (const axis of runwayAxes.get(node.id) || []) {
      if (seen.has(axis.runway)) continue; seen.add(axis.runway);
      const length = Math.hypot(axis.dx, axis.dy), side = (point: typeof node) => ((point.x - node.x) * axis.dy - (point.y - node.y) * axis.dx) / length;
      if (side(before) * side(after) >= -1e-6) continue;
      const distance = route.cumulativeDistances[i];
      assert.ok(route.operational?.clearanceCheckpoints.some(checkpoint => {
        if (checkpoint.distance <= previousCrossingDistance || checkpoint.distance >= distance - 1e-6) return false;
        const rule = operations.clearanceRules?.find(r => r.id === checkpoint.ruleId), location = data.locations.find(l => l.id === rule?.sourceHoldingLocationId);
        return rule?.runway === axis.runway && location?.kind === 'holding_point' && location.nodeId === checkpoint.nodeId;
      }), `${profile.typeCode}: missing source hold before physical ${axis.runway} crossing.`);
      previousCrossingDistance = distance; crossings++;
    }
  }
  checkedRoutes++;
}

const scenarios: { type: string; wingspanMetres: number; configuration: string; movement: string; requested: number; generated: number; generationMs: number; uniqueOrigins: number; advisories: number }[] = [];
for (const profile of AIRCRAFT_PROFILES) for (const configuration of ['east', 'west'] as const) for (const movement of ['departure', 'arrival', 'transfer'] as const) {
  const planner = new RoutePlanner(graph, rules, configuration); planner.setFlowPolicy(flow); planner.setAircraftRestrictions(aircraftRules); planner.setAircraftProfile(profile);
  planner.setAircraftRestrictionContext({ airportFlags: activeProfileIds });
  const sim = new AircraftSimulator(graph, planner), generationStarted = performance.now();
  const generated = sim.generate(requested, 20261113, { movement, aircraftProfile: profile });
  const generationMs = performance.now() - generationStarted;
  assert.equal(generated, requested, `${profile.typeCode}/${configuration}/${movement}: insufficient compatible traffic.`);
  for (const aircraft of sim.aircraft) {
    verifyRoute(aircraft.route, planner, profile); assert.equal(aircraft.route.flow?.status, 'nominal'); assert.equal(aircraft.route.aircraft?.status, 'compatible');
  }
  // A profile change for a new request must not silently resize existing traffic.
  planner.setAircraftProfile(profile.typeCode === 'A320' ? AIRCRAFT_PROFILES[1] : AIRCRAFT_PROFILES[0]);
  for (let step = 0; step < steps; step++) {
    sim.update(.25);
    for (const aircraft of sim.aircraft) {
      assert.deepEqual(aircraft.aircraftProfile, profile);
      const from = graph.nodes.get(aircraft.fromNodeId)!, to = graph.nodes.get(aircraft.toNodeId)!;
      assert.ok(Math.abs(aircraft.x - from.x - (to.x - from.x) * aircraft.edgeProgress) < 1e-6);
      assert.ok(Math.abs(aircraft.y - from.y - (to.y - from.y) * aircraft.edgeProgress) < 1e-6);
      if (aircraft.waitingClearanceId) assert.equal(aircraft.state, 'holding'); checkedPositions++;
    }
  }
  for (const aircraft of sim.aircraft.filter(a => a.waitingClearanceId)) assert.ok(new TrafficPredictor(graph).predict([aircraft], 300)[0].edges.every(e => e.stationary));
  scenarios.push({ type: profile.typeCode, wingspanMetres: profile.wingspanMetres, configuration, movement, requested, generated, generationMs: +generationMs.toFixed(1), uniqueOrigins: new Set(sim.aircraft.map(a => a.originId)).size, advisories: sim.aircraft.reduce((sum, a) => sum + (a.route.aircraft?.advisories.length || 0), 0) });
  console.log(`${profile.typeCode}/${configuration}/${movement}: ${generated}/${requested} compatible aircraft (${generationMs.toFixed(0)} ms).`);
}

if (probe) { console.log(`Probe passed: ${checkedRoutes} compatible routes; no final report written.`); process.exit(0); }
for (const snapshot of snapshots) assert.equal(digest(read(snapshot.file)), digest(snapshot.bytes), `${snapshot.file} changed during validation.`);
const report = { generatedAt: new Date().toISOString(), inputSha256: Object.fromEntries(snapshots.map(s => [s.file, digest(s.bytes)])), activeProfileIds, aircraftContextFlags: activeProfileIds, aircraftProfiles: AIRCRAFT_PROFILES, checkedRoutes, checkedPositions, physicalRunwayCrossingsChecked: crossings, unprotectedCrossings: 0, scenarios, elapsedMs: +(performance.now() - started).toFixed(1) };
writeFileSync(new URL('docs/aircraft-validation-results.json', root), JSON.stringify(report, null, 2) + '\n');
writeFileSync(new URL('docs/AIRCRAFT-VALIDATION.md', root), `# Validation des restrictions aéronef LFPG

Commande : \`npm.cmd run validate:aircraft\`. Le calcul emploie les mêmes quatre catalogues et règles que l’interface, sans restrictions liées aux travaux. Les prédictions ne délivrent aucune clairance de traversée.

Les ${checkedRoutes} parcours contrôlés respectent les contraintes d’envergure, de type et de virage lors de la recherche et du rejeu. Les ${checkedPositions.toLocaleString('fr-FR')} positions suivent exactement leurs arêtes ; les ${crossings} traversées physiques possèdent un point d’attente source antérieur adapté. Aucun parcours généré ne traverse les voies intérieures des baies de dégivrage exclues.

Chaque scénario contient 100 avions compatibles. Les origines sont d’abord essayées une fois, puis les origines compatibles peuvent être réutilisées si le type ne dispose pas de 100 stands distincts. Changer le profil de planification ne modifie pas celui du trafic déjà créé. Aucun profil de travaux n’est appliqué. Les contextes conditionnels de dégivrage et d’axes colorés sont désactivés dans ces scénarios, et vérifiés séparément par les tests de seuil et de scénario.

| Type | Envergure | Configuration | Mouvement | Avions | Origines distinctes | Génération ms |
| --- | ---: | --- | --- | ---: | ---: | ---: |
${scenarios.map(s => `| ${s.type} | ${s.wingspanMetres} m | ${s.configuration} | ${s.movement} | ${s.generated} | ${s.uniqueOrigins} | ${s.generationMs} |`).join('\n')}

Les durées mesurent le code Node.js local et excluent le rendu Canvas. D’autres contrôles peuvent s’exécuter en parallèle ; ce n’est pas un benchmark isolé.

## Reproduction

Graine : 20261113 ; Node.js : ${process.version}. Résultats détaillés : [aircraft-validation-results.json](aircraft-validation-results.json).

${snapshots.map(s => `- \`${s.file}\` : \`${digest(s.bytes)}\``).join('\n')}

Les tests indépendants comparent les virages conditionnels à un Dijkstra dirigé et vérifient les vrais identifiants des jonctions transcrites des cartes. Ils contrôlent aussi les bornes strictes ou inclusives, les profils falsifiés, les scénarios activés et les advisories d’oversteering. Les cartes restent des instantanés documentés : ce contrôle ne certifie pas l’intégralité des NOTAM ni toutes les décisions tactiques ATC.
`, 'utf8');
console.log(`Aircraft restrictions validated: ${checkedRoutes} routes, ${checkedPositions} exact positions, ${crossings} protected crossings.`);
