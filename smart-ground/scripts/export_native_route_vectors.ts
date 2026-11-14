import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { AircraftRestrictions, OperatingRules, RoutePlanner, TaxiFlowPolicy, TaxiGraph } from '../src/core';
import { resolveOperatingManifest } from '../src/core/resolveOperatingManifest';

const root = new URL('../', import.meta.url);
const read = (name: string) => JSON.parse(readFileSync(new URL(`public/data/${name}`, root), 'utf8'));
const graph = new TaxiGraph(read('lfpg.json'));
const operating = new OperatingRules(graph, resolveOperatingManifest(read('lfpg-operations.json')));
const flow = new TaxiFlowPolicy(graph, read('lfpg-corridors.json'));
const aircraft = new AircraftRestrictions(graph, read('lfpg-aircraft-restrictions.json'));
const cases = [
  ['east', 'A320', 35.8, 'stand:B08', 'operation:departure:09R:D3'],
  ['east', 'A320', 35.8, 'stand:M09', 'operation:departure:08L:T3'],
  ['east', 'A320', 35.8, 'stand:B03', 'operation:departure:09R:D5'],
  ['west', 'A320', 35.8, 'stand:B12', 'operation:departure:26R:T9'],
  ['west', 'A320', 35.8, 'stand:F02', 'operation:departure:26R:T11'],
  ['east', 'A320', 35.8, 'operation:arrival:09L:K6', 'stand:F02'],
  ['west', 'A320', 35.8, 'operation:arrival:27R:K3', 'stand:I24'],
  ['east', 'A388', 79.75, 'stand:K09', 'operation:departure:08L:T4'],
  ['west', 'A388', 79.75, 'stand:Y08', 'operation:departure:26R:T11'],
  ['east', 'B744', 64.92, 'stand:K09', 'operation:departure:08L:T4'],
  ['west', 'B748', 68.4, 'stand:Y08', 'operation:departure:26R:T11'],
] as const;
const vectors = cases.map(([configuration, typeCode, wingspanMetres, origin, destination]) => {
  const movement = origin.startsWith('operation:arrival') ? 'arrival' : 'departure';
  const runway = (movement === 'arrival' ? origin : destination).split(':')[2];
  const planner = new RoutePlanner(graph, operating, configuration); planner.setFlowPolicy(flow); planner.setAircraftRestrictions(aircraft);
  const profile = { typeCode, wingspanMetres };
  const from = planner.locations.find(l => l.id === origin)!, to = planner.locations.find(l => l.id === destination)!;
  const route = planner.findRoute(from.nodeId!, to.nodeId!, { movement, originId: origin, destinationId: destination, aircraftProfile: profile });
  if (!route) throw new Error(`No reference route: ${origin}/${destination}/${typeCode}`);
  return { configuration, movement, runway, profile, origin, destination, distance: route.distance, nodeIds: route.nodeIds, edgeIds: route.edgeIds };
});
const destination = new URL('../vSMR/tests/fixtures/smart_ground_routes.json', root);
mkdirSync(new URL('./', destination), { recursive: true });
writeFileSync(destination, JSON.stringify({ sourceGraphSha256: createHash('sha256').update(readFileSync(new URL('public/data/lfpg.json', root))).digest('hex'), vectors }, null, 2) + '\n');
console.log(`${vectors.length} native/TypeScript route parity vectors written.`);
