(() => {
  "use strict";

  const PROTOCOL_VERSION = 1;
  const HISTORY_LIMIT = 12;
  const HOST_MODE = Boolean(window.chrome?.webview?.postMessage);
  const DATA = window.VSMR_DATA || { profiles: [], aviso: { type: "FeatureCollection", features: [], styles: {} } };
  const DEFAULT_DATA = JSON.parse(JSON.stringify(DATA));
  document.documentElement.classList.toggle("host-mode", HOST_MODE);
  const PAGE_TITLES = {
    display: "Display",
    aviso: "AVISO",
    alerts: "Alerts",
    groups: "Groups",
    modes: "Modes",
    profiles: "Profiles",
    settings: "Settings"
  };
  const PROFILE_TITLES = { colors: "Colors", icons: "Icons", tags: "Tags", rules: "Rules" };
  const MAP_ZOOM_LABELS = [
    "All ranges", "34 km or closer", "28 km or closer", "22 km or closer", "18 km or closer",
    "14 km or closer", "12 km or closer", "9.5 km or closer", "8 km or closer", "6 km or closer",
    "5 km or closer", "4 km or closer", "3 km or closer", "2.5 km or closer", "2 km or closer"
  ];
  const MODE_STATUSES = ["no_status", "push", "startup", "taxi", "departure", "on_runway", "airborne", "arrivals", "no_fpl", "uncorrelated"];
  const RULE_STATUSES = ["default", "no_status", "push", "startup", "taxi", "departure", "on_runway", "airborne", "arrivals", "no_fpl", "uncorrelated"];
  const TAG_SCOPES = ["departure", "arrival", "uncorrelated", "airborne"];
  const TAG_STATUS_LABELS = {
    default: "Default", taxi: "Taxi", push: "Push", stup: "Startup", nofpl: "No FPL", depa: "Departure",
    airdep: "Airborne departure", airdep_onrunway: "Departure on runway", airarr: "Airborne arrival",
    airarr_onrunway: "Arrival on runway"
  };
  const COLOR_FAMILY_ORDER = ["Tags", "Targets", "RIMCAS", "Approach inset"];
  const COLOR_SECTION_ORDER = ["General", "Departure", "Arrival", "Uncorrelated", "Airborne"];
  const TAG_STATUS_COLOR_KEYS = {
    departure: {
      default: "background_no_status_color", taxi: "background_taxi_color", push: "background_push_color",
      stup: "background_startup_color", nofpl: "background_no_fpl_color", depa: "background_departure_color",
      airdep: "background_airborne_color", airdep_onrunway: "background_on_runway_color"
    },
    arrival: {
      default: "background_on_ground_color", nofpl: "background_no_fpl_color",
      airarr: "background_airborne_color", airarr_onrunway: "background_on_runway_color"
    },
    uncorrelated: { default: "background_on_ground_color" }
  };
  const TAG_TOKENS = ["callsign", "actype", "sctype", "wake", "deprwy", "gs", "flightlevel", "tendency", "scratchpad", "asid", "uk_stand", "sqerror", "groundstatus", "systemid"];
  const RULE_SOURCES = ["vacdm", "runway", "aircraft", "flightplan", "tag", "aviso"];
  const ALERT_TYPES = [
    { id: "NO PUSH", description: "Pushback not authorized" },
    { id: "NO TAXI", description: "Taxi movement not authorized" },
    { id: "NO TKOF", description: "Take-off not authorized" },
    { id: "STAT RPA", description: "Stationary runway protected-area alert" },
    { id: "RWY INC", description: "Runway incursion" },
    { id: "RWY TYPE", description: "Runway use or aircraft-type mismatch" },
    { id: "RWY CLSD", description: "Movement on a closed runway" },
    { id: "HIGH SPD", description: "High-speed movement" },
    { id: "EMERG", description: "Emergency squawk or emergency state" }
  ];
  const DEFAULT_ALERT_RUNWAYS = [
    { id: "09L / 27R", arrival: true, departure: true, closed: false },
    { id: "09R / 27L", arrival: true, departure: true, closed: false },
    { id: "08L / 26R", arrival: false, departure: false, closed: false },
    { id: "08R / 26L", arrival: false, departure: false, closed: false }
  ];
  const ALERT_COLOR_FIELDS = [
    ["alertStageOne", "background_color_stage_one", "#a05a1e"],
    ["alertStageTwo", "background_color_stage_two", "#960000"],
    ["alertCautionText", "caution_alert_text_color", "#000000"],
    ["alertCautionBg", "caution_alert_background_color", "#ffff00"],
    ["alertWarningText", "warning_alert_text_color", "#ffffff"],
    ["alertWarningBg", "warning_alert_background_color", "#ff0000"]
  ];
  const DEFAULT_AVISO_GROUP_BLUEPRINTS = [
    { id: "runway-details", name: "Runway details", accent: "#d9d9d9", styles: ["surface.runway", "marking.runway", "line.runway_centerline", "label.tora"] },
    { id: "ground-layout-arrows", name: "Ground layout arrows", accent: "#d4bd39", stylePrefix: "line.ground_layout_arrows." },
    { id: "holding-positions", name: "Holding positions", accent: "#e18956", styles: ["marking.holding_cat_i", "marking.holding_cat_iii", "marking.holding_permanent", "marking.stop_point", "lighting.stop_bar"] },
    { id: "stands-and-gates", name: "Stands & gates", accent: "#84b7d5", styles: ["label.gates_stands", "line.stand_entry", "line.stand_entry_dashed"] },
    { id: "vfr-points", name: "VFR points", accent: "#8eb68d", styles: ["label.vfr_points"] }
  ];

  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => Array.from(root.querySelectorAll(selector));
  const clone = value => JSON.parse(JSON.stringify(value));
  const uid = prefix => `${prefix}-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
  const escapeHtml = value => String(value ?? "")
    .replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;").replaceAll("'", "&#039;");

  function clamp(value, min, max) {
    const number = Number(value);
    return Math.min(max, Math.max(min, Number.isFinite(number) ? number : min));
  }

  function humanize(value) {
    return String(value || "")
      .replace(/_color$/i, "")
      .replaceAll("_", " ")
      .replace(/\b\w/g, letter => letter.toUpperCase());
  }

  function normalizeHex(value, fallback = "#ffffff") {
    const raw = String(value || "").trim();
    if (/^#[0-9a-f]{6}$/i.test(raw)) return raw.toLowerCase();
    if (/^#[0-9a-f]{3}$/i.test(raw)) return `#${raw[1]}${raw[1]}${raw[2]}${raw[2]}${raw[3]}${raw[3]}`.toLowerCase();
    return fallback;
  }

  function colorToHex(color, fallback = "#ffffff") {
    if (!color || typeof color !== "object") return fallback;
    const component = key => Math.round(clamp(color[key], 0, 255)).toString(16).padStart(2, "0");
    return `#${component("r")}${component("g")}${component("b")}`;
  }

  function hexToColor(hex, alpha = 255) {
    const normalized = normalizeHex(hex);
    return {
      r: parseInt(normalized.slice(1, 3), 16),
      g: parseInt(normalized.slice(3, 5), 16),
      b: parseInt(normalized.slice(5, 7), 16),
      a: Math.round(clamp(alpha, 0, 255))
    };
  }

  function isColorObject(value) {
    return value && typeof value === "object" && !Array.isArray(value)
      && ["r", "g", "b"].every(key => Number.isFinite(Number(value[key])));
  }

  function getAtPath(root, path) {
    return path.reduce((value, key) => value?.[key], root);
  }

  function setAtPath(root, path, value) {
    let cursor = root;
    path.slice(0, -1).forEach(key => {
      if (!cursor[key] || typeof cursor[key] !== "object") cursor[key] = {};
      cursor = cursor[key];
    });
    cursor[path[path.length - 1]] = value;
  }

  function inferAirport(text) {
    const match = String(text || "").toUpperCase().match(/\bLF[A-Z0-9]{2}\b/);
    return match?.[0] || "";
  }

  function normalizeAirportCode(value) {
    return String(value || "").trim().toUpperCase();
  }

  function normalizeAvisoPresetStore(store) {
    const normalized = store && typeof store === "object" && !Array.isArray(store) ? store : {};
    if (!Array.isArray(normalized.items)) normalized.items = [];
    for (let index = normalized.items.length - 1; index >= 0; --index) {
      if (!normalized.items[index] || typeof normalized.items[index] !== "object") normalized.items.splice(index, 1);
    }
    normalized.items.forEach((item, index) => {
      item.name = String(item.name || `Preset ${index + 1}`).trim() || `Preset ${index + 1}`;
      item.linked_movement = Boolean(item.linked_movement);
    });
    normalized.default = String(normalized.default || "").trim();
    if (normalized.default && !normalized.items.some(item => item.name === normalized.default)) normalized.default = "";
    return normalized;
  }

  function profileAvisoPresetStoreForAirport(profile, airport, migrateLegacy = true) {
    profile.aviso_presets ||= {};
    const root = profile.aviso_presets;
    if (!root.airports || typeof root.airports !== "object" || Array.isArray(root.airports)) root.airports = {};

    const airportCode = normalizeAirportCode(airport);
    if (!airportCode) return normalizeAvisoPresetStore({});

    let store = root.airports[airportCode];
    const hasAirportStores = Object.keys(root.airports).length > 0;
    const hasLegacyStore = Array.isArray(root.items) || typeof root.default === "string";
    if ((!store || typeof store !== "object" || Array.isArray(store)) && migrateLegacy && !hasAirportStores && hasLegacyStore) {
      store = { default: String(root.default || ""), items: clone(Array.isArray(root.items) ? root.items : []) };
      delete root.default;
      delete root.items;
    }
    if (!store || typeof store !== "object" || Array.isArray(store)) store = {};
    root.airports[airportCode] = normalizeAvisoPresetStore(store);
    return root.airports[airportCode];
  }

  function normalizeAvisoGroupId(value, fallback = "group") {
    const normalized = String(value || fallback).trim().toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "");
    return normalized || fallback;
  }

  function normalizeAvisoData(sourceAviso, createDefaults = true) {
    const aviso = clone(sourceAviso || { type: "FeatureCollection", features: [], styles: {} });
    if (!Array.isArray(aviso.features)) aviso.features = [];
    if (!aviso.styles || typeof aviso.styles !== "object" || Array.isArray(aviso.styles)) aviso.styles = {};
    if (!Array.isArray(aviso.vsmr_groups)) aviso.vsmr_groups = [];

    const seen = new Set();
    const groupIdAliases = new Map();
    aviso.vsmr_groups = aviso.vsmr_groups.map((group, index) => {
      const sourceGroup = group && typeof group === "object" ? group : {};
      const rawId = sourceGroup.id ?? sourceGroup.group_id ?? sourceGroup.name ?? `group-${index + 1}`;
      const preservedId = String(rawId ?? "");
      let id = preservedId || normalizeAvisoGroupId(sourceGroup.name || `group-${index + 1}`);
      const base = id;
      let suffix = 2;
      while (seen.has(id)) id = `${base}-${suffix++}`;
      seen.add(id);
      [rawId, sourceGroup.id, sourceGroup.group_id, id].forEach(alias => {
        if (alias != null && String(alias)) groupIdAliases.set(String(alias), id);
      });
      return {
        ...sourceGroup,
        id,
        name: String(sourceGroup.name || `Group ${index + 1}`).trim() || `Group ${index + 1}`,
        visible: sourceGroup.visible !== false,
        accent: normalizeHex(sourceGroup.accent || "#84b7d5", "#84b7d5")
      };
    });

    aviso.features.forEach(feature => {
      if (!feature || typeof feature !== "object") return;
      feature.properties ||= {};
      const properties = feature.properties;
      let sourceIds = [];
      if (Array.isArray(properties.vsmr_group_ids)) sourceIds = properties.vsmr_group_ids;
      else if (Array.isArray(properties.vsmr_groups)) sourceIds = properties.vsmr_groups;
      else if (Array.isArray(properties.group_ids)) sourceIds = properties.group_ids;
      else if (properties.group_id != null) sourceIds = [properties.group_id];
      else if (properties.vsmr_group_id != null) sourceIds = [properties.vsmr_group_id];
      const normalizedIds = Array.from(new Set(sourceIds
        .map(value => groupIdAliases.get(String(value)) || String(value || "").trim())
        .filter(Boolean)));
      if (normalizedIds.length) feature.properties.vsmr_group_ids = normalizedIds;
      else if (Array.isArray(feature.properties.vsmr_group_ids)) feature.properties.vsmr_group_ids = [];
    });

    if (createDefaults && !aviso.vsmr_groups.length) {
      DEFAULT_AVISO_GROUP_BLUEPRINTS.forEach(blueprint => {
        const indices = [];
        aviso.features.forEach((feature, index) => {
          const styleId = String(feature?.properties?.style_id || "");
          const exact = blueprint.styles?.includes(styleId);
          const prefixed = blueprint.stylePrefix && styleId.startsWith(blueprint.stylePrefix);
          if (exact || prefixed) indices.push(index);
        });
        if (!indices.length) return;
        const id = normalizeAvisoGroupId(blueprint.id || blueprint.name);
        aviso.vsmr_groups.push({ id, name: blueprint.name, visible: true, accent: blueprint.accent || "#84b7d5" });
        indices.forEach(index => setFeatureGroupMembership(aviso.features[index], id, true));
      });
    }

    return aviso;
  }

  function getProfileRecords(sourceProfiles = DATA.profiles) {
    const records = [];
    const extras = [];
    let metadata = { schema_version: 1, last_active_profile: "", vacdm: { server_url: "https://cdm.vatsim.fr" } };
    (Array.isArray(sourceProfiles) ? sourceProfiles : []).forEach((entry, index) => {
      if (entry && typeof entry === "object" && entry.name) {
        records.push({
          id: `profile-${index}-${String(entry.name).replace(/\W+/g, "-").toLowerCase()}`,
          persistedName: String(entry.name),
          data: clone(entry),
          original: clone(entry)
        });
      } else if (entry?._vsmr) {
        metadata = clone(entry._vsmr);
      } else {
        extras.push(clone(entry));
      }
    });
    return { records, metadata, extras };
  }

  function createState(bundle = DATA) {
    const { records, metadata, extras } = getProfileRecords(bundle.profiles);
    const preferred = records.find(record => record.data.name === "Custom LFPG")
      || records.find(record => record.data.name === metadata.last_active_profile)
      || records[0];
    const initialAirport = normalizeAirportCode(bundle.airport
      || bundle.aviso?.metadata?.airport
      || inferAirport(bundle.aviso?.name)
      || inferAirport(preferred?.data?.name));
    const preferredPresetStore = preferred
      ? profileAvisoPresetStoreForAirport(preferred.data, initialAirport, true)
      : normalizeAvisoPresetStore({});
    const preferredPresetItems = preferredPresetStore.items;
    const preferredPresetName = String(preferredPresetStore.default || preferredPresetItems[0]?.name || "");
    const preferredPreset = preferredPresetItems.find(item => item?.name === preferredPresetName) || preferredPresetItems[0] || null;
    const normalizedAviso = normalizeAvisoData(bundle.aviso || { type: "FeatureCollection", features: [], styles: {} });
    const avisoFeatures = normalizedAviso.features || [];
    const taxiwayLabelIndex = avisoFeatures.findIndex(feature => feature?.properties?.category === "Taxiway labels");
    const defaultTextIndex = taxiwayLabelIndex >= 0 ? taxiwayLabelIndex : avisoFeatures.findIndex(feature => feature?.properties?.object_type === "Label" || feature?.properties?.["text-field"] != null);
    const defaultTextStyleId = avisoFeatures[defaultTextIndex]?.properties?.style_id || "label.taxiways";
    const defaultGeometryFeature = avisoFeatures.find(feature => feature?.properties?.style_id === "surface.taxiway")
      || avisoFeatures.find(feature => feature?.properties?.object_type !== "Label" && feature?.properties?.["text-field"] == null);
    const defaultGeometryStyleId = defaultGeometryFeature?.properties?.style_id || "surface.taxiway";
    return {
      profiles: records,
      metadata: clone(metadata),
      profileExtras: clone(extras),
      activeProfileId: preferred?.id || "",
      aviso: normalizedAviso,
      airport: initialAirport,
      hostAirport: initialAirport,
      settings: {
        profileFile: "vSMR_DATA\\vSMR_Profiles.json",
        avisoFile: "vSMR_DATA\\AVISO\\AVISO_LFPG.geojson",
        watchFiles: true,
        bridgeMode: "Auto detect",
        updateInterval: 250,
        resolutionPreset: preferred?.data?.targets?.small_icon_boost_resolution_preset || "1080p",
        showFps: true,
        runtimeSync: true,
        confirmDelete: true,
        rimcas: true,
        vacdm: true,
        cpdlc: true,
        approachWindows: true
      },
      ui: {
        page: "display",
        profileTab: "colors",
        avisoView: "text",
        selectedColorPath: "labels.departure.background_taxi_color",
        selectedTagId: "departure:taxi",
        selectedRuleIndex: 0,
        selectedModeIndex: 0,
        managedProfileId: preferred?.id || "",
        selectedAvisoGeometryStyleId: defaultGeometryStyleId,
        selectedAvisoGeometryStyleIds: defaultGeometryStyleId ? [defaultGeometryStyleId] : [],
        avisoGeometrySelectionAnchorId: defaultGeometryStyleId,
        selectedAvisoTextStyleId: defaultTextStyleId,
        selectedAvisoTextIndex: defaultTextIndex >= 0 ? defaultTextIndex : 0,
        selectedAvisoTextIndices: defaultTextIndex >= 0 ? [defaultTextIndex] : [],
        avisoTextSelectionAnchorIndex: defaultTextIndex >= 0 ? defaultTextIndex : 0,
        selectedAvisoGroupId: normalizedAviso.vsmr_groups?.[0]?.id || "",
        avisoGroupSearch: "",
        avisoGroupMemberSearch: "",
        avisoGroupMemberFilter: "all",
        avisoGroupContentType: "text",
        avisoGroupContentSearch: "",
        controlCenterOpen: HOST_MODE,
        runtimePopover: "",
        avisoGeometrySearch: "",
        avisoTextSearch: "",
        alertsView: "active"
      },
      runtime: {
        avisoInsetVisible: false,
        insets: { aviso: false, srw1: false, srw2: false },
        activeAvisoPreset: preferredPresetName,
        activeAvisoPresetScope: `${preferred?.id || ""}:${initialAirport}`,
        avisoInsetSnapshot: preferredPreset ? clone(preferredPreset) : null,
        alerts: {
          visibility: "normal",
          runways: clone(DEFAULT_ALERT_RUNWAYS)
        }
      },
      dirty: false
    };
  }

  let state = createState();
  const treeState = { colors: new Set(), tags: new Set() };
  const drafts = { color: null, tag: null, rule: null, mode: null, profile: null, avisoGeometry: null, avisoTextStyle: null, avisoTextLabel: null, avisoGroup: null, alerts: null };
  let activeTagInput = null;
  let toastTimer = 0;
  let avisoGeometryRenderOrder = [];
  let avisoTextRenderOrder = [];
  let githubResourceType = "aviso";
  let avisoGroupContentDraft = null;
  let draggedAvisoGroupId = "";
  let insetPresetDialogMode = "capture";
  let outboundMessageSequence = 0;
  const pending = { save: "", reload: "", github: null };
  let splitAvisoContext = null;
  const history = { past: [], present: null, future: [] };
  let savedSnapshot = null;

  function activeProfileRecord() {
    return state.profiles.find(record => record.id === state.activeProfileId) || state.profiles[0];
  }
  function activeProfile() { return activeProfileRecord()?.data || {}; }
  function managedProfileRecord() {
    return state.profiles.find(record => record.id === state.ui.managedProfileId) || activeProfileRecord();
  }

  function assignAirportPresetStore(profile, airport, store) {
    if (!profile || typeof profile !== "object" || !airport || !store || typeof store !== "object" || Array.isArray(store)) return false;
    profile.aviso_presets ||= {};
    if (!profile.aviso_presets.airports || typeof profile.aviso_presets.airports !== "object" || Array.isArray(profile.aviso_presets.airports)) {
      profile.aviso_presets.airports = {};
    }
    profile.aviso_presets.airports[airport] = clone(store);
    return true;
  }

  function assignRecordAirportPresetStore(record, airport, store) {
    if (!record) return false;
    const assigned = assignAirportPresetStore(record.data, airport, store);
    assignAirportPresetStore(record.original, airport, store);
    return assigned;
  }

  function rebasePresetStoreSnapshots(profileId, profileNames, airport, store) {
    const names = new Set((Array.isArray(profileNames) ? profileNames : [profileNames]).filter(Boolean));
    const snapshots = [history.present, ...history.past, ...history.future, savedSnapshot];
    const visited = new Set();
    snapshots.forEach(snapshot => {
      if (!snapshot?.profiles || visited.has(snapshot)) return;
      visited.add(snapshot);
      try {
        const records = JSON.parse(snapshot.profiles);
        const record = Array.isArray(records)
          ? records.find(item => item?.id === profileId)
            || records.find(item => names.has(item?.data?.name))
          : null;
        if (record && assignRecordAirportPresetStore(record, airport, store)) {
          snapshot.profiles = JSON.stringify(records);
        }
      } catch (error) {
        console.warn("Could not rebase inset presets in editor history", error);
      }
    });
  }

  function nextMessageId() {
    outboundMessageSequence += 1;
    return `ui-${Date.now().toString(36)}-${outboundMessageSequence.toString(36)}`;
  }

  function postBridge(type, payload = {}) {
    const message = { version: PROTOCOL_VERSION, id: nextMessageId(), type, payload };
    try {
      if (window.chrome?.webview?.postMessage) window.chrome.webview.postMessage(message);
      else window.dispatchEvent(new CustomEvent("vsmr-control-center", { detail: message }));
    } catch (error) {
      console.warn("Bridge message failed", error);
    }
    return message.id;
  }

  function snapshotChunk(value) {
    return JSON.stringify(value ?? null);
  }

  function captureHistorySnapshot(reuse = history.present) {
    const values = {
      profiles: state.profiles,
      metadata: state.metadata,
      profileExtras: state.profileExtras,
      aviso: state.aviso,
      settings: state.settings
    };
    const snapshot = {};
    Object.entries(values).forEach(([key, value]) => {
      const encoded = snapshotChunk(value);
      snapshot[key] = reuse?.[key] === encoded ? reuse[key] : encoded;
    });
    return snapshot;
  }

  function snapshotsEqual(left, right) {
    if (!left || !right) return false;
    return ["profiles", "metadata", "profileExtras", "aviso", "settings"]
      .every(key => left[key] === right[key]);
  }

  function updateCommandState() {
    const busy = Boolean(pending.save || pending.reload);
    const airportMismatch = Boolean(state.airport && state.hostAirport && state.airport !== state.hostAirport);
    const saveButton = $("#saveButton");
    const reloadButton = $("#reloadButton");
    const undoButton = $("#undoButton");
    const redoButton = $("#redoButton");
    if (saveButton) {
      saveButton.disabled = !state.dirty || Boolean(pending.save) || airportMismatch;
      saveButton.classList.toggle("pending", Boolean(pending.save));
      saveButton.title = airportMismatch
        ? "Reload after changing airports before saving"
        : pending.save ? "Saving…" : state.dirty ? "Save changes" : "No changes to save";
    }
    if (reloadButton) {
      reloadButton.disabled = busy;
      reloadButton.classList.toggle("pending", Boolean(pending.reload));
      reloadButton.title = pending.reload ? "Reloading…" : "Reload configuration";
    }
    if (undoButton) undoButton.disabled = busy || airportMismatch || history.past.length === 0;
    if (redoButton) redoButton.disabled = busy || airportMismatch || history.future.length === 0;
  }

  function updateDirtyState(message = "") {
    state.dirty = !snapshotsEqual(history.present, savedSnapshot);
    const dot = $("#dirtyDot");
    if (dot) {
      dot.classList.toggle("dirty", state.dirty);
      dot.title = state.dirty ? "Unsaved changes" : "Saved";
    }
    $("#saveButton")?.classList.toggle("has-unsaved", state.dirty);
    updateCommandState();
    if (message) setStatus(message, state.dirty ? "info" : "");
  }

  function recordHistoryState() {
    const next = captureHistorySnapshot();
    if (history.present && snapshotsEqual(next, history.present)) return false;
    if (history.present) {
      history.past.push(history.present);
      if (history.past.length > HISTORY_LIMIT) history.past.splice(0, history.past.length - HISTORY_LIMIT);
    }
    history.present = next;
    history.future.length = 0;
    return true;
  }

  function resetHistory(saved = true) {
    history.past.length = 0;
    history.future.length = 0;
    history.present = captureHistorySnapshot(null);
    if (saved) savedSnapshot = history.present;
    updateDirtyState();
  }

  function restoreHistorySnapshot(snapshot) {
    if (!snapshot) return;
    const preservedUi = state.ui;
    const preservedActiveProfileId = state.activeProfileId;
    state.profiles = JSON.parse(snapshot.profiles);
    state.metadata = JSON.parse(snapshot.metadata);
    state.profileExtras = JSON.parse(snapshot.profileExtras || "[]");
    state.activeProfileId = preservedActiveProfileId;
    state.aviso = JSON.parse(snapshot.aviso);
    state.settings = JSON.parse(snapshot.settings);
    state.ui = preservedUi;
    if (!state.profiles.some(record => record.id === state.activeProfileId)) state.activeProfileId = state.profiles[0]?.id || "";
    if (!state.profiles.some(record => record.id === state.ui.managedProfileId)) state.ui.managedProfileId = state.activeProfileId;
    Object.keys(drafts).forEach(key => drafts[key] = null);
    history.present = snapshot;
    renderAll();
    updateDirtyState();
  }

  function showToast(message, type = "") {
    const toast = $("#toast");
    toast.textContent = message;
    toast.className = `toast visible ${type}`.trim();
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { toast.className = "toast"; }, 1800);
  }

  function setStatus(message, type = "") {
    const text = $("#statusText");
    const light = $("#statusLight");
    if (text) text.textContent = message;
    if (light) light.className = `status-light ${type}`.trim();
    document.documentElement.dataset.status = type || "ready";
  }
  function markDirty(message = "Changes not saved") {
    recordHistoryState();
    updateDirtyState(message);
  }

  function markSaved(message = "Saved") {
    state.profiles.forEach(record => {
      record.original = clone(record.data);
      record.persistedName = String(record.data?.name || "");
    });
    history.present = captureHistorySnapshot();
    savedSnapshot = history.present;
    state.dirty = false;
    const dot = $("#dirtyDot");
    if (dot) { dot.classList.remove("dirty"); dot.title = "Saved"; }
    $("#saveButton")?.classList.remove("has-unsaved");
    updateCommandState();
    setStatus(message);
  }

  function updateContext() {
    const context = $("#statusContext");
    if (!context) return;
    const profileName = activeProfile().name || "No profile";
    let suffix = PAGE_TITLES[state.ui.page];
    if (state.ui.page === "display") suffix = PROFILE_TITLES[state.ui.profileTab];
    if (state.ui.page === "aviso") {
      const viewLabel = state.ui.avisoView === "text" ? "Text" : "Geometry";
      suffix = `${state.aviso?.metadata?.airport || inferAirport(state.aviso?.name) || "AVISO"} · ${viewLabel}`;
    }
    context.textContent = `${profileName} · ${suffix}`;
  }
  function setPage(page) {
    if (!PAGE_TITLES[page]) return;
    state.ui.page = page;
    $$(".rail-button[data-page]").forEach(button => button.classList.toggle("active", button.dataset.page === page));
    $$('[data-page-panel]').forEach(panel => panel.classList.toggle("active", panel.dataset.pagePanel === page));
    const pageTitle = $("#pageTitle");
    if (pageTitle) pageTitle.textContent = PAGE_TITLES[page];
    if (page === "display") renderCurrentProfileTab();
    if (page === "aviso") renderAviso();
    if (page === "alerts") renderAlerts();
    if (page === "groups") renderAvisoGroups();
    if (page === "modes") renderModes();
    if (page === "profiles") renderProfilesManager();
    if (page === "settings") renderSettings();
    updateContext();
  }

  function setProfileTab(tab) {
    if (!PROFILE_TITLES[tab]) return;
    state.ui.profileTab = tab;
    $$('[data-profile-tab]').forEach(button => button.classList.toggle("active", button.dataset.profileTab === tab));
    $$('[data-profile-panel]').forEach(panel => panel.classList.toggle("active", panel.dataset.profilePanel === tab));
    renderCurrentProfileTab();
    updateContext();
  }

  function profileRailLabel(name) {
    const airport = inferAirport(name);
    if (airport) return airport;
    const words = String(name || "Profile").trim().split(/\s+/).filter(Boolean);
    const initials = words.map(word => word[0]).join("").slice(0, 4).toUpperCase();
    return initials || "PROF";
  }

  function setRailProfilePopoverOpen(open) {
    const popover = $("#railProfilePopover");
    const button = $("#railProfileButton");
    if (!popover || !button) return;
    popover.hidden = !open;
    button.setAttribute("aria-expanded", String(open));
    button.classList.toggle("open", open);
  }

  function renderGlobalProfileSelect() {
    const select = $("#globalProfileSelect");
    if (!select) return;
    select.innerHTML = state.profiles.map(record => `<option value="${escapeHtml(record.id)}">${escapeHtml(record.data.name)}</option>`).join("");
    select.value = state.activeProfileId;
    const name = activeProfile().name || "Profile";
    const shortLabel = $("#railProfileShort");
    if (shortLabel) shortLabel.textContent = profileRailLabel(name);
    const button = $("#railProfileButton");
    if (button) button.title = `Active profile: ${name}`;
  }

  function switchActiveProfile(profileId, syncFilters = true) {
    if (!state.profiles.some(record => record.id === profileId)) return;
    state.activeProfileId = profileId;
    state.ui.managedProfileId = profileId;
    const profile = activeProfile();
    const colors = collectProfileColors(profile);
    if (!colors.some(entry => entry.id === state.ui.selectedColorPath)) state.ui.selectedColorPath = colors[0]?.id || "";
    state.ui.selectedRuleIndex = 0;
    const modes = profile.filters?.display_modes?.items || [];
    state.ui.selectedModeIndex = Math.max(0, modes.findIndex(mode => mode.name === profile.filters?.display_modes?.active));
    state.ui.selectedTagId = "departure:taxi";
    Object.keys(drafts).forEach(key => drafts[key] = null);
    const airport = inferAirport(profile.name);
    renderAllProfileSections();
    drafts.alerts = null;
    syncRuntimePresetForProfile();
    if (state.ui.page === "alerts") renderAlerts();
    renderGlobalProfileSelect();
    setRailProfilePopoverOpen(false);
    renderRuntimeMenu();
    updateContext();
  }

  function postActiveProfileChange() {
    const record = activeProfileRecord();
    if (!record) return;
    postBridge("runtime.profile.change", { profileId: record.id, profile: record.data.name });
  }

  function activeModeName() {
    const modes = activeProfile()?.filters?.display_modes;
    return modes?.active || modes?.items?.[0]?.name || "Normal";
  }

  function avisoGroups() {
    state.aviso.vsmr_groups ||= [];
    return state.aviso.vsmr_groups;
  }

  function featureGroupIds(feature) {
    const properties = feature?.properties || {};
    let ids = [];
    if (Array.isArray(properties.vsmr_group_ids)) ids = properties.vsmr_group_ids;
    else if (Array.isArray(properties.vsmr_groups)) ids = properties.vsmr_groups;
    else if (Array.isArray(properties.group_ids)) ids = properties.group_ids;
    else if (properties.group_id != null) ids = [properties.group_id];
    else if (properties.vsmr_group_id != null) ids = [properties.vsmr_group_id];
    return Array.from(new Set(ids.map(String)));
  }

  function setFeatureGroupMembership(feature, groupId, member) {
    if (!feature) return;
    feature.properties ||= {};
    const ids = featureGroupIds(feature);
    const next = member ? Array.from(new Set([...ids, groupId])) : ids.filter(id => id !== groupId);
    if (next.length) feature.properties.vsmr_group_ids = next;
    else if (
      "group_id" in feature.properties ||
      "vsmr_group_id" in feature.properties ||
      "vsmr_groups" in feature.properties ||
      "group_ids" in feature.properties
    ) feature.properties.vsmr_group_ids = [];
    else delete feature.properties.vsmr_group_ids;
  }

  function avisoGroupMemberIndices(groupId) {
    return avisoFeatures().map((feature, index) => featureGroupIds(feature).includes(groupId) ? index : -1).filter(index => index >= 0);
  }

  function avisoGroupCounts(groupId) {
    const counts = { total: 0, text: 0, line: 0, area: 0 };
    avisoGroupMemberIndices(groupId).forEach(index => {
      const feature = avisoFeatures()[index];
      const kind = isAvisoTextFeature(feature) ? "text" : inferAvisoObjectType(feature).toLowerCase();
      counts.total += 1;
      if (kind === "text") counts.text += 1;
      else if (kind === "line") counts.line += 1;
      else counts.area += 1;
    });
    return counts;
  }

  function avisoGroupSummary(counts) {
    const lineLabel = counts.line === 1 ? "line" : "lines";
    const areaLabel = counts.area === 1 ? "area" : "areas";
    return `${counts.text} text · ${counts.line} ${lineLabel} · ${counts.area} ${areaLabel}`;
  }

  function selectedAvisoGroup() {
    const groups = avisoGroups();
    let group = groups.find(item => item.id === state.ui.selectedAvisoGroupId);
    if (!group && groups.length) {
      group = groups[0];
      state.ui.selectedAvisoGroupId = group.id;
    }
    return group || null;
  }

  function syncSurfaceVisibility() {
    const control = $("#controlWindow");
    const runtime = $("#runtimeMenu");
    if (!control || !runtime) return;
    control.classList.toggle("hidden", !HOST_MODE && !state.ui.controlCenterOpen);
    runtime.classList.toggle("hidden", HOST_MODE || state.ui.controlCenterOpen);
  }

  function openControlCenter(page = "settings", avisoView = "") {
    state.ui.controlCenterOpen = true;
    state.ui.runtimePopover = "";
    syncSurfaceVisibility();
    if (PAGE_TITLES[page]) setPage(page);
    if (page === "aviso" && ["geometry", "text"].includes(avisoView)) {
      state.ui.avisoView = avisoView;
      renderAviso();
    }
    renderRuntimeMenu();
  }

  function closeControlCenter() {
    if (HOST_MODE) {
      postBridge("window.close", { dirty: state.dirty });
      return;
    }
    state.ui.controlCenterOpen = false;
    syncSurfaceVisibility();
    renderRuntimeMenu();
  }

  function activePresetAirport() {
    return normalizeAirportCode(state.hostAirport);
  }

  function activePresetScope() {
    return `${state.activeProfileId || ""}:${activePresetAirport()}`;
  }

  function profileAvisoPresetStore(profile = activeProfile()) {
    return profileAvisoPresetStoreForAirport(profile, activePresetAirport(), true);
  }

  function avisoPresets() { return profileAvisoPresetStore().items; }

  function activeAvisoPreset() {
    if (state.runtime.activeAvisoPresetScope !== activePresetScope()) return syncRuntimePresetForProfile();
    if (!String(state.runtime.activeAvisoPreset || "")) {
      state.runtime.avisoInsetSnapshot = null;
      return null;
    }
    const presets = avisoPresets();
    const preset = presets.find(item => item.name === state.runtime.activeAvisoPreset);
    if (!preset) {
      state.runtime.avisoInsetSnapshot = null;
      return null;
    }
    return preset;
  }

  function syncRuntimePresetForProfile() {
    const store = profileAvisoPresetStore();
    const preset = store.items.find(item => item.name === store.default) || store.items[0] || null;
    state.runtime.activeAvisoPreset = preset?.name || "";
    state.runtime.activeAvisoPresetScope = activePresetScope();
    state.runtime.avisoInsetSnapshot = preset ? clone(preset) : null;
    return preset;
  }

  function insetState(kind) {
    state.runtime.insets ||= { aviso: false, srw1: false, srw2: false };
    return Boolean(state.runtime.insets[kind]);
  }

  function setRuntimePopover(kind = "") {
    const next = state.ui.runtimePopover === kind ? "" : kind;
    state.ui.runtimePopover = ["mode", "groups", "inset", "profile"].includes(next) ? next : "";
    renderRuntimeMenu();
  }

  function renderRuntimeMenu() {
    const menu = $("#runtimeMenu");
    if (!menu) return;
    const groups = avisoGroups();
    const visibleGroups = groups.filter(group => group.visible !== false).length;
    const profile = activeProfile();
    const mode = activeModeName();
    const preset = activeAvisoPreset();
    const anyInset = ["aviso", "srw1", "srw2"].some(insetState);

    const modeButton = $("#runtimeModeButton");
    const groupsButton = $("#runtimeGroupsButton");
    const profileButton = $("#runtimeProfileButton");
    const insetButton = $("#runtimeInsetButton");
    modeButton.title = `Mode · ${mode}`;
    modeButton.setAttribute("aria-label", `Mode: ${mode}`);
    groupsButton.title = `Groups · ${visibleGroups}/${groups.length || 0} visible`;
    groupsButton.setAttribute("aria-label", `Groups: ${visibleGroups} of ${groups.length || 0} visible`);
    profileButton.title = `Profile · ${profile?.name || "Profile"}`;
    profileButton.setAttribute("aria-label", `Profile: ${profile?.name || "Profile"}`);
    insetButton.title = `Insets · AVISO ${insetState("aviso") ? "on" : "off"}, SRW1 ${insetState("srw1") ? "on" : "off"}, SRW2 ${insetState("srw2") ? "on" : "off"}${preset ? ` · ${preset.name}` : ""}`;
    insetButton.setAttribute("aria-label", insetButton.title);
    insetButton.classList.toggle("active", anyInset);
    ["aviso", "srw1", "srw2"].forEach(kind => {
      const dot = $(`[data-inset-indicator="${kind}"]`, insetButton);
      dot?.classList.toggle("active", insetState(kind));
    });

    $$('[data-runtime-popover]').forEach(button => {
      const open = button.dataset.runtimePopover === state.ui.runtimePopover;
      button.classList.toggle("open", open);
      button.setAttribute("aria-expanded", String(open));
    });
    renderRuntimePopover();
    requestAnimationFrame(positionRuntimePopover);
    syncSurfaceVisibility();
  }

  function positionRuntimePopover() {
    const menu = $("#runtimeMenu");
    const popover = $("#runtimePopover");
    if (!menu || !popover || popover.hidden) return;
    const rect = menu.getBoundingClientRect();
    const popoverWidth = popover.offsetWidth || 276;
    menu.classList.toggle("popover-left", rect.right + 4 + popoverWidth > innerWidth - 4);
    const availableHeight = Math.max(120, innerHeight - 16);
    const popoverHeight = Math.min(popover.scrollHeight || 430, availableHeight);
    const defaultTop = 8;
    const maxTop = Math.max(0, innerHeight - 8 - rect.top - popoverHeight);
    popover.style.top = `${Math.min(defaultTop, maxTop)}px`;
  }

  function runtimeVisibilityIcon(visible) {
    return `<span class="runtime-row-icon runtime-eye-icon ${visible ? "is-on" : "is-off"}" aria-hidden="true"><svg viewBox="0 0 24 24"><path d="M2.5 12s3.6-6 9.5-6 9.5 6 9.5 6-3.6 6-9.5 6-9.5-6-9.5-6z"></path><circle cx="12" cy="12" r="2.6"></circle>${visible ? "" : '<path class="runtime-eye-slash" d="M4 4l16 16"></path>'}</svg></span>`;
  }

  function runtimeSelectionIcon(active) {
    return `<span class="runtime-row-icon runtime-radio-icon ${active ? "is-on" : ""}" aria-hidden="true"><i></i></span>`;
  }

  function renderRuntimePopover() {
    const popover = $("#runtimePopover");
    const content = $("#runtimePopoverContent");
    const title = $("#runtimePopoverTitle");
    if (!popover || !content || !title) return;
    const kind = state.ui.runtimePopover;
    popover.hidden = !kind;
    popover.dataset.kind = kind || "";
    if (!kind) { content.innerHTML = ""; return; }

    if (kind === "mode") {
      title.textContent = "Mode";
      const modes = activeProfile()?.filters?.display_modes?.items || [];
      const active = activeModeName();
      content.innerHTML = modes.length
        ? `<div class="runtime-choice-box">${modes.map(mode => {
            const selected = mode.name === active;
            return `<button type="button" class="runtime-choice-row runtime-compact-row ${selected ? "active" : ""}" data-runtime-mode="${escapeHtml(mode.name)}">${runtimeSelectionIcon(selected)}<strong class="runtime-row-label">${escapeHtml(mode.name)}</strong></button>`;
          }).join("")}</div>`
        : `<div class="runtime-popover-empty">No modes in this profile.</div>`;
      return;
    }

    if (kind === "profile") {
      title.textContent = "Profile";
      content.innerHTML = `<div class="runtime-choice-box">${state.profiles.map(record => {
        const active = record.id === state.activeProfileId;
        return `<button type="button" class="runtime-choice-row runtime-compact-row ${active ? "active" : ""}" data-runtime-profile="${escapeHtml(record.id)}">${runtimeSelectionIcon(active)}<strong class="runtime-row-label">${escapeHtml(record.data.name)}</strong></button>`;
      }).join("")}</div>`;
      return;
    }

    if (kind === "inset") {
      title.textContent = "Insets";
      const presets = avisoPresets();
      const activePreset = activeAvisoPreset();
      const store = profileAvisoPresetStore();
      const insetRows = [
        ["aviso", "AVISO inset"],
        ["srw1", "SRW 1"],
        ["srw2", "SRW 2"]
      ].map(([id, label]) => {
        const visible = insetState(id);
        return `<button type="button" class="runtime-choice-row runtime-compact-row runtime-inset-row ${visible ? "active" : ""}" data-runtime-inset="${id}">${runtimeVisibilityIcon(visible)}<strong class="runtime-row-label">${label}</strong></button>`;
      }).join("");
      const presetRows = presets.map(preset => {
        const active = preset.name === activePreset?.name;
        return `<button type="button" class="runtime-choice-row runtime-compact-row runtime-preset-row ${active ? "active" : ""}" data-inset-preset="${escapeHtml(preset.name)}">${runtimeSelectionIcon(active)}<strong class="runtime-row-label">${escapeHtml(preset.name)}</strong></button>`;
      }).join("");
      content.innerHTML = `<div class="runtime-choice-box">${insetRows}</div>
        <div class="runtime-section-heading"><span>Preset</span></div>
        ${presetRows ? `<div class="runtime-choice-box runtime-preset-list">${presetRows}</div>` : `<div class="runtime-popover-empty">No inset presets.</div>`}
        <label class="runtime-linked-toggle"><input type="checkbox" id="runtimePresetLinked" ${activePreset?.linked_movement ? "checked" : ""} ${activePreset ? "" : "disabled"}><span>Linked movement</span></label>
        <div class="runtime-preset-actions">
          <button class="ui-button primary" data-action="save-inset-preset" type="button">Save…</button>
          <button class="ui-button" data-action="update-inset-preset" type="button" ${activePreset ? "" : "disabled"}>Update</button>
          <button class="ui-button" data-action="reset-inset-preset" type="button" ${activePreset ? "" : "disabled"}>Reset</button>
          <button class="ui-button" data-action="rename-inset-preset" type="button" ${activePreset ? "" : "disabled"}>Rename…</button>
          <button class="ui-button" data-action="duplicate-inset-preset" type="button" ${activePreset ? "" : "disabled"}>Duplicate</button>
          <button class="ui-button" data-action="default-inset-preset" type="button" ${activePreset ? "" : "disabled"}>${activePreset?.name === store.default ? "Default ✓" : "Set default"}</button>
          <button class="ui-button danger runtime-preset-delete" data-action="delete-inset-preset" type="button" ${activePreset ? "" : "disabled"}>Delete</button>
        </div>`;
      return;
    }

    title.textContent = "Groups";
    const groupRows = avisoGroups().map(group => {
      const visible = group.visible !== false;
      return `<button type="button" class="runtime-choice-row runtime-compact-row ${visible ? "" : "muted"}" data-runtime-group="${escapeHtml(group.id)}">${runtimeVisibilityIcon(visible)}<strong class="runtime-row-label">${escapeHtml(group.name)}</strong></button>`;
    }).join("");
    content.innerHTML = groupRows
      ? `<div class="runtime-choice-box">${groupRows}</div>`
      : `<div class="runtime-popover-empty">No AVISO groups.</div>`;
  }

  function setRuntimeMode(modeName) {
    const displayModes = activeProfile()?.filters?.display_modes;
    if (!displayModes?.items?.some(mode => mode.name === modeName)) return;
    displayModes.active = modeName;
    markDirty(`${modeName} set active`);
    state.ui.selectedModeIndex = Math.max(0, displayModes.items.findIndex(mode => mode.name === modeName));
    state.ui.runtimePopover = "";
    renderModes();
    renderRuntimeMenu();
    postBridge("runtime.mode.change", { profile: activeProfile().name, mode: modeName });
    showToast(`Mode: ${modeName}`, "success");
  }

  function toggleRuntimeGroup(groupId) {
    const group = avisoGroups().find(item => item.id === groupId);
    if (!group) return;
    group.visible = group.visible === false;
    if (drafts.avisoGroup?.id === group.id) drafts.avisoGroup.data.visible = group.visible;
    markDirty(`Group ${group.visible ? "shown" : "hidden"}`);
    postBridge("aviso.group.visibility", { id: group.id, name: group.name, visible: group.visible });
    renderRuntimeMenu();
    if (state.ui.page === "groups") renderAvisoGroups();
  }

  function toggleInsetWindow(kind) {
    if (!["aviso", "srw1", "srw2"].includes(kind)) return;
    state.runtime.insets ||= { aviso: false, srw1: false, srw2: false };
    state.runtime.insets[kind] = !state.runtime.insets[kind];
    if (kind === "aviso") state.runtime.avisoInsetVisible = state.runtime.insets[kind];
    const preset = activeAvisoPreset();
    const action = kind === "aviso" ? "aviso.inset.toggle" : "display.srw.toggle";
    postBridge(action, { airport: activePresetAirport(), window: kind, visible: state.runtime.insets[kind], preset: preset?.name || "", profile: activeProfile().name });
    renderRuntimeMenu();
    showToast(`${kind === "aviso" ? "AVISO inset" : kind.toUpperCase()} ${state.runtime.insets[kind] ? "shown" : "hidden"}`, "success");
  }

  function loadAvisoPreset(name) {
    const preset = avisoPresets().find(item => item.name === name);
    if (!preset) return;
    state.runtime.activeAvisoPreset = preset.name;
    state.runtime.activeAvisoPresetScope = activePresetScope();
    state.runtime.avisoInsetSnapshot = clone(preset);
    postBridge("aviso.inset.preset.load", { airport: activePresetAirport(), profile: activeProfile().name, preset: clone(preset) });
    renderRuntimeMenu();
    showToast(`Inset preset: ${preset.name}`, "success");
  }

  function openInsetPresetDialog(mode = "capture") {
    const dialog = $("#insetPresetDialog");
    const current = activeAvisoPreset();
    insetPresetDialogMode = mode;
    $("#insetPresetDialogTitle").textContent = mode === "rename" ? "Rename inset preset" : "Save inset preset";
    $("#insetPresetConfirm").textContent = mode === "rename" ? "Rename" : "Save";
    $("#insetPresetName").value = mode === "rename" ? (current?.name || "") : uniqueInsetPresetName("New preset");
    $("#insetPresetLinked").checked = Boolean(current?.linked_movement);
    dialog.showModal();
    requestAnimationFrame(() => $("#insetPresetName").select());
  }

  function uniqueInsetPresetName(base) {
    const names = new Set(avisoPresets().map(item => item.name.toLowerCase()));
    let name = base;
    let n = 2;
    while (names.has(name.toLowerCase())) name = `${base} ${n++}`;
    return name;
  }

  function confirmInsetPresetDialog() {
    const dialog = $("#insetPresetDialog");
    const name = String($("#insetPresetName").value || "").trim();
    if (!name) { showToast("Enter a preset name", "error"); return; }
    const linked = $("#insetPresetLinked").checked;
    const store = profileAvisoPresetStore();
    const current = activeAvisoPreset();
    if (insetPresetDialogMode === "rename") {
      if (!current) return;
      if (store.items.some(item => item !== current && item.name.toLowerCase() === name.toLowerCase())) { showToast("A preset with this name already exists", "error"); return; }
      const oldName = current.name;
      postBridge("aviso.inset.preset.rename", { airport: activePresetAirport(), profile: activeProfile().name, oldName, name, linked_movement: linked });
    } else {
      if (store.items.some(item => item.name.toLowerCase() === name.toLowerCase())) { showToast("A preset with this name already exists", "error"); return; }
      postBridge("aviso.inset.preset.capture", {
        airport: activePresetAirport(),
        profile: activeProfile().name,
        preset: { name, linked_movement: linked }
      });
    }
    dialog.close();
    renderRuntimeMenu();
  }

  function updateAvisoPreset() {
    const current = activeAvisoPreset();
    if (!current) return;
    postBridge("aviso.inset.preset.update", { airport: activePresetAirport(), profile: activeProfile().name, preset: clone(current) });
    renderRuntimeMenu();
  }

  function resetAvisoPreset() {
    const current = activeAvisoPreset();
    if (!current) return;
    postBridge("aviso.inset.preset.reset", { airport: activePresetAirport(), profile: activeProfile().name, preset: current.name });
    renderRuntimeMenu();
  }

  function duplicateAvisoPreset() {
    const current = activeAvisoPreset();
    if (!current) return;
    const name = uniqueInsetPresetName(`${current.name} copy`);
    postBridge("aviso.inset.preset.duplicate", {
      airport: activePresetAirport(),
      profile: activeProfile().name,
      source: current.name,
      preset: { name }
    });
    renderRuntimeMenu();
  }

  function setDefaultAvisoPreset() {
    const current = activeAvisoPreset();
    if (!current) return;
    postBridge("aviso.inset.preset.default", { airport: activePresetAirport(), profile: activeProfile().name, preset: current.name });
    renderRuntimeMenu();
  }

  function deleteAvisoPreset() {
    const current = activeAvisoPreset();
    if (!current || !confirmDelete(`Delete the inset preset “${current.name}”?`)) return;
    postBridge("aviso.inset.preset.delete", { airport: activePresetAirport(), profile: activeProfile().name, preset: current.name });
    renderRuntimeMenu();
  }

  function toggleRuntimePresetLinked(checked) {
    const current = activeAvisoPreset();
    if (!current) return;
    postBridge("aviso.inset.preset.linked", {
      airport: activePresetAirport(),
      profile: activeProfile().name,
      preset: current.name,
      linked_movement: Boolean(checked)
    });
    renderRuntimeMenu();
  }

  function toggleRuntimeInset() { toggleInsetWindow("aviso"); }

  function collectProfileColors(profile) {
    const entries = [];
    const roots = ["labels", "rimcas", "targets", "approach_insets"];
    function visit(value, path) {
      if (isColorObject(value)) {
        const groupInfo = colorGroupInfo(path);
        entries.push({
          id: path.join("."),
          path,
          group: groupInfo.group,
          family: groupInfo.family,
          section: groupInfo.section,
          name: humanize(path[path.length - 1]),
          key: String(path[path.length - 1]),
          color: value
        });
        return;
      }
      if (!value || typeof value !== "object") return;
      if (Array.isArray(value)) value.forEach((item, index) => visit(item, [...path, index]));
      else Object.entries(value).forEach(([key, child]) => visit(child, [...path, key]));
    }
    roots.forEach(root => visit(profile?.[root], [root]));
    return entries.sort((a, b) => {
      const familyOrder = value => {
        const index = COLOR_FAMILY_ORDER.indexOf(value);
        return index < 0 ? 999 : index;
      };
      const sectionOrder = value => {
        const index = COLOR_SECTION_ORDER.indexOf(value || "");
        return index < 0 ? 999 : index;
      };
      return familyOrder(a.family) - familyOrder(b.family)
        || sectionOrder(a.section) - sectionOrder(b.section)
        || a.family.localeCompare(b.family)
        || a.section.localeCompare(b.section)
        || a.name.localeCompare(b.name);
    });
  }

  function colorGroupInfo(path) {
    const [root, second] = path;
    if (root === "labels") {
      const section = second && TAG_SCOPES.includes(second) ? humanize(second) : "General";
      return { family: "Tags", section, group: `Tags · ${section}` };
    }
    if (root === "targets") {
      const section = second && ["departure", "arrival"].includes(second) ? humanize(second) : "General";
      return { family: "Targets", section, group: `Targets · ${section}` };
    }
    if (root === "rimcas") return { family: "RIMCAS", section: "", group: "RIMCAS" };
    if (root === "approach_insets") return { family: "Approach inset", section: "", group: "Approach inset" };
    const family = humanize(root);
    return { family, section: "", group: family };
  }

  function selectedColorEntry() {
    const entries = collectProfileColors(activeProfile());
    return entries.find(entry => entry.id === state.ui.selectedColorPath) || entries[0];
  }

  function renderColors() {
    const entries = collectProfileColors(activeProfile());
    const search = $("#colorSearch").value.trim().toLowerCase();
    const searching = Boolean(search);
    const filtered = entries.filter(entry => !search || `${entry.family} ${entry.section} ${entry.name} ${entry.key} ${entry.id}`.toLowerCase().includes(search));
    $("#colorCountCaption").textContent = `(${entries.length})`;

    const groups = new Map();
    filtered.forEach(entry => {
      const caption = entry.section ? `${entry.family} · ${entry.section}` : entry.family;
      const key = `${entry.family}:${entry.section || "general"}`;
      if (!groups.has(key)) groups.set(key, { key, caption, family: entry.family, section: entry.section, items: [] });
      groups.get(key).items.push(entry);
    });

    $("#colorTree").innerHTML = [...groups.values()].map(group => {
      const groupKey = `colors:${group.key}`;
      const collapsed = !searching && treeState.colors.has(groupKey);
      const accent = colorToHex(group.items[0]?.color, "#5096b4");
      const rows = group.items.map(entry => {
        const hex = colorToHex(entry.color).toUpperCase();
        const opacity = Math.round((entry.color.a ?? 255) / 255 * 100);
        const opacityText = opacity === 100 ? "" : ` · ${opacity}%`;
        return `<button type="button" class="menu-tree-row color-menu-row ${entry.id === state.ui.selectedColorPath ? "active" : ""}" data-color-path="${escapeHtml(entry.id)}" style="--node-color:${hex}" title="${escapeHtml(`${entry.id} · ${hex}${opacityText}`)}">
          <span class="menu-row-swatch tree-color-swatch" aria-hidden="true"></span>
          <span class="menu-row-title">${escapeHtml(entry.name)}</span>
          <span class="menu-row-meta"><code>${hex}</code>${opacity === 100 ? "" : `<small>${opacity}%</small>`}</span>
        </button>`;
      }).join("");
      return `<section class="menu-tree-section color-menu-section" style="--menu-accent:${accent}">
        <button type="button" class="menu-tree-caption" data-tree-toggle="colors" data-tree-key="${escapeHtml(groupKey)}" aria-expanded="${!collapsed}">
          <span class="menu-tree-caret" aria-hidden="true">${collapsed ? "▸" : "▾"}</span>
          <span class="menu-tree-caption-text">${escapeHtml(group.caption)}</span>
          <span class="menu-tree-count">${group.items.length}</span>
        </button>
        <div class="menu-tree-box" ${collapsed ? "hidden" : ""}>${rows}</div>
      </section>`;
    }).join("") || `<div class="aviso-list-message">No colors found</div>`;
    requestAnimationFrame(() => $("#colorTree .color-menu-row.active")?.scrollIntoView({ block: "nearest" }));
    renderColorEditor();
  }

  function rgbToHsv(r, g, b) {
    r = clamp(r, 0, 255) / 255;
    g = clamp(g, 0, 255) / 255;
    b = clamp(b, 0, 255) / 255;
    const max = Math.max(r, g, b);
    const min = Math.min(r, g, b);
    const delta = max - min;
    let h = 0;
    if (delta) {
      if (max === r) h = 60 * (((g - b) / delta) % 6);
      else if (max === g) h = 60 * ((b - r) / delta + 2);
      else h = 60 * ((r - g) / delta + 4);
    }
    if (h < 0) h += 360;
    return { h, s: max === 0 ? 0 : delta / max, v: max };
  }

  function hsvToRgb(h, s, v) {
    h = ((Number(h) % 360) + 360) % 360;
    s = clamp(s, 0, 1);
    v = clamp(v, 0, 1);
    const c = v * s;
    const x = c * (1 - Math.abs((h / 60) % 2 - 1));
    const m = v - c;
    let rp = 0, gp = 0, bp = 0;
    if (h < 60) [rp, gp, bp] = [c, x, 0];
    else if (h < 120) [rp, gp, bp] = [x, c, 0];
    else if (h < 180) [rp, gp, bp] = [0, c, x];
    else if (h < 240) [rp, gp, bp] = [0, x, c];
    else if (h < 300) [rp, gp, bp] = [x, 0, c];
    else [rp, gp, bp] = [c, 0, x];
    return {
      r: Math.round((rp + m) * 255),
      g: Math.round((gp + m) * 255),
      b: Math.round((bp + m) * 255)
    };
  }

  function setColorDraftFromRgb(r, g, b) {
    if (!drafts.color) return;
    const rgb = { r: Math.round(clamp(r, 0, 255)), g: Math.round(clamp(g, 0, 255)), b: Math.round(clamp(b, 0, 255)) };
    const hsv = rgbToHsv(rgb.r, rgb.g, rgb.b);
    drafts.color.hex = colorToHex(rgb);
    drafts.color.h = hsv.h;
    drafts.color.s = hsv.s;
    drafts.color.v = hsv.v;
    syncColorEditorControls();
  }

  function setColorDraftFromHex(value) {
    if (!drafts.color) return;
    const normalized = normalizeHex(value, drafts.color.hex || "#ffffff");
    const rgb = hexToColor(normalized);
    const hsv = rgbToHsv(rgb.r, rgb.g, rgb.b);
    drafts.color.hex = normalized;
    drafts.color.h = hsv.h;
    drafts.color.s = hsv.s;
    drafts.color.v = hsv.v;
    syncColorEditorControls();
  }

  function setColorDraftFromHsv(h, saturation, value) {
    if (!drafts.color) return;
    drafts.color.h = ((Number(h) % 360) + 360) % 360;
    drafts.color.s = clamp(saturation, 0, 1);
    drafts.color.v = clamp(value, 0, 1);
    const rgb = hsvToRgb(drafts.color.h, drafts.color.s, drafts.color.v);
    drafts.color.hex = colorToHex(rgb);
    syncColorEditorControls();
  }

  function syncColorEditorControls() {
    if (!drafts.color) return;
    const rgb = hexToColor(drafts.color.hex);
    const hsv = Number.isFinite(drafts.color.h) ? drafts.color : rgbToHsv(rgb.r, rgb.g, rgb.b);
    drafts.color.h = hsv.h;
    drafts.color.s = hsv.s;
    drafts.color.v = hsv.v;
    const hex = normalizeHex(drafts.color.hex);
    const opacity = Math.round(clamp(drafts.color.opacity, 0, 100));

    $("#colorHex").value = hex.toUpperCase();
    $("#nativeColorPicker").value = hex;
    $("#colorHue").value = Math.round(drafts.color.h);
    $("#colorHueOutput").value = `${Math.round(drafts.color.h)}°`;
    $("#colorRed").value = rgb.r;
    $("#colorGreen").value = rgb.g;
    $("#colorBlue").value = rgb.b;
    $("#colorRedOutput").value = rgb.r;
    $("#colorGreenOutput").value = rgb.g;
    $("#colorBlueOutput").value = rgb.b;
    $("#colorOpacity").value = opacity;
    $("#colorOpacityOutput").value = `${opacity}%`;

    const palette = $("#colorSvPalette");
    palette.style.setProperty("--palette-hue", String(Math.round(drafts.color.h)));
    const cursor = $("#colorPaletteCursor");
    cursor.style.left = `${drafts.color.s * 100}%`;
    cursor.style.top = `${(1 - drafts.color.v) * 100}%`;
    const swatch = $("#colorSwatch");
    swatch.style.setProperty("--swatch-color", `rgba(${rgb.r}, ${rgb.g}, ${rgb.b}, ${opacity / 100})`);
    swatch.style.setProperty("--swatch-solid", hex);
  }

  function updateColorFromPalettePointer(event) {
    if (!drafts.color) return;
    const palette = $("#colorSvPalette");
    const rect = palette.getBoundingClientRect();
    const saturation = clamp((event.clientX - rect.left) / Math.max(1, rect.width), 0, 1);
    const value = 1 - clamp((event.clientY - rect.top) / Math.max(1, rect.height), 0, 1);
    setColorDraftFromHsv(drafts.color.h, saturation, value);
  }

  function renderColorEditor() {
    const entry = selectedColorEntry();
    if (!entry) return;
    if (!drafts.color || drafts.color.path !== entry.id) {
      const hex = colorToHex(entry.color);
      const rgb = hexToColor(hex);
      const hsv = rgbToHsv(rgb.r, rgb.g, rgb.b);
      drafts.color = {
        path: entry.id,
        hex,
        opacity: Math.round((entry.color.a ?? 255) / 255 * 100),
        h: hsv.h,
        s: hsv.s,
        v: hsv.v
      };
    }
    $("#selectedColorPath").textContent = entry.name;
    $("#selectedColorGroup").textContent = entry.group;
    $("#colorJsonPath").value = entry.id;
    syncColorEditorControls();
  }
  function applyColorDraft() {
    const entry = selectedColorEntry();
    if (!entry || !drafts.color) return;
    const hadAlpha = Object.prototype.hasOwnProperty.call(entry.color, "a");
    const next = hexToColor(drafts.color.hex, drafts.color.opacity / 100 * 255);
    if (!hadAlpha && Number(drafts.color.opacity) === 100) delete next.a;
    setAtPath(activeProfile(), entry.path, next);
    drafts.color = null;
    markDirty(`${entry.name} updated`);
    renderColors();
  }

  function resetSelectedColor() {
    const entry = selectedColorEntry();
    const record = activeProfileRecord();
    if (!entry || !record) return;
    const original = getAtPath(record.original, entry.path);
    if (!isColorObject(original)) return;
    setAtPath(record.data, entry.path, clone(original));
    drafts.color = null;
    markDirty(`${entry.name} reset`);
    renderColors();
  }

  function renderIconSymbolPreview() {
    const preview = $("#iconSymbolPreview");
    if (!preview) return;
    const style = String($("#targetIconStyle")?.value || activeProfile().targets?.icon_style || "realistic").toLowerCase();
    const showPrimary = $("#showPrimaryTarget")?.checked ?? true;
    const fixed = $("#fixedPixelIconSize")?.checked ?? false;
    const boost = $("#smallIconBoost")?.checked ?? false;
    const fixedScale = Number($("#fixedPixelIconScale")?.value || 1);
    const boostFactor = Number($("#smallIconBoostFactor")?.value || 1);
    const resolution = state.settings.resolutionPreset || activeProfile().targets?.small_icon_boost_resolution_preset || "1080p";
    const scale = (fixed ? fixedScale : 1) * (boost ? Math.max(1, boostFactor) : 1);

    let symbol = "";
    if (style === "nova") {
      symbol = `<circle cx="0" cy="0" r="11"/><path d="M-19 0H19M0-19V19"/><circle class="icon-preview-fill" cx="0" cy="0" r="3"/>`;
    } else if (style === "triangle" || style === "arrow") {
      symbol = `<path d="M0-17 14 14 0 9-14 14Z"/><path d="M0-12V11"/>`;
    } else if (style === "diamond") {
      symbol = `<path d="M0-16 16 0 0 16-16 0Z"/><circle class="icon-preview-fill" cx="0" cy="0" r="2.5"/>`;
    } else {
      symbol = `<path d="M0-20 4-5 19 1 19 5 4 2 3 16 9 20 9 23 0 20-9 23-9 20-3 16-4 2-19 5-19 1-4-5Z"/>`;
    }

    preview.innerHTML = `<svg viewBox="0 0 104 76" aria-hidden="true">
      <g class="icon-preview-grid"><path d="M8 38H96M52 6V70"/><circle cx="52" cy="38" r="27"/></g>
      <g class="icon-preview-symbol ${showPrimary ? "with-primary" : "without-primary"}" transform="translate(52 38) scale(${Math.min(1.45, Math.max(.68, scale)).toFixed(2)})">${symbol}</g>
    </svg><span>${escapeHtml(style)} · ${escapeHtml(resolution)}</span>`;
  }

  function renderIcons() {
    const profile = activeProfile();
    const targets = profile.targets ||= {};
    $("#iconProfileCaption").textContent = profile.name || "";
    ensureSelectValue($("#targetIconStyle"), targets.icon_style || "realistic");
    $("#showPrimaryTarget").checked = targets.show_primary_target !== false;
    $("#fixedPixelIconSize").checked = Boolean(targets.fixed_pixel_icon_size);
    $("#fixedPixelIconScale").value = targets.fixed_pixel_icon_scale ?? 1;
    $("#fixedPixelIconScaleOutput").value = `${Number(targets.fixed_pixel_icon_scale ?? 1).toFixed(2)}×`;
    $("#smallIconBoost").checked = Boolean(targets.small_icon_boost);
    targets.small_icon_boost_resolution_preset ||= state.settings.resolutionPreset || "1080p";
    $("#smallIconBoostFactor").value = targets.small_icon_boost_factor ?? 1;
    $("#smallIconBoostFactorOutput").value = `${Number(targets.small_icon_boost_factor ?? 1).toFixed(2)}×`;
    updateIconDependencies();
    renderIconSymbolPreview();
  }
  function updateIconDependencies() {
    const fixedEnabled = $("#fixedPixelIconSize").checked;
    const boostEnabled = $("#smallIconBoost").checked;
    $("#fixedPixelIconScale").disabled = !fixedEnabled;
    $("#smallIconBoostFactor").disabled = !boostEnabled;
    // Resolution is a global display setting and is edited on the Settings page.
    $("#fixedPixelIconSize").closest(".icon-parameter-block")?.classList.toggle("is-disabled", !fixedEnabled);
    $("#smallIconBoost").closest(".icon-parameter-block")?.classList.toggle("is-disabled", !boostEnabled);
    renderIconSymbolPreview();
  }
  function applyIcons() {
    const targets = activeProfile().targets ||= {};
    targets.icon_style = $("#targetIconStyle").value;
    targets.show_primary_target = $("#showPrimaryTarget").checked;
    targets.fixed_pixel_icon_size = $("#fixedPixelIconSize").checked;
    targets.fixed_pixel_icon_scale = Number($("#fixedPixelIconScale").value);
    targets.small_icon_boost = $("#smallIconBoost").checked;
    targets.small_icon_boost_resolution_preset = state.settings.resolutionPreset || targets.small_icon_boost_resolution_preset || "1080p";
    targets.small_icon_boost_factor = Number($("#smallIconBoostFactor").value);
    markDirty("Target icon settings updated");
    renderIcons();
  }

  function tagDefinitionColor(profile, scope, status) {
    const labels = profile.labels || {};
    if (!scope) return null;
    if (scope === "airborne") {
      const sourceScope = String(status || "").includes("arr") ? "arrival" : "departure";
      const key = String(status || "").includes("onrunway") ? "background_on_runway_color" : "background_airborne_color";
      const color = labels[sourceScope]?.[key];
      return isColorObject(color) ? color : null;
    }
    const key = TAG_STATUS_COLOR_KEYS[scope]?.[status || "default"];
    const color = key ? labels[scope]?.[key] : null;
    if (isColorObject(color)) return color;
    const fallbacks = ["background_no_status_color", "background_on_ground_color", "background_airborne_color", "text_color"];
    for (const fallback of fallbacks) {
      if (isColorObject(labels[scope]?.[fallback])) return labels[scope][fallback];
    }
    return null;
  }

  function tagDefinitions(profile = activeProfile()) {
    const labels = profile.labels || {};
    const result = [];
    TAG_SCOPES.forEach(scope => {
      const definition = labels[scope];
      if (!definition) return;
      result.push({
        id: `${scope}:default`, group: humanize(scope), label: "Default", scope, status: "default",
        target: definition, color: tagDefinitionColor(profile, scope, "default")
      });
      Object.entries(definition.status_definitions || {}).forEach(([status, target]) => {
        result.push({
          id: `${scope}:${status}`, group: humanize(scope), label: TAG_STATUS_LABELS[status] || humanize(status),
          scope, status, target, color: tagDefinitionColor(profile, scope, status)
        });
      });
    });
    return result;
  }
  function selectedTagDefinition() {
    const definitions = tagDefinitions();
    const selected = definitions.find(entry => entry.id === state.ui.selectedTagId) || definitions[0];
    if (selected && selected.id !== state.ui.selectedTagId) state.ui.selectedTagId = selected.id;
    return selected;
  }

  function renderTags() {
    const definitions = tagDefinitions();
    $("#tagCountCaption").textContent = `(${definitions.length})`;
    const groups = new Map();
    definitions.forEach(entry => {
      if (!groups.has(entry.group)) groups.set(entry.group, []);
      groups.get(entry.group).push(entry);
    });

    $("#tagDefinitionList").innerHTML = [...groups.entries()].map(([group, items]) => {
      const groupKey = `tags:${group}`;
      const collapsed = treeState.tags.has(groupKey);
      const accentEntry = items.find(item => item.color);
      const accent = accentEntry ? colorToHex(accentEntry.color) : "#5096b4";
      const rows = items.map(entry => `<button type="button" class="menu-tree-row tag-menu-row ${entry.id === state.ui.selectedTagId ? "active" : ""}" data-tag-id="${escapeHtml(entry.id)}" title="${escapeHtml(entry.label)}">
        <span class="menu-row-title">${escapeHtml(entry.label)}</span>
      </button>`).join("");
      return `<section class="menu-tree-section tag-menu-section" style="--menu-accent:${accent}">
        <button type="button" class="menu-tree-caption" data-tree-toggle="tags" data-tree-key="${escapeHtml(groupKey)}" aria-expanded="${!collapsed}">
          <span class="menu-tree-caret" aria-hidden="true">${collapsed ? "▸" : "▾"}</span>
          <span class="menu-tree-caption-text">${escapeHtml(group)}</span>
          <span class="menu-tree-count">${items.length}</span>
        </button>
        <div class="menu-tree-box" ${collapsed ? "hidden" : ""}>${rows}</div>
      </section>`;
    }).join("") || `<div class="aviso-list-message">No tag definitions</div>`;
    requestAnimationFrame(() => $("#tagDefinitionList .tag-menu-row.active")?.scrollIntoView({ block: "nearest" }));
    renderTagEditor();
  }

  function renderTagEditor() {
    const entry = selectedTagDefinition();
    if (!entry) return;
    $("#tagEditorCaption").textContent = entry.label;
    $("#tagEditorScope").textContent = entry.group;
    if (!drafts.tag || drafts.tag.id !== entry.id) drafts.tag = { id: entry.id, data: clone(entry.target) };
    const data = drafts.tag.data;
    const inherits = Boolean(data.definition_detailed_inherits_normal);
    $("#tagDetailedInherits").checked = inherits;
    const normal = data.definition || [];
    const detailed = data.definition_detailed || [];
    const rowCount = Math.max(3, normal.length, detailed.length);
    $("#tagLineGrid").innerHTML = Array.from({ length: rowCount }, (_, index) => `
      <div class="tag-line-row">
        <span>L${index + 1}</span>
        <input class="tag-line-input" data-kind="normal" data-line="${index}" type="text" value="${escapeHtml((normal[index] || []).join(" "))}" spellcheck="false">
        <input class="tag-line-input" data-kind="detailed" data-line="${index}" type="text" value="${escapeHtml((detailed[index] || []).join(" "))}" spellcheck="false" ${inherits ? "disabled" : ""}>
      </div>`).join("");
    const tokenSelect = $("#tagTokenSelect");
    tokenSelect.innerHTML = TAG_TOKENS.map(token => `<option value="${token}">${token}</option>`).join("");

    const labels = activeProfile().labels ||= {};
    $("#tagRoundedCorners").checked = Boolean(labels.rounded_corners);
    $("#tagAutoDeconfliction").checked = Boolean(labels.auto_deconfliction);
    $("#tagUseSpeedGate").checked = Boolean(labels.use_speed_for_gate);
    $("#tagDepartureArrivalColors").checked = Boolean(labels.use_departure_arrival_coloring);
    $("#tagLeaderLineLength").value = labels.leader_line_length ?? 0;
    const labelSize = Math.round(clamp(activeProfile().font?.label_font_size ?? 1, 1, 5));
    $("#tagLabelFontSize").value = labelSize;
    if ($("#tagLabelFontSizeOutput")) $("#tagLabelFontSizeOutput").value = String(labelSize);

    const profile = activeProfile();
    profile.font ||= {};
    const fontSelect = $("#profileFontName");
    const fonts = [...new Set([...(profile.font.available_fonts || []), profile.font.font_name || "Arial"].filter(Boolean))];
    fontSelect.innerHTML = fonts.map(font => `<option>${escapeHtml(font)}</option>`).join("");
    fontSelect.value = profile.font.font_name || fonts[0] || "Arial";
    ensureSelectValue($("#profileFontWeight"), profile.font.weight || "Regular");
    profile.font.sizes ||= { one: 10, two: 11, three: 12, four: 13, five: 14 };
  }

  function captureTagDraft() {
    const entry = selectedTagDefinition();
    if (!entry || entry.id === "options") return;
    const data = drafts.tag?.data || clone(entry.target);
    const rows = $$("#tagLineGrid .tag-line-row");
    const parse = input => String(input.value || "").trim().split(/[\s,]+/).filter(Boolean);
    data.definition = rows.map(row => parse($("input[data-kind='normal']", row))).filter(line => line.length);
    data.definition_detailed = $("#tagDetailedInherits").checked
      ? clone(data.definition)
      : rows.map(row => parse($("input[data-kind='detailed']", row))).filter(line => line.length);
    data.definition_detailed_inherits_normal = $("#tagDetailedInherits").checked;
    drafts.tag = { id: entry.id, data };
  }

  function applyTag() {
    const entry = selectedTagDefinition();
    if (!entry) return;
    captureTagDraft();
    Object.keys(entry.target).forEach(key => delete entry.target[key]);
    Object.assign(entry.target, clone(drafts.tag.data));

    const labels = activeProfile().labels ||= {};
    labels.rounded_corners = $("#tagRoundedCorners").checked;
    labels.auto_deconfliction = $("#tagAutoDeconfliction").checked;
    labels.use_speed_for_gate = $("#tagUseSpeedGate").checked;
    labels.use_departure_arrival_coloring = $("#tagDepartureArrivalColors").checked;
    labels.leader_line_length = Math.round(Number($("#tagLeaderLineLength").value) || 0);
    activeProfile().font ||= {};
    activeProfile().font.label_font_size = Math.round(clamp($("#tagLabelFontSize").value, 1, 5));
    activeProfile().font.font_name = $("#profileFontName").value || "Arial";
    activeProfile().font.weight = $("#profileFontWeight").value || "Regular";
    activeProfile().font.sizes ||= { one: 10, two: 11, three: 12, four: 13, five: 14 };

    drafts.tag = null;
    markDirty(`${entry.label} updated`);
    renderTags();
  }

  function rules() {
    activeProfile().rules ||= { version: 1, items: [] };
    activeProfile().rules.items ||= [];
    return activeProfile().rules.items;
  }

  function ruleLabel(rule, index) {
    return String(rule?.name || "").trim() || `Rule ${index + 1}`;
  }
  function selectedRuleStatuses(rule) {
    const valid = new Set(RULE_STATUSES);
    let statuses = Array.isArray(rule?.statuses) ? rule.statuses.filter(status => valid.has(status)) : [];
    if (!statuses.length) {
      const legacy = String(rule?.status || "any");
      if (!legacy || legacy === "any") statuses = RULE_STATUSES.slice();
      else statuses = legacy.split(/[\s,;|]+/).filter(status => valid.has(status));
    }
    return statuses.length ? uniqueValues(statuses) : RULE_STATUSES.slice();
  }

  function checkedRuleStatuses() {
    const checked = $$("#ruleStatusOptions input[data-rule-status]:checked").map(input => input.dataset.ruleStatus);
    return checked.length ? checked : RULE_STATUSES.slice();
  }

  function updateRuleStatusDropdownLabel() {
    const button = $("#ruleStatusButton");
    const all = $("#ruleStatusAll");
    const options = $$("#ruleStatusOptions input[data-rule-status]");
    const selected = options.filter(input => input.checked);
    all.checked = selected.length === options.length && options.length > 0;
    all.indeterminate = selected.length > 0 && selected.length < options.length;
    if (!selected.length || selected.length === options.length) button.textContent = "All statuses";
    else if (selected.length === 1) button.textContent = humanize(selected[0].dataset.ruleStatus);
    else button.textContent = `${selected.length} statuses`;
    button.title = selected.length === options.length ? "All statuses selected" : selected.map(input => humanize(input.dataset.ruleStatus)).join(", ");
  }

  function renderRuleStatusSelector(rule, disabled = false) {
    const selected = new Set(selectedRuleStatuses(rule));
    $("#ruleStatusOptions").innerHTML = RULE_STATUSES.map(status => `<label role="option" aria-selected="${selected.has(status)}"><input type="checkbox" data-rule-status="${status}" ${selected.has(status) ? "checked" : ""}><span>${escapeHtml(humanize(status))}</span></label>`).join("");
    $("#ruleStatusButton").disabled = disabled;
    $("#ruleStatusAll").disabled = disabled;
    $$("#ruleStatusOptions input").forEach(input => { input.disabled = disabled; });
    updateRuleStatusDropdownLabel();
  }

  function setRuleStatusMenuOpen(open) {
    const menu = $("#ruleStatusMenu");
    const button = $("#ruleStatusButton");
    if (!menu || !button) return;
    menu.hidden = !open;
    button.setAttribute("aria-expanded", String(open));
    $("#ruleStatusDropdown")?.classList.toggle("open", open);
  }

  function renderRules() {
    const items = rules();
    state.ui.selectedRuleIndex = items.length ? Math.min(items.length - 1, Math.max(0, state.ui.selectedRuleIndex)) : 0;
    $("#ruleCountCaption").textContent = `(${items.length})`;
    $("#ruleList").innerHTML = items.map((rule, index) => `<button type="button" class="selection-row ${index === state.ui.selectedRuleIndex ? "active" : ""}" data-rule-index="${index}"><span>${escapeHtml(ruleLabel(rule, index))}</span></button>`).join("") || `<div class="aviso-list-message">No rules</div>`;
    renderRuleEditor();
  }
  function renderRuleEditor() {
    const item = rules()[state.ui.selectedRuleIndex];
    const disabled = !item;
    $("#ruleFormCaption").textContent = item ? ruleLabel(item, state.ui.selectedRuleIndex) : "Rule";
    if (!item) {
      $("#ruleName").value = "";
      $("#criteriaList").innerHTML = "";
      renderRuleStatusSelector({ status: "any" }, true);
      return;
    }
    if (!drafts.rule || drafts.rule.index !== state.ui.selectedRuleIndex) drafts.rule = { index: state.ui.selectedRuleIndex, data: clone(item) };
    const rule = drafts.rule.data;
    $("#ruleName").value = rule.name || "";
    const criteria = Array.isArray(rule.criteria) && rule.criteria.length ? rule.criteria : [{ source: rule.source || "vacdm", token: rule.token || "", condition: rule.condition || "" }];
    $("#criteriaList").innerHTML = criteria.map((criterion, index) => `
      <div class="criterion-row" data-criterion-index="${index}">
        <select data-field="source">${RULE_SOURCES.map(source => `<option ${source === criterion.source ? "selected" : ""}>${source}</option>`).join("")}</select>
        <input data-field="token" type="text" value="${escapeHtml(criterion.token || "")}" placeholder="token">
        <input data-field="condition" type="text" value="${escapeHtml(criterion.condition || "")}" placeholder="condition">
        <button type="button" data-action="delete-condition" data-index="${index}">×</button>
      </div>`).join("");
    ensureSelectValue($("#ruleTagType"), rule.tag_type || "any");
    renderRuleStatusSelector(rule, disabled);
    ensureSelectValue($("#ruleDetail"), rule.detail || "any");
    setRuleColorControls("Target", rule.target_color);
    setRuleColorControls("Tag", rule.tag_color);
    setRuleColorControls("Text", rule.text_color);
    ["ruleName", "ruleTagType", "ruleDetail"].forEach(id => $("#" + id).disabled = disabled);
  }
  function setRuleColorControls(kind, color) {
    const checkbox = $(`#ruleUse${kind}Color`);
    const text = $(`#rule${kind}Color`);
    const picker = $(`#rule${kind}Picker`);
    const swatch = picker.closest("label");
    checkbox.checked = isColorObject(color);
    const hex = colorToHex(color, "#ffffff");
    text.value = hex.toUpperCase();
    text.disabled = !checkbox.checked;
    picker.value = hex;
    picker.disabled = !checkbox.checked;
    swatch.style.setProperty("--swatch-color", hex);
  }

  function captureRuleDraft() {
    if (!drafts.rule) return null;
    const rule = drafts.rule.data;
    const criteria = $$("#criteriaList .criterion-row").map(row => ({
      source: $("[data-field='source']", row).value,
      token: $("[data-field='token']", row).value.trim(),
      condition: $("[data-field='condition']", row).value.trim()
    })).filter(criterion => criterion.source || criterion.token || criterion.condition);
    rule.criteria = criteria.length ? criteria : [{ source: "vacdm", token: "", condition: "" }];
    const first = rule.criteria[0];
    rule.source = first.source;
    rule.token = first.token;
    rule.condition = first.condition;
    const name = $("#ruleName").value.trim();
    if (name) rule.name = name; else delete rule.name;
    rule.tag_type = $("#ruleTagType").value;
    const statuses = checkedRuleStatuses();
    rule.statuses = statuses;
    rule.status = statuses.length === 1 ? statuses[0] : "any";
    rule.detail = $("#ruleDetail").value;
    ["Target", "Tag", "Text"].forEach(kind => {
      const key = `${kind.toLowerCase()}_color`;
      if ($(`#ruleUse${kind}Color`).checked) rule[key] = hexToColor($(`#rule${kind}Color`).value, rule[key]?.a ?? 255);
      else delete rule[key];
    });
    return rule;
  }
  function applyRule() {
    const item = rules()[state.ui.selectedRuleIndex];
    if (!item || !drafts.rule) return;
    const rule = captureRuleDraft();
    rules()[state.ui.selectedRuleIndex] = clone(rule);
    drafts.rule = null;
    markDirty("Rule updated");
    renderRules();
  }

  function modes() {
    const filters = activeProfile().filters ||= {};
    filters.display_modes ||= { active: "Normal", items: [] };
    filters.display_modes.items ||= [];
    return filters.display_modes.items;
  }

  function renderModes() {
    const items = modes();
    const activeName = activeProfile().filters?.display_modes?.active;
    if (items.length && (state.ui.selectedModeIndex >= items.length || state.ui.selectedModeIndex < 0)) state.ui.selectedModeIndex = Math.max(0, items.findIndex(mode => mode.name === activeName));
    $("#modeCountCaption").textContent = `(${items.length})`;
    $("#modeList").innerHTML = items.map((mode, index) => `<button type="button" class="selection-row ${index === state.ui.selectedModeIndex ? "active" : ""}" data-mode-index="${index}"><span>${escapeHtml(mode.name || `Mode ${index + 1}`)}</span><span class="mode-active-mark">${mode.name === activeName ? "●" : ""}</span></button>`).join("") || `<div class="aviso-list-message">No modes</div>`;
    renderModeEditor();
  }

  function renderModeEditor() {
    const mode = modes()[state.ui.selectedModeIndex];
    if (!mode) return;
    if (!drafts.mode || drafts.mode.index !== state.ui.selectedModeIndex) drafts.mode = { index: state.ui.selectedModeIndex, data: clone(mode) };
    const data = drafts.mode.data;
    $("#modePropertiesCaption").textContent = data.name || "Mode properties";
    $("#modeName").value = data.name || "";
    data.blocked_auto_correlate_squawks ||= [];
    renderModeBlockedSquawkChips();
    $("#reqSquawk").checked = Boolean(data.require_assigned_squawk);
    $("#modeAcceptPilotSquawk").checked = data.accept_pilot_squawk !== false;
    $("#reqClearance").checked = Boolean(data.require_clearance);
    $("#reqTsat").checked = Boolean(data.require_valid_tsat);
    $("#reqTobt").checked = Boolean(data.require_active_tobt);
    $("#modeTowerFilter").checked = Boolean(data.tower_filter ?? data.tower_mode);
    $("#modeStructuredRules").checked = data.structured_rules !== false && data.structured_rules_enabled !== false;
    $("#modeStatusGrid").innerHTML = MODE_STATUSES.map(status => `<label class="check-field"><input type="checkbox" data-mode-status="${status}" ${data.statuses?.[status] ? "checked" : ""}><span>${escapeHtml(humanize(status))}</span></label>`).join("");
    $("[data-action='activate-mode']").textContent = data.name === activeProfile().filters?.display_modes?.active ? "Active" : "Set active";
  }

  function renderModeBlockedSquawkChips() {
    const container = $("#modeBlockedSquawkChips");
    if (!container || !drafts.mode) return;
    const squawks = drafts.mode.data.blocked_auto_correlate_squawks || [];
    container.innerHTML = squawks.length
      ? squawks.map((code, index) => `<span class="mode-squawk-chip"><span>${escapeHtml(code)}</span><button aria-label="Remove ${escapeHtml(code)}" data-action="remove-mode-squawk" data-index="${index}" title="Remove" type="button">×</button></span>`).join("")
      : `<span class="mode-squawk-empty">No blocked codes</span>`;
  }

  function addModeBlockedSquawk() {
    if (!drafts.mode) return false;
    const input = $("#modeBlockedSquawkInput");
    const code = String(input?.value || "").trim();
    if (!/^[0-7]{4}$/.test(code)) {
      showToast("Squawks must be four octal digits (0–7)", "error");
      input?.focus();
      return false;
    }
    const squawks = drafts.mode.data.blocked_auto_correlate_squawks ||= [];
    if (!squawks.includes(code)) squawks.push(code);
    input.value = "";
    renderModeBlockedSquawkChips();
    input.focus();
    return true;
  }

  function removeModeBlockedSquawk(index) {
    if (!drafts.mode) return;
    const squawks = drafts.mode.data.blocked_auto_correlate_squawks ||= [];
    if (index >= 0 && index < squawks.length) squawks.splice(index, 1);
    renderModeBlockedSquawkChips();
  }

  function setModeStatusVisibility(visible) {
    $$("[data-mode-status]").forEach(input => { input.checked = Boolean(visible); });
  }

  function captureModeDraft() {
    if (!drafts.mode) return null;
    const mode = drafts.mode.data;
    mode.name = $("#modeName").value.trim() || "Mode";
    mode.require_assigned_squawk = $("#reqSquawk").checked;
    mode.accept_pilot_squawk = $("#modeAcceptPilotSquawk").checked;
    mode.require_clearance = $("#reqClearance").checked;
    mode.require_valid_tsat = $("#reqTsat").checked;
    mode.require_active_tobt = $("#reqTobt").checked;
    mode.tower_filter = $("#modeTowerFilter").checked;
    mode.structured_rules = $("#modeStructuredRules").checked;
    delete mode.tower_mode;
    delete mode.structured_rules_enabled;
    mode.statuses ||= {};
    $$('[data-mode-status]').forEach(input => { mode.statuses[input.dataset.modeStatus] = input.checked; });
    return mode;
  }

  function applyMode() {
    const current = modes()[state.ui.selectedModeIndex];
    if (!current || !drafts.mode) return;
    if (String($("#modeBlockedSquawkInput").value || "").trim() && !addModeBlockedSquawk()) return;
    const oldName = current.name;
    const next = clone(captureModeDraft());
    modes()[state.ui.selectedModeIndex] = next;
    if (activeProfile().filters.display_modes.active === oldName) activeProfile().filters.display_modes.active = next.name;
    drafts.mode = null;
    markDirty("Display mode updated");
    renderModes();
    renderRuntimeMenu();
  }

  function renderProfilesManager() {
    if (!state.profiles.some(record => record.id === state.ui.managedProfileId)) state.ui.managedProfileId = state.activeProfileId;
    $("#profileCountCaption").textContent = `(${state.profiles.length})`;
    $("#profileList").innerHTML = state.profiles.map(record => `<button type="button" class="selection-row ${record.id === state.ui.managedProfileId ? "active" : ""}" data-managed-profile-id="${escapeHtml(record.id)}"><span>${escapeHtml(record.data.name)}</span><span class="profile-active-mark">${record.id === state.activeProfileId ? "●" : ""}</span></button>`).join("");
    renderProfileEditor();
  }

  function renderProfileEditor() {
    const record = managedProfileRecord();
    if (!record) return;
    if (!drafts.profile || drafts.profile.id !== record.id) drafts.profile = { id: record.id, data: clone(record.data) };
    const profile = drafts.profile.data;
    $("#profilePropertiesCaption").textContent = profile.name || "Profile properties";
    $("#profileName").value = profile.name || "";
    $("#profileSchema").value = profile.schema_version ?? 2;
    profile.filters ||= {};
    $("#profileMaxAltitude").value = profile.filters.max_altitude_ft ?? 5500;
    $("#profileMaxSpeed").value = profile.filters.max_speed_kt ?? 250;
    $("#profileRadarRange").value = profile.filters.radar_range_nm ?? 999;
    $("[data-action='activate-profile']").textContent = record.id === state.activeProfileId ? "Active" : "Set active";
  }

  function captureProfileDraft() {
    if (!drafts.profile) return null;
    const profile = drafts.profile.data;
    profile.name = $("#profileName").value.trim() || "Profile";
    profile.filters ||= {};
    profile.filters.max_altitude_ft = Number($("#profileMaxAltitude").value) || 0;
    profile.filters.max_speed_kt = Number($("#profileMaxSpeed").value) || 0;
    profile.filters.radar_range_nm = Number($("#profileRadarRange").value) || 0;
    return profile;
  }

  function applyProfile() {
    const record = managedProfileRecord();
    if (!record || !drafts.profile) return;
    const oldName = record.data.name;
    record.data = clone(captureProfileDraft());
    if (state.metadata.last_active_profile === oldName) state.metadata.last_active_profile = record.data.name;
    drafts.profile = null;
    markDirty("Profile updated");
    renderGlobalProfileSelect();
    renderProfilesManager();
    if (record.id === state.activeProfileId) renderAllProfileSections();
    renderRuntimeMenu();
  }

  function renderAllProfileSections() {
    renderColors();
    renderIcons();
    renderTags();
    renderRules();
    renderModes();
    renderProfilesManager();
  }

  function renderCurrentProfileTab() {
    if (state.ui.profileTab === "colors") renderColors();
    if (state.ui.profileTab === "icons") renderIcons();
    if (state.ui.profileTab === "tags") renderTags();
    if (state.ui.profileTab === "rules") renderRules();
  }

  function avisoFeatures() { return Array.isArray(state.aviso?.features) ? state.aviso.features : []; }

  const AVISO_GEOMETRY_PAINT_KEYS = ["fill", "fill-opacity", "stroke", "stroke-width", "stroke-opacity"];
  const AVISO_TEXT_PAINT_KEYS = ["text-font", "text-size", "text-color", "text-anchor", "text-halo-color", "text-halo-width", "zoomLevel"];
  const AVISO_TEXT_DEFAULTS = {
    "text-font": "Arial",
    "text-size": 12,
    "text-color": "#808080",
    "text-anchor": "center",
    "text-halo-color": "#000000",
    "text-halo-width": 1,
    zoomLevel: 6
  };

  function isAvisoTextFeature(feature) {
    const properties = feature?.properties || {};
    return properties.object_type === "Label" || properties["text-field"] != null || feature?.geometry?.type === "Point";
  }

  function inferAvisoObjectType(feature) {
    const properties = feature?.properties || {};
    if (properties.object_type) return properties.object_type;
    if (isAvisoTextFeature(feature)) return "Label";
    return String(feature?.geometry?.type || "").includes("Line") ? "Line" : "Area";
  }

  function avisoStyleSlug(value) {
    return String(value || "style").toLowerCase().replace(/[^a-z0-9]+/g, ".").replace(/^\.+|\.+$/g, "") || "style";
  }

  function collectAvisoStyleEntries() {
    const features = avisoFeatures();
    const catalog = state.aviso?.styles && typeof state.aviso.styles === "object" ? state.aviso.styles : {};
    const byId = new Map();

    features.forEach((feature, index) => {
      const properties = feature?.properties || {};
      const objectType = inferAvisoObjectType(feature);
      const fallbackPrefix = objectType === "Label" ? "label" : objectType === "Line" ? "line" : "area";
      const id = properties.style_id || `${fallbackPrefix}.${avisoStyleSlug(properties.category || properties.name || index)}`;
      if (!byId.has(id)) byId.set(id, { id, indices: [], firstFeature: feature });
      const entry = byId.get(id);
      entry.indices.push(index);
      if (!entry.firstFeature) entry.firstFeature = feature;
    });

    Object.entries(catalog).forEach(([id, style]) => {
      if (!byId.has(id)) byId.set(id, { id, indices: [], firstFeature: null });
      byId.get(id).style = style;
    });

    return Array.from(byId.values()).map(entry => {
      const properties = entry.firstFeature?.properties || {};
      const style = entry.style || catalog[entry.id] || {};
      const objectType = style.object_type || inferAvisoObjectType(entry.firstFeature);
      const paint = clone(style.paint || {});
      const paintKeys = objectType === "Label" ? AVISO_TEXT_PAINT_KEYS : AVISO_GEOMETRY_PAINT_KEYS;
      paintKeys.forEach(key => {
        if (paint[key] == null && properties[key] != null) paint[key] = properties[key];
      });
      return {
        id: entry.id,
        name: style.name || properties.category || properties.name || entry.id,
        layer: style.layer || properties.layer || (objectType === "Label" ? "Labels" : "Other"),
        objectType,
        paint,
        indices: entry.indices,
        count: entry.indices.length || Number(style.feature_count) || 0,
        isText: objectType === "Label" || isAvisoTextFeature(entry.firstFeature)
      };
    }).sort((a, b) => a.layer.localeCompare(b.layer) || a.name.localeCompare(b.name));
  }

  function avisoStyleEntries(kind) {
    return collectAvisoStyleEntries().filter(entry => kind === "text" ? entry.isText : !entry.isText);
  }

  function avisoStyleEntry(styleId, kind) {
    return avisoStyleEntries(kind).find(entry => entry.id === styleId) || null;
  }

  function ensureAvisoCatalogStyle(entry) {
    state.aviso.styles ||= {};
    const style = state.aviso.styles[entry.id] ||= {
      name: entry.name,
      layer: entry.layer,
      object_type: entry.objectType,
      paint: {},
      feature_count: entry.indices.length
    };
    style.name ||= entry.name;
    style.layer ||= entry.layer;
    style.object_type ||= entry.objectType;
    style.paint ||= {};
    style.feature_count = entry.indices.length;
    return style;
  }

  function preferredAvisoStyleId(kind) {
    const entries = avisoStyleEntries(kind);
    if (kind === "text") {
      return entries.find(entry => entry.id === "label.taxiways")?.id
        || entries.find(entry => /taxiway labels/i.test(entry.name))?.id
        || entries[0]?.id || "";
    }
    return entries.find(entry => entry.id === "surface.taxiway")?.id
      || entries.find(entry => /^taxiways$/i.test(entry.name))?.id
      || entries[0]?.id || "";
  }

  function uniqueValues(values) {
    return Array.from(new Set(values));
  }

  function commonValue(values, normalizer = value => value) {
    const normalized = values.map(value => normalizer(value));
    if (!normalized.length) return { mixed: false, value: undefined };
    const first = normalized[0];
    const mixed = normalized.some(value => value !== first);
    return { mixed, value: first };
  }

  function resetControlFlags(element, mixed = false) {
    if (!element) return;
    element.dataset.touched = "false";
    element.dataset.mixed = mixed ? "true" : "false";
    element.closest(".field")?.classList.toggle("mixed", mixed);
  }

  function markControlTouched(element) {
    if (!element) return;
    element.dataset.touched = "true";
    element.dataset.mixed = "false";
    element.closest(".field")?.classList.remove("mixed");
  }

  function wasControlTouched(selector) {
    return $(selector)?.dataset.touched === "true";
  }

  function setCommonInput(selector, result, formatter = value => value ?? "") {
    const input = $(selector);
    resetControlFlags(input, result.mixed);
    input.value = result.mixed ? "" : formatter(result.value);
    if (result.mixed) input.placeholder = "Mixed";
  }

  function setCommonSelect(selector, result, fallback = "") {
    const select = $(selector);
    select.querySelectorAll("option[data-mixed-option]").forEach(option => option.remove());
    resetControlFlags(select, result.mixed);
    if (result.mixed) {
      const option = document.createElement("option");
      option.value = "";
      option.textContent = "Mixed";
      option.dataset.mixedOption = "true";
      select.prepend(option);
      select.value = "";
    } else {
      ensureSelectValue(select, String(result.value ?? fallback));
    }
  }

  function setCommonCheckbox(selector, values) {
    const checkbox = $(selector);
    const yes = values.filter(Boolean).length;
    const mixed = yes > 0 && yes < values.length;
    checkbox.checked = yes > 0;
    checkbox.indeterminate = mixed;
    checkbox.dataset.mixed = mixed ? "true" : "false";
    checkbox.dataset.touched = "false";
  }

  function setCommonColor(textSelector, pickerSelector, pairSelector, result, fallback) {
    const text = $(textSelector);
    const picker = $(pickerSelector);
    const pair = $(pairSelector);
    const representative = normalizeHex(result.value, fallback);
    text.value = result.mixed ? "" : representative.toUpperCase();
    text.placeholder = result.mixed ? "Mixed" : "#RRGGBB";
    picker.value = representative;
    resetControlFlags(text, result.mixed);
    resetControlFlags(picker, result.mixed);
    pair.classList.toggle("mixed", result.mixed);
    pair.title = result.mixed ? "Selected items use different colors" : "";
  }

  function setCommonRange(selector, outputSelector, result, fallback = 100) {
    const range = $(selector);
    const output = $(outputSelector);
    const value = Math.round(clamp(result.value ?? fallback, 0, 100));
    range.value = value;
    output.value = result.mixed ? "Mixed" : `${value}%`;
    resetControlFlags(range, result.mixed);
    range.closest(".range-control")?.classList.toggle("mixed", result.mixed);
  }

  function geometrySelectionIds(entries = avisoStyleEntries("geometry")) {
    const valid = new Set(entries.map(entry => entry.id));
    let ids = Array.isArray(state.ui.selectedAvisoGeometryStyleIds)
      ? state.ui.selectedAvisoGeometryStyleIds.filter(id => valid.has(id))
      : [];
    if (!ids.length && valid.has(state.ui.selectedAvisoGeometryStyleId)) ids = [state.ui.selectedAvisoGeometryStyleId];
    if (!ids.length) {
      const fallback = preferredAvisoStyleId("geometry");
      if (fallback) ids = [fallback];
    }
    ids = uniqueValues(ids);
    state.ui.selectedAvisoGeometryStyleIds = ids;
    if (!ids.includes(state.ui.selectedAvisoGeometryStyleId)) state.ui.selectedAvisoGeometryStyleId = ids[ids.length - 1] || "";
    if (!valid.has(state.ui.avisoGeometrySelectionAnchorId)) state.ui.avisoGeometrySelectionAnchorId = state.ui.selectedAvisoGeometryStyleId;
    return ids;
  }

  function selectedAvisoGeometryEntries(entries = avisoStyleEntries("geometry")) {
    const selected = new Set(geometrySelectionIds(entries));
    return entries.filter(entry => selected.has(entry.id));
  }

  function textSelectionIndices(entry) {
    if (!entry) return [];
    const valid = new Set(entry.indices);
    let indices = Array.isArray(state.ui.selectedAvisoTextIndices)
      ? state.ui.selectedAvisoTextIndices.map(Number).filter(index => valid.has(index))
      : [];
    if (!indices.length && valid.has(Number(state.ui.selectedAvisoTextIndex))) indices = [Number(state.ui.selectedAvisoTextIndex)];
    if (!indices.length && entry.indices.length) indices = [entry.indices[0]];
    indices = uniqueValues(indices);
    state.ui.selectedAvisoTextIndices = indices;
    if (!indices.includes(Number(state.ui.selectedAvisoTextIndex))) state.ui.selectedAvisoTextIndex = indices[indices.length - 1] ?? entry.indices[0] ?? 0;
    if (!valid.has(Number(state.ui.avisoTextSelectionAnchorIndex))) state.ui.avisoTextSelectionAnchorIndex = state.ui.selectedAvisoTextIndex;
    return indices;
  }

  function updateMultiSelection(current, clicked, ordered, event, anchor, forceToggle = false) {
    const additive = forceToggle || event.ctrlKey || event.metaKey;
    const shift = event.shiftKey;
    let next = current.slice();

    if (shift && ordered.length) {
      const anchorValue = ordered.includes(anchor) ? anchor : (ordered.includes(current[current.length - 1]) ? current[current.length - 1] : clicked);
      const start = ordered.indexOf(anchorValue);
      const end = ordered.indexOf(clicked);
      const range = start >= 0 && end >= 0 ? ordered.slice(Math.min(start, end), Math.max(start, end) + 1) : [clicked];
      next = additive ? uniqueValues([...current, ...range]) : range;
    } else if (additive) {
      if (next.includes(clicked)) {
        if (next.length > 1) next = next.filter(value => value !== clicked);
      } else {
        next.push(clicked);
      }
    } else {
      next = [clicked];
    }

    return next.length ? uniqueValues(next) : [clicked];
  }

  function resetAvisoSelections() {
    const geometryId = preferredAvisoStyleId("geometry");
    const textId = preferredAvisoStyleId("text");
    const textEntry = avisoStyleEntry(textId, "text");
    const textIndex = textEntry?.indices[0] ?? 0;
    state.ui.selectedAvisoGeometryStyleId = geometryId;
    state.ui.selectedAvisoGeometryStyleIds = geometryId ? [geometryId] : [];
    state.ui.avisoGeometrySelectionAnchorId = geometryId;
    state.ui.selectedAvisoTextStyleId = textId;
    state.ui.selectedAvisoTextIndex = textIndex;
    state.ui.selectedAvisoTextIndices = textEntry ? [textIndex] : [];
    state.ui.avisoTextSelectionAnchorIndex = textIndex;
    state.ui.selectedAvisoGroupId = avisoGroups()[0]?.id || "";
    state.ui.avisoGeometrySearch = "";
    state.ui.avisoTextSearch = "";
    state.ui.avisoGroupSearch = "";
    state.ui.avisoGroupMemberSearch = "";
    state.ui.avisoGroupMemberFilter = "all";
    drafts.avisoGeometry = null;
    drafts.avisoTextStyle = null;
    drafts.avisoTextLabel = null;
    drafts.avisoGroup = null;
    avisoGroupContentDraft = null;
  }

  function avisoPaintColor(entry) {
    return normalizeHex(entry.paint[entry.isText ? "text-color" : (entry.objectType === "Area" ? "fill" : "stroke")], "#6d7a7f");
  }

  function filteredAvisoGeometryEntries(entries = avisoStyleEntries("geometry")) {
    const search = String(state.ui.avisoGeometrySearch || "").trim().toLowerCase();
    return entries.filter(entry => !search || [entry.name, entry.layer, entry.id, entry.objectType].join(" ").toLowerCase().includes(search));
  }

  function filteredAvisoTextIndices(entry) {
    const features = avisoFeatures();
    const search = String(state.ui.avisoTextSearch || "").trim().toLowerCase();
    return (entry?.indices || []).filter(index => {
      if (!search) return true;
      const properties = features[index]?.properties || {};
      return [properties["text-field"], properties.name, properties.section].join(" ").toLowerCase().includes(search);
    });
  }

  function uniqueAvisoGroupId(name = "group") {
    const base = normalizeAvisoGroupId(name);
    const used = new Set(avisoGroups().map(group => group.id));
    let id = base;
    let suffix = 2;
    while (used.has(id)) id = `${base}-${suffix++}`;
    return id;
  }

  function avisoFeatureDisplayName(index) {
    const feature = avisoFeatures()[index];
    const properties = feature?.properties || {};
    return String(properties["text-field"] || properties.name || properties.category || properties.style_id || `Feature ${index + 1}`);
  }

  function avisoGroupMemberRows(groupId) {
    const memberSet = new Set(avisoGroupMemberIndices(groupId));
    const rows = [];
    const features = avisoFeatures();

    memberSet.forEach(index => {
      const feature = features[index];
      if (!isAvisoTextFeature(feature)) return;
      const properties = feature?.properties || {};
      rows.push({
        key: `feature:${index}`,
        kind: "text",
        id: String(index),
        name: avisoFeatureDisplayName(index),
        subtitle: properties.category || properties.style_id || "Text",
        count: 1,
        total: 1
      });
    });

    avisoStyleEntries("geometry").forEach(entry => {
      const selected = entry.indices.filter(index => memberSet.has(index));
      if (!selected.length) return;
      rows.push({
        key: `style:${entry.id}`,
        kind: entry.objectType.toLowerCase(),
        id: entry.id,
        name: entry.name,
        subtitle: entry.layer,
        count: selected.length,
        total: entry.indices.length
      });
    });

    const order = { text: 0, line: 1, area: 2 };
    return rows.sort((a, b) => (order[a.kind] ?? 9) - (order[b.kind] ?? 9) || a.name.localeCompare(b.name));
  }

  function captureAvisoGroupDraft() {
    const group = selectedAvisoGroup();
    if (!group) return null;
    if (!drafts.avisoGroup || drafts.avisoGroup.id !== group.id) {
      drafts.avisoGroup = { id: group.id, original: clone(group), data: clone(group) };
    }
    const name = $("#avisoGroupName");
    const visible = $("#avisoGroupVisible");
    if (name) drafts.avisoGroup.data.name = name.value;
    if (visible) drafts.avisoGroup.data.visible = visible.checked;
    const accentInput = $("#avisoGroupAccentHex");
    if (accentInput) drafts.avisoGroup.data.accent = normalizeHex(accentInput.value, group.accent || "#84b7d5");
    return drafts.avisoGroup;
  }

  function renderAvisoGroupTargets() {
    const groups = avisoGroups();
    [["#avisoGeometryGroupTarget", "#avisoGeometryGroupAdd"], ["#avisoTextGroupTarget", "#avisoTextGroupAdd"]].forEach(([selectSelector, buttonSelector]) => {
      const select = $(selectSelector);
      const button = $(buttonSelector);
      if (!select || !button) return;
      const previous = select.value;
      select.innerHTML = groups.length
        ? groups.map(group => `<option value="${escapeHtml(group.id)}">${escapeHtml(group.name)}</option>`).join("")
        : `<option value="">Create a group first</option>`;
      if (groups.some(group => group.id === previous)) select.value = previous;
      else select.value = groups[0]?.id || "";
      select.disabled = !groups.length;
      button.disabled = !groups.length;
    });
  }

  function renderAvisoGroups() {
    const groups = avisoGroups();
    const search = String(state.ui.avisoGroupSearch || "").trim().toLowerCase();
    const filtered = groups.filter(group => !search || `${group.name} ${group.id}`.toLowerCase().includes(search));
    const selected = selectedAvisoGroup();

    $("#avisoGroupSearch").value = state.ui.avisoGroupSearch;
    $("#avisoGroupCount").textContent = `(${groups.length})`;
    $("#avisoGroupList").innerHTML = filtered.length ? `<div class="aviso-group-box">${filtered.map(group => {
      const counts = avisoGroupCounts(group.id);
      const active = group.id === selected?.id;
      return `<div class="aviso-group-row ${active ? "active" : ""}" role="option" aria-selected="${active}" data-aviso-group-id="${escapeHtml(group.id)}" draggable="${search ? "false" : "true"}" title="${search ? "Clear the search to reorder groups" : "Drag to reorder"}">
        <span class="aviso-group-row-copy"><strong>${escapeHtml(group.name)}</strong></span>
        <span class="aviso-group-row-count">${counts.total.toLocaleString()}</span>
      </div>`;
    }).join("")}</div>` : `<div class="aviso-list-message">${groups.length ? "No matching groups" : "No groups yet"}</div>`;

    renderAvisoGroupEditor();
    renderAvisoGroupTargets();
  }

  function renderAvisoGroupEditor() {
    const group = selectedAvisoGroup();
    const editor = $(".aviso-group-editor");
    if (!group) {
      $("#avisoGroupCaption").textContent = "No group selected";
      $("#avisoGroupSelectionMeta").textContent = "";
      $("#avisoGroupName").value = "";
      $("#avisoGroupName").disabled = true;
      $("#avisoGroupId").value = "";
      $("#avisoGroupMemberList").innerHTML = `<div class="aviso-group-empty">Create a group to combine text, line and area content.</div>`;
      editor?.classList.add("empty");
      return;
    }
    editor?.classList.remove("empty");
    $("#avisoGroupName").disabled = false;
    if (!drafts.avisoGroup || drafts.avisoGroup.id !== group.id) drafts.avisoGroup = { id: group.id, original: clone(group), data: clone(group) };
    const draft = drafts.avisoGroup.data;
    const counts = avisoGroupCounts(group.id);

    $("#avisoGroupCaption").textContent = draft.name || group.name;
    $("#avisoGroupSelectionMeta").textContent = `${counts.total.toLocaleString()} item${counts.total === 1 ? "" : "s"}`;
    $("#avisoGroupName").value = draft.name || "";
    $("#avisoGroupId").value = group.id;

    $("#avisoGroupMemberSearch").value = state.ui.avisoGroupMemberSearch;
    const search = String(state.ui.avisoGroupMemberSearch || "").trim().toLowerCase();
    const rows = avisoGroupMemberRows(group.id).filter(row =>
      !search || `${row.name} ${row.subtitle} ${row.id}`.toLowerCase().includes(search)
    );
    $("#avisoGroupMemberList").innerHTML = rows.length ? rows.map(row => {
      const kindLabel = row.kind === "text" ? "TXT" : row.kind === "line" ? "LIN" : "AREA";
      return `<div class="aviso-group-member-row">
        <span class="aviso-member-kind ${row.kind}">${kindLabel}</span>
        <span class="aviso-member-copy"><strong>${escapeHtml(row.name)}</strong><small>${escapeHtml(row.subtitle)}</small></span>
        <span class="aviso-member-count">${row.count === row.total ? row.count.toLocaleString() : `${row.count}/${row.total}`}</span>
        <button class="aviso-member-remove" type="button" data-action="remove-aviso-group-member" data-member-kind="${row.kind === "text" ? "feature" : "style"}" data-member-id="${escapeHtml(row.id)}" title="Remove from group">×</button>
      </div>`;
    }).join("") : `<div class="aviso-group-empty">${counts.total ? "No members match your search." : "This group is empty. Add text, lines or areas."}</div>`;
  }

  function createAvisoGroup() {
    const number = avisoGroups().length + 1;
    const group = { id: uniqueAvisoGroupId(`group-${number}`), name: `Group ${number}`, visible: true, accent: "#84b7d5" };
    avisoGroups().push(group);
    state.ui.selectedAvisoGroupId = group.id;
    drafts.avisoGroup = null;
    markDirty("AVISO group created");
    postBridge("aviso.groups.update", { groups: clone(avisoGroups()), aviso: clone(state.aviso) });
    renderAviso();
    renderRuntimeMenu();
  }

  function duplicateAvisoGroup() {
    const source = selectedAvisoGroup();
    if (!source) return;
    const copy = { id: uniqueAvisoGroupId(`${source.id}-copy`), name: `${source.name} copy`, visible: source.visible !== false, accent: normalizeHex(source.accent, "#84b7d5") };
    avisoGroups().splice(avisoGroups().indexOf(source) + 1, 0, copy);
    avisoFeatures().forEach(feature => {
      if (featureGroupIds(feature).includes(source.id)) setFeatureGroupMembership(feature, copy.id, true);
    });
    state.ui.selectedAvisoGroupId = copy.id;
    drafts.avisoGroup = null;
    markDirty("AVISO group copied");
    postBridge("aviso.groups.update", { groups: clone(avisoGroups()), aviso: clone(state.aviso) });
    renderAviso();
    renderRuntimeMenu();
  }

  function deleteAvisoGroup() {
    const group = selectedAvisoGroup();
    if (!group || !confirmDelete(`Delete the AVISO group “${group.name}”?`)) return;
    const index = avisoGroups().indexOf(group);
    avisoGroups().splice(index, 1);
    avisoFeatures().forEach(feature => setFeatureGroupMembership(feature, group.id, false));
    state.ui.selectedAvisoGroupId = avisoGroups()[Math.max(0, index - 1)]?.id || avisoGroups()[0]?.id || "";
    drafts.avisoGroup = null;
    markDirty("AVISO group deleted");
    postBridge("aviso.groups.update", { groups: clone(avisoGroups()), aviso: clone(state.aviso) });
    renderAviso();
    renderRuntimeMenu();
  }

  function applyAvisoGroup() {
    const group = selectedAvisoGroup();
    if (!group) return;
    const draft = captureAvisoGroupDraft();
    const name = String(draft?.data?.name || "").trim();
    if (!name) {
      showToast("Enter a group name", "error");
      return;
    }
    const visibilityChanged = group.visible !== (draft.data.visible !== false);
    group.name = name;
    group.visible = draft.data.visible !== false;
    group.accent = normalizeHex(draft.data.accent, group.accent || "#84b7d5");
    drafts.avisoGroup = null;
    markDirty("AVISO group updated");
    postBridge("aviso.groups.update", { groups: clone(avisoGroups()) });
    if (visibilityChanged) postBridge("aviso.group.visibility", { id: group.id, name: group.name, visible: group.visible });
    renderAviso();
    renderRuntimeMenu();
  }

  function revertAvisoGroup() {
    const group = selectedAvisoGroup();
    if (!group) return;
    drafts.avisoGroup = { id: group.id, original: clone(group), data: clone(group) };
    renderAvisoGroupEditor();
  }

  function setSelectedAvisoGroupVisibility(visible) {
    const group = selectedAvisoGroup();
    if (!group) return;
    group.visible = Boolean(visible);
    if (drafts.avisoGroup?.id === group.id) drafts.avisoGroup.data.visible = group.visible;
    markDirty(`Group ${group.visible ? "shown" : "hidden"}`);
    postBridge("aviso.group.visibility", { id: group.id, name: group.name, visible: group.visible });
    renderAvisoGroups();
    renderRuntimeMenu();
  }

  function isolateSelectedAvisoGroup() {
    const selected = selectedAvisoGroup();
    if (!selected) return;
    avisoGroups().forEach(group => { group.visible = group.id === selected.id; });
    if (drafts.avisoGroup?.id === selected.id) drafts.avisoGroup.data.visible = true;
    markDirty("AVISO group isolated");
    postBridge("aviso.groups.visibility", { groups: avisoGroups().map(group => ({ id: group.id, visible: group.visible })) });
    renderAvisoGroups();
    renderRuntimeMenu();
  }

  function clearSelectedAvisoGroup() {
    const group = selectedAvisoGroup();
    if (!group || !avisoGroupMemberIndices(group.id).length) return;
    if (!confirmDelete(`Remove all content from “${group.name}”?`)) return;
    avisoFeatures().forEach(feature => setFeatureGroupMembership(feature, group.id, false));
    markDirty("AVISO group cleared");
    postBridge("aviso.groups.update", { groups: clone(avisoGroups()), aviso: clone(state.aviso) });
    renderAvisoGroups();
    renderRuntimeMenu();
  }

  function removeAvisoGroupMember(button) {
    const group = selectedAvisoGroup();
    if (!group) return;
    if (button.dataset.memberKind === "feature") {
      const feature = avisoFeatures()[Number(button.dataset.memberId)];
      if (feature) setFeatureGroupMembership(feature, group.id, false);
    } else {
      const entry = avisoStyleEntry(button.dataset.memberId, "geometry");
      entry?.indices.forEach(index => setFeatureGroupMembership(avisoFeatures()[index], group.id, false));
    }
    markDirty("AVISO group contents updated");
    postBridge("aviso.groups.update", { groups: clone(avisoGroups()), aviso: clone(state.aviso) });
    renderAvisoGroups();
    renderRuntimeMenu();
  }

  function assignAvisoSelectionToGroup(kind) {
    const selector = kind === "geometry" ? "#avisoGeometryGroupTarget" : "#avisoTextGroupTarget";
    const groupId = $(selector)?.value;
    const group = avisoGroups().find(item => item.id === groupId);
    if (!group) return;
    const indices = kind === "geometry"
      ? uniqueValues(selectedAvisoGeometryEntries().flatMap(entry => entry.indices))
      : textSelectionIndices(avisoStyleEntry(state.ui.selectedAvisoTextStyleId, "text"));
    indices.forEach(index => setFeatureGroupMembership(avisoFeatures()[index], group.id, true));
    markDirty(`${indices.length} AVISO feature${indices.length === 1 ? "" : "s"} added to ${group.name}`);
    postBridge("aviso.groups.update", { groups: clone(avisoGroups()), aviso: clone(state.aviso) });
    showToast(`Added ${indices.length.toLocaleString()} feature${indices.length === 1 ? "" : "s"} to ${group.name}`, "success");
    renderRuntimeMenu();
  }

  function groupContentCandidates(type = state.ui.avisoGroupContentType, searchValue = state.ui.avisoGroupContentSearch) {
    const search = String(searchValue || "").trim().toLowerCase();
    if (type === "text") {
      return avisoFeatures().map((feature, index) => ({ feature, index })).filter(item => isAvisoTextFeature(item.feature)).map(item => {
        const properties = item.feature?.properties || {};
        return {
          key: `feature:${item.index}`,
          indices: [item.index],
          kind: "text",
          name: avisoFeatureDisplayName(item.index),
          subtitle: properties.category || properties.style_id || "Text",
          count: 1
        };
      }).filter(item => !search || `${item.name} ${item.subtitle}`.toLowerCase().includes(search));
    }
    const objectType = type === "line" ? "Line" : "Area";
    return avisoStyleEntries("geometry").filter(entry => entry.objectType === objectType).map(entry => ({
      key: `style:${entry.id}`,
      indices: entry.indices.slice(),
      kind: type,
      name: entry.name,
      subtitle: entry.layer,
      count: entry.indices.length
    })).filter(item => !search || `${item.name} ${item.subtitle} ${item.key}`.toLowerCase().includes(search));
  }

  function openAvisoGroupContentDialog() {
    const group = selectedAvisoGroup();
    if (!group) return;
    avisoGroupContentDraft = { groupId: group.id, members: new Set(avisoGroupMemberIndices(group.id)) };
    state.ui.avisoGroupContentSearch = "";
    $("#avisoGroupContentSearch").value = "";
    renderAvisoGroupContentDialog();
    $("#avisoGroupContentDialog").showModal();
  }

  function renderAvisoGroupContentDialog() {
    const group = avisoGroups().find(item => item.id === avisoGroupContentDraft?.groupId) || selectedAvisoGroup();
    if (!group || !avisoGroupContentDraft) return;
    $("#avisoGroupContentTitle").textContent = `Content · ${group.name}`;
    $$('[data-aviso-group-content-type]').forEach(button => button.classList.toggle("active", button.dataset.avisoGroupContentType === state.ui.avisoGroupContentType));
    $("#avisoGroupContentSearch").value = state.ui.avisoGroupContentSearch;
    const candidates = groupContentCandidates();
    const selectedTotal = avisoGroupContentDraft.members.size;
    $("#avisoGroupContentSummary").textContent = `${selectedTotal.toLocaleString()} features assigned · ${candidates.length.toLocaleString()} items shown`;
    $("#avisoGroupContentList").innerHTML = candidates.length ? `<div class="group-content-box">${candidates.map(item => {
      const selectedCount = item.indices.filter(index => avisoGroupContentDraft.members.has(index)).length;
      const selected = selectedCount === item.indices.length;
      const partial = selectedCount > 0 && !selected;
      return `<button type="button" class="group-content-row ${selected ? "selected" : ""} ${partial ? "partial" : ""}" data-group-content-key="${escapeHtml(item.key)}"><span class="group-content-check">${selected ? "✓" : partial ? "−" : ""}</span><span class="aviso-member-kind">${item.kind === "text" ? "TXT" : item.kind === "line" ? "LIN" : "AREA"}</span><span class="group-content-copy"><strong>${escapeHtml(item.name)}</strong><small>${escapeHtml(item.subtitle)}</small></span><span class="group-content-count">${partial ? `${selectedCount}/${item.count}` : item.count.toLocaleString()}</span></button>`;
    }).join("")}</div>` : `<div class="aviso-group-empty">No matching ${state.ui.avisoGroupContentType} content.</div>`;
  }

  function toggleAvisoGroupContentCandidate(key) {
    if (!avisoGroupContentDraft) return;
    const item = groupContentCandidates().find(candidate => candidate.key === key);
    if (!item) return;
    const allSelected = item.indices.every(index => avisoGroupContentDraft.members.has(index));
    item.indices.forEach(index => {
      if (allSelected) avisoGroupContentDraft.members.delete(index);
      else avisoGroupContentDraft.members.add(index);
    });
    renderAvisoGroupContentDialog();
  }

  function setFilteredAvisoGroupContent(selected) {
    if (!avisoGroupContentDraft) return;
    groupContentCandidates().forEach(item => item.indices.forEach(index => {
      if (selected) avisoGroupContentDraft.members.add(index);
      else avisoGroupContentDraft.members.delete(index);
    }));
    renderAvisoGroupContentDialog();
  }

  function applyAvisoGroupContent() {
    const group = avisoGroups().find(item => item.id === avisoGroupContentDraft?.groupId);
    if (!group || !avisoGroupContentDraft) return;
    avisoFeatures().forEach((feature, index) => setFeatureGroupMembership(feature, group.id, avisoGroupContentDraft.members.has(index)));
    avisoGroupContentDraft = null;
    $("#avisoGroupContentDialog").close();
    markDirty("AVISO group contents updated");
    postBridge("aviso.groups.update", { groups: clone(avisoGroups()), aviso: clone(state.aviso) });
    renderAvisoGroups();
    renderRuntimeMenu();
  }

  function renderAviso() {
    const features = avisoFeatures();
    const geometryEntries = avisoStyleEntries("geometry");
    const textEntries = avisoStyleEntries("text");
    const geometryObjectCount = features.filter(feature => !isAvisoTextFeature(feature)).length;
    const textObjectCount = features.length - geometryObjectCount;
    const airport = state.aviso?.metadata?.airport || inferAirport(state.aviso?.name) || "AVISO";

    geometrySelectionIds(geometryEntries);
    if (!textEntries.some(entry => entry.id === state.ui.selectedAvisoTextStyleId)) {
      state.ui.selectedAvisoTextStyleId = preferredAvisoStyleId("text");
      const entry = avisoStyleEntry(state.ui.selectedAvisoTextStyleId, "text");
      const index = entry?.indices[0] ?? 0;
      state.ui.selectedAvisoTextIndex = index;
      state.ui.selectedAvisoTextIndices = entry ? [index] : [];
      state.ui.avisoTextSelectionAnchorIndex = index;
      drafts.avisoTextStyle = null;
      drafts.avisoTextLabel = null;
    }

    $("#avisoDatasetCaption").textContent = airport;
    $("#avisoGeometryTabCount").textContent = `(${geometryObjectCount.toLocaleString()})`;
    $("#avisoTextTabCount").textContent = `(${textObjectCount.toLocaleString()})`;
    $$('[data-aviso-view]').forEach(button => button.classList.toggle("active", button.dataset.avisoView === state.ui.avisoView));
    $$('[data-aviso-view-panel]').forEach(panel => panel.classList.toggle("active", panel.dataset.avisoViewPanel === state.ui.avisoView));

    if (state.ui.avisoView === "geometry") renderAvisoGeometry();
    else renderAvisoText();
    renderAvisoGroupTargets();
  }

  function renderAvisoGeometry() {
    const allEntries = avisoStyleEntries("geometry");
    const selectedIds = new Set(geometrySelectionIds(allEntries));
    const filtered = filteredAvisoGeometryEntries(allEntries);
    avisoGeometryRenderOrder = filtered.map(entry => entry.id);

    $("#avisoGeometrySearch").value = state.ui.avisoGeometrySearch;
    $("#avisoGeometryStyleCount").textContent = `(${allEntries.length})`;
    $("#avisoGeometrySelectionCount").textContent = `${selectedIds.size} selected`;

    const grouped = new Map();
    filtered.forEach(entry => {
      if (!grouped.has(entry.layer)) grouped.set(entry.layer, []);
      grouped.get(entry.layer).push(entry);
    });

    $("#avisoGeometryStyleList").innerHTML = Array.from(grouped.entries()).map(([layer, entries]) => `
      <section class="aviso-style-section">
        <div class="aviso-style-section-title"><span>${escapeHtml(layer)}</span><small>${entries.length}</small></div>
        <div class="aviso-style-box">
          ${entries.map(entry => {
            const selected = selectedIds.has(entry.id);
            const current = entry.id === state.ui.selectedAvisoGeometryStyleId;
            return `<button type="button" role="option" aria-selected="${selected}" class="aviso-style-row geometry-select-row ${selected ? "active" : ""} ${current ? "current" : ""}" data-aviso-geometry-style="${escapeHtml(entry.id)}">
              <span class="aviso-select-box" data-aviso-selection-toggle="geometry" aria-hidden="true"></span>
              <span class="aviso-style-swatch" style="--aviso-swatch:${avisoPaintColor(entry)}"></span>
              <span class="aviso-style-copy"><strong>${escapeHtml(entry.name)}</strong><small>${escapeHtml(entry.objectType)}</small></span>
              <span class="aviso-style-count">${entry.count.toLocaleString()}</span>
            </button>`;
          }).join("")}
        </div>
      </section>`).join("") || `<div class="aviso-list-message">No matching geometry styles</div>`;

    renderAvisoGeometryEditor(allEntries);
  }

  function renderAvisoGeometryEditor(allEntries = avisoStyleEntries("geometry")) {
    const entries = selectedAvisoGeometryEntries(allEntries);
    if (!entries.length) {
      $("#avisoGeometryCaption").textContent = "No geometry selected";
      $("#avisoGeometryApplyButton").disabled = true;
      return;
    }

    const types = uniqueValues(entries.map(entry => entry.objectType));
    const layers = uniqueValues(entries.map(entry => entry.layer));
    const allArea = types.length === 1 && types[0] === "Area";
    const allLine = types.length === 1 && types[0] === "Line";
    const mixedTypes = types.length > 1;
    const objectCount = entries.reduce((sum, entry) => sum + entry.count, 0);
    const allIndices = uniqueValues(entries.flatMap(entry => entry.indices));
    const visibilityValues = allIndices.map(index => avisoFeatures()[index]?.properties?.visible !== false);

    $("#avisoGeometryCaption").textContent = entries.length === 1 ? entries[0].name : `${entries.length} geometry styles`;
    $("#avisoGeometrySelectionMeta").textContent = entries.length === 1 ? entries[0].id : "Multiple selection";
    $("#avisoGeometryKind").textContent = mixedTypes ? "Mixed" : types[0] || "—";
    $("#avisoGeometryLayer").textContent = layers.length === 1 ? layers[0] : `${layers.length} layers`;
    $("#avisoGeometryObjectCount").textContent = `${objectCount.toLocaleString()} object${objectCount === 1 ? "" : "s"}`;
    $("#avisoGeometryVisibleLabel").textContent = entries.length === 1 ? "Show this category" : "Show selected categories";
    $("#avisoGeometryApplyButton").textContent = entries.length === 1 ? "Update" : `Update ${entries.length} styles`;
    $("#avisoGeometryApplyButton").disabled = false;
    $("#avisoGeometryAdvancedInfo").textContent = `${entries.length} style${entries.length === 1 ? "" : "s"} · ${types.join(" + ")} · ${layers.length} layer${layers.length === 1 ? "" : "s"}`;

    const compatibility = $("#avisoGeometryCompatibility");
    compatibility.hidden = !mixedTypes;
    compatibility.innerHTML = mixedTypes
      ? `<strong>Area and line styles selected.</strong> Fill is hidden; only shared line / outline properties can be changed.`
      : "";

    $("#avisoGeometryFillGroup").hidden = !allArea;
    $("#avisoGeometryFillOpacityField").hidden = !allArea;
    $("#avisoGeometryStrokeLegend").textContent = allArea ? "Outline" : allLine ? "Line" : "Line / outline";

    setCommonCheckbox("#avisoGeometryVisible", visibilityValues);

    if (allArea) {
      setCommonColor(
        "#avisoGeometryFillColor",
        "#avisoGeometryFillPicker",
        "#avisoGeometryFillPair",
        commonValue(entries.map(entry => entry.paint.fill || entry.paint.stroke || "#000000"), value => normalizeHex(value, "#000000")),
        "#000000"
      );
      setCommonRange(
        "#avisoGeometryFillOpacity",
        "#avisoGeometryFillOpacityOutput",
        commonValue(entries.map(entry => Number(entry.paint["fill-opacity"] ?? 1) * 100), value => Math.round(Number(value) * 100) / 100),
        100
      );
    }

    setCommonColor(
      "#avisoGeometryStrokeColor",
      "#avisoGeometryStrokePicker",
      "#avisoGeometryStrokePair",
      commonValue(entries.map(entry => entry.paint.stroke || entry.paint.fill || "#000000"), value => normalizeHex(value, "#000000")),
      "#000000"
    );
    setCommonInput(
      "#avisoGeometryStrokeWidth",
      commonValue(entries.map(entry => Number(entry.paint["stroke-width"] ?? (entry.objectType === "Line" ? 1.5 : 0.5))), value => Number(value)),
      value => String(value)
    );
    setCommonRange(
      "#avisoGeometryStrokeOpacity",
      "#avisoGeometryStrokeOpacityOutput",
      commonValue(entries.map(entry => Number(entry.paint["stroke-opacity"] ?? 1) * 100), value => Math.round(Number(value) * 100) / 100),
      100
    );
  }

  function selectAvisoGeometryStyle(styleId, event, forceToggle = false) {
    const current = geometrySelectionIds();
    const next = updateMultiSelection(current, styleId, avisoGeometryRenderOrder, event, state.ui.avisoGeometrySelectionAnchorId, forceToggle);
    state.ui.selectedAvisoGeometryStyleIds = next;
    state.ui.selectedAvisoGeometryStyleId = next.includes(styleId) ? styleId : next[next.length - 1];
    if (!event.shiftKey) state.ui.avisoGeometrySelectionAnchorId = styleId;
    drafts.avisoGeometry = null;
    renderAvisoGeometry();
    setStatus(`${next.length} geometry style${next.length === 1 ? "" : "s"} selected`, "info");
  }

  function applyAvisoGeometry() {
    const entries = selectedAvisoGeometryEntries();
    if (!entries.length) return;
    const multi = entries.length > 1;
    const allArea = entries.every(entry => entry.objectType === "Area");
    const changed = {};
    const shouldApply = selector => !multi || wasControlTouched(selector);

    if (allArea && shouldApply("#avisoGeometryFillColor") && $("#avisoGeometryFillColor").value) {
      changed.fill = normalizeHex($("#avisoGeometryFillColor").value, entries[0].paint.fill || "#000000").toUpperCase();
    }
    if (allArea && shouldApply("#avisoGeometryFillOpacity")) {
      changed["fill-opacity"] = clamp(Number($("#avisoGeometryFillOpacity").value) / 100, 0, 1);
    }
    if (shouldApply("#avisoGeometryStrokeColor") && $("#avisoGeometryStrokeColor").value) {
      changed.stroke = normalizeHex($("#avisoGeometryStrokeColor").value, entries[0].paint.stroke || entries[0].paint.fill || "#000000").toUpperCase();
    }
    if (shouldApply("#avisoGeometryStrokeWidth") && $("#avisoGeometryStrokeWidth").value !== "") {
      changed["stroke-width"] = Math.max(0, Number($("#avisoGeometryStrokeWidth").value) || 0);
    }
    if (shouldApply("#avisoGeometryStrokeOpacity")) {
      changed["stroke-opacity"] = clamp(Number($("#avisoGeometryStrokeOpacity").value) / 100, 0, 1);
    }

    const visibility = $("#avisoGeometryVisible");
    const applyVisibility = !multi || visibility.dataset.touched === "true";

    if (!Object.keys(changed).length && !applyVisibility) {
      showToast("Change at least one shared geometry property");
      return;
    }

    let updatedCount = 0;
    entries.forEach(entry => {
      const style = ensureAvisoCatalogStyle(entry);
      Object.assign(style.paint, changed);
      entry.indices.forEach(index => {
        const properties = avisoFeatures()[index]?.properties;
        if (!properties) return;
        properties.style_id ||= entry.id;
        Object.assign(properties, changed);
        if (applyVisibility) properties.visible = visibility.checked;
        updatedCount += 1;
      });
    });

    drafts.avisoGeometry = null;
    markDirty(`${entries.length} geometry style${entries.length === 1 ? "" : "s"} updated`);
    showToast(`Updated ${updatedCount.toLocaleString()} geometry objects`, "success");
    renderAvisoGeometry();
  }

  function effectiveAvisoTextValue(index, entry, key) {
    const properties = avisoFeatures()[index]?.properties || {};
    return properties[key] ?? entry.paint[key] ?? AVISO_TEXT_DEFAULTS[key];
  }

  function renderAvisoText() {
    const entries = avisoStyleEntries("text");
    if (!entries.some(entry => entry.id === state.ui.selectedAvisoTextStyleId)) {
      state.ui.selectedAvisoTextStyleId = preferredAvisoStyleId("text");
      const entry = avisoStyleEntry(state.ui.selectedAvisoTextStyleId, "text") || entries[0];
      const index = entry?.indices[0] ?? 0;
      state.ui.selectedAvisoTextIndex = index;
      state.ui.selectedAvisoTextIndices = entry ? [index] : [];
      state.ui.avisoTextSelectionAnchorIndex = index;
      drafts.avisoTextStyle = null;
      drafts.avisoTextLabel = null;
    }
    const selectedEntry = avisoStyleEntry(state.ui.selectedAvisoTextStyleId, "text") || entries[0];
    if (!selectedEntry) return;

    const styleSelect = $("#avisoTextStyleSelect");
    styleSelect.innerHTML = entries.map(entry => `<option value="${escapeHtml(entry.id)}">${escapeHtml(entry.name)} · ${entry.count.toLocaleString()}</option>`).join("");
    styleSelect.value = selectedEntry.id;
    $("#avisoTextStyleCount").textContent = `${entries.length} styles`;

    const selectedIndices = textSelectionIndices(selectedEntry);
    const selectedSet = new Set(selectedIndices);
    const features = avisoFeatures();
    const filtered = filteredAvisoTextIndices(selectedEntry);
    const renderLimit = 500;
    const shown = filtered.slice(0, renderLimit);
    avisoTextRenderOrder = shown.slice();

    $("#avisoTextSearch").value = state.ui.avisoTextSearch;
    $("#avisoTextLabelCount").textContent = `(${selectedEntry.count.toLocaleString()})`;
    $("#avisoTextSelectionCount").textContent = `${selectedIndices.length} selected`;
    $("#avisoTextSelectionCount").title = filtered.length === selectedEntry.count ? "" : `${filtered.length.toLocaleString()} matching labels`;
    $("#avisoTextLabelList").innerHTML = shown.map(index => {
      const properties = features[index]?.properties || {};
      const label = properties["text-field"] || properties.name || `Label ${index + 1}`;
      const selected = selectedSet.has(index);
      const current = index === Number(state.ui.selectedAvisoTextIndex);
      return `<button type="button" role="option" aria-selected="${selected}" class="aviso-label-row ${selected ? "active" : ""} ${current ? "current" : ""}" data-aviso-text-index="${index}">
        <span class="aviso-select-box" data-aviso-selection-toggle="text" aria-hidden="true"></span>
        <span class="visibility-dot ${properties.visible !== false ? "visible" : ""}"></span>
        <span>${escapeHtml(label)}</span>
      </button>`;
    }).join("") + (filtered.length > renderLimit ? `<div class="aviso-list-message">Showing first ${renderLimit.toLocaleString()} labels</div>` : "") || `<div class="aviso-list-message">No matching labels</div>`;

    renderAvisoTextEditor(selectedEntry);
  }

  function renderAvisoTextEditor(entry = avisoStyleEntry(state.ui.selectedAvisoTextStyleId, "text")) {
    if (!entry) return;
    const indices = textSelectionIndices(entry);
    if (!indices.length) return;
    const features = avisoFeatures();
    const primaryFeature = features[state.ui.selectedAvisoTextIndex] || features[indices[0]];
    const primaryProperties = primaryFeature?.properties || {};
    const multi = indices.length > 1;
    const primaryText = primaryProperties["text-field"] || primaryProperties.name || "";

    $("#avisoTextCaption").textContent = multi ? `${indices.length} labels selected` : (primaryText || "Text label");
    $("#avisoTextStyleCaption").textContent = entry.name;

    const textInput = $("#avisoTextValue");
    textInput.value = multi ? "" : primaryText;
    textInput.placeholder = multi ? "Multiple labels selected" : "Label text";
    textInput.title = multi ? "Text content is individual and cannot be batch edited." : "";
    textInput.dataset.touched = "false";

    setCommonCheckbox("#avisoTextVisible", indices.map(index => features[index]?.properties?.visible !== false));
    setCommonInput(
      "#avisoTextFont",
      commonValue(indices.map(index => effectiveAvisoTextValue(index, entry, "text-font")), value => String(value || "Arial")),
      value => String(value || "Arial")
    );
    setCommonInput(
      "#avisoTextSize",
      commonValue(indices.map(index => effectiveAvisoTextValue(index, entry, "text-size")), value => Number(value)),
      value => String(value)
    );
    setCommonColor(
      "#avisoTextColor",
      "#avisoTextColorPicker",
      "#avisoTextColorPair",
      commonValue(indices.map(index => effectiveAvisoTextValue(index, entry, "text-color")), value => normalizeHex(value, "#808080")),
      "#808080"
    );
    setCommonColor(
      "#avisoTextHaloColor",
      "#avisoTextHaloPicker",
      "#avisoTextHaloPair",
      commonValue(indices.map(index => effectiveAvisoTextValue(index, entry, "text-halo-color")), value => normalizeHex(value, "#000000")),
      "#000000"
    );
    setCommonInput(
      "#avisoTextHaloWidth",
      commonValue(indices.map(index => effectiveAvisoTextValue(index, entry, "text-halo-width")), value => Number(value)),
      value => String(value)
    );
    const zoomCommon = commonValue(
      indices.map(index => effectiveAvisoTextValue(index, entry, "zoomLevel")),
      value => Math.round(clamp(value ?? 6, 0, 14))
    );
    setCommonInput("#avisoTextZoomLevel", zoomCommon, value => String(value));
    const zoomSlider = $("#avisoTextZoomSlider");
    resetControlFlags(zoomSlider, zoomCommon.mixed);
    zoomSlider.value = String(Math.round(clamp(zoomCommon.value ?? 6, 0, 14)));
    $("#avisoTextZoomMeaning").textContent = zoomCommon.mixed
      ? "Mixed zoom levels"
      : (MAP_ZOOM_LABELS[Math.round(clamp(zoomCommon.value ?? 6, 0, 14))] || "Zoom visibility");

    const scope = $("#avisoTextApplyTarget");
    scope.options[0].textContent = multi ? `Selected labels (${indices.length})` : "Selected label";
    scope.options[1].textContent = `Current text group (${entry.count.toLocaleString()})`;
    const allTextCount = avisoFeatures().filter(isAvisoTextFeature).length;
    scope.options[2].textContent = `All AVISO text (${allTextCount.toLocaleString()})`;
    if (!["selection", "group", "all"].includes(scope.value)) scope.value = "selection";
    updateAvisoTextApplyScopeUI(entry, indices);
  }
  function updateAvisoTextApplyScopeUI(entry = avisoStyleEntry(state.ui.selectedAvisoTextStyleId, "text"), indices = entry ? textSelectionIndices(entry) : []) {
    if (!entry || !indices.length) return;
    const scope = $("#avisoTextApplyTarget").value;
    const selectionScope = scope === "selection";
    const multi = indices.length > 1;
    const textInput = $("#avisoTextValue");
    const visible = $("#avisoTextVisible");
    textInput.disabled = !selectionScope || multi;
    visible.disabled = !selectionScope;
    textInput.closest(".field")?.classList.toggle("scope-disabled", !selectionScope);
    visible.closest(".check-field")?.classList.toggle("scope-disabled", !selectionScope);
    const button = $("#avisoTextApplyButton");
    button.textContent = "Update";
    if (scope === "selection") button.title = multi ? `Update changed properties on ${indices.length} selected labels` : "Update the selected label";
    else if (scope === "group") button.title = `Update typography, halo and zoom visibility for ${entry.name}`;
    else button.title = "Update typography, halo and zoom visibility for all AVISO text";
  }

  function applyAvisoTextScope() {
    const scope = $("#avisoTextApplyTarget").value;
    if (scope === "group") applyAvisoTextStyle("style");
    else if (scope === "all") applyAvisoTextStyle("all");
    else applyAvisoTextSelection();
  }

  function selectAvisoTextLabel(index, event, forceToggle = false) {
    const entry = avisoStyleEntry(state.ui.selectedAvisoTextStyleId, "text");
    if (!entry) return;
    const current = textSelectionIndices(entry);
    const next = updateMultiSelection(current, index, avisoTextRenderOrder, event, Number(state.ui.avisoTextSelectionAnchorIndex), forceToggle);
    state.ui.selectedAvisoTextIndices = next;
    state.ui.selectedAvisoTextIndex = next.includes(index) ? index : next[next.length - 1];
    if (!event.shiftKey) state.ui.avisoTextSelectionAnchorIndex = index;
    drafts.avisoTextLabel = null;
    renderAvisoText();
    setStatus(`${next.length} text label${next.length === 1 ? "" : "s"} selected`, "info");
  }

  function buildAvisoTextPaint(entry, onlyTouched) {
    const paint = {};
    const add = (key, selector, reader) => {
      const control = $(selector);
      if (!control) return;
      if (onlyTouched && control.dataset.touched !== "true") return;
      if (control.value === "") return;
      paint[key] = reader(control.value);
    };
    add("text-font", "#avisoTextFont", value => value.trim() || "Arial");
    add("text-size", "#avisoTextSize", value => clamp(Number(value), 1, 80));
    add("text-color", "#avisoTextColor", value => normalizeHex(value, entry.paint["text-color"] || "#808080").toUpperCase());
    add("text-halo-color", "#avisoTextHaloColor", value => normalizeHex(value, entry.paint["text-halo-color"] || "#000000").toUpperCase());
    add("text-halo-width", "#avisoTextHaloWidth", value => Math.max(0, Number(value) || 0));
    add("zoomLevel", "#avisoTextZoomLevel", value => Math.round(clamp(value, 0, 14)));
    return paint;
  }

  function applyAvisoTextSelection() {
    const entry = avisoStyleEntry(state.ui.selectedAvisoTextStyleId, "text");
    if (!entry) return;
    const indices = textSelectionIndices(entry);
    if (!indices.length) return;
    const multi = indices.length > 1;
    const paint = buildAvisoTextPaint(entry, multi);
    const visibility = $("#avisoTextVisible");
    const applyVisibility = !multi || visibility.dataset.touched === "true";
    let textChanged = false;

    if (!multi) {
      const index = indices[0];
      const properties = avisoFeatures()[index]?.properties;
      if (properties) {
        const previousText = properties["text-field"] || "";
        const nextText = $("#avisoTextValue").value;
        if (nextText !== previousText) {
          properties["text-field"] = nextText;
          if (!properties.name || properties.name === previousText) properties.name = nextText;
          textChanged = true;
        }
      }
    }

    if (!Object.keys(paint).length && !applyVisibility && !textChanged) {
      showToast("Change at least one shared text property");
      return;
    }

    let updatedCount = 0;
    indices.forEach(index => {
      const properties = avisoFeatures()[index]?.properties;
      if (!properties) return;
      properties.style_id ||= entry.id;
      Object.assign(properties, paint);
      if (applyVisibility) properties.visible = visibility.checked;
      updatedCount += 1;
    });

    drafts.avisoTextLabel = null;
    markDirty(`${updatedCount} AVISO label${updatedCount === 1 ? "" : "s"} updated`);
    showToast(`Updated ${updatedCount.toLocaleString()} text label${updatedCount === 1 ? "" : "s"}`, "success");
    renderAvisoText();
  }

  function applyAvisoTextStyle(scope = "style") {
    const currentEntry = avisoStyleEntry(state.ui.selectedAvisoTextStyleId, "text");
    if (!currentEntry) return;
    const selectedCount = textSelectionIndices(currentEntry).length;
    const textPaint = buildAvisoTextPaint(currentEntry, selectedCount > 1);
    if (!Object.keys(textPaint).length) {
      showToast("Change at least one shared text style property");
      return;
    }

    const targets = scope === "all" ? avisoStyleEntries("text") : [currentEntry];
    let updatedCount = 0;
    targets.forEach(entry => {
      const style = ensureAvisoCatalogStyle(entry);
      Object.assign(style.paint, textPaint);
      entry.indices.forEach(index => {
        const properties = avisoFeatures()[index]?.properties;
        if (!properties) return;
        properties.style_id ||= entry.id;
        Object.assign(properties, textPaint);
        updatedCount += 1;
      });
    });

    drafts.avisoTextStyle = null;
    markDirty(scope === "all" ? "All AVISO text styles updated" : `${currentEntry.name} updated`);
    showToast(`Updated ${updatedCount.toLocaleString()} text labels`, "success");
    renderAvisoText();
  }

  function revertAvisoEditor() {
    if (state.ui.avisoView === "geometry") {
      drafts.avisoGeometry = null;
      renderAvisoGeometryEditor();
    } else {
      drafts.avisoTextStyle = null;
      drafts.avisoTextLabel = null;
      renderAvisoTextEditor();
    }
  }


  function ensureProfileRimcas(profile = activeProfile()) {
    profile.rimcas ||= {};
    const rimcas = profile.rimcas;
    if (!Array.isArray(rimcas.timer) || rimcas.timer.length !== 5) rimcas.timer = [60, 45, 30, 15, 0];
    if (!Array.isArray(rimcas.timer_lvp) || rimcas.timer_lvp.length !== 5) rimcas.timer_lvp = [120, 90, 60, 30, 0];
    rimcas.stage_two_speed_threshold_kt = Number.isFinite(Number(rimcas.stage_two_speed_threshold_kt)) ? Number(rimcas.stage_two_speed_threshold_kt) : 25;
    rimcas.enabled = rimcas.enabled !== false;
    rimcas.rimcas_label_only = rimcas.rimcas_label_only !== false;
    rimcas.use_red_symbol_for_emergencies = rimcas.use_red_symbol_for_emergencies !== false;
    if (!Array.isArray(rimcas.inactive_alerts)) rimcas.inactive_alerts = [];
    if (!Array.isArray(rimcas.runways)) rimcas.runways = [];
    ALERT_COLOR_FIELDS.forEach(([, key, fallback]) => { if (!isColorObject(rimcas[key])) rimcas[key] = hexToColor(fallback); });
    return rimcas;
  }

  function ensureAlertsDraft() {
    const profileId = state.activeProfileId;
    if (!drafts.alerts || drafts.alerts.profileId !== profileId) {
      const rimcas = clone(ensureProfileRimcas());
      const runtimeAlerts = state.runtime.alerts ||= { visibility: "normal", runways: clone(DEFAULT_ALERT_RUNWAYS) };
      if (!Array.isArray(runtimeAlerts.runways)) runtimeAlerts.runways = clone(DEFAULT_ALERT_RUNWAYS);
      const profileRunways = rimcas.runways.length ? rimcas.runways : runtimeAlerts.runways;
      const profileVisibility = ["normal", "lvp"].includes(rimcas.visibility)
        ? rimcas.visibility
        : runtimeAlerts.visibility;
      drafts.alerts = {
        profileId,
        data: {
          enabled: rimcas.enabled !== false && state.settings.rimcas !== false,
          visibility: profileVisibility === "lvp" ? "lvp" : "normal",
          runways: clone(profileRunways),
          rimcas
        }
      };
    }
    return drafts.alerts.data;
  }

  function setAlertsView(view) {
    if (!["active", "runways", "timing", "appearance"].includes(view)) return;
    captureAlertsDraft();
    state.ui.alertsView = view;
    renderAlerts();
  }

  function renderAlerts() {
    const data = ensureAlertsDraft();
    $$('[data-alerts-view]').forEach(button => button.classList.toggle("active", button.dataset.alertsView === state.ui.alertsView));
    $$('[data-alerts-view-panel]').forEach(panel => panel.classList.toggle("active", panel.dataset.alertsViewPanel === state.ui.alertsView));
    $("#alertRimcasEnabled").checked = data.enabled;
    $("#alertLabelOnly").checked = Boolean(data.rimcas.rimcas_label_only);
    $("#alertRedEmergency").checked = Boolean(data.rimcas.use_red_symbol_for_emergencies);
    const inactive = new Set((data.rimcas.inactive_alerts || []).map(String));
    $("#alertTypeGrid").innerHTML = ALERT_TYPES.map(alert => `<label class="alert-toggle-card ${inactive.has(alert.id) ? "inactive" : ""}"><input type="checkbox" data-alert-type="${escapeHtml(alert.id)}" ${inactive.has(alert.id) ? "" : "checked"}><span><strong>${escapeHtml(alert.id)}</strong><small>${escapeHtml(alert.description)}</small></span></label>`).join("");

    $("#alertVisibilityMode").value = data.visibility;
    const runwayRowsHtml = data.runways.map((runway, index) => `<div class="alert-runway-row" data-alert-runway-index="${index}">
      <input aria-label="Runway pair" data-alert-runway-name="${index}" spellcheck="false" type="text" value="${escapeHtml(runway.id)}">
      <label class="alert-table-check"><input data-alert-runway-arr="${index}" type="checkbox" ${runway.arrival ? "checked" : ""}><span></span></label>
      <label class="alert-table-check"><input data-alert-runway-dep="${index}" type="checkbox" ${runway.departure ? "checked" : ""}><span></span></label>
      <label class="alert-table-check"><input data-alert-runway-closed="${index}" type="checkbox" ${runway.closed ? "checked" : ""}><span></span></label>
      <button class="alert-runway-remove" data-action="remove-alert-runway" data-index="${index}" title="Remove" type="button">×</button>
    </div>`).join("");
    $("#alertRunwayTable").innerHTML = `<div class="alert-runway-header"><span>Runway pair</span><span>ARR</span><span>DEP</span><span>Closed</span><span></span></div>${runwayRowsHtml || `<div class="aviso-list-message">No monitored runway pairs.</div>`}`;

    renderAlertTimerRow("#alertTimerNormal", data.rimcas.timer);
    renderAlertTimerRow("#alertTimerLvp", data.rimcas.timer_lvp);
    $("#alertSpeedThreshold").value = data.rimcas.stage_two_speed_threshold_kt;
    ALERT_COLOR_FIELDS.forEach(([prefix, key, fallback]) => {
      const hex = colorToHex(data.rimcas[key], fallback);
      $(`#${prefix}Picker`).value = hex;
      $(`#${prefix}Color`).value = hex.toUpperCase();
    });
  }

  function renderAlertTimerRow(selector, values) {
    const labels = ["Stage 1", "Stage 2", "Stage 3", "Stage 4", "Alert"];
    $(selector).innerHTML = labels.map((label, index) => `<label class="field"><span>${label}</span><input data-alert-timer-index="${index}" min="0" step="1" type="number" value="${Number(values[index] ?? 0)}"></label>`).join("");
  }

  function captureAlertsDraft() {
    if (!drafts.alerts || drafts.alerts.profileId !== state.activeProfileId) return ensureAlertsDraft();
    const data = drafts.alerts.data;
    if ($("#alertRimcasEnabled")) data.enabled = $("#alertRimcasEnabled").checked;
    data.rimcas.enabled = data.enabled;
    if ($("#alertLabelOnly")) data.rimcas.rimcas_label_only = $("#alertLabelOnly").checked;
    if ($("#alertRedEmergency")) data.rimcas.use_red_symbol_for_emergencies = $("#alertRedEmergency").checked;
    const alertChecks = $$('[data-alert-type]');
    if (alertChecks.length) data.rimcas.inactive_alerts = alertChecks.filter(input => !input.checked).map(input => input.dataset.alertType);
    if ($("#alertVisibilityMode")) data.visibility = $("#alertVisibilityMode").value === "lvp" ? "lvp" : "normal";
    const runwayRows = $$("[data-alert-runway-index]");
    if (runwayRows.length) data.runways = runwayRows.map(row => {
      const index = Number(row.dataset.alertRunwayIndex);
      return {
        id: String($(`[data-alert-runway-name="${index}"]`)?.value || "").trim().toUpperCase().replace(/\s*\/\s*/g, " / "),
        arrival: Boolean($(`[data-alert-runway-arr="${index}"]`)?.checked),
        departure: Boolean($(`[data-alert-runway-dep="${index}"]`)?.checked),
        closed: Boolean($(`[data-alert-runway-closed="${index}"]`)?.checked)
      };
    }).filter(row => row.id);
    const normalInputs = $$("#alertTimerNormal [data-alert-timer-index]");
    const lvpInputs = $$("#alertTimerLvp [data-alert-timer-index]");
    if (normalInputs.length) data.rimcas.timer = normalInputs.map(input => Math.max(0, Math.round(Number(input.value) || 0)));
    if (lvpInputs.length) data.rimcas.timer_lvp = lvpInputs.map(input => Math.max(0, Math.round(Number(input.value) || 0)));
    if ($("#alertSpeedThreshold")) data.rimcas.stage_two_speed_threshold_kt = Math.max(0, Math.round(Number($("#alertSpeedThreshold").value) || 0));
    data.rimcas.visibility = data.visibility;
    data.rimcas.runways = clone(data.runways);
    ALERT_COLOR_FIELDS.forEach(([prefix, key, fallback]) => {
      const input = $(`#${prefix}Color`);
      if (input) data.rimcas[key] = hexToColor(normalizeHex(input.value, fallback), data.rimcas[key]?.a ?? 255);
    });
    return data;
  }

  function applyAlerts() {
    const data = captureAlertsDraft();
    state.settings.rimcas = data.enabled;
    data.rimcas.enabled = data.enabled;
    data.rimcas.visibility = data.visibility;
    data.rimcas.runways = clone(data.runways);
    activeProfile().rimcas = clone(data.rimcas);
    state.runtime.alerts = { visibility: data.visibility, runways: clone(data.runways) };
    drafts.alerts = null;
    markDirty("Alert settings updated");
    postBridge("alerts.update", { profile: activeProfile().name, enabled: state.settings.rimcas, visibility: data.visibility, runways: clone(data.runways), rimcas: clone(data.rimcas) });
    renderAlerts();
    showToast("Alert settings updated", "success");
  }

  function revertAlerts() { drafts.alerts = null; renderAlerts(); }

  function setAllAlertTypes(active) {
    $$('[data-alert-type]').forEach(input => { input.checked = active; input.closest(".alert-toggle-card")?.classList.toggle("inactive", !active); });
  }

  function setAllAlertRunwayField(field, value = true) {
    captureAlertsDraft();
    ensureAlertsDraft().runways.forEach(runway => { runway[field] = value; });
    renderAlerts();
  }

  function addAlertRunway() {
    captureAlertsDraft();
    const input = window.prompt("Runway pair (for example 09L / 27R)", "");
    if (input == null) return;
    const normalized = input.trim().toUpperCase().replace(/\s*\/\s*/g, " / ");
    if (!/^\d{2}[LRC]? \/ \d{2}[LRC]?$/.test(normalized)) { showToast("Use a runway pair such as 09L / 27R", "error"); return; }
    const data = ensureAlertsDraft();
    if (data.runways.some(row => row.id === normalized)) { showToast("This runway pair is already monitored", "error"); return; }
    data.runways.push({ id: normalized, arrival: true, departure: true, closed: false });
    renderAlerts();
  }

  function removeAlertRunway(index) {
    captureAlertsDraft();
    ensureAlertsDraft().runways.splice(index, 1);
    renderAlerts();
  }

  function renderSettings() {
    const settings = state.settings;
    $("#settingsProfileFile").value = settings.profileFile;
    $("#settingsAvisoFile").value = settings.avisoFile;
    $("#settingsWatchFiles").checked = settings.watchFiles;
    $("#settingsBridgeMode").value = settings.bridgeMode;
    $("#settingsUpdateInterval").value = settings.updateInterval;
    ensureSelectValue($("#settingsResolutionPreset"), settings.resolutionPreset || "1080p");
    $("#settingsShowFps").checked = settings.showFps !== false;
    $("#settingsRuntimeSync").checked = settings.runtimeSync;
    $("#settingsConfirmDelete").checked = settings.confirmDelete;
    $("#settingsRimcas").checked = settings.rimcas;
    $("#settingsVacdm").checked = settings.vacdm;
    $("#settingsCpdlc").checked = settings.cpdlc;
    $("#settingsApproachWindows").checked = settings.approachWindows;
    if (HOST_MODE) {
      const unsupported = [
        ["#settingsWatchFiles", "Automatic file watching is not supported by the native host."],
        ["#settingsBridgeMode", "The native host always uses WebView2."],
        ["#settingsUpdateInterval", "Runtime synchronization is event-driven."],
        ["#settingsRuntimeSync", "Runtime synchronization is always enabled by the native host."],
        ["#settingsVacdm", "Configure VACDM through the existing profile metadata/server URL."],
        ["#settingsCpdlc", "Configure CPDLC through the EuroScope vSMR settings dialog."],
        ["#settingsApproachWindows", "Inset windows are controlled from the runtime rail."]
      ];
      unsupported.forEach(([selector, title]) => {
        const control = $(selector);
        if (!control) return;
        control.disabled = true;
        control.title = title;
      });
      ["#settingsProfileFile", "#settingsAvisoFile"].forEach(selector => {
        const control = $(selector);
        if (control) control.readOnly = true;
      });
    }
  }

  function applySettings() {
    Object.assign(state.settings, {
      profileFile: $("#settingsProfileFile").value,
      avisoFile: $("#settingsAvisoFile").value,
      watchFiles: $("#settingsWatchFiles").checked,
      bridgeMode: $("#settingsBridgeMode").value,
      updateInterval: Number($("#settingsUpdateInterval").value) || 250,
      resolutionPreset: $("#settingsResolutionPreset").value || "1080p",
      showFps: $("#settingsShowFps").checked,
      runtimeSync: $("#settingsRuntimeSync").checked,
      confirmDelete: $("#settingsConfirmDelete").checked,
      rimcas: $("#settingsRimcas").checked,
      vacdm: $("#settingsVacdm").checked,
      cpdlc: $("#settingsCpdlc").checked,
      approachWindows: $("#settingsApproachWindows").checked
    });
    state.profiles.forEach(record => {
      record.data.targets ||= {};
      record.data.targets.small_icon_boost_resolution_preset = state.settings.resolutionPreset;
    });
    ensureProfileRimcas().enabled = state.settings.rimcas;
    markDirty("Settings updated");
    renderIcons();
    postBridge("settings.update", clone(state.settings));
  }

  function fillSelect(select, values, firstLabel, selectedValue = "") {
    const unique = [...new Set(values.filter(Boolean))];
    select.innerHTML = `<option value="">${escapeHtml(firstLabel)}</option>${unique.map(value => `<option value="${escapeHtml(value)}">${escapeHtml(value)}</option>`).join("")}`;
    select.value = unique.includes(selectedValue) ? selectedValue : "";
  }

  function ensureSelectValue(select, value) {
    const stringValue = String(value ?? "");
    if (![...select.options].some(option => option.value === stringValue)) {
      const option = document.createElement("option");
      option.value = option.textContent = stringValue;
      select.append(option);
    }
    select.value = stringValue;
  }

  function serializeProfiles(updateLastActive = false) {
    if (updateLastActive) state.metadata.last_active_profile = activeProfile().name || "";
    return [
      ...state.profiles.map(record => clone(record.data)),
      ...(state.profileExtras || []).map(entry => clone(entry)),
      { _vsmr: clone(state.metadata) }
    ];
  }

  function serializeStatePayload(updateLastActive = false) {
    return {
      profiles: serializeProfiles(updateLastActive),
      profileIdentities: state.profiles.map(record => ({
        id: record.id,
        currentName: String(record.data?.name || ""),
        persistedName: String(record.persistedName || "")
      })),
      aviso: clone(state.aviso),
      settings: clone(state.settings),
      runtime: clone(state.runtime),
      airport: state.airport,
      activeProfile: activeProfile().name || ""
    };
  }

  function saveAll() {
    if (!state.dirty || pending.save || state.airport !== state.hostAirport) return;
    const payload = serializeStatePayload(true);
    pending.save = postBridge("state.save", payload);
    setStatus("Saving configuration…", "info");
    updateCommandState();
    if (!HOST_MODE) {
      const requestId = pending.save;
      setTimeout(() => receiveHostMessage({
        version: PROTOCOL_VERSION,
        id: requestId,
        type: "state.saved",
        payload: { requestId, state: payload, message: "Preview state saved" }
      }), 0);
    }
  }

  function requestReload() {
    if (pending.reload || pending.save) return;
    if (state.dirty && !window.confirm("Discard unsaved changes and reload configuration from disk?")) return;
    pending.reload = postBridge("state.reload", {});
    setStatus("Reloading configuration…", "info");
    updateCommandState();
    if (!HOST_MODE) {
      const requestId = pending.reload;
      const fallback = createState(DEFAULT_DATA);
      setTimeout(() => receiveHostMessage({
        version: PROTOCOL_VERSION,
        id: requestId,
        type: "state.authoritative",
        payload: {
          requestId,
          reason: "reload",
          profiles: DEFAULT_DATA.profiles,
          aviso: DEFAULT_DATA.aviso,
          settings: fallback.settings,
          runtime: fallback.runtime,
          airport: fallback.airport,
          activeProfile: fallback.metadata?.last_active_profile || fallback.profiles[0]?.data?.name || ""
        }
      }), 0);
    }
  }

  function undoHistory() {
    if (!history.past.length || pending.save || pending.reload || state.airport !== state.hostAirport) return;
    const target = history.past.pop();
    history.future.push(history.present);
    if (history.future.length > HISTORY_LIMIT) history.future.shift();
    restoreHistorySnapshot(target);
    postBridge("state.undo", { state: serializeStatePayload() });
    showToast("Undone", "success");
  }

  function redoHistory() {
    if (!history.future.length || pending.save || pending.reload || state.airport !== state.hostAirport) return;
    const target = history.future.pop();
    history.past.push(history.present);
    if (history.past.length > HISTORY_LIMIT) history.past.shift();
    restoreHistorySnapshot(target);
    postBridge("state.redo", { state: serializeStatePayload() });
    showToast("Redone", "success");
  }

  function resetFromSupplied(showMessage = true) {
    if (!window.confirm("Restore the bundled vSMR profiles and LFPG AVISO data? Current staged changes will be replaced.")) return;
    if (HOST_MODE) {
      postBridge("state.reset", {});
      setStatus("Loading bundled profile and LFPG AVISO defaults…", "info");
      return;
    }
    const preservedUi = state.ui;
    const insetStates = clone(state.runtime.insets || { aviso: false, srw1: false, srw2: false });
    state = createState(DEFAULT_DATA);
    state.ui = preservedUi;
    state.runtime.insets = insetStates;
    state.runtime.avisoInsetVisible = Boolean(insetStates.aviso);
    Object.keys(drafts).forEach(key => drafts[key] = null);
    resetAvisoSelections();
    renderAll();
    markDirty("Supplied data restored");
    if (showMessage) showToast("Supplied LFPG data restored", "success");
  }

  function downloadJson(filename, value) {
    const blob = new Blob([JSON.stringify(value, null, 2)], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = filename;
    document.body.append(anchor);
    anchor.click();
    anchor.remove();
    setTimeout(() => URL.revokeObjectURL(url), 500);
  }

  function confirmDelete(message) {
    return !state.settings.confirmDelete || window.confirm(message);
  }

  function renderAll() {
    renderGlobalProfileSelect();
    renderAllProfileSections();
    renderAviso();
    renderAlerts();
    renderAvisoGroups();
    renderSettings();
    setPage(state.ui.page);
    setProfileTab(state.ui.profileTab);
    renderRuntimeMenu();
    syncSurfaceVisibility();
    updateContext();
  }

  function applyQueryState() {
    const params = new URLSearchParams(window.VSMR_PREVIEW_QUERY || location.search);
    const page = params.get("page");
    const tab = params.get("tab");
    if (PAGE_TITLES[page]) state.ui.page = page;
    if (PROFILE_TITLES[tab]) state.ui.profileTab = tab;
    const avisoView = params.get("aviso") || params.get("view");
    if (["geometry", "text"].includes(avisoView)) state.ui.avisoView = avisoView;
    const ui = params.get("ui");
    if (ui === "control" || params.get("control") === "1" || PAGE_TITLES[page]) state.ui.controlCenterOpen = true;
    if (ui === "runtime") state.ui.controlCenterOpen = false;
    if (["mode", "groups", "inset", "profile"].includes(params.get("popup"))) state.ui.runtimePopover = params.get("popup");
    if (["active", "runways", "timing", "appearance"].includes(params.get("alerts"))) state.ui.alertsView = params.get("alerts");
    if (params.get("tag")) state.ui.selectedTagId = params.get("tag");
    if (params.get("profile")) {
      const match = state.profiles.find(record => record.data.name.toLowerCase() === params.get("profile").toLowerCase());
      if (match) { state.activeProfileId = match.id; state.ui.managedProfileId = match.id; }
    }
  }

  function bindEvents() {
    document.addEventListener("click", event => {
      if (!event.target.closest("#ruleStatusDropdown")) setRuleStatusMenuOpen(false);
      if (!event.target.closest(".aviso-load-control")) setAvisoLoadMenuOpen(false);
      if (!event.target.closest(".rail-profile-control")) setRailProfilePopoverOpen(false);
      if (!event.target.closest("#runtimeMenu") && state.ui.runtimePopover) {
        state.ui.runtimePopover = "";
        renderRuntimeMenu();
      }

      const runtimePopoverButton = event.target.closest("[data-runtime-popover]");
      if (runtimePopoverButton) { setRuntimePopover(runtimePopoverButton.dataset.runtimePopover); return; }
      const runtimeMode = event.target.closest("[data-runtime-mode]");
      if (runtimeMode) { setRuntimeMode(runtimeMode.dataset.runtimeMode); return; }
      const runtimeProfile = event.target.closest("[data-runtime-profile]");
      if (runtimeProfile) {
        switchActiveProfile(runtimeProfile.dataset.runtimeProfile);
        postActiveProfileChange();
        state.ui.runtimePopover = "";
        renderRuntimeMenu();
        showToast(`Profile: ${activeProfile().name}`, "success");
        return;
      }
      const runtimeGroup = event.target.closest("[data-runtime-group]");
      if (runtimeGroup) { toggleRuntimeGroup(runtimeGroup.dataset.runtimeGroup); return; }
      const runtimeInset = event.target.closest("[data-runtime-inset]");
      if (runtimeInset) { toggleInsetWindow(runtimeInset.dataset.runtimeInset); return; }
      const insetPreset = event.target.closest("[data-inset-preset]");
      if (insetPreset) { loadAvisoPreset(insetPreset.dataset.insetPreset); return; }
      const groupContentType = event.target.closest("[data-aviso-group-content-type]");
      if (groupContentType) {
        state.ui.avisoGroupContentType = groupContentType.dataset.avisoGroupContentType;
        state.ui.avisoGroupContentSearch = "";
        renderAvisoGroupContentDialog();
        return;
      }
      const groupContentRow = event.target.closest("[data-group-content-key]");
      if (groupContentRow) { toggleAvisoGroupContentCandidate(groupContentRow.dataset.groupContentKey); return; }
      const groupVisibilityButton = event.target.closest('[data-action="toggle-aviso-group-visibility"]');
      if (groupVisibilityButton) { toggleRuntimeGroup(groupVisibilityButton.dataset.groupId); return; }
      const groupMemberRemove = event.target.closest('[data-action="remove-aviso-group-member"]');
      if (groupMemberRemove) { removeAvisoGroupMember(groupMemberRemove); return; }
      const avisoGroupRow = event.target.closest("[data-aviso-group-id]");
      if (avisoGroupRow) {
        state.ui.selectedAvisoGroupId = avisoGroupRow.dataset.avisoGroupId;
        drafts.avisoGroup = null;
        renderAvisoGroups();
        return;
      }

      const pageButton = event.target.closest("[data-page]");
      if (pageButton) { setPage(pageButton.dataset.page); return; }
      const tabButton = event.target.closest("[data-profile-tab]");
      if (tabButton) { setProfileTab(tabButton.dataset.profileTab); return; }
      const avisoViewButton = event.target.closest("[data-aviso-view]");
      if (avisoViewButton) { state.ui.avisoView = avisoViewButton.dataset.avisoView; renderAviso(); return; }
      const alertsViewButton = event.target.closest("[data-alerts-view]");
      if (alertsViewButton) { setAlertsView(alertsViewButton.dataset.alertsView); return; }
      const treeToggle = event.target.closest("[data-tree-toggle]");
      if (treeToggle) {
        const store = treeState[treeToggle.dataset.treeToggle];
        const key = treeToggle.dataset.treeKey;
        if (store && key) {
          if (store.has(key)) store.delete(key); else store.add(key);
          if (treeToggle.dataset.treeToggle === "colors") renderColors();
          if (treeToggle.dataset.treeToggle === "tags") renderTags();
        }
        return;
      }
      const colorRow = event.target.closest("[data-color-path]");
      if (colorRow) { state.ui.selectedColorPath = colorRow.dataset.colorPath; drafts.color = null; renderColors(); return; }
      const tagRow = event.target.closest("[data-tag-id]");
      if (tagRow) { state.ui.selectedTagId = tagRow.dataset.tagId; drafts.tag = null; renderTags(); return; }
      const ruleRow = event.target.closest("[data-rule-index]");
      if (ruleRow) { state.ui.selectedRuleIndex = Number(ruleRow.dataset.ruleIndex); drafts.rule = null; renderRules(); return; }
      const modeRow = event.target.closest("[data-mode-index]");
      if (modeRow) { state.ui.selectedModeIndex = Number(modeRow.dataset.modeIndex); drafts.mode = null; renderModes(); return; }
      const profileRow = event.target.closest("[data-managed-profile-id]");
      if (profileRow) { state.ui.managedProfileId = profileRow.dataset.managedProfileId; drafts.profile = null; renderProfilesManager(); return; }
      const avisoGeometryStyleRow = event.target.closest("[data-aviso-geometry-style]");
      if (avisoGeometryStyleRow) {
        const forceToggle = Boolean(event.target.closest('[data-aviso-selection-toggle="geometry"]'));
        selectAvisoGeometryStyle(avisoGeometryStyleRow.dataset.avisoGeometryStyle, event, forceToggle);
        return;
      }
      const avisoTextStyleRow = event.target.closest("[data-aviso-text-style]");
      if (avisoTextStyleRow) {
        state.ui.selectedAvisoTextStyleId = avisoTextStyleRow.dataset.avisoTextStyle;
        const entry = avisoStyleEntry(state.ui.selectedAvisoTextStyleId, "text");
        const index = entry?.indices?.[0] ?? 0;
        state.ui.selectedAvisoTextIndex = index;
        state.ui.selectedAvisoTextIndices = entry ? [index] : [];
        state.ui.avisoTextSelectionAnchorIndex = index;
        state.ui.avisoTextSearch = "";
        drafts.avisoTextStyle = null;
        drafts.avisoTextLabel = null;
        renderAvisoText();
        return;
      }
      const avisoTextRow = event.target.closest("[data-aviso-text-index]");
      if (avisoTextRow) {
        const forceToggle = Boolean(event.target.closest('[data-aviso-selection-toggle="text"]'));
        selectAvisoTextLabel(Number(avisoTextRow.dataset.avisoTextIndex), event, forceToggle);
        return;
      }
      const actionButton = event.target.closest("[data-action]");
      if (actionButton) handleAction(actionButton.dataset.action, actionButton);
    });

    $("#globalProfileSelect").addEventListener("change", event => {
      switchActiveProfile(event.target.value);
      postActiveProfileChange();
      setRailProfilePopoverOpen(false);
    });
    $("#avisoTextStyleSelect").addEventListener("change", event => {
      state.ui.selectedAvisoTextStyleId = event.target.value;
      const entry = avisoStyleEntry(state.ui.selectedAvisoTextStyleId, "text");
      const index = entry?.indices?.[0] ?? 0;
      state.ui.selectedAvisoTextIndex = index;
      state.ui.selectedAvisoTextIndices = entry ? [index] : [];
      state.ui.avisoTextSelectionAnchorIndex = index;
      state.ui.avisoTextSearch = "";
      drafts.avisoTextStyle = null;
      drafts.avisoTextLabel = null;
      renderAvisoText();
    });
    $("#tagLabelFontSize").addEventListener("input", event => { $("#tagLabelFontSizeOutput").value = String(Math.round(clamp(event.target.value, 1, 5))); });

    $("#colorSearch").addEventListener("input", renderColors);
    $("#colorHex").addEventListener("input", event => setColorDraftFromHex(event.target.value));
    $("#nativeColorPicker").addEventListener("input", event => setColorDraftFromHex(event.target.value));
    $("#colorHue").addEventListener("input", event => setColorDraftFromHsv(Number(event.target.value), drafts.color?.s ?? 0, drafts.color?.v ?? 1));
    [["colorRed", "r"], ["colorGreen", "g"], ["colorBlue", "b"]].forEach(([id]) => {
      $("#" + id).addEventListener("input", () => setColorDraftFromRgb($("#colorRed").value, $("#colorGreen").value, $("#colorBlue").value));
    });
    $("#colorOpacity").addEventListener("input", event => {
      if (!drafts.color) return;
      drafts.color.opacity = Number(event.target.value);
      syncColorEditorControls();
    });
    const colorPalette = $("#colorSvPalette");
    colorPalette.addEventListener("pointerdown", event => {
      colorPalette.setPointerCapture(event.pointerId);
      colorPalette.dataset.dragging = "true";
      updateColorFromPalettePointer(event);
    });
    colorPalette.addEventListener("pointermove", event => {
      if (colorPalette.dataset.dragging === "true") updateColorFromPalettePointer(event);
    });
    const stopColorDrag = event => {
      colorPalette.dataset.dragging = "false";
      if (event.pointerId != null && colorPalette.hasPointerCapture?.(event.pointerId)) colorPalette.releasePointerCapture(event.pointerId);
    };
    colorPalette.addEventListener("pointerup", stopColorDrag);
    colorPalette.addEventListener("pointercancel", stopColorDrag);
    colorPalette.addEventListener("keydown", event => {
      if (!drafts.color || !["ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown"].includes(event.key)) return;
      event.preventDefault();
      const step = event.shiftKey ? .05 : .01;
      const nextS = drafts.color.s + (event.key === "ArrowRight" ? step : event.key === "ArrowLeft" ? -step : 0);
      const nextV = drafts.color.v + (event.key === "ArrowUp" ? step : event.key === "ArrowDown" ? -step : 0);
      setColorDraftFromHsv(drafts.color.h, nextS, nextV);
    });

    ["fixedPixelIconScale", "smallIconBoostFactor"].forEach(id => $("#" + id).addEventListener("input", event => {
      $("#" + id + "Output").value = `${Number(event.target.value).toFixed(2)}×`;
      renderIconSymbolPreview();
    }));
    $("#fixedPixelIconSize").addEventListener("change", updateIconDependencies);
    $("#smallIconBoost").addEventListener("change", updateIconDependencies);
    ["targetIconStyle"].forEach(id => $("#" + id).addEventListener("change", renderIconSymbolPreview));
    $("#showPrimaryTarget").addEventListener("change", renderIconSymbolPreview);

    $("#tagLineGrid").addEventListener("focusin", event => { if (event.target.matches(".tag-line-input")) activeTagInput = event.target; });
    $("#tagDetailedInherits").addEventListener("change", () => {
      captureTagDraft();
      drafts.tag.data.definition_detailed_inherits_normal = $("#tagDetailedInherits").checked;
      renderTagEditor();
    });

    ["Target", "Tag", "Text"].forEach(kind => {
      $(`#ruleUse${kind}Color`).addEventListener("change", () => {
        const enabled = $(`#ruleUse${kind}Color`).checked;
        $(`#rule${kind}Color`).disabled = !enabled;
        $(`#rule${kind}Picker`).disabled = !enabled;
      });
      $(`#rule${kind}Color`).addEventListener("input", event => {
        const hex = normalizeHex(event.target.value, "#ffffff");
        $(`#rule${kind}Picker`).value = hex;
        $(`#rule${kind}Picker`).closest("label").style.setProperty("--swatch-color", hex);
      });
      $(`#rule${kind}Picker`).addEventListener("input", event => {
        $(`#rule${kind}Color`).value = event.target.value.toUpperCase();
        event.target.closest("label").style.setProperty("--swatch-color", event.target.value);
      });
    });

    $("#ruleStatusButton").addEventListener("click", event => {
      event.stopPropagation();
      setRuleStatusMenuOpen($("#ruleStatusMenu").hidden);
    });
    $("#ruleStatusAll").addEventListener("change", event => {
      $$("#ruleStatusOptions input[data-rule-status]").forEach(input => { input.checked = event.target.checked; });
      updateRuleStatusDropdownLabel();
    });
    $("#ruleStatusOptions").addEventListener("change", event => {
      if (!event.target.matches("input[data-rule-status]")) return;
      updateRuleStatusDropdownLabel();
    });
    $("#ruleStatusDropdown").addEventListener("keydown", event => {
      if (event.key === "Escape") { setRuleStatusMenuOpen(false); $("#ruleStatusButton").focus(); }
    });
    $("#modeBlockedSquawkInput").addEventListener("keydown", event => {
      if (event.key === "Enter") {
        event.preventDefault();
        addModeBlockedSquawk();
      }
    });

    $("#avisoGeometrySearch").addEventListener("input", event => {
      state.ui.avisoGeometrySearch = event.target.value;
      renderAvisoGeometry();
    });
    $("#avisoTextSearch").addEventListener("input", event => {
      state.ui.avisoTextSearch = event.target.value;
      renderAvisoText();
    });
    $("#avisoGroupSearch").addEventListener("input", event => {
      captureAvisoGroupDraft();
      state.ui.avisoGroupSearch = event.target.value;
      renderAvisoGroups();
    });
    $("#avisoGroupMemberSearch").addEventListener("input", event => {
      captureAvisoGroupDraft();
      state.ui.avisoGroupMemberSearch = event.target.value;
      renderAvisoGroupEditor();
    });
    $("#avisoGroupName").addEventListener("input", event => {
      const draft = captureAvisoGroupDraft();
      if (draft) {
        draft.data.name = event.target.value;
        $("#avisoGroupCaption").textContent = event.target.value || "Group";
      }
    });
    $("#avisoGroupContentSearch").addEventListener("input", event => {
      state.ui.avisoGroupContentSearch = event.target.value;
      renderAvisoGroupContentDialog();
    });
    $("#avisoGroupContentDialog").addEventListener("close", () => { avisoGroupContentDraft = null; });
    const avisoGroupList = $("#avisoGroupList");
    avisoGroupList.addEventListener("dragstart", event => {
      const row = event.target.closest("[data-aviso-group-id]");
      if (!row || String(state.ui.avisoGroupSearch || "").trim()) {
        event.preventDefault();
        return;
      }
      draggedAvisoGroupId = row.dataset.avisoGroupId;
      row.classList.add("dragging");
      event.dataTransfer.effectAllowed = "move";
      event.dataTransfer.setData("text/plain", draggedAvisoGroupId);
    });
    avisoGroupList.addEventListener("dragover", event => {
      const row = event.target.closest("[data-aviso-group-id]");
      if (!row || !draggedAvisoGroupId || row.dataset.avisoGroupId === draggedAvisoGroupId) return;
      event.preventDefault();
      event.dataTransfer.dropEffect = "move";
      $$(".aviso-group-row.drop-target", avisoGroupList).forEach(item => item.classList.remove("drop-target", "drop-after"));
      row.classList.add("drop-target");
      if (event.clientY > row.getBoundingClientRect().top + row.getBoundingClientRect().height / 2) row.classList.add("drop-after");
    });
    avisoGroupList.addEventListener("drop", event => {
      const target = event.target.closest("[data-aviso-group-id]");
      if (!target || !draggedAvisoGroupId || target.dataset.avisoGroupId === draggedAvisoGroupId) return;
      event.preventDefault();
      const groups = avisoGroups();
      const sourceIndex = groups.findIndex(group => group.id === draggedAvisoGroupId);
      const targetId = target.dataset.avisoGroupId;
      const after = target.classList.contains("drop-after");
      if (sourceIndex < 0) return;
      const [moved] = groups.splice(sourceIndex, 1);
      let insertionIndex = groups.findIndex(group => group.id === targetId);
      if (insertionIndex < 0) insertionIndex = groups.length;
      else if (after) insertionIndex += 1;
      groups.splice(insertionIndex, 0, moved);
      state.ui.selectedAvisoGroupId = moved.id;
      draggedAvisoGroupId = "";
      markDirty("AVISO groups reordered");
      postBridge("aviso.groups.update", { groups: clone(groups), aviso: clone(state.aviso) });
      renderAvisoGroups();
      renderRuntimeMenu();
    });
    avisoGroupList.addEventListener("dragend", () => {
      draggedAvisoGroupId = "";
      $$(".aviso-group-row", avisoGroupList).forEach(item => item.classList.remove("dragging", "drop-target", "drop-after"));
    });

    [
      ["avisoGeometryFillColor", "avisoGeometryFillPicker", "avisoGeometryFillPair"],
      ["avisoGeometryStrokeColor", "avisoGeometryStrokePicker", "avisoGeometryStrokePair"],
      ["avisoTextColor", "avisoTextColorPicker", "avisoTextColorPair"],
      ["avisoTextHaloColor", "avisoTextHaloPicker", "avisoTextHaloPair"]
    ].forEach(([textId, pickerId, pairId]) => {
      $("#" + textId).addEventListener("input", event => {
        $("#" + pickerId).value = normalizeHex(event.target.value, $("#" + pickerId).value || "#808080");
        markControlTouched(event.target);
        markControlTouched($("#" + pickerId));
        $("#" + pairId).classList.remove("mixed");
      });
      $("#" + pickerId).addEventListener("input", event => {
        $("#" + textId).value = event.target.value.toUpperCase();
        markControlTouched(event.target);
        markControlTouched($("#" + textId));
        $("#" + pairId).classList.remove("mixed");
      });
    });
    ["avisoGeometryFillOpacity", "avisoGeometryStrokeOpacity"].forEach(id => {
      $("#" + id).addEventListener("input", event => {
        $("#" + id + "Output").value = `${Math.round(Number(event.target.value))}%`;
        event.target.closest(".range-control")?.classList.remove("mixed");
        markControlTouched(event.target);
      });
    });
    ["avisoGeometryStrokeWidth", "avisoTextFont", "avisoTextSize", "avisoTextHaloWidth"].forEach(id => {
      $("#" + id).addEventListener("input", event => markControlTouched(event.target));
      $("#" + id).addEventListener("change", event => markControlTouched(event.target));
    });
    const syncAvisoTextZoom = (sourceId) => {
      const number = $("#avisoTextZoomLevel");
      const slider = $("#avisoTextZoomSlider");
      const value = Math.round(clamp(sourceId === "avisoTextZoomSlider" ? slider.value : number.value, 0, 14));
      number.value = String(value);
      slider.value = String(value);
      $("#avisoTextZoomMeaning").textContent = MAP_ZOOM_LABELS[value] || `Zoom level ${value}`;
      markControlTouched(number);
      markControlTouched(slider);
    };
    $("#avisoTextZoomLevel").addEventListener("input", () => syncAvisoTextZoom("avisoTextZoomLevel"));
    $("#avisoTextZoomSlider").addEventListener("input", () => syncAvisoTextZoom("avisoTextZoomSlider"));
    ["avisoGeometryVisible", "avisoTextVisible"].forEach(id => {
      $("#" + id).addEventListener("change", event => {
        event.target.indeterminate = false;
        markControlTouched(event.target);
      });
    });
    $("#avisoTextApplyTarget").addEventListener("change", () => updateAvisoTextApplyScopeUI());
    document.addEventListener("change", event => {
      if (event.target.matches("#runtimePresetLinked")) toggleRuntimePresetLinked(event.target.checked);
      if (event.target.matches("[data-alert-type]")) event.target.closest(".alert-toggle-card")?.classList.toggle("inactive", !event.target.checked);
    });
    ALERT_COLOR_FIELDS.forEach(([prefix]) => {
      const picker = $(`#${prefix}Picker`);
      const text = $(`#${prefix}Color`);
      picker.addEventListener("input", () => { text.value = picker.value.toUpperCase(); });
      text.addEventListener("input", () => { picker.value = normalizeHex(text.value, picker.value); });
    });
    $("#resourceGithubLoadConfirm").addEventListener("click", loadResourceFromGithub);
    $("#resourceGithubUrl").addEventListener("keydown", event => {
      if (event.key === "Enter") { event.preventDefault(); loadResourceFromGithub(); }
    });

    $("#avisoGeometryStyleList").addEventListener("keydown", event => {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "a") {
        event.preventDefault();
        const ids = filteredAvisoGeometryEntries().map(entry => entry.id);
        if (ids.length) {
          state.ui.selectedAvisoGeometryStyleIds = ids;
          state.ui.selectedAvisoGeometryStyleId = ids[ids.length - 1];
          state.ui.avisoGeometrySelectionAnchorId = ids[0];
          renderAvisoGeometry();
        }
      } else if (event.key === "Escape") {
        const id = state.ui.selectedAvisoGeometryStyleId;
        state.ui.selectedAvisoGeometryStyleIds = id ? [id] : [];
        renderAvisoGeometry();
      }
    });
    $("#avisoTextLabelList").addEventListener("keydown", event => {
      const entry = avisoStyleEntry(state.ui.selectedAvisoTextStyleId, "text");
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "a") {
        event.preventDefault();
        const indices = filteredAvisoTextIndices(entry);
        if (indices.length) {
          state.ui.selectedAvisoTextIndices = indices;
          state.ui.selectedAvisoTextIndex = indices[indices.length - 1];
          state.ui.avisoTextSelectionAnchorIndex = indices[0];
          renderAvisoText();
        }
      } else if (event.key === "Escape") {
        const index = Number(state.ui.selectedAvisoTextIndex);
        state.ui.selectedAvisoTextIndices = Number.isInteger(index) ? [index] : [];
        renderAvisoText();
      }
    });


    $("#saveButton").addEventListener("click", saveAll);
    $("#reloadButton").addEventListener("click", requestReload);
    $("#undoButton").addEventListener("click", undoHistory);
    $("#redoButton").addEventListener("click", redoHistory);
    $("#closeButton").addEventListener("click", closeControlCenter);
    $("#profilesFileInput").addEventListener("change", importProfilesFile);
    $("#avisoFileInput").addEventListener("change", importAvisoFile);
    bindWindowDrag();
    bindRuntimeMenuDrag();
  }

  function handleAction(action, button) {
    if (action === "open-control-center") openControlCenter("settings");
    else if (action === "close-runtime-popover") { state.ui.runtimePopover = ""; renderRuntimeMenu(); }
    else if (action === "toggle-runtime-inset") toggleRuntimeInset();
    else if (action === "save-inset-preset") openInsetPresetDialog("capture");
    else if (action === "rename-inset-preset") openInsetPresetDialog("rename");
    else if (action === "capture-inset-preset") confirmInsetPresetDialog();
    else if (action === "update-inset-preset") updateAvisoPreset();
    else if (action === "reset-inset-preset") resetAvisoPreset();
    else if (action === "duplicate-inset-preset") duplicateAvisoPreset();
    else if (action === "default-inset-preset") setDefaultAvisoPreset();
    else if (action === "delete-inset-preset") deleteAvisoPreset();
    else if (action === "manage-aviso-groups") openControlCenter("groups");
    else if (action === "toggle-profile-menu") setRailProfilePopoverOpen($("#railProfilePopover").hidden);
    else if (action === "apply-color") applyColorDraft();
    else if (action === "revert-color") { drafts.color = null; renderColorEditor(); }
    else if (action === "reset-color" || action === "reset-colors") resetSelectedColor();
    else if (action === "apply-icons") applyIcons();
    else if (action === "revert-icons") renderIcons();

    else if (action === "apply-tag") applyTag();
    else if (action === "revert-tag") { drafts.tag = null; renderTagEditor(); }
    else if (action === "insert-tag-token") insertTagToken();
    else if (action === "new-rule") createRule();
    else if (action === "duplicate-rule") duplicateRule();
    else if (action === "delete-rule") deleteRule();
    else if (action === "add-condition") { captureRuleDraft(); drafts.rule.data.criteria.push({ source: "vacdm", token: "", condition: "" }); renderRuleEditor(); }
    else if (action === "delete-condition") deleteRuleCondition(Number(button.dataset.index));
    else if (action === "apply-rule") applyRule();
    else if (action === "revert-rule") { drafts.rule = null; renderRuleEditor(); }
    else if (action === "new-mode") createMode();
    else if (action === "duplicate-mode") duplicateMode();
    else if (action === "delete-mode") deleteMode();
    else if (action === "apply-mode") applyMode();
    else if (action === "revert-mode") { drafts.mode = null; renderModeEditor(); }
    else if (action === "activate-mode") activateMode();
    else if (action === "add-mode-squawk") addModeBlockedSquawk();
    else if (action === "remove-mode-squawk") removeModeBlockedSquawk(Number(button.dataset.index));
    else if (action === "mode-statuses-all") setModeStatusVisibility(true);
    else if (action === "mode-statuses-none") setModeStatusVisibility(false);
    else if (action === "new-profile") createProfile();
    else if (action === "duplicate-profile") duplicateProfile();
    else if (action === "delete-profile") deleteProfile();
    else if (action === "apply-profile") applyProfile();
    else if (action === "revert-profile") { drafts.profile = null; renderProfileEditor(); }
    else if (action === "activate-profile") {
      const record = managedProfileRecord();
      if (record) {
        switchActiveProfile(record.id);
        postActiveProfileChange();
      }
    }
    else if (action === "toggle-aviso-load-menu") setAvisoLoadMenuOpen($("#avisoLoadMenu").hidden);
    else if (action === "load-profiles-computer") {
      if (HOST_MODE) {
        postBridge("resource.computer.load", { resource: "profiles" });
        setStatus("Choose a profiles file…", "info");
      } else $("#profilesFileInput").click();
    }
    else if (action === "load-aviso-computer") {
      setAvisoLoadMenuOpen(false);
      if (HOST_MODE) {
        postBridge("resource.computer.load", { resource: "aviso" });
        setStatus("Choose an AVISO GeoJSON file…", "info");
      } else $("#avisoFileInput").click();
    }
    else if (action === "load-profiles-github") openResourceGithubDialog("profiles");
    else if (action === "load-aviso-github") { setAvisoLoadMenuOpen(false); openResourceGithubDialog("aviso"); }
    else if (action === "apply-aviso-geometry") applyAvisoGeometry();
    else if (action === "apply-aviso-text") applyAvisoTextScope();
    else if (action === "assign-aviso-geometry-group") assignAvisoSelectionToGroup("geometry");
    else if (action === "new-aviso-group") createAvisoGroup();
    else if (action === "duplicate-aviso-group") duplicateAvisoGroup();
    else if (action === "delete-aviso-group") deleteAvisoGroup();
    else if (action === "apply-aviso-group") applyAvisoGroup();
    else if (action === "revert-aviso-group") revertAvisoGroup();
    else if (action === "show-aviso-group") setSelectedAvisoGroupVisibility(true);
    else if (action === "hide-aviso-group") setSelectedAvisoGroupVisibility(false);
    else if (action === "isolate-aviso-group") isolateSelectedAvisoGroup();
    else if (action === "clear-aviso-group") clearSelectedAvisoGroup();
    else if (action === "open-aviso-group-content") openAvisoGroupContentDialog();
    else if (action === "apply-aviso-group-content") applyAvisoGroupContent();
    else if (action === "select-filtered-group-content") setFilteredAvisoGroupContent(true);
    else if (action === "clear-filtered-group-content") setFilteredAvisoGroupContent(false);
    else if (action === "toggle-aviso-group-visibility") toggleRuntimeGroup(button.dataset.groupId);
    else if (action === "remove-aviso-group-member") removeAvisoGroupMember(button);
    else if (action === "revert-aviso-editor") revertAvisoEditor();
    else if (action === "apply-alerts") applyAlerts();
    else if (action === "revert-alerts") revertAlerts();
    else if (action === "alerts-enable-all") setAllAlertTypes(true);
    else if (action === "alerts-disable-all") setAllAlertTypes(false);
    else if (action === "alert-runways-all-arr") setAllAlertRunwayField("arrival", true);
    else if (action === "alert-runways-all-dep") setAllAlertRunwayField("departure", true);
    else if (action === "alert-runways-open-all") setAllAlertRunwayField("closed", false);
    else if (action === "new-alert-runway") addAlertRunway();
    else if (action === "remove-alert-runway") removeAlertRunway(Number(button.dataset.index));
    else if (action === "apply-settings") applySettings();
    else if (action === "reset-data") resetFromSupplied();
    else if (action.startsWith("browse-")) { postBridge(action.replaceAll("-", ".")); showToast("Native file picker requested"); }
  }

  function copyText(value) {
    if (navigator.clipboard?.writeText) navigator.clipboard.writeText(value).then(() => showToast("Copied", "success")).catch(() => {});
    else showToast(value);
  }

  function insertTagToken() {
    if (!activeTagInput || !document.body.contains(activeTagInput)) activeTagInput = $("#tagLineGrid .tag-line-input:not(:disabled)");
    if (!activeTagInput) return;
    const token = $("#tagTokenSelect").value;
    const start = activeTagInput.selectionStart ?? activeTagInput.value.length;
    const end = activeTagInput.selectionEnd ?? start;
    const prefix = start > 0 && !/\s$/.test(activeTagInput.value.slice(0, start)) ? " " : "";
    activeTagInput.value = activeTagInput.value.slice(0, start) + prefix + token + " " + activeTagInput.value.slice(end);
    activeTagInput.focus();
  }

  function createRule() {
    rules().push({ source: "vacdm", token: "tsat", condition: "valid", criteria: [{ source: "vacdm", token: "tsat", condition: "valid" }], tag_type: "departure", status: "any", statuses: RULE_STATUSES.slice(), detail: "normal", text_color: hexToColor("#ffffff") });
    state.ui.selectedRuleIndex = rules().length - 1;
    drafts.rule = null;
    markDirty("Rule created");
    renderRules();
  }
  function duplicateRule() {
    const item = rules()[state.ui.selectedRuleIndex];
    if (!item) return;
    const copy = clone(item);
    copy.name = `${ruleLabel(item, state.ui.selectedRuleIndex)} copy`;
    rules().splice(state.ui.selectedRuleIndex + 1, 0, copy);
    state.ui.selectedRuleIndex += 1;
    drafts.rule = null;
    markDirty("Rule copied");
    renderRules();
  }
  function deleteRule() {
    if (!rules().length || !confirmDelete("Delete this rule?")) return;
    rules().splice(state.ui.selectedRuleIndex, 1);
    state.ui.selectedRuleIndex = Math.max(0, state.ui.selectedRuleIndex - 1);
    drafts.rule = null;
    markDirty("Rule deleted");
    renderRules();
  }
  function deleteRuleCondition(index) {
    captureRuleDraft();
    if (!drafts.rule) return;
    drafts.rule.data.criteria.splice(index, 1);
    if (!drafts.rule.data.criteria.length) drafts.rule.data.criteria.push({ source: "vacdm", token: "", condition: "" });
    renderRuleEditor();
  }

  function createMode() {
    modes().push({
      name: "New mode",
      require_assigned_squawk: false,
      accept_pilot_squawk: true,
      blocked_auto_correlate_squawks: ["2000", "2200", "1200", "7000"],
      statuses: Object.fromEntries(MODE_STATUSES.map(status => [status, true])),
      require_clearance: false,
      require_valid_tsat: false,
      require_active_tobt: false,
      tower_filter: false,
      structured_rules: true
    });
    state.ui.selectedModeIndex = modes().length - 1;
    drafts.mode = null;
    markDirty("Mode created");
    renderModes();
    renderRuntimeMenu();
  }
  function duplicateMode() {
    const item = modes()[state.ui.selectedModeIndex];
    if (!item) return;
    const copy = clone(item); copy.name = `${item.name} copy`;
    modes().splice(state.ui.selectedModeIndex + 1, 0, copy);
    state.ui.selectedModeIndex += 1; drafts.mode = null; markDirty("Mode copied"); renderModes(); renderRuntimeMenu();
  }
  function deleteMode() {
    const items = modes();
    if (items.length <= 1 || !confirmDelete("Delete this display mode?")) return;
    const deleted = items[state.ui.selectedModeIndex];
    items.splice(state.ui.selectedModeIndex, 1);
    state.ui.selectedModeIndex = Math.max(0, state.ui.selectedModeIndex - 1);
    if (activeProfile().filters.display_modes.active === deleted.name) activeProfile().filters.display_modes.active = items[0].name;
    drafts.mode = null; markDirty("Mode deleted"); renderModes(); renderRuntimeMenu();
  }
  function activateMode() {
    const mode = modes()[state.ui.selectedModeIndex];
    if (!mode) return;
    activeProfile().filters.display_modes.active = mode.name;
    markDirty(`${mode.name} set active`);
    renderModes();
    renderRuntimeMenu();
    postBridge("runtime.mode.change", { profile: activeProfile().name, mode: mode.name });
  }

  function createProfile() {
    const base = clone(activeProfile());
    base.name = "New profile";
    const record = { id: uid("profile"), persistedName: "", data: base, original: clone(base) };
    state.profiles.push(record);
    state.ui.managedProfileId = record.id;
    drafts.profile = null;
    markDirty("Profile created");
    renderGlobalProfileSelect(); renderProfilesManager(); renderRuntimeMenu();
  }
  function duplicateProfile() {
    const source = managedProfileRecord(); if (!source) return;
    const data = clone(source.data); data.name = `${data.name} copy`;
    const record = { id: uid("profile"), persistedName: "", data, original: clone(data) };
    const index = state.profiles.indexOf(source) + 1;
    state.profiles.splice(index, 0, record);
    state.ui.managedProfileId = record.id; drafts.profile = null; markDirty("Profile copied"); renderGlobalProfileSelect(); renderProfilesManager(); renderRuntimeMenu();
  }
  function deleteProfile() {
    if (state.profiles.length <= 1 || !confirmDelete("Delete this profile?")) return;
    const record = managedProfileRecord(); if (!record) return;
    const index = state.profiles.indexOf(record); state.profiles.splice(index, 1);
    if (record.id === state.activeProfileId) state.activeProfileId = state.profiles[Math.max(0, index - 1)].id;
    state.ui.managedProfileId = state.activeProfileId; drafts.profile = null; markDirty("Profile deleted"); renderGlobalProfileSelect(); renderAllProfileSections(); renderRuntimeMenu();
  }


  function setAvisoLoadMenuOpen(open) {
    const menu = $("#avisoLoadMenu");
    const button = $("#avisoLoadButton");
    if (!menu || !button) return;
    menu.hidden = !open;
    button.setAttribute("aria-expanded", String(open));
    $(".aviso-load-control")?.classList.toggle("open", open);
  }

  function normalizeGithubRawUrl(value) {
    const raw = String(value || "").trim();
    if (!raw) throw new Error("Enter a GitHub file URL");
    const url = new URL(raw);
    if (url.hostname === "raw.githubusercontent.com") return url.href;
    if (url.hostname !== "github.com" && url.hostname !== "www.github.com") throw new Error("Use a github.com or raw.githubusercontent.com URL");
    const parts = url.pathname.split("/").filter(Boolean);
    if (parts.length < 5) throw new Error("The GitHub URL must point to a file");
    const [owner, repo, mode, branch, ...fileParts] = parts;
    if (mode === "blob" || mode === "raw") {
      if (!fileParts.length) throw new Error("The GitHub URL must point to a JSON or GeoJSON file");
      return `https://raw.githubusercontent.com/${owner}/${repo}/${branch}/${fileParts.join("/")}`;
    }
    throw new Error("Open the file on GitHub and copy its file URL");
  }

  function resourceSourceKey(type) {
    return type === "profiles" ? "profileFile" : "avisoFile";
  }

  function setResourceSource(type, source) {
    state.settings[resourceSourceKey(type)] = source;
    if (state.ui.page === "settings") renderSettings();
  }

  function applyProfilesPayload(parsed, source = "Profiles") {
    if (!Array.isArray(parsed)) throw new Error("Expected a vSMR profiles JSON array");
    const { records, metadata, extras } = getProfileRecords(parsed);
    if (!records.length) throw new Error("No profiles were found in this file");
    state.profiles = records;
    state.metadata = metadata;
    state.profileExtras = clone(extras);
    const preferred = records.find(record => record.data.name === metadata.last_active_profile) || records[0];
    state.activeProfileId = preferred.id;
    state.ui.managedProfileId = preferred.id;
    state.ui.selectedRuleIndex = 0;
    state.ui.selectedModeIndex = Math.max(0, (preferred.data.filters?.display_modes?.items || []).findIndex(mode => mode.name === preferred.data.filters?.display_modes?.active));
    state.ui.selectedTagId = "departure:taxi";
    const airport = inferAirport(preferred.data.name);
    const colors = collectProfileColors(preferred.data);
    state.ui.selectedColorPath = colors[0]?.id || "";
    state.settings.resolutionPreset = preferred.data.targets?.small_icon_boost_resolution_preset || state.settings.resolutionPreset || "1080p";
    Object.keys(drafts).forEach(key => drafts[key] = null);
    setResourceSource("profiles", source);
    renderGlobalProfileSelect();
    renderAllProfileSections();
    renderSettings();
    renderRuntimeMenu();
    markDirty(`${source} loaded`);
  }


  function applyAvisoPayload(parsed, source = "AVISO GeoJSON") {
    if (parsed?.type !== "FeatureCollection" || !Array.isArray(parsed.features)) throw new Error("Expected a GeoJSON FeatureCollection");
    state.aviso = normalizeAvisoData(parsed);
    resetAvisoSelections();
    setResourceSource("aviso", source);
    renderAviso();
    renderRuntimeMenu();
    markDirty(`${source} loaded`);
  }

  function openResourceGithubDialog(type) {
    githubResourceType = ["profiles", "aviso"].includes(type) ? type : "aviso";
    const labels = { profiles: "Profiles", aviso: "AVISO GeoJSON" };
    const examples = {
      profiles: "https://github.com/owner/repo/blob/branch/vSMR_Profiles.json",
      aviso: "https://github.com/owner/repo/blob/branch/AVISO.geojson"
    };
    const dialog = $("#resourceGithubDialog");
    $("#resourceGithubTitle").textContent = `Load ${labels[githubResourceType]} from GitHub`;
    $("#resourceGithubUrl").value = "";
    $("#resourceGithubUrl").placeholder = examples[githubResourceType];
    $("#resourceGithubHint").textContent = "Paste a GitHub file URL or a raw.githubusercontent.com URL.";
    if (typeof dialog.showModal === "function") dialog.showModal();
    else dialog.setAttribute("open", "");
    setTimeout(() => $("#resourceGithubUrl")?.focus(), 0);
  }

  function setGithubRequestPending(isPending) {
    const button = $("#resourceGithubLoadConfirm");
    const input = $("#resourceGithubUrl");
    if (button) {
      button.disabled = isPending;
      button.textContent = isPending ? "Loading..." : "Load";
    }
    if (input) input.disabled = isPending;
  }

  function loadResourceFromGithub() {
    const input = $("#resourceGithubUrl");
    try {
      const sourceUrl = String(input.value || "").trim();
      const url = normalizeGithubRawUrl(sourceUrl);
      if (pending.github) return;
      const resource = githubResourceType;
      const id = postBridge("resource.github.load", { resource, url });
      pending.github = { id, resource, source: sourceUrl };
      setGithubRequestPending(true);
      setStatus(`Loading ${resource === "aviso" ? "AVISO GeoJSON" : "profiles"} from GitHub...`, "info");
      if (!HOST_MODE) {
        setTimeout(() => receiveHostMessage({
          version: PROTOCOL_VERSION,
          id,
          type: "resource.error",
          payload: { requestId: id, resource, message: "GitHub loading is available in the native Control Center." }
        }), 0);
      }
    } catch (error) {
      showToast(error.message || "Could not load the GitHub file", "error");
    }
  }

  async function importProfilesFile(event) {
    const file = event.target.files?.[0];
    if (!file) return;
    try {
      applyProfilesPayload(JSON.parse(await file.text()), file.name);
      showToast("Profiles loaded", "success");
    } catch (error) { showToast(error.message, "error"); }
    event.target.value = "";
  }

  async function importAvisoFile(event) {
    const file = event.target.files?.[0];
    if (!file) return;
    try {
      applyAvisoPayload(JSON.parse(await file.text()), file.name);
      showToast("GeoJSON loaded", "success");
    } catch (error) { showToast(error.message, "error"); }
    event.target.value = "";
  }


  function bindWindowDrag() {
    const handle = $("#dragHandle"); const windowElement = $("#controlWindow");
    let drag = null;
    handle.addEventListener("pointerdown", event => {
      if (event.target.closest("button") || event.button !== 0) return;
      if (HOST_MODE) {
        event.preventDefault();
        postBridge("window.drag.start", {
          screenX: Number(event.screenX) || 0,
          screenY: Number(event.screenY) || 0,
          pointerId: event.pointerId
        });
        return;
      }
      const rect = windowElement.getBoundingClientRect();
      windowElement.style.transform = "none"; windowElement.style.left = `${rect.left}px`; windowElement.style.top = `${rect.top}px`;
      drag = { x: event.clientX - rect.left, y: event.clientY - rect.top };
      handle.setPointerCapture(event.pointerId);
    });
    handle.addEventListener("pointermove", event => {
      if (!drag) return;
      const maxLeft = Math.max(0, innerWidth - windowElement.offsetWidth); const maxTop = Math.max(0, innerHeight - windowElement.offsetHeight);
      windowElement.style.left = `${clamp(event.clientX - drag.x, 0, maxLeft)}px`; windowElement.style.top = `${clamp(event.clientY - drag.y, 0, maxTop)}px`;
    });
    handle.addEventListener("pointerup", () => { drag = null; });
  }

  function bindRuntimeMenuDrag() {
    const handle = $("#runtimeDragHandle");
    const menu = $("#runtimeMenu");
    let drag = null;
    handle.addEventListener("pointerdown", event => {
      const rect = menu.getBoundingClientRect();
      menu.style.transform = "none";
      menu.style.left = `${rect.left}px`;
      menu.style.top = `${rect.top}px`;
      drag = { x: event.clientX - rect.left, y: event.clientY - rect.top };
      handle.setPointerCapture(event.pointerId);
    });
    handle.addEventListener("pointermove", event => {
      if (!drag) return;
      const maxLeft = Math.max(0, innerWidth - menu.offsetWidth);
      const maxTop = Math.max(0, innerHeight - menu.offsetHeight);
      menu.style.left = `${clamp(event.clientX - drag.x, 0, maxLeft)}px`;
      menu.style.top = `${clamp(event.clientY - drag.y, 0, maxTop)}px`;
      positionRuntimePopover();
    });
    handle.addEventListener("pointerup", () => { drag = null; positionRuntimePopover(); });
    handle.addEventListener("pointercancel", () => { drag = null; });
    window.addEventListener("resize", positionRuntimePopover);
  }

  function decodeHostMessage(input) {
    let message = input;
    if (typeof message === "string") {
      try { message = JSON.parse(message); }
      catch (error) {
        console.warn("Ignored malformed native message", error);
        return null;
      }
    }
    if (!message || typeof message !== "object" || Array.isArray(message)) return null;
    const type = String(message.type || message.action || "");
    if (!type) return null;
    let payload = message.payload;
    if (payload == null) {
      if ("data" in message) payload = { data: message.data };
      else payload = {};
    }
    if (typeof payload !== "object" || Array.isArray(payload)) payload = { data: payload };
    if (message.message != null && payload.message == null) payload = { ...payload, message: String(message.message) };
    return {
      version: Number(message.version) || 0,
      id: String(message.id || ""),
      requestId: String(message.requestId || message.replyTo || payload.requestId || payload.replyTo || ""),
      type,
      payload,
      legacy: !message.version
    };
  }

  function messageMatchesRequest(message, requestId) {
    if (!requestId) return false;
    if (message.requestId) return message.requestId === requestId;
    if (message.id) return message.id === requestId;
    return message.legacy;
  }

  function authoritativePayload(payload) {
    if (payload?.state && typeof payload.state === "object" && !Array.isArray(payload.state)) {
      return { ...payload.state, ...Object.fromEntries(Object.entries(payload).filter(([key]) => key !== "state")) };
    }
    return payload || {};
  }

  function applyAuthoritativeState(payload, reason = "update") {
    const incoming = authoritativePayload(payload);
    const preservedUi = state.ui;
    const previousProfileName = activeProfile().name || "";
    const previousHostAirport = state.hostAirport;
    const incomingAirport = normalizeAirportCode(
      typeof incoming.airport === "string" ? incoming.airport : state.hostAirport
    );
    const preservesStagedEditors = state.dirty &&
      !["initial", "reload", "save", "undo", "redo", "state.undo", "state.redo"].includes(reason);
    let avisoChanged = false;

    if (Array.isArray(incoming.profiles)) {
      const normalized = getProfileRecords(incoming.profiles);
      if (normalized.records.length) {
        const requestedProfile = String(incoming.activeProfile || incoming.active_profile || incoming.profile || "");
        if (preservesStagedEditors && incomingAirport) {
          const incomingProfile = normalized.records.find(record =>
            record.id === requestedProfile || record.data.name === requestedProfile
          ) || normalized.records.find(record => record.data.name === previousProfileName);
          const localProfile = incomingProfile
            ? state.profiles.find(record =>
                record.id === incomingProfile.id || record.data.name === incomingProfile.data.name
              )
            : activeProfileRecord();
          const incomingStore = incomingProfile?.data?.aviso_presets?.airports?.[incomingAirport];
          if (localProfile && incomingStore && typeof incomingStore === "object" && !Array.isArray(incomingStore)) {
            assignRecordAirportPresetStore(localProfile, incomingAirport, incomingStore);
            rebasePresetStoreSnapshots(
              localProfile.id,
              [localProfile.data.name, incomingProfile.data.name],
              incomingAirport,
              incomingStore
            );
          }
        }
        if (!preservesStagedEditors) {
          state.profiles = normalized.records;
          state.metadata = normalized.metadata;
          state.profileExtras = clone(normalized.extras);
        }
        const preferred = state.profiles.find(record => record.id === requestedProfile || record.data.name === requestedProfile)
          || state.profiles.find(record => record.data.name === previousProfileName)
          || state.profiles.find(record => record.data.name === state.metadata.last_active_profile)
          || state.profiles[0];
        state.activeProfileId = preferred?.id || "";
      }
    }
    if (!preservesStagedEditors && incoming.aviso?.type === "FeatureCollection") {
      state.aviso = normalizeAvisoData(incoming.aviso);
      avisoChanged = true;
    }
    if (!preservesStagedEditors && incoming.settings && typeof incoming.settings === "object") {
      state.settings = { ...state.settings, ...clone(incoming.settings) };
    }
    if (typeof incoming.airport === "string") {
      state.hostAirport = incoming.airport.trim().toUpperCase();
      if (!preservesStagedEditors)
        state.airport = state.hostAirport;
    }
    if (incoming.runtime && typeof incoming.runtime === "object") {
      state.runtime = { ...state.runtime, ...clone(incoming.runtime) };
      state.runtime.insets = { aviso: false, srw1: false, srw2: false, ...(state.runtime.insets || {}) };
      if (Object.hasOwn(incoming.runtime, "activeAvisoPreset")) {
        state.runtime.activeAvisoPresetScope = activePresetScope();
        const activePreset = profileAvisoPresetStore().items.find(item =>
          item.name === state.runtime.activeAvisoPreset
        );
        state.runtime.avisoInsetSnapshot = activePreset ? clone(activePreset) : null;
      }
    }

    state.ui = preservedUi;
    if (HOST_MODE) state.ui.controlCenterOpen = true;
    if (!state.profiles.some(record => record.id === state.activeProfileId)) state.activeProfileId = state.profiles[0]?.id || "";
    if (!state.profiles.some(record => record.id === state.ui.managedProfileId)) state.ui.managedProfileId = state.activeProfileId;
    Object.keys(drafts).forEach(key => drafts[key] = null);
    if (avisoChanged) resetAvisoSelections();
    renderAll();
    if (preservesStagedEditors && state.airport && state.hostAirport &&
      state.airport !== state.hostAirport && previousHostAirport !== state.hostAirport) {
      const message = "The active airport changed. Reload before saving or using Undo/Redo.";
      setStatus(message, "error");
      showToast(message, "error");
    }

    if (reason === "initial" || reason === "reload") {
      resetHistory(true);
    } else if (reason === "save") {
      history.present = captureHistorySnapshot();
      markSaved(incoming.message || "Configuration saved");
    } else {
      const wasDirty = state.dirty;
      history.present = captureHistorySnapshot();
      if (!wasDirty) savedSnapshot = history.present;
      updateDirtyState();
    }
  }

  function setInsetWindows(windows = {}) {
    state.runtime.insets ||= { aviso: false, srw1: false, srw2: false };
    ["aviso", "srw1", "srw2"].forEach(key => { if (key in windows) state.runtime.insets[key] = Boolean(windows[key]); });
    state.runtime.avisoInsetVisible = state.runtime.insets.aviso;
    renderRuntimeMenu();
  }

  function setAlertsState(alerts = {}) {
    state.runtime.alerts = { ...state.runtime.alerts, ...clone(alerts) };
    drafts.alerts = null;
    if (state.ui.page === "alerts") renderAlerts();
  }

  function finishGithubRequest(message, success) {
    if (pending.github && !messageMatchesRequest(message, pending.github.id)) return false;
    const request = pending.github || {
      id: message.id,
      resource: message.payload.resource,
      source: message.payload.source || "computer",
      kind: "computer"
    };
    if (pending.github) {
      pending.github = null;
      setGithubRequestPending(false);
    }
    if (!success) return true;
    const resource = String(message.payload.resource || request.resource);
    const data = message.payload.data;
    const source = message.payload.source || request.source;
    try {
      if (resource === "profiles") applyProfilesPayload(data, source);
      else if (resource === "aviso") applyAvisoPayload(data, source);
      else throw new Error("Unknown resource type");
      const dialog = $("#resourceGithubDialog");
      if (dialog.open) {
        if (typeof dialog.close === "function") dialog.close(); else dialog.removeAttribute("open");
      }
      const sourceLabel = source === "bundled defaults"
        ? "bundled defaults"
        : request.kind === "computer" || source === "computer"
          ? "computer"
          : "GitHub";
      showToast(`${resource === "aviso" ? "GeoJSON" : "Profiles"} loaded from ${sourceLabel}`, "success");
    } catch (error) {
      setStatus(error.message || "The loaded resource is invalid", "error");
      showToast(error.message || "The loaded resource is invalid", "error");
    }
    return true;
  }

  function receiveHostMessage(input) {
    const message = decodeHostMessage(input);
    if (!message) return;
    const payload = message.payload;

    if (message.type === "state.initial" || message.type === "initial.state") {
      applyAuthoritativeState(payload, "initial");
      setStatus(payload.message || "Configuration loaded");
      return;
    }
    if (message.type === "state.authoritative") {
      const isReload = pending.reload && messageMatchesRequest(message, pending.reload);
      if (isReload) pending.reload = "";
      const reason = isReload ? "reload" : String(payload.reason || "update");
      splitAvisoContext = { id: message.id, reason };
      applyAuthoritativeState(payload, reason);
      if (isReload) showToast("Configuration reloaded", "success");
      updateCommandState();
      return;
    }
    if (message.type === "state.aviso" || message.type === "aviso") {
      const aviso = payload.aviso || payload.data || (payload.type === "FeatureCollection" ? payload : null);
      if (aviso?.type === "FeatureCollection") {
        const contextMatches = splitAvisoContext && (!splitAvisoContext.id || !message.id || splitAvisoContext.id === message.id);
        const reason = contextMatches ? splitAvisoContext.reason : state.dirty ? "update" : "initial";
        splitAvisoContext = null;
        applyAuthoritativeState({ aviso }, reason);
      }
      return;
    }
    if (message.type === "state.saved" || message.type === "saved") {
      if (pending.save && !messageMatchesRequest(message, pending.save)) return;
      pending.save = "";
      const savedState = authoritativePayload(payload);
      if (savedState.profiles || savedState.aviso || savedState.settings || savedState.runtime) {
        applyAuthoritativeState(savedState, "save");
      } else {
        markSaved(payload.message || "Configuration saved");
      }
      updateCommandState();
      showToast(payload.message || "Configuration saved", "success");
      return;
    }
    if (message.type === "resource.loaded") {
      finishGithubRequest(message, true);
      return;
    }
    if (message.type === "resource.error") {
      if (!finishGithubRequest(message, false)) return;
      const text = payload.message || "Could not load the resource";
      setStatus(text, "error");
      showToast(text, "error");
      return;
    }
    if (message.type === "state.error" || message.type === "error") {
      let matched = false;
      if (pending.save && messageMatchesRequest(message, pending.save)) { pending.save = ""; matched = true; }
      if (pending.reload && messageMatchesRequest(message, pending.reload)) { pending.reload = ""; matched = true; }
      if (pending.github && messageMatchesRequest(message, pending.github.id)) {
        pending.github = null;
        setGithubRequestPending(false);
        matched = true;
      }
      if (!matched && !message.legacy && (pending.save || pending.reload || pending.github)) return;
      updateCommandState();
      const text = payload.message || payload.error || "Native operation failed";
      setStatus(text, "error");
      showToast(text, "error");
      return;
    }

    /* Compatibility aliases for existing previews and older native builds. */
    if (message.type === "profiles") {
      const profiles = payload.profiles || payload.data;
      if (Array.isArray(profiles)) applyAuthoritativeState({ profiles, activeProfile: payload.activeProfile }, state.dirty ? "update" : "initial");
    } else if (message.type === "insets") {
      setInsetWindows(payload.data || payload);
    } else if (message.type === "inset-state") {
      const snapshot = payload.data || payload;
      if (snapshot && typeof snapshot === "object") state.runtime.avisoInsetSnapshot = clone(snapshot);
    } else if (message.type === "alerts") {
      setAlertsState(payload.data || payload);
    } else if (message.type === "aviso.group.visibility") {
      const group = avisoGroups().find(item => item.id === payload.id || item.group_id === payload.id);
      if (group) {
        group.visible = payload.visible !== false;
        renderRuntimeMenu();
        if (state.ui.page === "groups") renderAvisoGroups();
      }
    }
  }

  window.vsmrControlCenter = {
    receive: receiveHostMessage,
    setProfiles(profiles) { receiveHostMessage({ type: "profiles", data: profiles }); },
    setAviso(aviso) { receiveHostMessage({ type: "aviso", data: aviso }); },
    open(page = "settings", avisoView = "") { openControlCenter(page, avisoView); },
    close() { closeControlCenter(); },
    setGroupVisibility(groupId, visible) {
      receiveHostMessage({ type: "aviso.group.visibility", payload: { id: groupId, visible } });
    },
    setInsetVisible(visible) { setInsetWindows({ aviso: visible }); },
    setInsetWindows,
    setInsetState(snapshot) {
      if (snapshot && typeof snapshot === "object") state.runtime.avisoInsetSnapshot = clone(snapshot);
    },
    setAlertsState,
    getState: serializeStatePayload
  };

  if (window.chrome?.webview?.addEventListener) {
    window.chrome.webview.addEventListener("message", event => receiveHostMessage(event.data));
  }

  applyQueryState();
  bindEvents();
  renderAll();
  resetHistory(true);
  setStatus(HOST_MODE ? "Waiting for configuration…" : "Bundled LFPG preview loaded");
  postBridge("ui.ready", {
    hostMode: HOST_MODE,
    protocolVersion: PROTOCOL_VERSION,
    capabilities: ["state", "save", "reload", "undo", "redo", "github-import", "computer-import", "bundled-reset", "window-actions", "split-aviso"]
  });
})();
