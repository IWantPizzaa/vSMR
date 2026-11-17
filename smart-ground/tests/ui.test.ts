/** DOM-emulation smoke tests of the production bundle, NOT visual/browser QA. */
import { strict as assert } from 'node:assert';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import { JSDOM, VirtualConsole } from 'jsdom';

async function createHarness() {
  const root = new URL('../', import.meta.url);
  const html = readFileSync(new URL('dist/index.html', root), 'utf8');
  const script = html.match(/<script[^>]*src="([^"]+\.js)"/);
  assert.ok(script, 'Build the application before running UI tests (npm test runs pretest).');
  const bundle = readFileSync(new URL(`dist/${script[1].replace(/^\//, '')}`, root), 'utf8');
  const data = readFileSync(new URL('public/data/lfpg.json', root), 'utf8');
  const operations = readFileSync(new URL('public/data/lfpg-operations.json', root), 'utf8');
  const corridors = readFileSync(new URL('public/data/lfpg-corridors.json', root), 'utf8');
  const aircraftRestrictions = readFileSync(new URL('public/data/lfpg-aircraft-restrictions.json', root), 'utf8');
  const errors: string[] = [], networkRequests: string[] = [];
  const console = new VirtualConsole();
  console.on('jsdomError', error => errors.push(String(error)));
  console.on('error', (...args) => errors.push(args.map(String).join(' ')));
  const dom = new JSDOM(html, { url: 'http://smart-ground.test/', runScripts: 'dangerously', virtualConsole: console });
  const { window } = dom;
  const document = window.document;
  let frameTime = 1000, wallTime = Date.parse('2026-11-12T18:00:00.000Z'), nextFrame = 1;
  const frames = new Map<number, FrameRequestCallback>(), intervals = new Map<number, () => void>();
  const canvasCalls: Record<string, number> = {};
  const canvasStrokeColours: string[] = [];
  const contexts = new WeakMap<object, object>();
  Object.defineProperty(window.performance, 'now', { value: () => frameTime });
  window.Date.now = () => wallTime;
  window.requestAnimationFrame = callback => { const id = nextFrame++; frames.set(id, callback); return id; };
  window.cancelAnimationFrame = id => { frames.delete(id); };
  window.setInterval = ((callback: () => void) => { const id = nextFrame++; intervals.set(id, callback); return id; }) as typeof window.setInterval;
  window.clearInterval = id => { if (id !== undefined) intervals.delete(id); };
  Object.defineProperty(window, 'ResizeObserver', { value: class { observe() {} unobserve() {} disconnect() {} } });
  Object.defineProperty(window, 'TextEncoder', { value: TextEncoder });
  Object.defineProperty(window, 'fetch', { value: async (url: string) => {
    networkRequests.push(String(url));
    assert.ok(/\/data\/lfpg(?:-operations|-corridors|-aircraft-restrictions)?\.json$/.test(String(url)), `Unexpected external request: ${url}`);
    return { ok: true, status: 200, json: async () => JSON.parse(String(url).includes('lfpg-aircraft-restrictions') ? aircraftRestrictions : String(url).includes('lfpg-operations') ? operations : String(url).includes('lfpg-corridors') ? corridors : data) };
  } });
  window.HTMLCanvasElement.prototype.getBoundingClientRect = () => ({ x: 0, y: 0, left: 0, top: 0, width: 1000, height: 700, right: 1000, bottom: 700, toJSON() { return {}; } });
  Object.defineProperty(window.HTMLCanvasElement.prototype, 'getContext', { value: function () {
    if (!contexts.has(this)) {
      const values: Record<PropertyKey, unknown> = { canvas: this, measureText: (text: string) => ({ width: text.length * 6 }) };
      contexts.set(this, new Proxy(values, {
        get(target, key) {
          if (key in target) return target[key];
          return (...args: unknown[]) => {
            assert.ok(args.every(value => typeof value !== 'number' || Number.isFinite(value)), `Canvas ${String(key)} received a non-finite coordinate`);
            canvasCalls[String(key)] = (canvasCalls[String(key)] ?? 0) + 1;
            if (key === 'stroke' && typeof target.strokeStyle === 'string') canvasStrokeColours.push(target.strokeStyle);
          };
        },
        set(target, key, value) { target[key] = value; return true; },
      }));
    }
    return contexts.get(this);
  } });
  window.HTMLDialogElement.prototype.showModal = function () { this.open = true; };
  window.HTMLDialogElement.prototype.close = function () { this.open = false; };
  window.HTMLCanvasElement.prototype.setPointerCapture = () => {};
  window.HTMLCanvasElement.prototype.releasePointerCapture = () => {};
  window.eval(`${bundle}\n//# sourceURL=smart-ground-production-bundle.js`);
  const settle = async () => { await new Promise<void>(resolve => setImmediate(resolve)); };
  await settle();
  assert.ok(document.getElementById('route-form'), `Production bundle did not initialise: ${errors.join('; ')}`);
  const get = <T extends HTMLElement = HTMLElement>(id: string): T => {
    const element = document.getElementById(id); assert.ok(element, `Missing rendered element #${id}`); return element as T;
  };
  const click = (id: string) => get<HTMLButtonElement>(id).click();
  const select = (id: string, value: string) => { const input = get<HTMLSelectElement>(id); input.value = value; input.dispatchEvent(new window.Event('change', { bubbles: true })); };
  const runwayAccess = (runway: string, id = 'destination') => {
    const option = [...get<HTMLSelectElement>(id).options].find(o => !o.disabled && o.textContent?.startsWith(`${runway} ·`));
    assert.ok(option, `No actual holding/exit point for active runway ${runway}`); return option.value;
  };
  const step = (count = 1, milliseconds = 250) => {
    for (let i = 0; i < count; i++) {
      frameTime += milliseconds; wallTime += milliseconds;
      const pending = [...frames.values()]; frames.clear();
      for (const callback of pending) callback(frameTime);
    }
  };
  const importFile = async (input: unknown, raw = false) => {
    const serialized = raw ? String(input) : JSON.stringify(input);
    const file = { name: 'capture.json', size: Buffer.byteLength(serialized), text: async () => serialized };
    const element = get<HTMLInputElement>('bridge-file');
    Object.defineProperty(element, 'files', { configurable: true, value: [file] });
    element.dispatchEvent(new window.Event('change', { bubbles: true })); await settle();
  };
  const capture = (age = 0, stand = 'A01', runway = '08L') => ({
    format: 'smart-ground.bridge.v1', airport: 'LFPG', capturedAt: new Date(wallTime - age).toISOString(), simulated: true,
    snapshot: { callsigns: ['AFR777'], providers: {
      rampagent: { major: 1, minor: 0, fields: { stand: { AFR777: stand } } },
      vsid: { major: 1, minor: 0, fields: { rwy: { AFR777: runway } } },
    } },
  });
  return { dom, window, document, errors, networkRequests, canvasCalls, canvasStrokeColours, get, click, select, runwayAccess, step, settle, importFile, capture,
    advanceFreshness(milliseconds: number) { wallTime += milliseconds; for (const callback of intervals.values()) callback(); } };
}

