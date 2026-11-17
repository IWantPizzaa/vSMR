/** Independent cardinal/portal expectations transcribed from the supplied MANEX and current GMC charts. */
import { strict as assert } from 'node:assert';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import { OperatingRules, type OperatingManifest } from '../src/core/OperatingRules';
import { resolveOperatingManifest } from '../src/core/resolveOperatingManifest';
import { RoutePlanner } from '../src/core/RoutePlanner';
import { TaxiFlowPolicy, type TaxiFlowManifest } from '../src/core/TaxiFlowPolicy';
import { TaxiGraph } from '../src/core/TaxiGraph';
import { AircraftSimulator } from '../src/core/AircraftSimulator';
import { AircraftRestrictions, type AircraftRestrictionManifest } from '../src/core/AircraftRestrictions';
import { DEFAULT_AIRCRAFT_PROFILE } from '../src/core/AircraftProfile';
import type { AirportData, MovementMode, OperatingConfiguration, Route, TaxiEdge } from '../src/core/types';

const root = new URL('../', import.meta.url);
type SourceEdge = TaxiEdge & { sourceWayId?: number };
const sourceWays = {
  N: new Set([112642877, 222939637, 222951652]),
  F: new Set([172796083, 222939600]),
  E: new Set([222948626]),
  TF_IN: new Set([179434323, 222951645, 222982688, 222982691, 177853154, 177853158]),
  TF_OUT: new Set([177853137, 177853145, 222982690, 222982692]),
  P4: new Set([179635683, 231845821, 231845823]),
  P5: new Set([231845825]),
  // Current GMC: horizontal/vertical E and the G circulation immediately west of J.
  E_G_JUNCTION: new Set([222948626, 170450475, 170450469, 222951659, 222951662]),
};

function setup(configuration: OperatingConfiguration, profiles?: string[]) {
  const data = JSON.parse(readFileSync(new URL('public/data/lfpg.json', root), 'utf8')) as AirportData;
  const raw = JSON.parse(readFileSync(new URL('public/data/lfpg-operations.json', root), 'utf8')) as OperatingManifest;
  const manifest = resolveOperatingManifest(raw, profiles ? { enabledProfileIds: profiles } : {});
  const graph = new TaxiGraph(data), rules = new OperatingRules(graph, manifest);
  assert.equal(rules.valid, true, JSON.stringify(rules.diagnostics));
  const corridors = JSON.parse(readFileSync(new URL('public/data/lfpg-corridors.json', root), 'utf8')) as TaxiFlowManifest;
  const policy = new TaxiFlowPolicy(graph, corridors);
  assert.equal(policy.valid, true, policy.diagnostics.join('\n'));
  const planner = new RoutePlanner(graph, rules, configuration); planner.setFlowPolicy(policy);
  const aircraftManifest = JSON.parse(readFileSync(new URL('public/data/lfpg-aircraft-restrictions.json', root), 'utf8')) as AircraftRestrictionManifest;
  const aircraftRules = new AircraftRestrictions(graph, aircraftManifest);
  assert.ok(aircraftRules.valid, aircraftRules.diagnostics.join('; '));
  planner.setAircraftRestrictions(aircraftRules); planner.setAircraftProfile(DEFAULT_AIRCRAFT_PROFILE);
  const activeProfileIds = Array.isArray(manifest.activeProfileIds) ? manifest.activeProfileIds.filter((id): id is string => typeof id === 'string') : [];
  planner.setAircraftRestrictionContext({ airportFlags: activeProfileIds });
  const getRoute = (originId: string, destinationId: string, movement: MovementMode): Route => {
    const origin = planner.locations.find(l => l.id === originId), destination = planner.locations.find(l => l.id === destinationId);
    assert.ok(origin?.nodeId && destination?.nodeId, `Missing source endpoint ${originId} / ${destinationId}`);
    assert.ok(rules.isLocationOpen(originId) && rules.isLocationOpen(destinationId), 'The regression route must use open endpoints.');
    const route = planner.findRoute(origin.nodeId, destination.nodeId, { movement, originId, destinationId });
    assert.ok(route, `${configuration}/${movement} ${originId} -> ${destinationId}: ${planner.lastFlowFailure}`);
    for (let i = 0; i < route.edgeIds.length; i++) {
      const edge: TaxiEdge = graph.edges.get(route.edgeIds[i])!;
      assert.notEqual(edge.kind, 'runway', 'Convergence must never use a runway as a taxi shortcut.');
      assert.ok(planner.allowArc(edge, route.nodeIds[i], route.nodeIds[i + 1], movement), 'A nominal preference may not override a legal restriction.');
    }
    return route;
  };
  function travelled(route: Route, ways: Set<number>) {
    return route.edgeIds.flatMap((id, i) => {
      const edge = graph.edges.get(id) as SourceEdge;
      if (!ways.has(edge.sourceWayId || -1)) return [];
      const from = graph.nodes.get(route.nodeIds[i])!, to = graph.nodes.get(route.nodeIds[i + 1])!;
      return [{ edge, from, to, dx: to.x - from.x, dy: to.y - from.y }];
    });
  }
  return { graph, rules, planner, policy, getRoute, travelled };
}

