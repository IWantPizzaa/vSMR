(() => {
  "use strict";

  const PROTOCOL_VERSION = 1;
  const MAX_BRIDGE_MESSAGE_BYTES = 28 * 1024 * 1024;
  const REQUEST_TIMEOUT_MS = 45000;
  const HISTORY_LIMIT = 12;
  const AUTOSAVE_DEBOUNCE_MS = 300;
  const AUTOSAVE_RETRY_MS = 250;
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
    datalink: "CPDLC / PDC",
    settings: "Settings"
  };
  const PROFILE_TITLES = { colors: "Colors", icons: "Icons", tags: "Tags", rules: "Rules" };
  const MAP_ZOOM_LABELS = [
    "All ranges", "34 km or closer", "28 km or closer", "22 km or closer", "18 km or closer",
    "14 km or closer", "12 km or closer", "9.5 km or closer", "8 km or closer", "6 km or closer",
    "5 km or closer", "4 km or closer", "3 km or closer", "2.5 km or closer", "2 km or closer"
  ];
  const MODE_STATUSES = ["no_status", "push", "startup", "taxi", "lineup", "departure", "on_runway", "airborne", "arrivals", "no_fpl", "uncorrelated"];
  const RULE_STATUSES = ["default", "nofpl", "push", "stup", "taxi", "lnup", "depa", "airdep", "airdep_onrunway", "airarr", "airarr_onrunway"];
  const RULE_STATUS_LABELS = {
    default: "Default", nofpl: "No FPL", push: "Push", stup: "Startup", taxi: "Taxi", lnup: "Line Up",
    depa: "Departure", airdep: "Airborne departure", airdep_onrunway: "Departure on runway",
    airarr: "Airborne arrival", airarr_onrunway: "Arrival on runway"
  };
  const TAG_SCOPES = ["departure", "arrival", "uncorrelated", "airborne"];
  const TAG_STATUS_LABELS = {
    default: "Default", taxi: "Taxi", lnup: "Line Up", push: "Push", stup: "Startup", nofpl: "No FPL", depa: "Departure",
    airdep: "Airborne departure", airdep_onrunway: "Departure on runway", airarr: "Airborne arrival",
    airarr_onrunway: "Arrival on runway"
  };
  const TAG_STATUS_ORDER = ["nofpl", "push", "stup", "taxi", "lnup", "depa", "airdep", "airdep_onrunway", "airarr", "airarr_onrunway"];
  const COLOR_FAMILY_ORDER = ["Tags", "Targets", "RIMCAS", "SRW 1"];
  const COLOR_SECTION_ORDER = ["Departure", "Arrival", "Uncorrelated", "Airborne", "General"];
  const TAG_STATUS_COLOR_KEYS = {
    departure: {
      default: "background_no_status_color", taxi: "background_taxi_color", lnup: "background_lineup_color", push: "background_push_color",
      stup: "background_startup_color", nofpl: "background_no_fpl_color", depa: "background_departure_color",
      airdep: "background_airborne_color", airdep_onrunway: "background_on_runway_color"
    },
    arrival: {
      default: "background_on_ground_color", nofpl: "background_no_fpl_color",
      airarr: "background_airborne_color", airarr_onrunway: "background_on_runway_color"
    },
    uncorrelated: { default: "background_on_ground_color" }
  };
  const TAG_TOKENS = ["callsign", "actype", "sctype", "wake", "deprwy", "gs", "flightlevel", "tendency", "scratchpad", "remark", "asid", "uk_stand", "sqerror", "groundstatus", "systemid"];
  const RULE_SOURCES = ["vacdm", "runway", "custom"];
  const RULE_SOURCE_LABELS = { vacdm: "VACDM", runway: "Runway", custom: "SID / custom" };
  const RULE_SOURCE_TOKENS = {
    vacdm: ["tobt", "tsat", "ttot", "asat", "aobt", "atot", "asrt", "aort", "ctot"],
    runway: ["deprwy", "seprwy", "arvrwy", "srvrwy"],
    custom: ["asid", "ssid"]
  };
  const ALERT_TYPES = ["NO PUSH", "NO TAXI", "NO TKOF", "STAT RPA", "RWY INC", "RWY TYPE", "RWY CLSD", "HIGH SPD", "EMERG"];
  const DEFAULT_ALERT_RUNWAYS = [
    { id: "09L / 27R", arrival: true, departure: true, closed: false },
    { id: "09R / 27L", arrival: true, departure: true, closed: false },
    { id: "08L / 26R", arrival: false, departure: false, closed: false },
    { id: "08R / 26L", arrival: false, departure: false, closed: false }
  ];
  const ALERT_COLOR_DEFAULTS = [
    ["background_color_stage_one", "#a05a1e"],
    ["background_color_stage_two", "#960000"],
    ["caution_alert_text_color", "#000000"],
    ["caution_alert_background_color", "#ffff00"],
    ["warning_alert_text_color", "#ffffff"],
    ["warning_alert_background_color", "#ff0000"]
  ];
  const DEFAULT_AVISO_GROUP_BLUEPRINTS = [
    { id: "runway-details", name: "Runway details", accent: "#d9d9d9", styles: ["surface.runway", "marking.runway", "line.runway_centerline", "label.tora"] },
    { id: "ground-layout-arrows", name: "Ground layout arrows", accent: "#d4bd39", stylePrefix: "line.ground_layout_arrows." },
    { id: "holding-positions", name: "Holding positions", accent: "#e18956", styles: ["marking.holding_cat_i", "marking.holding_cat_iii", "marking.holding_permanent", "marking.stop_point", "lighting.stop_bar"] },
    { id: "stands-and-gates", name: "Stands & gates", accent: "#84b7d5", styles: ["label.gates_stands", "line.stand_entry", "line.stand_entry_dashed"] },
    { id: "vfr-points", name: "VFR points", accent: "#8eb68d", styles: ["label.vfr_points"] }
  ];
  const SCROLL_CUE_LIST_IDS = [
    "colorTree", "tagDefinitionList", "avisoGeometryStyleList", "avisoTextStyleList",
    "ruleList", "modeList", "avisoGroupList", "profileList"
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
    const normalized = String(value || "");
    if (/^(lineup|line_up|lnup)$/i.test(normalized)) return "Line Up";
    return normalized
      .replace(/_color$/i, "")
      .replaceAll("_", " ")
      .replace(/\b\w/g, letter => letter.toUpperCase())
      .replace(/\bLineup\b/g, "Line Up");
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

  function isAirportCode(value) {
    return /^[A-Z0-9]{4}$/.test(normalizeAirportCode(value));
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
    if (normalized.default) {
      const defaultItem = normalized.items.find(item => item.name.toLowerCase() === normalized.default.toLowerCase());
      normalized.default = defaultItem?.name || "";
    }
    return normalized;
  }

  function metadataAvisoPresetStoreForAirport(metadata, airport, migrateLegacy = true) {
    if (!metadata.aviso_presets || typeof metadata.aviso_presets !== "object" || Array.isArray(metadata.aviso_presets)) {
      metadata.aviso_presets = {};
    }
    const root = metadata.aviso_presets;
    if (!root.airports || typeof root.airports !== "object" || Array.isArray(root.airports)) root.airports = {};

    const airportCode = normalizeAirportCode(airport);
    if (!airportCode) return normalizeAvisoPresetStore({});

    const persistedAirportKey = Object.keys(root.airports).find(key => normalizeAirportCode(key) === airportCode);
    let store = persistedAirportKey ? root.airports[persistedAirportKey] : undefined;
    if (persistedAirportKey && persistedAirportKey !== airportCode) delete root.airports[persistedAirportKey];
    const hasLegacyStore = Array.isArray(root.items) || typeof root.default === "string";
    const legacyOwner = normalizeAirportCode(root.airport);
    if (migrateLegacy && hasLegacyStore && isAirportCode(legacyOwner) && legacyOwner === airportCode) {
      store = mergeAvisoPresetStore(
        store && typeof store === "object" && !Array.isArray(store) ? store : {},
        { default: String(root.default || ""), items: clone(Array.isArray(root.items) ? root.items : []) },
        "Legacy"
      );
      delete root.default;
      delete root.items;
      delete root.airport;
    }
    if (!store || typeof store !== "object" || Array.isArray(store)) store = {};
    root.airports[airportCode] = normalizeAvisoPresetStore(store);
    return root.airports[airportCode];
  }

  function sameAvisoPresetContent(left, right) {
    return JSON.stringify(left) === JSON.stringify(right);
  }

  function validAvisoPresetStoreShape(store) {
    if (!store || typeof store !== "object" || Array.isArray(store)) return false;
    if (Object.hasOwn(store, "items") && !Array.isArray(store.items)) return false;
    if (Object.hasOwn(store, "default") && typeof store.default !== "string") return false;
    return !Array.isArray(store.items) || store.items.every(item => item && typeof item === "object" && !Array.isArray(item));
  }

  function mergeAvisoPresetStore(destination, source, sourceLabel) {
    const target = normalizeAvisoPresetStore(destination);
    const incoming = normalizeAvisoPresetStore(clone(source));
    const resolvedNames = new Map();

    incoming.items.forEach(item => {
      const originalName = item.name;
      const existing = target.items.find(candidate => candidate.name.toLowerCase() === originalName.toLowerCase());
      if (!existing) {
        target.items.push(item);
        resolvedNames.set(originalName.toLowerCase(), item.name);
        return;
      }
      if (sameAvisoPresetContent(existing, item)) {
        resolvedNames.set(originalName.toLowerCase(), existing.name);
        return;
      }

      const suffix = String(sourceLabel || "Profile").trim() || "Profile";
      const baseName = `${originalName} (${suffix})`;
      let uniqueName = baseName;
      let index = 2;
      while (target.items.some(candidate => candidate.name.toLowerCase() === uniqueName.toLowerCase())) {
        uniqueName = `${baseName} ${index++}`;
      }
      item.name = uniqueName;
      target.items.push(item);
      resolvedNames.set(originalName.toLowerCase(), uniqueName);
    });

    if (!target.default && incoming.default) {
      const resolvedDefault = resolvedNames.get(incoming.default.toLowerCase());
      if (resolvedDefault) target.default = resolvedDefault;
    }
    return normalizeAvisoPresetStore(target);
  }

  function migrateProfileAvisoPresetStores(metadata, records, preferredProfile) {
    if (!metadata.aviso_presets || typeof metadata.aviso_presets !== "object" || Array.isArray(metadata.aviso_presets)) {
      metadata.aviso_presets = {};
    }
    const globalRoot = metadata.aviso_presets;
    if (!globalRoot.airports || typeof globalRoot.airports !== "object" || Array.isArray(globalRoot.airports)) {
      globalRoot.airports = {};
    }

    const preferredName = String(preferredProfile || "").toLowerCase();
    const orderedRecords = [...records].sort((left, right) => {
      const leftPreferred = left.id.toLowerCase() === preferredName || String(left.data?.name || "").toLowerCase() === preferredName;
      const rightPreferred = right.id.toLowerCase() === preferredName || String(right.data?.name || "").toLowerCase() === preferredName;
      return Number(rightPreferred) - Number(leftPreferred);
    });

    orderedRecords.forEach(record => {
      const profile = record.data;
      const profileRoot = profile?.aviso_presets;
      if (!profileRoot || typeof profileRoot !== "object" || Array.isArray(profileRoot)) return;

      const sourceLabel = String(profile.name || "Profile").trim() || "Profile";
      const sourceAirports = profileRoot.airports;
      if (sourceAirports && typeof sourceAirports === "object" && !Array.isArray(sourceAirports)) {
        let migratedAllAirportStores = true;
        Object.entries(sourceAirports).forEach(([airport, sourceStore]) => {
          const airportCode = normalizeAirportCode(airport);
          if (!airportCode || !validAvisoPresetStoreShape(sourceStore)) {
            migratedAllAirportStores = false;
            return;
          }
          const destination = metadataAvisoPresetStoreForAirport(metadata, airportCode, false);
          globalRoot.airports[airportCode] = mergeAvisoPresetStore(destination, sourceStore, sourceLabel);
        });
        if (migratedAllAirportStores) delete profileRoot.airports;
      }

      const airportCode = normalizeAirportCode(profileRoot.airport || profile.airport);
      const hasLegacyItems = Object.hasOwn(profileRoot, "items");
      const hasLegacyDefault = Object.hasOwn(profileRoot, "default");
      const validLegacyStore =
        (!hasLegacyItems || Array.isArray(profileRoot.items)) &&
        (!hasLegacyDefault || typeof profileRoot.default === "string");
      if (isAirportCode(airportCode) && validLegacyStore && (hasLegacyItems || hasLegacyDefault)) {
        const destination = metadataAvisoPresetStoreForAirport(metadata, airportCode, false);
        globalRoot.airports[airportCode] = mergeAvisoPresetStore(
          destination,
          { items: Array.isArray(profileRoot.items) ? profileRoot.items : [], default: profileRoot.default || "" },
          sourceLabel
        );
        delete profileRoot.items;
        delete profileRoot.default;
        delete profileRoot.airport;
      }

      if (Object.keys(profileRoot).length === 0) delete profile.aviso_presets;
      record.original = clone(profile);
    });

    return globalRoot;
  }

  function unscopedLegacyPresetSources() {
    const sources = [];
    const metadataRoot = state.metadata?.aviso_presets;
    if (metadataRoot && typeof metadataRoot === "object" && !Array.isArray(metadataRoot)) {
      const ownsAirport = isAirportCode(metadataRoot.airport);
      const hasStore = Object.hasOwn(metadataRoot, "items") || Object.hasOwn(metadataRoot, "default");
      if (!ownsAirport && hasStore && validAvisoPresetStoreShape(metadataRoot)) {
        sources.push({ root: metadataRoot, label: "Legacy", profile: null });
      }
    }

    state.profiles.forEach(record => {
      const profile = record?.data;
      const root = profile?.aviso_presets;
      if (!root || typeof root !== "object" || Array.isArray(root)) return;
      const ownsAirport = isAirportCode(root.airport || profile.airport);
      const hasStore = Object.hasOwn(root, "items") || Object.hasOwn(root, "default");
      if (!ownsAirport && hasStore && validAvisoPresetStoreShape(root)) {
        sources.push({
          root,
          label: String(profile.name || "Profile").trim() || "Profile",
          profile
        });
      }
    });
    return sources;
  }

  function legacyPresetAssignmentSummary() {
    const sources = unscopedLegacyPresetSources();
    return {
      sources,
      count: sources.reduce((total, source) =>
        total + (Array.isArray(source.root.items) ? source.root.items.length : 0), 0)
    };
  }

  function assignLegacyInsetPresetsToActiveAirport() {
    const airport = activePresetAirport();
    const summary = legacyPresetAssignmentSummary();
    if (!isAirportCode(airport) || !summary.sources.length) return;
	if (state.dirty || pending.save || pending.reload || pending.resource ||
		runtimeCommandPending.size || splitAvisoContext) {
	  showToast("Wait for automatic saving or revert current edits before assigning legacy presets", "error");
	  return;
	}

    const noun = summary.count === 1 ? "preset" : "presets";
    if (!window.confirm(
      `Assign ${summary.count} legacy inset ${noun} to ${airport}? ` +
      "This one-time migration is saved immediately."
    )) return;

	if (HOST_MODE) {
	  const requestId = postBridge("aviso.inset.preset.legacy.assign", { airport });
	  if (!requestId) return;
	  pending.reload = requestId;
	  armPendingTimeout("reload", requestId);
	  setStatus(`Assigning legacy inset presets to ${airport}...`, "info");
	  updateCommandState();
	  return;
	}

    const root = state.metadata.aviso_presets ||= {};
    if (!root.airports || typeof root.airports !== "object" || Array.isArray(root.airports)) root.airports = {};
    let destination = metadataAvisoPresetStoreForAirport(state.metadata, airport, false);

    summary.sources.forEach(source => {
      destination = mergeAvisoPresetStore(
        destination,
        {
          items: Array.isArray(source.root.items) ? source.root.items : [],
          default: typeof source.root.default === "string" ? source.root.default : ""
        },
        source.label
      );
      delete source.root.items;
      delete source.root.default;
      if (!isAirportCode(source.root.airport)) delete source.root.airport;
      if (source.profile && Object.keys(source.root).length === 0)
        delete source.profile.aviso_presets;
    });
    root.airports[airport] = normalizeAvisoPresetStore(destination);

    setPersistentStatus("", "", [], "legacy-presets");
    markDirty(`Legacy inset presets assigned to ${airport}`, ["metadata"]);
    renderAll();
    showToast(`Inset presets assigned to ${airport}`, "success");
  }

  function normalizeAvisoGroupId(value, fallback = "group") {
    const normalized = String(value || fallback).trim().toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "");
    return normalized || fallback;
  }

  function normalizeAvisoData(sourceAviso, createDefaults = true) {
    const hasExplicitGroups = Array.isArray(sourceAviso?.vsmr_groups);
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

    if (createDefaults && !hasExplicitGroups && !aviso.vsmr_groups.length) {
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
        const data = clone(entry);
        if (data.approach_insets && typeof data.approach_insets === "object")
          delete data.approach_insets.background_color;
        records.push({
          id: `profile-${index}-${String(entry.name).replace(/\W+/g, "-").toLowerCase()}`,
          persistedName: String(entry.name),
          data,
          original: clone(data)
        });
      } else if (entry?._vsmr && typeof entry._vsmr === "object" && !Array.isArray(entry._vsmr)) {
        metadata = clone(entry._vsmr);
      } else {
        extras.push(clone(entry));
      }
    });
    return { records, metadata, extras };
  }

  function normalizeDatalinkRuntimeState(incoming = {}, fallback = {}) {
    const source = incoming && typeof incoming === "object" && !Array.isArray(incoming) ? incoming : {};
    const previous = fallback && typeof fallback === "object" && !Array.isArray(fallback) ? fallback : {};
    const readBool = (key, defaultValue = false) => Object.hasOwn(source, key)
      ? Boolean(source[key])
      : Object.hasOwn(previous, key) ? Boolean(previous[key]) : defaultValue;
    const readString = (key, defaultValue = "") => Object.hasOwn(source, key)
      ? String(source[key] ?? "")
      : Object.hasOwn(previous, key) ? String(previous[key] ?? "") : defaultValue;
    const readMinutes = (key, defaultValue) => Math.round(clamp(
      Object.hasOwn(source, key) ? source[key] : Object.hasOwn(previous, key) ? previous[key] : defaultValue,
      0,
      1440
    ));
    return {
      connected: readBool("connected"),
      connecting: readBool("connecting"),
      pollInProgress: readBool("pollInProgress"),
      controllerConnected: readBool("controllerConnected"),
      logonCallsign: readString("logonCallsign").trim().toUpperCase().slice(0, 8),
      hasPassword: readBool("hasPassword"),
      playSound: readBool("playSound"),
      cdmAutoEnabled: readBool("cdmAutoEnabled"),
      cdmDelayMinutes: readMinutes("cdmDelayMinutes", 5),
      cdmCooldownMinutes: readMinutes("cdmCooldownMinutes", 60),
      vacdmConfigured: readBool("vacdmConfigured"),
      vacdmReady: readBool("vacdmReady"),
      activeAirport: normalizeAirportCode(readString("activeAirport")),
      cdmAliasPath: readString("cdmAliasPath"),
      cdmAliasReady: readBool("cdmAliasReady"),
      statusMessage: readString("statusMessage")
    };
  }

  function createPreviewDatalinkState(activeAirport, metadata) {
    return normalizeDatalinkRuntimeState({
      controllerConnected: true,
      logonCallsign: activeAirport || "LFPG",
      hasPassword: true,
      playSound: true,
      cdmAutoEnabled: false,
      cdmDelayMinutes: 5,
      cdmCooldownMinutes: 60,
      vacdmConfigured: Boolean(String(metadata?.vacdm?.server_url || "").trim()),
      vacdmReady: Boolean(String(metadata?.vacdm?.server_url || "").trim()),
      activeAirport,
      cdmAliasPath: "C:\\EuroScope\\Alias\\alias.txt",
      cdmAliasReady: true,
      statusMessage: "Ready to connect to Hoppie."
    });
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
    migrateProfileAvisoPresetStores(metadata, records, preferred?.id || preferred?.data?.name);
    const preferredPresetStore = metadataAvisoPresetStoreForAirport(metadata, initialAirport, true);
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
      configRevision: "",
      avisoRevision: "",
      recoveryConfirmed: false,
      avisoRecoveryConfirmed: false,
      externalEditConflict: false,
      settings: {
        profileFile: "vSMR_DATA\\vSMR_Profiles.json",
        avisoFile: "vSMR_DATA\\AVISO\\LFPG.geojson",
        watchFiles: true,
        bridgeMode: "Auto detect",
        updateInterval: 250,
        resolutionPreset: preferred?.data?.targets?.small_icon_boost_resolution_preset || "1080p",
        showFps: true,
        screenRotation: 0.0,
        avisoColorPalette: "night",
        runtimeSync: true,
        confirmDelete: true,
        vacdm: true,
        dataHealth: {
          profilesHealthy: true,
          profilesUsingBackup: false,
          profilesBackupAvailable: false,
          profilesMessage: "",
          avisoHealthy: true,
          avisoMessage: ""
        }
      },
      datalink: createPreviewDatalinkState(initialAirport, metadata),
      ui: {
        page: "display",
        profileTab: "colors",
        avisoView: "text",
        selectedColorPath: "labels.departure.background_taxi_color",
        selectedTagId: "departure:taxi",
        selectedTagIds: ["departure:taxi"],
        tagSelectionAnchorId: "departure:taxi",
        selectedRuleIndex: 0,
        selectedModeIndex: 0,
        managedProfileId: preferred?.id || "",
        selectedAvisoGeometryStyleId: defaultGeometryStyleId,
        selectedAvisoGeometryStyleIds: defaultGeometryStyleId ? [defaultGeometryStyleId] : [],
        avisoGeometrySelectionAnchorId: defaultGeometryStyleId,
        selectedAvisoTextStyleId: defaultTextStyleId,
        selectedAvisoTextStyleIds: defaultTextStyleId ? [defaultTextStyleId] : [],
        avisoTextSelectionAnchorId: defaultTextStyleId,
        avisoTextColorTarget: "text",
        selectedAvisoGroupId: normalizedAviso.vsmr_groups?.[0]?.id || "",
        avisoGroupMemberSearch: "",
        avisoGroupMemberFilter: "all",
        avisoGroupContentType: "text",
        avisoGroupContentSearch: "",
        controlCenterOpen: HOST_MODE,
        runtimePopover: ""
      },
      runtime: {
        avisoInsetVisible: false,
        insets: { aviso: false, srw1: false, weather: false, timer: false },
        activeAvisoPreset: preferredPresetName,
        activeAvisoPresetScope: initialAirport,
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
  const drafts = { color: null, tag: null, rule: null, mode: null, profile: null, avisoGeometry: null, avisoTextStyle: null, avisoGroup: null, alerts: null };
  let activeTagInput = null;
  let toastTimer = 0;
  let persistentStatusState = null;
  let dismissedPersistentStatusKey = "";
  let avisoGeometryRenderOrder = [];
  let avisoTextRenderOrder = [];
  let githubResourceType = "aviso";
  let avisoGroupContentDraft = null;
  let draggedAvisoGroupId = "";
  let insetPresetDialogMode = "capture";
  let outboundMessageSequence = 0;
  const pending = { save: "", reload: "", resource: null };
  const runtimeCommandPending = new Map();
  let hostAuthoritativeReady = !HOST_MODE;
  let initialAuthoritativeMessageId = "";
  const unappliedEditorSections = new Set();
  let avisoGroupContentDirty = false;
  const expiredRequestIds = new Set();
  const datalinkPending = { settings: null, connection: null, poll: null, scan: null };
  let datalinkDraft = null;
  let datalinkBaseline = null;
  let datalinkPasswordVisible = false;
  let datalinkPasswordCommitReady = false;
  let datalinkControlsInitialized = false;
  const datalinkFieldRevisions = {
    logonCallsign: 0,
    password: 0,
    playSound: 0,
    cdmDelayMinutes: 0,
    cdmCooldownMinutes: 0
  };
  let datalinkConnectAfterSave = false;
  let datalinkQueuedReminderAction = null;
  let globalSaveAfterDatalink = false;
  let discardDatalinkDraftOnReload = false;
  let lastDatalinkStateRequestAt = 0;
  const updateCenter = {
    config: {
      schema_version: 1,
      auto_check: true,
      auto_download: true,
      auto_install: true,
      channel: "beta",
      skipped_version: ""
    },
    state: {
      schema_version: 1,
      status: "idle",
      installed_version: String(DATA.version || "Preview"),
      selected_version: "",
      available_version: "",
      download_percent: 0,
      last_checked_utc: "",
      next_check_utc: "",
      message: "Updates are checked before the vSMR runtime loads.",
      error_code: "",
      error: "",
      restart_required: false,
      release_url: ""
    },
    available: !HOST_MODE,
    configWritable: !HOST_MODE,
    configError: "",
    lastRequestAt: 0,
    pending: { state: null, settings: null, action: null }
  };
  let splitAvisoContext = null;
	let ignoreNextUncorrelatedAviso = false;
  const history = { past: [], present: null, future: [], gestureKey: "" };
  let savedSnapshot = null;
  let saveInFlightSnapshot = null;
  let activeHistoryGestureKey = "";
  let historyGestureSequence = 0;
  const historyGestureIds = new WeakMap();
  let deferredHistoryGesture = null;
  let autosaveTimer = 0;
  let autosaveQueued = false;

  function beginHistoryGesture(control) {
    if (!control || typeof control !== "object") return "";
    historyGestureSequence += 1;
    const key = `editor-${historyGestureSequence}`;
    historyGestureIds.set(control, key);
    return key;
  }

  function historyGestureKey(control) {
    return historyGestureIds.get(control) || beginHistoryGesture(control);
  }

  function withHistoryGesture(control, callback) {
    const previous = activeHistoryGestureKey;
    activeHistoryGestureKey = historyGestureKey(control);
    try {
      return callback();
    } finally {
      activeHistoryGestureKey = previous;
    }
  }

  function flushDeferredHistoryGesture(updateDirty = true) {
    const deferred = deferredHistoryGesture;
    if (!deferred) return false;
    deferredHistoryGesture = null;
    const next = captureHistorySnapshot(history.present, deferred.chunks);
    if (!snapshotsEqual(next, history.present)) history.present = next;
    history.future.length = 0;
    history.gestureKey = deferred.key;
    if (history.past.length && snapshotsEqual(history.present, history.past[history.past.length - 1]))
      history.past.pop();
    if (updateDirty) updateDirtyState();
    return true;
  }

  const UNAPPLIED_EDITOR_SECTION_SELECTOR = [
    "[data-profile-panel]",
    "[data-aviso-panel]",
    "[data-aviso-view-panel]",
    "[data-page-panel]"
  ].join(", ");

  function editorSectionKey(element) {
    if (!element?.closest) return "";
    const panel = element.closest(UNAPPLIED_EDITOR_SECTION_SELECTOR);
    if (!panel) return "";
    const attributes = [
      ["profilePanel", "profile"],
      ["avisoPanel", "aviso"],
      ["avisoViewPanel", "aviso"],
      ["pagePanel", "page"]
    ];
    for (const [datasetKey, prefix] of attributes) {
      if (panel.dataset[datasetKey] != null) return `${prefix}:${panel.dataset[datasetKey]}`;
    }
    return "";
  }

  function hasUnappliedEditorInputs() {
    // Input events are tracked until their change/blur boundary. This protects
    // a value that is still being typed from an asynchronous host repaint; it
    // never blocks automatic saving, Undo or navigation.
    return unappliedEditorSections.size > 0;
  }

  function markEditorSectionUnapplied(element) {
    const key = editorSectionKey(element);
    if (!key) return false;
    unappliedEditorSections.add(key);
    updateCommandState();
    scheduleAutosave();
    return true;
  }

  function clearUnappliedEditorSection(element) {
    const key = editorSectionKey(element);
    if (!key) return false;
    const changed = unappliedEditorSections.delete(key);
    updateCommandState();
    return changed;
  }

  function clearUnappliedEditorSectionsWithin(scope) {
    if (!scope) return;
    const keys = new Set([editorSectionKey(scope)]);
    $$(UNAPPLIED_EDITOR_SECTION_SELECTOR, scope).forEach(panel => keys.add(editorSectionKey(panel)));
    keys.delete("");
    keys.forEach(key => unappliedEditorSections.delete(key));
    updateCommandState();
  }

  function clearAllUnappliedEditorSections() {
    unappliedEditorSections.clear();
    updateCommandState();
  }

  function isProfileBoundEditorSection(key) {
    return key.startsWith("profile:") || key === "page:modes" || key === "page:profiles" ||
      key === "page:alerts" || key.startsWith("alerts:");
  }

  function hasUnappliedProfileEditorInputs() {
    return Array.from(unappliedEditorSections).some(isProfileBoundEditorSection);
  }

  function discardUnappliedProfileEditorInputs() {
    Array.from(unappliedEditorSections).filter(isProfileBoundEditorSection)
      .forEach(key => unappliedEditorSections.delete(key));
    ["color", "tag", "rule", "mode", "profile", "alerts"].forEach(key => { drafts[key] = null; });
    updateCommandState();
  }

  function activeProfileRecord() {
    return state.profiles.find(record => record.id === state.activeProfileId) || state.profiles[0];
  }
  function activeProfile() { return activeProfileRecord()?.data || {}; }
  function managedProfileRecord() {
    return state.profiles.find(record => record.id === state.ui.managedProfileId) || activeProfileRecord();
  }

  function assignAvisoPresetRoot(metadata, root) {
    if (!metadata || typeof metadata !== "object" || Array.isArray(metadata)) return false;
    if (!root || typeof root !== "object" || Array.isArray(root)) {
      delete metadata.aviso_presets;
      return true;
    }
    metadata.aviso_presets = clone(root);
    return true;
  }

  function rebasePresetStoreSnapshots(root) {
    const snapshots = [history.present, ...history.past, ...history.future, savedSnapshot];
    const visited = new Set();
    snapshots.forEach(snapshot => {
      if (!snapshot?.metadata || visited.has(snapshot)) return;
      visited.add(snapshot);
      try {
        const metadata = JSON.parse(snapshot.metadata);
        if (assignAvisoPresetRoot(metadata, root)) {
          snapshot.metadata = JSON.stringify(metadata);
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
      if (HOST_MODE) {
        const encoded = new TextEncoder().encode(JSON.stringify(message));
        if (encoded.byteLength > MAX_BRIDGE_MESSAGE_BYTES)
          throw new Error("The request exceeds the 28 MB Control Center limit");
      }
      if (window.chrome?.webview?.postMessage) window.chrome.webview.postMessage(message);
      else window.dispatchEvent(new CustomEvent("vsmr-control-center", { detail: message }));
    } catch (error) {
      console.warn("Bridge message failed", error);
      const reason = error?.message || "unknown bridge error";
      setStatus(`Could not send the request: ${reason}`, "error");
      showToast("Could not send the request", "error");
      return "";
    }
    return message.id;
  }

  function rebaseDisplayModeSelections(incomingRecords) {
    const activeModes = new Map();
    (incomingRecords || []).forEach(record => {
      const name = String(record?.data?.name || "").trim().toLowerCase();
      const active = String(record?.data?.filters?.display_modes?.active || "");
      if (name && active) activeModes.set(name, active);
    });
    if (!activeModes.size) return;

    const applySelections = records => {
      let changed = false;
      (records || []).forEach(record => {
        const currentName = String(record?.data?.name || "").trim().toLowerCase();
        const persistedName = String(record?.persistedName || "").trim().toLowerCase();
        const active = activeModes.get(currentName) || activeModes.get(persistedName);
        const displayModes = record?.data?.filters?.display_modes;
        if (active && displayModes && displayModes.active !== active) {
          displayModes.active = active;
          changed = true;
        }
      });
      return changed;
    };

    applySelections(state.profiles);
    const snapshots = [history.present, ...history.past, ...history.future, savedSnapshot];
    const visited = new Set();
    snapshots.forEach(snapshot => {
      if (!snapshot?.profiles || visited.has(snapshot)) return;
      visited.add(snapshot);
      try {
        const records = JSON.parse(snapshot.profiles);
        if (applySelections(records)) snapshot.profiles = JSON.stringify(records);
      } catch (error) {
        console.warn("Could not rebase display mode selections in editor history", error);
      }
    });
  }

  function armPendingTimeout(slot, requestId) {
    if (!requestId) return;
    window.setTimeout(() => {
      const currentId = slot === "resource" ? pending.resource?.id : pending[slot];
      if (currentId !== requestId) return;
      if (slot === "resource") {
        pending.resource = null;
        state.recoveryConfirmed = false;
        state.avisoRecoveryConfirmed = false;
        setGithubRequestPending(false);
      } else {
        pending[slot] = "";
        if (slot === "save") saveInFlightSnapshot = null;
      }
      expiredRequestIds.add(requestId);
      while (expiredRequestIds.size > 32)
        expiredRequestIds.delete(expiredRequestIds.values().next().value);
      state.externalEditConflict = true;
	  clearSplitAvisoContext(requestId);
      updateCommandState();
      const message = "The native operation timed out. Its result was not confirmed; reload before saving again.";
      setStatus(message, "error");
      showToast("Native operation timed out", "error");
    }, REQUEST_TIMEOUT_MS);
  }

  function snapshotChunk(value) {
    return JSON.stringify(value ?? null);
  }

  function captureHistorySnapshot(reuse = history.present, changedChunks = null) {
    const historySettings = clone(state.settings);
    delete historySettings.profileFile;
    delete historySettings.avisoFile;
    delete historySettings.dataHealth;
    const values = {
      profiles: state.profiles,
      metadata: state.metadata,
      profileExtras: state.profileExtras,
      aviso: state.aviso,
      settings: historySettings
    };
    const snapshot = {};
    const changed = changedChunks ? new Set(changedChunks) : null;
    Object.entries(values).forEach(([key, value]) => {
      if (reuse?.[key] != null && changed && !changed.has(key)) {
        snapshot[key] = reuse[key];
        return;
      }
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

  function resourceHasUnsavedChanges(resource) {
    if (!savedSnapshot) return Boolean(state.dirty);
    if (resource === "aviso")
      return snapshotChunk(state.aviso) !== savedSnapshot.aviso;
    if (resource === "profiles") {
      const current = captureHistorySnapshot();
      return ["profiles", "metadata", "profileExtras", "settings"]
        .some(key => current[key] !== savedSnapshot[key]);
    }
    return Boolean(state.dirty);
  }

  function confirmResourceReplacement(resource) {
    if (!resourceHasUnsavedChanges(resource)) return true;
    const label = resource === "aviso" ? "AVISO" : "profiles";
    return window.confirm(`Discard unsaved ${label} edits and load another file?`);
  }

  function workspaceBusyMessage() {
    // Automatic persistence is deliberately non-modal: users can continue
    // editing while the current snapshot is written in the background.
    if (pending.reload) return "Reloading vSMR data...";
    if (pending.resource) return "Loading and validating vSMR data...";
    if (runtimeCommandPending.size) return "Applying vSMR changes...";
	if (splitAvisoContext) return "Synchronizing vSMR data...";
    return "";
  }

  function clearSplitAvisoContext(expectedId = null) {
    const context = splitAvisoContext;
    if (!context) return null;
    const contextId = String(context.id || "");
	if (expectedId !== null && contextId !== String(expectedId || "")) return null;
    if (context.timer) window.clearTimeout(context.timer);
    splitAvisoContext = null;
    return context;
  }

  function stageSplitAvisoContext(context) {
    clearSplitAvisoContext();
	if (!context.id) ignoreNextUncorrelatedAviso = false;
    splitAvisoContext = context;
    context.timer = window.setTimeout(() => {
      if (splitAvisoContext !== context) return;
      splitAvisoContext = null;
      const requestId = String(context.id || "");
      if (requestId) expiredRequestIds.add(requestId);
	  else ignoreNextUncorrelatedAviso = true;
      state.externalEditConflict = true;
      updateCommandState();
      const message = "The authoritative AVISO update was incomplete. Reload before editing or saving.";
      setStatus(message, "error");
      showToast("vSMR synchronization timed out", "error");
    }, REQUEST_TIMEOUT_MS);
    updateCommandState();
  }

  function updateWorkspaceInterlock() {
    const busyMessage = workspaceBusyMessage();
    const operationBusy = Boolean(busyMessage);
    const locked = !hostAuthoritativeReady || operationBusy;
    const activeElement = document.activeElement;
    if (locked && activeElement &&
        (activeElement.closest?.(".page-rail") || activeElement.closest?.(".workspace"))) {
      activeElement.blur();
    }
    [$(".page-rail"), $(".workspace")].forEach(element => {
      if (!element) return;
      element.inert = locked;
      element.setAttribute("aria-disabled", String(locked));
    });
    const overlay = $("#operationBusyOverlay");
    if (overlay) {
      overlay.hidden = !hostAuthoritativeReady || !operationBusy;
      overlay.textContent = busyMessage || "Applying vSMR changes...";
    }
    $("#controlWindow")?.setAttribute(
      "aria-busy",
      String(!hostAuthoritativeReady || operationBusy)
    );
  }

  function updateCommandState() {
    const busy = Boolean(
	  pending.save || pending.reload || pending.resource ||
	  runtimeCommandPending.size || splitAvisoContext || datalinkPending.settings ||
      globalSaveAfterDatalink
	);
    const airportMismatch = Boolean(state.airport && state.hostAirport && state.airport !== state.hostAirport);
    const profilesUnsafe = state.settings?.dataHealth?.profilesHealthy === false && !state.recoveryConfirmed;
    const externalConflict = Boolean(state.externalEditConflict);
    const reloadButton = $("#reloadButton");
    const undoButton = $("#undoButton");
    const redoButton = $("#redoButton");
    if (reloadButton) {
	  reloadButton.disabled = !hostAuthoritativeReady || busy;
      reloadButton.classList.toggle("pending", Boolean(pending.reload));
      reloadButton.title = pending.reload ? "Reverting…" : "Revert to saved configuration";
    }
	if (undoButton) undoButton.disabled = !hostAuthoritativeReady || busy || airportMismatch || externalConflict || history.past.length === 0;
	if (redoButton) redoButton.disabled = !hostAuthoritativeReady || busy || airportMismatch || externalConflict || history.future.length === 0;
    updateWorkspaceInterlock();
  }

  function setHostAuthoritativeReady(ready) {
    hostAuthoritativeReady = !HOST_MODE || Boolean(ready);
    const overlay = $("#hostLoadingOverlay");
    if (overlay) overlay.hidden = hostAuthoritativeReady;
    updateCommandState();
  }

  function updateDirtyState(message = "") {
    state.dirty = !snapshotsEqual(history.present, savedSnapshot);
    const visuallyDirty = state.dirty || hasUnappliedEditorInputs() || hasDatalinkDraftChanges();
    updateCommandState();
    if (message) setStatus(message, visuallyDirty ? "info" : "");
  }

  function recordHistoryState(changedChunks = null, gestureKey = "") {
    if (gestureKey && history.gestureKey === gestureKey) {
      const chunks = deferredHistoryGesture?.key === gestureKey
        ? deferredHistoryGesture.chunks
        : new Set();
      (changedChunks || ["profiles", "metadata", "profileExtras", "aviso", "settings"])
        .forEach(key => chunks.add(key));
      deferredHistoryGesture = { key: gestureKey, chunks };
      return true;
    }
    flushDeferredHistoryGesture(false);
    const next = captureHistorySnapshot(history.present, changedChunks);
    if (history.present && snapshotsEqual(next, history.present)) return false;
    const coalesce = Boolean(gestureKey && history.gestureKey === gestureKey);
    if (history.present && !coalesce) {
      history.past.push(history.present);
      if (history.past.length > HISTORY_LIMIT) history.past.splice(0, history.past.length - HISTORY_LIMIT);
    }
    history.present = next;
    history.future.length = 0;
    history.gestureKey = gestureKey;
    return true;
  }

  function resetHistory(saved = true) {
    history.past.length = 0;
    history.future.length = 0;
    history.gestureKey = "";
    deferredHistoryGesture = null;
    history.present = captureHistorySnapshot(null);
    if (saved) savedSnapshot = history.present;
    updateDirtyState();
  }

  function restoreHistorySnapshot(snapshot) {
    if (!snapshot) return;
    const preservedUi = state.ui;
    const preservedActiveProfileId = state.activeProfileId;
    const preservedResourcePaths = {
      profileFile: state.settings.profileFile,
      avisoFile: state.settings.avisoFile,
      dataHealth: clone(state.settings.dataHealth)
    };
    state.profiles = JSON.parse(snapshot.profiles);
    state.metadata = JSON.parse(snapshot.metadata);
    state.profileExtras = JSON.parse(snapshot.profileExtras || "[]");
    state.activeProfileId = preservedActiveProfileId;
    state.aviso = JSON.parse(snapshot.aviso);
    state.settings = { ...JSON.parse(snapshot.settings), ...preservedResourcePaths };
    state.ui = preservedUi;
    if (!state.profiles.some(record => record.id === state.activeProfileId)) state.activeProfileId = state.profiles[0]?.id || "";
    if (!state.profiles.some(record => record.id === state.ui.managedProfileId)) state.ui.managedProfileId = state.activeProfileId;
    Object.keys(drafts).forEach(key => drafts[key] = null);
    clearAllUnappliedEditorSections();
    avisoGroupContentDirty = false;
    history.present = snapshot;
    history.gestureKey = "";
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

  function renderPersistentStatus() {
    const region = $("#persistentStatus");
    const text = $("#persistentStatusText");
    const actions = $("#persistentStatusActions");
    if (!region || !text || !actions) return;
    const current = persistentStatusState;
    const key = current ? `${current.type}|${current.message}` : "";
    if (!current || !current.message || key === dismissedPersistentStatusKey) {
      region.hidden = true;
      actions.replaceChildren();
      return;
    }
    region.hidden = false;
    region.className = `persistent-status ${current.type === "info" ? "info" : "error"}`;
    text.textContent = current.message;
    text.title = current.message;
    actions.replaceChildren();
    (current.actions || []).forEach(action => {
      const button = document.createElement("button");
      button.type = "button";
      button.dataset.action = action.action;
      button.textContent = action.label;
      if (action.disabled) button.disabled = true;
      actions.append(button);
    });
  }

  function setPersistentStatus(message, type = "error", actions = [], origin = "native") {
    const normalized = String(message || "").trim();
    if (!normalized) {
      if (!origin || persistentStatusState?.origin === origin) {
        persistentStatusState = null;
        // Dismissal applies to one occurrence only.  If the condition clears,
        // the same error must be visible should it happen again later.
        dismissedPersistentStatusKey = "";
      }
      renderPersistentStatus();
      return;
    }
    const nextKey = `${type}|${normalized}`;
    // Native errors are discrete operation results.  Treat every delivery as a
    // new occurrence so dismissing one failure cannot hide an identical failure
    // from a later retry.
    if (origin === "native" ||
        (persistentStatusState && `${persistentStatusState.type}|${persistentStatusState.message}` !== nextKey))
      dismissedPersistentStatusKey = "";
    persistentStatusState = { message: normalized, type, actions, origin };
    renderPersistentStatus();
  }

  function renderDataHealthStatus() {
    const health = state.settings?.dataHealth || {};
    if (health.profilesHealthy === false) {
      const actions = [];
      if (health.profilesBackupAvailable) actions.push({ label: "Restore .bak", action: "restore-profiles-backup" });
      actions.push({ label: "Defaults", action: "restore-bundled-defaults" });
      setPersistentStatus(
        health.profilesMessage || "The profiles source is unavailable or invalid.",
        "error",
        actions,
        "health"
      );
      return;
    }
    if (health.avisoHealthy === false) {
      setPersistentStatus(
        health.avisoMessage || "The active airport AVISO source is unavailable or invalid.",
        "error",
        [{ label: "Settings", action: "open-settings" }],
        "health"
      );
      return;
    }
    const legacyPresets = legacyPresetAssignmentSummary();
    if (legacyPresets.sources.length) {
      const airport = activePresetAirport();
      const countText = legacyPresets.count === 1
        ? "1 legacy inset preset has no airport"
        : `${legacyPresets.count} legacy inset presets have no airport`;
      const actions = isAirportCode(airport)
		? [{
			label: `Assign to ${airport}`,
			action: "assign-legacy-inset-presets",
			disabled: Boolean(
			  state.dirty || pending.save || pending.reload || pending.resource ||
			  runtimeCommandPending.size || splitAvisoContext
			)
		  }]
        : [];
      setPersistentStatus(
		`${countText}. Choose its airport explicitly${state.dirty ? " after saving or reloading current edits" : ""}.`,
        "info",
        actions,
        "legacy-presets"
      );
      return;
    }
    if (persistentStatusState?.origin === "legacy-presets")
      setPersistentStatus("", "", [], "legacy-presets");
    if (health.profilesMessage) {
      setPersistentStatus(health.profilesMessage, "info", [], "health");
      return;
    }
    if (persistentStatusState?.origin === "health") setPersistentStatus("", "", [], "health");
  }

  function setStatus(message, type = "") {
    const text = $("#statusText");
    const light = $("#statusLight");
    if (text) text.textContent = message;
    if (light) light.className = `status-light ${type}`.trim();
    document.documentElement.dataset.status = type || "ready";
    if (type === "error") setPersistentStatus(message, "error", [], "native");
  }
  function markDirty(message = "Saving automatically…", changedChunks = null, gestureKey = activeHistoryGestureKey) {
    recordHistoryState(changedChunks, gestureKey);
    updateDirtyState(message);
    scheduleAutosave();
  }

  function markSaved(message = "Saved") {
    state.profiles.forEach(record => {
      record.original = clone(record.data);
      record.persistedName = String(record.data?.name || "");
    });
    history.present = captureHistorySnapshot();
    history.gestureKey = "";
    savedSnapshot = history.present;
    state.dirty = false;
    state.recoveryConfirmed = false;
    state.avisoRecoveryConfirmed = false;
    state.externalEditConflict = false;
    updateDirtyState();
    setStatus(message);
    if (!hasAutosaveWork()) cancelAutosave();
    else scheduleAutosave();
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
    if (state.ui.page === "datalink") suffix = "CPDLC / PDC";
    if (state.ui.page === "settings") suffix = "Settings";
    context.textContent = `${profileName} · ${suffix}`;
  }
  function setPage(page) {
    if (["cpdlc", "cdm"].includes(page)) page = "datalink";
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
    if (page === "datalink") {
      renderDatalink();
      requestDatalinkState();
    }
    if (page === "settings") {
      renderSettings();
      renderUpdateCenter();
      requestUpdateState(true);
    }
    updateContext();
  }

  function setProfileTab(tab) {
    if (!PROFILE_TITLES[tab]) return;
    state.ui.profileTab = tab;
    syncProfileTabSelection();
    renderCurrentProfileTab();
    updateContext();
  }

  function syncProfileTabSelection() {
    const tab = state.ui.profileTab;
    $$('[data-profile-tab]').forEach(button => button.classList.toggle("active", button.dataset.profileTab === tab));
    $$('[data-profile-panel]').forEach(panel => panel.classList.toggle("active", panel.dataset.profilePanel === tab));
  }

  function switchActiveProfile(profileId, syncFilters = true) {
    if (!state.profiles.some(record => record.id === profileId)) return false;
    if (profileId === state.activeProfileId) return true;
	if (state.dirty || hasUnappliedEditorInputs() || avisoGroupContentDirty) {
	  showToast("Wait for automatic saving or revert current edits before switching profile", "error");
	  return false;
	}
    state.activeProfileId = profileId;
    state.ui.managedProfileId = profileId;
    const profile = activeProfile();
    const colors = collectProfileColors(profile);
    if (!colors.some(entry => entry.id === state.ui.selectedColorPath)) state.ui.selectedColorPath = colors[0]?.id || "";
    state.ui.selectedRuleIndex = 0;
    const modes = profile.filters?.display_modes?.items || [];
    state.ui.selectedModeIndex = Math.max(0, modes.findIndex(mode => mode.name === profile.filters?.display_modes?.active));
    state.ui.selectedTagId = "departure:taxi";
    state.ui.selectedTagIds = [state.ui.selectedTagId];
    state.ui.tagSelectionAnchorId = state.ui.selectedTagId;
    discardUnappliedProfileEditorInputs();
    renderAllProfileSections();
    drafts.alerts = null;
    if (state.ui.page === "alerts") renderAlerts();
    renderRuntimeMenu();
    updateContext();
    return true;
  }

  function captureRuntimeCommandRollback() {
    return {
      profiles: clone(state.profiles),
      metadata: clone(state.metadata),
      profileExtras: clone(state.profileExtras),
      aviso: clone(state.aviso),
      settings: clone(state.settings),
      activeProfileId: state.activeProfileId,
      airport: state.airport,
      hostAirport: state.hostAirport,
      ui: clone(state.ui),
      runtime: clone(state.runtime),
      datalink: clone(state.datalink),
      drafts: clone(drafts),
      historyPast: history.past.slice(),
      historyPresent: history.present,
      historyFuture: history.future.slice(),
      savedSnapshot,
      dirty: state.dirty,
      unappliedEditorSections: Array.from(unappliedEditorSections),
      avisoGroupContentDirty,
      recoveryConfirmed: state.recoveryConfirmed,
      avisoRecoveryConfirmed: state.avisoRecoveryConfirmed,
      externalEditConflict: state.externalEditConflict,
      configRevision: state.configRevision,
      avisoRevision: state.avisoRevision
    };
  }

  function restoreRuntimeCommandRollback(rollback) {
    if (!rollback) return;
    state.profiles = rollback.profiles;
    state.metadata = rollback.metadata;
    state.profileExtras = rollback.profileExtras;
    state.aviso = rollback.aviso;
    state.settings = rollback.settings;
    state.activeProfileId = rollback.activeProfileId;
    state.airport = rollback.airport;
    state.hostAirport = rollback.hostAirport;
    state.ui = rollback.ui;
    state.runtime = rollback.runtime;
    state.datalink = rollback.datalink;
    Object.keys(drafts).forEach(key => { drafts[key] = rollback.drafts?.[key] ?? null; });
    history.past.splice(0, history.past.length, ...rollback.historyPast);
    history.present = rollback.historyPresent;
    history.future.splice(0, history.future.length, ...rollback.historyFuture);
    savedSnapshot = rollback.savedSnapshot;
    state.dirty = rollback.dirty;
    unappliedEditorSections.clear();
    (rollback.unappliedEditorSections || []).forEach(key => unappliedEditorSections.add(String(key)));
    avisoGroupContentDirty = Boolean(rollback.avisoGroupContentDirty);
    state.recoveryConfirmed = Boolean(rollback.recoveryConfirmed);
    state.avisoRecoveryConfirmed = Boolean(rollback.avisoRecoveryConfirmed);
    state.externalEditConflict = Boolean(rollback.externalEditConflict);
    state.configRevision = rollback.configRevision || "";
    state.avisoRevision = rollback.avisoRevision || "";
    renderAll();
    updateDirtyState();
  }

  function postRuntimeCommand(type, payload, rollback) {
	if (runtimeCommandPending.size || splitAvisoContext || pending.save || pending.reload || pending.resource) {
      restoreRuntimeCommandRollback(rollback);
      setStatus("Wait for the current vSMR operation to finish.", "error");
      return "";
    }
    const requestId = postBridge(type, payload);
    if (!requestId) {
      restoreRuntimeCommandRollback(rollback);
      return "";
    }
    const timer = window.setTimeout(() => {
      const pendingCommand = runtimeCommandPending.get(requestId);
      if (!pendingCommand) return;
      runtimeCommandPending.delete(requestId);
      restoreRuntimeCommandRollback(pendingCommand.rollback);
      expiredRequestIds.add(requestId);
      state.externalEditConflict = true;
	  clearSplitAvisoContext(requestId);
      setStatus("The runtime change was not confirmed and has been reverted.", "error");
      showToast("Runtime change timed out", "error");
      updateCommandState();
    }, REQUEST_TIMEOUT_MS);
    runtimeCommandPending.set(requestId, {
      rollback,
      timer,
      type,
      unappliedAtSend: hasUnappliedEditorInputs() || avisoGroupContentDirty
    });
    updateCommandState();
    return requestId;
  }

  function finishRuntimeCommand(requestId, restore = false) {
    const pendingCommand = runtimeCommandPending.get(requestId);
    if (!pendingCommand) return null;
    window.clearTimeout(pendingCommand.timer);
    runtimeCommandPending.delete(requestId);
    if (restore) restoreRuntimeCommandRollback(pendingCommand.rollback);
    updateCommandState();
    return {
      type: pendingCommand.type,
      trustedCleanResponse: !restore && !pendingCommand.rollback?.dirty &&
        !pendingCommand.unappliedAtSend && !hasUnappliedEditorInputs() && !avisoGroupContentDirty
    };
  }

  function pendingRuntimeCommandInfo(requestId) {
    const pendingCommand = runtimeCommandPending.get(requestId);
    if (!pendingCommand) return null;
    return {
      type: pendingCommand.type,
      trustedCleanResponse: !pendingCommand.rollback?.dirty &&
        !pendingCommand.unappliedAtSend && !hasUnappliedEditorInputs() && !avisoGroupContentDirty
    };
  }

  function postActiveProfileChange(rollback) {
    const record = activeProfileRecord();
    if (!record) return;
    postRuntimeCommand(
      "runtime.profile.change",
      { profileId: record.id, profile: record.data.name },
      rollback
    );
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

  function buildAvisoGroupIndex() {
    const index = new Map(avisoGroups().map(group => [group.id, {
      indices: [],
      counts: { total: 0, text: 0, line: 0, area: 0 }
    }]));
    avisoFeatures().forEach((feature, featureIndex) => {
      const groupIds = featureGroupIds(feature);
      if (!groupIds.length) return;
      const kind = isAvisoTextFeature(feature) ? "text" : inferAvisoObjectType(feature).toLowerCase();
      groupIds.forEach(groupId => {
        if (!index.has(groupId)) {
          index.set(groupId, { indices: [], counts: { total: 0, text: 0, line: 0, area: 0 } });
        }
        const entry = index.get(groupId);
        entry.indices.push(featureIndex);
        entry.counts.total += 1;
        if (kind === "text") entry.counts.text += 1;
        else if (kind === "line") entry.counts.line += 1;
        else entry.counts.area += 1;
      });
    });
    return index;
  }

  function avisoGroupMemberIndices(groupId, groupIndex = buildAvisoGroupIndex()) {
    return groupIndex.get(groupId)?.indices || [];
  }

  function avisoGroupCounts(groupId, groupIndex = buildAvisoGroupIndex()) {
    return groupIndex.get(groupId)?.counts || { total: 0, text: 0, line: 0, area: 0 };
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

  function openControlCenter(page = state.ui.page, avisoView = "") {
    state.ui.controlCenterOpen = true;
    state.ui.runtimePopover = "";
    syncSurfaceVisibility();
    setPage(page);
    if (page === "aviso" && ["geometry", "text"].includes(avisoView)) {
      state.ui.avisoView = avisoView;
      renderAviso();
    }
    renderRuntimeMenu();
  }

  function finalizeControlCenterClose() {
    if (HOST_MODE) {
      state.ui.controlCenterOpen = false;
      postBridge("window.close", {
        dirty: state.dirty || hasUnappliedEditorInputs() || hasDatalinkDraftChanges()
      });
      return;
    }
    state.ui.controlCenterOpen = false;
    syncSurfaceVisibility();
    renderRuntimeMenu();
  }

  function closeControlCenter() {
    // Closing only hides the Control Center. Keep the in-memory form draft so
    // reopening restores it; automatic persistence remains the Control Center's job.
    if (datalinkControlsInitialized && datalinkDraft && datalinkBaseline)
      captureDatalinkDraftFromControls();
    finalizeControlCenterClose();
  }

  function activePresetAirport() {
    return normalizeAirportCode(state.hostAirport);
  }

  function activePresetScope() {
    return activePresetAirport();
  }

  function airportAvisoPresetStore() {
    return metadataAvisoPresetStoreForAirport(state.metadata, activePresetAirport(), true);
  }

  function avisoPresets() { return airportAvisoPresetStore().items; }

  function activeAvisoPreset() {
    if (state.runtime.activeAvisoPresetScope !== activePresetScope()) return syncRuntimePresetForAirport();
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

  function syncRuntimePresetForAirport() {
    const store = airportAvisoPresetStore();
    const preset = store.items.find(item => item.name === store.default) || store.items[0] || null;
    state.runtime.activeAvisoPreset = preset?.name || "";
    state.runtime.activeAvisoPresetScope = activePresetScope();
    state.runtime.avisoInsetSnapshot = preset ? clone(preset) : null;
    return preset;
  }

  function insetState(kind) {
    state.runtime.insets ||= { aviso: false, srw1: false, weather: false, timer: false };
    return Boolean(state.runtime.insets[kind]);
  }

  function setRuntimePopover(kind = "") {
    const next = state.ui.runtimePopover === kind ? "" : kind;
    state.ui.runtimePopover = ["mode", "groups", "inset", "profile"].includes(next) ? next : "";
    renderRuntimeMenu();
  }

  function renderRuntimeMenu() {
    const menu = $("#runtimeMenu");
    if (!menu || HOST_MODE) return;
    const groups = avisoGroups();
    const visibleGroups = groups.filter(group => group.visible !== false).length;
    const profile = activeProfile();
    const mode = activeModeName();
    const preset = activeAvisoPreset();
    const anyInset = ["aviso", "srw1", "weather", "timer"].some(insetState);

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
    insetButton.title = `Insets · AVISO ${insetState("aviso") ? "on" : "off"}, SRW1 ${insetState("srw1") ? "on" : "off"}, Weather ${insetState("weather") ? "on" : "off"}, Timer ${insetState("timer") ? "on" : "off"}${preset ? ` · ${preset.name}` : ""}`;
    insetButton.setAttribute("aria-label", insetButton.title);
    insetButton.classList.toggle("active", anyInset);
    ["aviso", "srw1", "weather", "timer"].forEach(kind => {
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
      const store = airportAvisoPresetStore();
      const insetRows = [
        ["aviso", "AVISO inset"],
        ["srw1", "SRW 1"],
        ["weather", "Weather"],
        ["timer", "Timer"]
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
	if (modeName === displayModes.active) {
	  state.ui.runtimePopover = "";
	  renderRuntimeMenu();
	  return;
	}
	if (state.dirty || hasUnappliedEditorInputs() || avisoGroupContentDirty) {
	  showToast("Wait for automatic saving or revert current edits before changing mode", "error");
	  return;
	}
	const modeEditor = $("[data-page-panel='modes']");
	const modeSectionDirty = unappliedEditorSections.has(editorSectionKey(modeEditor));
	if (modeSectionDirty &&
		!window.confirm("Discard unapplied editor fields and change display mode?")) return;
	const rollback = captureRuntimeCommandRollback();
	drafts.mode = null;
	clearUnappliedEditorSection(modeEditor);
    displayModes.active = modeName;
    state.ui.selectedModeIndex = Math.max(0, displayModes.items.findIndex(mode => mode.name === modeName));
    state.ui.runtimePopover = "";
    renderModes();
    renderRuntimeMenu();
	postRuntimeCommand(
	  "runtime.mode.change",
	  { profile: activeProfile().name, mode: modeName },
	  rollback
	);
    showToast(`Mode: ${modeName}`, "success");
  }

  function toggleRuntimeGroup(groupId) {
    const group = avisoGroups().find(item => item.id === groupId);
    if (!group) return;
	const rollback = captureRuntimeCommandRollback();
    group.visible = group.visible === false;
    if (drafts.avisoGroup?.id === group.id) drafts.avisoGroup.data.visible = group.visible;
    markDirty(`Group ${group.visible ? "shown" : "hidden"}`, ["aviso"]);
	postRuntimeCommand(
	  "aviso.group.visibility",
	  { id: group.id, name: group.name, visible: group.visible },
	  rollback
	);
    renderRuntimeMenu();
    if (state.ui.page === "groups") renderAvisoGroups();
  }

  function toggleInsetWindow(kind) {
    if (!["aviso", "srw1", "weather", "timer"].includes(kind)) return;
	const rollback = captureRuntimeCommandRollback();
    state.runtime.insets ||= { aviso: false, srw1: false, weather: false, timer: false };
    state.runtime.insets[kind] = !state.runtime.insets[kind];
    if (kind === "aviso") state.runtime.avisoInsetVisible = state.runtime.insets[kind];
    const preset = activeAvisoPreset();
    const action = kind === "srw1" ? "display.srw.toggle" : "aviso.inset.toggle";
	postRuntimeCommand(
	  action,
	  { airport: activePresetAirport(), window: kind, visible: state.runtime.insets[kind], preset: preset?.name || "", profile: activeProfile().name },
	  rollback
	);
    renderRuntimeMenu();
    showToast(`${kind === "aviso" ? "AVISO inset" : kind === "weather" ? "Weather" : kind === "timer" ? "Timer" : kind.toUpperCase()} ${state.runtime.insets[kind] ? "shown" : "hidden"}`, "success");
  }

  function loadAvisoPreset(name) {
    const preset = avisoPresets().find(item => item.name === name);
    if (!preset) return;
	const rollback = captureRuntimeCommandRollback();
    state.runtime.activeAvisoPreset = preset.name;
    state.runtime.activeAvisoPresetScope = activePresetScope();
    state.runtime.avisoInsetSnapshot = clone(preset);
	postRuntimeCommand(
	  "aviso.inset.preset.load",
	  { airport: activePresetAirport(), preset: clone(preset) },
	  rollback
	);
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
    const store = airportAvisoPresetStore();
    const current = activeAvisoPreset();
	const rollback = captureRuntimeCommandRollback();
    if (insetPresetDialogMode === "rename") {
      if (!current) return;
      if (store.items.some(item => item !== current && item.name.toLowerCase() === name.toLowerCase())) { showToast("A preset with this name already exists", "error"); return; }
      const oldName = current.name;
	  postRuntimeCommand(
		"aviso.inset.preset.rename",
		{ airport: activePresetAirport(), oldName, name, linked_movement: linked },
		rollback
	  );
    } else {
      if (store.items.some(item => item.name.toLowerCase() === name.toLowerCase())) { showToast("A preset with this name already exists", "error"); return; }
	  postRuntimeCommand("aviso.inset.preset.capture", {
        airport: activePresetAirport(),
        preset: { name, linked_movement: linked }
      }, rollback);
    }
    dialog.close();
    renderRuntimeMenu();
  }

  function updateAvisoPreset() {
    const current = activeAvisoPreset();
    if (!current) return;
	postRuntimeCommand(
	  "aviso.inset.preset.update",
	  { airport: activePresetAirport(), preset: clone(current) },
	  captureRuntimeCommandRollback()
	);
    renderRuntimeMenu();
  }

  function resetAvisoPreset() {
    const current = activeAvisoPreset();
    if (!current) return;
	postRuntimeCommand(
	  "aviso.inset.preset.reset",
	  { airport: activePresetAirport(), preset: current.name },
	  captureRuntimeCommandRollback()
	);
    renderRuntimeMenu();
  }

  function duplicateAvisoPreset() {
    const current = activeAvisoPreset();
    if (!current) return;
    const name = uniqueInsetPresetName(`${current.name} copy`);
	postRuntimeCommand("aviso.inset.preset.duplicate", {
      airport: activePresetAirport(),
      source: current.name,
      preset: { name }
    }, captureRuntimeCommandRollback());
    renderRuntimeMenu();
  }

  function setDefaultAvisoPreset() {
    const current = activeAvisoPreset();
    if (!current) return;
	postRuntimeCommand(
	  "aviso.inset.preset.default",
	  { airport: activePresetAirport(), preset: current.name },
	  captureRuntimeCommandRollback()
	);
    renderRuntimeMenu();
  }

  function deleteAvisoPreset() {
    const current = activeAvisoPreset();
    if (!current || !confirmDelete(`Delete the inset preset “${current.name}”?`)) return;
	postRuntimeCommand(
	  "aviso.inset.preset.delete",
	  { airport: activePresetAirport(), preset: current.name },
	  captureRuntimeCommandRollback()
	);
    renderRuntimeMenu();
  }

  function toggleRuntimePresetLinked(checked) {
    const current = activeAvisoPreset();
    if (!current) return;
	postRuntimeCommand("aviso.inset.preset.linked", {
      airport: activePresetAirport(),
      preset: current.name,
      linked_movement: Boolean(checked)
    }, captureRuntimeCommandRollback());
    renderRuntimeMenu();
  }

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
    roots.forEach(root => {
      if (root !== "targets") {
        visit(profile?.[root], [root]);
        return;
      }
      const targets = profile?.targets;
      const iconStyle = String(targets?.icon_style || "realistic").toLowerCase();
      if (iconStyle === "nova") {
        visit(targets?.target_color, ["targets", "target_color"]);
      } else {
        visit(targets?.departure, ["targets", "departure"]);
        visit(targets?.arrival, ["targets", "arrival"]);
      }
    });
    return entries.sort((a, b) => {
      const familyOrder = value => {
        const index = COLOR_FAMILY_ORDER.indexOf(value);
        return index < 0 ? 999 : index;
      };
      const sectionOrder = value => {
        const index = COLOR_SECTION_ORDER.indexOf(value || "");
        return index < 0 ? 999 : index;
      };
      const tagColorOrder = entry => {
        if (entry.family !== "Tags" || !TAG_SCOPES.includes(String(entry.path[1] || ""))) return 999;
        const scope = String(entry.path[1]);
        const statusKeys = ["default", ...TAG_STATUS_ORDER]
          .map(status => TAG_STATUS_COLOR_KEYS[scope]?.[status])
          .filter(Boolean);
        const trailingKeys = ["text_on_ground_color", "text_airborne_color", "text_color", "background_no_sid_color"];
        const orderedKeys = [...new Set([...statusKeys, ...trailingKeys])];
        const index = orderedKeys.indexOf(entry.key);
        return index < 0 ? 999 : index;
      };
      return familyOrder(a.family) - familyOrder(b.family)
        || sectionOrder(a.section) - sectionOrder(b.section)
        || a.family.localeCompare(b.family)
        || a.section.localeCompare(b.section)
        || tagColorOrder(a) - tagColorOrder(b)
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
    if (root === "approach_insets") return { family: "SRW 1", section: "", group: "SRW 1" };
    const family = humanize(root);
    return { family, section: "", group: family };
  }

  function selectedColorEntry() {
    const entries = collectProfileColors(activeProfile());
    const selected = entries.find(entry => entry.id === state.ui.selectedColorPath) || entries[0];
    if (selected && state.ui.selectedColorPath !== selected.id) state.ui.selectedColorPath = selected.id;
    return selected;
  }

  function renderColors() {
    const entries = collectProfileColors(activeProfile());

    const groups = new Map();
    entries.forEach(entry => {
      const caption = entry.family === "Targets" && entry.section === "General"
        ? "Target"
        : entry.section ? `${entry.family} · ${entry.section}` : entry.family;
      const key = `${entry.family}:${entry.section || "general"}`;
      if (!groups.has(key)) groups.set(key, { key, caption, family: entry.family, section: entry.section, items: [] });
      groups.get(key).items.push(entry);
    });

    $("#colorTree").innerHTML = [...groups.values()].map(group => {
      const groupKey = `colors:${group.key}`;
      const collapsed = treeState.colors.has(groupKey);
      const accent = colorToHex(group.items[0]?.color, "#5096b4");
      const rows = group.items.map(entry => {
        const hex = colorToHex(entry.color).toUpperCase();
        return `<button type="button" class="menu-tree-row color-menu-row ${entry.id === state.ui.selectedColorPath ? "active" : ""}" data-color-path="${escapeHtml(entry.id)}" style="--node-color:${hex}" title="${escapeHtml(entry.name)}">
          <span class="menu-row-swatch tree-color-swatch" aria-hidden="true"></span>
          <span class="menu-row-title">${escapeHtml(entry.name)}</span>
        </button>`;
      }).join("");
      return `<section class="menu-tree-section color-menu-section" style="--menu-accent:${accent}">
        <button type="button" class="menu-tree-caption" data-tree-toggle="colors" data-tree-key="${escapeHtml(groupKey)}" aria-expanded="${!collapsed}">
          <span class="menu-tree-caret" aria-hidden="true">${collapsed ? "▸" : "▾"}</span>
          <span class="menu-tree-caption-text">${escapeHtml(group.caption)}</span>
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

  function createAvisoColorDraft(signature, colorResult, fallback, opacityResult = null) {
    const hex = normalizeHex(colorResult?.value, fallback);
    const rgb = hexToColor(hex);
    const hsv = rgbToHsv(rgb.r, rgb.g, rgb.b);
    return {
      signature,
      hex,
      h: hsv.h,
      s: hsv.s,
      v: hsv.v,
      colorMixed: Boolean(colorResult?.mixed),
      opacity: Math.round(clamp(opacityResult?.value ?? 100, 0, 100)),
      opacityMixed: Boolean(opacityResult?.mixed)
    };
  }

  function syncAvisoColorEditor(prefix, draft, includeOpacity = false) {
    if (!draft) return;
    const rgb = hexToColor(draft.hex);
    const hex = normalizeHex(draft.hex);
    const opacity = Math.round(clamp(draft.opacity, 0, 100));
    const selectedRgb = `rgb(${rgb.r}, ${rgb.g}, ${rgb.b})`;
    const element = suffix => $("#" + prefix + suffix);

    const hexInput = element("Hex");
    hexInput.value = draft.colorMixed ? "" : hex.toUpperCase();
    hexInput.placeholder = draft.colorMixed ? "Mixed" : "#RRGGBB";
    element("Hue").value = Math.round(draft.h);
    element("Hue").style.setProperty("--hue-slider-value", String(Math.round(draft.h)));
    element("HueOutput").value = draft.colorMixed ? "" : String(Math.round(draft.h));
    element("Red").value = rgb.r;
    element("Green").value = rgb.g;
    element("Blue").value = rgb.b;
    element("RedOutput").value = draft.colorMixed ? "" : String(rgb.r);
    element("GreenOutput").value = draft.colorMixed ? "" : String(rgb.g);
    element("BlueOutput").value = draft.colorMixed ? "" : String(rgb.b);

    const configureChannel = (suffix, low, high, thumb = selectedRgb) => {
      const slider = element(suffix);
      slider.style.setProperty("--channel-low", low);
      slider.style.setProperty("--channel-high", high);
      slider.style.setProperty("--channel-thumb", thumb);
    };
    configureChannel("Red", `rgb(0, ${rgb.g}, ${rgb.b})`, `rgb(255, ${rgb.g}, ${rgb.b})`);
    configureChannel("Green", `rgb(${rgb.r}, 0, ${rgb.b})`, `rgb(${rgb.r}, 255, ${rgb.b})`);
    configureChannel("Blue", `rgb(${rgb.r}, ${rgb.g}, 0)`, `rgb(${rgb.r}, ${rgb.g}, 255)`);

    if (includeOpacity) {
      element("Opacity").value = opacity;
      element("OpacityOutput").value = draft.opacityMixed ? "" : String(opacity);
      configureChannel(
        "Opacity",
        `rgba(${rgb.r}, ${rgb.g}, ${rgb.b}, 0)`,
        selectedRgb,
        `rgba(${rgb.r}, ${rgb.g}, ${rgb.b}, ${opacity / 100})`
      );
    }

    const palette = element("SvPalette");
    palette.style.setProperty("--palette-hue", String(Math.round(draft.h)));
    palette.setAttribute("aria-valuenow", String(Math.round(draft.v * 100)));
    palette.setAttribute("aria-valuetext", `Saturation ${Math.round(draft.s * 100)}%, brightness ${Math.round(draft.v * 100)}%`);
    const cursor = element("PaletteCursor");
    cursor.style.left = `${draft.s * 100}%`;
    cursor.style.top = `${(1 - draft.v) * 100}%`;
    const editor = element("Editor");
    editor?.classList.toggle("mixed", draft.colorMixed || (includeOpacity && draft.opacityMixed));
    const swatch = element("Swatch");
    swatch.style.setProperty("--swatch-color", `rgba(${rgb.r}, ${rgb.g}, ${rgb.b}, ${includeOpacity ? opacity / 100 : 1})`);
    swatch.style.setProperty("--swatch-solid", hex);
  }

  function initializeAvisoColorControls(prefix, draft, includeOpacity = false) {
    ["Hex", "Hue", "HueOutput", "Red", "RedOutput", "Green", "GreenOutput", "Blue", "BlueOutput"]
      .forEach(suffix => resetControlFlags($("#" + prefix + suffix), draft.colorMixed));
    if (includeOpacity) {
      resetControlFlags($("#" + prefix + "Opacity"), draft.opacityMixed);
      resetControlFlags($("#" + prefix + "OpacityOutput"), draft.opacityMixed);
    }
    syncAvisoColorEditor(prefix, draft, includeOpacity);
  }

  function updateAvisoColorDraftFromRgb(draft, prefix, r, g, b, includeOpacity = false) {
    if (!draft) return;
    const rgb = { r: Math.round(clamp(r, 0, 255)), g: Math.round(clamp(g, 0, 255)), b: Math.round(clamp(b, 0, 255)) };
    const hsv = rgbToHsv(rgb.r, rgb.g, rgb.b);
    draft.hex = colorToHex(rgb);
    draft.h = hsv.h;
    draft.s = hsv.s;
    draft.v = hsv.v;
    draft.colorMixed = false;
    markControlTouched($("#" + prefix + "Hex"));
    syncAvisoColorEditor(prefix, draft, includeOpacity);
  }

  function updateAvisoColorDraftFromHex(draft, prefix, value, includeOpacity = false) {
    if (!draft) return;
    const normalized = normalizeHex(value, draft.hex || "#ffffff");
    const rgb = hexToColor(normalized);
    const hsv = rgbToHsv(rgb.r, rgb.g, rgb.b);
    draft.hex = normalized;
    draft.h = hsv.h;
    draft.s = hsv.s;
    draft.v = hsv.v;
    draft.colorMixed = false;
    markControlTouched($("#" + prefix + "Hex"));
    syncAvisoColorEditor(prefix, draft, includeOpacity);
  }

  function updateAvisoColorDraftFromHsv(draft, prefix, h, saturation, value, includeOpacity = false) {
    if (!draft) return;
    draft.h = ((Number(h) % 360) + 360) % 360;
    draft.s = clamp(saturation, 0, 1);
    draft.v = clamp(value, 0, 1);
    const rgb = hsvToRgb(draft.h, draft.s, draft.v);
    draft.hex = colorToHex(rgb);
    draft.colorMixed = false;
    markControlTouched($("#" + prefix + "Hex"));
    syncAvisoColorEditor(prefix, draft, includeOpacity);
  }

  function setColorDraftFromRgb(r, g, b) {
    if (!drafts.color) return;
    markEditorSectionUnapplied($("#colorSvPalette"));
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
    markEditorSectionUnapplied($("#colorHex"));
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
    markEditorSectionUnapplied($("#colorSvPalette"));
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
    $("#colorHue").value = Math.round(drafts.color.h);
    $("#colorHue").style.setProperty("--hue-slider-value", String(Math.round(drafts.color.h)));
    $("#colorHueOutput").value = String(Math.round(drafts.color.h));
    $("#colorRed").value = rgb.r;
    $("#colorGreen").value = rgb.g;
    $("#colorBlue").value = rgb.b;
    const selectedRgb = `rgb(${rgb.r}, ${rgb.g}, ${rgb.b})`;
    const configureChannelSlider = (selector, low, high, thumb = selectedRgb) => {
      const slider = $(selector);
      slider.style.setProperty("--channel-low", low);
      slider.style.setProperty("--channel-high", high);
      slider.style.setProperty("--channel-thumb", thumb);
    };
    configureChannelSlider("#colorRed", `rgb(0, ${rgb.g}, ${rgb.b})`, `rgb(255, ${rgb.g}, ${rgb.b})`);
    configureChannelSlider("#colorGreen", `rgb(${rgb.r}, 0, ${rgb.b})`, `rgb(${rgb.r}, 255, ${rgb.b})`);
    configureChannelSlider("#colorBlue", `rgb(${rgb.r}, ${rgb.g}, 0)`, `rgb(${rgb.r}, ${rgb.g}, 255)`);
    configureChannelSlider(
      "#colorOpacity",
      `rgba(${rgb.r}, ${rgb.g}, ${rgb.b}, 0)`,
      selectedRgb,
      `rgba(${rgb.r}, ${rgb.g}, ${rgb.b}, ${opacity / 100})`
    );
    $("#colorRedOutput").value = rgb.r;
    $("#colorGreenOutput").value = rgb.g;
    $("#colorBlueOutput").value = rgb.b;
    $("#colorOpacity").value = opacity;
    $("#colorOpacityOutput").value = String(opacity);

    const palette = $("#colorSvPalette");
    palette.style.setProperty("--palette-hue", String(Math.round(drafts.color.h)));
    palette.setAttribute("aria-valuenow", String(Math.round(drafts.color.v * 100)));
    palette.setAttribute(
      "aria-valuetext",
      `Saturation ${Math.round(drafts.color.s * 100)}%, brightness ${Math.round(drafts.color.v * 100)}%`
    );
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
    syncColorEditorControls();
  }
  function applyColorDraft({ render = true } = {}) {
    const entry = selectedColorEntry();
    if (!entry || !drafts.color) return;
    const hadAlpha = Object.prototype.hasOwnProperty.call(entry.color, "a");
    const next = hexToColor(drafts.color.hex, drafts.color.opacity / 100 * 255);
    if (!hadAlpha && Number(drafts.color.opacity) === 100) delete next.a;
    setAtPath(activeProfile(), entry.path, next);
    clearUnappliedEditorSection($("#colorHex"));
    markDirty(`${entry.name} updated`, ["profiles"]);
    if (render) renderColors();
  }

  function renderIconSymbolPreview() {
    const preview = $("#iconSymbolPreview");
    if (!preview) return;
    const style = String($("#targetIconStyle")?.value || activeProfile().targets?.icon_style || "realistic").toLowerCase();
    const symbolScale = clamp($("#targetSymbolScale")?.value ?? activeProfile().targets?.symbol_scale ?? 1, 0.5, 1.5);
    const trailEnabled = $("#targetTrailEnabled")?.checked ?? activeProfile().targets?.trail_enabled !== false;

    let symbol = "";
    let caption = "";
    let usesAircraftImage = false;
    if (style === "nova") {
      const shape = "M0-38-8-35-10-18-38 6-36 17-11 8-7 32 0 39 7 32 11 8 36 17 38 6 10-18 8-35Z";
      const afterglow = trailEnabled
        ? `<path class="nova-afterglow oldest" transform="translate(-15 8)" d="${shape}"/><path class="nova-afterglow middle" transform="translate(-10 5)" d="${shape}"/><path class="nova-afterglow newest" transform="translate(-5 2)" d="${shape}"/>`
        : "";
      symbol = `<svg class="icon-preview-vector nova" viewBox="-62 -52 124 108" aria-hidden="true">${afterglow}<path class="nova-primary-return" d="${shape}"/><path class="nova-secondary-return" d="M0-7 7 0 0 7-7 0Z"/></svg>`;
      caption = "NOVA";
    } else if (style === "triangle" || style === "arrow") {
      symbol = `<svg class="icon-preview-vector triangle" viewBox="-52 -52 104 104" aria-hidden="true"><path d="M0-42 38 36 0 13-38 36Z"/></svg>`;
      caption = "Triangle";
    } else if (style === "diamond") {
      symbol = `<svg class="icon-preview-vector diamond" viewBox="-48 -48 96 96" aria-hidden="true"><rect x="-12" y="-12" width="24" height="24" rx="5.3" transform="rotate(45)"/></svg>`;
      caption = "Diamond";
    } else {
      const aircraftHeight = 82;
      const aircraftWidth = aircraftHeight * (35.8 / 37.6);
      symbol = `<img class="icon-preview-aircraft" data-aircraft-icon alt="" width="${aircraftWidth.toFixed(2)}" height="${aircraftHeight.toFixed(2)}">`;
      caption = "Icon";
      usesAircraftImage = true;
    }

    const trailClass = style === "nova" ? "nova" : style === "realistic" ? "realistic" : "triangle";
    const trail = trailEnabled
      ? `<span class="icon-preview-trail ${trailClass}" aria-hidden="true"><i></i><i></i><i></i><i></i></span>`
      : "";
    preview.innerHTML = `<div class="icon-preview-stage"><span class="icon-preview-symbol" style="--icon-preview-scale:${symbolScale}">${symbol}</span>${trail}</div><span>${escapeHtml(caption)}</span>`;

    if (usesAircraftImage) {
      const aircraftImage = preview.querySelector("[data-aircraft-icon]");
      const sources = HOST_MODE
        ? ["https://icons.vsmr/a320.png"]
        : ["../../../data/aircraft_icons/a320.png"];
      let sourceIndex = 0;
      aircraftImage.addEventListener("error", () => {
        sourceIndex += 1;
        if (sourceIndex < sources.length) aircraftImage.src = sources[sourceIndex];
        else aircraftImage.classList.add("missing");
      });
      aircraftImage.src = sources[sourceIndex];
    }
  }

  function renderIcons() {
    const profile = activeProfile();
    const targets = profile.targets ||= {};
    ensureSelectValue($("#targetIconStyle"), targets.icon_style || "realistic");
    const symbolScale = clamp(targets.symbol_scale ?? 1, 0.5, 1.5);
    $("#targetSymbolScale").value = symbolScale;
    $("#targetSymbolScaleOutput").value = `${symbolScale.toFixed(2)}×`;
    targets.small_icon_boost_resolution_preset ||= state.settings.resolutionPreset || "1080p";
    $("#targetTrailEnabled").checked = targets.trail_enabled !== false;
    $("#targetTrailGroundPoints").value = clamp(targets.trail_ground_points ?? 4, 0, 16);
    $("#targetTrailGroundPointsOutput").value = String(Math.round(clamp(targets.trail_ground_points ?? 4, 0, 16)));
    $("#targetTrailAirbornePoints").value = clamp(targets.trail_airborne_points ?? 8, 0, 16);
    $("#targetTrailAirbornePointsOutput").value = String(Math.round(clamp(targets.trail_airborne_points ?? 8, 0, 16)));
    updateIconDependencies();
    renderIconSymbolPreview();
  }
  function updateIconDependencies() {
    const trailEnabled = $("#targetTrailEnabled").checked;
    $("#targetTrailGroundPoints").disabled = !trailEnabled;
    $("#targetTrailAirbornePoints").disabled = !trailEnabled;
    $$(".icon-trail-value").forEach(field => field.classList.toggle("is-disabled", !trailEnabled));
    renderIconSymbolPreview();
  }
  function applyIcons({ render = true } = {}) {
    const targets = activeProfile().targets ||= {};
    targets.icon_style = $("#targetIconStyle").value;
    targets.symbol_scale = clamp($("#targetSymbolScale").value, 0.5, 1.5);
    targets.small_icon_boost_resolution_preset = state.settings.resolutionPreset || targets.small_icon_boost_resolution_preset || "1080p";
    targets.trail_enabled = $("#targetTrailEnabled").checked;
    targets.trail_ground_points = Math.round(clamp($("#targetTrailGroundPoints").value, 0, 16));
    targets.trail_airborne_points = Math.round(clamp($("#targetTrailAirbornePoints").value, 0, 16));
    clearUnappliedEditorSection($("#targetIconStyle"));
    markDirty("Target icon settings updated", ["profiles"]);
    if (render) renderIcons();
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
      Object.entries(definition.status_definitions || {})
        .sort(([left], [right]) => {
          const leftIndex = TAG_STATUS_ORDER.indexOf(left);
          const rightIndex = TAG_STATUS_ORDER.indexOf(right);
          return (leftIndex < 0 ? Number.MAX_SAFE_INTEGER : leftIndex) - (rightIndex < 0 ? Number.MAX_SAFE_INTEGER : rightIndex) || left.localeCompare(right);
        })
        .forEach(([status, target]) => {
        result.push({
          id: `${scope}:${status}`, group: humanize(scope), label: TAG_STATUS_LABELS[status] || humanize(status),
          scope, status, target, color: tagDefinitionColor(profile, scope, status)
        });
        });
    });
    return result;
  }
  function tagSelectionIds(definitions = tagDefinitions()) {
    const valid = new Set(definitions.map(entry => entry.id));
    let ids = Array.isArray(state.ui.selectedTagIds)
      ? state.ui.selectedTagIds.filter(id => valid.has(id))
      : [];
    if (!ids.length && valid.has(state.ui.selectedTagId)) ids = [state.ui.selectedTagId];
    if (!ids.length && definitions[0]) ids = [definitions[0].id];
    ids = uniqueValues(ids);
    state.ui.selectedTagIds = ids;
    if (!ids.includes(state.ui.selectedTagId)) state.ui.selectedTagId = ids[ids.length - 1] || "";
    if (!valid.has(state.ui.tagSelectionAnchorId)) state.ui.tagSelectionAnchorId = state.ui.selectedTagId;
    return ids;
  }

  function selectedTagDefinitions(definitions = tagDefinitions()) {
    const selected = new Set(tagSelectionIds(definitions));
    return definitions.filter(entry => selected.has(entry.id));
  }

  function selectedTagDefinition(definitions = tagDefinitions()) {
    const selected = selectedTagDefinitions(definitions);
    return selected.find(entry => entry.id === state.ui.selectedTagId) || selected[selected.length - 1] || definitions[0];
  }

  function selectTagDefinition(tagId, event) {
    const definitions = tagDefinitions();
    const ordered = definitions.map(entry => entry.id);
    const current = tagSelectionIds(definitions);
    const next = updateMultiSelection(current, tagId, ordered, event, state.ui.tagSelectionAnchorId);
    state.ui.selectedTagIds = next;
    state.ui.selectedTagId = next.includes(tagId) ? tagId : next[next.length - 1];
    if (!event.shiftKey) state.ui.tagSelectionAnchorId = tagId;
    drafts.tag = null;
    clearUnappliedEditorSection($("#tagDefinitionEditor"));
    renderTags();
    setStatus(`${next.length} tag definition${next.length === 1 ? "" : "s"} selected`, "info");
  }

  function renderTags() {
    const definitions = tagDefinitions();
    const selectedIds = new Set(tagSelectionIds(definitions));
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
      const rows = items.map(entry => `<button type="button" role="option" aria-selected="${selectedIds.has(entry.id)}" class="menu-tree-row tag-menu-row ${selectedIds.has(entry.id) ? "active" : ""} ${entry.id === state.ui.selectedTagId ? "current" : ""}" data-tag-id="${escapeHtml(entry.id)}" title="${escapeHtml(entry.label)}">
        <span class="menu-row-title">${escapeHtml(entry.label)}</span>
      </button>`).join("");
      return `<section class="menu-tree-section tag-menu-section" style="--menu-accent:${accent}">
        <button type="button" class="menu-tree-caption" data-tree-toggle="tags" data-tree-key="${escapeHtml(groupKey)}" aria-expanded="${!collapsed}">
          <span class="menu-tree-caret" aria-hidden="true">${collapsed ? "▸" : "▾"}</span>
          <span class="menu-tree-caption-text">${escapeHtml(group)}</span>
        </button>
        <div class="menu-tree-box" ${collapsed ? "hidden" : ""}>${rows}</div>
      </section>`;
    }).join("") || `<div class="aviso-list-message">No tag definitions</div>`;
    requestAnimationFrame(() => $("#tagDefinitionList .tag-menu-row.active")?.scrollIntoView({ block: "nearest" }));
    renderTagEditor();
  }

  function renderTagEditor() {
    const definitions = tagDefinitions();
    const entries = selectedTagDefinitions(definitions);
    const entry = selectedTagDefinition(definitions);
    if (!entry) return;
    $("#tagEditorCaption").textContent = entries.length === 1 ? entry.label : `${entries.length} tag definitions`;
    const signature = entries.map(item => item.id).join("|");
    if (!drafts.tag || drafts.tag.signature !== signature || drafts.tag.id !== entry.id)
      drafts.tag = { id: entry.id, signature, data: clone(entry.target) };
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
    const labelSize = Math.round(clamp(activeProfile().font?.label_font_size ?? 1, 1, 5));
    $("#tagLabelFontSize").value = labelSize;

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
    drafts.tag = { id: entry.id, signature: tagSelectionIds().join("|"), data };
  }

  function applyTag({ render = true, applyContent = true } = {}) {
    const entries = selectedTagDefinitions();
    const entry = selectedTagDefinition();
    if (!entry || !entries.length) return;
    if (applyContent) {
      captureTagDraft();
      entries.forEach(targetEntry => {
        Object.keys(targetEntry.target).forEach(key => delete targetEntry.target[key]);
        Object.assign(targetEntry.target, clone(drafts.tag.data));
      });
    }

    const labels = activeProfile().labels ||= {};
    labels.rounded_corners = $("#tagRoundedCorners").checked;
    labels.auto_deconfliction = $("#tagAutoDeconfliction").checked;
    activeProfile().font ||= {};
    activeProfile().font.label_font_size = Math.round(clamp($("#tagLabelFontSize").value, 1, 5));
    activeProfile().font.font_name = $("#profileFontName").value || "Arial";
    activeProfile().font.weight = $("#profileFontWeight").value || "Regular";
    activeProfile().font.sizes ||= { one: 10, two: 11, three: 12, four: 13, five: 14 };

    clearUnappliedEditorSection($("#tagDefinitionEditor"));
    markDirty(`${entries.length === 1 ? entry.label : `${entries.length} tag definitions`} updated`, ["profiles"]);
    if (render) renderTags();
  }

  function rules() {
    activeProfile().rules ||= { version: 1, items: [] };
    activeProfile().rules.items ||= [];
    return activeProfile().rules.items;
  }

  function ruleLabel(rule, index) {
    return String(rule?.name || "").trim() || `Rule ${index + 1}`;
  }
  function normalizeRuleSourceUi(source) {
    const normalized = String(source || "").trim().toLowerCase();
    if (normalized === "runway" || normalized === "rwy") return "runway";
    if (["custom", "sid", "list", "sidlist"].includes(normalized)) return "custom";
    return "vacdm";
  }
  function ruleTokensForSource(source) {
    return RULE_SOURCE_TOKENS[normalizeRuleSourceUi(source)] || RULE_SOURCE_TOKENS.vacdm;
  }
  function ruleConditionsFor(source, token, selected = "") {
    const normalizedSource = normalizeRuleSourceUi(source);
    const normalizedToken = String(token || "").trim().toLowerCase();
    let values;
    if (normalizedSource === "runway")
      values = ["any", "set", "missing"];
    else if (normalizedSource === "custom")
      values = ["any", "set", "missing", "in: SID1X,SID2A", "not_in: SID1X,SID2A"];
    else if (normalizedToken === "tobt")
      values = ["any", "set", "missing", "inactive", "unconfirmed", "confirmed", "unconfirmed_delay", "confirmed_delay", "expired"];
    else if (normalizedToken === "tsat")
      values = ["any", "set", "missing", "inactive", "future", "valid", "expired", "future_ctot", "valid_ctot", "expired_ctot"];
    else
      values = ["any", "set", "missing", "future", "past"];

    if (normalizedSource !== "vacdm") {
      rules().forEach(rule => {
        const criteria = Array.isArray(rule.criteria) && rule.criteria.length ? rule.criteria : [rule];
        criteria.forEach(criterion => {
          if (normalizeRuleSourceUi(criterion.source) === normalizedSource &&
              String(criterion.token || "").trim().toLowerCase() === normalizedToken &&
              String(criterion.condition || "").trim()) {
            values.push(String(criterion.condition).trim());
          }
        });
      });
    }
    if (String(selected || "").trim()) values.push(String(selected).trim());
    return uniqueValues(values);
  }
  function ruleSelectOptions(values, selected, labels = null) {
    const desired = String(selected || "").toLowerCase();
    return values.map(value => `<option value="${escapeHtml(value)}" ${String(value).toLowerCase() === desired ? "selected" : ""}>${escapeHtml(labels?.[value] || value)}</option>`).join("");
  }
  function selectedRuleStatuses(rule) {
    const valid = new Set(RULE_STATUSES);
    const normalizeStatus = status => {
      const raw = String(status || "").trim().toLowerCase();
      const compact = raw.replace(/[\s_-]+/g, "");
      const tagType = String(rule?.tag_type || "").trim().toLowerCase();
      if (!compact || compact === "any" || compact === "all" || compact === "*") return "any";
      if (["default", "def", "nostatus", "nsts", "onground"].includes(compact)) return "default";
      if (["nofpl", "noflightplan"].includes(compact)) return "nofpl";
      if (compact === "push") return "push";
      if (compact === "stup" || compact === "startup") return "stup";
      if (compact === "taxi") return "taxi";
      if (compact === "lineup" || compact === "lnup" || compact === "l/up") return "lnup";
      if (compact === "depa" || compact === "departure") return "depa";
      if (["airdep", "airbornedep", "airbornedeparture"].includes(compact)) return "airdep";
      if (["airdeponrunway", "airbornedeponrunway", "airbornedepartureonrunway"].includes(compact)) return "airdep_onrunway";
      if (["airarr", "airbornearr", "airbornearrival"].includes(compact)) return "airarr";
      if (["airarronrunway", "airbornearronrunway", "airbornearrivalonrunway"].includes(compact)) return "airarr_onrunway";
      if (compact === "airborne") return tagType === "arrival" ? "airarr" : "airdep";
      if (compact === "onrunway") return tagType === "arrival" ? "airarr_onrunway" : "airdep_onrunway";
      if (compact === "arrival" || compact === "arrivals") return tagType === "arrival" ? "default" : "airarr";
      if (compact === "uncorrelated") return "default";
      return raw;
    };
    let statuses = Array.isArray(rule?.statuses) ? rule.statuses.map(normalizeStatus).filter(status => status !== "any" && valid.has(status)) : [];
    if (!statuses.length) {
      const legacy = String(rule?.status || "any").trim().toLowerCase();
      if (!legacy || legacy === "any") statuses = RULE_STATUSES.slice();
      else statuses = legacy.replace(/\bline[\s_-]*up\b/g, "lnup").split(/[\s,;|]+/).map(normalizeStatus).filter(status => status !== "any" && valid.has(status));
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
    else if (selected.length === 1) button.textContent = RULE_STATUS_LABELS[selected[0].dataset.ruleStatus] || humanize(selected[0].dataset.ruleStatus);
    else button.textContent = `${selected.length} statuses`;
    button.title = selected.length === options.length ? "All statuses selected" : selected.map(input => RULE_STATUS_LABELS[input.dataset.ruleStatus] || humanize(input.dataset.ruleStatus)).join(", ");
  }

  function renderRuleStatusSelector(rule, disabled = false) {
    const selected = new Set(selectedRuleStatuses(rule));
    $("#ruleStatusOptions").innerHTML = RULE_STATUSES.map(status => `<label role="option" aria-selected="${selected.has(status)}"><input type="checkbox" data-rule-status="${status}" ${selected.has(status) ? "checked" : ""}><span>${escapeHtml(RULE_STATUS_LABELS[status] || humanize(status))}</span></label>`).join("");
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
    const rows = items.map((rule, index) => `<button type="button" class="selection-row menu-tree-row manager-menu-row simple-manager-row ${index === state.ui.selectedRuleIndex ? "active" : ""}" data-rule-index="${index}"><span class="menu-row-title">${escapeHtml(ruleLabel(rule, index))}</span></button>`).join("");
    $("#ruleList").innerHTML = rows ? `<div class="menu-tree-box manager-menu-box">${rows}</div>` : `<div class="aviso-list-message">No rules</div>`;
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
        <select aria-label="Rule source" data-field="source">${ruleSelectOptions(RULE_SOURCES, normalizeRuleSourceUi(criterion.source), RULE_SOURCE_LABELS)}</select>
        <select aria-label="Rule token" data-field="token">${ruleSelectOptions(ruleTokensForSource(criterion.source), criterion.token)}</select>
        <select aria-label="Rule condition" data-field="condition">${ruleSelectOptions(ruleConditionsFor(criterion.source, criterion.token, criterion.condition), criterion.condition || "any")}</select>
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
  function applyRule({ render = true } = {}) {
    const item = rules()[state.ui.selectedRuleIndex];
    if (!item || !drafts.rule) return;
    const rule = captureRuleDraft();
    rules()[state.ui.selectedRuleIndex] = clone(rule);
    clearUnappliedEditorSection($("#ruleName"));
    markDirty("Rule updated", ["profiles"]);
    if (render) renderRules();
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
    const rows = items.map((mode, index) => `<button type="button" class="selection-row menu-tree-row manager-menu-row ${index === state.ui.selectedModeIndex ? "active" : ""}" data-mode-index="${index}"><span class="menu-row-title">${escapeHtml(mode.name || `Mode ${index + 1}`)}</span><span class="mode-active-mark">${mode.name === activeName ? "●" : ""}</span></button>`).join("");
    $("#modeList").innerHTML = rows ? `<div class="menu-tree-box manager-menu-box">${rows}</div>` : `<div class="aviso-list-message">No modes</div>`;
    renderModeEditor();
  }

  function renderModeEditor() {
    const mode = modes()[state.ui.selectedModeIndex];
    if (!mode) return;
    if (!drafts.mode || drafts.mode.index !== state.ui.selectedModeIndex) drafts.mode = { index: state.ui.selectedModeIndex, data: clone(mode) };
    const data = drafts.mode.data;
    $("#modePropertiesCaption").textContent = data.name || "Mode properties";
    $("#modeName").value = data.name || "";
    data.statuses ||= {};
    if (typeof data.statuses.lineup !== "boolean")
      data.statuses.lineup = typeof data.statuses.lnup === "boolean" ? data.statuses.lnup : (typeof data.statuses.taxi === "boolean" ? data.statuses.taxi : true);
    delete data.statuses.lnup;
    $("#reqSquawk").checked = Boolean(data.require_assigned_squawk);
    $("#modeAcceptPilotSquawk").checked = data.accept_pilot_squawk !== false;
    $("#reqClearance").checked = Boolean(data.require_clearance);
    $("#reqTsat").checked = Boolean(data.require_valid_tsat);
    $("#reqTobt").checked = Boolean(data.require_active_tobt);
    $("#modeTowerFilter").checked = Boolean(data.tower_filter ?? data.tower_mode);
    $("#modeStructuredRules").checked = data.structured_rules !== false && data.structured_rules_enabled !== false;
    $("#modeStatusGrid").innerHTML = MODE_STATUSES.map(status => `<label class="check-field"><input type="checkbox" data-mode-status="${status}" ${data.statuses[status] ? "checked" : ""}><span>${escapeHtml(humanize(status))}</span></label>`).join("");
    $("[data-action='activate-mode']").textContent = data.name === activeProfile().filters?.display_modes?.active ? "Active" : "Set active";
  }

  function setModeStatusVisibility(visible) {
    $$("[data-mode-status]").forEach(input => { input.checked = Boolean(visible); });
    applyMode({ render: false });
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

  function applyMode({ render = true } = {}) {
    const current = modes()[state.ui.selectedModeIndex];
    if (!current || !drafts.mode) return;
    const oldName = current.name;
    const next = clone(captureModeDraft());
    modes()[state.ui.selectedModeIndex] = next;
    if (activeProfile().filters.display_modes.active === oldName) activeProfile().filters.display_modes.active = next.name;
    clearUnappliedEditorSection($("#modeName"));
    markDirty("Display mode updated", ["profiles"]);
    if (render) renderModes();
    else {
      $("#modePropertiesCaption").textContent = next.name || "Mode properties";
      const rowLabel = $(`[data-mode-index="${state.ui.selectedModeIndex}"] span`);
      if (rowLabel) rowLabel.textContent = next.name || `Mode ${state.ui.selectedModeIndex + 1}`;
    }
    renderRuntimeMenu();
  }

  function renderProfilesManager() {
    if (!state.profiles.some(record => record.id === state.ui.managedProfileId)) state.ui.managedProfileId = state.activeProfileId;
    const rows = state.profiles.map(record => `<button type="button" class="selection-row menu-tree-row manager-menu-row ${record.id === state.ui.managedProfileId ? "active" : ""}" data-managed-profile-id="${escapeHtml(record.id)}"><span class="menu-row-title">${escapeHtml(record.data.name)}</span><span class="profile-active-mark">${record.id === state.activeProfileId ? "●" : ""}</span></button>`).join("");
    $("#profileList").innerHTML = rows ? `<div class="menu-tree-box manager-menu-box">${rows}</div>` : `<div class="aviso-list-message">No profiles</div>`;
    renderProfileEditor();
  }

  function renderProfileEditor() {
    const record = managedProfileRecord();
    if (!record) return;
    if (!drafts.profile || drafts.profile.id !== record.id) drafts.profile = { id: record.id, data: clone(record.data) };
    const profile = drafts.profile.data;
    $("#profilePropertiesCaption").textContent = profile.name || "Profile properties";
    $("#profileName").value = profile.name || "";
    $("[data-action='activate-profile']").textContent = record.id === state.activeProfileId ? "Active" : "Set active";
  }

  function captureProfileDraft() {
    if (!drafts.profile) return null;
    const profile = drafts.profile.data;
    profile.name = $("#profileName").value.trim() || "Profile";
    return profile;
  }

  function applyProfile({ render = true } = {}) {
    const record = managedProfileRecord();
    if (!record || !drafts.profile) return;
    const oldName = record.data.name;
    record.data = clone(captureProfileDraft());
    if (state.metadata.last_active_profile === oldName) state.metadata.last_active_profile = record.data.name;
    clearUnappliedEditorSection($("#profileName"));
    markDirty("Profile updated", ["profiles", "metadata"]);
    if (render) {
      renderProfilesManager();
      if (record.id === state.activeProfileId) renderAllProfileSections();
    } else {
      $("#profilePropertiesCaption").textContent = record.data.name || "Profile properties";
      const rowLabel = $(`[data-managed-profile-id="${CSS.escape(record.id)}"] span`);
      if (rowLabel) rowLabel.textContent = record.data.name || "Profile";
    }
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
  const AVISO_PALETTE_COLOR_KEYS = new Set(["fill", "stroke", "marker-color", "text-color", "text-halo-color"]);
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

  function activeAvisoColorPalette() {
    return state.settings.avisoColorPalette === "day" ? "day" : "night";
  }

  function avisoPaletteOverride(paint, palette = activeAvisoColorPalette()) {
    if (palette !== "day" || !paint || typeof paint !== "object") return null;
    const overrides = paint["palette-overrides"];
    const selected = overrides && typeof overrides === "object" ? overrides.day : null;
    return selected && typeof selected === "object" ? selected : null;
  }

  function effectiveAvisoPaintValue(sharedPaint, inlinePaint, key, fallback = undefined) {
    if (activeAvisoColorPalette() === "day" && AVISO_PALETTE_COLOR_KEYS.has(key)) {
      const inlineDay = avisoPaletteOverride(inlinePaint, "day");
      if (inlineDay?.[key] != null) return inlineDay[key];
      // Match the native renderer: an intentional feature-level Night color
      // remains authoritative until that feature receives its own Day value.
      if (inlinePaint?.[key] != null) return inlinePaint[key];
      const sharedDay = avisoPaletteOverride(sharedPaint, "day");
      if (sharedDay?.[key] != null) return sharedDay[key];
    }
    return inlinePaint?.[key] ?? sharedPaint?.[key] ?? fallback;
  }

  function applyAvisoPaintChanges(target, changes) {
    if (!target || typeof target !== "object") return;
    const dayColors = {};
    Object.entries(changes).forEach(([key, value]) => {
      if (activeAvisoColorPalette() === "day" && AVISO_PALETTE_COLOR_KEYS.has(key)) dayColors[key] = value;
      else target[key] = value;
    });
    if (!Object.keys(dayColors).length) return;
    if (!target["palette-overrides"] || typeof target["palette-overrides"] !== "object") target["palette-overrides"] = {};
    if (!target["palette-overrides"].day || typeof target["palette-overrides"].day !== "object") target["palette-overrides"].day = {};
    Object.assign(target["palette-overrides"].day, dayColors);
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
      const sharedPaint = style.paint && typeof style.paint === "object" ? style.paint : {};
      const paintKeys = objectType === "Label" ? AVISO_TEXT_PAINT_KEYS : AVISO_GEOMETRY_PAINT_KEYS;
      const paint = {};
      paintKeys.forEach(key => {
        const value = effectiveAvisoPaintValue(sharedPaint, properties, key);
        if (value != null) paint[key] = clone(value);
      });
      return {
        id: entry.id,
        name: style.name || properties.category || properties.name || entry.id,
        layer: style.layer || properties.layer || (objectType === "Label" ? "Labels" : "Other"),
        objectType,
        paint,
        indices: entry.indices,
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
    markEditorSectionUnapplied(element);
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

  function textStyleSelectionIds(entries = avisoStyleEntries("text")) {
    const valid = new Set(entries.map(entry => entry.id));
    let ids = Array.isArray(state.ui.selectedAvisoTextStyleIds)
      ? state.ui.selectedAvisoTextStyleIds.filter(id => valid.has(id))
      : [];
    if (!ids.length && valid.has(state.ui.selectedAvisoTextStyleId)) ids = [state.ui.selectedAvisoTextStyleId];
    if (!ids.length) {
      const fallback = preferredAvisoStyleId("text");
      if (fallback) ids = [fallback];
    }
    ids = uniqueValues(ids);
    state.ui.selectedAvisoTextStyleIds = ids;
    if (!ids.includes(state.ui.selectedAvisoTextStyleId)) state.ui.selectedAvisoTextStyleId = ids[ids.length - 1] || "";
    if (!valid.has(state.ui.avisoTextSelectionAnchorId)) state.ui.avisoTextSelectionAnchorId = state.ui.selectedAvisoTextStyleId;
    return ids;
  }

  function selectedAvisoTextEntries(entries = avisoStyleEntries("text")) {
    const selected = new Set(textStyleSelectionIds(entries));
    return entries.filter(entry => selected.has(entry.id));
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
    state.ui.selectedAvisoGeometryStyleId = geometryId;
    state.ui.selectedAvisoGeometryStyleIds = geometryId ? [geometryId] : [];
    state.ui.avisoGeometrySelectionAnchorId = geometryId;
    state.ui.selectedAvisoTextStyleId = textId;
    state.ui.selectedAvisoTextStyleIds = textId ? [textId] : [];
    state.ui.avisoTextSelectionAnchorId = textId;
    state.ui.selectedAvisoGroupId = avisoGroups()[0]?.id || "";
    state.ui.avisoGroupMemberSearch = "";
    state.ui.avisoGroupMemberFilter = "all";
    drafts.avisoGeometry = null;
    drafts.avisoTextStyle = null;
    drafts.avisoGroup = null;
    avisoGroupContentDraft = null;
  }

  function avisoPaintColor(entry) {
    return normalizeHex(entry.paint[entry.isText ? "text-color" : (entry.objectType === "Area" ? "fill" : "stroke")], "#6d7a7f");
  }

  function avisoStyleVisibility(entry) {
    const values = entry.indices.map(index => avisoFeatures()[index]?.properties?.visible !== false);
    const visibleCount = values.filter(Boolean).length;
    return {
      allVisible: visibleCount === values.length,
      mixed: visibleCount > 0 && visibleCount < values.length
    };
  }

  function avisoStyleVisibilityControl(entry, kind) {
    const visibility = avisoStyleVisibility(entry);
    const action = visibility.allVisible ? "Hide" : "Show";
    return `<span class="aviso-style-eye ${visibility.allVisible ? "is-on" : "is-off"} ${visibility.mixed ? "is-mixed" : ""}" data-aviso-style-visibility="${kind}" data-aviso-style-id="${escapeHtml(entry.id)}" role="button" aria-label="${action} ${escapeHtml(entry.name)}" title="${action} ${escapeHtml(entry.name)}">
      <svg aria-hidden="true" viewBox="0 0 24 24"><path d="M2.5 12s3.6-6 9.5-6 9.5 6 9.5 6-3.6 6-9.5 6-9.5-6-9.5-6z"></path><circle cx="12" cy="12" r="2.6"></circle>${visibility.allVisible || visibility.mixed ? "" : '<path class="aviso-eye-slash" d="M4 4l16 16"></path>'}</svg>
    </span>`;
  }

  function toggleAvisoStyleVisibility(kind, styleId) {
    const entry = avisoStyleEntries(kind).find(item => item.id === styleId);
    if (!entry) return;
    const visible = !avisoStyleVisibility(entry).allVisible;
    entry.indices.forEach(index => {
      const properties = avisoFeatures()[index]?.properties;
      if (properties) properties.visible = visible;
    });
    markDirty(`${entry.name} ${visible ? "shown" : "hidden"}`, ["aviso"]);
    if (kind === "geometry") renderAvisoGeometry();
    else renderAvisoText();
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

  function avisoGroupMemberRows(groupId, groupIndex = buildAvisoGroupIndex()) {
    const memberSet = new Set(avisoGroupMemberIndices(groupId, groupIndex));
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
        subtitle: properties.category || properties.style_id || "Text"
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
        subtitle: entry.layer
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

  function renderAvisoGroups() {
    const groups = avisoGroups();
    const groupIndex = buildAvisoGroupIndex();
    const selected = selectedAvisoGroup();

    $("#avisoGroupList").innerHTML = groups.length ? `<div class="aviso-group-box menu-tree-box manager-menu-box">${groups.map(group => {
      const active = group.id === selected?.id;
      return `<div class="aviso-group-row menu-tree-row manager-menu-row simple-manager-row ${active ? "active" : ""}" role="option" aria-selected="${active}" data-aviso-group-id="${escapeHtml(group.id)}" draggable="true" title="Drag to reorder">
        <span class="aviso-group-row-copy menu-row-title"><strong>${escapeHtml(group.name)}</strong></span>
      </div>`;
    }).join("")}</div>` : `<div class="aviso-list-message">No groups yet</div>`;

    renderAvisoGroupEditor(groupIndex);
  }

  function renderAvisoGroupEditor(groupIndex = buildAvisoGroupIndex()) {
    const group = selectedAvisoGroup();
    const editor = $(".aviso-group-editor");
    const nameControl = $("#avisoGroupName");
    nameControl?.setCustomValidity("");
    if (!group) {
      $("#avisoGroupCaption").textContent = "No group selected";
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
    const counts = avisoGroupCounts(group.id, groupIndex);

    $("#avisoGroupCaption").textContent = draft.name || group.name;
    $("#avisoGroupName").value = draft.name || "";
    $("#avisoGroupId").value = group.id;

    $("#avisoGroupMemberSearch").value = state.ui.avisoGroupMemberSearch;
    const search = String(state.ui.avisoGroupMemberSearch || "").trim().toLowerCase();
    const rows = avisoGroupMemberRows(group.id, groupIndex).filter(row =>
      !search || `${row.name} ${row.subtitle} ${row.id}`.toLowerCase().includes(search)
    );
    $("#avisoGroupMemberList").innerHTML = rows.length ? rows.map(row => {
      return `<div class="aviso-group-member-row">
        <strong class="aviso-member-name">${escapeHtml(row.name)}</strong>
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
    clearUnappliedEditorSection($("#avisoGroupName"));
    markDirty("AVISO group created", ["aviso"]);
    renderAvisoGroups();
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
    clearUnappliedEditorSection($("#avisoGroupName"));
    markDirty("AVISO group copied", ["aviso"]);
    renderAvisoGroups();
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
    clearUnappliedEditorSection($("#avisoGroupName"));
    markDirty("AVISO group deleted", ["aviso"]);
    renderAvisoGroups();
    renderRuntimeMenu();
  }

  function applyAvisoGroup({ render = true, feedback = true } = {}) {
    const group = selectedAvisoGroup();
    if (!group) return;
    const draft = captureAvisoGroupDraft();
    const name = String(draft?.data?.name || "").trim();
    if (!name) {
      $("#avisoGroupName")?.setCustomValidity("Enter a group name");
      if (feedback) showToast("Enter a group name", "error");
      return false;
    }
    $("#avisoGroupName")?.setCustomValidity("");
    group.name = name;
    group.visible = draft.data.visible !== false;
    group.accent = normalizeHex(draft.data.accent, group.accent || "#84b7d5");
    clearUnappliedEditorSection($("#avisoGroupName"));
    markDirty("AVISO group updated", ["aviso"]);
    if (render) renderAviso();
    else $("#avisoGroupCaption").textContent = group.name;
    renderRuntimeMenu();
    return true;
  }

  function revertAvisoGroup() {
    const group = selectedAvisoGroup();
    if (!group) return;
    drafts.avisoGroup = { id: group.id, original: clone(group), data: clone(group) };
    clearUnappliedEditorSection($("#avisoGroupName"));
    renderAvisoGroupEditor();
  }

  function clearSelectedAvisoGroup() {
    const group = selectedAvisoGroup();
    if (!group || !avisoGroupMemberIndices(group.id).length) return;
    if (!confirmDelete(`Remove all content from “${group.name}”?`)) return;
    avisoFeatures().forEach(feature => setFeatureGroupMembership(feature, group.id, false));
    markDirty("AVISO group cleared", ["aviso"]);
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
    markDirty("AVISO group contents updated", ["aviso"]);
    renderAvisoGroups();
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
          subtitle: properties.category || properties.style_id || "Text"
        };
      }).filter(item => !search || `${item.name} ${item.subtitle}`.toLowerCase().includes(search));
    }
    const objectType = type === "line" ? "Line" : "Area";
    return avisoStyleEntries("geometry").filter(entry => entry.objectType === objectType).map(entry => ({
      key: `style:${entry.id}`,
      indices: entry.indices.slice(),
      kind: type,
      name: entry.name,
      subtitle: entry.layer
    })).filter(item => !search || `${item.name} ${item.subtitle} ${item.key}`.toLowerCase().includes(search));
  }

  function openAvisoGroupContentDialog() {
    const group = selectedAvisoGroup();
    if (!group) return;
    avisoGroupContentDraft = { groupId: group.id, members: new Set(avisoGroupMemberIndices(group.id)) };
    avisoGroupContentDirty = false;
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
    $("#avisoGroupContentList").innerHTML = candidates.length ? `<div class="group-content-box">${candidates.map(item => {
      const selectedCount = item.indices.filter(index => avisoGroupContentDraft.members.has(index)).length;
      const selected = selectedCount === item.indices.length;
      const partial = selectedCount > 0 && !selected;
      return `<button type="button" class="group-content-row ${selected ? "selected" : ""} ${partial ? "partial" : ""}" data-group-content-key="${escapeHtml(item.key)}"><span class="group-content-check">${selected ? "✓" : partial ? "−" : ""}</span><strong class="group-content-name">${escapeHtml(item.name)}</strong></button>`;
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
    stageAvisoGroupContent();
    renderAvisoGroupContentDialog();
  }

  function setFilteredAvisoGroupContent(selected) {
    if (!avisoGroupContentDraft) return;
    groupContentCandidates().forEach(item => item.indices.forEach(index => {
      if (selected) avisoGroupContentDraft.members.add(index);
      else avisoGroupContentDraft.members.delete(index);
    }));
    stageAvisoGroupContent();
    renderAvisoGroupContentDialog();
  }

  function stageAvisoGroupContent() {
    const group = avisoGroups().find(item => item.id === avisoGroupContentDraft?.groupId);
    if (!group || !avisoGroupContentDraft) return false;
    avisoFeatures().forEach((feature, index) => setFeatureGroupMembership(feature, group.id, avisoGroupContentDraft.members.has(index)));
    avisoGroupContentDirty = false;
    markDirty("AVISO group contents updated", ["aviso"]);
    renderRuntimeMenu();
    return true;
  }

  function renderAviso() {
    const geometryEntries = avisoStyleEntries("geometry");
    const textEntries = avisoStyleEntries("text");

    geometrySelectionIds(geometryEntries);
    textStyleSelectionIds(textEntries);

    const avisoColorPalette = activeAvisoColorPalette();
    $$('[data-aviso-color-palette]').forEach(button => {
      const active = button.dataset.avisoColorPalette === avisoColorPalette;
      button.classList.toggle("active", active);
      button.setAttribute("aria-pressed", active ? "true" : "false");
    });
    $$('[data-aviso-view]').forEach(button => button.classList.toggle("active", button.dataset.avisoView === state.ui.avisoView));
    $$('[data-aviso-view-panel]').forEach(panel => panel.classList.toggle("active", panel.dataset.avisoViewPanel === state.ui.avisoView));

    if (state.ui.avisoView === "geometry") renderAvisoGeometry();
    else renderAvisoText();
  }

  function renderAvisoGeometry() {
    const allEntries = avisoStyleEntries("geometry");
    const selectedIds = new Set(geometrySelectionIds(allEntries));
    avisoGeometryRenderOrder = allEntries.map(entry => entry.id);

    const grouped = new Map();
    allEntries.forEach(entry => {
      if (!grouped.has(entry.layer)) grouped.set(entry.layer, []);
      grouped.get(entry.layer).push(entry);
    });

    $("#avisoGeometryStyleList").innerHTML = Array.from(grouped.entries()).map(([layer, entries]) => `
      <section class="aviso-style-section menu-tree-section">
        <div class="aviso-style-section-title"><span>${escapeHtml(layer)}</span></div>
        <div class="aviso-style-box menu-tree-box">
          ${entries.map(entry => {
            const selected = selectedIds.has(entry.id);
            const current = entry.id === state.ui.selectedAvisoGeometryStyleId;
            return `<button type="button" role="option" aria-selected="${selected}" class="aviso-style-row menu-tree-row geometry-select-row ${selected ? "active" : ""} ${current ? "current" : ""}" data-aviso-geometry-style="${escapeHtml(entry.id)}">
              ${avisoStyleVisibilityControl(entry, "geometry")}
              <span class="aviso-style-swatch" style="--aviso-swatch:${avisoPaintColor(entry)}"></span>
              <span class="aviso-style-copy"><strong>${escapeHtml(entry.name)}</strong></span>
            </button>`;
          }).join("")}
        </div>
      </section>`).join("") || `<div class="aviso-list-message">No geometry styles</div>`;

    renderAvisoGeometryEditor(allEntries);
  }

  function renderAvisoGeometryEditor(allEntries = avisoStyleEntries("geometry")) {
    const entries = selectedAvisoGeometryEntries(allEntries);
    if (!entries.length) {
      $("#avisoGeometryCaption").textContent = "No geometry selected";
      return;
    }

    const types = uniqueValues(entries.map(entry => entry.objectType));
    $("#avisoGeometryCaption").textContent = entries.length === 1 ? entries[0].name : `${entries.length} geometry styles`;
    const paletteLabel = activeAvisoColorPalette() === "day" ? "Day" : "Night";
    const colorKind = types.length > 1 ? "Primary" : types[0] === "Line" ? "Line" : "Fill";
    $("#avisoGeometryColorLabel").textContent = `${colorKind} color · ${paletteLabel}`;

    const colorResult = commonValue(
      entries.map(entry => entry.objectType === "Line" ? (entry.paint.stroke || entry.paint.fill || "#000000") : (entry.paint.fill || entry.paint.stroke || "#000000")),
      value => normalizeHex(value, "#000000")
    );
    const opacityResult = commonValue(
      entries.map(entry => Number(entry.paint[entry.objectType === "Line" ? "stroke-opacity" : "fill-opacity"] ?? 1) * 100),
      value => Math.round(Number(value) * 100) / 100
    );
    const signature = `${activeAvisoColorPalette()}|${entries.map(entry => entry.id).join("|")}`;
    if (!drafts.avisoGeometry || drafts.avisoGeometry.signature !== signature) {
      drafts.avisoGeometry = createAvisoColorDraft(signature, colorResult, "#000000", opacityResult);
      initializeAvisoColorControls("avisoGeometryColor", drafts.avisoGeometry, true);
    } else {
      syncAvisoColorEditor("avisoGeometryColor", drafts.avisoGeometry, true);
    }
  }

  function selectAvisoGeometryStyle(styleId, event, forceToggle = false) {
    const current = geometrySelectionIds();
    const next = updateMultiSelection(current, styleId, avisoGeometryRenderOrder, event, state.ui.avisoGeometrySelectionAnchorId, forceToggle);
    state.ui.selectedAvisoGeometryStyleIds = next;
    state.ui.selectedAvisoGeometryStyleId = next.includes(styleId) ? styleId : next[next.length - 1];
    if (!event.shiftKey) state.ui.avisoGeometrySelectionAnchorId = styleId;
    drafts.avisoGeometry = null;
    clearUnappliedEditorSection($("#avisoGeometryColorHex"));
    renderAvisoGeometry();
    setStatus(`${next.length} geometry style${next.length === 1 ? "" : "s"} selected`, "info");
  }

  function applyAvisoGeometry({ render = true, feedback = true } = {}) {
    const entries = selectedAvisoGeometryEntries();
    if (!entries.length) return;
    const colorTouched = wasControlTouched("#avisoGeometryColorHex");
    const opacityTouched = wasControlTouched("#avisoGeometryColorOpacity") || wasControlTouched("#avisoGeometryColorOpacityOutput");

    if (!colorTouched && !opacityTouched) {
      if (feedback) showToast("Change the geometry color or opacity");
      if (feedback) return false;
      clearUnappliedEditorSection($("#avisoGeometryColorHex"));
      return true;
    }

    let updatedCount = 0;
    entries.forEach(entry => {
      const colorKey = entry.objectType === "Line" ? "stroke" : "fill";
      const opacityKey = entry.objectType === "Line" ? "stroke-opacity" : "fill-opacity";
      const changes = {};
      if (colorTouched && drafts.avisoGeometry)
        changes[colorKey] = normalizeHex(drafts.avisoGeometry.hex, entry.paint[colorKey] || "#000000").toUpperCase();
      if (opacityTouched && drafts.avisoGeometry)
        changes[opacityKey] = clamp(Number(drafts.avisoGeometry.opacity) / 100, 0, 1);
      const style = ensureAvisoCatalogStyle(entry);
      applyAvisoPaintChanges(style.paint, changes);
      entry.indices.forEach(index => {
        const properties = avisoFeatures()[index]?.properties;
        if (!properties) return;
        properties.style_id ||= entry.id;
        applyAvisoPaintChanges(properties, changes);
        updatedCount += 1;
      });
    });

    resetControlFlags($("#avisoGeometryColorHex"));
    resetControlFlags($("#avisoGeometryColorOpacity"));
    resetControlFlags($("#avisoGeometryColorOpacityOutput"));
    clearUnappliedEditorSection($("#avisoGeometryColorHex"));
    markDirty(`${entries.length} geometry style${entries.length === 1 ? "" : "s"} updated`, ["aviso"]);
    if (feedback) showToast(`Updated ${updatedCount.toLocaleString()} geometry objects`, "success");
    if (render) {
      drafts.avisoGeometry = null;
      renderAvisoGeometry();
    }
    return true;
  }

  function effectiveAvisoTextValue(index, entry, key) {
    const properties = avisoFeatures()[index]?.properties || {};
    const sharedPaint = state.aviso?.styles?.[entry.id]?.paint || {};
    return effectiveAvisoPaintValue(sharedPaint, properties, key, AVISO_TEXT_DEFAULTS[key]);
  }

  function renderAvisoText() {
    const allEntries = avisoStyleEntries("text");
    const selectedIds = new Set(textStyleSelectionIds(allEntries));
    avisoTextRenderOrder = allEntries.map(entry => entry.id);

    const grouped = new Map();
    allEntries.forEach(entry => {
      if (!grouped.has(entry.layer)) grouped.set(entry.layer, []);
      grouped.get(entry.layer).push(entry);
    });

    $("#avisoTextStyleList").innerHTML = Array.from(grouped.entries()).map(([layer, entries]) => `
      <section class="aviso-style-section menu-tree-section">
        <div class="aviso-style-section-title"><span>${escapeHtml(layer)}</span></div>
        <div class="aviso-style-box menu-tree-box">
          ${entries.map(entry => {
            const selected = selectedIds.has(entry.id);
            const current = entry.id === state.ui.selectedAvisoTextStyleId;
            return `<button type="button" role="option" aria-selected="${selected}" title="${escapeHtml(entry.name)}" class="aviso-style-row menu-tree-row aviso-text-style-row geometry-select-row ${selected ? "active" : ""} ${current ? "current" : ""}" data-aviso-text-style="${escapeHtml(entry.id)}">
              ${avisoStyleVisibilityControl(entry, "text")}
              <span class="aviso-style-swatch" style="--aviso-swatch:${avisoPaintColor(entry)}"></span>
              <span class="aviso-style-copy"><strong>${escapeHtml(entry.name)}</strong></span>
            </button>`;
          }).join("")}
        </div>
      </section>`).join("") || `<div class="aviso-list-message">No text styles</div>`;

    renderAvisoTextEditor(allEntries);
  }

  function renderAvisoTextEditor(allEntries = avisoStyleEntries("text")) {
    const entries = selectedAvisoTextEntries(allEntries);
    if (!entries.length) {
      $("#avisoTextCaption").textContent = "No text style selected";
      return;
    }

    const items = entries.flatMap(entry => entry.indices.map(index => ({ index, entry })));
    const values = key => items.map(item => effectiveAvisoTextValue(item.index, item.entry, key));

    $("#avisoTextCaption").textContent = entries.length === 1 ? entries[0].name : `${entries.length} text styles`;
    const paletteLabel = activeAvisoColorPalette() === "day" ? "Day" : "Night";
    const colorTarget = state.ui.avisoTextColorTarget === "halo" ? "halo" : "text";
    const colorKey = colorTarget === "halo" ? "text-halo-color" : "text-color";
    const colorFallback = colorTarget === "halo" ? "#000000" : "#808080";
    $("#avisoTextColorLabel").textContent = `${colorTarget === "halo" ? "Halo" : "Text"} color · ${paletteLabel}`;
    $$("[data-color-target]", $("#avisoTextColorEditor")).forEach(button => button.classList.toggle("active", button.dataset.colorTarget === colorTarget));

    setCommonInput("#avisoTextFont", commonValue(values("text-font"), value => String(value || "Arial")), value => String(value || "Arial"));
    setCommonInput("#avisoTextSize", commonValue(values("text-size"), value => Number(value)), value => String(value));
    setCommonInput("#avisoTextHaloWidth", commonValue(values("text-halo-width"), value => Number(value)), value => String(value));
    const colorResult = commonValue(values(colorKey), value => normalizeHex(value, colorFallback));
    const signature = `${activeAvisoColorPalette()}|${colorTarget}|${entries.map(entry => entry.id).join("|")}`;
    if (!drafts.avisoTextStyle || drafts.avisoTextStyle.signature !== signature) {
      drafts.avisoTextStyle = createAvisoColorDraft(signature, colorResult, colorFallback);
      initializeAvisoColorControls("avisoTextColor", drafts.avisoTextStyle);
    } else {
      syncAvisoColorEditor("avisoTextColor", drafts.avisoTextStyle);
    }
    const zoomCommon = commonValue(values("zoomLevel"), value => Math.round(clamp(value ?? 6, 0, 14)));
    setCommonInput("#avisoTextZoomLevel", zoomCommon, value => String(value));
    const zoomSlider = $("#avisoTextZoomSlider");
    resetControlFlags(zoomSlider, zoomCommon.mixed);
    zoomSlider.value = String(Math.round(clamp(zoomCommon.value ?? 6, 0, 14)));
    $("#avisoTextZoomMeaning").textContent = zoomCommon.mixed
      ? "Mixed zoom levels"
      : (MAP_ZOOM_LABELS[Math.round(clamp(zoomCommon.value ?? 6, 0, 14))] || "Zoom visibility");

  }

  function selectAvisoTextStyle(styleId, event, forceToggle = false) {
    const current = textStyleSelectionIds();
    const next = updateMultiSelection(current, styleId, avisoTextRenderOrder, event, state.ui.avisoTextSelectionAnchorId, forceToggle);
    state.ui.selectedAvisoTextStyleIds = next;
    state.ui.selectedAvisoTextStyleId = next.includes(styleId) ? styleId : next[next.length - 1];
    if (!event.shiftKey) state.ui.avisoTextSelectionAnchorId = styleId;
    drafts.avisoTextStyle = null;
    clearUnappliedEditorSection($("#avisoTextFont"));
    renderAvisoText();
    setStatus(`${next.length} text style${next.length === 1 ? "" : "s"} selected`, "info");
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
    add("text-size", "#avisoTextSize", value => clamp(Number(value), 6, 32));
    if ((!onlyTouched || wasControlTouched("#avisoTextColorHex")) && drafts.avisoTextStyle) {
      const colorKey = state.ui.avisoTextColorTarget === "halo" ? "text-halo-color" : "text-color";
      const fallback = colorKey === "text-halo-color" ? "#000000" : "#808080";
      paint[colorKey] = normalizeHex(drafts.avisoTextStyle.hex, entry.paint[colorKey] || fallback).toUpperCase();
    }
    add("text-halo-width", "#avisoTextHaloWidth", value => clamp(Number(value), 0, 6));
    add("zoomLevel", "#avisoTextZoomLevel", value => Math.round(clamp(value, 0, 14)));
    return paint;
  }

  function applyAvisoTextStyles({ render = true, feedback = true } = {}) {
    const targets = selectedAvisoTextEntries();
    if (!targets.length) return;
    // Only propagate controls touched by the current gesture. Effective values
    // from one text group must not flatten other selected groups implicitly.
    const textPaint = buildAvisoTextPaint(targets[0], true);
    if (!Object.keys(textPaint).length) {
      if (feedback) showToast("Change at least one shared text property");
      if (feedback) return false;
      clearUnappliedEditorSection($("#avisoTextFont"));
      return true;
    }

    let updatedCount = 0;
    targets.forEach(entry => {
      const style = ensureAvisoCatalogStyle(entry);
      applyAvisoPaintChanges(style.paint, textPaint);
      entry.indices.forEach(index => {
        const properties = avisoFeatures()[index]?.properties;
        if (!properties) return;
        properties.style_id ||= entry.id;
        applyAvisoPaintChanges(properties, textPaint);
        updatedCount += 1;
      });
    });

    resetControlFlags($("#avisoTextColorHex"));
    clearUnappliedEditorSection($("#avisoTextFont"));
    markDirty(`${targets.length} text style${targets.length === 1 ? "" : "s"} updated`, ["aviso"]);
    if (feedback) showToast(`Updated ${updatedCount.toLocaleString()} text labels`, "success");
    if (render) {
      drafts.avisoTextStyle = null;
      renderAvisoText();
    }
    return true;
  }

  function revertAvisoEditor() {
    if (state.ui.avisoView === "geometry") {
      drafts.avisoGeometry = null;
      renderAvisoGeometryEditor();
    } else {
      drafts.avisoTextStyle = null;
      renderAvisoTextEditor();
    }
    clearUnappliedEditorSection(state.ui.avisoView === "geometry" ? $("#avisoGeometryColorHex") : $("#avisoTextFont"));
  }


  function ensureProfileRimcas(profile = activeProfile()) {
    // Rendering an editor must not mutate the live profile. In particular, a
    // legacy profile with no `runways` member must keep that absence until the
    // user actually changes and stages the Alerts form.
    const source = profile?.rimcas;
    const rimcas = source && typeof source === "object" && !Array.isArray(source)
      ? clone(source)
      : {};
    if (!Array.isArray(rimcas.timer) || rimcas.timer.length !== 5) rimcas.timer = [60, 45, 30, 15, 0];
    if (!Array.isArray(rimcas.timer_lvp) || rimcas.timer_lvp.length !== 5) rimcas.timer_lvp = [120, 90, 60, 30, 0];
    rimcas.stage_two_speed_threshold_kt = Number.isFinite(Number(rimcas.stage_two_speed_threshold_kt)) ? Number(rimcas.stage_two_speed_threshold_kt) : 25;
    delete rimcas.enabled;
    delete rimcas.rimcas_label_only;
    delete rimcas.use_red_symbol_for_emergencies;
    if (!Array.isArray(rimcas.inactive_alerts)) rimcas.inactive_alerts = [];
    if (!Array.isArray(rimcas.runways)) rimcas.runways = [];
    ALERT_COLOR_DEFAULTS.forEach(([key, fallback]) => { if (!isColorObject(rimcas[key])) rimcas[key] = hexToColor(fallback); });
    return rimcas;
  }

  function ensureAlertsDraft() {
    const profileId = state.activeProfileId;
    if (!drafts.alerts || drafts.alerts.profileId !== profileId) {
      const profile = activeProfile();
      const hasConfiguredRunways = Boolean(
        profile?.rimcas &&
        Object.prototype.hasOwnProperty.call(profile.rimcas, "runways") &&
        Array.isArray(profile.rimcas.runways) &&
        profile.rimcas.runways.length
      );
      const rimcas = ensureProfileRimcas();
      const runtimeAlerts = state.runtime.alerts ||= { visibility: "normal", runways: clone(DEFAULT_ALERT_RUNWAYS) };
      if (!Array.isArray(runtimeAlerts.runways)) runtimeAlerts.runways = clone(DEFAULT_ALERT_RUNWAYS);
      // Empty arrays were written by older editors even when runway monitoring
      // was meant to follow EuroScope. Only actual rows override the runtime list.
      const profileRunways = hasConfiguredRunways ? rimcas.runways : runtimeAlerts.runways;
      const profileVisibility = ["normal", "lvp"].includes(rimcas.visibility)
        ? rimcas.visibility
        : runtimeAlerts.visibility;
      drafts.alerts = {
        profileId,
        data: {
          visibility: profileVisibility === "lvp" ? "lvp" : "normal",
          runways: clone(profileRunways),
          rimcas
        }
      };
    }
    return drafts.alerts.data;
  }

  function renderAlerts() {
    const data = ensureAlertsDraft();
    const inactive = new Set((data.rimcas.inactive_alerts || []).map(String));
    $("#alertTypeGrid").innerHTML = ALERT_TYPES.map(alert => `<label class="alert-toggle-card ${inactive.has(alert) ? "inactive" : ""}"><input type="checkbox" data-alert-type="${escapeHtml(alert)}" ${inactive.has(alert) ? "" : "checked"}><strong>${escapeHtml(alert)}</strong></label>`).join("");

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
  }

  function renderAlertTimerRow(selector, values) {
    const labels = ["Stage 1", "Stage 2", "Stage 3", "Stage 4", "Alert"];
    $(selector).innerHTML = labels.map((label, index) => `<label class="field"><span>${label}</span><input data-alert-timer-index="${index}" min="0" step="1" type="number" value="${Number(values[index] ?? 0)}"></label>`).join("");
  }

  function captureAlertsDraft() {
    if (!drafts.alerts || drafts.alerts.profileId !== state.activeProfileId) return ensureAlertsDraft();
    const data = drafts.alerts.data;
    delete data.rimcas.enabled;
    delete data.rimcas.rimcas_label_only;
    delete data.rimcas.use_red_symbol_for_emergencies;
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
    data.rimcas.visibility = data.visibility;
    data.rimcas.runways = clone(data.runways);
    return data;
  }

  function applyAlerts({ render = true, feedback = true } = {}) {
    const data = captureAlertsDraft();
    data.rimcas.visibility = data.visibility;
    data.rimcas.runways = clone(data.runways);
    activeProfile().rimcas = clone(data.rimcas);
    state.runtime.alerts = { visibility: data.visibility, runways: clone(data.runways) };
    clearUnappliedEditorSectionsWithin($("[data-page-panel='alerts']"));
    markDirty("Alert settings updated", ["profiles"]);
    if (render) renderAlerts();
    if (feedback) showToast("Alert settings updated", "success");
  }

  function revertAlerts() {
    drafts.alerts = null;
    clearUnappliedEditorSectionsWithin($("[data-page-panel='alerts']"));
    renderAlerts();
  }

  function setAllAlertRunwayField(field, value = true) {
    captureAlertsDraft();
    ensureAlertsDraft().runways.forEach(runway => { runway[field] = value; });
    renderAlerts();
    applyAlerts({ render: false, feedback: false });
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
    applyAlerts({ render: false, feedback: false });
  }

  function removeAlertRunway(index) {
    captureAlertsDraft();
    ensureAlertsDraft().runways.splice(index, 1);
    renderAlerts();
    applyAlerts({ render: false, feedback: false });
  }

  function datalinkFormFromRuntime(runtime = state.datalink) {
    return {
      logonCallsign: String(runtime?.logonCallsign || "").trim().toUpperCase().slice(0, 8),
      password: "",
      replacePassword: false,
      playSound: Boolean(runtime?.playSound),
      cdmAutoEnabled: Boolean(runtime?.cdmAutoEnabled),
      cdmDelayMinutes: Math.round(clamp(runtime?.cdmDelayMinutes ?? 5, 0, 1440)),
      cdmCooldownMinutes: Math.round(clamp(runtime?.cdmCooldownMinutes ?? 60, 0, 1440))
    };
  }

  function normalizeDatalinkForm(form = {}) {
    const password = String(form.password || "").trim();
    return {
      logonCallsign: String(form.logonCallsign || "").trim().toUpperCase().slice(0, 8),
      password,
      replacePassword: Boolean(password),
      playSound: Boolean(form.playSound),
      cdmAutoEnabled: Boolean(form.cdmAutoEnabled),
      cdmDelayMinutes: Math.round(clamp(form.cdmDelayMinutes, 0, 1440)),
      cdmCooldownMinutes: Math.round(clamp(form.cdmCooldownMinutes, 0, 1440))
    };
  }

  function datalinkDirtyFields(draft = datalinkDraft, baseline = datalinkBaseline) {
    if (!draft || !baseline) {
      return {
        connection: { logonCallsign: false, password: false, playSound: false },
        cdm: { delay: false, cooldown: false }
      };
    }
    return {
      connection: {
        logonCallsign: draft.logonCallsign !== baseline.logonCallsign,
        password: Boolean(draft.replacePassword && draft.password),
        playSound: draft.playSound !== baseline.playSound
      },
      cdm: {
        delay: draft.cdmDelayMinutes !== baseline.cdmDelayMinutes,
        cooldown: draft.cdmCooldownMinutes !== baseline.cdmCooldownMinutes
      }
    };
  }

  function datalinkRequestIncludesField(request, field) {
    if (!request?.payload) return false;
    if (field === "password") return Boolean(request.payload.replacePassword);
    return Object.hasOwn(request.payload, field);
  }

  function datalinkFieldChangedSinceRequest(request, field) {
    return datalinkRequestIncludesField(request, field) &&
      Number(request?.revisions?.[field] ?? -1) !== datalinkFieldRevisions[field];
  }

  function datalinkEffectiveDirtyFields() {
    const fields = datalinkDirtyFields();
    const request = datalinkPending.settings;
    if (!request) return fields;
    fields.connection.logonCallsign ||= datalinkFieldChangedSinceRequest(request, "logonCallsign");
    fields.connection.password ||= datalinkFieldChangedSinceRequest(request, "password");
    fields.connection.playSound ||= datalinkFieldChangedSinceRequest(request, "playSound");
    fields.cdm.delay ||= datalinkFieldChangedSinceRequest(request, "cdmDelayMinutes");
    fields.cdm.cooldown ||= datalinkFieldChangedSinceRequest(request, "cdmCooldownMinutes");
    return fields;
  }

  function datalinkScopeChangedSinceRequest(request, scope) {
    const fields = scope === "connection"
      ? ["logonCallsign", "password", "playSound"]
      : ["cdmDelayMinutes", "cdmCooldownMinutes"];
    return fields.some(field => Number(request?.revisions?.[field] ?? -1) !== datalinkFieldRevisions[field]);
  }

  function datalinkDirtyParts() {
    const fields = datalinkEffectiveDirtyFields();
    return {
      connection: Object.values(fields.connection).some(Boolean),
      cdm: Object.values(fields.cdm).some(Boolean)
    };
  }

  function hasDatalinkDraftChanges() {
    const dirty = datalinkDirtyParts();
    return dirty.connection || dirty.cdm;
  }

  function datalinkHasSavableConnectionChanges() {
    const fields = datalinkEffectiveDirtyFields().connection;
    return fields.logonCallsign || fields.playSound || (fields.password && datalinkPasswordCommitReady);
  }

  function refreshDatalinkDirtyState() {
    const dirty = datalinkDirtyParts();
    updateDirtyState();
    return dirty;
  }

  function resetDatalinkDraftFromRuntime() {
    datalinkBaseline = datalinkFormFromRuntime();
    datalinkDraft = clone(datalinkBaseline);
    datalinkPasswordCommitReady = false;
  }

  function rebaseDatalinkDraftFromRuntime() {
    if (!datalinkDraft || !datalinkBaseline) {
      resetDatalinkDraftFromRuntime();
      return;
    }
    const dirty = datalinkEffectiveDirtyFields();
    const nextBaseline = datalinkFormFromRuntime();
    const nextDraft = clone(datalinkDraft);
    if (!dirty.connection.logonCallsign)
      nextDraft.logonCallsign = nextBaseline.logonCallsign;
    if (!dirty.connection.password) {
      nextDraft.password = "";
      nextDraft.replacePassword = false;
      datalinkPasswordCommitReady = false;
    }
    if (!dirty.connection.playSound)
      nextDraft.playSound = nextBaseline.playSound;
    if (!dirty.cdm.delay)
      nextDraft.cdmDelayMinutes = nextBaseline.cdmDelayMinutes;
    if (!dirty.cdm.cooldown)
      nextDraft.cdmCooldownMinutes = nextBaseline.cdmCooldownMinutes;
    nextDraft.cdmAutoEnabled = nextBaseline.cdmAutoEnabled;
    datalinkBaseline = nextBaseline;
    datalinkDraft = normalizeDatalinkForm(nextDraft);
    refreshDatalinkDirtyState();
  }

  function captureDatalinkDraftFromControls() {
    if (!datalinkDraft) resetDatalinkDraftFromRuntime();
    const callsign = $("#datalinkLogonCallsign");
    if (!callsign) return datalinkDraft;
    datalinkDraft = normalizeDatalinkForm({
      logonCallsign: callsign.value,
      password: $("#datalinkPassword").value,
      playSound: $("#datalinkPlaySound").checked,
      cdmAutoEnabled: Boolean(state.datalink?.cdmAutoEnabled),
      cdmDelayMinutes: $("#datalinkCdmDelay").value,
      cdmCooldownMinutes: $("#datalinkCdmCooldown").value
    });
    refreshDatalinkDirtyState();
    return datalinkDraft;
  }

  function setDatalinkInputValue(control, value) {
    if (control && control.value !== String(value ?? "")) control.value = String(value ?? "");
  }

  function renderDatalink() {
    if (!$("#datalinkConnectionState")) return;
    if (!datalinkDraft || !datalinkBaseline) resetDatalinkDraftFromRuntime();
    const runtime = state.datalink || normalizeDatalinkRuntimeState();
    const draft = datalinkDraft;
    const dirty = refreshDatalinkDirtyState();
    const connectionState = runtime.connecting
      ? { label: "Connecting", className: "connecting" }
      : runtime.connected
        ? { label: "Connected", className: "connected" }
        : !runtime.controllerConnected
          ? { label: "EuroScope offline", className: "offline" }
          : { label: "Disconnected", className: "disconnected" };
    const connectionBadge = $("#datalinkConnectionState");
    connectionBadge.className = `datalink-state-badge ${connectionState.className}`;
    $("span", connectionBadge).textContent = connectionState.label;
    connectionBadge.title = runtime.statusMessage || connectionState.label;

    setDatalinkInputValue($("#datalinkLogonCallsign"), draft.logonCallsign);
    const password = $("#datalinkPassword");
    setDatalinkInputValue(password, draft.password);
    password.type = datalinkPasswordVisible ? "text" : "password";
    const showingStoredPassword = runtime.hasPassword && !draft.password;
    password.placeholder = showingStoredPassword ? "••••••••••••••" : "Enter Hoppie code";
    password.classList.toggle("stored-secret", showingStoredPassword);
    const passwordToggle = $("#datalinkPasswordToggle");
    passwordToggle.classList.toggle("showing", datalinkPasswordVisible);
    passwordToggle.disabled = showingStoredPassword;
    passwordToggle.title = showingStoredPassword
      ? "The saved code is hidden; enter a new code to replace it"
      : datalinkPasswordVisible ? "Hide code" : "Show code";
    passwordToggle.setAttribute("aria-label", showingStoredPassword
      ? "Saved Hoppie code is hidden"
      : datalinkPasswordVisible ? "Hide Hoppie code" : "Show Hoppie code");

    $("#datalinkPlaySound").checked = draft.playSound;
    setDatalinkInputValue($("#datalinkCdmDelay"), draft.cdmDelayMinutes);
    setDatalinkInputValue($("#datalinkCdmCooldown"), draft.cdmCooldownMinutes);

    const aliasPath = $("#settingsAliasFile");
    if (aliasPath) {
      aliasPath.value = runtime.cdmAliasPath || "No alias file found";
      aliasPath.title = runtime.cdmAliasPath || "No alias file found";
    }

    const connectionBusy = Boolean(runtime.connecting || datalinkPending.connection);
    const settingsRequest = datalinkPending.settings;
    const cdmSettingsBusy = Boolean(settingsRequest?.includeCdm);
    const connectButton = $("#datalinkConnectButton");
    connectButton.textContent = datalinkConnectAfterSave
      ? "Saving..."
      : runtime.connected
      ? connectionBusy ? "Disconnecting..." : "Disconnect"
      : connectionBusy ? "Connecting..." : "Connect";
    connectButton.classList.toggle("primary", !runtime.connected);
    connectButton.disabled = connectionBusy || datalinkConnectAfterSave || !runtime.controllerConnected ||
      (!runtime.connected && (!draft.logonCallsign || (!runtime.hasPassword && !draft.replacePassword)));
    connectButton.title = !runtime.controllerConnected
      ? "EuroScope is not connected as a controller"
      : !runtime.connected && !draft.logonCallsign
        ? "Enter a CPDLC logon callsign"
      : !runtime.connected && !runtime.hasPassword && !draft.replacePassword
          ? "Store a Hoppie code before connecting"
        : !runtime.connected && dirty.connection
          ? "Save changed connection settings, then connect"
          : runtime.connected ? "Disconnect Hoppie CPDLC" : "Connect Hoppie CPDLC";

    const pollButton = $("#datalinkPollButton");
    pollButton.disabled = !runtime.connected || runtime.connecting || runtime.pollInProgress || Boolean(datalinkPending.poll);
    pollButton.textContent = runtime.pollInProgress || datalinkPending.poll ? "Polling..." : "Poll";

    const reminderRequest = settingsRequest;
    const pendingReminderOperation = String(reminderRequest?.kind || "").startsWith("reminder-")
      ? String(reminderRequest.kind)
      : "";
    const reminderOperation = pendingReminderOperation || String(datalinkQueuedReminderAction?.kind || "");
    const reminderActionBusy = cdmSettingsBusy || Boolean(datalinkQueuedReminderAction);
    const reminderRunning = Boolean(runtime.cdmAutoEnabled);
    const reminderReady = runtime.controllerConnected && Boolean(runtime.activeAirport) &&
      runtime.vacdmConfigured && runtime.vacdmReady && runtime.cdmAliasReady;
    const reminderUnavailableReason = !runtime.controllerConnected
      ? "EuroScope is not connected as a controller"
      : !runtime.activeAirport
        ? "Select an active airport first"
        : !runtime.vacdmConfigured
          ? "Configure a vACDM server for the active profile"
          : !runtime.vacdmReady
            ? "Wait for a current vACDM snapshot"
            : !runtime.cdmAliasReady
              ? "Add a valid .cdm entry to the EuroScope alias file"
              : "";
    const reminderWaiting = reminderRunning && !reminderReady;
    const reminderTransitioning = reminderOperation === "reminder-run" || reminderOperation === "reminder-stop";
    const reminderBadge = $("#datalinkReminderState");
    const reminderLabel = reminderOperation === "reminder-run"
      ? "Starting"
      : reminderOperation === "reminder-stop"
        ? "Stopping"
        : reminderWaiting ? "Waiting"
          : reminderRunning ? "Running" : "Stopped";
    reminderBadge.className = `datalink-state-badge ${reminderWaiting || reminderTransitioning ? "connecting" : reminderRunning ? "connected" : "disconnected"}`;
    $("span", reminderBadge).textContent = reminderLabel;
    reminderBadge.title = reminderOperation === "reminder-run"
      ? "Starting automatic PDC reminders"
      : reminderOperation === "reminder-stop"
        ? "Stopping automatic PDC reminders"
        : reminderWaiting
          ? `Automatic PDC reminders are enabled but waiting: ${reminderUnavailableReason}`
      : reminderRunning
        ? "Automatic PDC reminders are running"
        : "Automatic PDC reminders are stopped";

    const scanButton = $("#datalinkScanButton");
    scanButton.disabled = !reminderReady || Boolean(datalinkPending.scan);
    scanButton.textContent = datalinkPending.scan ? "Checking..." : "Check now";
    scanButton.title = !runtime.controllerConnected
      ? "EuroScope is not connected as a controller"
      : !runtime.activeAirport ? "Select an active airport first"
        : !runtime.vacdmConfigured ? "Configure a vACDM server for the active profile"
        : !runtime.vacdmReady ? "Wait for a current vACDM snapshot"
        : !runtime.cdmAliasReady ? "Add a valid .cdm entry to the EuroScope alias file"
          : "Check eligible departures now";

    const reminderToggleButton = $("#datalinkReminderToggleButton");
    reminderToggleButton.textContent = reminderOperation === "reminder-run"
      ? "Starting..."
      : reminderOperation === "reminder-stop"
        ? "Stopping..."
        : reminderRunning ? "Stop" : "Run";
    reminderToggleButton.classList.toggle("primary", !reminderRunning);
    reminderToggleButton.classList.toggle("danger", reminderRunning);
    reminderToggleButton.disabled = reminderActionBusy || (!reminderRunning && !reminderReady);
    reminderToggleButton.title = reminderRunning
      ? "Stop automatic PDC reminders"
      : reminderUnavailableReason || "Start automatic PDC reminders";

    const delayInput = $("#datalinkCdmDelay");
    const cooldownInput = $("#datalinkCdmCooldown");
    delayInput.disabled = reminderActionBusy || reminderRunning;
    cooldownInput.disabled = reminderActionBusy || reminderRunning;
    delayInput.title = reminderRunning ? "Stop reminders before changing the delay" : "Delay before the first automatic reminder";
    cooldownInput.title = reminderRunning ? "Stop reminders before changing the cooldown" : "Delay before an automatic reminder may be repeated; 0 sends only once per eligibility period";
    datalinkControlsInitialized = true;
  }

  function applyDatalinkRuntimeState(incoming) {
    const previous = state.datalink || normalizeDatalinkRuntimeState();
    state.datalink = normalizeDatalinkRuntimeState(incoming, state.datalink);
    rebaseDatalinkDraftFromRuntime();
    if (previous.connecting && !state.datalink.connecting) {
      const message = state.datalink.statusMessage || (state.datalink.connected ? "Connected to Hoppie" : "Hoppie connection failed");
      if (state.datalink.connected) {
        setStatus(message);
      } else {
        setStatus(message, "error");
        showToast(message, "error");
      }
    }
    if (state.ui.page === "datalink") {
      renderDatalink();
      updateContext();
    }
  }

  function requestDatalinkState(force = false) {
    if (state.ui.page !== "datalink" || !state.ui.controlCenterOpen || document.hidden) return;
    const now = Date.now();
    if (!force && now - lastDatalinkStateRequestAt < 900) return;
    lastDatalinkStateRequestAt = now;
    const requestId = postBridge("datalink.state.request", {});
    if (!HOST_MODE) {
      setTimeout(() => receiveHostMessage({
        version: PROTOCOL_VERSION,
        requestId,
        type: "datalink.state",
        payload: { datalink: clone(state.datalink) }
      }), 20);
    }
  }

  function previewDatalinkAck(action, requestId, message, delay = 120) {
    if (HOST_MODE) return;
    setTimeout(() => receiveHostMessage({
      version: PROTOCOL_VERSION,
      requestId,
      type: "state.ack",
      payload: { action, message }
    }), delay);
  }

  function submitDatalinkSettings({
    includeConnection = false,
    includeCdm = false,
    cdmAutoEnabled,
    kind = "live-action",
    silent = false
  } = {}) {
    const draft = captureDatalinkDraftFromControls();
    if (!draft || datalinkPending.settings) return false;
    const dirtyFields = datalinkDirtyFields();
    const requestIncludesConnection = includeConnection && (
      dirtyFields.connection.logonCallsign ||
      dirtyFields.connection.playSound ||
      (dirtyFields.connection.password && datalinkPasswordCommitReady));
    const requestIncludesCdm = Boolean(includeCdm);
    if (!requestIncludesConnection && !requestIncludesCdm) return false;
    if (requestIncludesConnection && !draft.logonCallsign) {
      $("#datalinkLogonCallsign").classList.add("invalid");
      if (!silent) showToast("Enter a CPDLC logon callsign", "error");
      return false;
    }
    $("#datalinkLogonCallsign").classList.remove("invalid");

    const payload = {};
    if (requestIncludesConnection) {
      if (dirtyFields.connection.logonCallsign)
        payload.logonCallsign = draft.logonCallsign;
      if (dirtyFields.connection.playSound)
        payload.playSound = draft.playSound;
      if (dirtyFields.connection.password && datalinkPasswordCommitReady) {
        payload.password = draft.password;
        payload.replacePassword = true;
      }
    }
    if (requestIncludesCdm) {
      payload.cdmAutoEnabled = typeof cdmAutoEnabled === "boolean"
        ? cdmAutoEnabled
        : Boolean(state.datalink.cdmAutoEnabled);
      payload.cdmDelayMinutes = draft.cdmDelayMinutes;
      payload.cdmCooldownMinutes = draft.cdmCooldownMinutes;
    }

    const requestId = postBridge("datalink.settings.update", payload);
    if (!requestId) return false;
    datalinkPending.settings = {
      id: requestId,
      action: "datalink.settings.update",
      kind,
      silent,
      includeConnection: requestIncludesConnection,
      includeCdm: requestIncludesCdm,
      submitted: clone(draft),
      revisions: { ...datalinkFieldRevisions },
      payload: clone(payload)
    };
    armDatalinkPendingTimeout("settings", requestId);
    renderDatalink();
    if (!silent) {
      const pendingText = kind === "reminder-run"
        ? "Starting PDC reminders..."
        : kind === "reminder-stop"
          ? "Stopping PDC reminders..."
          : kind === "global-save"
            ? "Saving datalink settings..."
            : kind === "connect"
              ? "Saving connection settings..."
              : "Updating datalink settings...";
      setStatus(pendingText);
    }
    previewDatalinkAck("datalink.settings.update", requestId, "Datalink settings applied");
    return true;
  }

  function togglePdcReminders() {
    const running = Boolean(state.datalink.cdmAutoEnabled);
    const operation = {
      includeCdm: true,
      cdmAutoEnabled: !running,
      kind: running ? "reminder-stop" : "reminder-run"
    };
    if (datalinkPending.settings) {
      if (datalinkPending.settings.includeCdm) return;
      datalinkQueuedReminderAction = operation;
      renderDatalink();
      return;
    }
    submitDatalinkSettings(operation);
  }

  function flushQueuedPdcReminderAction() {
    if (datalinkPending.settings || !datalinkQueuedReminderAction) return false;
    const operation = datalinkQueuedReminderAction;
    datalinkQueuedReminderAction = null;
    return submitDatalinkSettings(operation);
  }

  function continueQueuedDatalinkWork() {
    if (datalinkPending.settings) return;
    if (globalSaveAfterDatalink) {
      continueGlobalSaveAfterDatalink();
      return;
    }
    if (flushQueuedPdcReminderAction()) return;
    if (!datalinkConnectAfterSave) return;
    if (datalinkDirtyParts().connection) {
      if (!datalinkHasSavableConnectionChanges()) {
        datalinkConnectAfterSave = false;
        renderDatalink();
        showToast("Finish the CPDLC connection settings before connecting", "error");
        return;
      }
      if (!submitDatalinkSettings({ includeConnection: true, kind: "connect", silent: true })) {
        datalinkConnectAfterSave = false;
        renderDatalink();
      }
      return;
    }
    datalinkConnectAfterSave = false;
    renderDatalink();
    window.setTimeout(toggleDatalinkConnection, 0);
  }

  function toggleDatalinkConnection() {
    const runtime = state.datalink;
    if (datalinkPending.connection || runtime.connecting) return;
    captureDatalinkDraftFromControls();
    if (!runtime.connected && datalinkDraft?.replacePassword)
      datalinkPasswordCommitReady = true;
    if (!runtime.controllerConnected) {
      showToast("Connect EuroScope as a controller first", "error");
      return;
    }
    if (!runtime.connected && !datalinkDraft?.logonCallsign) {
      $("#datalinkLogonCallsign").classList.add("invalid");
      showToast("Enter a CPDLC logon callsign", "error");
      return;
    }
    if (!runtime.connected && !runtime.hasPassword && !datalinkDraft?.replacePassword) {
      showToast("Store a Hoppie code before connecting", "error");
      return;
    }
    if (!runtime.connected &&
      (datalinkDirtyParts().connection || datalinkPending.settings?.includeConnection)) {
      datalinkConnectAfterSave = true;
      const pendingConnectionSave = datalinkPending.settings?.includeConnection
        ? datalinkPending.settings
        : null;
      const dirtyFields = datalinkEffectiveDirtyFields().connection;
      const pendingCoversDraft = pendingConnectionSave &&
        (!dirtyFields.logonCallsign || pendingConnectionSave.payload.logonCallsign === datalinkDraft.logonCallsign) &&
        (!dirtyFields.playSound || pendingConnectionSave.payload.playSound === datalinkDraft.playSound) &&
        (!dirtyFields.password || !datalinkPasswordCommitReady ||
          (pendingConnectionSave.payload.replacePassword && pendingConnectionSave.submitted.password === datalinkDraft.password));
      if (!pendingCoversDraft && !datalinkPending.settings &&
          !submitDatalinkSettings({ includeConnection: true, kind: "connect", silent: true })) {
        datalinkConnectAfterSave = false;
        showToast("Save valid CPDLC settings before connecting", "error");
      }
      renderDatalink();
      return;
    }
    const action = runtime.connected ? "datalink.connection.disconnect" : "datalink.connection.connect";
    const requestId = postBridge(action, {});
    if (!requestId) return;
    datalinkPending.connection = { id: requestId, action };
    armDatalinkPendingTimeout("connection", requestId);
    if (!HOST_MODE) {
      state.datalink.connecting = !runtime.connected;
      if (runtime.connected) state.datalink.connected = false;
    }
    renderDatalink();
    previewDatalinkAck(action, requestId, runtime.connected ? "CPDLC disconnected" : "CPDLC connected", 260);
    if (!HOST_MODE) {
      setTimeout(() => receiveHostMessage({
        version: PROTOCOL_VERSION,
        requestId,
        type: "datalink.state",
        payload: { datalink: { ...clone(state.datalink), connecting: false, connected: action.endsWith(".connect"), statusMessage: action.endsWith(".connect") ? "Connected to Hoppie." : "Disconnected from Hoppie." } }
      }), 320);
    }
  }

  function pollDatalinkNow() {
    const runtime = state.datalink;
    if (!runtime.connected || runtime.pollInProgress || datalinkPending.poll) return;
    const requestId = postBridge("datalink.poll", {});
    if (!requestId) return;
    datalinkPending.poll = { id: requestId, action: "datalink.poll" };
    armDatalinkPendingTimeout("poll", requestId);
    if (!HOST_MODE) state.datalink.pollInProgress = true;
    renderDatalink();
    previewDatalinkAck("datalink.poll", requestId, "CPDLC message poll completed", 260);
    if (!HOST_MODE) {
      setTimeout(() => receiveHostMessage({
        version: PROTOCOL_VERSION,
        requestId,
        type: "datalink.state",
        payload: { datalink: { ...clone(state.datalink), pollInProgress: false, statusMessage: "Message poll completed." } }
      }), 300);
    }
  }

  function scanCdmReminders() {
    const runtime = state.datalink;
    if (datalinkPending.scan || !runtime.controllerConnected || !runtime.activeAirport ||
        !runtime.vacdmConfigured || !runtime.vacdmReady || !runtime.cdmAliasReady) return;
    const requestId = postBridge("cdm.scan", {});
    if (!requestId) return;
    datalinkPending.scan = { id: requestId, action: "cdm.scan" };
    armDatalinkPendingTimeout("scan", requestId);
    renderDatalink();
    previewDatalinkAck("cdm.scan", requestId, `CDM reminder check completed for ${runtime.activeAirport}`, 260);
  }

  function datalinkPendingSlotForAction(action) {
    if (action === "datalink.settings.update") return "settings";
    if (action === "datalink.connection.connect" || action === "datalink.connection.disconnect") return "connection";
    if (action === "datalink.poll") return "poll";
    if (action === "cdm.scan") return "scan";
    return "";
  }

  function armDatalinkPendingTimeout(slot, requestId) {
    if (!slot || !requestId) return;
    window.setTimeout(() => {
      const timedOut = datalinkPending[slot];
      if (timedOut?.id !== requestId) return;
      datalinkPending[slot] = null;
      if (slot === "settings" && timedOut.kind === "global-save") globalSaveAfterDatalink = false;
      if (slot === "settings" && timedOut.kind === "connect") datalinkConnectAfterSave = false;
      if (slot === "connection") state.datalink.connecting = false;
      if (slot === "poll") state.datalink.pollInProgress = false;
      renderDatalink();
      const message = "The datalink operation timed out. Check the connection and try again.";
      setStatus(message, "error");
      showToast("Datalink operation timed out", "error");
      updateDirtyState();
    }, REQUEST_TIMEOUT_MS);
  }

  function isDatalinkAction(action) {
    return String(action || "").startsWith("datalink.") || action === "cdm.scan";
  }

  function finishDatalinkAck(message) {
    const action = String(message.payload.action || "");
    if (!isDatalinkAction(action)) return false;
    const slot = datalinkPendingSlotForAction(action);
    const pending = slot ? datalinkPending[slot] : null;
    if (pending?.id && !messageMatchesRequest(message, pending.id)) return true;
    if (slot === "settings" && !pending) return true;
    let completedRequest = null;
    if (slot === "settings" && datalinkPending.settings) {
      const request = datalinkPending.settings;
      completedRequest = request;
      const submitted = request.submitted;
      const current = clone(datalinkDraft || submitted);
      const currentDirty = datalinkDirtyFields(current, datalinkBaseline);
      const changedAfterSubmit = {
        logonCallsign: datalinkFieldChangedSinceRequest(request, "logonCallsign"),
        password: datalinkFieldChangedSinceRequest(request, "password"),
        playSound: datalinkFieldChangedSinceRequest(request, "playSound"),
        cdmDelayMinutes: datalinkFieldChangedSinceRequest(request, "cdmDelayMinutes"),
        cdmCooldownMinutes: datalinkFieldChangedSinceRequest(request, "cdmCooldownMinutes")
      };
      const applied = {};
      if (request.includeConnection) {
        if (Object.hasOwn(request.payload, "logonCallsign"))
          applied.logonCallsign = request.payload.logonCallsign;
        if (Object.hasOwn(request.payload, "playSound"))
          applied.playSound = request.payload.playSound;
        if (request.payload.replacePassword) applied.hasPassword = true;
      }
      if (request.includeCdm) {
        applied.cdmAutoEnabled = request.payload.cdmAutoEnabled;
        applied.cdmDelayMinutes = request.payload.cdmDelayMinutes;
        applied.cdmCooldownMinutes = request.payload.cdmCooldownMinutes;
      }
      state.datalink = normalizeDatalinkRuntimeState(applied, state.datalink);
      datalinkPending.settings = null;

      const nextBaseline = datalinkFormFromRuntime();
      if (request.includeConnection) {
        if (!changedAfterSubmit.logonCallsign && (!currentDirty.connection.logonCallsign ||
          (Object.hasOwn(request.payload, "logonCallsign") && current.logonCallsign === submitted.logonCallsign)))
          current.logonCallsign = nextBaseline.logonCallsign;
        if (!changedAfterSubmit.playSound && (!currentDirty.connection.playSound ||
          (Object.hasOwn(request.payload, "playSound") && current.playSound === submitted.playSound)))
          current.playSound = nextBaseline.playSound;
        if (!changedAfterSubmit.password && request.payload.replacePassword && current.password === submitted.password) {
          current.password = "";
          current.replacePassword = false;
          datalinkPasswordVisible = false;
          datalinkPasswordCommitReady = false;
        }
      }
      if (request.includeCdm) {
        if (!changedAfterSubmit.cdmDelayMinutes &&
          (!currentDirty.cdm.delay || current.cdmDelayMinutes === submitted.cdmDelayMinutes))
          current.cdmDelayMinutes = nextBaseline.cdmDelayMinutes;
        if (!changedAfterSubmit.cdmCooldownMinutes &&
          (!currentDirty.cdm.cooldown || current.cdmCooldownMinutes === submitted.cdmCooldownMinutes))
          current.cdmCooldownMinutes = nextBaseline.cdmCooldownMinutes;
      }
      current.cdmAutoEnabled = nextBaseline.cdmAutoEnabled;
      datalinkBaseline = nextBaseline;
      datalinkDraft = normalizeDatalinkForm(current);
      refreshDatalinkDirtyState();
    } else if (slot) {
      datalinkPending[slot] = null;
    }
    const text = completedRequest?.kind === "reminder-run"
      ? "PDC reminders running"
      : completedRequest?.kind === "reminder-stop"
        ? "PDC reminders stopped"
        : completedRequest?.kind === "global-save" || completedRequest?.kind === "connect"
          ? "Datalink settings saved"
          : message.payload.message || "Datalink operation completed";
    renderDatalink();
    setStatus(text);
    if (!completedRequest?.silent) showToast(text, "success");
    if (completedRequest) window.setTimeout(continueQueuedDatalinkWork, 0);
    return true;
  }

  function finishDatalinkError(message) {
    let action = String(message.payload.action || "");
    let slot = datalinkPendingSlotForAction(action);
    if (!slot) {
      for (const candidate of ["settings", "connection", "poll", "scan"]) {
        const request = datalinkPending[candidate];
        if (request?.id && messageMatchesRequest(message, request.id)) {
          slot = candidate;
          action = request.action || request.payload?.action || action;
          break;
        }
      }
    }
    if (!slot && !isDatalinkAction(action)) return false;
    const failedRequest = slot ? datalinkPending[slot] : null;
    if (failedRequest?.id && !messageMatchesRequest(message, failedRequest.id)) return true;
    if (slot === "settings" && !failedRequest) return true;
    if (slot) datalinkPending[slot] = null;
    if (slot === "settings" && failedRequest?.kind === "global-save") globalSaveAfterDatalink = false;
    if (slot === "settings" && failedRequest?.kind === "connect") datalinkConnectAfterSave = false;
    if (slot === "connection") state.datalink.connecting = false;
    if (slot === "poll") state.datalink.pollInProgress = false;
    const text = message.payload.message || message.payload.error || "Datalink operation failed";
    state.datalink.statusMessage = text;
    renderDatalink();
    setStatus(text, "error");
    showToast(text, "error");
    updateDirtyState();
    if (slot === "settings") window.setTimeout(continueQueuedDatalinkWork, 0);
    return true;
  }

  function updateViewActive() {
    return state.ui.controlCenterOpen && state.ui.page === "settings" && !document.hidden;
  }

  function formatUpdateTime(value, fallback = "Never") {
    const raw = String(value || "").trim();
    if (!raw) return fallback;
    const timestamp = new Date(raw);
    if (Number.isNaN(timestamp.getTime())) return raw;
    return timestamp.toLocaleString([], {
      year: "numeric", month: "short", day: "2-digit", hour: "2-digit", minute: "2-digit"
    });
  }

  function updateStatusLabel(status) {
    const labels = {
      idle: "Idle",
      checking: "Checking",
      rate_limited: "Rate limited",
      downloading: "Downloading",
      verifying: "Verifying",
      installing: "Installing",
      updated: "Updated",
      up_to_date: "Up to date",
      deferred: "Manual update",
      error: "Update error"
    };
    return labels[String(status || "").toLowerCase()] || "Awaiting state";
  }

  function updateStatusClass(status) {
    const normalized = String(status || "").toLowerCase();
    if (["checking", "downloading", "verifying", "installing"].includes(normalized)) return "active";
    if (["updated", "up_to_date"].includes(normalized)) return "live";
    if (["rate_limited", "deferred"].includes(normalized)) return "warning";
    if (normalized === "error") return "error";
    return "waiting";
  }

  function expireUpdateRequest(slot, now = Date.now()) {
    const request = updateCenter.pending[slot];
    if (!request || now - request.startedAt < REQUEST_TIMEOUT_MS) return false;
    updateCenter.pending[slot] = null;
    if (slot === "settings" && request.previous) updateCenter.config = request.previous;
    updateCenter.configError = slot === "state"
      ? "Still waiting for updater state from vSMR."
      : "The updater request timed out; no radar state was changed.";
    return true;
  }

  function requestUpdateState(force = false) {
    if (!HOST_MODE || !updateViewActive()) return;
    const now = Date.now();
    expireUpdateRequest("state", now);
    if (updateCenter.pending.state) return;
    if (!force && now - updateCenter.lastRequestAt < 1400) return;
    updateCenter.lastRequestAt = now;
    const id = postBridge("update.state.request", {});
    if (!id) return;
    updateCenter.pending.state = { id, startedAt: now };
  }

  function applyUpdateState(payload, message = null) {
    if (!payload || typeof payload !== "object" || Array.isArray(payload)) return;
    if (message && updateCenter.pending.state?.id && messageMatchesRequest(message, updateCenter.pending.state.id))
      updateCenter.pending.state = null;
    if (message && updateCenter.pending.settings?.id && messageMatchesRequest(message, updateCenter.pending.settings.id))
      updateCenter.pending.settings = null;
    if (payload.config && typeof payload.config === "object" && !Array.isArray(payload.config))
      updateCenter.config = { ...updateCenter.config, ...clone(payload.config) };
    if (payload.state && typeof payload.state === "object" && !Array.isArray(payload.state))
      updateCenter.state = { ...updateCenter.state, ...clone(payload.state) };
    updateCenter.available = payload.available !== false;
    updateCenter.configWritable = payload.configWritable !== false;
    updateCenter.configError = String(payload.configError || "");
    if (state.ui.page === "settings") renderUpdateCenter();
  }

  function submitUpdateSettings(changes, successMessage = "Update settings saved") {
    if (updateCenter.pending.settings || !updateCenter.configWritable) return;
    const previous = clone(updateCenter.config);
    updateCenter.config = { ...updateCenter.config, ...changes };
    const payload = {
      auto_check: updateCenter.config.auto_check !== false,
      auto_download: updateCenter.config.auto_download !== false,
      auto_install: updateCenter.config.auto_install !== false,
      channel: updateCenter.config.channel === "stable" ? "stable" : "beta",
      skipped_version: String(updateCenter.config.skipped_version || "")
    };
    if (!HOST_MODE) {
      renderUpdateCenter();
      showToast(successMessage, "success");
      return;
    }
    const id = postBridge("update.settings.update", payload);
    if (!id) {
      updateCenter.config = previous;
      renderUpdateCenter();
      return;
    }
    updateCenter.pending.settings = { id, startedAt: Date.now(), previous, successMessage };
    renderUpdateCenter();
  }

  function requestUpdateAction(action) {
    if (updateCenter.pending.action || !updateCenter.available) return;
    if (!HOST_MODE) {
      updateCenter.state.message = action === "retry_update"
        ? "Update retry queued for the next startup."
        : "Update check queued for the next startup.";
      renderUpdateCenter();
      showToast(updateCenter.state.message, "success");
      return;
    }
    const id = postBridge("update.action.request", { action });
    if (!id) return;
    updateCenter.pending.action = { id, action, startedAt: Date.now() };
    renderUpdateCenter();
  }

  function openUpdateRelease() {
    const url = safeReleaseUrl(updateCenter.state?.release_url);
    if (!url) return;
    if (!HOST_MODE) {
      showToast("Release links open through the native vSMR host.", "info");
      return;
    }
    postBridge("update.release.open", { url });
  }

  function updateMessageSlot(message) {
    for (const slot of ["state", "settings", "action"]) {
      const request = updateCenter.pending[slot];
      if (request?.id && messageMatchesRequest(message, request.id)) return slot;
    }
    return "";
  }

  function finishUpdateAck(message) {
    const action = String(message.payload?.action || "");
    let slot = updateMessageSlot(message);
    if (!slot && action === "update.settings.update") slot = "settings";
    if (!slot && action === "update.action.request") slot = "action";
    if (!slot) return false;
    const request = updateCenter.pending[slot];
    updateCenter.pending[slot] = null;
    const text = String(message.payload?.message || request?.successMessage || "Updater request saved");
    setStatus(text, "info");
    showToast(text, "success");
    renderUpdateCenter();
    return true;
  }

  function finishUpdateError(message) {
    const slot = updateMessageSlot(message);
    if (!slot) return false;
    const request = updateCenter.pending[slot];
    updateCenter.pending[slot] = null;
    if (slot === "settings" && request?.previous) updateCenter.config = request.previous;
    const text = String(message.payload?.message || message.payload?.error || "Updater request failed");
    updateCenter.configError = text;
    setStatus(text, "error");
    showToast(text, "error");
    renderUpdateCenter();
    return true;
  }

  function safeReleaseUrl(value) {
    try {
      const raw = String(value || "").trim();
      if (raw.includes("%") || /(?:^|\/)\.{1,2}(?:\/|$)/.test(raw)) return "";
      const url = new URL(raw);
      if (url.protocol !== "https:" || url.hostname !== "github.com" || url.port ||
        url.username || url.password || url.search || url.hash) return "";

      const releasesPath = "/IWantPizzaa/vSMR/releases";
      if (url.pathname === releasesPath || url.pathname === `${releasesPath}/latest`) {
        return url.href;
      }

      const tagPrefix = `${releasesPath}/tag/`;
      if (!url.pathname.startsWith(tagPrefix)) return "";
      const tag = url.pathname.slice(tagPrefix.length);
      if (!tag || tag === "." || tag === ".." || !/^[A-Za-z0-9._+~-]+$/.test(tag)) return "";
      return url.href;
    } catch (_) {
      return "";
    }
  }

  function renderUpdateCenter() {
    const badge = $("#updateStateBadge");
    if (!badge) return;
    expireUpdateRequest("settings");
    expireUpdateRequest("action");

    const updater = updateCenter.state || {};
    const config = updateCenter.config || {};
    const status = String(updater.status || "idle").toLowerCase();
    const directPreview = !HOST_MODE;
    const writable = directPreview || (updateCenter.available && updateCenter.configWritable);
    const busy = Boolean(updateCenter.pending.settings);

    badge.className = `update-state-badge ${directPreview ? "preview" : updateStatusClass(status)}`;
    badge.textContent = directPreview ? "Preview" : updateStatusLabel(status);

    const installed = String(updater.installed_version || "--");
    const available = String(updater.available_version || "").trim();
    $("#updateVersionSummary").textContent = available
      ? `Installed ${installed} · Available ${available}`
      : `Installed ${installed}`;

    const errorText = String(updater.error || updateCenter.configError || "").trim();
    const notice = $("#updateNotice");
    notice.classList.toggle("error", Boolean(errorText));
    notice.textContent = errorText || String(
      updater.message ||
      (config.auto_check === false
        ? "Automatic update checks are disabled."
        : `Last checked: ${formatUpdateTime(updater.last_checked_utc)}`)
    );

    ensureSelectValue($("#updateChannel"), config.channel === "stable" ? "stable" : "beta");
    $("#updateAutoCheck").checked = config.auto_check !== false;
    $("#updateAutoDownload").checked = config.auto_download !== false;
    $("#updateAutoInstall").checked = config.auto_install !== false;
    $("#updateChannel").disabled = !writable || busy;
    $("#updateAutoCheck").disabled = !writable || busy;
    $("#updateAutoDownload").disabled = !writable || busy || config.auto_check === false;
    $("#updateAutoInstall").disabled = !writable || busy || config.auto_download === false;

    const pendingAction = Boolean(updateCenter.pending.action);
    const checkButton = $("#updateCheckButton");
    checkButton.disabled = !updateCenter.available || pendingAction;
    checkButton.textContent = pendingAction ? "Queuing..." : "Check on next startup";

    const retryButton = $("#updateRetryButton");
    retryButton.hidden = !["error", "rate_limited"].includes(status);
    retryButton.disabled = !updateCenter.available || pendingAction;

    const clearSkipButton = $("#updateClearSkipButton");
    const skippedVersion = String(config.skipped_version || "").trim();
    clearSkipButton.hidden = !skippedVersion;
    clearSkipButton.disabled = !writable || busy;
    clearSkipButton.title = skippedVersion ? `Resume offering ${skippedVersion}` : "No release is skipped";

    const releaseLink = $("#updateReleaseLink");
    const releaseUrl = safeReleaseUrl(updater.release_url);
    releaseLink.hidden = !releaseUrl;
    releaseLink.title = releaseUrl || "No release notes are available";
  }

  function renderSettings() {
    const settings = state.settings;
    $("#settingsProfileFile").value = settings.profileFile;
    $("#settingsAvisoFile").value = settings.avisoFile;
    $("#settingsProfileFile").title = settings.profileFile;
    $("#settingsAvisoFile").title = settings.avisoFile;
    ensureSelectValue($("#settingsResolutionPreset"), settings.resolutionPreset || "1080p");
    $("#settingsShowFps").checked = settings.showFps !== false;
    $("#settingsScreenRotation").value = clamp(settings.screenRotation ?? 0, 0, 359.9).toFixed(1);
    const avisoColorPalette = settings.avisoColorPalette === "day" ? "day" : "night";
    $$('[data-aviso-color-palette]').forEach(button => {
      const active = button.dataset.avisoColorPalette === avisoColorPalette;
      button.classList.toggle("active", active);
      button.setAttribute("aria-pressed", active ? "true" : "false");
    });
    const restoreBackup = $("#restoreProfilesBackupButton");
    if (restoreBackup) {
      restoreBackup.disabled = !settings.dataHealth?.profilesBackupAvailable || Boolean(pending.reload || pending.save || pending.resource);
      restoreBackup.title = restoreBackup.disabled
        ? "No validated profiles backup is available"
        : "Restore the last validated .bak copy";
    }
    if (HOST_MODE) {
      ["#settingsProfileFile", "#settingsAvisoFile", "#settingsAliasFile"].forEach(selector => {
        const control = $(selector);
        if (control) control.readOnly = true;
      });
    }
    renderDataHealthStatus();
  }

  function applySettings({ render = true } = {}) {
    Object.assign(state.settings, {
      profileFile: $("#settingsProfileFile").value,
      avisoFile: $("#settingsAvisoFile").value,
      resolutionPreset: $("#settingsResolutionPreset").value || "1080p",
      showFps: $("#settingsShowFps").checked,
      screenRotation: Math.round(clamp($("#settingsScreenRotation").value, 0, 359.9) * 10) / 10
    });
    state.profiles.forEach(record => {
      record.data.targets ||= {};
      record.data.targets.small_icon_boost_resolution_preset = state.settings.resolutionPreset;
    });
    clearUnappliedEditorSection($("#settingsResolutionPreset"));
    markDirty("Settings updated", ["profiles", "settings"]);
    if (render) renderIcons();
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
      activeProfile: activeProfile().name || "",
      configRevision: state.configRevision || "",
      avisoRevision: state.avisoRevision || "",
      recoveryConfirmed: Boolean(state.recoveryConfirmed),
      avisoRecoveryConfirmed: Boolean(state.avisoRecoveryConfirmed)
    };
  }

  function stageFocusedEditorValue() {
    const active = document.activeElement;
    if (!stageEditorControl(active)) {
      revealInvalidEditorControl(active);
      return false;
    }
    if (active?.checkValidity && !active.checkValidity()) {
      revealInvalidEditorControl(active);
      return false;
    }
    if (hasUnappliedEditorInputs()) {
      let invalid = $("#controlWindow input:invalid, #controlWindow select:invalid, #controlWindow textarea:invalid");
      const groupInput = $("#avisoGroupName");
      if (!invalid && unappliedEditorSections.has(editorSectionKey(groupInput)) &&
          !String(drafts.avisoGroup?.data?.name ?? groupInput?.value ?? "").trim()) {
        groupInput?.setCustomValidity("Enter a group name");
        invalid = groupInput;
      }
      if (invalid) revealInvalidEditorControl(invalid);
      if (!invalid) showToast("Finish the current edit before saving", "error");
      return false;
    }
    return true;
  }

  function revealInvalidEditorControl(control) {
    if (!control) return;
    if (control.id === "avisoGroupName" && state.ui.page !== "groups") setPage("groups");
    control.scrollIntoView?.({ block: "nearest" });
    control.focus?.({ preventScroll: true });
    control.reportValidity?.();
  }

  function hasAutosaveWork() {
    return state.dirty || hasUnappliedEditorInputs() || hasDatalinkDraftChanges();
  }

  function cancelAutosave(clearQueued = true) {
    if (autosaveTimer) window.clearTimeout(autosaveTimer);
    autosaveTimer = 0;
    if (clearQueued) autosaveQueued = false;
  }

  function scheduleAutosave(delay = AUTOSAVE_DEBOUNCE_MS) {
    autosaveQueued = true;
    if (autosaveTimer) window.clearTimeout(autosaveTimer);
    autosaveTimer = window.setTimeout(runAutosave, delay);
  }

  function runAutosave() {
    autosaveTimer = 0;
    if (!autosaveQueued || !hasAutosaveWork()) {
      autosaveQueued = false;
      return;
    }
    const busy = Boolean(
      pending.save || pending.reload || pending.resource || runtimeCommandPending.size ||
      splitAvisoContext || datalinkPending.settings || globalSaveAfterDatalink
    );
    if (busy) {
      scheduleAutosave(AUTOSAVE_RETRY_MS);
      return;
    }
    if (!hostAuthoritativeReady || state.externalEditConflict || state.airport !== state.hostAirport ||
        (state.settings?.dataHealth?.profilesHealthy === false && !state.recoveryConfirmed)) {
      return;
    }
    autosaveQueued = false;
    saveAll({ automatic: true });
  }

  function startConfigurationSave() {
	if (!hostAuthoritativeReady || !state.dirty || pending.save || pending.reload || pending.resource || runtimeCommandPending.size || splitAvisoContext || state.externalEditConflict || state.airport !== state.hostAirport ||
      (state.settings?.dataHealth?.profilesHealthy === false && !state.recoveryConfirmed)) return false;
    const payload = serializeStatePayload(true);
    // AVISO is an independent, potentially multi-megabyte resource.  Do not
    // rewrite it (or make profile recovery depend on it) when only profiles or
    // settings changed.  Resource imports and AVISO editor changes are already
    // represented in the AVISO history chunk and will still be included.
    if (savedSnapshot && snapshotChunk(state.aviso) === savedSnapshot.aviso)
      delete payload.aviso;
    const submittedSnapshot = captureHistorySnapshot();
    pending.save = postBridge("state.save", payload);
    if (!pending.save) {
      saveInFlightSnapshot = null;
      return false;
    }
    saveInFlightSnapshot = submittedSnapshot;
    armPendingTimeout("save", pending.save);
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
    return true;
  }

  function requestReload() {
	if (pending.reload || pending.save || pending.resource || runtimeCommandPending.size || splitAvisoContext) return;
    const couldNotStageDraft = !stageFocusedEditorValue();
    if (datalinkControlsInitialized) captureDatalinkDraftFromControls();
    const hasUnsaved = state.dirty || hasUnappliedEditorInputs() || hasDatalinkDraftChanges();
    if (couldNotStageDraft) {
      if (!window.confirm("Some current editor fields are invalid or unfinished. Discard them and all unsaved changes, then reload from disk?")) return;
    } else if (hasUnsaved && !window.confirm("Discard unsaved changes and reload configuration from disk?")) {
      return;
    }
    cancelAutosave();
    discardDatalinkDraftOnReload = true;
    pending.reload = postBridge("state.reload", {});
    if (!pending.reload) {
      discardDatalinkDraftOnReload = false;
      return;
    }
    armPendingTimeout("reload", pending.reload);
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

  function continueGlobalSaveAfterDatalink() {
    if (!globalSaveAfterDatalink || datalinkPending.settings) return;
    globalSaveAfterDatalink = false;
    if (hasDatalinkDraftChanges()) {
      updateDirtyState();
      setStatus("Datalink settings changed while saving; saving the latest values…", "info");
      scheduleAutosave();
      return;
    }
    if (state.dirty) {
      startConfigurationSave();
      return;
    }
    updateDirtyState();
    setStatus("Datalink settings saved");
  }

  function saveAll({ automatic = false } = {}) {
    if (automatic) {
      const active = document.activeElement;
      if (!stageEditorControl(active) || (active?.checkValidity && !active.checkValidity())) return false;
      if (hasUnappliedEditorInputs()) return false;
    } else if (!stageFocusedEditorValue()) return false;
	flushDeferredHistoryGesture();
    if (datalinkControlsInitialized) captureDatalinkDraftFromControls();
    const dirty = datalinkDirtyParts();
    if (dirty.connection && datalinkDraft?.password) datalinkPasswordCommitReady = true;
    if (dirty.connection && !datalinkDraft?.logonCallsign) {
      if (automatic) return false;
      setPage("settings");
      $("#datalinkLogonCallsign")?.classList.add("invalid");
      $("#datalinkLogonCallsign")?.focus();
      showToast("Enter a CPDLC logon callsign before saving", "error");
      return false;
    }
    if (datalinkPending.settings || globalSaveAfterDatalink) return false;
    if (dirty.connection || dirty.cdm) {
      globalSaveAfterDatalink = true;
      const submitted = submitDatalinkSettings({
        includeConnection: dirty.connection,
        includeCdm: dirty.cdm,
        cdmAutoEnabled: Boolean(state.datalink.cdmAutoEnabled),
        kind: "global-save",
        silent: true
      });
      if (!submitted) globalSaveAfterDatalink = false;
      updateDirtyState();
      if (submitted) setStatus("Saving datalink settings...", "info");
      return submitted;
    }
    if (!state.dirty) return true;
    return startConfigurationSave();
  }

  function restoreProfilesBackup() {
	if (pending.reload || pending.save || pending.resource || runtimeCommandPending.size || splitAvisoContext || !state.settings?.dataHealth?.profilesBackupAvailable) return;
    if (!window.confirm("Restore vSMR_Profiles.json from its validated .bak copy? Current unsaved edits will be discarded.")) return;
    pending.reload = postBridge("state.restore.backup", {});
    if (!pending.reload) return;
    armPendingTimeout("reload", pending.reload);
    setStatus("Restoring profiles backup...", "info");
    updateCommandState();
  }

  function restoreBundledDefaults() {
	if (pending.reload || pending.save || pending.resource || runtimeCommandPending.size || splitAvisoContext) return;
    if (!window.confirm("Load the bundled profiles and, when available, the AVISO default for the active airport? Valid changes will be saved automatically.")) return;
    const id = postBridge("state.reset", {});
    if (!id) return;
    pending.resource = { id, resource: "defaults", source: "bundled defaults", kind: "defaults" };
    armPendingTimeout("resource", id);
    setStatus("Loading bundled defaults...", "info");
    updateCommandState();
  }

  function undoHistory() {
	if (!history.past.length || pending.save || pending.reload || pending.resource || runtimeCommandPending.size || splitAvisoContext || state.externalEditConflict || state.airport !== state.hostAirport) return;
	flushDeferredHistoryGesture();
	const rollback = captureRuntimeCommandRollback();
    const target = history.past.pop();
    history.future.push(history.present);
    if (history.future.length > HISTORY_LIMIT) history.future.shift();
    state.recoveryConfirmed = false;
    state.avisoRecoveryConfirmed = false;
    restoreHistorySnapshot(target);
    updateDirtyState("Undoing change…");
	if (postRuntimeCommand("state.undo", { state: serializeStatePayload() }, rollback)) scheduleAutosave();
  }

  function redoHistory() {
	if (!history.future.length || pending.save || pending.reload || pending.resource || runtimeCommandPending.size || splitAvisoContext || state.externalEditConflict || state.airport !== state.hostAirport) return;
	flushDeferredHistoryGesture();
	const rollback = captureRuntimeCommandRollback();
    const target = history.future.pop();
    history.past.push(history.present);
    if (history.past.length > HISTORY_LIMIT) history.past.shift();
    state.recoveryConfirmed = false;
    state.avisoRecoveryConfirmed = false;
    restoreHistorySnapshot(target);
    updateDirtyState("Redoing change…");
	if (postRuntimeCommand("state.redo", { state: serializeStatePayload() }, rollback)) scheduleAutosave();
  }

  function confirmDelete(message) {
    return !state.settings.confirmDelete || window.confirm(message);
  }

  function renderAll() {
    syncProfileTabSelection();
    setPage(state.ui.page);
    renderRuntimeMenu();
    syncSurfaceVisibility();
  }

  function applyQueryState() {
    const params = new URLSearchParams(window.VSMR_PREVIEW_QUERY || location.search);
    const requestedPage = params.get("page");
    const page = ["datalink", "cpdlc", "cdm"].includes(requestedPage)
      ? "datalink"
      : ["performance", "diagnostics", "updates"].includes(requestedPage) ? "settings" : requestedPage;
    const tab = params.get("tab");
    if (PAGE_TITLES[page]) state.ui.page = page;
    if (PROFILE_TITLES[tab]) state.ui.profileTab = tab;
    const avisoView = params.get("aviso") || params.get("view");
    if (["geometry", "text"].includes(avisoView)) state.ui.avisoView = avisoView;
    if (["day", "night"].includes(params.get("palette"))) state.settings.avisoColorPalette = params.get("palette");
    const ui = params.get("ui");
    if (ui === "control" || params.get("control") === "1" || PAGE_TITLES[page]) state.ui.controlCenterOpen = true;
    if (ui === "runtime") state.ui.controlCenterOpen = false;
    if (["mode", "groups", "inset", "profile"].includes(params.get("popup"))) state.ui.runtimePopover = params.get("popup");
    if (params.get("tag")) {
      state.ui.selectedTagId = params.get("tag");
      state.ui.selectedTagIds = [state.ui.selectedTagId];
      state.ui.tagSelectionAnchorId = state.ui.selectedTagId;
    }
    if (params.get("profile")) {
      const match = state.profiles.find(record => record.data.name.toLowerCase() === params.get("profile").toLowerCase());
      if (match) { state.activeProfileId = match.id; state.ui.managedProfileId = match.id; }
    }
  }

  function editorControlScope(control) {
    if (!(control instanceof HTMLInputElement || control instanceof HTMLSelectElement ||
        control instanceof HTMLTextAreaElement)) return "";
    if (control.matches(
      '[type="search"], [type="file"], ' +
      '#tagTokenSelect'
    ) || control.closest(".datalink-card, .updater-general-group, #runtimeMenu, .page-rail, dialog")) return "";

    const profilePanel = control.closest("[data-profile-panel]")?.dataset.profilePanel;
    if (["colors", "icons", "tags", "rules"].includes(profilePanel)) return profilePanel;
    if (control.closest("[data-page-panel='modes']")) return "modes";
    if (control.closest("[data-page-panel='profiles']")) return "profiles";
    if (control.closest("[data-aviso-view-panel='geometry']")) return "aviso-geometry";
    if (control.closest("[data-aviso-view-panel='text']")) return "aviso-text";
    if (control.closest("[data-page-panel='groups']")) return "groups";
    if (control.closest("[data-page-panel='alerts']")) return "alerts";
    if (control.closest(".settings-layout")) return "settings";
    return "";
  }

  function stageEditorControl(control) {
    const scope = editorControlScope(control);
    if (!scope) return true;
    const sectionKey = editorSectionKey(control);
    if (sectionKey && !unappliedEditorSections.has(sectionKey)) return true;
    if (control.checkValidity && !control.checkValidity()) return false;
    const result = withHistoryGesture(control, () => {
      if (scope === "colors") return applyColorDraft({ render: false });
      if (scope === "icons") return applyIcons({ render: false });
      if (scope === "tags") return applyTag({
        render: false,
        applyContent: Boolean(control.closest("#tagDefinitionEditor"))
      });
      if (scope === "rules") return applyRule({ render: false });
      if (scope === "modes") return applyMode({ render: false });
      if (scope === "profiles") return applyProfile({ render: false });
      if (scope === "aviso-geometry") return applyAvisoGeometry({ render: false, feedback: false });
      if (scope === "aviso-text") return applyAvisoTextStyles({ render: false, feedback: false });
      if (scope === "groups") return applyAvisoGroup({ render: false, feedback: false });
      if (scope === "alerts") return applyAlerts({ render: false, feedback: false });
      if (scope === "settings") return applySettings({ render: false });
    });
    if (result === false) return false;
    refreshEditorDerivedVisuals(scope, control);
    return true;
  }

  const deferredDerivedRefreshes = new WeakMap();

  function refreshSelectedColorRow() {
    const entry = selectedColorEntry();
    const row = entry && $$("#colorTree [data-color-path]").find(item => item.dataset.colorPath === entry.id);
    if (!entry || !row) return;
    const hex = colorToHex(entry.color).toUpperCase();
    row.style.setProperty("--node-color", hex);
    row.title = entry.name;
  }

  function performDeferredDerivedRefresh(scope) {
    if (scope === "aviso-geometry") renderAvisoGeometry();
    else if (scope === "aviso-text") renderAvisoText();
    else if (scope === "alerts") renderAlerts();
  }

  function deferDerivedRefreshUntilFocusout(scope, control) {
    if (!control || document.activeElement !== control) {
      performDeferredDerivedRefresh(scope);
      return;
    }
    let scopes = deferredDerivedRefreshes.get(control);
    if (!scopes) {
      scopes = new Set();
      deferredDerivedRefreshes.set(control, scopes);
      control.addEventListener("focusout", () => {
        const queued = deferredDerivedRefreshes.get(control) || new Set();
        deferredDerivedRefreshes.delete(control);
        window.requestAnimationFrame(() => queued.forEach(performDeferredDerivedRefresh));
      }, { once: true });
    }
    scopes.add(scope);
  }

  function refreshEditorDerivedVisuals(scope, control = null) {
    if (scope === "colors") refreshSelectedColorRow();
    else if (scope === "rules") {
      const item = rules()[state.ui.selectedRuleIndex];
      if (item) {
        $("#ruleFormCaption").textContent = ruleLabel(item, state.ui.selectedRuleIndex);
        const row = $(`[data-rule-index="${state.ui.selectedRuleIndex}"] span`);
        if (row) row.textContent = ruleLabel(item, state.ui.selectedRuleIndex);
      }
    } else if (scope === "groups") {
      const group = selectedAvisoGroup();
      if (group) {
        $("#avisoGroupCaption").textContent = group.name;
        const row = $$("#avisoGroupList [data-aviso-group-id]").find(item => item.dataset.avisoGroupId === group.id);
        const label = row && $("strong", row);
        if (label) label.textContent = group.name;
      }
    } else if (scope === "icons") renderIconSymbolPreview();
    else if (scope === "settings") renderIconSymbolPreview();
    else if (["aviso-geometry", "aviso-text", "alerts"].includes(scope))
      deferDerivedRefreshUntilFocusout(scope, control);
  }

  function bindAvisoColorEditor(prefix, draftName, scope, includeOpacity, apply) {
    const element = suffix => $("#" + prefix + suffix);
    const draft = () => drafts[draftName];
    const syncRgbOutputs = () => {
      const current = hexToColor(draft()?.hex || "#000000");
      const values = ["Red", "Green", "Blue"].map((channel, index) => {
        const value = element(channel + "Output").value.trim();
        return value === "" ? [current.r, current.g, current.b][index] : Number(value);
      });
      if (values.every(Number.isFinite)) updateAvisoColorDraftFromRgb(draft(), prefix, ...values, includeOpacity);
    };

    element("Hex").addEventListener("input", event => {
      if (/^#?[0-9a-f]{6}$/i.test(event.target.value.trim()))
        updateAvisoColorDraftFromHex(draft(), prefix, event.target.value, includeOpacity);
    });
    element("Hex").addEventListener("change", event => {
      if (/^#?[0-9a-f]{6}$/i.test(event.target.value.trim()))
        updateAvisoColorDraftFromHex(draft(), prefix, event.target.value, includeOpacity);
    });
    element("Hue").addEventListener("input", event => updateAvisoColorDraftFromHsv(draft(), prefix, Number(event.target.value), draft()?.s ?? 0, draft()?.v ?? 1, includeOpacity));
    ["Red", "Green", "Blue"].forEach(suffix => {
      element(suffix).addEventListener("input", () => updateAvisoColorDraftFromRgb(draft(), prefix, element("Red").value, element("Green").value, element("Blue").value, includeOpacity));
    });
    element("HueOutput").addEventListener("input", event => {
      if (event.target.value.trim() === "") return;
      const value = Number(event.target.value);
      if (Number.isFinite(value)) updateAvisoColorDraftFromHsv(draft(), prefix, value, draft()?.s ?? 0, draft()?.v ?? 1, includeOpacity);
    });
    ["RedOutput", "GreenOutput", "BlueOutput"].forEach(suffix => {
      element(suffix).addEventListener("input", event => {
        if (event.target.value.trim() !== "") syncRgbOutputs();
      });
    });
    if (includeOpacity) {
      const updateOpacity = value => {
        if (!draft() || !Number.isFinite(Number(value))) return;
        draft().opacity = Math.round(clamp(Number(value), 0, 100));
        draft().opacityMixed = false;
        markControlTouched(element("Opacity"));
        markControlTouched(element("OpacityOutput"));
        syncAvisoColorEditor(prefix, draft(), true);
      };
      element("Opacity").addEventListener("input", event => updateOpacity(event.target.value));
      element("OpacityOutput").addEventListener("input", event => {
        if (event.target.value.trim() !== "") updateOpacity(event.target.value);
      });
    }

    const palette = element("SvPalette");
    const updateFromPointer = event => {
      if (!draft()) return;
      const rect = palette.getBoundingClientRect();
      const saturation = clamp((event.clientX - rect.left) / Math.max(1, rect.width), 0, 1);
      const value = 1 - clamp((event.clientY - rect.top) / Math.max(1, rect.height), 0, 1);
      updateAvisoColorDraftFromHsv(draft(), prefix, draft().h, saturation, value, includeOpacity);
    };
    palette.addEventListener("pointerdown", event => {
      palette.setPointerCapture(event.pointerId);
      palette.dataset.dragging = "true";
      updateFromPointer(event);
    });
    palette.addEventListener("pointermove", event => {
      if (palette.dataset.dragging === "true") updateFromPointer(event);
    });
    const stopDrag = event => {
      const changed = palette.dataset.dragging === "true";
      palette.dataset.dragging = "false";
      if (event.pointerId != null && palette.hasPointerCapture?.(event.pointerId)) palette.releasePointerCapture(event.pointerId);
      if (!changed) return;
      withHistoryGesture(palette, () => apply({ render: false, feedback: false }));
      performDeferredDerivedRefresh(scope);
    };
    palette.addEventListener("pointerup", stopDrag);
    palette.addEventListener("pointercancel", stopDrag);
    palette.addEventListener("keydown", event => {
      if (!draft() || !["ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown"].includes(event.key)) return;
      event.preventDefault();
      const step = event.shiftKey ? .05 : .01;
      const nextS = draft().s + (event.key === "ArrowRight" ? step : event.key === "ArrowLeft" ? -step : 0);
      const nextV = draft().v + (event.key === "ArrowUp" ? step : event.key === "ArrowDown" ? -step : 0);
      updateAvisoColorDraftFromHsv(draft(), prefix, draft().h, nextS, nextV, includeOpacity);
      withHistoryGesture(palette, () => apply({ render: false, feedback: false }));
      performDeferredDerivedRefresh(scope);
    });
  }

  const scrollCueObservers = [];

  function updateScrollCues(list) {
    const shell = list?.closest(".scroll-cue-shell");
    if (!shell) return;
    if (list.clientHeight <= 0) {
      shell.classList.remove("can-scroll-up", "can-scroll-down");
      return;
    }
    const tolerance = 2;
    shell.classList.toggle("can-scroll-up", list.scrollTop > tolerance);
    shell.classList.toggle(
      "can-scroll-down",
      list.scrollTop + list.clientHeight < list.scrollHeight - tolerance
    );
  }

  function initializeScrollCues() {
    SCROLL_CUE_LIST_IDS.forEach(id => {
      const list = $("#" + id);
      if (!list || list.closest(".scroll-cue-shell")) return;
      const shell = document.createElement("div");
      shell.className = "scroll-cue-shell";
      list.parentNode.insertBefore(shell, list);
      shell.appendChild(list);
      list.classList.add("scroll-cued-list");

      const top = document.createElement("div");
      top.className = "scroll-edge-cue scroll-edge-cue-top";
      top.setAttribute("aria-hidden", "true");
      top.innerHTML = "<span>▲</span>";
      const bottom = document.createElement("div");
      bottom.className = "scroll-edge-cue scroll-edge-cue-bottom";
      bottom.setAttribute("aria-hidden", "true");
      bottom.innerHTML = "<span>▼</span>";
      shell.append(top, bottom);

      let updateFrame = 0;
      const scheduleUpdate = () => {
        if (updateFrame) return;
        updateFrame = window.requestAnimationFrame(() => {
          updateFrame = 0;
          updateScrollCues(list);
        });
      };
      list.addEventListener("scroll", scheduleUpdate, { passive: true });
      const mutationObserver = new MutationObserver(scheduleUpdate);
      mutationObserver.observe(list, { childList: true, subtree: true });
      const resizeObserver = typeof ResizeObserver === "function" ? new ResizeObserver(scheduleUpdate) : null;
      resizeObserver?.observe(list);
      resizeObserver?.observe(shell);
      scrollCueObservers.push({ mutationObserver, resizeObserver });
      scheduleUpdate();
    });
  }

  function bindEvents() {
    document.addEventListener("focusin", event => {
      if (editorControlScope(event.target) || event.target.matches?.(".color-sv-palette"))
        beginHistoryGesture(event.target);
    });
    document.addEventListener("pointerdown", event => {
      if (editorControlScope(event.target) || event.target.matches?.(".color-sv-palette"))
        beginHistoryGesture(event.target);
    }, true);
    document.addEventListener("focusout", event => {
      if (historyGestureIds.get(event.target) === deferredHistoryGesture?.key)
        flushDeferredHistoryGesture();
    });
    document.addEventListener("input", event => {
      if (editorControlScope(event.target)) markEditorSectionUnapplied(event.target);
    });
    document.addEventListener("change", event => {
      // Do not depend on a preceding `input` event. WebView/keyboard/control
      // combinations may emit only `change`; marking here guarantees ARR/DEP
      // checkboxes and every other editor control are staged before Save.
      if (editorControlScope(event.target)) markEditorSectionUnapplied(event.target);
      if (!stageEditorControl(event.target)) {
        revealInvalidEditorControl(event.target);
      }
    });

    document.addEventListener("click", event => {
      const guardedEditorAction = event.target.closest(
        "#controlWindow button[data-action], #controlWindow button[data-page], " +
        "#controlWindow button[data-profile-tab], #controlWindow [data-tree-toggle], " +
        "#controlWindow [data-color-path], #controlWindow [data-tag-id], " +
        "#controlWindow [data-rule-index], #controlWindow [data-mode-index], " +
        "#controlWindow [data-managed-profile-id], #controlWindow [data-aviso-group-id], " +
        "#controlWindow [data-aviso-geometry-style], #controlWindow [data-aviso-text-style]"
      );
      if (guardedEditorAction && hasUnappliedEditorInputs() && !stageFocusedEditorValue()) {
        event.preventDefault();
        return;
      }
      if (!event.target.closest("#ruleStatusDropdown")) setRuleStatusMenuOpen(false);
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
		const rollback = captureRuntimeCommandRollback();
		if (!switchActiveProfile(runtimeProfile.dataset.runtimeProfile)) return;
		postActiveProfileChange(rollback);
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
        if (!stageFocusedEditorValue()) return;
        state.ui.selectedAvisoGroupId = avisoGroupRow.dataset.avisoGroupId;
        drafts.avisoGroup = null;
        clearUnappliedEditorSection($("#avisoGroupName"));
        renderAvisoGroups();
        return;
      }

      const pageButton = event.target.closest("[data-page]");
      if (pageButton) { if (stageFocusedEditorValue()) setPage(pageButton.dataset.page); return; }
      const tabButton = event.target.closest("[data-profile-tab]");
      if (tabButton) { if (stageFocusedEditorValue()) setProfileTab(tabButton.dataset.profileTab); return; }
      const avisoViewButton = event.target.closest("[data-aviso-view]");
      if (avisoViewButton) { if (stageFocusedEditorValue()) { state.ui.avisoView = avisoViewButton.dataset.avisoView; renderAviso(); } return; }
      const treeToggle = event.target.closest("[data-tree-toggle]");
      if (treeToggle) {
        if (!stageFocusedEditorValue()) return;
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
      if (colorRow) { if (!stageFocusedEditorValue()) return; state.ui.selectedColorPath = colorRow.dataset.colorPath; drafts.color = null; clearUnappliedEditorSection($("#colorHex")); renderColors(); return; }
      const tagRow = event.target.closest("[data-tag-id]");
      if (tagRow) { if (!stageFocusedEditorValue()) return; selectTagDefinition(tagRow.dataset.tagId, event); return; }
      const ruleRow = event.target.closest("[data-rule-index]");
      if (ruleRow) { if (!stageFocusedEditorValue()) return; state.ui.selectedRuleIndex = Number(ruleRow.dataset.ruleIndex); drafts.rule = null; clearUnappliedEditorSection($("#ruleName")); renderRules(); return; }
      const modeRow = event.target.closest("[data-mode-index]");
      if (modeRow) { if (!stageFocusedEditorValue()) return; state.ui.selectedModeIndex = Number(modeRow.dataset.modeIndex); drafts.mode = null; clearUnappliedEditorSection($("#modeName")); renderModes(); return; }
      const profileRow = event.target.closest("[data-managed-profile-id]");
      if (profileRow) { if (!stageFocusedEditorValue()) return; state.ui.managedProfileId = profileRow.dataset.managedProfileId; drafts.profile = null; clearUnappliedEditorSection($("#profileName")); renderProfilesManager(); return; }
      const styleVisibilityToggle = event.target.closest("[data-aviso-style-visibility]");
      if (styleVisibilityToggle) {
        if (!stageFocusedEditorValue()) return;
        toggleAvisoStyleVisibility(
          styleVisibilityToggle.dataset.avisoStyleVisibility,
          styleVisibilityToggle.dataset.avisoStyleId
        );
        return;
      }
      const avisoGeometryStyleRow = event.target.closest("[data-aviso-geometry-style]");
      if (avisoGeometryStyleRow) {
        if (!stageFocusedEditorValue()) return;
        selectAvisoGeometryStyle(avisoGeometryStyleRow.dataset.avisoGeometryStyle, event);
        return;
      }
      const avisoTextStyleRow = event.target.closest("[data-aviso-text-style]");
      if (avisoTextStyleRow) {
        if (!stageFocusedEditorValue()) return;
        selectAvisoTextStyle(avisoTextStyleRow.dataset.avisoTextStyle, event);
        return;
      }
      const actionButton = event.target.closest("[data-action]");
      if (actionButton) handleAction(actionButton.dataset.action, actionButton);
    });

    const syncDatalinkDraft = (...fields) => {
      if (!datalinkDraft) resetDatalinkDraftFromRuntime();
      const before = clone(datalinkDraft);
      const after = captureDatalinkDraftFromControls();
      fields.forEach(field => {
        if (before?.[field] !== after?.[field]) datalinkFieldRevisions[field] += 1;
      });
      renderDatalink();
      scheduleAutosave();
    };
    $("#datalinkLogonCallsign").addEventListener("input", event => {
      const selection = event.target.selectionStart;
      event.target.value = event.target.value.toUpperCase().replace(/\s/g, "").slice(0, 8);
      if (selection != null) event.target.setSelectionRange(Math.min(selection, event.target.value.length), Math.min(selection, event.target.value.length));
      syncDatalinkDraft("logonCallsign");
    });
    $("#datalinkPassword").addEventListener("input", () => {
      datalinkPasswordCommitReady = false;
      syncDatalinkDraft("password");
    });
    $("#datalinkPassword").addEventListener("change", () => {
      syncDatalinkDraft("password");
      datalinkPasswordCommitReady = true;
    });
    $("#datalinkPassword").addEventListener("keydown", event => {
      if (event.key === "Enter") {
        syncDatalinkDraft("password");
        datalinkPasswordCommitReady = true;
      }
    });
    $("#datalinkPlaySound").addEventListener("change", () => {
      syncDatalinkDraft("playSound");
    });
    [["#datalinkCdmDelay", "cdmDelayMinutes"], ["#datalinkCdmCooldown", "cdmCooldownMinutes"]].forEach(([selector, field]) => {
      $(selector).addEventListener("input", () => {
        syncDatalinkDraft(field);
      });
      $(selector).addEventListener("change", event => {
        event.target.value = String(Math.round(clamp(event.target.value, 0, 1440)));
        syncDatalinkDraft(field);
      });
    });
    $("#tagLabelFontSize").addEventListener("change", event => {
      event.target.value = String(Math.round(clamp(event.target.value, 1, 5)));
    });

    $("#colorHex").addEventListener("input", event => {
      if (/^#?[0-9a-f]{6}$/i.test(event.target.value.trim())) setColorDraftFromHex(event.target.value);
    });
    $("#colorHex").addEventListener("change", event => setColorDraftFromHex(event.target.value));
    $("#colorHue").addEventListener("input", event => setColorDraftFromHsv(Number(event.target.value), drafts.color?.s ?? 0, drafts.color?.v ?? 1));
    [["colorRed", "r"], ["colorGreen", "g"], ["colorBlue", "b"]].forEach(([id]) => {
      $("#" + id).addEventListener("input", () => setColorDraftFromRgb($("#colorRed").value, $("#colorGreen").value, $("#colorBlue").value));
    });
    $("#colorOpacity").addEventListener("input", event => {
      if (!drafts.color) return;
      drafts.color.opacity = Number(event.target.value);
      syncColorEditorControls();
    });
    $("#colorHueOutput").addEventListener("input", event => {
      if (event.target.value.trim() === "") return;
      const value = Number(event.target.value);
      if (Number.isFinite(value)) setColorDraftFromHsv(value, drafts.color?.s ?? 0, drafts.color?.v ?? 1);
    });
    [["colorRedOutput", "r"], ["colorGreenOutput", "g"], ["colorBlueOutput", "b"]].forEach(([id]) => {
      $("#" + id).addEventListener("input", event => {
        if (event.target.value.trim() === "") return;
        const values = [$("#colorRedOutput").value, $("#colorGreenOutput").value, $("#colorBlueOutput").value].map(Number);
        if (values.every(Number.isFinite)) setColorDraftFromRgb(...values);
      });
    });
    $("#colorOpacityOutput").addEventListener("input", event => {
      if (!drafts.color || event.target.value.trim() === "") return;
      const value = Number(event.target.value);
      if (!Number.isFinite(value)) return;
      markEditorSectionUnapplied(event.target);
      drafts.color.opacity = clamp(value, 0, 100);
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
      const changed = colorPalette.dataset.dragging === "true";
      colorPalette.dataset.dragging = "false";
      if (event.pointerId != null && colorPalette.hasPointerCapture?.(event.pointerId)) colorPalette.releasePointerCapture(event.pointerId);
      if (changed) {
        withHistoryGesture(colorPalette, () => applyColorDraft({ render: false }));
        refreshEditorDerivedVisuals("colors");
      }
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
      withHistoryGesture(colorPalette, () => applyColorDraft({ render: false }));
      refreshEditorDerivedVisuals("colors");
    });

    $("#targetSymbolScale").addEventListener("input", event => {
      const scale = clamp(event.target.value, 0.5, 1.5);
      $("#targetSymbolScaleOutput").value = `${scale.toFixed(2)}×`;
      renderIconSymbolPreview();
    });
    ["targetTrailGroundPoints", "targetTrailAirbornePoints"].forEach(id => $("#" + id).addEventListener("input", event => {
      $("#" + id + "Output").value = String(Math.round(Number(event.target.value)));
    }));
    $("#targetTrailEnabled").addEventListener("change", updateIconDependencies);
    ["targetIconStyle"].forEach(id => $("#" + id).addEventListener("change", renderIconSymbolPreview));

    $("#tagLineGrid").addEventListener("focusin", event => { if (event.target.matches(".tag-line-input")) activeTagInput = event.target; });
    $("#tagDetailedInherits").addEventListener("change", event => {
      captureTagDraft();
      drafts.tag.data.definition_detailed_inherits_normal = $("#tagDetailedInherits").checked;
      withHistoryGesture(event.target, () => applyTag({ render: false }));
      renderTagEditor();
    });

    $("#criteriaList").addEventListener("change", event => {
      const field = event.target.dataset.field;
      if (!field || !["source", "token"].includes(field)) return;
      const row = event.target.closest(".criterion-row");
      const source = $("[data-field='source']", row);
      const token = $("[data-field='token']", row);
      const condition = $("[data-field='condition']", row);
      if (field === "source") {
        const tokens = ruleTokensForSource(source.value);
        token.innerHTML = ruleSelectOptions(tokens, tokens[0]);
      }
      condition.innerHTML = ruleSelectOptions(ruleConditionsFor(source.value, token.value), "any");
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
    $("#avisoGroupMemberSearch").addEventListener("input", event => {
      captureAvisoGroupDraft();
      state.ui.avisoGroupMemberSearch = event.target.value;
      renderAvisoGroupEditor();
    });
    $("#avisoGroupName").addEventListener("input", event => {
      event.target.setCustomValidity(event.target.value.trim() ? "" : "Enter a group name");
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
    $("#avisoGroupContentDialog").addEventListener("close", () => {
      avisoGroupContentDraft = null;
      avisoGroupContentDirty = false;
      renderAvisoGroups();
      renderRuntimeMenu();
      updateCommandState();
    });
    const avisoGroupList = $("#avisoGroupList");
    avisoGroupList.addEventListener("dragstart", event => {
      const row = event.target.closest("[data-aviso-group-id]");
      if (!row) {
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
      markDirty("AVISO groups reordered", ["aviso"]);
      renderAvisoGroups();
      renderRuntimeMenu();
    });
    avisoGroupList.addEventListener("dragend", () => {
      draggedAvisoGroupId = "";
      $$(".aviso-group-row", avisoGroupList).forEach(item => item.classList.remove("dragging", "drop-target", "drop-after"));
    });

    bindAvisoColorEditor("avisoGeometryColor", "avisoGeometry", "aviso-geometry", true, applyAvisoGeometry);
    bindAvisoColorEditor("avisoTextColor", "avisoTextStyle", "aviso-text", false, applyAvisoTextStyles);
    ["avisoTextFont", "avisoTextSize", "avisoTextHaloWidth"].forEach(id => {
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
    document.addEventListener("change", event => {
      if (event.target.matches("#runtimePresetLinked")) toggleRuntimePresetLinked(event.target.checked);
      if (event.target.matches("[data-alert-type]")) event.target.closest(".alert-toggle-card")?.classList.toggle("inactive", !event.target.checked);
    });
    $("#resourceGithubLoadConfirm").addEventListener("click", loadResourceFromGithub);
    $("#resourceGithubUrl").addEventListener("keydown", event => {
      if (event.key === "Enter") { event.preventDefault(); loadResourceFromGithub(); }
    });

    $("#avisoGeometryStyleList").addEventListener("keydown", event => {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "a") {
        event.preventDefault();
        const ids = avisoStyleEntries("geometry").map(entry => entry.id);
        if (ids.length) {
          state.ui.selectedAvisoGeometryStyleIds = ids;
          state.ui.selectedAvisoGeometryStyleId = ids[ids.length - 1];
          state.ui.avisoGeometrySelectionAnchorId = ids[0];
          clearUnappliedEditorSection($("#avisoGeometryColorHex"));
          renderAvisoGeometry();
        }
      } else if (event.key === "Escape") {
        const id = state.ui.selectedAvisoGeometryStyleId;
        state.ui.selectedAvisoGeometryStyleIds = id ? [id] : [];
        clearUnappliedEditorSection($("#avisoGeometryColorHex"));
        renderAvisoGeometry();
      }
    });
    $("#avisoTextStyleList").addEventListener("keydown", event => {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "a") {
        event.preventDefault();
        const ids = avisoStyleEntries("text").map(entry => entry.id);
        if (ids.length) {
          state.ui.selectedAvisoTextStyleIds = ids;
          state.ui.selectedAvisoTextStyleId = ids[ids.length - 1];
          state.ui.avisoTextSelectionAnchorId = ids[0];
          clearUnappliedEditorSection($("#avisoTextFont"));
          renderAvisoText();
        }
      } else if (event.key === "Escape") {
        const id = state.ui.selectedAvisoTextStyleId;
        state.ui.selectedAvisoTextStyleIds = id ? [id] : [];
        clearUnappliedEditorSection($("#avisoTextFont"));
        renderAvisoText();
      }
    });
    $("#tagDefinitionList").addEventListener("keydown", event => {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "a") {
        event.preventDefault();
        const ids = tagDefinitions().map(entry => entry.id);
        if (ids.length) {
          state.ui.selectedTagIds = ids;
          state.ui.selectedTagId = ids[ids.length - 1];
          state.ui.tagSelectionAnchorId = ids[0];
          drafts.tag = null;
          clearUnappliedEditorSection($("#tagDefinitionEditor"));
          renderTags();
        }
      } else if (event.key === "Escape") {
        const id = state.ui.selectedTagId;
        state.ui.selectedTagIds = id ? [id] : [];
        drafts.tag = null;
        clearUnappliedEditorSection($("#tagDefinitionEditor"));
        renderTags();
      }
    });

    $("#updateChannel").addEventListener("change", event => {
      submitUpdateSettings({ channel: event.target.value === "stable" ? "stable" : "beta" }, "Update channel saved");
    });
    $("#updateAutoCheck").addEventListener("change", event => {
      submitUpdateSettings({ auto_check: event.target.checked }, "Automatic update checks saved");
    });
    $("#updateAutoDownload").addEventListener("change", event => {
      submitUpdateSettings({ auto_download: event.target.checked }, "Automatic download preference saved");
    });
    $("#updateAutoInstall").addEventListener("change", event => {
      submitUpdateSettings({ auto_install: event.target.checked }, "Automatic activation preference saved");
    });
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
    if (action === "open-control-center") openControlCenter();
    else if (action === "open-settings") { openControlCenter(); setPage("settings"); }
    else if (action === "set-aviso-palette") {
      const palette = button.dataset.avisoColorPalette === "day" ? "day" : "night";
      if (state.settings.avisoColorPalette !== palette) {
        state.settings.avisoColorPalette = palette;
        renderSettings();
        renderAviso();
        markDirty(`AVISO ${palette} palette selected`, ["settings"]);
      }
    }
    else if (action === "dismiss-persistent-status") {
      if (persistentStatusState)
        dismissedPersistentStatusKey = `${persistentStatusState.type}|${persistentStatusState.message}`;
      renderPersistentStatus();
    }
    else if (action === "restore-profiles-backup") restoreProfilesBackup();
    else if (action === "restore-bundled-defaults") restoreBundledDefaults();
    else if (action === "update-check") requestUpdateAction("check_now");
    else if (action === "update-retry") requestUpdateAction("retry_update");
    else if (action === "update-release-open") openUpdateRelease();
    else if (action === "update-skip") {
      const version = String(updateCenter.state?.available_version || "");
      if (version) submitUpdateSettings({ skipped_version: version }, `${version} skipped`);
    }
    else if (action === "update-clear-skip") submitUpdateSettings({ skipped_version: "" }, "Skipped update cleared");
    else if (action === "assign-legacy-inset-presets") assignLegacyInsetPresetsToActiveAirport();
    else if (action === "close-runtime-popover") { state.ui.runtimePopover = ""; renderRuntimeMenu(); }
    else if (action === "save-inset-preset") openInsetPresetDialog("capture");
    else if (action === "rename-inset-preset") openInsetPresetDialog("rename");
    else if (action === "capture-inset-preset") confirmInsetPresetDialog();
    else if (action === "update-inset-preset") updateAvisoPreset();
    else if (action === "reset-inset-preset") resetAvisoPreset();
    else if (action === "duplicate-inset-preset") duplicateAvisoPreset();
    else if (action === "default-inset-preset") setDefaultAvisoPreset();
    else if (action === "delete-inset-preset") deleteAvisoPreset();
    else if (action === "select-aviso-text-color") {
      const target = button.dataset.colorTarget === "halo" ? "halo" : "text";
      if (target !== state.ui.avisoTextColorTarget) {
        state.ui.avisoTextColorTarget = target;
        drafts.avisoTextStyle = null;
        renderAvisoTextEditor();
      }
    }
    else if (action === "insert-tag-token") insertTagToken();
    else if (action === "new-rule") createRule();
    else if (action === "duplicate-rule") duplicateRule();
    else if (action === "delete-rule") deleteRule();
    else if (action === "add-condition") { captureRuleDraft(); drafts.rule.data.criteria.push({ source: "vacdm", token: "", condition: "" }); renderRuleEditor(); applyRule({ render: false }); }
    else if (action === "delete-condition") deleteRuleCondition(Number(button.dataset.index));
    else if (action === "new-mode") createMode();
    else if (action === "duplicate-mode") duplicateMode();
    else if (action === "delete-mode") deleteMode();
    else if (action === "activate-mode") activateMode();
    else if (action === "mode-statuses-all") setModeStatusVisibility(true);
    else if (action === "mode-statuses-none") setModeStatusVisibility(false);
    else if (action === "new-profile") createProfile();
    else if (action === "duplicate-profile") duplicateProfile();
    else if (action === "delete-profile") deleteProfile();
    else if (action === "activate-profile") {
      const record = managedProfileRecord();
      if (record) {
		const rollback = captureRuntimeCommandRollback();
		if (!switchActiveProfile(record.id)) return;
		postActiveProfileChange(rollback);
      }
    }
    else if (action === "load-profiles-computer") {
	  if (!confirmResourceReplacement("profiles")) return;
      if (HOST_MODE) {
        postBridge("resource.computer.load", { resource: "profiles" });
        setStatus("Choose a profiles file…", "info");
      } else $("#profilesFileInput").click();
    }
    else if (action === "load-aviso-computer") {
	  if (!confirmResourceReplacement("aviso")) return;
      if (HOST_MODE) {
        postBridge("resource.computer.load", { resource: "aviso" });
        setStatus("Choose an AVISO GeoJSON file…", "info");
      } else $("#avisoFileInput").click();
    }
    else if (action === "load-profiles-github") {
	  openResourceGithubDialog("profiles");
	}
    else if (action === "load-aviso-github") {
	  openResourceGithubDialog("aviso");
	}
    else if (action === "new-aviso-group") createAvisoGroup();
    else if (action === "duplicate-aviso-group") duplicateAvisoGroup();
    else if (action === "delete-aviso-group") deleteAvisoGroup();
    else if (action === "clear-aviso-group") clearSelectedAvisoGroup();
    else if (action === "open-aviso-group-content") openAvisoGroupContentDialog();
    else if (action === "select-filtered-group-content") setFilteredAvisoGroupContent(true);
    else if (action === "clear-filtered-group-content") setFilteredAvisoGroupContent(false);
    else if (action === "toggle-aviso-group-visibility") toggleRuntimeGroup(button.dataset.groupId);
    else if (action === "remove-aviso-group-member") removeAvisoGroupMember(button);
    else if (action === "alert-runways-all-arr") setAllAlertRunwayField("arrival", true);
    else if (action === "alert-runways-all-dep") setAllAlertRunwayField("departure", true);
    else if (action === "alert-runways-open-all") setAllAlertRunwayField("closed", false);
    else if (action === "new-alert-runway") addAlertRunway();
    else if (action === "remove-alert-runway") removeAlertRunway(Number(button.dataset.index));
    else if (action === "toggle-datalink-password") { datalinkPasswordVisible = !datalinkPasswordVisible; renderDatalink(); $("#datalinkPassword").focus(); }
    else if (action === "datalink-toggle-connection") toggleDatalinkConnection();
    else if (action === "datalink-poll") pollDatalinkNow();
    else if (action === "datalink-scan") scanCdmReminders();
    else if (action === "datalink-reminder-toggle") togglePdcReminders();
    else if (action.startsWith("browse-")) { postBridge(action.replaceAll("-", ".")); showToast("Native file picker requested"); }

  }

  function insertTagToken() {
    if (!activeTagInput || !document.body.contains(activeTagInput)) activeTagInput = $("#tagLineGrid .tag-line-input:not(:disabled)");
    if (!activeTagInput) return;
    const token = $("#tagTokenSelect").value;
    const start = activeTagInput.selectionStart ?? activeTagInput.value.length;
    const end = activeTagInput.selectionEnd ?? start;
    const prefix = start > 0 && !/\s$/.test(activeTagInput.value.slice(0, start)) ? " " : "";
    activeTagInput.value = activeTagInput.value.slice(0, start) + prefix + token + " " + activeTagInput.value.slice(end);
    applyTag({ render: false });
    activeTagInput.focus();
  }

  function createRule() {
    rules().push({ source: "vacdm", token: "tsat", condition: "valid", criteria: [{ source: "vacdm", token: "tsat", condition: "valid" }], tag_type: "departure", status: "any", statuses: RULE_STATUSES.slice(), detail: "normal", text_color: hexToColor("#ffffff") });
    state.ui.selectedRuleIndex = rules().length - 1;
    drafts.rule = null;
    clearUnappliedEditorSection($("#ruleName"));
    markDirty("Rule created", ["profiles"]);
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
    clearUnappliedEditorSection($("#ruleName"));
    markDirty("Rule copied", ["profiles"]);
    renderRules();
  }
  function deleteRule() {
    if (!rules().length || !confirmDelete("Delete this rule?")) return;
    rules().splice(state.ui.selectedRuleIndex, 1);
    state.ui.selectedRuleIndex = Math.max(0, state.ui.selectedRuleIndex - 1);
    drafts.rule = null;
    clearUnappliedEditorSection($("#ruleName"));
    markDirty("Rule deleted", ["profiles"]);
    renderRules();
  }
  function deleteRuleCondition(index) {
    captureRuleDraft();
    if (!drafts.rule) return;
    drafts.rule.data.criteria.splice(index, 1);
    if (!drafts.rule.data.criteria.length) drafts.rule.data.criteria.push({ source: "vacdm", token: "", condition: "" });
    renderRuleEditor();
    applyRule({ render: false });
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
    clearUnappliedEditorSection($("#modeName"));
    markDirty("Mode created", ["profiles"]);
    renderModes();
    renderRuntimeMenu();
  }
  function duplicateMode() {
    const item = modes()[state.ui.selectedModeIndex];
    if (!item) return;
    const copy = clone(item); copy.name = `${item.name} copy`;
    modes().splice(state.ui.selectedModeIndex + 1, 0, copy);
    state.ui.selectedModeIndex += 1;
    drafts.mode = null;
    clearUnappliedEditorSection($("#modeName"));
    markDirty("Mode copied", ["profiles"]); renderModes(); renderRuntimeMenu();
  }
  function deleteMode() {
    const items = modes();
    if (items.length <= 1 || !confirmDelete("Delete this display mode?")) return;
    const deleted = items[state.ui.selectedModeIndex];
    items.splice(state.ui.selectedModeIndex, 1);
    state.ui.selectedModeIndex = Math.max(0, state.ui.selectedModeIndex - 1);
    if (activeProfile().filters.display_modes.active === deleted.name) activeProfile().filters.display_modes.active = items[0].name;
    drafts.mode = null;
    clearUnappliedEditorSection($("#modeName"));
    markDirty("Mode deleted", ["profiles"]); renderModes(); renderRuntimeMenu();
  }
  function activateMode() {
    const mode = modes()[state.ui.selectedModeIndex];
    if (!mode) return;
	if (mode.name === activeProfile()?.filters?.display_modes?.active) return;
	if (state.dirty || hasUnappliedEditorInputs() || avisoGroupContentDirty) {
	  showToast("Wait for automatic saving or revert current edits before changing mode", "error");
	  return;
	}
	const modeEditor = $("[data-page-panel='modes']");
	const modeSectionDirty = unappliedEditorSections.has(editorSectionKey(modeEditor));
	if (modeSectionDirty &&
		!window.confirm("Discard unapplied editor fields and change display mode?")) return;
	const rollback = captureRuntimeCommandRollback();
	drafts.mode = null;
	clearUnappliedEditorSection(modeEditor);
    activeProfile().filters.display_modes.active = mode.name;
    renderModes();
    renderRuntimeMenu();
	postRuntimeCommand(
	  "runtime.mode.change",
	  { profile: activeProfile().name, mode: mode.name },
	  rollback
	);
  }

  function createProfile() {
    const base = clone(activeProfile());
    base.name = "New profile";
    const record = { id: uid("profile"), persistedName: "", data: base, original: clone(base) };
    state.profiles.push(record);
    state.ui.managedProfileId = record.id;
    drafts.profile = null;
    clearUnappliedEditorSection($("#profileName"));
    markDirty("Profile created", ["profiles"]);
    renderProfilesManager(); renderRuntimeMenu();
  }
  function duplicateProfile() {
    const source = managedProfileRecord(); if (!source) return;
    const data = clone(source.data); data.name = `${data.name} copy`;
    const record = { id: uid("profile"), persistedName: "", data, original: clone(data) };
    const index = state.profiles.indexOf(source) + 1;
    state.profiles.splice(index, 0, record);
    state.ui.managedProfileId = record.id;
    drafts.profile = null;
    clearUnappliedEditorSection($("#profileName"));
    markDirty("Profile copied", ["profiles"]); renderProfilesManager(); renderRuntimeMenu();
  }
  function deleteProfile() {
    if (state.profiles.length <= 1 || !confirmDelete("Delete this profile?")) return;
    const record = managedProfileRecord(); if (!record) return;
    const index = state.profiles.indexOf(record); state.profiles.splice(index, 1);
    if (record.id === state.activeProfileId) state.activeProfileId = state.profiles[Math.max(0, index - 1)].id;
    state.ui.managedProfileId = state.activeProfileId;
    drafts.profile = null;
    clearUnappliedEditorSection($("#profileName"));
    markDirty("Profile deleted", ["profiles", "metadata"]); renderAllProfileSections(); renderRuntimeMenu();
  }


  function normalizeGithubRawUrl(value) {
    const raw = String(value || "").trim();
    if (!raw) throw new Error("Enter a GitHub file URL");
    const url = new URL(raw);
    if (url.protocol !== "https:" || url.username || url.password || url.port)
      throw new Error("Use an HTTPS GitHub URL without credentials or a custom port");
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

  function applyProfilesPayload(parsed, source = "") {
    if (!Array.isArray(parsed)) throw new Error("Expected a vSMR profiles JSON array");
    const { records, metadata, extras } = getProfileRecords(parsed);
    if (!records.length) throw new Error("No profiles were found in this file");
    const preferred = records.find(record => record.data.name === metadata.last_active_profile) || records[0];
    migrateProfileAvisoPresetStores(metadata, records, preferred.id);
    state.profiles = records;
    state.metadata = metadata;
    state.profileExtras = clone(extras);
    state.activeProfileId = preferred.id;
    state.ui.managedProfileId = preferred.id;
    state.ui.selectedRuleIndex = 0;
    state.ui.selectedModeIndex = Math.max(0, (preferred.data.filters?.display_modes?.items || []).findIndex(mode => mode.name === preferred.data.filters?.display_modes?.active));
    state.ui.selectedTagId = "departure:taxi";
    state.ui.selectedTagIds = [state.ui.selectedTagId];
    state.ui.tagSelectionAnchorId = state.ui.selectedTagId;
    const colors = collectProfileColors(preferred.data);
    state.ui.selectedColorPath = colors[0]?.id || "";
    state.settings.resolutionPreset = preferred.data.targets?.small_icon_boost_resolution_preset || state.settings.resolutionPreset || "1080p";
    Object.keys(drafts).forEach(key => drafts[key] = null);
    clearAllUnappliedEditorSections();
    avisoGroupContentDirty = false;
    if (source) setResourceSource("profiles", source);
    renderAllProfileSections();
    renderSettings();
    renderRuntimeMenu();
    markDirty(`${source || "Profiles"} loaded`, ["profiles", "metadata", "profileExtras", "settings"]);
  }


  function applyAvisoPayload(parsed, source = "") {
    if (parsed?.type !== "FeatureCollection" || !Array.isArray(parsed.features)) throw new Error("Expected a GeoJSON FeatureCollection");
    state.aviso = normalizeAvisoData(parsed);
    clearAllUnappliedEditorSections();
    avisoGroupContentDirty = false;
    resetAvisoSelections();
    if (source) setResourceSource("aviso", source);
    renderAviso();
    renderRuntimeMenu();
    markDirty(`${source || "AVISO GeoJSON"} loaded`, ["aviso"]);
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
      if (pending.resource) return;
      const resource = githubResourceType;
	  if (!confirmResourceReplacement(resource)) return;
      const id = postBridge("resource.github.load", { resource, url });
      if (!id) return;
      pending.resource = { id, resource, source: sourceUrl, kind: "github" };
      armPendingTimeout("resource", id);
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

  function applyAuthoritativeState(payload, reason = "update", trustedRuntimeResponse = false) {
    const incoming = authoritativePayload(payload);
    const preservedUi = state.ui;
    const previousProfileName = activeProfile().name || "";
    const previousHostAirport = normalizeAirportCode(state.hostAirport);
    const resourceSourceChanged = reason === "resource-source";
	const hasUnappliedEditorWork =
	  (hasUnappliedEditorInputs() || avisoGroupContentDirty) && !trustedRuntimeResponse;
    const preservesNewerSaveEdits = Boolean(
      reason === "save" && saveInFlightSnapshot &&
      (!snapshotsEqual(captureHistorySnapshot(), saveInFlightSnapshot) ||
        hasUnappliedEditorWork || hasDatalinkDraftChanges())
    );
    const externallyChangedDirtyEditors =
	  (state.dirty || hasUnappliedEditorWork) &&
	  ["resource-source", "external-save", "backup-restored", "profile", "mode"].includes(reason);
    const incomingAirport = normalizeAirportCode(
      typeof incoming.airport === "string" ? incoming.airport : state.hostAirport
    );
	const preservesStagedEditors = preservesNewerSaveEdits || ((state.dirty || hasUnappliedEditorWork) &&
      !["initial", "reload", "save", "undo", "redo", "state.undo", "state.redo"].includes(reason));
    const hostAirportChanged = !preservesStagedEditors && typeof incoming.airport === "string" &&
      incomingAirport !== previousHostAirport;
    const profileModeReplacement = !preservesStagedEditors &&
      ["profile", "mode"].includes(reason) && Array.isArray(incoming.profiles);
    const resetsExternalHistory = !preservesStagedEditors && (
      ["external-save", "backup-restored"].includes(reason) ||
      profileModeReplacement || hostAirportChanged
    );
    let avisoChanged = false;

    // A background sync must not bless an older staged editor snapshot with a
    // newer disk revision.  Keeping its old token makes the next save fail
    // closed and directs the user to reload instead of overwriting another
    // Control Center's changes.
    if ((!preservesStagedEditors || reason === "preset" || reason === "save") && typeof incoming.configRevision === "string")
      state.configRevision = incoming.configRevision;
    if ((!preservesStagedEditors || reason === "save") && typeof incoming.avisoRevision === "string")
      state.avisoRevision = incoming.avisoRevision;
    if (["initial", "reload", "save", "backup-restored", "resource-source", "undo", "redo", "state.undo", "state.redo"].includes(reason))
      state.recoveryConfirmed = false;
    if (["initial", "reload", "save", "backup-restored", "resource-source", "undo", "redo", "state.undo", "state.redo"].includes(reason))
      state.avisoRecoveryConfirmed = false;
    if (!preservesStagedEditors)
      state.externalEditConflict = false;
    else if (externallyChangedDirtyEditors)
      state.externalEditConflict = true;
    if (["initial", "reload", "save", "backup-restored"].includes(reason) &&
      persistentStatusState?.origin === "native")
      setPersistentStatus("", "", [], "native");

    if (Array.isArray(incoming.profiles)) {
      const normalized = getProfileRecords(incoming.profiles);
      if (normalized.records.length) {
        const requestedProfile = String(incoming.activeProfile || incoming.active_profile || incoming.profile || "");
        migrateProfileAvisoPresetStores(
          normalized.metadata,
          normalized.records,
          requestedProfile || previousProfileName
        );
        if (preservesStagedEditors) {
          assignAvisoPresetRoot(state.metadata, normalized.metadata.aviso_presets);
          rebasePresetStoreSnapshots(normalized.metadata.aviso_presets);
          if (reason === "mode") rebaseDisplayModeSelections(normalized.records);
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
      } else if (!preservesStagedEditors && incoming.settings?.dataHealth?.profilesHealthy === false) {
        state.profiles = [];
        state.metadata = normalized.metadata;
        state.profileExtras = clone(normalized.extras);
        state.activeProfileId = "";
      }
    }
    if (!preservesStagedEditors && incoming.aviso?.type === "FeatureCollection") {
      state.aviso = normalizeAvisoData(incoming.aviso);
      avisoChanged = true;
    }
    if (incoming.settings && typeof incoming.settings === "object" && !Array.isArray(incoming.settings)) {
      if (!preservesStagedEditors) {
        state.settings = { ...state.settings, ...clone(incoming.settings) };
      } else {
        ["profileFile", "avisoFile"].forEach(key => {
          if (typeof incoming.settings[key] === "string") state.settings[key] = incoming.settings[key];
        });
      }
    }
    if (incoming.datalink && typeof incoming.datalink === "object" && !Array.isArray(incoming.datalink)) {
      state.datalink = normalizeDatalinkRuntimeState(incoming.datalink, state.datalink);
      if (!(preservesNewerSaveEdits && hasDatalinkDraftChanges()))
        rebaseDatalinkDraftFromRuntime();
    }
    if (reason === "reload" && discardDatalinkDraftOnReload) {
      resetDatalinkDraftFromRuntime();
      discardDatalinkDraftOnReload = false;
    }
    if (typeof incoming.airport === "string") {
      state.hostAirport = incomingAirport;
      if (!preservesStagedEditors)
        state.airport = state.hostAirport;
    }
    if (incoming.runtime && typeof incoming.runtime === "object") {
      state.runtime = { ...state.runtime, ...clone(incoming.runtime) };
      const incomingInsets = { ...(state.runtime.insets || {}) };
      state.runtime.insets = {
        aviso: Boolean(incomingInsets.aviso),
        srw1: Boolean(incomingInsets.srw1),
        weather: Boolean(incomingInsets.weather),
        timer: Boolean(incomingInsets.timer)
      };
      if (Object.hasOwn(incoming.runtime, "activeAvisoPreset")) {
        state.runtime.activeAvisoPresetScope = activePresetScope();
        const activePreset = airportAvisoPresetStore().items.find(item =>
          item.name === state.runtime.activeAvisoPreset
        );
        state.runtime.avisoInsetSnapshot = activePreset ? clone(activePreset) : null;
      }
    }

    state.ui = preservedUi;
    if (HOST_MODE) state.ui.controlCenterOpen = true;
    if (!state.profiles.some(record => record.id === state.activeProfileId)) state.activeProfileId = state.profiles[0]?.id || "";
    if (!state.profiles.some(record => record.id === state.ui.managedProfileId)) state.ui.managedProfileId = state.activeProfileId;
	if (!preservesStagedEditors) {
	  Object.keys(drafts).forEach(key => drafts[key] = null);
	  clearAllUnappliedEditorSections();
	  avisoGroupContentDirty = false;
	}
    if (avisoChanged && reason !== "save") resetAvisoSelections();
	// A benign runtime/preset sync must not repaint the focused editor while
	// local staged changes are waiting for automatic persistence.
	if (preservesStagedEditors) {
	  renderRuntimeMenu();
	  updateContext();
	  updateCommandState();
	} else {
	  renderAll();
	}
    renderDataHealthStatus();
    if (preservesStagedEditors && state.airport && state.hostAirport &&
      state.airport !== state.hostAirport && previousHostAirport !== state.hostAirport) {
      const message = "The active airport changed. Reload before saving or using Undo/Redo.";
      setStatus(message, "error");
      showToast(message, "error");
    }

    if (reason === "initial" || reason === "reload" || (resourceSourceChanged && !preservesStagedEditors) || resetsExternalHistory) {
      resetHistory(true);
    } else if (reason === "save") {
      history.present = captureHistorySnapshot();
      if (preservesNewerSaveEdits) {
        savedSnapshot = saveInFlightSnapshot;
        saveInFlightSnapshot = null;
        updateDirtyState("Saved previous changes; newer edits are queued…");
        scheduleAutosave();
      } else {
        saveInFlightSnapshot = null;
        markSaved(incoming.message || "Configuration saved");
      }
    } else {
      const wasDirty = state.dirty;
      history.present = captureHistorySnapshot();
      if (!wasDirty) savedSnapshot = history.present;
      updateDirtyState();
    }
    if (externallyChangedDirtyEditors) {
      const message = "Another vSMR window changed the active data. Your unsaved edits were retained; revert before editing further.";
      setStatus(message, "error");
      showToast(message, "error");
    }
  }

  function setInsetWindows(windows = {}) {
    state.runtime.insets = {
      aviso: Boolean(state.runtime.insets?.aviso),
      srw1: Boolean(state.runtime.insets?.srw1),
      weather: Boolean(state.runtime.insets?.weather),
      timer: Boolean(state.runtime.insets?.timer)
    };
    const normalizedWindows = { ...windows };
    ["aviso", "srw1", "weather", "timer"].forEach(key => {
      if (key in normalizedWindows) state.runtime.insets[key] = Boolean(normalizedWindows[key]);
    });
    state.runtime.avisoInsetVisible = state.runtime.insets.aviso;
    renderRuntimeMenu();
  }

  function setAlertsState(alerts = {}) {
    state.runtime.alerts = { ...state.runtime.alerts, ...clone(alerts) };
    drafts.alerts = null;
    if (state.ui.page === "alerts") renderAlerts();
  }

  function finishResourceRequest(message, success) {
    const pendingRequest = pending.resource;
    const matchesPending = Boolean(pendingRequest && messageMatchesRequest(message, pendingRequest.id));
    const source = String(message.payload.source || "");
    const messageResource = String(message.payload.resource || "");
    const request = matchesPending ? pendingRequest : {
      id: message.id,
      resource: message.payload.resource,
      source: source || "computer",
      kind: /^https:\/\/(?:www\.)?github\.com\//i.test(source) ||
        /^https:\/\/raw\.githubusercontent\.com\//i.test(source)
        ? "github"
        : source === "bundled defaults" ? "defaults" : "computer"
    };
    const completesRequest = !success || pendingRequest?.kind !== "defaults" || messageResource === "profiles";
    if (matchesPending && completesRequest) {
      pending.resource = null;
      setGithubRequestPending(false);
    }
    if (!success) {
      if (request.kind === "defaults") state.recoveryConfirmed = false;
      if (String(message.payload.resource || request.resource) === "aviso")
        state.avisoRecoveryConfirmed = false;
      updateCommandState();
      return true;
    }
    const resource = String(message.payload.resource || request.resource);
    const data = message.payload.data;
    const resourceSource = source || request.source;
    const effectivePath = String(message.payload.path || "");
    // Each resource has an independent optimistic-concurrency token. Loading
    // one must never bless staged edits to the other with a newer revision.
    if (resource === "profiles" && typeof message.payload.configRevision === "string")
      state.configRevision = message.payload.configRevision;
    if (resource === "aviso" && typeof message.payload.avisoRevision === "string")
      state.avisoRevision = message.payload.avisoRevision;
    if (message.payload.settings && typeof message.payload.settings === "object" && !Array.isArray(message.payload.settings))
      state.settings = { ...state.settings, ...clone(message.payload.settings) };
    try {
      if (resource === "profiles") applyProfilesPayload(data, effectivePath);
      else if (resource === "aviso") applyAvisoPayload(data, effectivePath);
      else throw new Error("Unknown resource type");
	  // A source switch is a revision boundary. Old history snapshots do not
	  // carry file paths/revisions and must never be replayed into the new file.
	  if (effectivePath) resetHistory(false);
      if (resource === "profiles" && resourceSource === "bundled defaults")
        state.recoveryConfirmed = true;
      if (resource === "aviso") state.avisoRecoveryConfirmed = true;
      if (matchesPending && persistentStatusState?.origin === "native")
        setPersistentStatus("", "", [], "native");
      renderDataHealthStatus();
      const dialog = $("#resourceGithubDialog");
      if (dialog.open && (!pendingRequest || matchesPending)) {
        if (typeof dialog.close === "function") dialog.close(); else dialog.removeAttribute("open");
      }
      const sourceLabel = request.kind === "defaults" || resourceSource === "bundled defaults"
        ? "bundled defaults"
        : request.kind === "computer" || resourceSource === "computer"
          ? "computer"
          : "GitHub";
      showToast(`${resource === "aviso" ? "GeoJSON" : "Profiles"} loaded from ${sourceLabel}`, "success");
      updateCommandState();
    } catch (error) {
      if (request.kind === "defaults") state.recoveryConfirmed = false;
      if (resource === "aviso") state.avisoRecoveryConfirmed = false;
      updateCommandState();
      setStatus(error.message || "The loaded resource is invalid", "error");
      showToast(error.message || "The loaded resource is invalid", "error");
    }
    return true;
  }

  function receiveHostMessage(input) {
    const message = decodeHostMessage(input);
    if (!message) return;
    const payload = message.payload;
    const responseId = String(message.requestId || message.id || payload.requestId || "");
    if (responseId && expiredRequestIds.has(responseId)) return;

    if (message.type === "state.initial" || message.type === "initial.state") {
      applyAuthoritativeState(payload, "initial");
	  if (payload.aviso?.type === "FeatureCollection") setHostAuthoritativeReady(true);
      setStatus(payload.message || "Configuration loaded");
      return;
    }
    if (message.type === "state.authoritative") {
	  const hasInlineAviso = payload.aviso?.type === "FeatureCollection";
	  const avisoFollows = !hasInlineAviso && payload.avisoFollows !== false;
	  const runtimeCommandInfo = !avisoFollows
		? finishRuntimeCommand(responseId, false)
		: pendingRuntimeCommandInfo(responseId);
      const isReload = Boolean(pending.reload && messageMatchesRequest(message, pending.reload));
      const isSave = Boolean(pending.save && messageMatchesRequest(message, pending.save));
	  if (isReload && hasInlineAviso) pending.reload = "";
      const reason = isReload ? "reload" : String(payload.reason || "update");
	  if (HOST_MODE && reason === "initial") {
		setHostAuthoritativeReady(false);
		initialAuthoritativeMessageId = String(message.id || responseId || "");
      }
      const trustedRuntimeResponse = Boolean(runtimeCommandInfo?.trustedCleanResponse);
	  if (avisoFollows) {
		stageSplitAvisoContext({
		  id: message.id,
		  payload: clone(payload),
		  reason,
		  trustedRuntimeResponse,
		  completesSave: isSave,
		  completesReload: isReload,
		  runtimeRequestId: runtimeCommandInfo ? responseId : ""
		});
		return;
	  }
	  clearSplitAvisoContext(responseId);
      applyAuthoritativeState(
		payload,
		reason,
		trustedRuntimeResponse
	  );
	  if (isSave && !avisoFollows) {
		pending.save = "";
	  }
	  if (hasInlineAviso && runtimeCommandInfo?.type === "state.undo") showToast("Undone", "success");
	  if (hasInlineAviso && runtimeCommandInfo?.type === "state.redo") showToast("Redone", "success");
      if (isReload && hasInlineAviso) showToast("Configuration reloaded", "success");
      updateCommandState();
      return;
    }
    if (message.type === "state.aviso" || message.type === "aviso") {
	  if (!message.id && ignoreNextUncorrelatedAviso) {
		ignoreNextUncorrelatedAviso = false;
		return;
	  }
      const aviso = payload.aviso || payload.data || (payload.type === "FeatureCollection" ? payload : null);
      if (aviso?.type === "FeatureCollection") {
		const context = clearSplitAvisoContext(message.id);
		const reason = context ? context.reason : state.dirty ? "update" : "initial";
		const trustedRuntimeResponse = Boolean(context?.trustedRuntimeResponse);
		const completesSave = Boolean(context?.completesSave);
		const completesReload = Boolean(context?.completesReload);
		const runtimeRequestId = String(context?.runtimeRequestId || "");
		const authoritativeState = context
		  ? { ...context.payload, aviso }
		  : { aviso };
		applyAuthoritativeState(authoritativeState, reason, trustedRuntimeResponse);
		if (completesSave) {
		  pending.save = "";
		}
		if (completesReload) {
		  pending.reload = "";
		  showToast("Configuration reloaded", "success");
		}
		const completedRuntimeCommand = runtimeRequestId
		  ? finishRuntimeCommand(runtimeRequestId, false)
		  : null;
		if (completedRuntimeCommand?.type === "state.undo") showToast("Undone", "success");
		if (completedRuntimeCommand?.type === "state.redo") showToast("Redone", "success");
		updateCommandState();
		if (HOST_MODE && reason === "initial" &&
			(!initialAuthoritativeMessageId || !message.id ||
			 initialAuthoritativeMessageId === message.id)) {
			initialAuthoritativeMessageId = "";
			setHostAuthoritativeReady(true);
		}
      }
      return;
    }
    if (message.type === "update.state") {
      applyUpdateState(payload, message);
      return;
    }
    if (message.type === "datalink.state") {
      const incomingDatalink = payload.datalink || payload.state?.datalink || payload.state || payload;
      if (datalinkPending.connection?.id && messageMatchesRequest(message, datalinkPending.connection.id)) {
        datalinkPending.connection = null;
      }
      if (datalinkPending.poll?.id && messageMatchesRequest(message, datalinkPending.poll.id)) {
        datalinkPending.poll = null;
      }
      if (incomingDatalink && typeof incomingDatalink === "object" && !Array.isArray(incomingDatalink)) {
        applyDatalinkRuntimeState(incomingDatalink);
      }
      if (payload.message) {
        state.datalink.statusMessage = String(payload.message);
        if (state.ui.page === "datalink") renderDatalink();
      }
      return;
    }
    if (message.type === "state.ack") {
      if (finishUpdateAck(message)) return;
      if (finishDatalinkAck(message)) return;
      return;
    }
    if (message.type === "state.saved" || message.type === "saved") {
      if (pending.save && !messageMatchesRequest(message, pending.save)) return;
      const savedState = authoritativePayload(payload);
      // Native sends this as a durable-write acknowledgement immediately
      // before the authoritative state and split AVISO payload. The submitted
      // snapshot remains in flight, but the editor stays available.
      const completeSavedState = Boolean(
        savedState.profiles || savedState.aviso || savedState.runtime
      );
      if (completeSavedState) {
        pending.save = "";
        applyAuthoritativeState(savedState, "save");
        updateCommandState();
      } else {
        setStatus("Saved; synchronizing vSMR data...", "info");
        updateCommandState();
      }
      return;
    }
    if (message.type === "resource.loaded") {
      finishResourceRequest(message, true);
      return;
    }
    if (message.type === "resource.error") {
      finishResourceRequest(message, false);
      const text = payload.message || "Could not load the resource";
      setStatus(text, "error");
      showToast(text, "error");
      return;
    }
    if (message.type === "state.error" || message.type === "error") {
      if (finishUpdateError(message)) return;
      if (finishDatalinkError(message)) return;
	  finishRuntimeCommand(responseId, true);
	  clearSplitAvisoContext(responseId);
      if (pending.save && messageMatchesRequest(message, pending.save)) {
        pending.save = "";
        saveInFlightSnapshot = null;
      }
      if (pending.reload && messageMatchesRequest(message, pending.reload)) pending.reload = "";
      if (pending.resource && messageMatchesRequest(message, pending.resource.id)) {
        pending.resource = null;
        setGithubRequestPending(false);
      }
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
    open(page = state.ui.page, avisoView = "") { openControlCenter(page, avisoView); },
    close() { closeControlCenter(); },
    setGroupVisibility(groupId, visible) {
      receiveHostMessage({ type: "aviso.group.visibility", payload: { id: groupId, visible } });
    },
    setInsetVisible(visible) { setInsetWindows({ aviso: visible }); },
    setInsetWindows,
    setInsetState(snapshot) {
      if (snapshot && typeof snapshot === "object") state.runtime.avisoInsetSnapshot = clone(snapshot);
    },
    setDatalinkState(datalink) { applyDatalinkRuntimeState(datalink); },
    setUpdateState(payload) { applyUpdateState(payload); },
    setAlertsState,
    getState: serializeStatePayload
  };

  if (window.chrome?.webview?.addEventListener) {
    window.chrome.webview.addEventListener("message", event => receiveHostMessage(event.data));
  }

  applyQueryState();
  initializeScrollCues();
  bindEvents();
  renderAll();
  resetHistory(true);
  setHostAuthoritativeReady(!HOST_MODE);
  window.setInterval(() => requestDatalinkState(), 1250);
  window.setInterval(() => requestUpdateState(), 1500);
  document.addEventListener("visibilitychange", () => {
    if (!document.hidden && state.ui.page === "datalink") requestDatalinkState(true);
    if (!document.hidden && updateViewActive()) requestUpdateState(true);
  });
  setStatus(HOST_MODE ? "Waiting for configuration…" : "Bundled LFPG preview loaded");
  postBridge("ui.ready", {
    hostMode: HOST_MODE,
    protocolVersion: PROTOCOL_VERSION,
    capabilities: ["state", "save", "reload", "undo", "redo", "github-import", "computer-import", "window-actions", "split-aviso", "datalink", "startup-updates"]
  });
})();
