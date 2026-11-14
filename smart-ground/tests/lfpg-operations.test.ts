import { strict as assert } from 'node:assert';
import { createHash } from 'node:crypto';
import { readFileSync, existsSync } from 'node:fs';
import { performance } from 'node:perf_hooks';
import { test } from 'node:test';
import { AircraftSimulator, OperatingRules, RoutePlanner, TaxiGraph } from '../src/core';
import type { AirportData } from '../src/core/types';
import type { OperatingManifest } from '../src/core/OperatingRules';
import { resolveOperatingManifest } from '../src/core/resolveOperatingManifest';

const root = new URL('../', import.meta.url);
function load() {
  const bytes = readFileSync(new URL('public/data/lfpg.json', root));
  const manifest = resolveOperatingManifest(JSON.parse(readFileSync(new URL('public/data/lfpg-operations.json', root), 'utf8')) as OperatingManifest);
  const data = JSON.parse(bytes.toString('utf8')) as AirportData, graph = new TaxiGraph(data), rules = new OperatingRules(graph, manifest);
  return { bytes, data, graph, rules, manifest };
}

test('LFPG operational overlay is source-backed and references the exact shipped graph', () => {
  const { bytes, graph, rules, manifest } = load();
  assert.equal(rules.valid, true, JSON.stringify(rules.diagnostics));
  assert.equal(manifest.dataSha256, createHash('sha256').update(bytes).digest('hex'));
  assert.ok(manifest.edgeRules.length > 100, 'Published local flow mapping must be present.');
  assert.ok(manifest.sources.some(s => s.url.includes('sia.aviation-civile.gouv.fr')));
  for (const source of manifest.sources) {
    if (typeof source.file === 'string' && source.sha256 && existsSync(new URL(source.file, root))) assert.equal(createHash('sha256').update(readFileSync(new URL(source.file, root))).digest('hex'), source.sha256, source.id);
  }
  for (const rule of manifest.edgeRules) assert.ok(rule.edgeIds.every(id => graph.edges.has(id)), rule.id);
  assert.equal(manifest.forbidUnmarkedSyntheticTurns, true);
  assert.equal(manifest.forbidUTurns, true);
  assert.equal(manifest.restrictParkingToEndpoints, true);
});

test('LFPG closures and departure-only runway access rules never fall back to free routing', () => {
  const { graph, rules, manifest } = load();
  for (const rule of manifest.edgeRules.filter(r => r.direction === 'closed')) for (const id of rule.edgeIds) {
    const edge = graph.edges.get(id)!;
    for (const configuration of ['east', 'west'] as const) for (const movement of ['departure', 'arrival', 'transfer'] as const) {
      if (rule.configuration !== 'both' && rule.configuration !== configuration) continue;
      if (rule.movement && rule.movement !== 'both' && rule.movement !== movement) continue;
      assert.equal(rules.allowArc(edge, edge.from, edge.to, configuration, movement), false, rule.id);
      assert.equal(rules.allowArc(edge, edge.to, edge.from, configuration, movement), false, rule.id);
    }
  }
  for (const configuration of ['east', 'west'] as const) {
    for (const role of ['departure', 'arrival'] as const) {
      const accesses = rules.runwayAccess(configuration, role); assert.ok(accesses.length > 0);
      for (const access of accesses) {
        const location = rules.locations.find(l => l.id === access.locationId)!;
        assert.ok(location?.nodeId && graph.nodes.has(location.nodeId));
        assert.ok(manifest.runwayUse![configuration][role].includes(access.runway)); assert.notEqual(access.pprRequired, true);
        assert.ok(!location.id.startsWith('runway:'), 'Original runway-center destinations are not operational hold points or exits.');
      }
    }
    const sim = new AircraftSimulator(graph, new RoutePlanner(graph, rules, configuration));
    const stand = graph.locations.find(l => l.kind === 'stand' && l.nodeId)!;
    assert.equal(sim.addAircraft(stand.id, 'runway:26R', { movement: 'departure' }), null);
  }
});

