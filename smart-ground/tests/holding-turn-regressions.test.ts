/** Source geometry oracles for the user's exact B08/D3, M09/T3 and B03/D5 cases. */
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
import type { AirportData, OperatingConfiguration, Route, TaxiEdge } from '../src/core/types';

const root = new URL('../', import.meta.url);
const airport = JSON.parse(readFileSync(new URL('public/data/lfpg.json', root), 'utf8')) as AirportData;
const operating = JSON.parse(readFileSync(new URL('public/data/lfpg-operations.json', root), 'utf8')) as OperatingManifest;
const flows = JSON.parse(readFileSync(new URL('public/data/lfpg-corridors.json', root), 'utf8')) as TaxiFlowManifest;
const aircraftManifest = JSON.parse(readFileSync(new URL('public/data/lfpg-aircraft-restrictions.json', root), 'utf8')) as AircraftRestrictionManifest;
type SourceEdge = TaxiEdge & { sourceWayId?: number };

function setup(configuration: OperatingConfiguration) {
  const manifest = resolveOperatingManifest(operating);
  const graph = new TaxiGraph(airport), rules = new OperatingRules(graph, manifest);
  const planner = new RoutePlanner(graph, rules, configuration), policy = new TaxiFlowPolicy(graph, flows);
  assert.ok(rules.valid && policy.valid);
  planner.setFlowPolicy(policy);
  const aircraftRules = new AircraftRestrictions(graph, aircraftManifest);
  assert.ok(aircraftRules.valid, aircraftRules.diagnostics.join('; '));
  planner.setAircraftRestrictions(aircraftRules); planner.setAircraftProfile(DEFAULT_AIRCRAFT_PROFILE);
  const activeProfileIds = Array.isArray(manifest.activeProfileIds) ? manifest.activeProfileIds.filter((id): id is string => typeof id === 'string') : [];
  planner.setAircraftRestrictionContext({ airportFlags: activeProfileIds });
  function route(originId: string, destinationId: string): Route {
    const a = planner.locations.find(l => l.id === originId), b = planner.locations.find(l => l.id === destinationId);
    assert.ok(a?.nodeId && b?.nodeId, 'The exact selected physical endpoints must exist.');
    const path = planner.findRoute(a.nodeId, b.nodeId, { movement: 'departure', originId, destinationId });
    assert.ok(path, `${configuration} ${originId} -> ${destinationId}: ${planner.lastFlowFailure}`);
    assert.ok(planner.isRouteAllowed(path, 'departure', { originId, destinationId }));
    let distance = 0;
    for (let i = 0; i < path.edgeIds.length; i++) {
      const edge: TaxiEdge = graph.edges.get(path.edgeIds[i])!;
      assert.notEqual(edge.kind, 'runway');
      assert.deepEqual(new Set([edge.from, edge.to]), new Set([path.nodeIds[i], path.nodeIds[i + 1]]));
      distance += edge.length;
    }
    assert.ok(Math.abs(distance - path.distance) < .01, 'The route must remain the real oriented source chain.');
    return path;
  }
  function deflections(path: Route, lastMetres = Infinity) {
    return path.nodeIds.slice(1, -1).flatMap((id, offset) => {
      const i = offset + 1;
      if (path.distance - path.cumulativeDistances[i] > lastMetres) return [];
      const [a, b, c] = [path.nodeIds[i - 1], id, path.nodeIds[i + 1]].map(n => graph.nodes.get(n)!);
      const incoming = { x: b.x - a.x, y: b.y - a.y }, outgoing = { x: c.x - b.x, y: c.y - b.y };
      const angle = Math.atan2(incoming.x * outgoing.y - incoming.y * outgoing.x, incoming.x * outgoing.x + incoming.y * outgoing.y) * 180 / Math.PI;
      return [{ id, i, angle }];
    });
  }
  return { graph, route, deflections };
}

test('B08 to D3 avoids the 270-degree S approach while arriving northbound on the original D3 lead', () => {
  const { graph, route, deflections } = setup('east'), path = route('stand:B08', 'operation:departure:09R:D3');
  const rotation = deflections(path, 700).reduce((sum, turn) => sum + Math.abs(turn.angle), 0);
  assert.ok(rotation <= 180, `A D3 approach from the eastern D frontage must not accumulate the old 270-degree hook (${rotation.toFixed(1)} degrees).`);
  assert.ok(path.edgeIds.includes('osm:222981759:0') && path.edgeIds.includes('osm:222939619:0'), 'Keep the real upper BD5/D3 curved connection; a sharp direct 90-degree cut is not the expected fix.');
  const last = graph.edges.get(path.edgeIds.at(-1)!) as SourceEdge;
  assert.equal(last.sourceWayId, 156840796, 'Keep the real published D3 holding lead.');
  const before = graph.nodes.get(path.nodeIds.at(-2)!)!, end = graph.nodes.get(path.nodeIds.at(-1)!)!;
  assert.ok(end.y > before.y, 'D3 must be reached toward the northern runway, not with a reversed terminal segment.');
});