test('production UI works with the offline LFPG data in a virtual DOM', async t => {
  const ui = await createHarness(); t.after(() => ui.dom.window.close());
  const { get, click, select, step, document } = ui;
  await t.test('loads source stands and calculates a rendered route before adding an aircraft', () => {
    assert.equal(get('stat-aircraft').textContent, '0');
    assert.equal(get<HTMLSelectElement>('origin').options.length, 523);
    for (const id of ['stand:E09', 'stand:X03', 'stand:W06']) assert.equal([...get<HTMLSelectElement>('origin').options].find(option => option.value === id)?.disabled, false, `${id} must no longer be closed for works.`);
    assert.ok([...get<HTMLSelectElement>('origin').options].some(option => option.disabled && option.textContent?.includes('non raccordé')));
    assert.equal(get<HTMLSelectElement>('configuration').value, 'west');
    assert.ok([...get<HTMLSelectElement>('destination').options].every(o => /^26R ·|^27L ·/.test(o.textContent || '')));
    select('configuration', 'east');
    select('origin', 'stand:A01'); select('destination', ui.runwayAccess('08L'));
    get<HTMLFormElement>('route-form').dispatchEvent(new ui.window.Event('submit', { bubbles: true, cancelable: true }));
    assert.match(get('route-summary').textContent ?? '', /\d+\.\d+ km/);
    assert.match(get('route-summary').textContent ?? '', /Face est/);
    assert.match(get('route-summary').textContent ?? '', /AXES DE RÉFÉRENCE/);
    assert.match(get('route-summary').querySelector('.route-quality')?.textContent ?? '', /^\d+ virages aux jonctions · \d+ changements d’axe principal$/);
    assert.equal(get<HTMLButtonElement>('add-aircraft').disabled, false);
    get<HTMLInputElement>('callsign').value = 'AFR123'; click('add-aircraft');
    assert.equal(get('stat-aircraft').textContent, '1'); assert.match(get('aircraft-detail').textContent ?? '', /AFR123/);
    step(); assert.ok((ui.canvasCalls.lineTo ?? 0) > 8000, 'The map and planned polyline execute Canvas drawing commands.');
  });
  await t.test('play, pause, speed, hold and resume affect the selected aircraft', () => {
    const distance = () => get<HTMLProgressElement>('aircraft-detail').querySelector('progress')!.value;
    const initial = distance(); step(8); assert.equal(distance(), initial, 'Paused traffic must remain still.');
    click('play'); step(8); assert.ok(distance() > initial); assert.match(get('simulation-state').textContent ?? '', /EN COURS/);
    click('hold-aircraft'); const held = distance(); step(8); assert.equal(distance(), held); assert.match(get('aircraft-detail').textContent ?? '', /En attente/);
    click('hold-aircraft'); select('speed', '10'); step(8); assert.ok(distance() > held + 50, 'Speed selector must advance actual motion.');
    click('play'); step(4); const paused = distance(); step(8); assert.equal(distance(), paused); assert.equal(get('simulation-state').textContent, 'EN PAUSE');
    click('remove-aircraft'); assert.equal(get('stat-aircraft').textContent, '0'); assert.equal(get('aircraft-detail').hidden, true);
  });
  await t.test('generation mixes arrivals and departures, schedules later starts and supports selection/removal', () => {
    const preset = document.querySelector<HTMLButtonElement>('[data-count="100"]'); assert.ok(preset); preset.click(); click('generate');
    assert.equal(get('stat-aircraft').textContent, '100'); assert.equal(get('traffic-count').textContent, '100');
    click('aircraft-tab'); assert.equal(document.querySelectorAll('[data-aircraft]').length, 100);
    assert.equal(document.querySelectorAll('[data-aircraft][data-movement="arrival"]').length, 50);
    assert.equal(document.querySelectorAll('[data-aircraft][data-movement="departure"]').length, 50);
    const pending = document.querySelectorAll('[data-aircraft][data-state="scheduled"]');
    assert.ok(pending.length >= 98, 'Future movements stay scheduled instead of entering the surface at once.');
    assert.match(get('traffic-summary').textContent ?? '', /50 arrivées · 50 départs/);
    assert.equal(get('stat-offtrack').textContent, '0', 'Normal assigned routes must not generate deviation alerts.');
    (pending[0] as HTMLButtonElement).click();
    assert.equal(get<HTMLButtonElement>('hold-aircraft').disabled, true, 'A scheduled aircraft cannot be started early with Resume.');
    assert.equal(get<HTMLButtonElement>('deviate-aircraft').disabled, true);
    assert.match(get('aircraft-detail').textContent ?? '', /Programmé|mise en mouvement/);
    assert.equal(get('aircraft-tab').getAttribute('aria-selected'), 'true');
    document.querySelector<HTMLButtonElement>('[data-aircraft]')!.click(); assert.equal(get('aircraft-detail').hidden, false);
    click('remove-aircraft'); assert.equal(get('stat-aircraft').textContent, '99'); assert.equal(document.querySelectorAll('[data-aircraft]').length, 99);
    select('horizon', '300'); click('alerts-tab'); step(4);
    assert.equal(get('stat-offtrack').textContent, '0');
    assert.ok(!document.querySelector('[data-track-aircraft]'), 'Scheduled normal traffic must not create off-route alarms.');
    click('clear'); assert.equal(get('stat-aircraft').textContent, '0'); assert.equal(get('stat-conflicts').textContent, '0'); assert.equal(get('simulation-clock').textContent, '00:00:00');
  });
  await t.test('map layers change rendered network commands and audit exposes real counts', () => {
    click('layers-toggle'); assert.equal(get('layers').hidden, false);
    get<HTMLInputElement>('layer-network').click();
    const beforeOff = ui.canvasCalls.stroke ?? 0; step(); const strokesOff = (ui.canvasCalls.stroke ?? 0) - beforeOff;
    get<HTMLInputElement>('layer-network').click();
    const beforeOn = ui.canvasCalls.stroke ?? 0; step(); const strokesOn = (ui.canvasCalls.stroke ?? 0) - beforeOn;
    assert.ok(strokesOn > strokesOff + 8000, 'Enabling taxiways must restore the source graph drawing.');
    get<HTMLInputElement>('layer-components').click(); step();
    click('zoom-in'); click('zoom-out'); click('fit-map'); step();
    click('audit-button'); assert.equal(get<HTMLDialogElement>('info-dialog').open, true);
    assert.match(get('dialog-title').textContent ?? '', /audit du réseau/);
    assert.match(get('dialog-content').textContent ?? '', /523/); assert.match(get('dialog-content').textContent ?? '', /Composantes/);
    click('dialog-close'); assert.equal(get<HTMLDialogElement>('info-dialog').open, false);
  });
  await t.test('bridge import rejects malformed JSON and resolves exact fresh stand/runway assignments', async () => {
    await ui.importFile('{broken', true); assert.match(get('toast').textContent ?? '', /Import refusé/); assert.equal(get('stat-aircraft').textContent, '0');
    await ui.importFile(ui.capture()); assert.equal(get<HTMLDialogElement>('info-dialog').open, true);
    const apply = document.querySelector<HTMLButtonElement>('[data-apply="AFR777"]'); assert.ok(apply); assert.equal(apply.disabled, false);
    apply.click(); assert.equal(get<HTMLSelectElement>('origin').value, 'stand:A01'); assert.equal(get<HTMLSelectElement>('destination').value, ui.runwayAccess('08L'));
    assert.equal(get<HTMLInputElement>('callsign').value, 'AFR777'); assert.match(get('route-summary').textContent ?? '', /\d+\.\d+ km/); assert.equal(get('stat-aircraft').textContent, '0', 'Preparing a plugin assignment must not issue a movement or add traffic.');
  });
  await t.test('stale, expired and unknown external assignments remain unusable', async () => {
    await ui.importFile(ui.capture(60_000)); assert.match(get('bridge-status').textContent ?? '', /PÉRIMÉE/); assert.equal(document.querySelector<HTMLButtonElement>('[data-apply]')!.disabled, true); click('dialog-close');
    await ui.importFile(ui.capture(0, 'NO_SUCH_STAND')); assert.equal(document.querySelector<HTMLButtonElement>('[data-apply]')!.disabled, true); assert.match(get('dialog-content').textContent ?? '', /absent, non raccordé ou indisponible/); click('dialog-close');
    await ui.importFile(ui.capture()); assert.equal(document.querySelector<HTMLButtonElement>('[data-apply]')!.disabled, false);
    ui.advanceFreshness(16_000); assert.equal(document.querySelector<HTMLButtonElement>('[data-apply]')!.disabled, true); assert.match(get('bridge-capture-status').textContent ?? '', /PÉRIMÉE/);
    assert.equal(get('stat-aircraft').textContent, '0'); click('dialog-close');
  });
  await t.test('changing east/west invalidates traffic, exposes only active accesses and refuses an opposite vSID runway', async () => {
    document.querySelector<HTMLButtonElement>('[data-count="10"]')!.click(); click('generate');
    assert.equal(get('stat-aircraft').textContent, '10'); click('play'); step();
    select('configuration', 'west'); assert.equal(get('stat-aircraft').textContent, '0');
    assert.equal(get('simulation-state').textContent, 'EN PAUSE');
    assert.equal(get<HTMLButtonElement>('add-aircraft').disabled, true);
    assert.ok([...get<HTMLSelectElement>('destination').options].every(o => /^26R ·|^27L ·/.test(o.textContent || '')));
    await ui.importFile(ui.capture(0, 'A01', '08L'));
    assert.equal(document.querySelector<HTMLButtonElement>('[data-apply]')!.disabled, true);
    assert.match(get('dialog-content').textContent ?? '', /inactive dans cette configuration/); click('dialog-close');
  });
  await t.test('arrival and transfer controls use real operational origins and preserve manual crossing authority', () => {
    select('movement', 'arrival');
    assert.ok([...get<HTMLSelectElement>('origin').options].every(o => /^26L ·|^27R ·/.test(o.textContent || '')));
    assert.equal(get('origin-label').textContent, 'SORTIE DE PISTE');
    assert.ok(![...get<HTMLSelectElement>('origin').options].some(o => o.value.startsWith('runway:')), 'Legacy runway-centre endpoints must never stand in for real exit holds.');
    select('origin', ui.runwayAccess('27R', 'origin')); select('destination', 'stand:A01');
    get<HTMLFormElement>('route-form').dispatchEvent(new ui.window.Event('submit', { bubbles: true, cancelable: true }));
    assert.match(get('route-summary').textContent ?? '', /\d+\.\d+ km/);
    get<HTMLInputElement>('callsign').value = 'AFR778'; click('add-aircraft');
    assert.ok(get('clear-runway'), 'An arrival at its actual pre-crossing hold must require a manual clearance.');
    assert.equal(get<HTMLButtonElement>('hold-aircraft').disabled, true);
    const progress = () => get('aircraft-detail').querySelector<HTMLProgressElement>('progress')!.value;
    const held = progress(); click('play'); step(8); assert.equal(progress(), held);
    click('clear-runway'); step(8); assert.ok(progress() > held, 'Only the explicit crossing action releases this checkpoint.');
    select('movement', 'transfer'); assert.equal(get('stat-aircraft').textContent, '1', 'Changing a planned movement must preserve traffic with its own per-aircraft ARR/DEP context.');
    assert.ok([...get<HTMLSelectElement>('origin').options, ...get<HTMLSelectElement>('destination').options].every(o => o.value.startsWith('stand:')));
    click('operations-button'); assert.match(get('dialog-content').textContent ?? '', /Sources/);
    assert.equal(document.querySelector('[data-works]'), null);
    assert.doesNotMatch(get('dialog-content').textContent ?? '', /Profils de travaux publiés/);
    assert.ok(document.querySelector('[data-aircraft-flag="colored-dual-axis"]'));
    assert.ok(document.querySelector('[data-aircraft-flag="southwest-deicing-occupied"]'));
    click('dialog-close');
  });
  await t.test('mixed arrivals and departures keep their own flow while PPR changes invalidate traffic', () => {
    select('movement', 'departure'); select('origin', 'stand:A01'); select('destination', ui.runwayAccess('26R'));
    get<HTMLFormElement>('route-form').dispatchEvent(new ui.window.Event('submit', { bubbles: true, cancelable: true }));
    assert.equal(get<HTMLButtonElement>('add-aircraft').disabled, false);
    get<HTMLInputElement>('callsign').value = 'AFR779'; click('add-aircraft');
    assert.equal(get('stat-aircraft').textContent, '2', 'An arrival and departure must coexist in one active runway configuration.');
    click('aircraft-tab');
    const arrival = [...document.querySelectorAll<HTMLButtonElement>('[data-aircraft]')].find(b => b.textContent?.includes('AFR778'));
    assert.ok(arrival); arrival.click();
    assert.equal(get<HTMLSelectElement>('movement').value, 'arrival', 'Selecting an aircraft must display its own directional flow.');
    assert.match(get('map-configuration').textContent ?? '', /ARRIVÉES/);
    select('movement', 'departure');
    assert.equal(get<HTMLSelectElement>('origin').querySelector<HTMLOptionElement>('option[value="stand:E09"]')!.disabled, false, 'The removed SUP188 closure must not return after a configuration or movement change.');
    assert.ok(![...get<HTMLSelectElement>('destination').options].some(o => o.textContent?.includes('PPR')));
    click('operations-button'); get<HTMLInputElement>('include-ppr').click();
    assert.equal(get('stat-aircraft').textContent, '0', 'A policy/permission change must invalidate existing routes.');
    assert.ok([...get<HTMLSelectElement>('destination').options].some(o => o.textContent?.includes('PPR')));
    click('dialog-close');
  });
  await t.test('aircraft profiles invalidate the planned route while existing aircraft retain their own profile', () => {
    select('configuration', 'west'); select('movement', 'departure'); select('aircraft-type', 'A320');
    select('origin', 'stand:A01'); select('destination', ui.runwayAccess('26R'));
    get<HTMLFormElement>('route-form').dispatchEvent(new ui.window.Event('submit', { bubbles: true, cancelable: true }));
    assert.match(get('route-summary').querySelector('.route-aircraft')?.textContent ?? '', /A320/);
    get<HTMLInputElement>('callsign').value = 'AFR880'; click('add-aircraft');
    assert.equal(get('stat-aircraft').textContent, '1');
    select('aircraft-type', 'B748');
    assert.equal(get('stat-aircraft').textContent, '1', 'Planning for a B748 must not remove an existing A320.');
    assert.equal(get<HTMLButtonElement>('add-aircraft').disabled, true, 'A route computed for another profile must be recalculated.');
    assert.match(get('map-configuration').textContent ?? '', /B748/);
    select('aircraft-type', 'B744'); assert.match(get('aircraft-profile-label').textContent ?? '', /64,92/);
    click('aircraft-tab'); document.querySelector<HTMLButtonElement>('[data-aircraft]')!.click();
    assert.equal(get<HTMLSelectElement>('aircraft-type').value, 'A320');
    assert.match(get('aircraft-detail').querySelector('.detail-aircraft')?.textContent ?? '', /A320/);
    assert.match(get('map-configuration').textContent ?? '', /A320/);
  });
  await t.test('custom wingspans reject invalid or ambiguous profiles and do not silently generate unidentified types', () => {
    select('aircraft-type', 'custom'); assert.equal(get('aircraft-custom').hidden, false);
    get<HTMLInputElement>('aircraft-type-code').value = 'A388'; get<HTMLInputElement>('aircraft-wingspan').value = '30';
    get('aircraft-wingspan').dispatchEvent(new ui.window.Event('change', { bubbles: true }));
    assert.equal(get('aircraft-profile-error').hidden, false);
    assert.equal(document.querySelector<HTMLButtonElement>('#route-form [type="submit"]')!.disabled, true);
    assert.equal(get<HTMLButtonElement>('generate').disabled, true);
    assert.equal(get('stat-aircraft').textContent, '1');
    get<HTMLInputElement>('aircraft-type-code').value = 'B747'; get<HTMLInputElement>('aircraft-wingspan').value = '68.4';
    get('aircraft-type-code').dispatchEvent(new ui.window.Event('change', { bubbles: true }));
    assert.equal(get('aircraft-profile-error').hidden, false, 'The generic B747 name must require a variant.');
    get<HTMLInputElement>('aircraft-type-code').value = 'B77W'; get<HTMLInputElement>('aircraft-wingspan').value = '64.8';
    get('aircraft-wingspan').dispatchEvent(new ui.window.Event('change', { bubbles: true }));
    assert.equal(get('aircraft-profile-error').hidden, true); assert.equal(get<HTMLButtonElement>('generate').disabled, false);
    assert.match(get('aircraft-profile-label').textContent ?? '', /B77W/);
    get<HTMLInputElement>('aircraft-type-code').value = 'UNKNOWN';
    get('aircraft-type-code').dispatchEvent(new ui.window.Event('change', { bubbles: true }));
    assert.equal(get<HTMLButtonElement>('generate').disabled, true);
    select('aircraft-type', 'A320');
  });
  await t.test('conditional wingspan scenarios are explicit, update map context and invalidate old traffic', () => {
    click('operations-button');
    assert.match(get('dialog-content').textContent ?? '', /Type, envergure et virages/);
    const southwest = document.querySelector<HTMLInputElement>('[data-aircraft-flag="southwest-deicing-occupied"]'); assert.ok(southwest); assert.equal(southwest.checked, false);
    southwest.click();
    assert.equal(get('stat-aircraft').textContent, '0');
    assert.equal(document.querySelector<HTMLInputElement>('[data-aircraft-flag="southwest-deicing-occupied"]')!.checked, true);
    document.querySelector<HTMLInputElement>('[data-aircraft-flag="colored-dual-axis"]')!.click();
    assert.match(get('aircraft-profile-label').textContent ?? '', /orange\/bleu/);
    assert.match(get('aircraft-profile-label').textContent ?? '', /SW1–SW4/);
    click('dialog-close'); click('layers-toggle'); get<HTMLInputElement>('layer-turns').click(); step();
    get<HTMLInputElement>('layer-turns').click(); step();
  });
  assert.equal(ui.networkRequests.length, 4, 'Only the four offline geometry, operating, corridor and aircraft manifests are fetched.');
  assert.deepEqual(ui.errors, [], 'The production bundle must run without uncaught DOM or Canvas command errors.');
});