test('MANEX/GMC cardinal directions apply along the complete main N/F/E portions rather than isolated arrows', () => {
  // GMC01 face EST: N south, F north. GMC02 face OUEST: N north, F south, E east.
  // Oracle is the source chart cardinal direction, not the generated flow manifest.
  for (const configuration of ['east', 'west'] as const) {
    const { graph, planner } = setup(configuration);
    for (const [name, northbound] of [['N', configuration === 'west'], ['F', configuration === 'east']] as const) {
      let checked = 0;
      for (const edge of graph.edges.values() as Iterable<SourceEdge>) {
        if (!sourceWays[name].has(edge.sourceWayId || -1)) continue;
        const a = graph.nodes.get(edge.from)!, b = graph.nodes.get(edge.to)!;
        if (a.x > 900 || b.x > 900 || a.y < -1320 || b.y < -1320 || a.y > 1000 || b.y > 1000 || Math.abs(b.y - a.y) < .3) continue;
        const northFrom = a.y < b.y ? edge.from : edge.to, northTo = a.y < b.y ? edge.to : edge.from;
        for (const movement of ['departure', 'arrival', 'transfer'] as const) {
          assert.equal(planner.allowArc(edge, northFrom, northTo, movement), northbound, `${configuration}/${movement} ${name} ${edge.id} northward`);
          assert.equal(planner.allowArc(edge, northTo, northFrom, movement), !northbound, `${configuration}/${movement} ${name} ${edge.id} southward`);
        }
        checked++;
      }
      assert.ok(checked > 20, `The full ${name} main corridor must be tested, not one sampled arrow.`);
    }
    if (configuration === 'west') {
      let checked = 0;
      for (const edge of graph.edges.values() as Iterable<SourceEdge>) {
        if (!sourceWays.E.has(edge.sourceWayId || -1)) continue;
        const a = graph.nodes.get(edge.from)!, b = graph.nodes.get(edge.to)!;
        if (Math.abs(b.x - a.x) < .3) continue;
        const eastFrom = a.x < b.x ? edge.from : edge.to, eastTo = a.x < b.x ? edge.to : edge.from;
        for (const movement of ['departure', 'arrival', 'transfer'] as const) {
          assert.equal(planner.allowArc(edge, eastFrom, eastTo, movement), true, `GMC02 E must remain eastbound for ${movement}.`);
          assert.equal(planner.allowArc(edge, eastTo, eastFrom, movement), false);
        }
        checked++;
      }
      assert.ok(checked > 30, 'The complete horizontal E frontage must be covered.');
    }
  }
});