test('LFPG works closures are removed while permanent operating rules remain active', () => {
  const raw = JSON.parse(readFileSync(new URL('public/data/lfpg-operations.json', root), 'utf8'));
  const defaults = resolveOperatingManifest(raw), baseline = resolveOperatingManifest(raw, { enabledProfileIds: [] });
  assert.deepEqual(raw.temporaryProfiles, []);
  assert.deepEqual(defaults.activeProfileIds, []); assert.deepEqual(defaults, baseline);
  for (const id of ['sup188-2026', 'sup151-2026-phase2']) assert.throws(() => resolveOperatingManifest(raw, { enabledProfileIds: [id] }), /Unknown operating works profile/);
  const { graph, rules } = load(); assert.ok(rules.valid, JSON.stringify(rules.diagnostics));
  for (const id of ['stand:E09', 'stand:E44', 'stand:X03', 'stand:W06']) assert.equal(rules.isLocationOpen(id), true, id);
  // Exact source segments formerly closed by SUP188 P3/TE3 and SUP151 B/A.
  // Ordinary direction, aircraft and turn constraints remain separate.
  for (const id of ['osm:179460847:8', 'osm:179458113:0', 'osm:156840795:48', 'osm:157180716:0']) {
    const edge = graph.edges.get(id)!;
    for (const configuration of ['east', 'west'] as const) for (const movement of ['departure', 'arrival', 'transfer'] as const) {
      assert.equal(rules.allowArc(edge, edge.from, edge.to, configuration, movement, 'hard'), true, id);
      assert.equal(rules.allowArc(edge, edge.to, edge.from, configuration, movement, 'hard'), true, id);
    }
  }
  assert.equal(raw.edgeRules.length, 2298);
  assert.equal(raw.clearanceRules.length, 60); assert.equal(raw.priorityRules.length, 15);
  assert.equal(raw.forbidUTurns, true); assert.equal(raw.forbidUnmarkedSyntheticTurns, true);
});

test('LFPG departures and arrivals follow the selected operational overlay through 100-aircraft motion', () => {
  const { graph, rules } = load();
  for (const configuration of ['east', 'west'] as const) for (const movement of ['departure', 'arrival', 'transfer'] as const) {
    const planner = new RoutePlanner(graph, rules, configuration), sim = new AircraftSimulator(graph, planner), before = performance.now();
    const count = sim.generate(100, 20261112, { movement });
    assert.ok(count > 0, `${configuration}/${movement} must have at least one sourced route.`);
    assert.ok(performance.now() - before < 30000, '100-aircraft generation must remain bounded.');
    const accessIds = new Set(rules.runwayAccess(configuration, movement === 'arrival' ? 'arrival' : 'departure').map(a => a.locationId));
    for (const a of sim.aircraft) {
      assert.ok(planner.isRouteAllowed(a.route, movement)); assert.equal(a.route.operational?.configuration, configuration); assert.equal(a.route.operational?.movement, movement);
      if (movement === 'departure') assert.ok(accessIds.has(a.destinationId));
      if (movement === 'arrival') assert.ok(accessIds.has(a.originId));
      assert.ok(rules.isLocationOpen(a.originId)); assert.ok(rules.isLocationOpen(a.destinationId));
    }
    for (let frame = 0; frame < 120; frame++) {
      sim.update(.25);
      for (const a of sim.aircraft) {
        const p = graph.nodes.get(a.fromNodeId)!, q = graph.nodes.get(a.toNodeId)!;
        assert.ok(Math.abs(a.x - p.x - (q.x - p.x) * a.edgeProgress) < 1e-6); assert.ok(Math.abs(a.y - p.y - (q.y - p.y) * a.edgeProgress) < 1e-6);
        if (a.waitingClearanceId) assert.equal(a.state, 'holding');
      }
    }
    console.log(`${configuration}/${movement}: ${count}/100 aircraft generated on sourced constrained routes.`);
  }
});