test('production UI identifies an off-route aircraft and keeps its assignment and recovery visible', async t => {
  const ui = await createHarness(); t.after(() => ui.dom.window.close());
  const { get, click, select, document, step } = ui;
  select('configuration', 'east'); select('movement', 'departure');
  select('origin', 'stand:B08'); select('destination', 'operation:departure:09R:D3');
  get<HTMLFormElement>('route-form').dispatchEvent(new ui.window.Event('submit', { bubbles: true, cancelable: true }));
  assert.equal(get<HTMLButtonElement>('add-aircraft').disabled, false);
  get<HTMLInputElement>('callsign').value = 'TRK123'; click('add-aircraft');
  assert.equal(get('stat-offtrack').textContent, '0');
  click('deviate-aircraft');
  assert.match(get('aircraft-detail').textContent ?? '', /Mauvais virage armé/);
  assert.equal(get('stat-offtrack').textContent, '0', 'Arming the demo must not teleport the aircraft or raise an early alarm.');
  select('speed', '10'); click('play');
  for (let cycle = 0; cycle < 350 && get('stat-offtrack').textContent === '0'; cycle++) step(4);
  assert.equal(get('stat-offtrack').textContent, '1');
  assert.match(get('toast').textContent ?? '', /TRK123.*hors itinéraire/);
  click('alerts-tab');
  const alert = document.querySelector<HTMLButtonElement>('[data-track-aircraft]'); assert.ok(alert);
  assert.match(alert.textContent ?? '', /TRK123/);
  alert.click();
  assert.match(get('aircraft-detail').textContent ?? '', /Référence :/);
  assert.match(get('aircraft-detail').textContent ?? '', /Historique des écarts/);
  assert.match(get('aircraft-detail').textContent ?? '', /maintien obligatoire/);
  assert.doesNotMatch(get('aircraft-detail').textContent ?? '', /Raccordement calculé\./);
  assert.equal(get<HTMLButtonElement>('hold-aircraft').disabled, true);
  click('play'); step(4);
  assert.ok(document.querySelector('[data-track-aircraft]'), 'The identified deviation must remain visible after pausing.');
  assert.deepEqual(ui.errors, []);
});

