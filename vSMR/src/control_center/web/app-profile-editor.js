"use strict";

  function renderIconSymbolPreview() {
    const preview = $("#iconSymbolPreview");
    if (!preview) return;
    const style = String($("#targetIconStyle")?.value || activeProfile().targets?.icon_style || "realistic").toLowerCase();
    const trailEnabled = $("#targetTrailEnabled")?.checked ?? activeProfile().targets?.trail_enabled !== false;

    let symbol = "";
    let caption = "";
    let usesAircraftImage = false;
    if (style === "nova") {
      const shape = "M0-38-8-35-10-18-38 6-36 17-11 8-7 32 0 39 7 32 11 8 36 17 38 6 10-18 8-35Z";
      const afterglow = trailEnabled
        ? `<path class="nova-afterglow oldest" transform="translate(0 15)" d="${shape}"/><path class="nova-afterglow middle" transform="translate(0 10)" d="${shape}"/><path class="nova-afterglow newest" transform="translate(0 5)" d="${shape}"/>`
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

    const trailClass = style === "nova" ? "nova" : style === "realistic" ? "realistic" : style === "diamond" ? "triangle diamond" : "triangle";
    const trail = trailEnabled
      ? `<span class="icon-preview-trail ${trailClass}" aria-hidden="true"><i></i><i></i><i></i><i></i></span>`
      : "";
    preview.innerHTML = `<div class="icon-preview-stage"><span class="icon-preview-flight">${trail}<span class="icon-preview-symbol">${symbol}</span></span></div><span>${escapeHtml(caption)}</span>`;

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
    const symbolScale = clamp(targets.symbol_scale ?? 1, 0.25, 5);
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
    targets.symbol_scale = clamp($("#targetSymbolScale").value, 0.25, 5);
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
    TAG_EDITOR_SCOPES.forEach(scope => {
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

  function tagDefinitionContent(source = {}) {
    const definition = Array.isArray(source.definition) ? clone(source.definition) : [];
    const inheritsNormal = Boolean(source.definition_detailed_inherits_normal);
    const detailed = inheritsNormal
      ? clone(definition)
      : (Array.isArray(source.definition_detailed) ? clone(source.definition_detailed) : []);
    return {
      definition,
      definition_detailed: detailed,
      definition_detailed_inherits_normal: inheritsNormal
    };
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
      const rows = items.map(entry => `<button type="button" role="option" aria-selected="${selectedIds.has(entry.id)}" class="${uiListRowClass("tag", selectedIds.has(entry.id), entry.id === state.ui.selectedTagId, "tag-menu-row")}" data-tag-id="${escapeHtml(entry.id)}" title="${escapeHtml(entry.label)}">
        <span class="ui-list__label menu-row-title">${escapeHtml(entry.label)}</span>
      </button>`).join("");
      return `<section class="ui-list__section tag-menu-section" style="--menu-accent:${accent}">
        <button type="button" class="ui-list__heading" data-tree-toggle="tags" data-tree-key="${escapeHtml(groupKey)}" aria-expanded="${!collapsed}">
          <span class="ui-list__caret" aria-hidden="true">${collapsed ? "▸" : "▾"}</span>
          <span class="ui-list__heading-label">${escapeHtml(group)}</span>
        </button>
        <div aria-label="${escapeHtml(group)} tag definitions" aria-multiselectable="true" class="ui-list__items" role="listbox" ${collapsed ? "hidden" : ""}>${rows}</div>
      </section>`;
    }).join("") || `<div class="ui-list__empty">No tag definitions</div>`;
    syncUiListFocus($("#tagDefinitionList"));
    requestAnimationFrame(() => $("#tagDefinitionList .tag-menu-row.is-selected")?.scrollIntoView({ block: "nearest" }));
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
      drafts.tag = { id: entry.id, signature, data: tagDefinitionContent(entry.target) };
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
    $("#tagFitBackgroundToText").checked = Boolean(labels.fit_background_to_text);
    $("#tagAutoDeconfliction").checked = Boolean(labels.auto_deconfliction);
    const labelSlot = Math.round(clamp(activeProfile().font?.label_font_size ?? 1, 1, 5));
    const labelKey = ["one", "two", "three", "four", "five"][labelSlot - 1];
    const storedSize = Number(activeProfile().font?.sizes?.[labelKey] ?? (9 + labelSlot));
    const labelSize = Number.isFinite(storedSize) ? Math.max(6, Math.round(storedSize)) : 9 + labelSlot;
    $("#tagLabelFontSize").max = Math.max(72, labelSize);
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
    const data = drafts.tag?.data || tagDefinitionContent(entry.target);
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
      const content = tagDefinitionContent(drafts.tag.data);
      entries.forEach(targetEntry => {
        targetEntry.target.definition = clone(content.definition);
        targetEntry.target.definition_detailed = clone(content.definition_detailed);
        targetEntry.target.definition_detailed_inherits_normal = content.definition_detailed_inherits_normal;
      });
    }

    const labels = activeProfile().labels ||= {};
    labels.rounded_corners = $("#tagRoundedCorners").checked;
    labels.fit_background_to_text = $("#tagFitBackgroundToText").checked;
    labels.auto_deconfliction = $("#tagAutoDeconfliction").checked;
    activeProfile().font ||= {};
    // Keep the legacy slot identity so old profiles/ASRs remain compatible;
    // the editor changes its actual pixel size, never the hidden 1-5 index.
    const labelSlot = Math.round(clamp(activeProfile().font.label_font_size ?? 1, 1, 5));
    const labelKey = ["one", "two", "three", "four", "five"][labelSlot - 1];
    activeProfile().font.label_font_size = labelSlot;
    activeProfile().font.font_name = $("#profileFontName").value || "Arial";
    activeProfile().font.weight = $("#profileFontWeight").value || "Regular";
    activeProfile().font.sizes ||= { one: 10, two: 11, three: 12, four: 13, five: 14 };
    activeProfile().font.sizes[labelKey] = Math.round(clamp($("#tagLabelFontSize").value, 6, Number($("#tagLabelFontSize").max) || 72));

    clearUnappliedEditorSection($("#tagDefinitionEditor"));
    markDirty(`${entries.length === 1 ? entry.label : `${entries.length} tag definitions`} updated`, ["profiles"]);
    if (render) renderTags();
  }

  function normalizeClipboardTagLines(value) {
    if (!Array.isArray(value)) return null;
    return value.map(line => {
      if (!Array.isArray(line)) return null;
      return line.map(token => String(token || "").trim()).filter(Boolean);
    }).filter(line => Array.isArray(line) && line.length);
  }

  async function copyTagDefinition() {
    captureTagDraft();
    const entry = selectedTagDefinition();
    if (!entry || !drafts.tag?.data) return;
    const content = tagDefinitionContent(drafts.tag.data);
    const value = JSON.stringify({
      vsmr: "tag-definition",
      version: 1,
      definition: content.definition,
      definition_detailed: content.definition_detailed,
      definition_detailed_inherits_normal: content.definition_detailed_inherits_normal
    }, null, 2);
    await writeEditorClipboard(value, "tag");
    showToast("Tag definition copied", "success");
  }

  async function pasteTagDefinition() {
    const raw = String(await readEditorClipboard("tag", "Paste a vSMR tag definition") || "").trim();
    if (!raw) return;
    let parsed;
    try { parsed = JSON.parse(raw); }
    catch (error) {
      showToast("Clipboard does not contain a vSMR tag definition", "error");
      return;
    }
    const normal = normalizeClipboardTagLines(parsed?.definition);
    const detailed = normalizeClipboardTagLines(parsed?.definition_detailed);
    if (!normal || !detailed) {
      showToast("Clipboard does not contain a valid tag definition", "error");
      return;
    }
    const entries = selectedTagDefinitions();
    const entry = selectedTagDefinition();
    if (!entry || !entries.length) return;
    const inheritsNormal = Boolean(parsed.definition_detailed_inherits_normal);
    const content = {
      definition: normal,
      definition_detailed: inheritsNormal ? clone(normal) : detailed,
      definition_detailed_inherits_normal: inheritsNormal
    };
    entries.forEach(targetEntry => {
      targetEntry.target.definition = clone(content.definition);
      targetEntry.target.definition_detailed = clone(content.definition_detailed);
      targetEntry.target.definition_detailed_inherits_normal = content.definition_detailed_inherits_normal;
    });
    drafts.tag = {
      id: entry.id,
      signature: tagSelectionIds().join("|"),
      data: clone(content)
    };
    clearUnappliedEditorSection($("#tagDefinitionEditor"));
    markDirty(`${entries.length === 1 ? entry.label : `${entries.length} tag definitions`} pasted`, ["profiles"]);
    renderTagEditor();
    showToast(`Tag definition pasted to ${entries.length} selection${entries.length === 1 ? "" : "s"}`, "success");
  }

  function rules() {
    activeProfile().rules ||= { version: 2, items: [] };
    activeProfile().rules.items ||= [];
    return activeProfile().rules.items;
  }
  function ruleLabel(rule, index) { return String(rule?.name || "").trim() || `Rule ${index + 1}`; }
  function ruleCanonicalName(value) { return typeof value === "string" ? value.trim().toLowerCase() : ""; }
  function ruleFieldDefinition(field) { return [...RULE_FIELDS, ...RULE_COMPAT_FIELDS].find(item => item.id === ruleCanonicalName(field)); }
  function ruleOperators(field) {
    return ruleFieldDefinition(field)?.numeric
      ? ["set", "missing", "equals", "not_equals", "lt", "lte", "gt", "gte", "between"]
      : ["set", "missing", "equals", "not_equals", "in", "not_in", "contains", "starts_with", "ends_with"];
  }
  function ruleSelectOptions(values, selected, labels = null) {
    return values.map(value => `<option value="${escapeHtml(value)}" ${String(value) === String(selected ?? "") ? "selected" : ""}>${escapeHtml(labels?.[value] || value)}</option>`).join("");
  }
  function defaultRuleCondition() { return { field: "cdm.tobt", op: "set" }; }
  function defaultRuleEffect() { return { type: "text_color", color: hexToColor("#ffffff") }; }
  function migrateLegacyRuleCondition(item) {
    const rawSource = ruleCanonicalName(item?.source ?? item?.kind ?? "cdm");
    let source = "cdm";
    if (["vsid", "v_sid"].includes(rawSource)) source = "vsid";
    else if (rawSource.includes("custom") || ["list", "sidlist", "sid"].includes(rawSource)) source = "custom";
    else if (rawSource.includes("runway") || rawSource === "rwy") source = "runway";
    let token = ruleCanonicalName(item?.token);
    if (source === "custom" && token === "sid") token = "asid";
    if (source === "vsid") token = ({ sid: "vsid_sid", rwy: "vsid_rwy", runway: "vsid_rwy", cfl: "vsid_cfl" })[token] || token;
    if (source === "cdm" && token.startsWith("cdm_")) token = token.slice(4);
    return { field: "legacy", source, token, condition: String(item?.condition ?? item?.runway ?? item?.state ?? "").trim() || "any" };
  }
  function canonicalizeRuleTree(node, depth = 1) {
    if (!node || typeof node !== "object" || depth > RULE_LIMITS.depth) return;
    if (Array.isArray(node.all || node.any)) (node.all || node.any).forEach(child => canonicalizeRuleTree(child, depth + 1));
    else if (node.not) canonicalizeRuleTree(node.not, depth + 1);
    else {
      node.field = ruleCanonicalName(node.field);
      if (node.field === "legacy") { node.source = ruleCanonicalName(node.source); node.token = ruleCanonicalName(node.token); }
      else node.op = ruleCanonicalName(node.op);
    }
  }
  function migrateRuleForEditor(source) {
    if (!source || typeof source !== "object" || Array.isArray(source)) return null;
    const rule = clone(source);
    if (!Object.hasOwn(rule, "when")) {
      const criteria = Array.isArray(rule.criteria) && rule.criteria.length
        ? rule.criteria : [{ source: rule.source ?? rule.kind ?? "cdm", token: rule.token, condition: rule.condition ?? rule.runway ?? rule.state }];
      rule.when = { all: criteria.map(migrateLegacyRuleCondition) };
    }
    canonicalizeRuleTree(rule.when);
    if (!Object.hasOwn(rule, "effects")) rule.effects = ["target_color", "tag_color", "text_color"]
      .filter(type => isColorObject(rule[type])).map(type => ({ type, color: { ...clone(rule[type]), a: rule[type].a ?? 255 } }));
    rule.enabled = rule.enabled !== false;
    rule.stop_processing = rule.stop_processing === true;
    rule.tag_type = ruleCanonicalName(rule.tag_type || "any");
    rule.detail = ruleCanonicalName(rule.detail || "any");
    const statuses = selectedRuleStatuses(source);
    rule.statuses = statuses.length === RULE_STATUSES.length && RULE_STATUSES.every(status => statuses.includes(status))
      ? ["any"] : statuses;
    // The migrated list now carries the complete scope. Keeping an old scalar
    // such as "taxi | any" would make otherwise valid v2 data fail validation.
    delete rule.status;
    (Array.isArray(rule.effects) ? rule.effects : []).forEach(effect => {
      effect.type = ruleCanonicalName(effect.type);
      if (effect.field) effect.field = ruleCanonicalName(effect.field);
      if (effect.color) effect.color.a ??= 255;
      if (["field_bold", "field_blink"].includes(effect.type)) effect.value ??= true;
    });
    const keys = ["name", "enabled", "stop_processing", "tag_type", "status", "statuses", "detail", "when", "effects"];
    Object.keys(rule).filter(key => !keys.includes(key)).forEach(key => delete rule[key]);
    return rule;
  }
  function validateRuleEditorData(rule, { allowDefaults = false } = {}) {
    let count = 0;
    const known = (value, names) => value && typeof value === "object" && !Array.isArray(value) && Object.keys(value).every(key => names.includes(key));
    const validText = (value, allowEmpty = false) => typeof value === "string" && new TextEncoder().encode(value).length <= 512 &&
      !/[\x00-\x1f\x7f]/.test(value) && (allowEmpty || value.trim().length > 0);
    const finiteNumber = value => typeof value === "number" && Number.isFinite(value);
    const legacyTokens = {
      runway: ["deprwy", "seprwy", "arvrwy", "srvrwy"],
      custom: ["sid", "asid", "ssid", "deprwy", "seprwy", "arvrwy", "srvrwy"],
      vsid: ["vsid_sid", "vsid_rwy", "vsid_cfl"],
      cdm: ["tobt", "tsat", "ttot", "ctot", "tsac", "asrt", "asat", "aobt", "atot", "aort", "deice",
        "tobt_set_by", "flow_restriction", "ecfmp_restriction", "manual_ctot"]
    };
    const checkNode = (node, depth) => {
      if (++count > RULE_LIMITS.nodes || depth > RULE_LIMITS.depth) return "Use at most 128 condition nodes and 8 nesting levels.";
      if (!node || typeof node !== "object" || Array.isArray(node)) return "Each condition must be an object.";
      const groups = ["all", "any", "not"].filter(key => Object.hasOwn(node, key));
      if (groups.length) {
        if (groups.length !== 1 || Object.keys(node).length !== 1) return "A condition group has exactly one operator.";
        const key = groups[0], children = key === "not" ? [node.not] : node[key];
        if (!Array.isArray(children) || !children.length || children.length > RULE_LIMITS.nodes) return "All / Any groups need at least one condition.";
        for (const child of children) { const error = checkNode(child, depth + 1); if (error) return error; }
        return "";
      }
      if (!validText(node.field)) return "Choose a supported condition field.";
      const fieldName = ruleCanonicalName(node.field), op = ruleCanonicalName(node.op);
      if (fieldName === "legacy") {
        if (!known(node, ["field", "source", "token", "condition"]) ||
          !validText(node.source) || !validText(node.token) || !validText(node.condition) ||
          !legacyTokens[ruleCanonicalName(node.source)]?.includes(ruleCanonicalName(node.token)))
          return "Unknown or invalid legacy source, field or condition; replace it to use the new editor.";
        return "";
      }
      const field = ruleFieldDefinition(fieldName);
      if (!field || !validText(node.op) || !ruleOperators(fieldName).includes(op)) return "Choose a supported field and comparison.";
      if (!known(node, ["field", "op", "value", "values", "min", "max"])) return "Unknown condition property.";
      const members = Object.keys(node).length;
      if (["set", "missing"].includes(op)) return members === 2 ? "" : "Set / missing comparisons do not take a value.";
      if (op === "between") return members === 4 && finiteNumber(node.min) && finiteNumber(node.max) && node.min <= node.max
        ? "" : "Enter a finite minimum and maximum; minimum must not exceed maximum.";
      if (["in", "not_in"].includes(op)) return members === 3 && Array.isArray(node.values) && node.values.length > 0 &&
        node.values.length <= RULE_LIMITS.values && node.values.every(value => validText(value))
        ? "" : "Enter between 1 and 128 non-empty list values, separated by commas.";
      if (members !== 3 || !Object.hasOwn(node, "value")) return "This comparison needs exactly one value.";
      return field.numeric ? (finiteNumber(node.value) ? "" : "Enter a finite numeric comparison value.")
        : (validText(node.value) ? "" : "Enter a non-empty value of at most 512 UTF-8 bytes, without control characters.");
    };
    if (!known(rule, ["name", "enabled", "stop_processing", "tag_type", "status", "statuses", "detail", "when", "effects"])) return "Invalid or unknown rule property.";
    if (Object.hasOwn(rule, "name") && !validText(rule.name, true)) return "Use a rule name of at most 512 UTF-8 bytes, without control characters.";
    for (const flag of ["enabled", "stop_processing"]) {
      if ((!allowDefaults || Object.hasOwn(rule, flag)) && typeof rule[flag] !== "boolean") return "Rule options must be booleans.";
    }
    for (const [key, values] of [["tag_type", ["any", "departure", "arrival", "airborne", "uncorrelated"]], ["detail", ["any", "normal", "detailed"]]]) {
      if ((!allowDefaults || Object.hasOwn(rule, key)) && (!validText(rule[key]) || !values.includes(ruleCanonicalName(rule[key])))) return "Choose a supported tag scope and detail.";
    }
    if (Object.hasOwn(rule, "status") && (!validText(rule.status) || !RULE_ALL_STATUSES.includes(ruleCanonicalName(rule.status)))) return "Unknown status scope.";
    if ((!allowDefaults || Object.hasOwn(rule, "statuses")) && (!Array.isArray(rule.statuses) || !rule.statuses.length ||
      rule.statuses.length > 32 || rule.statuses.some(status => !validText(status) || !RULE_ALL_STATUSES.includes(ruleCanonicalName(status))))) return "Choose at least one supported status.";
    const conditionError = checkNode(rule.when, 1);
    if (conditionError) return conditionError;
    if (!Array.isArray(rule.effects) || !rule.effects.length || rule.effects.length > RULE_LIMITS.effects) return "Choose between 1 and 32 effects.";
    for (const effect of rule.effects) {
      if (!known(effect, ["type", "field", "color", "value"]) || !validText(effect.type) ||
        !Object.hasOwn(RULE_EFFECT_LABELS, ruleCanonicalName(effect.type))) return "Choose a supported effect.";
      const type = ruleCanonicalName(effect.type), fieldEffect = type.startsWith("field_"), colorEffect = type.endsWith("_color") || type === "field_background";
      if (fieldEffect) {
        if (!validText(effect.field) || !RULE_EFFECT_FIELDS.includes(ruleCanonicalName(effect.field))) return "Choose a supported tag field for this effect.";
      } else if (Object.hasOwn(effect, "field")) return "Whole-tag / target effects do not take a field.";
      if (colorEffect) {
        if (Object.hasOwn(effect, "value") || !known(effect.color, ["r", "g", "b", "a"]) ||
          ["r", "g", "b"].some(channel => !Number.isInteger(effect.color[channel]) || effect.color[channel] < 0 || effect.color[channel] > 255) ||
          (Object.hasOwn(effect.color, "a") && (!Number.isInteger(effect.color.a) || effect.color.a < 0 || effect.color.a > 255)))
          return "Colors need integer RGB(A) channels between 0 and 255 and no other operands.";
      } else if (Object.hasOwn(effect, "color") || (Object.hasOwn(effect, "value") && typeof effect.value !== "boolean")) return "Bold / blink effects need a boolean value and no color.";
    }
    return "";
  }
  function ruleNodeAtPath(root, path) { return String(path || "").split("/").filter(Boolean).reduce((node, part) => node?.[part], root); }
  function replaceRuleNode(path, replacement) {
    if (!path) { drafts.rule.data.when = replacement; return; }
    const parts = path.split("/").filter(Boolean), key = parts.pop();
    const parent = ruleNodeAtPath(drafts.rule.data.when, parts.join("/"));
    if (parent) parent[key] = replacement;
  }
  function ruleConditionCount(node, depth = 1) {
    if (!node || typeof node !== "object" || depth > RULE_LIMITS.depth) return 0;
    if (Array.isArray(node.all)) return node.all.reduce((sum, child) => sum + ruleConditionCount(child, depth + 1), 0);
    if (Array.isArray(node.any)) return node.any.reduce((sum, child) => sum + ruleConditionCount(child, depth + 1), 0);
    if (node.not) return ruleConditionCount(node.not, depth + 1);
    return 1;
  }
  function selectedRuleStatuses(rule) {
    const valid = new Set([...RULE_STATUSES, ...RULE_COMPAT_STATUSES]);
    const normalizeStatus = status => {
      const raw = String(status || "").trim().toLowerCase(), compact = raw.replace(/[\s_-]+/g, ""), tagType = String(rule?.tag_type || "").trim().toLowerCase();
      if (!compact || compact === "any" || compact === "all" || compact === "*") return "any";
      if (["default", "def", "onground"].includes(compact)) return "default";
      if (["nostatus", "nsts"].includes(compact)) return rule?.when ? "nsts" : "default";
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
      if (compact === "arr") return rule?.when ? "arr" : "default";
      if (compact === "gate") return rule?.when ? "gate" : "default";
      if (compact === "arrival" || compact === "arrivals") return tagType === "arrival" ? "default" : "airarr";
      if (compact === "uncorrelated") return "default";
      return raw;
    };
    const explicitStatuses = Array.isArray(rule?.statuses) ? rule.statuses.map(normalizeStatus) : [];
    if (explicitStatuses.includes("any")) return RULE_STATUSES.slice();
    let statuses = explicitStatuses.filter(status => valid.has(status));
    if (!statuses.length) {
      const legacy = String(rule?.status || "any").trim().toLowerCase();
      if (!legacy || legacy === "any") statuses = RULE_STATUSES.slice();
      else {
        const legacyStatuses = legacy.replace(/\bline[\s_-]*up\b/g, "lnup").split(/[\s,;|]+/).map(normalizeStatus);
        if (legacyStatuses.includes("any")) return RULE_STATUSES.slice();
        statuses = legacyStatuses.filter(status => valid.has(status));
      }
    }
    return statuses.length ? uniqueValues(statuses) : RULE_STATUSES.slice();
  }
  function checkedRuleStatuses() {
    const options = $$("#ruleStatusOptions input[data-rule-status]");
    const selected = options.filter(input => input.checked);
    return options.length > 0 && selected.length === options.length ? ["any"] : selected.map(input => input.dataset.ruleStatus);
  }
  function updateRuleStatusDropdownLabel() {
    const button = $("#ruleStatusButton"), all = $("#ruleStatusAll"), options = $$("#ruleStatusOptions input[data-rule-status]"), selected = options.filter(input => input.checked);
    all.checked = selected.length === options.length && options.length > 0;
    all.indeterminate = selected.length > 0 && selected.length < options.length;
    if (!selected.length) button.textContent = "No statuses";
    else if (selected.length === options.length) button.textContent = "All statuses";
    else if (selected.length === 1) button.textContent = RULE_STATUS_LABELS[selected[0].dataset.ruleStatus] || humanize(selected[0].dataset.ruleStatus);
    else button.textContent = `${selected.length} statuses`;
    button.title = selected.length === options.length ? "All statuses selected" : selected.map(input => RULE_STATUS_LABELS[input.dataset.ruleStatus] || humanize(input.dataset.ruleStatus)).join(", ");
  }
  function renderRuleStatusSelector(rule, disabled = false) {
    const selected = new Set(selectedRuleStatuses(rule));
    const statuses = [...RULE_STATUSES, ...RULE_COMPAT_STATUSES.filter(status => selected.has(status))];
    $("#ruleStatusOptions").innerHTML = statuses.map(status => `<label role="option" aria-selected="${selected.has(status)}"><input type="checkbox" data-rule-status="${status}" ${selected.has(status) ? "checked" : ""}><span>${escapeHtml(RULE_STATUS_LABELS[status] || humanize(status))}</span></label>`).join("");
    $("#ruleStatusButton").disabled = disabled;
    $("#ruleStatusAll").disabled = disabled;
    $$("#ruleStatusOptions input").forEach(input => { input.disabled = disabled; });
    updateRuleStatusDropdownLabel();
  }
  function setRuleStatusMenuOpen(open) {
    const menu = $("#ruleStatusMenu"), button = $("#ruleStatusButton");
    if (!menu || !button) return;
    menu.hidden = !open;
    button.setAttribute("aria-expanded", String(open));
    $("#ruleStatusDropdown")?.classList.toggle("open", open);
  }
  function renderRules() {
    const items = rules();
    state.ui.selectedRuleIndex = items.length ? Math.min(items.length - 1, Math.max(0, state.ui.selectedRuleIndex)) : 0;
    const rows = items.map((rule, index) => {
      const selected = index === state.ui.selectedRuleIndex, count = rule?.when ? ruleConditionCount(rule.when) : (rule?.criteria?.length || 1);
      return `<button type="button" role="option" aria-selected="${selected}" class="${uiListRowClass("status", selected)} ${rule?.enabled === false ? "rule-disabled" : ""}" data-rule-index="${index}" title="${escapeHtml(ruleLabel(rule, index))}"><span class="ui-list__label">${escapeHtml(ruleLabel(rule, index))}</span><span class="ui-list__trailing rule-criteria-count" aria-label="${count} conditions">${count}</span></button>`;
    }).join("");
    $("#ruleList").innerHTML = rows ? `<div class="ui-list__items" role="presentation">${rows}</div>` : '<div class="ui-list__empty">No rules</div>';
    const hasSelection = items.length > 0;
    ["duplicate-rule", "delete-rule", "copy-rule"].forEach(action => { $(`[data-action="${action}"]`).disabled = !hasSelection; });
    $('[data-action="duplicate-rule"]').disabled ||= items.length >= RULE_LIMITS.rules;
    $('[data-action="new-rule"]').disabled = items.length >= RULE_LIMITS.rules;
    $('[data-action="move-rule-up"]').disabled = !hasSelection || state.ui.selectedRuleIndex === 0;
    $('[data-action="move-rule-down"]').disabled = !hasSelection || state.ui.selectedRuleIndex === items.length - 1;
    syncUiListFocus($("#ruleList"));
    renderRuleEditor();
  }
  function ruleConditionValueHtml(node) {
    const field = ruleFieldDefinition(node.field);
    if (["set", "missing"].includes(node.op)) return '<span class="rule-no-value">No value needed</span>';
    if (node.op === "between") return `<div class="rule-range-values"><input aria-label="Minimum value" data-field="min" type="number" step="any" value="${escapeHtml(node.min ?? "")}" placeholder="−5"/><span>to</span><input aria-label="Maximum value" data-field="max" type="number" step="any" value="${escapeHtml(node.max ?? "")}" placeholder="+5"/></div>`;
    if (["in", "not_in"].includes(node.op)) return `<input aria-label="Rule match values" data-field="values" type="text" spellcheck="false" maxlength="16384" value="${escapeHtml((node.values || []).join(", "))}" placeholder="Value 1, Value 2"/>`;
    return `<input aria-label="Rule comparison value" data-field="value" type="${field?.numeric ? "number" : "text"}" ${field?.numeric ? 'step="any"' : 'maxlength="4096"'} value="${escapeHtml(node.value ?? "")}" placeholder="${field?.time ? "Offset in minutes" : "Exact value"}"/>`;
  }
  function renderRuleNode(node, path = "", depth = 1) {
    if (!node || typeof node !== "object" || depth > RULE_LIMITS.depth) return '<p class="rule-help">Invalid condition; replace it with a template.</p>';
    const group = ["all", "any", "not"].find(key => Object.hasOwn(node, key));
    if (group) {
      const children = group === "not" ? [node.not] : (Array.isArray(node[group]) ? node[group] : []);
      return `<div class="rule-condition-group" data-node-path="${path}"><div class="rule-group-tools"><select aria-label="Condition group operator" data-field="group">${ruleSelectOptions(["all", "any", "not"], group, { all: "All of these (AND)", any: "Any of these (OR)", not: "Not (NOT)" })}</select>${group !== "not" ? `<button class="ui-button ui-button--compact" data-action="add-condition" data-node-path="${path}" type="button">+ Condition</button><button class="ui-button ui-button--compact" data-action="add-condition-group" data-node-path="${path}" type="button">+ Group</button>` : ""}${path ? `<button class="ui-button ui-button--compact ui-button--destructive" data-action="delete-condition" data-node-path="${path}" type="button" aria-label="Delete group">×</button>` : ""}</div><div class="rule-group-children">${children.map((child, index) => renderRuleNode(child, path + "/" + group + (group === "not" ? "" : "/" + index), depth + 1)).join("") || '<span class="rule-help">Add a condition to complete this group.</span>'}</div></div>`;
    }
    if (node.field === "legacy") return `<div class="criterion-row rule-legacy-condition" data-node-path="${path}"><div><strong>Legacy condition</strong><span>${escapeHtml(node.source)} · ${escapeHtml(node.token)} · ${escapeHtml(node.condition)}</span><small>Kept unchanged to preserve its original matching behavior.</small></div><button class="ui-button ui-button--compact" data-action="replace-legacy-condition" data-node-path="${path}" type="button">Replace</button><button class="ui-button ui-button--compact ui-button--destructive" data-action="delete-condition" data-node-path="${path}" type="button" aria-label="Delete condition">×</button></div>`;
    const fields = [...RULE_FIELDS, ...RULE_COMPAT_FIELDS.filter(field => field.id === node.field)];
    const labels = Object.fromEntries(fields.map(field => [field.id, field.label]));
    return `<div class="criterion-row" data-node-path="${path}"><select aria-label="Rule field" data-field="field">${ruleSelectOptions(fields.map(field => field.id), node.field, labels)}</select><select aria-label="Rule comparison" data-field="op">${ruleSelectOptions(ruleOperators(node.field), node.op, RULE_OPERATOR_LABELS)}</select><div class="rule-condition-value">${ruleConditionValueHtml(node)}</div><button type="button" aria-label="Delete condition" class="ui-button ui-button--compact ui-button--destructive criterion-delete" data-action="delete-condition" data-node-path="${path}">×</button></div>`;
  }
  function renderRuleEffect(effect, index) {
    const colorEffect = effect.type?.endsWith("_color") || effect.type === "field_background", color = colorToHex(effect.color, "#ffffff");
    return `<div class="rule-effect-row" data-effect-index="${index}"><select aria-label="Effect type" data-field="effect-type">${ruleSelectOptions(Object.keys(RULE_EFFECT_LABELS), effect.type, RULE_EFFECT_LABELS)}</select>${effect.type?.startsWith("field_") ? `<select aria-label="Effect tag field" data-field="effect-field">${ruleSelectOptions(RULE_EFFECT_FIELDS, effect.field)}</select>` : '<span class="rule-effect-scope">Whole target / tag</span>'}${colorEffect ? `<div class="rule-effect-color"><input aria-label="Effect color" class="rule-color-value" data-field="effect-color" type="text" value="${color.toUpperCase()}" maxlength="7"/><label class="mini-color-swatch" style="--swatch-color:${color}"><input aria-label="Pick effect color" data-field="effect-picker" type="color" value="${color}"/></label><input aria-label="Effect opacity (0 to 255)" data-field="effect-alpha" type="number" min="0" max="255" step="1" value="${effect.color?.a ?? 255}" title="Opacity: 0 transparent, 255 opaque"/></div>` : `<label class="check-field"><input aria-label="Effect enabled value" data-field="effect-value" type="checkbox" ${effect.value !== false ? "checked" : ""}/><span>On</span></label>`}<button class="ui-button ui-button--compact ui-button--destructive" data-action="delete-rule-effect" data-index="${index}" type="button" aria-label="Delete effect">×</button></div>`;
  }
  function renderRuleEditor() {
    const item = rules()[state.ui.selectedRuleIndex], disabled = !item;
    $("#ruleFormCaption").textContent = item ? ruleLabel(item, state.ui.selectedRuleIndex) : "Rule";
    $("#ruleEditorEmpty").hidden = !disabled;
    $("#ruleEditorForm").hidden = disabled;
    if (!item) {
      drafts.rule = null;
      $("#ruleName").value = "";
      $("#criteriaList").innerHTML = "";
      $("#ruleEffectsList").innerHTML = "";
      $("#ruleValidationMessage").hidden = true;
      renderRuleStatusSelector({ status: "any" }, true);
      $$("#ruleEditorForm input, #ruleEditorForm select, #ruleEditorForm button").forEach(control => { control.disabled = true; });
      clearUnappliedEditorSection($("#ruleName"));
      return;
    }
    const importedError = Object.hasOwn(item, "when") ? validateRuleEditorData(item, { allowDefaults: true }) : "";
    if (importedError) {
      // Existing malformed v2 data must not be silently repaired by focus,
      // naming, select fallbacks or autosave. Delete or paste replaces it
      // explicitly; the original profile item remains byte-for-byte intact.
      drafts.rule = { index: state.ui.selectedRuleIndex, data: clone(item), invalidSource: true };
      $("#ruleName").value = typeof item.name === "string" ? item.name : "";
      $("#criteriaList").innerHTML = '<p class="rule-help">This saved rule is invalid and cannot be edited safely. Delete it or paste a valid replacement. It has not been changed.</p>';
      $("#ruleEffectsList").innerHTML = "";
      renderRuleStatusSelector({ status: "any" }, true);
      $$("#ruleEditorForm input, #ruleEditorForm select, #ruleEditorForm button").forEach(control => { control.disabled = true; });
      ["copy-rule", "duplicate-rule", "move-rule-up", "move-rule-down"].forEach(action => { $(`[data-action="${action}"]`).disabled = true; });
      updateRuleValidationMessage(importedError);
      clearUnappliedEditorSection($("#ruleName"));
      return;
    }
    $$("#ruleEditorForm input, #ruleEditorForm select, #ruleEditorForm button").forEach(control => { control.disabled = false; });
    if (!drafts.rule || drafts.rule.index !== state.ui.selectedRuleIndex) drafts.rule = { index: state.ui.selectedRuleIndex, data: migrateRuleForEditor(item) };
    const rule = drafts.rule.data;
    $("#ruleName").value = rule.name || "";
    $("#ruleEnabled").checked = rule.enabled;
    $("#ruleStopProcessing").checked = rule.stop_processing;
    $("#criteriaList").innerHTML = renderRuleNode(rule.when);
    if (rule.when && !["all", "any", "not"].some(key => Object.hasOwn(rule.when, key))) {
      $("#criteriaList").insertAdjacentHTML("beforeend", '<div class="rule-group-tools"><button class="ui-button ui-button--compact" data-action="add-condition" data-node-path="" type="button">+ Condition</button><button class="ui-button ui-button--compact" data-action="add-condition-group" data-node-path="" type="button">+ Group</button></div>');
    }
    $("#ruleEffectsList").innerHTML = (rule.effects || []).map(renderRuleEffect).join("");
    $('[data-action="add-rule-effect"]').disabled = (rule.effects || []).length >= RULE_LIMITS.effects;
    ensureSelectValue($("#ruleTagType"), rule.tag_type || "any");
    renderRuleStatusSelector(rule);
    ensureSelectValue($("#ruleDetail"), rule.detail || "any");
    updateRuleValidationMessage(validateRuleEditorData(rule));
  }
  function updateRuleValidationMessage(message) {
    const control = $("#ruleValidationMessage");
    control.textContent = message ? `Not saved: ${message}` : "";
    control.hidden = !message;
  }
  function captureRuleDraft() {
    if (!drafts.rule || drafts.rule.invalidSource) return null;
    const rule = drafts.rule.data;
    $$("#criteriaList .criterion-row:not(.rule-legacy-condition)").forEach(row => {
      if (!ruleNodeAtPath(rule.when, row.dataset.nodePath)) return;
      const field = $("[data-field='field']", row)?.value, op = $("[data-field='op']", row)?.value, node = { field, op }, numeric = ruleFieldDefinition(field)?.numeric;
      const numericValue = input => input && input.value.trim() !== "" ? Number(input.value) : null;
      if (op === "between") { node.min = numericValue($("[data-field='min']", row)); node.max = numericValue($("[data-field='max']", row)); }
      else if (["in", "not_in"].includes(op)) node.values = String($("[data-field='values']", row)?.value || "").split(/[,;\n]+/).map(value => value.trim()).filter(Boolean);
      else if (!["set", "missing"].includes(op)) node.value = numeric ? numericValue($("[data-field='value']", row)) : String($("[data-field='value']", row)?.value || "").trim();
      replaceRuleNode(row.dataset.nodePath, node);
    });
    rule.effects = $$("#ruleEffectsList .rule-effect-row").map(row => {
      const type = $("[data-field='effect-type']", row).value, effect = { type };
      if (type.startsWith("field_")) effect.field = $("[data-field='effect-field']", row)?.value || "callsign";
      if (type.endsWith("_color") || type === "field_background") {
        const raw = $("[data-field='effect-color']", row)?.value || "", alpha = $("[data-field='effect-alpha']", row)?.value || "";
        effect.color = /^#?[0-9a-f]{6}$/i.test(raw.trim()) && /^\d{1,3}$/.test(alpha) ? { ...hexToColor(raw), a: Number(alpha) } : null;
      } else effect.value = Boolean($("[data-field='effect-value']", row)?.checked);
      return effect;
    });
    const name = $("#ruleName").value.trim();
    if (name) rule.name = name; else delete rule.name;
    rule.enabled = $("#ruleEnabled").checked;
    rule.stop_processing = $("#ruleStopProcessing").checked;
    rule.tag_type = $("#ruleTagType").value;
    rule.statuses = checkedRuleStatuses();
    delete rule.status;
    rule.detail = $("#ruleDetail").value;
    return rule;
  }
  function applyRule({ render = true } = {}) {
    const item = rules()[state.ui.selectedRuleIndex];
    if (!item || !drafts.rule) { clearUnappliedEditorSection($("#ruleName")); return true; }
    if (drafts.rule.invalidSource) { clearUnappliedEditorSection($("#ruleName")); return true; }
    const rule = captureRuleDraft(), error = validateRuleEditorData(rule);
    updateRuleValidationMessage(error);
    if (error) return false;
    activeProfile().rules.version = 2;
    rules()[state.ui.selectedRuleIndex] = clone(rule);
    const row = $(`[data-rule-index="${state.ui.selectedRuleIndex}"]`);
    row?.classList.toggle("rule-disabled", rule.enabled === false);
    const countControl = row && $(".rule-criteria-count", row);
    if (countControl) {
      const count = ruleConditionCount(rule.when);
      countControl.textContent = String(count);
      countControl.setAttribute("aria-label", `${count} conditions`);
    }
    clearUnappliedEditorSection($("#ruleName"));
    markDirty("Rule updated", ["profiles"]);
    if (render) renderRules();
    return true;
  }
  function normalizeClipboardRule(value) {
    const source = value?.rule ?? value;
    // Validate supplied v2 data before applying native-compatible optional defaults.
    if (source && Object.hasOwn(source, "when") && validateRuleEditorData(source, { allowDefaults: true })) return null;
    const rule = migrateRuleForEditor(source);
    return rule && !validateRuleEditorData(rule) ? rule : null;
  }
  async function copyRule() {
    const item = rules()[state.ui.selectedRuleIndex];
    if (!item || drafts.rule?.invalidSource) return;
    const rule = captureRuleDraft() || migrateRuleForEditor(item), error = validateRuleEditorData(rule);
    if (error) { showToast(error, "error"); return; }
    await writeEditorClipboard(JSON.stringify({ vsmr: "rule", version: 2, rule }, null, 2), "rule");
    showToast("Rule copied", "success");
  }
  async function pasteRule() {
    const raw = String(await readEditorClipboard("rule", "Paste a vSMR rule") || "").trim();
    if (!raw) return;
    let parsed;
    try { if (raw.length > 256 * 1024) throw new Error("Rule is too large"); parsed = JSON.parse(raw); }
    catch (error) { showToast("Clipboard does not contain a vSMR rule", "error"); return; }
    const rule = normalizeClipboardRule(parsed);
    if (!rule) { showToast("Clipboard does not contain a valid vSMR rule", "error"); return; }
    const items = rules();
    if (items.length) items[state.ui.selectedRuleIndex] = rule;
    else { items.push(rule); state.ui.selectedRuleIndex = 0; }
    activeProfile().rules.version = 2;
    drafts.rule = null;
    clearUnappliedEditorSection($("#ruleName"));
    markDirty("Rule pasted", ["profiles"]);
    renderRules();
    showToast("Rule pasted", "success");
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
    const rows = items.map((mode, index) => {
      const selected = index === state.ui.selectedModeIndex;
      return `<button type="button" role="option" aria-selected="${selected}" class="${uiListRowClass("status", selected)}" data-mode-index="${index}"><span class="ui-list__label">${escapeHtml(mode.name || `Mode ${index + 1}`)}</span><span class="ui-list__trailing mode-active-mark">${mode.name === activeName ? "●" : ""}</span></button>`;
    }).join("");
    $("#modeList").innerHTML = rows ? `<div class="ui-list__items" role="presentation">${rows}</div>` : `<div class="ui-list__empty">No modes</div>`;
    syncUiListFocus($("#modeList"));
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
    if (typeof data.statuses.parked !== "boolean") data.statuses.parked = true;
    if (typeof data.statuses.lineup !== "boolean")
      data.statuses.lineup = typeof data.statuses.lnup === "boolean" ? data.statuses.lnup : (typeof data.statuses.taxi === "boolean" ? data.statuses.taxi : true);
    delete data.statuses.lnup;
    $("#reqSquawk").checked = Boolean(data.require_assigned_squawk);
    $("#modeAcceptPilotSquawk").checked = data.accept_pilot_squawk !== false;
    $("#reqClearance").checked = Boolean(data.require_clearance);
	$("#reqTsat").checked = Boolean(data.require_valid_tsat);
	$("#reqTobt").checked = Boolean(data.require_active_tobt);
	$("#reqReady").checked = Boolean(data.require_ready);
    $("#modeTowerFilter").checked = Boolean(data.tower_filter ?? data.tower_mode);
    $("#modeStructuredRules").checked = data.structured_rules !== false && data.structured_rules_enabled !== false;
    $("#modeMaxAirborneAltitude").value = String(Math.round(clamp(data.max_airborne_altitude_ft ?? 5500, 0, 60000)));
    $("#modeMaxAirborneSpeed").value = String(Math.round(clamp(data.max_airborne_speed_kt ?? 250, 0, 1000)));
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
	mode.require_ready = $("#reqReady").checked;
    mode.tower_filter = $("#modeTowerFilter").checked;
    mode.structured_rules = $("#modeStructuredRules").checked;
    mode.max_airborne_altitude_ft = Math.round(clamp(Number($("#modeMaxAirborneAltitude").value), 0, 60000));
    mode.max_airborne_speed_kt = Math.round(clamp(Number($("#modeMaxAirborneSpeed").value), 0, 1000));
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
    const rows = state.profiles.map(record => {
      const selected = record.id === state.ui.managedProfileId;
      return `<button type="button" role="option" aria-selected="${selected}" class="${uiListRowClass("status", selected)}" data-managed-profile-id="${escapeHtml(record.id)}"><span class="ui-list__label">${escapeHtml(record.data.name)}</span><span class="ui-list__trailing profile-active-mark">${record.id === state.activeProfileId ? "●" : ""}</span></button>`;
    }).join("");
    $("#profileList").innerHTML = rows ? `<div class="ui-list__items" role="presentation">${rows}</div>` : `<div class="ui-list__empty">No profiles</div>`;
    syncUiListFocus($("#profileList"));
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