test('actual cross-airport routes use the source-backed main corridor in the correct cardinal direction', () => {
  const cases: { configuration: OperatingConfiguration; movement: MovementMode; from: string; to: string; axis: 'N' | 'F'; sign: 1 | -1 }[] = [
    { configuration: 'west', movement: 'departure', from: 'stand:B24', to: 'operation:departure:27L:Q4', axis: 'N', sign: 1 },
    { configuration: 'west', movement: 'arrival', from: 'operation:arrival:27R:K3', to: 'stand:F02', axis: 'F', sign: -1 },
    { configuration: 'east', movement: 'departure', from: 'stand:F02', to: 'operation:departure:09R:D3', axis: 'F', sign: 1 },
    { configuration: 'east', movement: 'arrival', from: 'operation:arrival:09L:K6', to: 'stand:F02', axis: 'N', sign: -1 },
  ];
  for (const item of cases) {
    const { getRoute, travelled } = setup(item.configuration), route = getRoute(item.from, item.to, item.movement);
    const legs = travelled(route, sourceWays[item.axis]);
    assert.ok(legs.reduce((sum, leg) => sum + item.sign * leg.dy, 0) > 350, `${item.configuration}/${item.movement} should converge onto ${item.axis}, not detour through a foreign apron.`);
    for (const leg of legs) assert.ok(item.sign * leg.dy > -.3, `${item.configuration} ${item.axis} must not be followed in the opposite direction.`);
    assert.equal(route.flow?.apronTransitMetres, 0, 'An unrelated apron must not replace the main corridor.');
    assert.equal(route.flow?.mainAxisReentries, 0, 'Do not leave and rejoin the main corridor through intermediate stands.');
  }
});

test('Terminal 2F uses nominal entry TF1/TF2 and exit TF3/TF4 before joining the primary network', () => {
  // The AVISO TF4 label is 4m from this common G4 segment, before the
  // north/south curves and the straight G continuation diverge.
  const tf4Boundary = 'osm:547651208:2';
  const family = new Set([...sourceWays.TF_IN, ...sourceWays.TF_OUT, 547651208]);
  const isPortal = (edge: SourceEdge) => sourceWays.TF_IN.has(edge.sourceWayId!) || sourceWays.TF_OUT.has(edge.sourceWayId!) || edge.id === tf4Boundary;
  for (const configuration of ['east', 'west'] as const) {
    const { getRoute, travelled } = setup(configuration);
    const departure = getRoute('stand:F02', configuration === 'east' ? 'operation:departure:09R:D3' : 'operation:departure:27L:Q4', 'departure');
    const arrival = getRoute(configuration === 'east' ? 'operation:arrival:09L:K6' : 'operation:arrival:27R:K3', 'stand:F02', 'arrival');
    const out = travelled(departure, family).filter(leg => isPortal(leg.edge)), incoming = travelled(arrival, family).filter(leg => isPortal(leg.edge));
    assert.ok(out.length && incoming.length, 'Both movements must actually traverse a mapped apron portal.');
    assert.ok(sourceWays.TF_OUT.has(out[0].edge.sourceWayId!) || out[0].edge.id === tf4Boundary, 'A departure must not exit via TF1/TF2 then loop around just to satisfy a later TF3/TF4 stage.');
    for (const leg of out.filter(leg => leg.edge.id === tf4Boundary)) assert.equal(leg.from.id, leg.edge.to, 'The shared TF4 source segment is stored westbound; a departure crosses it eastbound before choosing a branch.');
    assert.ok(sourceWays.TF_IN.has(incoming.at(-1)!.edge.sourceWayId!), 'The final ingress to 2F must be TF1/TF2, rather than a departure exit.');
    assert.equal(departure.flow?.apronTransitMetres, 0);
    assert.equal(arrival.flow?.apronTransitMetres, 0);
  }
});

