/** Physical regressions from the user's route screenshots, not generated-catalog oracles. */
import { strict as assert } from 'node:assert';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import { TaxiGraph } from '../src/core/TaxiGraph';
import { OperatingRules, type OperatingManifest } from '../src/core/OperatingRules';
import { resolveOperatingManifest } from '../src/core/resolveOperatingManifest';
import { TaxiFlowPolicy, type TaxiFlowManifest } from '../src/core/TaxiFlowPolicy';
import { RoutePlanner } from '../src/core/RoutePlanner';
import { AircraftRestrictions, type AircraftRestrictionManifest } from '../src/core/AircraftRestrictions';
import { DEFAULT_AIRCRAFT_PROFILE } from '../src/core/AircraftProfile';
import type { AirportData, MovementMode, OperatingConfiguration, Route, TaxiEdge } from '../src/core/types';

const root = new URL('../', import.meta.url);
const data = JSON.parse(readFileSync(new URL('public/data/lfpg.json', root), 'utf8')) as AirportData;
const operations = JSON.parse(readFileSync(new URL('public/data/lfpg-operations.json', root), 'utf8')) as OperatingManifest;
const corridors = JSON.parse(readFileSync(new URL('public/data/lfpg-corridors.json', root), 'utf8')) as TaxiFlowManifest;
const aircraftManifest = JSON.parse(readFileSync(new URL('public/data/lfpg-aircraft-restrictions.json', root), 'utf8')) as AircraftRestrictionManifest;
type SourceEdge = TaxiEdge & { sourceWayId?: number };
// Source centerline identities are transcribed separately from the frozen graph
// and visually checked against GMC01/02 + the user screenshots. See source notes.
const ways = {
  G3: new Set([159161262]),
  F: new Set([172796083, 222939600]),
  N: new Set([112642877, 222939637, 222951652]),
  R: new Set([622439759, 170606401]),
  D: new Set([156840791, 967374616]),
  Q: new Set([179434334, 1528268012, 1528268013, 222948631, 222939647]),
  B: new Set([156840795]),
  ROMEO: new Set([112642886, 622439721, 622439717]),
  NE3_NE4: new Set([1092616455, 222981758]),
  MD1_MD2: new Set([156840762, 222939612]),
  P1: new Set([520141421]),
};

function setup(configuration: OperatingConfiguration) {
  const manifest = resolveOperatingManifest(operations);
  const graph = new TaxiGraph(data), rules = new OperatingRules(graph, manifest);
  const policy = new TaxiFlowPolicy(graph, corridors), planner = new RoutePlanner(graph, rules, configuration);
  assert.ok(rules.valid && policy.valid, 'Catalog validation must pass before a physical route regression.');
  planner.setFlowPolicy(policy);
  const aircraftRules = new AircraftRestrictions(graph, aircraftManifest);
  assert.ok(aircraftRules.valid, aircraftRules.diagnostics.join('; '));
  planner.setAircraftRestrictions(aircraftRules); planner.setAircraftProfile(DEFAULT_AIRCRAFT_PROFILE);
  const activeProfileIds = Array.isArray(manifest.activeProfileIds) ? manifest.activeProfileIds.filter((id): id is string => typeof id === 'string') : [];
  planner.setAircraftRestrictionContext({ airportFlags: activeProfileIds });
  function route(from: string, to: string, movement: MovementMode = 'departure'): Route {
    const origin = planner.locations.find(location => location.id === from), destination = planner.locations.find(location => location.id === to);
    assert.ok(origin?.nodeId && destination?.nodeId, `Missing physical endpoints: ${from}/${to}`);
    assert.ok(rules.isLocationOpen(from) && rules.isLocationOpen(to), 'Default works must be active when testing the actual route.');
    const result = planner.findRoute(origin.nodeId, destination.nodeId, { movement, originId: from, destinationId: to });
    assert.ok(result, `${configuration}/${movement} ${from} -> ${to}: ${planner.lastFlowFailure}`);
    assert.ok(planner.isRouteAllowed(result, movement, { originId: from, destinationId: to }), 'Replay the edge chain with the same endpoint and clearance restrictions.');
    assert.equal(new Set(result.nodeIds).size, result.nodeIds.length, 'A convergence preference must not introduce a physical loop.');
    let length = 0;
    for (const [index, id] of result.edgeIds.entries()) {
      const edge = graph.edges.get(id)!;
      assert.notEqual(edge.kind, 'runway', 'Runways cannot replace principal taxiways.');
      assert.deepEqual(new Set([edge.from, edge.to]), new Set([result.nodeIds[index], result.nodeIds[index + 1]]), 'Every step must follow the original edge endpoints.');
      assert.ok(planner.allowArc(edge, result.nodeIds[index], result.nodeIds[index + 1], movement));
      length += edge.length;
    }
    assert.ok(Math.abs(length - result.distance) < .01, 'Distance must be the actual source-edge length.');
    return result;
  }
  function legs(path: Route, selected: Set<number>) {
    return path.edgeIds.flatMap((id, index) => {
      const edge = graph.edges.get(id) as SourceEdge;
      if (!selected.has(edge.sourceWayId || -1)) return [];
      const from = graph.nodes.get(path.nodeIds[index])!, to = graph.nodes.get(path.nodeIds[index + 1])!;
      return [{ edge, index, from, to, dx: to.x - from.x, dy: to.y - from.y }];
    });
  }
  return { graph, planner, route, legs };
}