test('production surveillance defaults to five-second reports and shows age between independent measurements', async t => {
  const ui = await createHarness(); t.after(() => ui.dom.window.close());
  const { get, click, select, step } = ui;
  assert.equal(get<HTMLSelectElement>('surveillance-mode').value, 'radar');
  select('configuration', 'east'); select('origin', 'stand:B08'); select('destination', 'operation:departure:09R:D3');
  get<HTMLFormElement>('route-form').dispatchEvent(new ui.window.Event('submit', { bubbles: true, cancelable: true }));
  click('add-aircraft');
  assert.match(get('radar-summary').textContent ?? '', /5 s/);
  const reportAge = () => Number(get('aircraft-detail').querySelector('.detail-surveillance')!.textContent!.match(/il y a ([\d.]+) s/)![1]);
  assert.equal(reportAge(), 0);
  click('play'); step(16);
  assert.ok(reportAge() >= 3 && reportAge() < 5, 'Render frames must not refresh the original radar report timestamp.');
  step(12); assert.ok(reportAge() < 2.5, 'A new report arrives after the five-second source interval.');
  assert.equal(get('stat-offtrack').textContent, '0');
  select('surveillance-mode', 'exact');
  assert.match(get('radar-summary').textContent ?? '', /Référence exacte/);
  assert.match(get('aircraft-detail').querySelector('.detail-surveillance')!.textContent ?? '', /référence exacte/);
  select('surveillance-mode', 'radar');
  assert.match(get('radar-summary').textContent ?? '', /5 s/);
  assert.deepEqual(ui.errors, []);
});