test('J apron uses P5 ingress and P4 departure egress before converging toward E or G', () => {
  for (const configuration of ['east', 'west'] as const) {
    const { getRoute, travelled } = setup(configuration);
    const departure = getRoute('stand:J14', configuration === 'east' ? 'operation:departure:09R:D3' : 'operation:departure:27L:Q4', 'departure');
    const arrival = getRoute(configuration === 'east' ? 'operation:arrival:09L:K6' : 'operation:arrival:27R:K3', 'stand:J14', 'arrival');
    assert.ok(travelled(departure, sourceWays.P4).length, 'J departure must pass its published P4 egress.');
    assert.ok(travelled(arrival, sourceWays.P5).length, 'J arrival must enter via P5; J14 is explicitly reachable from it without P4.');
    const egressIndex = departure.edgeIds.findIndex(id => travelled(departure, sourceWays.P4).some(leg => leg.edge.id === id));
    const mainConnection = travelled(departure, sourceWays.E_G_JUNCTION);
    assert.ok(mainConnection.some(leg => departure.edgeIds.indexOf(leg.edge.id) > egressIndex), 'After P4, J departure must physically converge onto the published E/G connection; the G connector need not be labelled a main axis in metadata.');
    assert.equal(departure.flow?.apronTransitMetres, 0);
    assert.equal(arrival.flow?.apronTransitMetres, 0);
  }
});

test('main-corridor convergence preserves valid routes and reopened stands without works restrictions', () => {
  for (const configuration of ['east', 'west'] as const) for (const profiles of [undefined, []]) {
    const { graph, rules, planner, getRoute } = setup(configuration, profiles);
    const destination = configuration === 'east' ? 'operation:departure:09R:D3' : 'operation:departure:27L:Q4';
    for (const stand of ['stand:I24', 'stand:F02', 'stand:J14']) {
      const route = getRoute(stand, destination, 'departure');
      assert.ok(route.flow?.axes.length, 'A long departure should join a primary axis.');
      assert.equal(route.flow?.apronTransitMetres, 0, `${configuration} ${stand}: do not converge through an unrelated apron.`);
    }
    const simulator = new AircraftSimulator(graph, planner);
    assert.equal(rules.isLocationOpen('stand:E09'), true, 'The removed work profile must not leave a permanent stand closure.');
    assert.ok(simulator.addAircraft('stand:E09', destination, { movement: 'departure' }), 'The reopened E09 stand can generate a route that still respects the main-corridor and aircraft rules.');
  }
});

test('FedEx crosses its physical eastern apron boundary once, via B outbound and M inbound in both configurations', () => {
  // Source MANEX vACC page12. This geographic cut crosses the B and M access
  // segments just east of the FedEx stand complex, independently of policy zones.
  const boundaryX = -2400;
  for (const configuration of ['east', 'west'] as const) {
    const { graph, getRoute } = setup(configuration);
    const departure = getRoute('stand:I24', configuration === 'east' ? 'operation:departure:09R:D3' : 'operation:departure:27L:Q4', 'departure');
    const arrival = getRoute(configuration === 'east' ? 'operation:arrival:09L:K6' : 'operation:arrival:27R:K3', 'stand:I24', 'arrival');
    for (const [route, outbound] of [[departure, true], [arrival, false]] as const) {
      const crossings = route.edgeIds.flatMap((id, index) => {
        const from = graph.nodes.get(route.nodeIds[index])!, to = graph.nodes.get(route.nodeIds[index + 1])!;
        return (from.x < boundaryX) !== (to.x < boundaryX) ? [{ edge: graph.edges.get(id) as SourceEdge, dx: to.x - from.x }] : [];
      });
      assert.equal(crossings.length, 1, 'The route must use its actual apron portal without an exit/reentry loop.');
      assert.equal(crossings[0].edge.sourceWayId, outbound ? 156840795 : 222939611, `${configuration}: FedEx ${outbound ? 'departure must use B' : 'arrival must use M'} at the boundary.`);
      assert.equal(crossings[0].dx > 0, outbound, 'Portal direction must match the movement.');
      assert.equal(route.flow?.apronTransitMetres, 0);
    }
  }
});

