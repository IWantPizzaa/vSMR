/** AIP diagram/AD2 oracles transcribed independently of the generated rules. */
import { strict as assert } from 'node:assert';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import { TaxiGraph } from '../src/core/TaxiGraph';
import { AircraftRestrictions, type AircraftRestrictionManifest } from '../src/core/AircraftRestrictions';
import type { AircraftProfile, AirportData } from '../src/core/types';

const root = new URL('../', import.meta.url);
const airport = JSON.parse(readFileSync(new URL('public/data/lfpg.json', root), 'utf8')) as AirportData;
const manifest = JSON.parse(readFileSync(new URL('public/data/lfpg-aircraft-restrictions.json', root), 'utf8')) as AircraftRestrictionManifest;
const graph = new TaxiGraph(airport), rules = new AircraftRestrictions(graph, manifest);
const a320: AircraftProfile = { typeCode: 'A320', wingspanMetres: 35.8 };
const b744: AircraftProfile = { typeCode: 'B744', wingspanMetres: 64.92 };
const b748: AircraftProfile = { typeCode: 'B748', wingspanMetres: 68.4 };
const a388: AircraftProfile = { typeCode: 'A388', wingspanMetres: 79.75 };

test('the illustrated dangerous turns are prohibited for small and large aircraft in both configurations', () => {
  assert.ok(rules.valid, rules.diagnostics.join('; '));
  // Exact source junctions shown on the AIP AD2 dangerous-turn image. The
  // expected result does not read conditions or labels from the rule manifest.
  const turns = [
    ['osm:1690796501', 'osm:222939612:0', 'osm:156840791:43'], // MD2 -> D west
    ['osm:1690796501', 'osm:222939612:0', 'osm:156840791:44'], // MD2 -> D east
    ['intersection:27', 'osm:156840762:9:part0', 'osm:156840795:38:part1'], // MD1 / B
    ['intersection:29', 'osm:222939612:3:part0', 'osm:156840795:38:part0'], // MD2 / B
    ['osm:1690901700', 'osm:222939599:0:part1', 'osm:156849409:0'], // south MD2 / M
    ['osm:1690902025', 'osm:156840762:4', 'osm:156849441:5'], // MD1 / BM6
    ['osm:1690795982', 'osm:156840739:7', 'osm:222939638:2'], // BD7 / BD8
    ['osm:1712613144', 'osm:222934041:0', 'osm:172796083:14'], // FN2 / F
    ['osm:43210348', 'osm:59465741:6', 'osm:172796083:16'], // FN3 / F
  ] as const;
  for (const profile of [a320, b744, b748, a388]) for (const configuration of ['east', 'west'] as const)
    for (const movement of ['departure', 'arrival', 'transfer'] as const) for (const [via, from, to] of turns) {
      assert.equal(rules.allowTurn(from, via, to, configuration, movement, profile), false, `${from}/${to} must not be authorized by a smaller wingspan.`);
      assert.equal(rules.allowTurn(to, via, from, configuration, movement, profile), false, 'The diagram double arrow prohibits the reciprocal turn.');
    }
});

test('prohibited turns do not close the source axes, their straight continuations or other marked connectors', () => {
  for (const profile of [a320, a388]) {
    assert.equal(rules.allowTurn('osm:222939612:0', 'osm:1690796501', 'osm:222939601:9', 'east', 'departure', profile), true, 'MD2/D5 straight continuation is distinct from the prohibited MD2/D turn.');
    assert.equal(rules.allowTurn('osm:156840762:4', 'osm:1690902025', 'osm:156840762:5', 'east', 'departure', profile), true, 'MD1 continuation must not be closed by the MD1/BM6 diagram.');
    assert.equal(rules.allowTurn('osm:156840739:7', 'osm:1690795982', 'osm:156840739:8', 'east', 'departure', profile), true, 'BD7 continuation remains distinct from BD7/BD8 turn.');
    assert.equal(rules.allowTurn('osm:159161262:65', 'osm:43210348', 'osm:172796083:17:part0', 'west', 'departure', profile), true, 'G3/F uses different source incidence from prohibited FN3/F.');
  }
});