test('production UI proposes a legal recovery for M09 to T3 and resumes only after review', async t => {
  const ui = await createHarness(); t.after(() => ui.dom.window.close());
  const { get, click, select, document, step } = ui;
  select('configuration', 'east'); select('origin', 'stand:M09'); select('destination', 'operation:departure:08L:T3');
  get<HTMLFormElement>('route-form').dispatchEvent(new ui.window.Event('submit', { bubbles: true, cancelable: true }));
  assert.equal(get<HTMLButtonElement>('add-aircraft').disabled, false);
  get<HTMLInputElement>('callsign').value = 'TRK456'; click('add-aircraft'); click('deviate-aircraft');
  assert.match(get('aircraft-detail').textContent ?? '', /Mauvais virage armé/);
  select('speed', '10'); click('play');
  for (let cycle = 0; cycle < 350 && get('stat-offtrack').textContent === '0'; cycle++) step(4);
  assert.equal(get('stat-offtrack').textContent, '1');
  assert.match(get('hold-aircraft').textContent ?? '', /Suivre la récupération/);
  assert.equal(get<HTMLButtonElement>('hold-aircraft').disabled, false);
  const held = get<HTMLProgressElement>('aircraft-detail').querySelector('progress')!.value;
  step(12); assert.equal(get<HTMLProgressElement>('aircraft-detail').querySelector('progress')!.value, held);
  click('hold-aircraft');
  for (let cycle = 0; cycle < 800 && get('stat-offtrack').textContent !== '0'; cycle++) step(4);
  assert.equal(get('stat-offtrack').textContent, '0', 'The aircraft must physically rejoin the directed assigned route after approval.');
  assert.match(get('aircraft-detail').textContent ?? '', /Sur l’itinéraire attribué/);
  click('alerts-tab');
  assert.match(document.querySelector('[data-track-aircraft]')?.textContent ?? '', /Itinéraire rejoint.*TRK456/);
  assert.deepEqual(ui.errors, []);
});