test('Terminal 1 nominal outer A follows the source-confirmed counterclockwise ring without extending the rule to its feeder', () => {
  // Current GMC diagrams and the supplied MANEX agree on this reference flow.
  // Positive signed radial travel around T1 is independently counterclockwise.
  const outerWays = new Set([112636526, 112642878, 157180716]);
  const center = { x: -445, y: 538 };
  for (const configuration of ['east', 'west'] as const) {
    const { graph, planner, policy } = setup(configuration);
    let checked = 0, feeder = 0;
    for (const edge of graph.edges.values() as Iterable<SourceEdge>) {
      if (edge.sourceWayId === 222939615) {
        assert.equal(policy.edgeDirection(edge.id, configuration, 'departure'), null, 'The A feeder is not part of the reference ring and must not inherit its radial restriction.');
        feeder++; continue;
      }
      if (!outerWays.has(edge.sourceWayId || -1)) continue;
      const a = graph.nodes.get(edge.from)!, b = graph.nodes.get(edge.to)!;
      const radialTravel = (a.x - center.x) * (b.y - a.y) - (a.y - center.y) * (b.x - a.x);
      assert.ok(radialTravel > 0, 'The selected source arc must physically describe a counterclockwise movement.');
      for (const movement of ['departure', 'arrival', 'transfer'] as const) {
        assert.equal(planner.allowArc(edge, edge.from, edge.to, movement), true);
        assert.equal(planner.allowArc(edge, edge.to, edge.from, movement), false);
      }
      checked++;
    }
    assert.equal(checked, 148, 'All source ring arcs must be checked, including the ends near the open feeder.');
    assert.ok(feeder > 0, 'The excluded feeder must actually be present in the source graph.');
  }
});

test('Terminal 1 nominal A3 preserves the clockwise source C-chain, including its local stand-access hooks', () => {
  for (const configuration of ['east', 'west'] as const) {
    const { graph, planner } = setup(configuration);
    const chain = [...graph.edges.values()].filter(e => (e as SourceEdge).sourceWayId === 172859840);
    assert.equal(chain.length, 110);
    const fromIds = new Set(chain.map(e => e.from)), toIds = new Set(chain.map(e => e.to));
    const starts = [...fromIds].filter(id => !toIds.has(id)), ends = [...toIds].filter(id => !fromIds.has(id));
    assert.equal(starts.length, 1); assert.equal(ends.length, 1, 'A3 is an open C-shaped chain, not an invented closed circle.');
    const outgoing = new Map(chain.map(e => [e.from, e]));
    const visited = new Set<string>(); let cursor = starts[0];
    while (outgoing.has(cursor)) { const edge = outgoing.get(cursor)!; assert.ok(!visited.has(edge.id)); visited.add(edge.id); cursor = edge.to; }
    assert.equal(visited.size, chain.length); assert.equal(cursor, ends[0]);
    let northOnWestSide = 0, southOnEastSide = 0, eastAtTop = 0;
    for (const edge of chain) {
      const a = graph.nodes.get(edge.from)!, b = graph.nodes.get(edge.to)!;
      if ([a, b].every(n => n.x < -550 && n.y > 450)) { assert.ok(b.y > a.y); northOnWestSide++; }
      if ([a, b].every(n => n.x > -330 && n.y > 450)) { assert.ok(b.y < a.y); southOnEastSide++; }
      if ([a, b].every(n => n.y > 640)) { assert.ok(b.x > a.x); eastAtTop++; }
      for (const movement of ['departure', 'arrival', 'transfer'] as const) {
        assert.equal(planner.allowArc(edge, edge.from, edge.to, movement), true, 'Stored chain direction agrees with the visually confirmed clockwise reference flow.');
        assert.equal(planner.allowArc(edge, edge.to, edge.from, movement), false);
      }
    }
    assert.ok(northOnWestSide > 10 && southOnEastSide > 5 && eastAtTop > 5, 'Independent geographic samples must establish the actual clockwise orientation.');
  }
});