test('A bridge/A3 and A/DA1 limits apply strictly above 36m, with their published directions', () => {
  const at36: AircraftProfile = { typeCode: 'TEST', wingspanMetres: 36 };
  const above36: AircraftProfile = { typeCode: 'TEST', wingspanMetres: 36.001 };
  for (const configuration of ['east', 'west'] as const) {
    assert.equal(rules.allowTurn('osm:112642878:57', 'osm:1279879907', 'osm:172859840:109', configuration, 'transfer', at36), true);
    assert.equal(rules.allowTurn('osm:112642878:57', 'osm:1279879907', 'osm:172859840:109', configuration, 'transfer', above36), false);
    assert.equal(rules.allowTurn('osm:172859840:109', 'osm:1279879907', 'osm:112642878:57', configuration, 'transfer', above36), false);
    assert.equal(rules.allowTurn('osm:112642878:41', 'osm:1279879799', 'osm:172859840:0', configuration, 'transfer', above36), true, 'The separate western A3 access must not be closed by extending the bridge-specific rule.');
    assert.equal(rules.allowTurn('osm:112642878:0', 'osm:1693909137', 'osm:157174702:0', configuration, 'transfer', at36), true);
    assert.equal(rules.allowTurn('osm:112642878:0', 'osm:1693909137', 'osm:157174702:0', configuration, 'transfer', above36), false);
    assert.equal(rules.allowTurn('osm:157174702:0', 'osm:1693909137', 'osm:112642878:0', configuration, 'transfer', above36), true, 'A -> DA1 does not establish the reciprocal DA1 -> A prohibition.');
  }
});

test('the Y4 to QD1 prohibition is directional and does not close the legitimate terminal-1 A/B tangent', () => {
  assert.equal(rules.allowTurn('osm:967374627:0', 'osm:8949536314', 'osm:967374610:0', 'east', 'transfer', a320), false);
  assert.equal(rules.allowTurn('osm:967374610:0', 'osm:8949536314', 'osm:967374627:0', 'east', 'transfer', a320), true, 'The source figure has a single arrow; separate general geometric rules remain independent.');
  assert.equal(rules.allowTurn('osm:112636526:56', 'osm:1279879806', 'osm:156840795:54', 'west', 'arrival', a320), true, 'The A/B tangent must remain distinct from its prohibited reversing branch.');
  assert.equal(rules.allowTurn('osm:112636526:56', 'osm:1279879806', 'osm:156840795:55', 'west', 'arrival', a320), false);
});

function allowed(edgeId: string, profile: AircraftProfile, airportFlags: string[] = []) {
  const edge = graph.edges.get(edgeId); assert.ok(edge, `Missing independently selected source edge ${edgeId}`);
  return rules.allowArc(edge, edge.from, edge.to, 'east', 'transfer', profile, { airportFlags });
}

test('temporary SUP188 stand limits are removed without changing the permanent aircraft restrictions', () => {
  // Actual E15 and E34 source parking leads formerly limited by SUP188.
  const leads = ['osm:179460840:0', 'osm:179460840:1', 'osm:179751534:0', 'osm:179751534:1', 'osm:179751534:2'];
  const at36: AircraftProfile = { typeCode: 'TEST', wingspanMetres: 36 };
  for (const edge of leads) for (const profile of [a320, at36, b744, b748, a388]) {
    assert.equal(allowed(edge, profile), true);
    assert.equal(allowed(edge, profile, ['sup188-2026']), true, 'A stale flag cannot reinstate a removed works restriction.');
  }
  assert.equal(manifest.edgeRules.length, 70);
  assert.equal(manifest.turnRules.length, 65);
  assert.ok(!manifest.edgeRules.some(rule => rule.id.startsWith('sup188-')));
});