test('dragging a real LFPG route junction assigns a blue controller route without moving the aircraft', async t => {
  const ui = await createHarness(); t.after(() => ui.dom.window.close());
  const { get, click, select, document, step } = ui;
  select('configuration', 'east'); select('origin', 'stand:M09'); select('destination', 'operation:departure:08L:T3');
  get<HTMLFormElement>('route-form').dispatchEvent(new ui.window.Event('submit', { bubbles: true, cancelable: true }));
  get<HTMLInputElement>('callsign').value = 'EDIT123'; click('add-aircraft');
  assert.match(get('aircraft-detail').querySelector('.detail-assignment')!.textContent ?? '', /Parcours standard calculé/);
  click('aircraft-tab'); document.querySelector<HTMLButtonElement>('[data-aircraft]')!.click();
  const airport = JSON.parse(readFileSync(new URL('../public/data/lfpg.json', import.meta.url), 'utf8'));
  const origin = airport.locations.find((p: { id: string }) => p.id === 'stand:M09');
  const node = airport.nodes.find((p: { id: string }) => p.id === origin.nodeId);
  const anchor = airport.nodes.find((p: { id: string }) => p.id === 'osm:2318941305');
  const target = airport.nodes.find((p: { id: string }) => p.id === 'osm:8968901466');
  const canvas = get<HTMLCanvasElement>('airport-map');
  const pointer = (type: string, x: number, y: number) => canvas.dispatchEvent(new ui.window.MouseEvent(type, { clientX: x, clientY: y, button: 0, bubbles: true }));
  // Selecting in the traffic list focuses the origin at scale0.35. Pan the
  // real junction into the centre, then zoom so the27m alternative is clear.
  pointer('pointerdown', 10, 10);
  pointer('pointermove', 10 + (node.x - anchor.x) * .35, 10 + (anchor.y - node.y) * .35);
  pointer('pointerup', 10 + (node.x - anchor.x) * .35, 10 + (anchor.y - node.y) * .35);
  for (let zoom = 0; zoom < 8; zoom++) click('zoom-in'); step();
  const progress = () => get<HTMLProgressElement>('aircraft-detail').querySelector('progress')!.value;
  const before = progress();
  pointer('pointerdown', 500, 350);
  pointer('pointermove', 500 + (target.x - anchor.x) * 4, 350 - (target.y - anchor.y) * 4); step();
  pointer('pointerup', 500 + (target.x - anchor.x) * 4, 350 - (target.y - anchor.y) * 4); ui.canvasStrokeColours.length = 0; step();
  assert.match(get('aircraft-detail').querySelector('.detail-assignment.controller')?.textContent ?? '', /Route attribuée · révision 1/);
  assert.equal(progress(), before, 'Assigning another future junction cannot teleport the aircraft.');
  assert.equal(get('stat-offtrack').textContent, '0');
  assert.ok(ui.canvasStrokeColours.includes('#76c8ff'), 'The committed controller route is drawn blue after the drag ghost is gone.');
  assert.match(get('toast').textContent ?? '', /EDIT123.*route attribuée modifiée/);
  assert.ok(document.querySelector('#undo-route-edit'));
  click('undo-route-edit'); ui.canvasStrokeColours.length = 0; step();
  assert.match(get('aircraft-detail').querySelector('.detail-assignment.standard')?.textContent ?? '', /Parcours standard calculé/);
  assert.equal(progress(), before); assert.equal(get('stat-offtrack').textContent, '0');
  assert.ok(ui.canvasStrokeColours.includes('#c4ee7b')); assert.ok(!ui.canvasStrokeColours.includes('#76c8ff'));
  assert.deepEqual(ui.errors, []);
});