test('M09 to T3 joins the southbound N frontage instead of hooking north through the P1/F connectors', () => {
  const { graph, route, deflections } = setup('east'), path = route('stand:M09', 'operation:departure:08L:T3');
  const rotation = deflections(path, 700).reduce((sum, turn) => sum + Math.abs(turn.angle), 0);
  // The real N/T3 centerline contains its own bends: its measured 219-degree
  // total is valid. Verify the source-backed connection and cardinal geometry
  // rather than imposing an invented 180-degree steering limit.
  const sourceWays = path.edgeIds.map(id => (graph.edges.get(id) as SourceEdge).sourceWayId);
  const stub = path.edgeIds.indexOf('osm:59465744:0');
  assert.ok(stub >= 0, 'Use the actual short U-to-N connection, rather than a northbound P1 hook.');
  assert.ok(sourceWays.includes(622439753) && sourceWays.includes(222951646), 'Keep both real N-entry and T3-entry curved source links.');
  for (let i = stub + 1; i < path.edgeIds.length; i++) {
    const before = graph.nodes.get(path.nodeIds[i])!, after = graph.nodes.get(path.nodeIds[i + 1])!;
    assert.ok(after.y - before.y <= .3, 'After the U/N entry, continue south without reversing north through P1.');
  }
  assert.ok(rotation < 260, `The correct source connection removes the old 270-degree P1 hook (${rotation.toFixed(1)} degrees).`);
  const nWays = new Set([112642877, 222939637, 222951652]);
  let south = 0;
  for (let i = 0; i < path.edgeIds.length; i++) {
    const edge = graph.edges.get(path.edgeIds[i]) as SourceEdge;
    if (nWays.has(edge.sourceWayId || -1)) south += graph.nodes.get(path.nodeIds[i])!.y - graph.nodes.get(path.nodeIds[i + 1])!.y;
  }
  assert.ok(south > 100, 'GMC01 shows U east joining the N south stream; verify real traversal, not an axis label.');
  assert.equal((graph.edges.get(path.edgeIds.at(-1)!) as SourceEdge).sourceWayId, 222951679);
  assert.ok(graph.nodes.get(path.nodeIds.at(-1)!)!.y < graph.nodes.get(path.nodeIds.at(-2)!)!.y, 'T3 final orientation is south toward 08L.');
});

test('B03 to D5 preserves a real smooth holding approach without an instantaneous reversal', () => {
  const { graph, route, deflections } = setup('east'), path = route('stand:B03', 'operation:departure:09R:D5');
  assert.ok(deflections(path, 700).reduce((sum, turn) => sum + Math.abs(turn.angle), 0) <= 180, 'The source-backed D5/MD2 curve must not be replaced by a gratuitous full hook.');
  const sourceWays = path.edgeIds.map(id => (graph.edges.get(id) as SourceEdge).sourceWayId);
  assert.ok(sourceWays.includes(156840791) && sourceWays.includes(157174725), 'B03 should approach D5 along the real D frontage and retain its curved D5 entry.');
  assert.ok(!sourceWays.includes(156840739), 'For this B03 case the open continuous D approach should not dip along B and return through BD7; BD7 remains a genuine usable connector for other routes.');
  assert.ok(deflections(path).every(turn => Math.abs(turn.angle) <= 110), 'No reverse-heading branch may be hidden by metadata.');
  assert.equal((graph.edges.get(path.edgeIds.at(-1)!) as SourceEdge).sourceWayId, 222939601);
  assert.ok(graph.nodes.get(path.nodeIds.at(-1)!)!.y > graph.nodes.get(path.nodeIds.at(-2)!)!.y, 'D5 final orientation is north toward 09R.');
});

test('the same geometric checks cover other normal departure accesses and source origins', () => {
  for (const configuration of ['east', 'west'] as const) {
    const { route, deflections } = setup(configuration);
    const destinations = configuration === 'east'
      ? ['09R:D3', '09R:D4', '09R:D5', '08L:T3', '08L:T4', '08L:T5', '08L:T6']
      : ['27L:Q4', '27L:Q5', '26R:T9', '26R:T10', '26R:T11', '26R:T12'];
    for (const from of ['F02', 'A39', 'B12', 'J14']) for (const to of destinations) {
      const path = route(`stand:${from}`, `operation:departure:${to}`);
      const reverse = deflections(path).find(turn => Math.abs(turn.angle) > 110);
      assert.equal(reverse, undefined, `${configuration} ${from}/${to}: incompatible source headings at ${reverse?.id} (${reverse?.angle.toFixed(1)} deg).`);
      assert.equal(new Set(path.nodeIds).size, path.nodeIds.length, 'Do not revisit a source junction to satisfy an approach preference.');
    }
  }
});
