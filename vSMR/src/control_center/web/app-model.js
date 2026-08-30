"use strict";

// These ordered classic scripts share one lexical environment. Keep the
// index.html load order stable; app.js performs the final host bootstrap.

  const PROTOCOL_VERSION = 1;
  const MAX_BRIDGE_MESSAGE_BYTES = 28 * 1024 * 1024;
  const REQUEST_TIMEOUT_MS = 45000;
  const AUTOSAVE_DEBOUNCE_MS = 120;
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
  // Airborne is retained in the profile schema only as an old-profile migration
  // source. Runtime airborne definitions are owned by departure/arrival statuses.
  const TAG_EDITOR_SCOPES = ["departure", "arrival", "uncorrelated"];
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
  const TAG_TOKENS = ["callsign", "actype", "sctype", "wake", "deprwy", "gs", "flightlevel", "tendency", "scratchpad", "holdingpoint", "remark", "asid", "uk_stand", "sqerror", "groundstatus", "systemid"];
  const RULE_SOURCES = ["vacdm", "runway", "custom"];
  const RULE_SOURCE_LABELS = { vacdm: "VACDM", runway: "Runway", custom: "SID / custom" };
  const RULE_SOURCE_TOKENS = {
    vacdm: ["tobt", "tsat", "ttot", "asat", "aobt", "atot", "asrt", "aort", "ctot"],
    runway: ["deprwy", "seprwy", "arvrwy", "srvrwy"],
    custom: ["asid", "ssid"]
  };
  const AVISO_BACKGROUND_STYLE_ID = "__aviso_background__";
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
    if (!aviso.metadata || typeof aviso.metadata !== "object" || Array.isArray(aviso.metadata)) aviso.metadata = {};
    const sourceBackgroundColors = aviso.metadata.background_colors;
    const nightBackground = normalizeHex(sourceBackgroundColors?.night, "#434A4F").toUpperCase();
    aviso.metadata.background_colors = {
      night: nightBackground,
      day: normalizeHex(sourceBackgroundColors?.day, nightBackground).toUpperCase()
    };
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

  function stripObsoleteProfileSettings(profile) {
    const labels = profile?.labels;
    if (labels && typeof labels === "object") {
      delete labels.leader_line_length;
      delete labels.use_speed_for_gate;
      delete labels.use_aspeed_for_gate;
      delete labels.use_departure_arrival_coloring;
      if (labels.airborne && typeof labels.airborne === "object")
        delete labels.airborne.use_departure_arrival_coloring;
    }

    const rimcas = profile?.rimcas;
    if (rimcas && typeof rimcas === "object") {
      delete rimcas.stage_two_speed_threshold_kt;
      delete rimcas.rimcas_stage_two_speed_threshold;
    }

    const targets = profile?.targets;
    if (targets && typeof targets === "object") {
      [
        "fixed_pixel_icon_size",
        "fixed_pixel_icon_scale",
        "fixed_pixel_triangle_scale",
        "show_primary_target",
        "small_icon_boost",
        "small_icon_boost_factor"
      ].forEach(key => delete targets[key]);
    }

    const modes = profile?.filters?.display_modes?.items;
    if (Array.isArray(modes)) modes.forEach(mode => {
      if (!mode || typeof mode !== "object") return;
      delete mode.blocked_auto_correlate_squawks;
      delete mode.do_not_autocorrelate_squawks;
    });
    const legacyProMode = profile?.filters?.pro_mode;
    if (legacyProMode && typeof legacyProMode === "object") {
      delete legacyProMode.blocked_auto_correlate_squawks;
      delete legacyProMode.do_not_autocorrelate_squawks;
    }
    return profile;
  }

  function getProfileRecords(sourceProfiles = DATA.profiles) {
    const records = [];
    const extras = [];
    let metadata = { schema_version: 1, last_active_profile: "", vacdm: { server_url: "https://cdm.vatsim.fr" } };
    (Array.isArray(sourceProfiles) ? sourceProfiles : []).forEach((entry, index) => {
      if (entry && typeof entry === "object" && entry.name) {
        const data = stripObsoleteProfileSettings(clone(entry));
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
        aliasFile: "C:\\EuroScope\\Alias\\alias.txt",
        resolutionPreset: preferred?.data?.targets?.small_icon_boost_resolution_preset || "1080p",
        showFps: true,
        avisoColorPalette: "night",
        dataHealth: {
          profilesHealthy: true,
          profilesUsingBackup: false,
          profilesBackupAvailable: false,
          profilesBackupModifiedUnixSeconds: 0,
          profilesMessage: "",
          avisoHealthy: true,
          avisoMessage: ""
        }
      },
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
  const expiredRequestIds = new Set();
  const updateCenter = {
    config: {
      schema_version: 1,
      auto_check: true,
      auto_download: true,
      auto_install: true,
      protect_modified_aviso: true,
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
  let savedSnapshot = null;
  // Exactly one immutable model snapshot may be written at a time. Any edits
  // made while it is in flight remain in `state` and are sent by the next
  // queue pass; a save acknowledgement never replaces the live model.
  let saveInFlight = null;
  let autosaveTimer = 0;
  let autosaveQueued = false;
  const editorClipboard = { tag: "", color: "" };
