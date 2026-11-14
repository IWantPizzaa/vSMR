/** Real local EuroScope reports; the log contains NO authoritative taxi clearance.
 * Provisional nominal references are labelled as such, never pilot-error truth. */
import { strict as assert } from 'node:assert';
import { createHash } from 'node:crypto';
import { createReadStream, existsSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { createInterface } from 'node:readline';
import { performance } from 'node:perf_hooks';
import { AIRCRAFT_PROFILES, AircraftRestrictions, OperatingRules, RoutePlanner, SurfaceTracker, SurveillanceConflictMonitor, TaxiFlowPolicy, TaxiGraph } from '../src/core';
import { resolveOperatingManifest } from '../src/core/resolveOperatingManifest';
import type { Aircraft, AircraftProfile, AirportLocation, OperatingConfiguration, Route, TaxiEdge } from '../src/core/types';
import type { SurfaceTrackResult } from '../src/core/SurfaceTracker';

const root = new URL('../', import.meta.url), started = performance.now();
const read = (file: string) => readFileSync(new URL(file, root));
const hash = (bytes: Buffer) => createHash('sha256').update(bytes).digest('hex');
const inputFiles = ['public/data/lfpg.json', 'public/data/lfpg-operations.json', 'public/data/lfpg-corridors.json', 'public/data/lfpg-aircraft-restrictions.json'];
const codeFiles = ['scripts/replay_format.py', 'scripts/import_euroscope_replays.py', 'scripts/validate-replays.ts', 'src/core/SurfaceTracker.ts', 'src/core/SurveillanceConflictMonitor.ts', 'src/core/ConflictDetector.ts', 'src/core/TrafficPredictor.ts', 'src/core/AircraftGeometry.ts', 'src/core/AircraftDimensions.ts', 'src/core/PhysicalConflictDetector.ts', 'src/core/RoutePlanner.ts', 'src/core/OperatingRules.ts', 'src/core/TaxiFlowPolicy.ts', 'src/core/AircraftRestrictions.ts'];
const snapshots = [...inputFiles, ...codeFiles].map(file => ({ file, bytes: read(file) }));
const cacheRoot = new URL('tmp/replay-cache/', root);
if (!existsSync(new URL('inventory.json', cacheRoot))) throw new Error('Run python scripts/import_euroscope_replays.py first. Raw files are never bundled.');
const inventory = JSON.parse(readFileSync(new URL('inventory.json', cacheRoot), 'utf8')) as { files: Source[]; codeSha256: Record<string, string> };
for (const [file, digest] of Object.entries(inventory.codeSha256)) assert.equal(hash(read(file.replaceAll('\\', '/'))), digest, `Replay cache was generated with another ${file}; rebuild it.`);
const graph = new TaxiGraph(JSON.parse(snapshots[0].bytes.toString('utf8')));
const operations = resolveOperatingManifest(JSON.parse(snapshots[1].bytes.toString('utf8')));
const rules = new OperatingRules(graph, operations), flow = new TaxiFlowPolicy(graph, JSON.parse(snapshots[2].bytes.toString('utf8'))), restrictions = new AircraftRestrictions(graph, JSON.parse(snapshots[3].bytes.toString('utf8')));
assert.ok(rules.valid); assert.ok(flow.valid); assert.ok(restrictions.valid);
const profiles = new Map(AIRCRAFT_PROFILES.map(profile => [profile.typeCode, profile]));
const planners = new Map((['east', 'west'] as const).map(configuration => {
  const planner = new RoutePlanner(graph, rules, configuration); planner.setFlowPolicy(flow); planner.setAircraftRestrictions(restrictions); return [configuration, planner];
}));

interface Source { fileName: string; bytes: number; sha256: string; lastWriteTimeNs: string; cache: string; durationSeconds: number; midnightRollovers: number; counts: Record<string, number> }
interface Telemetry { aircraftId: string; observedAt: number; sourceLine: number; x: number; y: number; latitude: number; longitude: number; altitudeFeet: number; pressureAltitudeFeet: number; groundSpeedKnots: number; headingDegrees: number; reportedOnGround: boolean }
interface Plan { typeCode: string; origin: string | null; destination: string | null }
interface Track { id: string; plan: Plan; records: Telemetry[]; reference?: Route; profile?: AircraftProfile; mode?: string; configuration?: OperatingConfiguration; from?: string; to?: string }
const grid = new Map<string, TaxiEdge[]>();
for (const edge of graph.edges.values()) {
  if (edge.kind === 'runway') continue;
  const a = graph.nodes.get(edge.from)!, b = graph.nodes.get(edge.to)!;
  for (let x = Math.floor(Math.min(a.x, b.x) / 100); x <= Math.floor(Math.max(a.x, b.x) / 100); x++) for (let y = Math.floor(Math.min(a.y, b.y) / 100); y <= Math.floor(Math.max(a.y, b.y) / 100); y++) {
    const key = `${x},${y}`, cell = grid.get(key) || []; cell.push(edge); grid.set(key, cell);
  }
}
function nearestGroundDistance(record: Telemetry) {
  let best = Infinity;
  const ids = new Set<string>();
  for (let x = Math.floor((record.x - 45) / 100); x <= Math.floor((record.x + 45) / 100); x++) for (let y = Math.floor((record.y - 45) / 100); y <= Math.floor((record.y + 45) / 100); y++) for (const edge of grid.get(`${x},${y}`) || []) {
    if (ids.has(edge.id)) continue; ids.add(edge.id);
    const a = graph.nodes.get(edge.from)!, b = graph.nodes.get(edge.to)!, dx = b.x - a.x, dy = b.y - a.y;
    const t = Math.max(0, Math.min(1, ((record.x - a.x) * dx + (record.y - a.y) * dy) / (dx * dx + dy * dy)));
    best = Math.min(best, Math.hypot(record.x - a.x - dx * t, record.y - a.y - dy * t));
  }
  return best;
}
const distance = (point: { x: number; y: number }, other: { x: number; y: number }) => Math.hypot(point.x - other.x, point.y - other.y);
function distanceToReference(point: Telemetry, route: Route) {
  let best = Infinity;
  for (const id of route.edgeIds) {
    const edge = graph.edges.get(id)!, a = graph.nodes.get(edge.from)!, b = graph.nodes.get(edge.to)!, dx = b.x - a.x, dy = b.y - a.y;
    const t = Math.max(0, Math.min(1, ((point.x - a.x) * dx + (point.y - a.y) * dy) / (dx * dx + dy * dy)));
    best = Math.min(best, Math.hypot(point.x - a.x - dx * t, point.y - a.y - dy * t));
  }
  return best;
}
const stands = rules.locations.filter(location => location.kind === 'stand' && location.nodeId && flow.hasMappedStand(location.id));
function stationaryAnchor(records: Telemetry[], atEnd: boolean): { index: number; location: AirportLocation } | null {
  const start = atEnd ? Math.max(0, records.length - 80) : 0, end = atEnd ? records.length : Math.min(80, records.length);
  let best: { index: number; location: AirportLocation; distance: number } | null = null;
  for (let i = start; i + 2 < end; i++) {
    if (records[i].groundSpeedKnots > 1 || records[i + 1].groundSpeedKnots > 1 || records[i + 2].groundSpeedKnots > 1 || records[i + 2].observedAt - records[i].observedAt < 8 || distance(records[i], records[i + 2]) > 12) continue;
    for (const location of stands) {
      const d = distance(records[i], location);
      if (d < 65 && (!best || d < best.distance)) best = { index: atEnd ? i + 2 : i, location, distance: d };
    }
  }
  return best;
}
function inferStandard(track: Track): string | null {
  const profile = profiles.get(track.plan.typeCode); if (!profile) return 'unknown-or-unsupported-profile';
  const departure = track.plan.origin === 'LFPG' && track.plan.destination !== 'LFPG', arrival = track.plan.destination === 'LFPG' && track.plan.origin !== 'LFPG';
  if (!departure && !arrival) return 'unknown-movement';
  const stand = stationaryAnchor(track.records, arrival); if (!stand) return 'no-stable-stand-anchor';
  const accesses = operations.runwayAccess!.filter(access => access.role === (departure ? 'departure' : 'arrival') && !access.pprRequired);
  let best: { index: number; location: AirportLocation; configuration: OperatingConfiguration; distance: number } | null = null;
  const begin = departure ? Math.max(stand.index + 12, Math.floor(track.records.length * .6)) : 0;
  const end = arrival ? Math.min(stand.index - 12, Math.ceil(track.records.length * .4)) : track.records.length;
  for (let index = Math.max(0, begin); index < end; index++) for (const access of accesses) {
    const location = rules.locations.find(l => l.id === access.locationId)!;
    const d = distance(track.records[index], location);
    if (d < 180 && (!best || d < best.distance)) best = { index, location, configuration: /^(08|09)/.test(access.runway) ? 'east' : 'west', distance: d };
  }
  if (!best) return 'no-runway-access-anchor';
  const from = departure ? stand.location : best.location, to = departure ? best.location : stand.location;
  const movement = departure ? 'departure' : 'arrival', planner = planners.get(best.configuration)!;
  const route = planner.findRoute(from.nodeId!, to.nodeId!, { originId: from.id, destinationId: to.id, movement, aircraftProfile: profile });
  if (!route) return 'nominal-route-unavailable';
  const first = departure ? stand.index : best.index, last = departure ? best.index : stand.index;
  if (last - first < 12 || track.records[last].observedAt - track.records[first].observedAt < 60) return 'reference-window-too-short';
  track.records = track.records.slice(first, last + 1);
  // The recording carries positions, NOT the controller's crossing clearance.
  // Do not label an observed crossing as unauthorized because that field is absent.
  track.reference = { ...route, operational: route.operational ? { ...route.operational, clearanceCheckpoints: [] } : undefined };
  track.profile = profile; track.mode = movement; track.configuration = best.configuration; track.from = from.id; track.to = to.id;
  return null;
}

const selectedNames = new Set(['PBN_08_05_26.txt', 'PBN_09_04_26.txt', 'PBN_20_03_26.txt', 'PBN_21_02_26.txt', 'TGL_SightSeeing_LFPG_22_03_26.txt', 'WF2025.txt']);
const results: Record<string, unknown>[] = [], detailedTracks: Record<string, unknown>[] = [];
let totalRetained = 0, totalCompared = 0, totalReferences = 0, totalCandidates = 0;
for (const source of inventory.files.filter(item => selectedNames.has(item.fileName))) {
  const metadataName = source.cache.replace('.jsonl', '.metadata.json');
  const metadata = JSON.parse(readFileSync(new URL(metadataName, cacheRoot), 'utf8')) as { targets: Record<string, Plan> };
  const tracks = new Map<string, Track>();
  let nearGraph = 0, rejectedAltitudeOrSpeed = 0, rejectedGraphDistance = 0;
  const deltas: number[] = [], previousTime = new Map<string, number>();
  for await (const line of createInterface({ input: createReadStream(new URL(source.cache, cacheRoot)), crlfDelay: Infinity })) {
    const record = JSON.parse(line) as Telemetry;
    const prior = previousTime.get(record.aircraftId); if (prior !== undefined && record.observedAt > prior) deltas.push(record.observedAt - prior); previousTime.set(record.aircraftId, record.observedAt);
    if (record.altitudeFeet < 200 || record.altitudeFeet > 600 || record.groundSpeedKnots > 40) { rejectedAltitudeOrSpeed++; continue; }
    if (nearestGroundDistance(record) > 45) { rejectedGraphDistance++; continue; }
    nearGraph++;
    const track = tracks.get(record.aircraftId) || { id: record.aircraftId, plan: metadata.targets[record.aircraftId], records: [] };
    track.records.push(record); tracks.set(track.id, track);
  }
  const eligible = [...tracks.values()].filter(track => track.records.length >= 12 && track.records.at(-1)!.observedAt - track.records[0].observedAt >= 60 && track.records.filter(r => r.groundSpeedKnots > 1).length >= 5);
  const skipped: Record<string, number> = {};
  const comparable: Track[] = [];
  for (const track of eligible) {
    const reason = inferStandard(track);
    if (reason) skipped[reason] = (skipped[reason] || 0) + 1; else comparable.push(track);
  }
  const tracker = new SurfaceTracker(graph), monitor = new SurveillanceConflictMonitor(graph, { physicalEnvelopes: true });
  const records = comparable.flatMap(track => track.records.map(record => ({ record, track }))).sort((a, b) => a.record.observedAt - b.record.observedAt || a.record.aircraftId.localeCompare(b.record.aircraftId));
  const counts = new Map(comparable.map(track => [track.id, { reports: 0, accepted: 0, ambiguous: 0, suspected: 0, confirmed: 0, wrongBranch: 0, wrongDirection: 0, rejected: 0, confirmedWithin10MetresOfReference: 0 }]));
  const latest = new Map<string, { record: Telemetry; track: Track; result: SurfaceTrackResult; orientationAnchor: Telemetry; bodyAxis?: number }>();
  const candidatePairs = new Set<string>(), candidateTypes: Record<string, number> = {}, examples: unknown[] = [];
  let predictionCycles = 0, lastPrediction = -Infinity, snapshotsWithoutEstablishedOrientation = 0, snapshotsWithInferredOrientation = 0;
  for (const { record, track } of records) {
    const result = tracker.ingest({ aircraftId: track.id, observedAt: record.observedAt, receivedAt: record.observedAt, x: record.x, y: record.y,
      groundSpeedKnots: record.groundSpeedKnots, positionAccuracyMetres: 6,
      // PBH orientation is decoded and retained, but the replay does not declare
      // its true/magnetic reference. Geometric displacement supplies true course.
    }, track.reference!, record.observedAt);
    const stat = counts.get(track.id)!; stat.reports++; if (!result.accepted) stat.rejected++; else stat.accepted++;
    if (result.status === 'ambiguous') stat.ambiguous++;
    if (result.status === 'suspected-off-track') stat.suspected++;
    if (result.status === 'confirmed-off-track') { stat.confirmed++; if (result.deviationKind === 'wrong-branch') stat.wrongBranch++; if (result.deviationKind === 'wrong-direction') stat.wrongDirection++; if (distanceToReference(record, track.reference!) < 10) stat.confirmedWithin10MetresOfReference++; }
    if (result.accepted) {
      const prior = latest.get(track.id), anchor = prior?.orientationAnchor ?? prior?.record ?? record;
      let bodyAxis = prior?.bodyAxis, orientationAnchor = anchor;
      if (record.observedAt - anchor.observedAt > 0 && record.observedAt - anchor.observedAt <= 30 && distance(record, anchor) >= 12) {
        bodyAxis = Math.atan2(record.y - anchor.y, record.x - anchor.x); orientationAnchor = record;
      } else if (prior && bodyAxis !== undefined) {
        // Relative PBH rotation is useful without pretending its absolute angle
        // is referenced to true north. The initial axis remains an estimate.
        const delta = (record.headingDegrees - prior.record.headingDegrees) * Math.PI / 180;
        bodyAxis += Math.atan2(Math.sin(delta), Math.cos(delta));
      }
      if (record.observedAt - anchor.observedAt > 30 && record.groundSpeedKnots > 1) { bodyAxis = undefined; orientationAnchor = record; }
      latest.set(track.id, { record, track, result, orientationAnchor, bodyAxis });
    }
    if (record.observedAt - lastPrediction < 5) continue;
    lastPrediction = record.observedAt; predictionCycles++;
    const fleet: Aircraft[] = [], quality: SurfaceTrackResult[] = [];
    for (const [id, value] of latest) {
      const estimate = tracker.estimate(id, record.observedAt); if (!estimate || estimate.ageSeconds > 20) { latest.delete(id); continue; }
      const observed = value.result.observation; if (!observed) continue;
      if (value.bodyAxis === undefined) { snapshotsWithoutEstablishedOrientation++; continue; }
      snapshotsWithInferredOrientation++;
      const edge = graph.edges.get(observed.edgeId)!, from = graph.nodes.get(observed.fromNodeId)!, to = graph.nodes.get(observed.toNodeId)!;
      // Real replay forecasts stop at the current measured arc's next node.
      // Its following turn and ATC clearance are unknown, never invented.
      const speed = value.result.speedMetresPerSecond, fraction = Math.min(1, observed.edgeProgress + speed * Math.min(10, estimate.ageSeconds) / edge.length);
      const observation = { ...observed, edgeProgress: fraction }, x = from.x + (to.x - from.x) * fraction, y = from.y + (to.y - from.y) * fraction;
      const route: Route = { nodeIds: [from.id, to.id], edgeIds: [edge.id], distance: edge.length, cumulativeDistances: [0, edge.length] };
      const aircraft: Aircraft = { id, callsign: id, originId: track.from!, destinationId: track.to!, aircraftProfile: value.track.profile,
        route, distance: edge.length * fraction, speed: Math.max(.1, speed), state: speed > .5 ? 'taxiing' : 'holding',
        x, y, heading: Math.atan2(to.y - from.y, to.x - from.x), bodyHeading: value.bodyAxis,
        edgeId: edge.id, fromNodeId: from.id, toNodeId: to.id, edgeProgress: fraction };
      assert.ok(Number.isFinite(x) && Number.isFinite(y));
      fleet.push(aircraft); quality.push({ ...estimate, observation, x, y, speedMetresPerSecond: speed });
    }
    const alerts = monitor.detect(fleet, quality, record.observedAt, 10);
    for (const alert of alerts) {
      assert.ok(Number.isFinite(alert.timeToConflict) && alert.timeToConflict >= 0 && alert.timeToConflict <= 10);
      const key = alert.aircraftIds.join('|');
      if (!candidatePairs.has(key)) {
        candidatePairs.add(key); candidateTypes[alert.type] = (candidateTypes[alert.type] || 0) + 1;
        if (examples.length < 12) examples.push({ at: record.observedAt, ids: alert.aircraftIds, type: alert.type, quality: alert.quality, timeToConflict: +alert.timeToConflict.toFixed(2) });
      }
    }
  }
  const sortedDeltas = deltas.sort((a, b) => a - b), percentile = (p: number) => sortedDeltas[Math.min(sortedDeltas.length - 1, Math.floor(sortedDeltas.length * p))] ?? null;
  const histogram = Object.fromEntries([...new Set(sortedDeltas)].sort((a, b) => a - b).filter(n => n <= 30).map(n => [String(n), sortedDeltas.filter(x => x === n).length]));
  const compared = [...counts.values()].reduce((sum, item) => sum + item.reports, 0);
  const confirmedTrackCount = [...counts.values()].filter(item => item.confirmed > 0).length;
  for (const track of comparable) detailedTracks.push({ source: source.fileName, id: track.id, type: track.profile!.typeCode, movement: track.mode, configuration: track.configuration, inferredFrom: track.from, inferredTo: track.to, ...counts.get(track.id) });
  const result = { source: source.fileName, sourceSha256: source.sha256, surfaceCandidates: source.counts.surfaceCandidates, nearGraphSurfaceReports: nearGraph, rejectedAltitudeOrSpeed, rejectedGraphDistance,
    eligibleMovingTracks: eligible.length, provisionalStandardReferences: comparable.length, comparedReports: compared, tracksWithConfirmedStandardDifference: confirmedTrackCount,
    skippedReferences: skipped, reportIntervalSeconds: { count: deltas.length, median: percentile(.5), p95: percentile(.95), greaterThan10: deltas.filter(t => t > 10).length, greaterThan30: deltas.filter(t => t > 30).length, histogram },
    predictionCycles, snapshotsWithoutEstablishedOrientation, snapshotsWithInferredOrientation, physicalProximityCandidatePairs: candidatePairs.size, candidateTypes, candidateExamples: examples };
  results.push(result); totalRetained += nearGraph; totalCompared += compared; totalReferences += comparable.length; totalCandidates += candidatePairs.size;
  console.log(`${source.fileName}: ${nearGraph} mapped surface reports; ${comparable.length} provisional references/${compared} replayed reports; ${candidatePairs.size} proximity candidate pairs (unlabelled).`);
}
assert.equal(results.length, selectedNames.size);
assert.ok(totalReferences >= 12 && totalCompared >= 1000, 'Real replay validation must exercise substantial observed traffic, not an empty pass.');
for (const item of snapshots) assert.equal(hash(read(item.file)), hash(item.bytes), `${item.file} changed during replay validation.`);
const summary = { generatedAt: new Date().toISOString(), parser: 'incoming FSD @ reports with independent receipt clocks, anonymous sessions and duplicates removed',
  inventory: { filesScanned: inventory.files.length, originalBytesRead: inventory.files.reduce((s, f) => s + f.bytes, 0), filesWithLfpgCandidates: inventory.files.filter(f => f.counts.surfaceCandidates > 0).length,
    lfpgSurfaceCandidates: inventory.files.reduce((s, f) => s + f.counts.surfaceCandidates, 0), sourceFiles: inventory.files },
  selectedSessions: results.length, mappedSurfaceReports: totalRetained, provisionalStandardReferences: totalReferences, comparedReports: totalCompared, physicalProximityCandidatePairs: totalCandidates,
  tracksWithConfirmedStandardDifference: detailedTracks.filter(track => Number(track.confirmed) > 0).length,
  confirmedStateReportsWithin10MetresOfReference: detailedTracks.reduce((sum, track) => sum + Number(track.confirmedWithin10MetresOfReference), 0),
  implausibleOrRejectedReports: detailedTracks.reduce((sum, track) => sum + Number(track.rejected), 0),
  groundTruthTaxiClearancesAvailable: false, groundTruthCollisionLabelsAvailable: false, falsePositiveRateMeasured: false,
  physicalEnvelopeMode: 'body-wing-tail-envelope; inferred geometric motion axis plus relative PBH rotation, not a verified true-body heading',
  limitations: ['The real logs do not provide the issued taxi route, controller crossing clearances or labelled collisions. Standard-route differences are review candidates, never proven pilot errors.',
    'The archived sessions predate the pinned current LFPG operational data; works are excluded, but geometry/procedures may still differ historically.',
    'The logged clock has one-second precision. Duplicate/nonadvancing timestamps are not independent confirmation reports.',
    'The FSD ground bit is missing for most reports. Conservative altitude/speed/taxiway-distance and sustained-track filters retain surface-like contexts, not an asserted SDK ground sensor.',
    'Raw PBH heading is preserved with its decoded scale; because reference is not declared in the files, geometric displacement is used for true course.',
    'Physical body orientation is estimated only after a sustained geometric displacement, then updated by relative PBH rotation. Before that, snapshots are excluded from the physical-envelope comparison; the estimated nose direction during pushback remains uncertain.',
    'Collision review is limited to tracks with a supported profile and plausible nominal anchors, projected at most10 seconds to the current observed arc endpoint. No unknown future turn or clearance is invented.'],
  inputSha256: Object.fromEntries(snapshots.filter(s => inputFiles.includes(s.file)).map(s => [s.file, hash(s.bytes)])), codeSha256: Object.fromEntries(snapshots.filter(s => !inputFiles.includes(s.file)).map(s => [s.file, hash(s.bytes)])),
  sessions: results, comparedTracks: detailedTracks, elapsedMs: +(performance.now() - started).toFixed(1) };
writeFileSync(new URL('docs/replay-validation-results.json', root), JSON.stringify(summary, null, 2) + '\n');
writeFileSync(new URL('docs/REPLAY-VALIDATION.md', root), `# Validation sur les replays EuroScope locaux

Les fichiers originaux de \`aa-REPLAYS\` sont ouverts en lecture seule. Le scan parcourt **${inventory.files.length} fichiers**, soit **${(summary.inventory.originalBytesRead / 1e9).toFixed(3)} Go** ; **${summary.inventory.filesWithLfpgCandidates}** contiennent des positions proches de LFPG, indépendamment du nom de fichier. Les dérivés ignorés de \`tmp/replay-cache\` ne conservent que la télémétrie anonyme, le type et les aéroports. Les messages d'authentification, identités personnelles et conversations ne sont ni exportés ni nécessaires aux tests.

Les **${results.length} sessions** ci-dessous donnent **${totalRetained.toLocaleString('fr-FR')} rapports de surface proches du graphe**. **${totalReferences} références de parcours standard provisoires** sont rapprochées de **${totalCompared.toLocaleString('fr-FR')} rapports réellement enregistrés**. Le suivi emploie les intervalles réels du journal, sans les remplacer par une cadence artificielle de cinq secondes. Les horodatages sont ceux de réception du journal, à la seconde, avec passage de minuit déroulé ; les répétitions au même instant ne comptent pas comme nouveaux votes.

**Ces replays ne contiennent pas la clairance complète de roulage attribuée ni un étiquetage des collisions.** On ne peut donc pas annoncer un taux de faux positifs ou assimiler les différences au standard à des erreurs de pilote. Le stand immobile, le point d'accès de piste proche, le plan de vol et un profil connu permettent seulement de construire une référence plausible à examiner. Les clairances de traversée inconnues ne sont pas évaluées comme des infractions ; les projections de conflit s'arrêtent au prochain nœud du segment actuellement observé, au plus à dix secondes.

**${summary.tracksWithConfirmedStandardDifference} des ${totalReferences} trajectoires comparées présentent au moins une différence persistante avec la référence standard inférée.** Ce résultat non nul est conservé : il signale des différences de cheminement, d'ancrage supposé ou de données historiques à examiner, et ne prouve pas une faute du pilote. ${summary.confirmedStateReportsWithin10MetresOfReference} rapports classés dans un état d'écart confirmé restent géométriquement à moins de10m du tracé de référence, ce qui mérite aussi une revue des raccordements proches. ${summary.implausibleOrRejectedReports} rapports sont rejetés par les garde-fous de suivi.

| Session | Rapports surface proches du graphe | Références provisoires | Rapports comparés | Paires de proximité à examiner |
| --- | ---: | ---: | ---: | ---: |
${results.map(s => `| ${s.source} | ${s.nearGraphSurfaceReports} | ${s.provisionalStandardReferences} | ${s.comparedReports} | ${s.physicalProximityCandidatePairs} |`).join('\n')}

| Session | Intervalle médian entre rapports retenus | 95e percentile |
| --- | ---: | ---: |
${results.map(s => { const interval = s.reportIntervalSeconds as { median: number; p95: number }; return `| ${s.source} | ${interval.median} s | ${interval.p95} s |`; }).join('\n')}

Les intervalles ci-dessus viennent des rapports de la même génération de cible, dans la sélection géographique/altitude/vitesse. Un grand intervalle peut aussi refléter une sortie de cette sélection ; ce n'est pas à lui seul une preuve de perte réseau.

Le bit FSD sol n'est pas renseigné dans la majorité des rapports. Le filtre retient une altitude entre200 et600 pieds, une vitesse de40kt maximum, une distance de45m maximum aux voies/positions de stationnement, puis des trajectoires durables avec mouvement. Il ne prétend pas transformer ces critères en capteur sol/vol. Les enregistrements2025/2026 sont comparés au graphe et aux règles actuellement livrés : les restrictions de travaux sont exclues, mais les différences historiques restent possibles.

Les champs \`@\`, l'altitude en pieds, la vitesse en nœuds et le PBH sont vérifiés contre les sources primaires [Swift PilotDataUpdate](https://apidocs.swift-project.org/pilotdataupdate_8cpp_source.html) et [Swift PBH](https://apidocs.swift-project.org/pbh_8h_source.html). Le cap PBH est décodé comme bits2–11 multipliés par360/1024 ; sa référence vrai/magnétique n'étant pas déclarée par le journal, le tracker exploite le déplacement géométrique pour sa direction vraie.

Le contrôle des proximités utilise les nouvelles enveloppes physiques, avec longueur, envergure et orientation. L'axe du corps est une **estimation** obtenue après au moins12m de déplacement géométrique, puis mise à jour par les variations relatives du PBH ; ce n'est pas un cap vrai de fuselage vérifié. Les premières positions stationnaires sans axe établi sont exclues de cette comparaison. La direction du nez pendant un repoussage reste une limite de cet oracle. Les résultats sont des candidats à examiner et ne prouvent aucun impact réel.

${results.reduce((sum, s) => sum + Number(s.snapshotsWithInferredOrientation), 0)} instantanés actifs possèdent un axe estimé pour cette comparaison physique ; ${results.reduce((sum, s) => sum + Number(s.snapshotsWithoutEstablishedOrientation), 0)} sont exclus faute d'orientation établie. Il s'agit de comptages par cycle de prévision, pas d'autant de rapports indépendants supplémentaires.

## Reproduction

\`python scripts/import_euroscope_replays.py\` lit le répertoire local fourni, puis \`npm.cmd run validate:replays\` rejoue les caches. \`python tests/test_euroscope_replay.py\` vérifie le format, le passage de minuit, le PBH, les unités et le filtrage des données non télémétriques. Résultats, intervalles, exclusions, identifiants anonymes et empreintes des fichiers : [replay-validation-results.json](replay-validation-results.json). Les originaux ne sont pas inclus dans le projet.
`, 'utf8');
console.log(`Real replays validated: ${totalCompared} independent recorded reports against ${totalReferences} provisional references across ${results.length} sessions; ${totalCandidates} unlabelled proximity pairs.`);