test('B12 departures physically use G3 west then F south and a continuous southern principal axis', () => {
  const { route, legs } = setup('west');
  for (const holding of ['T9', 'T10', 'T11', 'T12']) {
    const path = route('stand:B12', `operation:departure:26R:${holding}`);
    const g3 = legs(path, ways.G3), f = legs(path, ways.F), r = legs(path, ways.R);
    assert.ok(g3.reduce((sum, leg) => sum - leg.dx, 0) > 400, 'The aircraft must actually leave B12 westward along G3.');
    assert.ok(f.reduce((sum, leg) => sum - leg.dy, 0) > 300, 'GMC02 connects G3 to the southbound F stream.');
    assert.ok(r.reduce((sum, leg) => sum + leg.dx, 0) > 100, 'The southern R frontage must physically lead toward the eastern holding points.');
    assert.ok(g3.at(-1)!.index < f[0].index && f.at(-1)!.index < r[0].index, 'Source-axis traversal order must be G3, F, then R/T.');
    for (const leg of legs(path, ways.N)) assert.ok(leg.dy >= -.3, 'User example does not authorize reversing the nominal northbound N arrows.');
    assert.equal(legs(path, ways.ROMEO).length + legs(path, ways.NE3_NE4).length, 0, 'No transit via unrelated deicing centerlines.');
    assert.ok(path.distance < 5100, 'Avoid a cross-airport detour to satisfy metadata about a named axis.');
  }
});

test('north-south transfers stay on F/N instead of using the internal ROMEO RN/RS spine', () => {
  const cases = [
    ['east', 'departure', 'stand:F02', 'operation:departure:09R:D3', 'F', 1],
    ['west', 'arrival', 'operation:arrival:27R:K3', 'stand:F02', 'F', -1],
    ['east', 'arrival', 'operation:arrival:09L:K6', 'stand:F02', 'N', -1],
    ['west', 'departure', 'stand:A39', 'operation:departure:27L:Q4', 'N', 1],
  ] as const;
  for (const [configuration, movement, from, to, axis, sign] of cases) {
    const { route, legs } = setup(configuration), path = route(from, to, movement);
    assert.equal(legs(path, ways.ROMEO).length, 0, `${configuration}/${movement} ${from} -> ${to} must not substitute ROMEO for ${axis}.`);
    const main = legs(path, ways[axis]);
    assert.ok(main.reduce((sum, leg) => sum + sign * leg.dy, 0) > 900, 'Test the physical continuous corridor, not a single arrow or flow label.');
    for (const leg of main) assert.ok(sign * leg.dy >= -.3, `All traversed ${axis} segments must follow the source cardinal direction.`);
  }
});

test('Q convergence avoids NE3/NE4 bay shortcuts and J departure keeps the open Q trunk', () => {
  const { route, legs } = setup('west');
  for (const from of ['stand:A39', 'stand:B24', 'stand:J14']) {
    const path = route(from, 'operation:departure:27L:Q4');
    assert.equal(legs(path, ways.NE3_NE4).length, 0, 'QB5/QB6 deicing bay centerlines are not ordinary through shortcuts.');
  }
  const j = route('stand:J14', 'operation:departure:27L:Q4'), q = legs(j, ways.Q);
  assert.ok(q.reduce((sum, leg) => sum + leg.edge.length, 0) > 1800, 'J14 must actually join the eastern Q trunk.');
  assert.equal(legs(j, ways.B).length, 0, 'The open nominal Q path should not leave via QB12, run along B, then return via QB8.');
  assert.ok(q.reduce((sum, leg) => sum + leg.dy, 0) > 1000, 'Q must make the actual northbound crossing between terminal and northern frontage.');
  for (const leg of q.filter(leg => Math.abs(leg.dy) > Math.abs(leg.dx))) assert.ok(leg.dy >= -.3, 'The vertical Q trunk remains northbound; its westward northern bend naturally slopes slightly south.');
});

test('A39/P1 and the D3 approach keep genuine connectors without visiting a foreign stand', () => {
  for (const configuration of ['east', 'west'] as const) {
    const { graph, route, legs } = setup(configuration);
    const path = route('stand:A39', 'stand:D12', 'transfer');
    assert.ok(legs(path, ways.P1).length > 0, 'The genuine P1 apron frontage remains connected; it is not a deicing bay.');
    const parkingWays = path.edgeIds.map(id => graph.edges.get(id) as SourceEdge).filter(edge => edge.kind === 'parking_position');
    assert.ok(parkingWays.every(edge => [1092605080, 236527649].includes(edge.sourceWayId || -1)), 'Only origin A39 and destination D12 stand leads may be used.');
  }
  const { route, legs } = setup('east'), d3 = route('stand:F02', 'operation:departure:09R:D3');
  assert.equal(legs(d3, ways.MD1_MD2).length, 0, 'This straight D approach must not dip to M through the MD1/MD2 junction.');
  assert.ok(legs(d3, ways.D).reduce((sum, leg) => sum - leg.dx, 0) > 1300, 'D3 is approached along a substantial physical D segment.');
});
