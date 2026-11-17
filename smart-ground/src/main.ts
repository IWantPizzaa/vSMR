import './style.css';
import { AirportMap } from './AirportMap';
import { TaxiGraph } from './core/TaxiGraph';
import { RoutePlanner } from './core/RoutePlanner';
import { AircraftSimulator } from './core/AircraftSimulator';
import { PhysicalConflictDetector } from './core/PhysicalConflictDetector';
import { resolveAircraftGeometry } from './core/AircraftGeometry';
import { SurfaceSurveillance } from './core/SurfaceSurveillance';
import type { SurveillanceConflict } from './core/SurveillanceConflictMonitor';
import { OperatingRules, type OperatingManifest } from './core/OperatingRules';
import { TaxiFlowPolicy, type TaxiFlowManifest } from './core/TaxiFlowPolicy';
import { AircraftRestrictions, type AircraftRestrictionManifest } from './core/AircraftRestrictions';
import { AIRCRAFT_PROFILES, DEFAULT_AIRCRAFT_PROFILE, normalizeAircraftProfile, isUnknownAircraftType } from './core/AircraftProfile';
import { resolveOperatingManifest } from './core/resolveOperatingManifest';
import type { AirportData, AirportLocation, Conflict, Route, OperatingConfiguration, MovementType, AircraftProfile, Aircraft } from './core/types';
import { parseBridgeImport } from './adapters';

const escape = (s: unknown) => String(s ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]!));
const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;
const km = (distance: number) => `${(distance / 1000).toFixed(2)} km`;
const clock = (seconds: number) => `${String(Math.floor(seconds / 3600)).padStart(2, '0')}:${String(Math.floor(seconds / 60) % 60).padStart(2, '0')}:${String(Math.floor(seconds) % 60).padStart(2, '0')}`;
const typeNames = { 'head-on': 'Face-à-face', crossing: 'Croisement', overtaking: 'Rattrapage', blockage: 'Blocage' };
const aircraftNames: Record<string, string> = { A320: 'A320', A388: 'A380-800', B744: 'B747-400', B748: 'B747-8' };
const metres = (value: number) => value.toLocaleString('fr-FR', { maximumFractionDigits: 2 });
const profileText = (profile: AircraftProfile) => `${aircraftNames[profile.typeCode] || profile.typeCode} · ${metres(profile.wingspanMetres)} m${profile.lengthMetres === undefined ? '' : ` · L ${metres(profile.lengthMetres)} m`}`;