test('published 52m, 61m and 65m limits keep their inclusive boundary on actual source ways', () => {
  const span = (wingspanMetres: number): AircraftProfile => ({ typeCode: 'TEST', wingspanMetres });
  assert.equal(allowed('osm:180375120:0', span(52)), true, 'EM23 permits the stated maximum 52m.');
  assert.equal(allowed('osm:180375120:0', span(52.001)), false);
  assert.equal(allowed('osm:172859840:40', span(60)), true, 'A3 is not globally a 36m taxiway.');
  assert.equal(allowed('osm:172859840:40', span(61)), true);
  assert.equal(allowed('osm:172859840:40', span(61.001)), false);
  assert.equal(allowed('osm:159161262:20', span(65)), true, 'The yellow G3 centerline is not its <=36m lateral colored axis.');
  assert.equal(allowed('osm:159161262:20', span(65.001)), false);
  assert.equal(allowed('osm:159161262:20', b744), true, 'B747-400 is below 65m and must not inherit B748 restrictions.');
  assert.equal(allowed('osm:159161262:20', b748), false);
  assert.equal(allowed('osm:159161262:20', a388), false);
  assert.equal(allowed('osm:172796083:14', span(80)), true, 'A 65m restriction on colored areas must not close the white main F corridor globally.');
});

test('GMC03 colored-axis mode limits the lateral mode while keeping the yellow centerline available', () => {
  const at36: AircraftProfile = { typeCode: 'TEST', wingspanMetres: 36 };
  const above36: AircraftProfile = { typeCode: 'TEST', wingspanMetres: 36.001 };
  assert.equal(allowed('osm:159161262:20', above36), true);
  assert.equal(allowed('osm:159161262:20', at36, ['colored-dual-axis']), true);
  assert.equal(allowed('osm:159161262:20', above36, ['colored-dual-axis']), false);
});

test('Juliett E12 and the WT common stem are scoped separately from homonymous stands and adjacent curves', () => {
  assert.equal(allowed('osm:152634782:0', b744), false, 'The east Juliett E12 taxiway has the published 36m limit.');
  assert.equal(allowed('osm:1531265018:0', b744), true, 'The unrelated parking lead called E12 is not that taxiway.');
  assert.equal(allowed('osm:229282487:3', b744), false, 'Only the short common WT stem is the shared 36m choke point.');
  assert.equal(allowed('osm:229282487:7', b744), true, 'The T-side approach curve must not be wholesale closed as WT.');
  assert.equal(allowed('osm:5655424:23', b744), true, 'The adjacent principal T remains usable.');
});

test('the A380-only cargo green area stops before UT1 and remains available to B748', () => {
  assert.equal(allowed('osm:179615753:4', a388), false, 'GMC07/08 show the UC5 transverse in green.');
  assert.equal(allowed('osm:179615753:4', b748), true, 'Green is A380-only, not every aircraft above 65m.');
  assert.equal(allowed('osm:160095778:0', a388), true, 'UT1 lies beyond the reviewed green U/C boundary.');
});

test('orange S sections preserve the white S3 and S6 frontages and white V2/V3 main exits', () => {
  assert.equal(allowed('osm:222951672:0', b748), false, 'East of S3 toward V4 is orange on GMC07/08.');
  assert.equal(allowed('osm:515164111:1', b748), false);
  assert.equal(allowed('osm:515164108:4', b748), false, 'V5 toward S6 is orange.');
  assert.equal(allowed('osm:515164109:0', b748), false);
  assert.equal(allowed('osm:222951672:3', b748), true, 'West of S3 is white and must remain usable.');
  assert.equal(allowed('osm:515164109:2', b748), true, 'East of S6 is white.');
  assert.equal(allowed('osm:158959143:4', b748), true, 'The long V3 exit diagonal is white.');
  assert.equal(allowed('osm:158959145:6', b748), false, 'The separate V3 runway-end spur is orange.');
});
