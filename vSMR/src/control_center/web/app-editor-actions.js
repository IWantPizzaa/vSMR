"use strict";

  function selectAllEditorItems(kind) {
    if (!stageFocusedEditorValue()) return;
    const lists = {
      colors: [() => collectProfileColors(activeProfile()), "selectedColorPaths", "selectedColorPath", "colorSelectionAnchorPath", "color", renderColors],
      tags: [tagDefinitions, "selectedTagIds", "selectedTagId", "tagSelectionAnchorId", "tag", renderTags],
      geometry: [() => avisoStyleEntries("geometry"), "selectedAvisoGeometryStyleIds", "selectedAvisoGeometryStyleId", "avisoGeometrySelectionAnchorId", "avisoGeometry", renderAvisoGeometry],
      text: [() => avisoStyleEntries("text"), "selectedAvisoTextStyleIds", "selectedAvisoTextStyleId", "avisoTextSelectionAnchorId", "avisoTextStyle", renderAvisoText]
    };
    const list = lists[kind];
    if (!list) return;
    const ids = list[0]().map(entry => entry.id);
    state.ui[list[1]] = ids;
    state.ui[list[2]] = ids.at(-1) || "";
    state.ui[list[3]] = ids[0] || "";
    drafts[list[4]] = null;
    const editors = { colors: "#colorHex", tags: "#tagDefinitionEditor", geometry: "#avisoGeometryColorHex", text: "#avisoTextFont" };
    clearUnappliedEditorSection($(editors[kind]));
    list[5]();
  }

  const RESET_SECTION_LABELS = {
    colors: "selected profile colors", icons: "icon appearance and trails",
    tags: "selected tag definitions", "tag-options": "tag font and layout options",
    rules: "this profile's color rules", alerts: "alert options (keeping runway states)",
    geometry: "selected AVISO geometry styles in this palette",
    text: "selected AVISO text styles in this palette"
  };

  function requestSectionDefaults(section) {
    if (!RESET_SECTION_LABELS[section] || !hostAuthoritativeReady || state.externalEditConflict ||
        pending.reload || pending.save || pending.resource || runtimeCommandPending.size || splitAvisoContext) return;
    if (!stageFocusedEditorValue()) return;
    if (!window.confirm(`Reset ${RESET_SECTION_LABELS[section]} to bundled defaults? Other sections are unchanged. Custom profiles use the Default profile when no matching bundled profile exists. Changes are saved automatically.`)) return;
    const request = {
      section, profileId: state.activeProfileId, airport: state.airport,
      palette: activeAvisoColorPalette(), configRevision: state.configRevision, avisoRevision: state.avisoRevision,
      colors: colorSelectionIds().slice(), tags: tagSelectionIds().slice(),
      geometry: geometrySelectionIds().slice(), text: textStyleSelectionIds().slice()
    };
    const id = postBridge("state.reset", {});
    if (!id) return;
    pending.resource = { ...request, id, resource: "defaults", source: "bundled defaults", kind: "section-defaults" };
    armPendingTimeout("resource", id);
    updateCommandState();
    if (!HOST_MODE) {
      // The standalone preview has no native default provider.
      setTimeout(() => {
        receiveHostMessage({ version: PROTOCOL_VERSION, id, type: "resource.loaded", payload: {
          resource: "aviso", source: "bundled defaults", data: clone(DEFAULT_DATA.aviso) } });
        receiveHostMessage({ version: PROTOCOL_VERSION, id, type: "resource.loaded", payload: {
          resource: "profiles", source: "bundled defaults", data: clone(DEFAULT_DATA.profiles) } });
      }, 0);
    }
  }

  function copyDefaultKey(target, defaults, key) {
    if (Object.hasOwn(defaults || {}, key)) target[key] = clone(defaults[key]);
    else delete target[key];
  }

  function profileResetSource(profiles, record) {
    const candidates = profiles.filter(profile => profile?.name);
    const identity = record.persistedName && record.data?._vsmr_profile_id;
    return (identity && candidates.find(profile => profile._vsmr_profile_id === identity))
      || candidates.find(profile => record.persistedName && profile.name === record.persistedName)
      || candidates.find(profile => profile.name === "Default");
  }

  function applyProfileSectionDefaults(request, profiles) {
    if (!Array.isArray(profiles)) throw new Error("Bundled profile defaults are unavailable");
    const record = activeProfileRecord();
    const defaults = profileResetSource(profiles, record);
    if (!defaults) throw new Error("No matching bundled profile or Default profile is available");
    const profile = record.data;
    if (request.section === "colors") {
      let count = 0;
      request.colors.forEach(id => {
        const value = getAtPath(defaults, id.split("."));
        if (!isColorObject(value)) return;
        setAtPath(profile, id.split("."), clone(value));
        ++count;
      });
      if (!count) throw new Error("No bundled defaults match the selected colors");
    } else if (request.section === "icons") {
      profile.targets ||= {};
      ["icon_style", "symbol_scale", "trail_enabled", "trail_ground_points", "trail_airborne_points"]
        .forEach(key => copyDefaultKey(profile.targets, defaults.targets, key));
    } else if (request.section === "tags") {
      const reference = new Map(tagDefinitions(defaults).map(entry => [entry.id, entry]));
      let count = 0;
      tagDefinitions(profile).filter(entry => request.tags.includes(entry.id)).forEach(entry => {
        const original = reference.get(entry.id);
        if (!original) return;
        ["definition", "definition_detailed", "definition_detailed_inherits_normal"]
          .forEach(key => copyDefaultKey(entry.target, original.target, key));
        ++count;
      });
      if (!count) throw new Error("No bundled defaults match the selected tag definitions");
    } else if (request.section === "tag-options") {
      profile.labels ||= {};
      ["rounded_corners", "fit_background_to_text", "auto_deconfliction"]
        .forEach(key => copyDefaultKey(profile.labels, defaults.labels, key));
      copyDefaultKey(profile, defaults, "font");
    } else if (request.section === "rules") {
      copyDefaultKey(profile, defaults, "rules");
      state.ui.selectedRuleIndex = 0;
    } else if (request.section === "alerts") {
      // Reset only the configurable alert options, never runway assignments or closures.
      const current = profile.rimcas || {};
      profile.rimcas = clone(defaults.rimcas || {});
      copyDefaultKey(profile.rimcas, current, "runways");
      copyDefaultKey(profile.rimcas, current, "visibility");
    }
    ["color", "tag", "rule", "alerts"].forEach(key => { drafts[key] = null; });
    markDirty("Section restored to bundled defaults", ["profiles"]);
  }

  function applyAvisoSectionDefaults(request) {
    if (!request.aviso || normalizeAirportCode(request.aviso.metadata?.icao || request.aviso.metadata?.airport || inferAirport(request.aviso.name)) !== request.airport)
      throw new Error("No bundled AVISO defaults match this airport; the map is unchanged");
    const baseline = normalizeAvisoData(clone(request.aviso));
    const features = new Map((baseline.features || []).filter(feature => feature.id != null)
      .map(feature => [String(feature.id), feature]));
    const keys = request.section === "text" ? AVISO_TEXT_PAINT_KEYS : AVISO_GEOMETRY_PAINT_KEYS;
    let count = 0;
    avisoStyleEntries(request.section).filter(entry => request[request.section].includes(entry.id)).forEach(entry => {
      if (entry.isBackground) {
        const colors = baseline.metadata?.background_colors;
        const color = colors?.[request.palette];
        if (!color) return;
        state.aviso.metadata.background_colors[request.palette] = color;
        ++count;
        return;
      }
      const defaultPaint = baseline.styles?.[entry.id]?.paint;
      if (!defaultPaint) return; // Custom styles have no official reset target.
      const paint = ensureAvisoCatalogStyle(entry).paint;
      const previous = clone(paint);
      const changesFor = properties => Object.fromEntries(keys.map(key => [key,
        effectiveAvisoPaintValue(defaultPaint, properties || {}, key, undefined, request.palette)
          ?? (request.section === "text" ? AVISO_TEXT_DEFAULTS[key] : undefined)
      ]).filter(([key, value]) => value !== undefined || !AVISO_PALETTE_COLOR_KEYS.has(key)));
      applyAvisoPaintChanges(paint, changesFor({}));
      entry.indices.forEach(index => {
        const feature = avisoFeatures()[index];
        const original = features.get(String(feature.id));
        applyAvisoPaintChanges(feature.properties, changesFor(original?.properties), previous);
      });
      ++count;
    });
    if (!count) throw new Error("No bundled defaults match the selected styles; custom styles are unchanged");
    drafts.avisoGeometry = drafts.avisoTextStyle = null;
    markDirty("Selected AVISO styles restored to bundled defaults", ["aviso"]);
  }

  function finishSectionDefaults(message, success) {
    const request = pending.resource;
    if (request?.kind !== "section-defaults" || !messageMatchesRequest(message, request.id)) return false;
    if (success && message.payload.resource === "aviso") {
      request.aviso = clone(message.payload.data);
      return true;
    }
    pending.resource = null;
    expiredRequestIds.add(request.id); // Ignore duplicate/late resource replies, never turn them into a global reset.
    while (expiredRequestIds.size > 32) expiredRequestIds.delete(expiredRequestIds.values().next().value);
    if (!success) { updateCommandState(); return true; }
    try {
      if (message.payload.resource !== "profiles" || request.profileId !== state.activeProfileId ||
          request.airport !== state.airport || request.palette !== activeAvisoColorPalette() ||
          request.configRevision !== state.configRevision || request.avisoRevision !== state.avisoRevision)
        throw new Error("The editing context changed while defaults loaded. Try Reset again.");
      if (["geometry", "text"].includes(request.section)) applyAvisoSectionDefaults(request);
      else applyProfileSectionDefaults(request, message.payload.data);
      renderAll();
      showToast("Selected settings restored to bundled defaults", "success");
    } catch (error) {
      showToast(error.message || "Cannot restore these defaults", "error");
    }
    updateCommandState();
    return true;
  }
