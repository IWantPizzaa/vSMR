"use strict";

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
    return normalizeAvisoColorPalette(state.settings.avisoColorPalette);
  }

  function avisoPaletteOverride(paint, palette = activeAvisoColorPalette()) {
    if (palette === "dark" || !paint || typeof paint !== "object") return null;
    const overrides = paint["palette-overrides"];
    if (!overrides || typeof overrides !== "object") return null;
    const candidates = palette === "real" ? ["real", "light", "day"] : ["light", "day"];
    return candidates.map(name => overrides[name]).find(value => value && typeof value === "object") || null;
  }

  function effectiveAvisoPaintValue(sharedPaint, inlinePaint, key, fallback = undefined) {
    const palette = activeAvisoColorPalette();
    if (palette !== "dark" && AVISO_PALETTE_COLOR_KEYS.has(key)) {
      const inlineOverride = avisoPaletteOverride(inlinePaint, palette);
      if (inlineOverride?.[key] != null) return inlineOverride[key];
      // Match the native renderer: an intentional feature-level Dark color
      // remains authoritative until that feature receives its own override.
      if (inlinePaint?.[key] != null) return inlinePaint[key];
      const sharedOverride = avisoPaletteOverride(sharedPaint, palette);
      if (sharedOverride?.[key] != null) return sharedOverride[key];
    }
    return inlinePaint?.[key] ?? sharedPaint?.[key] ?? fallback;
  }

  function applyAvisoPaintChanges(target, changes) {
    if (!target || typeof target !== "object") return;
    const paletteColors = {};
    const palette = activeAvisoColorPalette();
    Object.entries(changes).forEach(([key, value]) => {
      if (palette !== "dark" && AVISO_PALETTE_COLOR_KEYS.has(key)) paletteColors[key] = value;
      else target[key] = value;
    });
    if (!Object.keys(paletteColors).length) return;
    if (!target["palette-overrides"] || typeof target["palette-overrides"] !== "object") target["palette-overrides"] = {};
    if (!target["palette-overrides"][palette] || typeof target["palette-overrides"][palette] !== "object") target["palette-overrides"][palette] = {};
    Object.assign(target["palette-overrides"][palette], paletteColors);
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
    const entries = collectAvisoStyleEntries().filter(entry => kind === "text" ? entry.isText : !entry.isText);
    if (kind !== "geometry") return entries;
    const palette = activeAvisoColorPalette();
    const colors = state.aviso?.metadata?.background_colors || {};
    const color = normalizeHex(colors[palette] ?? colors.light ?? colors.day ?? colors.dark ?? colors.night, "#434A4F");
    return [{
      id: AVISO_BACKGROUND_STYLE_ID,
      name: "Background",
      layer: "AVISO",
      objectType: "Background",
      paint: { fill: color },
      indices: [],
      isText: false,
      isBackground: true
    }, ...entries];
  }

  function avisoStyleEntry(styleId, kind) {
    return avisoStyleEntries(kind).find(entry => entry.id === styleId) || null;
  }

  function ensureAvisoCatalogStyle(entry) {
    if (entry?.isBackground) return null;
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
    drafts.avisoGeometry = null;
    drafts.avisoTextStyle = null;
    drafts.avisoGroup = null;
    avisoGroupContentDraft = null;
  }

  function avisoPaintColor(entry) {
    if (entry.isBackground) return normalizeHex(entry.paint.fill, "#434A4F");
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
    if (entry.isBackground) return `<span class="aviso-style-eye" aria-hidden="true"></span>`;
    const visibility = avisoStyleVisibility(entry);
    const action = visibility.allVisible ? "Hide" : "Show";
    return `<button type="button" class="ui-button ui-button--compact ui-button--icon aviso-style-eye ${visibility.allVisible ? "is-on" : "is-off"} ${visibility.mixed ? "is-mixed" : ""}" data-aviso-style-visibility="${kind}" data-aviso-style-id="${escapeHtml(entry.id)}" aria-label="${action} ${escapeHtml(entry.name)}" title="${action} ${escapeHtml(entry.name)}">
      <svg aria-hidden="true" viewBox="0 0 24 24"><path d="M2.5 12s3.6-6 9.5-6 9.5 6 9.5 6-3.6 6-9.5 6-9.5-6-9.5-6z"></path><circle cx="12" cy="12" r="2.6"></circle>${visibility.allVisible || visibility.mixed ? "" : '<path class="aviso-eye-slash" d="M4 4l16 16"></path>'}</svg>
    </button>`;
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
    if (name) drafts.avisoGroup.data.name = name.value;
    return drafts.avisoGroup;
  }

  function renderAvisoGroups() {
    const groups = avisoGroups();
    const groupIndex = buildAvisoGroupIndex();
    const selected = selectedAvisoGroup();

    $("#avisoGroupList").innerHTML = groups.length ? `<div class="ui-list__items aviso-group-box" role="presentation">${groups.map(group => {
      const active = group.id === selected?.id;
      return `<button type="button" class="${uiListRowClass("group", active, false, "aviso-group-row")}" role="option" aria-selected="${active}" data-aviso-group-id="${escapeHtml(group.id)}" draggable="true" title="Drag to reorder">
        <span class="ui-list__label aviso-group-row-copy">${escapeHtml(group.name)}</span>
      </button>`;
    }).join("")}</div>` : `<div class="ui-list__empty">No groups yet</div>`;
    syncUiListFocus($("#avisoGroupList"));

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
      $("#avisoGroupMemberList").innerHTML = `<div class="ui-list__empty">Create a group to combine text, line and area content.</div>`;
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
    $("#avisoGroupMemberList").innerHTML = rows.length ? `<div class="ui-list__items" role="list">${rows.map(row => {
      return `<div class="${uiListRowClass("action", false, false, "aviso-group-member-row")}" role="listitem">
        <strong class="ui-list__label aviso-member-name">${escapeHtml(row.name)}</strong>
        <button aria-label="Remove ${escapeHtml(row.name)} from group" class="ui-button ui-button--compact ui-button--icon ui-button--destructive aviso-member-remove" type="button" data-action="remove-aviso-group-member" data-member-kind="${row.kind === "text" ? "feature" : "style"}" data-member-id="${escapeHtml(row.id)}" title="Remove from group">×</button>
      </div>`;
    }).join("")}</div>` : `<div class="ui-list__empty">${counts.total ? "No members match your search." : "This group is empty. Add text, lines or areas."}</div>`;
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
    state.ui.avisoGroupContentSearch = "";
    $("#avisoGroupContentSearch").value = "";
    renderAvisoGroupContentDialog();
    $("#avisoGroupContentDialog").showModal();
  }

  function renderAvisoGroupContentDialog() {
    const group = avisoGroups().find(item => item.id === avisoGroupContentDraft?.groupId) || selectedAvisoGroup();
    if (!group || !avisoGroupContentDraft) return;
    $("#avisoGroupContentTitle").textContent = `Content · ${group.name}`;
    syncToggleButtons('[data-aviso-group-content-type]', state.ui.avisoGroupContentType, "avisoGroupContentType");
    $("#avisoGroupContentSearch").value = state.ui.avisoGroupContentSearch;
    const candidates = groupContentCandidates();
    $("#avisoGroupContentList").innerHTML = candidates.length ? `<div aria-label="${escapeHtml(state.ui.avisoGroupContentType)} content" class="ui-list__items group-content-box" role="group">${candidates.map(item => {
      const selectedCount = item.indices.filter(index => avisoGroupContentDraft.members.has(index)).length;
      const selected = selectedCount === item.indices.length;
      const partial = selectedCount > 0 && !selected;
      return `<button type="button" class="${uiListRowClass("content", selected, false, `group-content-row ${partial ? "partial" : ""}`)}" data-group-content-key="${escapeHtml(item.key)}"><span class="ui-list__leading group-content-check">${selected ? "✓" : partial ? "−" : ""}</span><strong class="ui-list__label group-content-name">${escapeHtml(item.name)}</strong></button>`;
    }).join("")}</div>` : `<div class="ui-list__empty">No matching ${state.ui.avisoGroupContentType} content.</div>`;
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
    markDirty("AVISO group contents updated", ["aviso"]);
    renderRuntimeMenu();
    return true;
  }

  function renderAviso() {
    const geometryEntries = avisoStyleEntries("geometry");
    const textEntries = avisoStyleEntries("text");

    geometrySelectionIds(geometryEntries);
    textStyleSelectionIds(textEntries);

    const availablePalettes = new Set(avisoColorPalettes(state.aviso, state.settings.avisoColorPalettes));
    let avisoColorPalette = activeAvisoColorPalette();
    if (availablePalettes.size && !availablePalettes.has(avisoColorPalette)) {
      avisoColorPalette = [...availablePalettes][0];
      state.settings.avisoColorPalette = avisoColorPalette;
    }
    $$('[data-aviso-color-palette]').forEach(button => {
      const available = availablePalettes.has(button.dataset.avisoColorPalette);
      button.disabled = !available;
      button.title = available
        ? `Use the ${button.textContent.trim()} AVISO palette`
        : `${button.textContent.trim()} is not available for this airport`;
    });
    syncToggleButtons('[data-aviso-color-palette]', avisoColorPalette, "avisoColorPalette");
    syncTabButtons('[data-aviso-view]', state.ui.avisoView, "avisoView");
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
      <section class="ui-list__section aviso-style-section">
        <div class="ui-list__heading aviso-style-section-title"><span class="ui-list__heading-label">${escapeHtml(layer)}</span></div>
        <div aria-label="${escapeHtml(layer)} geometry styles" class="ui-list__items aviso-style-box" role="list">
          ${entries.map(entry => {
            const selected = selectedIds.has(entry.id);
            const current = entry.id === state.ui.selectedAvisoGeometryStyleId;
            return `<div class="${uiListRowClass("style", selected, current, "aviso-style-row")}" role="listitem">
              ${avisoStyleVisibilityControl(entry, "geometry")}
              <button aria-pressed="${selected}" class="ui-list__selection" data-aviso-geometry-style="${escapeHtml(entry.id)}" title="${escapeHtml(entry.name)}" type="button">
                <span class="ui-list__leading aviso-style-swatch" style="--aviso-swatch:${avisoPaintColor(entry)}"></span>
                <span class="ui-list__label aviso-style-copy">${escapeHtml(entry.name)}</span>
              </button>
            </div>`;
          }).join("")}
        </div>
      </section>`).join("") || `<div class="ui-list__empty">No geometry styles</div>`;
    syncUiListFocus($("#avisoGeometryStyleList"));

    renderAvisoGeometryEditor(allEntries);
  }

  function renderAvisoGeometryEditor(allEntries = avisoStyleEntries("geometry")) {
    const entries = selectedAvisoGeometryEntries(allEntries);
    if (!entries.length) {
      $("#avisoGeometryCaption").textContent = "No geometry selected";
      return;
    }

    const types = uniqueValues(entries.map(entry => entry.objectType));
    const backgroundOnly = entries.every(entry => entry.isBackground);
    $("#avisoGeometryCaption").textContent = entries.length === 1 ? entries[0].name : `${entries.length} geometry styles`;
    const paletteLabel = `${activeAvisoColorPalette()[0].toUpperCase()}${activeAvisoColorPalette().slice(1)}`;
    const colorKind = backgroundOnly ? "Background" : types.length > 1 ? "Primary" : types[0] === "Line" ? "Line" : "Fill";
    $("#avisoGeometryColorLabel").textContent = `${colorKind} color · ${paletteLabel}`;
    $("#avisoGeometryColorOpacity")?.closest(".opacity-channel")?.toggleAttribute("hidden", backgroundOnly);

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
      if (entry.isBackground) {
        if (colorTouched && changes.fill) {
          state.aviso.metadata.background_colors[activeAvisoColorPalette()] = changes.fill;
          updatedCount += 1;
        }
        return;
      }
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
    if (feedback) showToast(`Updated ${updatedCount.toLocaleString()} AVISO object${updatedCount === 1 ? "" : "s"}`, "success");
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
      <section class="ui-list__section aviso-style-section">
        <div class="ui-list__heading aviso-style-section-title"><span class="ui-list__heading-label">${escapeHtml(layer)}</span></div>
        <div aria-label="${escapeHtml(layer)} text styles" class="ui-list__items aviso-style-box" role="list">
          ${entries.map(entry => {
            const selected = selectedIds.has(entry.id);
            const current = entry.id === state.ui.selectedAvisoTextStyleId;
            return `<div class="${uiListRowClass("style", selected, current, "aviso-style-row")}" role="listitem">
              ${avisoStyleVisibilityControl(entry, "text")}
              <button aria-pressed="${selected}" class="ui-list__selection" data-aviso-text-style="${escapeHtml(entry.id)}" title="${escapeHtml(entry.name)}" type="button">
                <span class="ui-list__leading aviso-style-swatch" style="--aviso-swatch:${avisoPaintColor(entry)}"></span>
                <span class="ui-list__label aviso-style-copy">${escapeHtml(entry.name)}</span>
              </button>
            </div>`;
          }).join("")}
        </div>
      </section>`).join("") || `<div class="ui-list__empty">No text styles</div>`;
    syncUiListFocus($("#avisoTextStyleList"));

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
    const paletteLabel = `${activeAvisoColorPalette()[0].toUpperCase()}${activeAvisoColorPalette().slice(1)}`;
    const colorTarget = state.ui.avisoTextColorTarget === "halo" ? "halo" : "text";
    const colorKey = colorTarget === "halo" ? "text-halo-color" : "text-color";
    const colorFallback = colorTarget === "halo" ? "#000000" : "#808080";
    $("#avisoTextColorLabel").textContent = `${colorTarget === "halo" ? "Halo" : "Text"} color · ${paletteLabel}`;
    syncToggleButtons("[data-color-target]", colorTarget, "colorTarget", $("#avisoTextColorEditor"));

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
