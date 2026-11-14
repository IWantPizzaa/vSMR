import { strict as assert } from 'node:assert';
import { execFileSync } from 'node:child_process';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';

test('offline EuroScope replay parser handles receipt clocks, PBH and telemetry-only extraction', () => {
  const root = fileURLToPath(new URL('../', import.meta.url));
  const output = execFileSync('python', ['tests/test_euroscope_replay.py'], { cwd: root, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
  assert.equal(output, '');
});