async function start() {
  const [response, rulesResponse, flowResponse, aircraftResponse] = await Promise.all([
    fetch(`${import.meta.env.BASE_URL}data/lfpg.json`), fetch(`${import.meta.env.BASE_URL}data/lfpg-operations.json`), fetch(`${import.meta.env.BASE_URL}data/lfpg-corridors.json`),
    fetch(`${import.meta.env.BASE_URL}data/lfpg-aircraft-restrictions.json`),
  ]);
  if (!response.ok || !rulesResponse.ok || !flowResponse.ok || !aircraftResponse.ok) throw new Error('Impossible de charger LFPG, ses règles, ses cheminements et ses restrictions aéronefs. Aucun routage libre ne sera utilisé.');
  const data = await response.json() as AirportData;
  const flowManifest = await flowResponse.json() as TaxiFlowManifest;
  const aircraftManifest = await aircraftResponse.json() as AircraftRestrictionManifest;
  const baseManifest = await rulesResponse.json() as OperatingManifest & { coverage?: Record<string, unknown>; limitations?: string[]; edgeNameOverrides?: Record<string, string> };
  let includePpr = false;
  const airportFlags = new Set<string>();
  const aircraftContext = () => ({ airportFlags: [...airportFlags] });
  let aircraftProfile = normalizeAircraftProfile(DEFAULT_AIRCRAFT_PROFILE), profileValid = true, useCustomProfile = false;
  const getManifest = (): OperatingManifest => resolveOperatingManifest(baseManifest, {
    enabledProfileIds: [],
    approvedPprLocationIds: includePpr ? (baseManifest.runwayAccess || []).filter(a => a.pprRequired).map(a => a.locationId) : [],
  });
  const corridorNames = flowManifest.edgeNameOverrides as Record<string, string> | undefined;
  const graph = new TaxiGraph({ ...data, edges: data.edges.map(e => ({ ...e, name: corridorNames?.[e.id] || baseManifest.edgeNameOverrides?.[e.id] || e.name })) });
  const planner = new RoutePlanner(graph), simulator = new AircraftSimulator(graph, planner), detector = new PhysicalConflictDetector(graph);
  let configuration: OperatingConfiguration = 'west', movement: MovementType = 'departure';
  let rules = new OperatingRules(graph, getManifest());
  if (!rules.valid) throw new Error(`Règles LFPG incohérentes : ${rules.diagnostics.filter(d => d.severity === 'error').slice(0, 3).map(d => d.message).join(' ')}`);
  planner.setOperatingRules(rules, configuration);
  const flowPolicy = new TaxiFlowPolicy(graph, flowManifest);
  if (!flowPolicy.valid) throw new Error(`Cheminements MANEX incohérents : ${flowPolicy.diagnostics.slice(0, 3).join(' ')}`);
  planner.setFlowPolicy(flowPolicy);
  const aircraftRestrictions = new AircraftRestrictions(graph, aircraftManifest);
  if (!aircraftRestrictions.valid) throw new Error(`Restrictions aéronefs incohérentes : ${aircraftRestrictions.diagnostics.slice(0, 3).join(' ')}`);
  planner.setAircraftRestrictions(aircraftRestrictions);
  planner.setAircraftProfile(aircraftProfile);
  planner.setAircraftRestrictionContext(aircraftContext());
  const surveillance = new SurfaceSurveillance(graph, { onConfirmedDeviation: (aircraft, observation) => {
    if ((aircraft.conformance?.status ?? 'on-route') === 'on-route') simulator.observeAircraft(aircraft.id, observation);
  } });
  let surveillanceMode: 'radar' | 'exact' = 'radar';
  const locations = [...data.locations, ...(baseManifest.operationalLocations || [])].filter((l, i, all) => all.findIndex(p => p.id === l.id) === i);
  const closedLocationIds = () => new Set(baseManifest.closedLocationIds || []);
  let running = false, multiplier = 1, horizon = 120, seed = 42, selectedId: string | null = null;
  let route: Route | null = null, conflicts: Conflict[] = [], detectionMs = 0, routeMs = 0, activeTab = 'alerts';
  let bridge: ReturnType<typeof parseBridgeImport> | null = null;
  let bridgeEnvelope: unknown = null;
  const stands = data.locations.filter(l => l.kind === 'stand').sort((a, b) => a.name.localeCompare(b.name, 'fr', { numeric: true }));
  const routable = data.locations.filter(l => l.nodeId);
  const option = (l: AirportLocation) => `<option value="${escape(l.id)}" ${l.nodeId ? '' : 'disabled'}>${escape(l.kind === 'runway' ? 'Piste ' : l.kind === 'holding_point' ? 'Point ' : '')}${escape(l.name)}${!l.nodeId ? ' · non raccordé' : graph.componentByNode.get(l.nodeId) !== 0 ? ' · réseau isolé' : ''}</option>`;

  $('app').innerHTML = `
    <header class="topbar">
      <a class="brand" href="./" aria-label="Smart Ground, accueil"><span class="brand-mark">✳</span><span>SMART<span class="brand-light">GROUND</span><small>SURFACE MOVEMENT LAB</small></span></a>
      <div class="airport-title"><span class="icao">LFPG</span><div>Paris Charles de Gaulle<small>France · 49°00′ N 002°33′ E</small></div></div>
      <div class="header-actions"><span class="badge"><i></i> SIMULATEUR LOCAL</span><button class="icon-button" id="help" title="Guide et raccourcis" aria-label="Guide et raccourcis">?</button></div>
    </header>
    <main class="workspace">
      <aside class="left-panel panel">
        <div class="panel-heading"><span class="eyebrow">PLANIFICATION</span><span class="pill">FLUX MANEX</span></div>
        <h1>Un itinéraire.<br>Chaque mouvement.</h1>
        <p class="muted intro">Calculez le roulage sur le réseau réel de LFPG, puis observez le trafic.</p>
        <div class="operating-controls"><label for="configuration">CONFIGURATION EN SERVICE</label><select id="configuration"><option value="west">Face ouest · 26 / 27</option><option value="east">Face est · 08 / 09</option></select><div class="active-runways" id="active-runways"></div><label for="movement">MOUVEMENT</label><select id="movement"><option value="departure">Départ · stand → point d’attente</option><option value="arrival">Arrivée · sortie de piste → stand</option><option value="transfer">Transfert entre stands</option></select><button type="button" id="operations-button" class="text-button">↗ Règles et sources</button></div>
        <div class="aircraft-controls">
          <label for="aircraft-type">PROFIL AÉRONEF</label>
          <select id="aircraft-type">${AIRCRAFT_PROFILES.map(profile => `<option value="${profile.typeCode}">${escape(profileText(profile))}</option>`).join('')}<option value="custom">Autre type / envergure</option></select>
          <div id="aircraft-custom" hidden><label for="aircraft-type-code">Type ICAO</label><input id="aircraft-type-code" value="UNKNOWN" maxlength="8" autocomplete="off" spellcheck="false" /><label for="aircraft-wingspan">Envergure en mètres</label><input id="aircraft-wingspan" type="number" min="0.1" step="0.01" value="35.8" inputmode="decimal" /><label for="aircraft-length">Longueur en mètres · facultatif</label><input id="aircraft-length" type="number" min="0.1" step="0.01" placeholder="Selon le type si connue" inputmode="decimal" /></div>
          <p id="aircraft-profile-label" class="muted tiny">Profil appliqué au prochain itinéraire et au générateur.</p>
          <p id="aircraft-profile-error" class="route-error" role="alert" hidden></p>
        </div>
        <form id="route-form">
          <label for="origin"><span id="origin-label">STAND DE DÉPART</span> <span>A</span></label>
          <select id="origin" aria-label="Stand de départ">${stands.map(option).join('')}</select>
          <div class="route-connector"><span></span><small id="routing-context">Cheminements de référence · face ouest</small></div>
          <label for="destination"><span id="destination-label">POINT D’ATTENTE</span> <span>B</span></label>
          <select id="destination" aria-label="Destination"></select>
          <button class="primary full" type="submit">Calculer l’itinéraire <span>↗</span></button>
        </form>
        <div id="route-summary" class="route-summary" aria-live="polite"><span class="muted">Sélectionnez deux points du réseau.</span></div>
        <div class="create-aircraft"><label for="callsign">INDICATIF</label><div class="input-row"><input id="callsign" value="AFR123" maxlength="16" aria-label="Indicatif de l’avion"/><button id="add-aircraft" class="secondary" disabled title="Ajouter cet avion sur l’itinéraire">+ Avion</button></div></div>
        <div class="section-divider"></div>
        <div class="panel-heading"><span class="eyebrow">GÉNÉRATEUR DE TRAFIC</span><span class="tiny muted">MAX. 100</span></div>
        <div class="traffic-presets" aria-label="Nombre d’avions">${[10, 25, 50, 100].map(n => `<button data-count="${n}" class="preset ${n === 25 ? 'active' : ''}">${n}</button>`).join('')}</div>
        <button id="generate" class="secondary full">⟳ Générer le trafic <span>25 avions</span></button>
        <p class="muted tiny">Arrivées et départs mélangés, mises en mouvement échelonnées.<br>Remplace le trafic · graine <span id="seed">42</span><br>Les traversées de piste restent sous autorisation manuelle.</p>
        <div class="section-divider"></div>
        <div class="panel-heading"><span class="eyebrow">DONNÉES EXTERNES</span><span class="tiny source-status">JSON</span></div>
        <button id="import-bridge" class="text-button full">↥ Importer Ramp Agent / vSID</button>
        <input type="file" id="bridge-file" accept=".json,application/json" hidden />
        <div id="bridge-status" class="muted tiny" role="status" aria-live="polite">Aucune connexion EuroScope active.</div>
        <div class="left-footer"><span class="connection-dot"></span> Prototype v0.1 <span>vSMR v2</span></div>
      </aside>
      <section class="map-panel" aria-label="Carte interactive de LFPG">
        <canvas id="airport-map" tabindex="0" aria-label="Carte LFPG. Glisser pour déplacer, molette pour zoomer, F pour recentrer. Cliquer sur un avion pour le sélectionner."></canvas>
        <div class="map-top"><div class="map-title"><span class="live-dot"></span><span>VUE SOL <small id="map-configuration">LFPG / FACE OUEST · DÉPARTS</small></span></div><button class="map-chip" id="audit-button">◉ <span id="network-count">${data.nodes.length.toLocaleString('fr')} nœuds</span></button></div>
        <div class="map-tools"><button id="zoom-in" aria-label="Zoom avant">+</button><button id="zoom-out" aria-label="Zoom arrière">−</button><button id="fit-map" aria-label="Afficher tout l’aéroport" title="Tout l’aéroport (F)">⌖</button><button id="layers-toggle" aria-label="Afficher les calques" aria-expanded="false" aria-controls="layers" title="Calques">▱</button></div>
        <div id="layers" class="layers" hidden><span class="eyebrow">CALQUES</span>${[['network','Taxiways'],['axes','Axes principaux MANEX'],['operating','Sens de référence et interdictions'],['turns','Jonctions avec virages interdits'],['references','Marquages AVISO · référence'],['stands','Stands'],['labels','Libellés'],['footprints','Enveloppes des avions'],['prediction','Prévision avion · 60 s'],['components','Composantes du graphe']].map(([id, label]) => `<label><input type="checkbox" id="layer-${id}" ${id === 'components' ? '' : 'checked'} />${label}</label>`).join('')}<small class="muted tiny">Sens et interdictions : configuration, mouvement et profil sélectionnés.<br>Les segments interdits sont orange et pointillés.<br>Les cercles barrés signalent au moins un virage interdit au carrefour, sans fermer tous ses mouvements.</small></div>
        <div id="route-edit-tip" class="map-edit-tip" hidden>Glissez un cercle du tracé vers une autre intersection pour attribuer le parcours.</div><div class="north-indicator">N<span>↑</span></div>
        <div class="map-empty" id="map-empty"><span>À vous les commandes.</span><small>Tracez un itinéraire ou générez du trafic pour commencer.</small></div>
        <div class="map-legend"><span><i class="legend-line"></i> Standard</span><span><i class="legend-controller"></i> Attribué</span><span><i class="legend-assigned"></i> Référence assignée</span><span><i class="legend-dot aircraft"></i> Avion</span><span><i class="legend-dot conflict"></i> Conflit prévu</span></div>
        <div class="map-coordinates" id="coordinates">49.00970° N · 2.54790° E</div>
        <div class="simulation-bar"><div class="playback"><button id="play" class="play-button" aria-label="Démarrer la simulation">▶</button><div><span id="simulation-state">EN PAUSE</span><strong id="simulation-clock">00:00:00</strong></div></div><div class="speed-control"><label for="speed">VITESSE</label><select id="speed" aria-label="Vitesse de simulation">${[1,2,5,10].map(n => `<option value="${n}">×${n}</option>`).join('')}</select></div><button id="clear" class="quiet-button" title="Retirer tout le trafic">Réinitialiser</button></div>
      </section>
      <aside class="right-panel panel">
        <div class="panel-heading"><span class="eyebrow">SURVEILLANCE</span><span class="pill" id="monitor-status">EN VEILLE</span></div>
        <div class="stats-grid"><div><strong id="stat-aircraft">0</strong><span>AVIONS</span></div><div><strong id="stat-conflicts" class="alert-number">0</strong><span>CONFLITS PRÉVUS</span></div></div>
        <div class="traffic-summary" id="traffic-summary">0 arrivées · 0 départs · 0 programmés</div>
        <div class="track-summary" id="track-summary" role="status" aria-live="polite"><strong id="stat-offtrack">0</strong> avion hors itinéraire</div>
        <div class="prediction-setting"><label for="horizon">Horizon de prévision</label><select id="horizon">${[60,120,180,300].map(n => `<option value="${n}" ${n === 120 ? 'selected' : ''}>${n} s</option>`).join('')}</select></div>
        <div class="prediction-setting"><label for="surveillance-mode">Données de surveillance</label><select id="surveillance-mode"><option value="radar">Rapports espacés · 5 s</option><option value="exact">Simulation exacte · référence</option></select></div>
        <p id="radar-summary" class="radar-summary muted tiny" role="status">Rapports simulés toutes les 5 s · estimation entre deux mesures.</p>
        <div class="tabs" role="tablist" aria-label="Surveillance"><button id="alerts-tab" role="tab" aria-selected="true" class="active">Alertes <span id="alert-count">0</span></button><button id="aircraft-tab" role="tab" aria-selected="false">Trafic <span id="traffic-count">0</span></button></div>
        <div id="monitor-list" class="monitor-list" role="tabpanel" aria-labelledby="alerts-tab"></div>
        <div id="aircraft-detail" class="aircraft-detail" hidden></div>
        <div class="monitor-footer"><span><i class="connection-dot"></i> PRÉDICTION LOCALE</span><span id="performance">0 ms / cycle</span></div>
      </aside>
    </main>
    <footer class="statusbar"><span>LFPG <i>•</i> AVISO + OpenStreetMap <i>•</i> <button id="sources-button">Sources et connectivité ↗</button></span><span><i class="connection-dot"></i> <span id="bottom-status">Données géographiques locales chargées</span></span><span>SIMULATION UNIQUEMENT</span></footer>
    <div id="toast" role="status" hidden></div>
    <dialog id="info-dialog" aria-labelledby="dialog-title"><div class="dialog-heading"><h2 id="dialog-title"></h2><button id="dialog-close" class="icon-button" aria-label="Fermer">×</button></div><div id="dialog-content"></div></dialog>
  `;

  const map = new AirportMap($<HTMLCanvasElement>('airport-map'), { ...data, locations }, graph);
  map.primaryAxes = flowManifest.axes.filter(a => a.role === 'main').map(a => ({ name: a.name, edgeIds: a.edgeIds, label: (a as typeof a & { label?: { x: number; y: number } }).label }));
  const originSelect = $<HTMLSelectElement>('origin'), destinationSelect = $<HTMLSelectElement>('destination');
  const aircraftSelect = $<HTMLSelectElement>('aircraft-type');
  originSelect.value = stands.find(s => s.nodeId && graph.componentByNode.get(s.nodeId) === 0)?.id || routable.find(s => s.kind === 'stand')?.id || '';
  let trafficCount = 25;
  let toastTimer = 0;
  function toast(text: string) {
    $('toast').textContent = text; $('toast').hidden = false;
    clearTimeout(toastTimer); toastTimer = window.setTimeout(() => $('toast').hidden = true, 5000);
  }
  function findLocation(id: string) { return locations.find(l => l.id === id); }
  function accesses(role: 'departure' | 'arrival') {
    const active = baseManifest.runwayUse?.[configuration]?.[role] || [];
    const closed = closedLocationIds();
    return (baseManifest.runwayAccess || []).filter(a => a.role === role && active.includes(a.runway) && !closed.has(a.locationId) && (includePpr || !a.pprRequired));
  }
  function accessOptions(role: 'departure' | 'arrival') {
    return accesses(role).map(a => {
      const l = findLocation(a.locationId);
      return l ? `<option value="${escape(l.id)}" ${l.nodeId ? '' : 'disabled'}>${escape(a.runway)} · ${escape(l.name)}${a.pprRequired ? ' · PPR' : ''}</option>` : '';
    }).join('');
  }
  function refreshOperatingControls() {
    const oldOrigin = originSelect.value, oldDestination = destinationSelect.value, closed = closedLocationIds();
    map.closedLocationIds.clear(); closed.forEach(id => map.closedLocationIds.add(id));
    const standOptions = stands.map(l => closed.has(l.id) ? `<option disabled value="${escape(l.id)}">${escape(l.name)} · indisponible</option>` : option(l)).join('');
    originSelect.innerHTML = movement === 'arrival' ? accessOptions('arrival') : standOptions;
    destinationSelect.innerHTML = movement === 'departure' ? accessOptions('departure') : standOptions;
    if ([...originSelect.options].some(o => o.value === oldOrigin && !o.disabled)) originSelect.value = oldOrigin;
    if ([...destinationSelect.options].some(o => o.value === oldDestination && !o.disabled)) destinationSelect.value = oldDestination;
    if (originSelect.value === destinationSelect.value) {
      const other = [...destinationSelect.options].find(o => o.value !== originSelect.value && !o.disabled); if (other) destinationSelect.value = other.value;
    }
    const cfg = configuration === 'east' ? 'est' : 'ouest';
    $('origin-label').textContent = movement === 'arrival' ? 'SORTIE DE PISTE' : 'STAND DE DÉPART';
    originSelect.setAttribute('aria-label', movement === 'arrival' ? 'Sortie de piste' : 'Stand de départ');
    $('destination-label').textContent = movement === 'departure' ? 'POINT D’ATTENTE' : 'STAND DE DESTINATION';
    $('routing-context').textContent = `Flux de référence · face ${cfg}`;
    $('active-runways').textContent = `DEP ${(baseManifest.runwayUse?.[configuration]?.departure || []).join(' / ')} · ARR ${(baseManifest.runwayUse?.[configuration]?.arrival || []).join(' / ')}`;
    $('map-configuration').textContent = `FACE ${cfg.toUpperCase()} · ${movement === 'departure' ? 'DÉPARTS' : movement === 'arrival' ? 'ARRIVÉES' : 'TRANSFERTS'} · ${aircraftProfile.typeCode} / ${metres(aircraftProfile.wingspanMetres)} m`;
    map.edgeDirections.clear();
    for (const edge of data.edges) {
      if (edge.kind === 'runway') continue;
      const forward = planner.allowArc(edge, edge.from, edge.to, movement, aircraftProfile);
      const reverse = planner.allowArc(edge, edge.to, edge.from, movement, aircraftProfile);
      if (!forward || !reverse) map.edgeDirections.set(edge.id, forward ? 'forward' : reverse ? 'reverse' : 'closed');
    }
    map.restrictedTurnNodes.clear();
    for (const rule of [...rules.manifest.turnRules || [], ...aircraftManifest.turnRules]) {
      if (!planner.allowTurn(rule.fromEdgeId, rule.viaNodeId, rule.toEdgeId, movement, aircraftProfile)) map.restrictedTurnNodes.add(rule.viaNodeId);
    }
    const scenarios = [airportFlags.has('colored-dual-axis') ? 'axes orange/bleu' : 'axe central', ...(airportFlags.has('southwest-deicing-occupied') ? ['SW1–SW4 occupées'] : [])];
    $('aircraft-profile-label').textContent = `${profileText(aircraftProfile)} · ${scenarios.join(' · ')}. ${isUnknownAircraftType(aircraftProfile) ? 'Type à préciser pour la génération automatique.' : 'Les avions déjà présents conservent leur profil.'}`;
    map.invalidate();
  }
  function syncProfileControls() {
    const preset = !useCustomProfile && aircraftProfile.lengthMetres === undefined && AIRCRAFT_PROFILES.find(p => p.typeCode === aircraftProfile.typeCode && p.wingspanMetres === aircraftProfile.wingspanMetres);
    aircraftSelect.value = preset ? preset.typeCode : 'custom';
    $('aircraft-custom').hidden = !!preset;
    $<HTMLInputElement>('aircraft-type-code').value = aircraftProfile.typeCode;
    $<HTMLInputElement>('aircraft-wingspan').value = String(aircraftProfile.wingspanMetres);
    $<HTMLInputElement>('aircraft-length').value = aircraftProfile.lengthMetres === undefined ? '' : String(aircraftProfile.lengthMetres);
    $('aircraft-profile-label').textContent = `${profileText(aircraftProfile)} · prochain itinéraire et génération. Chaque avion déjà présent conserve son profil.`;
    $('aircraft-profile-error').hidden = true;
    profileValid = true;
    document.querySelector<HTMLButtonElement>('#route-form [type="submit"]')!.disabled = false;
    $<HTMLButtonElement>('generate').disabled = isUnknownAircraftType(aircraftProfile);
  }
  function setPlanningProfile(next: AircraftProfile, custom = false) {
    aircraftProfile = normalizeAircraftProfile(next);
    useCustomProfile = custom;
    planner.setAircraftProfile(aircraftProfile);
    syncProfileControls();
    selectedId = null; map.selectedId = null; route = null; map.route = null;
    $('route-summary').innerHTML = '<span class="muted">Profil modifié : calculez le nouvel itinéraire. Le trafic existant est conservé.</span>';
    refreshOperatingControls(); updateStats(); renderMonitor(); renderDetail();
  }
  function readProfileControls() {
    try {
      const preset = AIRCRAFT_PROFILES.find(p => p.typeCode === aircraftSelect.value);
      const declaredLength = $<HTMLInputElement>('aircraft-length').value.trim();
      const next = preset || { typeCode: $<HTMLInputElement>('aircraft-type-code').value, wingspanMetres: Number($<HTMLInputElement>('aircraft-wingspan').value), ...(declaredLength ? { lengthMetres: Number(declaredLength) } : {}) };
      setPlanningProfile(next, aircraftSelect.value === 'custom');
    } catch {
      profileValid = false; route = null; map.route = null;
      $('aircraft-profile-error').textContent = 'Indiquez un type ICAO et une envergure positive. Pour un B747, choisissez B744 ou B748. Une valeur inférieure au minimum connu du type est refusée.';
      $('aircraft-profile-error').hidden = false;
      document.querySelector<HTMLButtonElement>('#route-form [type="submit"]')!.disabled = true;
      $<HTMLButtonElement>('generate').disabled = true;
      $<HTMLButtonElement>('add-aircraft').disabled = true;
      $('route-summary').innerHTML = '<span class="route-error">Profil invalide : aucun itinéraire ne peut être ajouté.</span>';
      map.invalidate();
    }
  }
  aircraftSelect.onchange = () => {
    $('aircraft-custom').hidden = aircraftSelect.value !== 'custom';
    readProfileControls();
  };
  for (const id of ['aircraft-type-code', 'aircraft-wingspan', 'aircraft-length']) $(id).addEventListener('change', readProfileControls);
  syncProfileControls();
  function invalidateOperatingContext(message: string) {
    running = false; $('play').textContent = '▶'; $('play').setAttribute('aria-label', 'Démarrer la simulation');
    $('simulation-state').textContent = 'EN PAUSE'; $('simulation-state').classList.remove('running');
    simulator.clear(); selectedId = null; map.selectedId = null; route = null; map.route = null;
    rules = new OperatingRules(graph, getManifest()); planner.setOperatingRules(rules, configuration);
    planner.setAircraftRestrictionContext(aircraftContext());
    refreshOperatingControls(); detect(); updateStats(); renderMonitor(); renderDetail();
    $('route-summary').innerHTML = '<span class="muted">Calculez un itinéraire dans la nouvelle configuration.</span>';
    toast(message);
  }
  $('configuration').onchange = () => { configuration = $<HTMLSelectElement>('configuration').value as OperatingConfiguration; invalidateOperatingContext('Configuration modifiée : trafic retiré et itinéraires invalidés.'); };
  function changeMovement(next: MovementType) {
    movement = next; $<HTMLSelectElement>('movement').value = next;
    selectedId = null; map.selectedId = null; route = null; map.route = null;
    refreshOperatingControls(); updateStats(); renderMonitor(); renderDetail();
    $('route-summary').innerHTML = '<span class="muted">Choisissez les points du nouveau mouvement. Le trafic existant est conservé.</span>';
  }
  $('movement').onchange = () => { changeMovement($<HTMLSelectElement>('movement').value as MovementType); toast('Mouvement de planification modifié ; les autres avions poursuivent leurs propres itinéraires.'); };
  refreshOperatingControls();
  function displayRoute() {
    if (!profileValid) { toast('Corrigez le profil aéronef avant de calculer.'); return; }
    const from = findLocation(originSelect.value), to = findLocation(destinationSelect.value);
    if (!from?.nodeId || !to?.nodeId) { toast('Ce point n’est pas raccordé au réseau.'); return; }
    if (closedLocationIds().has(from.id) || closedLocationIds().has(to.id)) { toast('Ce stand ou point est indisponible.'); return; }
    const t = performance.now(); route = planner.findRoute(from.nodeId, to.nodeId, { movement, originId: from.id, destinationId: to.id, aircraftProfile }); routeMs = performance.now() - t;
    selectedId = null; map.selectedId = null; map.route = route;
    if (!route || route.distance < .1) {
      route = null; map.route = null; $('route-summary').innerHTML = `<span class="route-error">${escape(from.id === to.id ? 'Choisissez une destination différente.' : planner.lastFlowFailure || 'Aucun chemin cohérent avec les accès d’aire, axes principaux et restrictions sélectionnés. Aucun retour au routage libre.')}</span>`;
      $<HTMLButtonElement>('add-aircraft').disabled = true; renderDetail(); return;
    }
    const names = route.edgeIds.map(id => graph.edges.get(id)!.name || 'liaison').filter((n, i, all) => i === 0 || all[i - 1] !== n);
    $('route-summary').innerHTML = `<div class="route-metrics"><div><strong>${km(route.distance)}</strong><small>DISTANCE</small></div><div><strong>${Math.ceil(route.distance / 7.7 / 60)} min</strong><small>À 15 KT</small></div></div><div class="route-chain" title="${escape(names.join(' → '))}">${escape(names.join(' → '))}</div><small class="muted tiny">${route.edgeIds.length} segments · A* ${routeMs.toFixed(1)} ms</small>`;
    if (route.flow) {
      const flow = document.createElement('div'); flow.className = 'route-flow';
      const title = document.createElement('span'); title.className = 'eyebrow'; title.textContent = 'AXES DE RÉFÉRENCE';
      const axes = document.createElement('strong'); axes.textContent = route.flow.axes.join(' → ') || 'Desserte locale';
      flow.append(title, axes);
      const quality = document.createElement('small'); quality.className = 'route-quality muted';
      quality.textContent = `${route.flow.significantTurns} virages aux jonctions · ${route.flow.mainAxisChanges} changements d’axe principal`;
      flow.append(quality);
      for (const warning of route.flow.warnings) {
        const note = document.createElement('p'); note.className = 'route-unverified'; note.textContent = warning; flow.append(note);
      }
      $('route-summary').prepend(flow);
    }
    if (route.aircraft) {
      const profile = document.createElement('p'); profile.className = 'route-aircraft tiny';
      profile.textContent = `${profileText(route.aircraft.profile)} · ${route.aircraft.status === 'compatible' ? 'gabarit vérifié sur les restrictions cartographiées' : 'vérification complémentaire requise'}`;
      $('route-summary').prepend(profile);
      for (const message of [...route.aircraft.warnings, ...route.aircraft.advisories.map(a => a.reason)]) {
        const note = document.createElement('p'); note.className = 'route-unverified'; note.textContent = message; $('route-summary').append(note);
      }
    }
    if (route.operational) {
      const note = document.createElement('p'); note.className = 'route-operating-note';
      note.textContent = `Face ${configuration === 'east' ? 'est' : 'ouest'} · ${route.operational.ruleIds.length} règles appliquées · ${route.operational.clearanceCheckpoints.length} traversée(s) sous clairance manuelle.`;
      $('route-summary').append(note);
      if (route.operational.unverifiedEdgeIds.length) {
        const unknown = document.createElement('p'); unknown.className = 'route-unverified';
        unknown.textContent = `${route.operational.unverifiedEdgeIds.length} segments n’ont pas de sens publié numérisé : leur utilisation reste à vérifier.`;
        $('route-summary').append(unknown);
      }
      for (const advisory of route.operational.advisories) {
        const priority = document.createElement('p'); priority.className = 'route-unverified'; priority.textContent = advisory.reason;
        $('route-summary').append(priority);
      }
    }
    $<HTMLButtonElement>('add-aircraft').disabled = simulator.aircraft.length >= 100;
    renderDetail(); updateStats();
  }
  $('route-form').addEventListener('submit', e => { e.preventDefault(); displayRoute(); });
  for (const select of [originSelect, destinationSelect]) select.addEventListener('change', () => {
    route = null; map.route = null; $('route-summary').innerHTML = '<span class="muted">Calculez le nouvel itinéraire.</span>'; $<HTMLButtonElement>('add-aircraft').disabled = true;
  });
  $('add-aircraft').onclick = () => {
    if (!profileValid || !route || simulator.aircraft.length >= 100) return;
    const callsign = $<HTMLInputElement>('callsign').value.trim().toUpperCase();
    if (!/^[A-Z0-9-]{2,16}$/.test(callsign)) { toast('Utilisez un indicatif de 2 à 16 lettres ou chiffres.'); return; }
    if (simulator.aircraft.some(a => a.callsign === callsign)) { toast('Cet indicatif existe déjà dans la simulation.'); return; }
    const aircraft = simulator.addAircraft(originSelect.value, destinationSelect.value, { callsign, movement, aircraftProfile });
    if (!aircraft) { toast('Impossible de créer cet itinéraire.'); return; }
    selectedId = aircraft.id; map.selectedId = selectedId;
    $<HTMLInputElement>('callsign').value = `AFR${124 + simulator.aircraft.length}`;
    detect(); updateStats(); renderMonitor(); renderDetail(); toast(`${callsign} ajouté · appuyez sur ▶ pour rouler.`);
  };
  document.querySelectorAll<HTMLButtonElement>('[data-count]').forEach(button => button.onclick = () => {
    trafficCount = Number(button.dataset.count);
    document.querySelectorAll('[data-count]').forEach(b => b.classList.toggle('active', b === button));
    $('generate').innerHTML = `⟳ Générer le trafic <span>${trafficCount} avions</span>`;
  });
  $('generate').onclick = () => {
    if (!profileValid) return;
    const actualSeed = seed++;
    const count = simulator.generateMixed(trafficCount, actualSeed, { aircraftProfile });
    const scheduled = simulator.aircraft.filter(a => a.state === 'scheduled').length;
    $('seed').textContent = String(actualSeed); selectedId = null; map.selectedId = null;
    detect(); updateStats(); renderMonitor(); renderDetail();
    toast(`${count} avions ${aircraftProfile.typeCode} · arrivées et départs échelonnés · ${scheduled} programmés${count < trafficCount ? ' · itinéraires compatibles insuffisants' : ''}.`);
  };
  $('play').onclick = () => {
    running = !running;
    $('play').textContent = running ? 'Ⅱ' : '▶'; $('play').setAttribute('aria-label', running ? 'Mettre en pause' : 'Démarrer la simulation');
    $('simulation-state').textContent = running ? 'SIMULATION EN COURS' : 'EN PAUSE'; $('simulation-state').classList.toggle('running', running);
  };
  $('speed').onchange = () => { multiplier = Number($<HTMLSelectElement>('speed').value); };
  $('horizon').onchange = () => { horizon = Number($<HTMLSelectElement>('horizon').value); detect(); renderMonitor(); updateStats(); };
  $('surveillance-mode').onchange = () => {
    surveillanceMode = $<HTMLSelectElement>('surveillance-mode').value === 'exact' ? 'exact' : 'radar';
    surveillance.conflicts.reset(); detect(); updateStats(); renderMonitor(); renderDetail();
  };
  $('clear').onclick = () => { simulator.clear(); selectedId = null; map.selectedId = null; detect(); updateStats(); renderMonitor(); renderDetail(); toast('Trafic réinitialisé.'); };
  $('zoom-in').onclick = () => map.zoom(1.4); $('zoom-out').onclick = () => map.zoom(1 / 1.4); $('fit-map').onclick = () => map.fit();
  $('layers-toggle').onclick = () => { $('layers').hidden = !$('layers').hidden; $('layers-toggle').setAttribute('aria-expanded', String(!$('layers').hidden)); };
  for (const [id, key] of [['network','showNetwork'],['axes','showAxes'],['operating','showOperating'],['turns','showTurnRestrictions'],['references','showReferences'],['stands','showStands'],['labels','showLabels'],['footprints','showFootprints'],['prediction','showPrediction'],['components','showComponents']] as const) {
    $( 'layer-' + id).onchange = () => { map[key] = $<HTMLInputElement>('layer-' + id).checked; map.invalidate(); };
  }
  map.onPointer = (x, y) => {
    const lat = 49.0097 + y / 111320, lon = 2.5479 + x / (111320 * Math.cos(49.0097 * Math.PI / 180));
    $('coordinates').textContent = `${lat.toFixed(5)}° N · ${lon.toFixed(5)}° E`;
  };
  function selectAircraft(id: string, focus = false) {
    const chosen = simulator.aircraft.find(a => a.id === id);
    const context = chosen?.route.operational?.movement;
    if (context && context !== movement) changeMovement(context);
    const profile = chosen?.route.aircraft?.profile;
    if (profile && (profile.typeCode !== aircraftProfile.typeCode || profile.wingspanMetres !== aircraftProfile.wingspanMetres || profile.lengthMetres !== aircraftProfile.lengthMetres)) setPlanningProfile(profile, profile.lengthMetres !== undefined);
    selectedId = id; map.selectedId = id;
    const aircraft = simulator.aircraft.find(a => a.id === id);
    if (focus && aircraft) map.focus(aircraft.x, aircraft.y);
    updateStats(); renderDetail(); renderMonitor();
  }
  map.onSelect = id => selectAircraft(id);
  map.onRouteEdit = (id, _displayNodeIndex, targetNodeId, anchorNodeId) => {
    const source = simulator.aircraft.find(a => a.id === id);
    const anchorNodeIndex = source?.route.nodeIds.findIndex((nodeId, index) => nodeId === anchorNodeId && source.route.cumulativeDistances[index] > source.distance + 1e-8) ?? -1;
    const result = simulator.editAssignedRoute(id, { anchorNodeIndex, targetNodeId });
    if (!result.ok) { toast(`Modification refusée : ${result.reason}`); return; }
    const aircraft = simulator.aircraft.find(a => a.id === id)!;
    surveillance.assignmentChanged(aircraft);
    detect(); updateStats(); renderMonitor(); renderDetail();
    toast(`${aircraft.callsign} · route attribuée modifiée. ${aircraft.state === 'holding' ? 'Reprenez le roulage après vérification.' : 'La position de l’avion est conservée.'}`);
  };
  const trackNotificationCounts = new WeakMap<Aircraft, number>();
  const movementOf = (aircraft: Aircraft) => aircraft.route.operational?.movement ?? aircraft.route.flow?.movement ?? aircraft.route.aircraft?.movement;
  const movementLabel = (aircraft: Aircraft) => movementOf(aircraft) === 'arrival' ? 'ARRIVÉE' : movementOf(aircraft) === 'departure' ? 'DÉPART' : 'TRANSFERT';
  const trackStatus = (aircraft: Aircraft) => aircraft.conformance?.status ?? 'on-route';
  const needsRecoveryApproval = (aircraft: Aircraft) => trackStatus(aircraft) === 'recovering' && !!aircraft.conformance?.awaitingApproval;
  const activeDeviations = () => simulator.aircraft.filter(a => trackStatus(a) !== 'on-route');
  function rejoinLabel(nodeId?: string) {
    if (!nodeId) return 'itinéraire attribué';
    const axes = flowManifest.axes.filter(axis => axis.role === 'main' && axis.edgeIds.some(id => {
      const edge = graph.edges.get(id); return edge?.from === nodeId || edge?.to === nodeId;
    })).map(axis => axis.name);
    const names = axes.length ? axes : [...new Set((graph.adjacency.get(nodeId) || []).map(arc => graph.edges.get(arc.edgeId)?.name).filter(Boolean))];
    return names.join(' / ') || 'itinéraire attribué';
  }
  function aircraftStateLabel(aircraft: Aircraft) {
    if (aircraft.state === 'scheduled') return `PRÉVU ${clock(aircraft.scheduledAt ?? 0)}`;
    if (trackStatus(aircraft) === 'unresolved') return 'HORS ROUTE';
    if (trackStatus(aircraft) === 'recovering') return aircraft.conformance?.awaitingApproval ? 'À REPRENDRE' : 'RACCORDEMENT';
    return aircraft.state === 'arrived' ? 'ARRIVÉ' : aircraft.state === 'holding' ? 'ATTENTE' : `${Math.round(aircraft.speed / .514444)} kt`;
  }
  for (const [id, tab] of [['alerts-tab','alerts'],['aircraft-tab','aircraft']]) {
    $(id).onclick = () => {
      activeTab = tab;
      for (const buttonId of ['alerts-tab','aircraft-tab']) { $(buttonId).classList.toggle('active', buttonId === id); $(buttonId).setAttribute('aria-selected', String(buttonId === id)); }
      $('monitor-list').setAttribute('aria-labelledby', id); renderMonitor();
    };
  }
  function detect() {
    const t = performance.now();
    surveillance.captureSimulation(simulator.aircraft, simulator.time);
    conflicts = surveillanceMode === 'radar' ? surveillance.detect(simulator.aircraft, simulator.time, horizon) : detector.detect(simulator.aircraft, horizon);
    detectionMs = performance.now() - t;
    map.conflicts = conflicts;
    for (const aircraft of simulator.aircraft) {
      const events = aircraft.conformance?.events || [];
      if (events.length > (trackNotificationCounts.get(aircraft) || 0)) {
        const event = events.at(-1)!;
        toast(`${aircraft.callsign} · ${event.status === 'rejoined' ? 'itinéraire rejoint' : 'hors itinéraire'} · ${event.reason}`);
      }
      trackNotificationCounts.set(aircraft, events.length);
    }
  }
  function updateStats() {
    map.aircraft = mapAircraft();
    const qualities = simulator.aircraft.filter(a => a.state !== 'scheduled').map(a => surveillance.estimate(a.id, simulator.time)).filter(q => q !== null);
    const uncertain = qualities.filter(q => q.status !== 'tracking').length;
    $('radar-summary').textContent = surveillanceMode === 'radar'
      ? `Rapports simulés / 5 s · ${qualities.length} pistes suivies · ${uncertain} incertaines.`
      : 'Référence exacte du simulateur · aucune latence de mesure.';
    $('stat-aircraft').textContent = $('traffic-count').textContent = String(simulator.aircraft.length);
    const deviations = activeDeviations(), scheduled = simulator.aircraft.filter(a => a.state === 'scheduled').length;
    const arrivals = simulator.aircraft.filter(a => movementOf(a) === 'arrival').length;
    const departures = simulator.aircraft.filter(a => movementOf(a) === 'departure').length;
    $('stat-conflicts').textContent = String(conflicts.length);
    $('alert-count').textContent = String(conflicts.length + deviations.length);
    $('stat-offtrack').textContent = String(deviations.length);
    $('track-summary').classList.toggle('danger', deviations.length > 0);
    $('traffic-summary').textContent = `${arrivals} arrivées · ${departures} départs · ${scheduled} programmés`;
    const vigilance = conflicts.some(f => f.severity === 'critical') || deviations.length > 0;
    $('monitor-status').textContent = vigilance ? 'VIGILANCE' : simulator.aircraft.length ? 'ACTIF' : 'EN VEILLE';
    $('monitor-status').classList.toggle('danger', vigilance);
    $('performance').textContent = `${detectionMs.toFixed(1)} ms / cycle`;
    $('map-empty').hidden = simulator.aircraft.length > 0 || !!route;
    $('route-edit-tip').hidden = !selectedId || map.getRouteEditHandles().length === 0;
    $('simulation-clock').textContent = clock(simulator.time);
    $('bottom-status').textContent = `${simulator.aircraft.filter(a => a.state === 'taxiing').length} en roulage · ${simulator.aircraft.filter(a => a.state === 'holding').length} en attente · ${scheduled} programmés · ${simulator.aircraft.filter(a => a.state === 'arrived').length} arrivés`;
    $<HTMLButtonElement>('add-aircraft').disabled = !profileValid || !route || simulator.aircraft.length >= 100;
  }
  function mapAircraft(): Aircraft[] {
    return surveillanceMode === 'radar'
      ? [...surveillance.predictionFleet(simulator.aircraft, simulator.time).aircraft, ...simulator.aircraft.filter(a => a.state === 'scheduled')]
      : simulator.aircraft;
  }
  function renderMonitor() {
    const list = $('monitor-list'), scroll = list.scrollTop;
    const focused = list.contains(document.activeElement) ? document.activeElement as HTMLElement : null;
    const focusSelector = focused?.dataset.trackAircraft ? `[data-track-aircraft="${CSS.escape(focused.dataset.trackAircraft)}"]`
      : focused?.dataset.aircraft ? `[data-aircraft="${CSS.escape(focused.dataset.aircraft)}"]`
      : focused?.dataset.conflict ? `[data-conflict="${CSS.escape(focused.dataset.conflict)}"]` : null;
    const signature = activeTab === 'aircraft'
      ? JSON.stringify([selectedId, simulator.aircraft.map(a => [a.id, a.callsign, a.originId, a.destinationId, a.speed, a.state, a.scheduledAt, trackStatus(a), a.conformance?.awaitingApproval, a.yielding?.priorityAircraftId, a.waitingClearanceId])])
      : JSON.stringify([horizon, simulator.aircraft.map(a => [a.id, trackStatus(a), a.conformance?.events.length, a.conformance?.awaitingApproval]), conflicts.map(f => [f.id, f.message, f.location, f.severity, Math.ceil(f.timeToConflict), 'quality' in f ? f.quality : null, 'timeToConflictRange' in f ? f.timeToConflictRange : null])]);
    if (list.dataset.signature === signature) return;
    list.dataset.signature = signature;
    if (activeTab === 'alerts') {
      const trackCards = simulator.aircraft.filter(a => a.conformance?.events.length).map(a => {
        const event = a.conformance!.events.at(-1)!;
        const resolved = trackStatus(a) === 'on-route';
        const state = resolved ? 'Itinéraire rejoint' : trackStatus(a) === 'unresolved' ? 'Maintenu hors itinéraire' : 'Raccordement calculé';
        const observed = graph.edges.get(event.observed.edgeId)?.name || 'voie sans nom';
        const expected = event.expectedEdgeId ? graph.edges.get(event.expectedEdgeId)?.name || 'voie sans nom' : 'itinéraire attribué';
        return `<button class="conflict-card track-card ${resolved ? 'resolved' : 'warning'}" data-track-aircraft="${escape(a.id)}"><div class="conflict-heading"><span class="conflict-symbol">${resolved ? '✓' : '↪'}</span><strong>${state}</strong><span>${clock(event.time)}</span></div><div class="conflict-callsigns">${escape(a.callsign)}</div><small>${escape(event.reason)}</small><small>Observé : ${escape(observed)} · attendu : ${escape(expected)}${event.rejoinNodeId ? ` · vers ${escape(rejoinLabel(event.rejoinNodeId))}` : ''}</small></button>`;
      }).join('');
      const conflictCards = conflicts.slice(0, 150).map(f => {
        const aircraft = f.aircraftIds.map(id => simulator.aircraft.find(a => a.id === id)?.callsign || id);
        const radar = 'quality' in f ? f as SurveillanceConflict : null;
        const quality = radar ? `<small class="conflict-quality">${radar.quality === 'degraded' ? 'Données incertaines' : 'Rapports concordants'} · prévision ${Math.floor(radar.timeToConflictRange[0])}–${Math.ceil(radar.timeToConflictRange[1])} s</small>` : '';
        return `<button class="conflict-card ${f.severity}" data-conflict="${escape(f.id)}"><div class="conflict-heading"><span class="conflict-symbol">${f.severity === 'critical' ? '!' : '△'}</span><strong>${typeNames[f.type]}</strong><span>${Math.ceil(f.timeToConflict)} s</span></div><div class="conflict-callsigns">${aircraft.map(escape).join(' <span>↔</span> ')}</div><small>${escape(f.location)} · ${escape(f.message)}</small>${quality}</button>`;
      }).join('');
      list.innerHTML = trackCards + conflictCards || '<div class="empty-state"><div class="empty-radar">◎</div><strong>Aucun écart ni conflit prévu</strong><p>Chaque avion est suivi sur son itinéraire attribué.</p><span class="tiny muted">Horizon : ' + horizon + ' secondes</span></div>';
      list.querySelectorAll<HTMLButtonElement>('[data-track-aircraft]').forEach(button => button.onclick = () => selectAircraft(button.dataset.trackAircraft!, true));
      list.querySelectorAll<HTMLButtonElement>('[data-conflict]').forEach(button => button.onclick = () => {
        const conflict = conflicts.find(f => f.id === button.dataset.conflict); if (!conflict) return;
        selectAircraft(conflict.aircraftIds[0]); map.focus(conflict.x, conflict.y);
      });
    } else {
      list.innerHTML = simulator.aircraft.length ? simulator.aircraft.map(a => `<button class="aircraft-row ${a.id === selectedId ? 'selected' : ''}" data-aircraft="${escape(a.id)}" data-state="${a.state}" data-movement="${movementOf(a) || 'transfer'}"><span class="aircraft-symbol">${trackStatus(a) !== 'on-route' ? '!' : '✈'}</span><span><strong>${escape(a.callsign)}</strong><small>${movementLabel(a)} · ${escape(findLocation(a.originId)?.name || a.originId)} → ${escape(findLocation(a.destinationId)?.name || a.destinationId)}</small></span><span class="aircraft-state ${a.state} ${trackStatus(a)}">${aircraftStateLabel(a)}</span></button>`).join('') : '<div class="empty-state"><div class="empty-radar">✈</div><strong>Le terrain est libre</strong><p>Ajoutez un avion ou générez du trafic pour démarrer.</p></div>';
      list.querySelectorAll<HTMLButtonElement>('[data-aircraft]').forEach(button => button.onclick = () => selectAircraft(button.dataset.aircraft!, true));
    }
    list.scrollTop = scroll;
    if (focusSelector) list.querySelector<HTMLElement>(focusSelector)?.focus({ preventScroll: true });
  }
  let detailSignature = '';
  function renderDetail() {
    const aircraft = simulator.aircraft.find(a => a.id === selectedId), panel = $('aircraft-detail');
    panel.hidden = !aircraft; if (!aircraft) { detailSignature = ''; return; }
    const signature = `${aircraft.id}|${aircraft.state}|${aircraft.waitingClearanceId || ''}|${aircraft.yielding?.ruleId || ''}|${aircraft.yielding?.priorityAircraftId || ''}|${trackStatus(aircraft)}|${aircraft.conformance?.events.length}|${aircraft.conformance?.awaitingApproval}|${aircraft.pendingWrongTurn}|${aircraft.scheduledAt}|${surveillanceMode}|${Math.floor(simulator.time)}|${aircraft.assignedRoute?.assignment?.revision ?? 0}|${aircraft.assignedRoute?.assignment?.source ?? 'standard'}`;
    const focusedId = panel.contains(document.activeElement) ? document.activeElement?.id : null;
    if (detailSignature === signature) {
      panel.querySelector('progress')!.value = aircraft.distance;
      panel.querySelector('.detail-status')!.firstChild!.textContent = `${km(aircraft.distance)} / ${km(aircraft.route.distance)} `;
      return;
    }
    detailSignature = signature;
    panel.innerHTML = `<div class="panel-heading"><strong>${escape(aircraft.callsign)}</strong><button id="deselect" class="quiet-button" aria-label="Désélectionner l’avion">×</button></div><div class="detail-route">${escape(findLocation(aircraft.originId)?.name || aircraft.originId)} <span>→</span> ${escape(findLocation(aircraft.destinationId)?.name || aircraft.destinationId)}</div><p class="detail-movement muted tiny">${movementLabel(aircraft)}${aircraft.state === 'scheduled' ? ` · mise en mouvement au plus tôt à ${clock(aircraft.scheduledAt ?? 0)}, après libération de l’origine` : ''}</p><progress max="${aircraft.route.distance}" value="${aircraft.distance}" aria-label="Progression du roulage"></progress><div class="detail-status">${km(aircraft.distance)} / ${km(aircraft.route.distance)} <span>${aircraft.state === 'scheduled' ? 'Programmé' : aircraft.state === 'arrived' ? 'Arrivé' : aircraft.state === 'holding' ? 'En attente' : 'En roulage'}</span></div><div class="detail-actions"><button id="hold-aircraft" class="secondary" ${aircraft.state === 'arrived' || aircraft.state === 'scheduled' || trackStatus(aircraft) === 'unresolved' ? 'disabled' : ''}>${needsRecoveryApproval(aircraft) ? '▶ Suivre la récupération' : aircraft.state === 'holding' ? '▶ Reprendre' : 'Ⅱ Attendre'}</button><button id="remove-aircraft" class="text-button">Retirer</button></div>`;
    if (aircraft.route.aircraft) {
      const profile = document.createElement('p'); profile.className = 'detail-aircraft tiny';
      profile.textContent = profileText(aircraft.route.aircraft.profile);
      panel.querySelector('.detail-route')!.after(profile);
      for (const message of [...aircraft.route.aircraft.warnings, ...aircraft.route.aircraft.advisories.map(a => a.reason)]) {
        const note = document.createElement('p'); note.className = 'route-unverified'; note.textContent = message; panel.append(note);
      }
    }
    const measurement = surveillance.estimate(aircraft.id, simulator.time);
    const radarDetail = document.createElement('p'); radarDetail.className = 'detail-surveillance muted tiny';
    radarDetail.textContent = surveillanceMode === 'exact' ? 'Surveillance : référence exacte du simulateur.'
      : measurement ? `Dernier rapport reçu il y a ${measurement.ageSeconds.toFixed(1)} s · vitesse filtrée ${(measurement.speedMetresPerSecond / .514444).toFixed(1)} kt · incertitude estimée ±${Math.round(measurement.uncertaintyMetres)} m · ${measurement.status === 'tracking' ? 'suivi établi' : measurement.status === 'suspected-off-track' ? 'écart à confirmer' : measurement.status === 'confirmed-off-track' ? 'écart confirmé' : measurement.status === 'lost' ? 'piste perdue' : measurement.status === 'stale' ? 'rapport ancien' : 'position ambiguë'}.`
      : 'Aucun rapport de position reçu pour ce mouvement programmé.';
    panel.querySelector('.detail-route')!.after(radarDetail);
    $('deselect').onclick = () => { selectedId = null; map.selectedId = null; updateStats(); renderDetail(); renderMonitor(); };
    $('hold-aircraft').onclick = () => { simulator.setHolding(aircraft.id, aircraft.state !== 'holding'); detect(); renderDetail(); renderMonitor(); updateStats(); };
    $('remove-aircraft').onclick = () => { simulator.removeAircraft(aircraft.id); selectedId = null; map.selectedId = null; detect(); renderDetail(); renderMonitor(); updateStats(); };
    const conformance = document.createElement('section'); conformance.className = `detail-conformance ${trackStatus(aircraft)}`;
    const assigned = aircraft.assignedRoute ?? aircraft.route;
    const assignment = assigned.assignment;
    const assignmentPanel = document.createElement('section'); assignmentPanel.className = `detail-assignment ${assignment?.source === 'controller' ? 'controller' : 'standard'}`;
    assignmentPanel.innerHTML = `<strong>${assignment?.source === 'controller' ? `Route attribuée · révision ${assignment.revision}` : 'Parcours standard calculé'}</strong><p class="tiny">Glissez un cercle du tracé vers une autre intersection. Seule la partie à venir est modifiée ; les restrictions restent appliquées.</p>${assignment?.waypointNodeIds.length ? `<p class="tiny">Passages : ${assignment.waypointNodeIds.map(nodeId => escape(rejoinLabel(nodeId))).join(' → ')}</p>` : ''}`;
    panel.append(assignmentPanel);
    if (assignment?.source === 'controller') {
      const undo = document.createElement('button'); undo.id = 'undo-route-edit'; undo.className = 'text-button'; undo.textContent = 'Annuler la dernière modification';
      undo.onclick = () => {
        const result = simulator.undoLastRouteEdit(aircraft.id);
        if (!result.ok) { toast(result.reason); return; }
        surveillance.assignmentChanged(aircraft); detect(); updateStats(); renderMonitor(); renderDetail();
        toast(`${aircraft.callsign} · affectation précédente rétablie ; position conservée.`);
      };
      assignmentPanel.append(undo);
    }
    const geometry = resolveAircraftGeometry(aircraft.aircraftProfile);
    const envelope = document.createElement('p'); envelope.className = 'detail-envelope muted tiny';
    envelope.textContent = `Enveloppe orientée : ${metres(geometry.lengthMetres)} m × ${metres(geometry.wingspanMetres)} m · marge de proximité 2 m.${geometry.warnings.length ? ` ${geometry.warnings.join(' ')}` : ' Forme de planification simplifiée.'}`;
    panel.querySelector('.detail-route')!.after(envelope);
    const assignedAxes = assigned.flow?.axes.join(' → ') || 'desserte locale';
    const currentTrack = trackStatus(aircraft);
    const stateText = currentTrack === 'unresolved' ? 'Hors itinéraire · maintien obligatoire' : currentTrack === 'recovering' ? `Raccordement vers ${rejoinLabel(aircraft.conformance?.rejoinNodeId)}` : 'Sur l’itinéraire attribué';
    conformance.innerHTML = `<strong>${escape(stateText)}</strong><p class="tiny">Référence : ${escape(assignedAxes)}</p>${needsRecoveryApproval(aircraft) ? '<p class="tiny">Raccordement calculé. Vérifiez le trafic avant de suivre la récupération ; les traversées restent soumises à leur autorisation manuelle.</p>' : ''}${currentTrack === 'unresolved' ? '<p class="tiny">Aucun raccordement autorisé trouvé depuis cette position et ce sens. La reprise reste bloquée.</p>' : ''}${aircraft.conformance?.events.length ? `<details open><summary>Historique des écarts (${aircraft.conformance.events.length})</summary><ol>${aircraft.conformance.events.slice(-8).map(event => `<li><time>${clock(event.time)}</time> ${escape(event.reason)} <span>${event.status === 'rejoined' ? 'Référence rejointe' : event.status === 'recovering' ? 'Raccordement proposé' : 'Maintenu'}</span></li>`).join('')}</ol></details>` : ''}`;
    panel.append(conformance);
    const deviate = document.createElement('button'); deviate.id = 'deviate-aircraft'; deviate.className = 'text-button full deviation-demo';
    deviate.textContent = aircraft.pendingWrongTurn ? 'Mauvais virage armé au prochain carrefour' : 'Simuler un mauvais virage';
    deviate.disabled = aircraft.state !== 'taxiing' || currentTrack !== 'on-route' || !!aircraft.pendingWrongTurn;
    deviate.onclick = () => {
      const armed = simulator.simulateWrongTurn(aircraft.id);
      toast(armed ? `${aircraft.callsign} : un mauvais virage sera simulé au prochain carrefour compatible.` : 'Aucun mauvais virage simulable sur le parcours restant.');
      renderDetail(); renderMonitor(); updateStats();
    };
    panel.append(deviate);
    const demoNote = document.createElement('p'); demoNote.className = 'muted tiny';
    demoNote.textContent = 'Test direct du routage sur une position exacte, indépendant du filtre radar.';
    deviate.after(demoNote);
    if (aircraft.route.flow) {
      const axes = document.createElement('p'); axes.className = 'detail-flow';
      axes.textContent = `Axes : ${aircraft.route.flow.axes.join(' → ') || 'desserte locale'}`;
      panel.querySelector('.detail-route')!.after(axes);
      const quality = document.createElement('p'); quality.className = 'muted tiny route-quality';
      quality.textContent = `${aircraft.route.flow.significantTurns} virages aux jonctions · ${aircraft.route.flow.mainAxisChanges} changements d’axe principal`;
      axes.after(quality);
    }
    if (aircraft.yielding) {
      const priority = document.createElement('p'); priority.className = 'clearance-notice';
      const high = simulator.aircraft.find(a => a.id === aircraft.yielding!.priorityAircraftId)?.callsign || aircraft.yielding.priorityAircraftId;
      priority.textContent = `Priorité à ${high}. ${aircraft.yielding.reason} ${aircraft.state === 'holding' ? 'Reprise automatique après dégagement ; aucune clairance de piste délivrée.' : 'L’avion s’arrêtera avant la jonction si nécessaire.'}`;
      panel.append(priority);
      if (aircraft.state === 'holding' && !needsRecoveryApproval(aircraft)) $<HTMLButtonElement>('hold-aircraft').disabled = true;
    }
    if (aircraft.waitingClearanceId) {
      $<HTMLButtonElement>('hold-aircraft').disabled = !needsRecoveryApproval(aircraft);
      if (needsRecoveryApproval(aircraft)) $('hold-aircraft').textContent = 'Valider le raccordement';
      const checkpoint = aircraft.route.operational?.clearanceCheckpoints.find(c => c.id === aircraft.waitingClearanceId);
      const notice = document.createElement('p'); notice.className = 'clearance-notice';
      notice.textContent = checkpoint?.reason || 'Arrêt avant traversée de piste : une clairance manuelle est nécessaire.';
      const clearButton = document.createElement('button'); clearButton.id = 'clear-runway'; clearButton.className = 'secondary full';
      clearButton.textContent = 'Autoriser cette traversée · simulation';
      clearButton.disabled = needsRecoveryApproval(aircraft) || trackStatus(aircraft) === 'unresolved';
      clearButton.onclick = () => { simulator.clearRunwayCrossing(aircraft.id); detect(); renderDetail(); renderMonitor(); updateStats(); };
      panel.append(notice, clearButton);
    }
    if (focusedId) document.getElementById(focusedId)?.focus({ preventScroll: true });
  }

  function dialog(title: string, html: string) { $('dialog-title').textContent = title; $('dialog-content').innerHTML = html; $<HTMLDialogElement>('info-dialog').showModal(); }
  $('dialog-close').onclick = () => $<HTMLDialogElement>('info-dialog').close();
  $<HTMLDialogElement>('info-dialog').addEventListener('click', e => { if (e.target === $('info-dialog')) $<HTMLDialogElement>('info-dialog').close(); });
  function showOperations() {
    const activeRules = { ...getManifest(), edgeRules: getManifest().edgeRules.filter(r => r.authoritative !== false) };
    const blocked = [...map.edgeDirections.values()].filter(d => d === 'closed').length;
    dialog('LFPG · règles de circulation', `<p>Configuration <b>face ${configuration === 'east' ? 'est' : 'ouest'}</b>, mouvement <b>${movement === 'departure' ? 'départ' : movement === 'arrival' ? 'arrivée' : 'transfert'}</b>. Les sens sont appliqués à chaque section ; les flux départ et arrivée peuvent différer sur une même voie.</p><div class="audit-grid"><div><strong>${activeRules.edgeRules.length}</strong>Contraintes de section</div><div><strong>${(activeRules.turnRules || []).length}</strong>Virages interdits</div><div><strong>${blocked}</strong>Segments interdits ici</div><div><strong>${accesses('departure').length}</strong>Points de départ actifs</div></div><p>Les départs se terminent à un point d’attente réel. Les arrivées commencent après dégagement de la piste extérieure, avant la traversée de l’intérieure. Chaque traversée matérialisée exige une intervention manuelle dans la simulation. Les virages non documentés aux croisements synthétiques ne sont pas utilisés.</p><p class="route-error">Les contraintes publiées sont datées ; les instructions ATC et les NOTAM en vigueur peuvent modifier le roulage. Les sections sans sens numérisé sont signalées dans chaque itinéraire. Les restrictions de type et d’envergure utilisent le profil sélectionné ; elles s’ajoutent aux restrictions générales.</p><h3>Accès soumis à accord préalable</h3><label class="works-option"><input id="include-ppr" type="checkbox" ${includePpr ? 'checked' : ''} />Inclure les accès PPR après accord préalable simulé</label><h3>Sources</h3><ul class="operations-sources">${baseManifest.sources.map(s => `<li><a href="${escape(s.url)}" target="_blank" rel="noopener">${escape(s.title)}</a>${s.effectiveDate ? ` · ${escape(s.effectiveDate)}` : ''}</li>`).join('')}</ul><details><summary>Couverture et limites de numérisation</summary><pre>${escape(JSON.stringify({ coverage: baseManifest.coverage, limitations: baseManifest.limitations, diagnostics: rules.diagnostics }, null, 2))}</pre></details>`);
    const flowSection = document.createElement('section');
    flowSection.innerHTML = `<h3>Cheminements de référence MANEX / AIP</h3><p>Le parcours rejoint les axes principaux par les accès de l’aire concernée, puis conserve un cheminement continu jusqu’à la desserte finale. Les passages par une aire étrangère et les sorties/rentrées répétées dans le réseau principal sont évalués avant la distance. Le choix tient aussi compte des virages aux jonctions et des changements d’axe pour éviter les zigzags.</p><p>Les voies intérieures de ${flowManifest.specialAreas?.length || 0} baies de dégivrage cartographiées sont exclues du transit normal simulé. Les axes qui les desservent restent utilisables ; une mission de dégivrage n’est pas simulée.</p><p>Les interdictions AIP et les fermetures restent strictes. Les anciennes flèches locales déduites d’AVISO ne sont plus traitées comme des interdictions générales. Les adaptations aux voies secondaires restent soumises à coordination.</p><p><b>${flowManifest.axes.filter(a => a.role === 'main').map(a => escape(a.name)).join(' · ')}</b> : axes principaux identifiés. ${flowManifest.flowPolicies.length} cheminements nominaux décrits.</p><ul class="operations-sources">${flowManifest.sources.map(s => `<li>${/^https?:\/\//.test(s.url || '') ? `<a href="${escape(s.url)}" target="_blank" rel="noopener">${escape(s.title)}</a>` : escape(s.title)}${s.effectiveDate ? ` · ${escape(s.effectiveDate)}` : ''}</li>`).join('')}</ul><details><summary>Portée du modèle de convergence</summary><pre>${escape(JSON.stringify({ diagnostics: flowPolicy.diagnostics, limitations: flowManifest.limitations || flowManifest.diagnostics }, null, 2))}</pre></details>`;
    $('dialog-content').prepend(flowSection);
    const aircraftSection = document.createElement('section');
    const applicableTurns = aircraftManifest.turnRules.filter(rule => aircraftRestrictions.turnConstraints(rule.fromEdgeId, rule.viaNodeId, rule.toEdgeId, configuration, movement, aircraftProfile, aircraftContext()).some(candidate => candidate.id === rule.id));
    const turnReasons = [...new Set(applicableTurns.map(rule => rule.reason))];
    aircraftSection.innerHTML = `<h3>Type, envergure et virages</h3><p><b>${escape(profileText(aircraftProfile))}</b>. Les voies interdites pour ce profil sont retirées du calcul. Les virages sont vérifiés avec leur voie d’entrée et de sortie ; une interdiction de tourner ne ferme pas toute la voie.</p><p>${aircraftManifest.edgeRules.length} restrictions de sections et ${aircraftManifest.turnRules.length} transitions cartographiées, avec leurs conditions. ${applicableTurns.length} transitions interdites correspondent au profil et au contexte actuels.</p><label class="works-option"><input type="checkbox" data-aircraft-flag="southwest-deicing-occupied" ${airportFlags.has('southwest-deicing-occupied') ? 'checked' : ''} /><span><b>Baies SW1–SW4 occupées</b><small>Active la limite d’envergure conditionnelle sur la section de T concernée.</small></span></label><label class="works-option"><input type="checkbox" data-aircraft-flag="colored-dual-axis" ${airportFlags.has('colored-dual-axis') ? 'checked' : ''} /><span><b>Circulation sur les axes orange et bleu</b><small>Applique la limite de 36 m sur les voies concernées. Sinon, l’axe central est utilisé. Les positions des deux lignes parallèles ne sont pas simulées.</small></span></label><p class="muted tiny">Changer ces conditions retire le trafic existant. Les consignes d’oversteering sont affichées sur le parcours ; elles ne sont pas transformées en fermetures.</p>${turnReasons.length ? `<details><summary>Virages interdits dans ce contexte</summary><ul>${turnReasons.map(reason => `<li>${escape(reason)}</li>`).join('')}</ul></details>` : ''}<ul class="operations-sources">${aircraftManifest.sources.map(source => `<li>${/^https?:\/\//.test(source.url) ? `<a href="${escape(source.url)}" target="_blank" rel="noopener">${escape(source.title)}</a>` : escape(source.title)}</li>`).join('')}</ul><details><summary>Couverture des restrictions aéronefs</summary><pre>${escape(JSON.stringify({ coverage: aircraftManifest.coverage, limitations: aircraftManifest.limitations, diagnostics: aircraftRestrictions.diagnostics, unresolved: aircraftManifest.coverageIssues }, null, 2))}</pre></details>`;
    $('dialog-content').prepend(aircraftSection);
    document.querySelectorAll<HTMLInputElement>('[data-aircraft-flag]').forEach(input => input.onchange = () => {
      const flag = input.dataset.aircraftFlag!;
      if (input.checked) airportFlags.add(flag); else airportFlags.delete(flag);
      invalidateOperatingContext('Conditions de gabarit modifiées : trafic retiré.'); showOperations();
    });
    $('include-ppr').onchange = () => { includePpr = $<HTMLInputElement>('include-ppr').checked; invalidateOperatingContext('Accès PPR modifiés : trafic retiré.'); showOperations(); };
  }
  $('operations-button').onclick = showOperations;
  function showAudit() {
    const unconnected = data.locations.filter(l => !l.nodeId);
    const unavailableStands = stands.filter(l => !l.nodeId || graph.componentByNode.get(l.nodeId) !== 0);
    const mainComponent = Math.max(...graph.components.map(c => c.length));
    dialog('LFPG · audit du réseau', `<p>Le graphe conserve la topologie des sources. Les composantes isolées et les stands non raccordés restent visibles ; aucun segment n’est ajouté pour combler un manque.</p><div class="audit-grid"><div><strong>${data.nodes.length.toLocaleString('fr')}</strong>Nœuds</div><div><strong>${data.edges.length.toLocaleString('fr')}</strong>Segments</div><div><strong>${stands.length}</strong>Stands importés</div><div><strong>${graph.components.length}</strong>Composantes</div></div><p><b>${mainComponent.toLocaleString('fr')}</b> nœuds dans la plus grande composante. <b>${unconnected.length}</b> points sans raccordement (${unconnected.filter(l => l.kind === 'stand').length} stands).</p><p>Les avions suivent les segments importés. Les pistes ne servent pas de raccourci entre taxiways. Les entrées de piste sont des destinations de simulation ; aucune clairance n’est émise.</p><details><summary>Points non raccordés</summary><p class="tiny">${escape(unconnected.map(l => `${l.name} (${l.kind})`).join(', ') || 'Aucun')}</p></details><details><summary>Provenance et diagnostics complets</summary><pre>${escape(JSON.stringify(data.metadata, null, 2))}</pre></details><p class="muted">© OpenStreetMap contributors · ODbL. Surfaces AVISO et positions Ramp Agent : voir les notices du jeu de données. Dates, versions et limites dans <a href="data/connectivity-report.json" target="_blank" rel="noopener">le rapport de connectivité</a>.</p><button class="secondary" id="download-audit">Télécharger l’audit JSON</button>`);
    $('download-audit').before(Object.assign(document.createElement('p'), { textContent: `${unavailableStands.length} stands ne peuvent pas rejoindre le réseau principal : ${unavailableStands.filter(l => !l.nodeId).length} sans raccordement et ${unavailableStands.filter(l => l.nodeId).length} sur un réseau isolé. Ces limites restent signalées dans les sélecteurs.` }));
    $('download-audit').onclick = () => {
      const blob = new Blob([JSON.stringify({ metadata: data.metadata, componentSizes: graph.components.map(c => c.length), disconnectedLocations: unconnected, unavailableStands }, null, 2)], { type: 'application/json' });
      const url = URL.createObjectURL(blob), a = document.createElement('a'); a.href = url; a.download = 'smart-ground-lfpg-audit.json'; a.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
    };
  }
  $('audit-button').onclick = $('sources-button').onclick = showAudit;
  $('help').onclick = () => dialog('Utiliser Smart Ground', `<p>1. Choisissez face est ou face ouest, puis départ, arrivée ou transfert. Changer de configuration retire le trafic existant. Chaque avion conserve son propre mouvement.</p><p>2. Choisissez les points, puis calculez le parcours. Un départ se termine à un point d’attente réel. Une arrivée commence après dégagement de l’extérieure, avant la traversée de l’intérieure.</p><p>3. Ajoutez un avion ou générez 10 à 100 mouvements mixtes, puis démarrez avec ▶. Les arrivées et départs sont échelonnés ; les avions programmés apparaissent dans la liste avant leur mise en mouvement. Cliquez sur un avion pour inspecter son parcours ou le maintenir manuellement.</p><p>Aux points de traversée, l’avion s’arrête et attend votre action « Autoriser cette traversée · simulation ». Reprendre le roulage ne constitue pas une clairance. Les prédictions ne supposent pas qu’une autorisation future sera délivrée.</p><p>Un écart de voie ou de sens identifie l’avion dans les alertes et conserve son itinéraire attribué comme référence. Un raccordement autorisé est recalculé ; il faut reprendre explicitement la récupération. Sans solution autorisée, l’avion reste maintenu. Le bouton « Simuler un mauvais virage » arme une seule erreur au prochain carrefour compatible. Aucun écart aléatoire n’est activé par défaut.</p><p>Les règles sont accessibles sous les sélecteurs. Les points PPR exigent une sélection explicite d’accord préalable simulé. Les sections sans règle numérisée restent signalées : la couverture IRL n’est pas exhaustive et les NOTAM temps réel ne sont pas disponibles.</p><p><b>Carte :</b> glisser pour déplacer, molette ou +/− pour zoomer, F pour tout LFPG. Le calque des sens correspond à la configuration et au mouvement sélectionnés.</p><p><b>Plugins :</b> import JSON <code>smart-ground.bridge.v1</code>, sans lien actif à EuroScope. Les affectations contraires à la configuration ou périmées sont refusées.</p><p>Choisissez A380, B747-400, B747-8 ou un type et une envergure personnalisés. Chaque avion conserve son profil ; les calques suivent celui qui est sélectionné. Les options de dégivrage et d’axes orange/bleu figurent dans les règles. Les alertes de conflit ne délivrent aucune autorisation et ne résolvent pas toutes les rencontres.</p>`);
  $('import-bridge').onclick = () => $<HTMLInputElement>('bridge-file').click();
  function refreshBridge() {
    if (!bridgeEnvelope) return null;
    bridge = parseBridgeImport(bridgeEnvelope, Date.now());
    $('bridge-status').textContent = `${bridge.assignments.length} indicatifs importés · ${bridge.simulated ? 'données simulées' : 'capture JSON'}${bridge.stale ? ' · PÉRIMÉE' : ''} · fichier local`;
    return bridge;
  }
  function resolveBridgeLocations(assignment: ReturnType<typeof parseBridgeImport>['assignments'][number]) {
    const standMatches = assignment.stand ? stands.filter(l => l.name.trim().toUpperCase() === assignment.stand) : [];
    const stand = standMatches.length === 1 && standMatches[0].nodeId && !closedLocationIds().has(standMatches[0].id) ? standMatches[0] : null;
    const runwayMatches = assignment.runway ? accesses('departure').filter(a => a.runway === assignment.runway) : [];
    // vSID chooses the runway. This simulator only selects an existing entry for that exact runway.
    const runway = runwayMatches.map(a => findLocation(a.locationId)).find(l => l?.nodeId) || null;
    const reason = !assignment.stand || !assignment.runway
      ? 'Affectation de stand ou piste indisponible.'
      : standMatches.length > 1
        ? 'Nom ambigu dans le réseau : plusieurs correspondances exactes.'
        : !stand ? 'Stand absent, non raccordé ou indisponible.'
          : !runway ? 'Piste attribuée inactive dans cette configuration, ou aucun point d’attente utilisable.' : '';
    return { stand, runway, reason };
  }
  function showBridgeImport() {
    const imported = refreshBridge(); if (!imported) return;
    dialog('Ramp Agent / vSID · capture importée', `<p>Capture ${escape(imported.capturedAt)} · ${imported.simulated ? 'SIMULÉE' : 'fichier local'}</p><p id="bridge-capture-status" role="status" aria-live="polite">${imported.stale ? 'PÉRIMÉE : affectations inutilisables.' : 'Capture fraîche · validité maximale de 15 secondes.'}</p>${imported.diagnostics.map(d => `<p class="route-error">${escape(d)}</p>`).join('')}<div class="bridge-assignments">${imported.assignments.map(a => {
      const resolved = resolveBridgeLocations(a);
      const unavailable = imported.stale || !!resolved.reason;
      return `<div class="bridge-row"><span><b>${escape(a.callsign)}</b><small>Stand ${escape(a.stand || '—')} · Piste ${escape(a.runway || '—')}</small>${resolved.reason ? `<small>${escape(resolved.reason)}</small>` : ''}</span><button class="secondary" data-apply="${escape(a.callsign)}" aria-label="Préparer l’itinéraire de ${escape(a.callsign)}" ${unavailable ? 'disabled' : ''}>Préparer</button></div>`;
    }).join('')}</div><p class="muted tiny">Préparer sélectionne les données attribuées par les plugins. Cela ne crée pas d’affectation et ne déplace aucun avion. La fraîcheur est vérifiée de nouveau à chaque sélection.</p>`);
    document.querySelectorAll<HTMLButtonElement>('[data-apply]').forEach(button => button.onclick = () => {
      const current = refreshBridge();
      if (!current || current.stale) {
        document.querySelectorAll<HTMLButtonElement>('[data-apply]').forEach(b => b.disabled = true);
        const status = document.getElementById('bridge-capture-status');
        if (status) status.textContent = 'PÉRIMÉE : affectations inutilisables. Importez une nouvelle capture.';
        toast('La capture est périmée. Importez un nouvel instantané pour utiliser ses affectations.');
        return;
      }
      const assignment = current.assignments.find(a => a.callsign === button.dataset.apply);
      if (!assignment) { toast('Cet indicatif ne figure plus dans la capture importée.'); return; }
      const { stand, runway, reason } = resolveBridgeLocations(assignment);
      if (!stand || !runway) { toast(reason); return; }
      if (movement !== 'departure') changeMovement('departure');
      originSelect.value = stand.id; destinationSelect.value = runway.id; $<HTMLInputElement>('callsign').value = assignment.callsign;
      $<HTMLDialogElement>('info-dialog').close(); displayRoute();
    });
  }
  window.setInterval(() => {
    if (!bridgeEnvelope || bridge?.stale) return;
    const current = refreshBridge();
    if (current?.stale) {
      document.querySelectorAll<HTMLButtonElement>('[data-apply]').forEach(button => button.disabled = true);
      const status = document.getElementById('bridge-capture-status');
      if (status) status.textContent = 'PÉRIMÉE : affectations inutilisables. Importez une nouvelle capture.';
    }
  }, 1000);
  $('bridge-file').onchange = async () => {
    const input = $<HTMLInputElement>('bridge-file'), file = input.files?.[0]; if (!file) return;
    try {
      if (file.size > 5_000_000) throw new Error('Le fichier dépasse 5 Mo.');
      const envelope: unknown = JSON.parse(await file.text());
      bridge = parseBridgeImport(envelope);
      bridgeEnvelope = envelope;
      showBridgeImport();
    } catch (e) { toast(`Import refusé : ${e instanceof Error ? e.message : String(e)}`); }
    input.value = '';
  };

  let lastFrame = performance.now(), lastMonitor = 0, lastDetect = 0;
  function frame(now: number) {
    const dt = Math.min((now - lastFrame) / 1000, .25); lastFrame = now;
    if (running) simulator.update(dt * multiplier);
    if (running && now - lastDetect > 350) { detect(); lastDetect = now; }
    if (running && now - lastMonitor > 700) { updateStats(); renderMonitor(); renderDetail(); lastMonitor = now; }
    if (surveillanceMode === 'radar') map.aircraft = mapAircraft();
    map.draw(); requestAnimationFrame(frame);
  }
  renderMonitor(); updateStats(); requestAnimationFrame(frame);
}

start().catch(error => {
  console.error(error);
  $('app').innerHTML = `<div class="loading"><strong>Le simulateur n’a pas pu démarrer.</strong><span>${escape(error instanceof Error ? error.message : error)}</span><small>Depuis smart-ground : npm install, puis npm run dev.</small><button onclick="location.reload()">Réessayer</button></div>`;
});
