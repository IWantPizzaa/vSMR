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

    const trailClass = style === "nova" ? "nova" : style === "realistic" ? "realistic" : "triangle";
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
    if (normalizedSource === "runway" || normalizedSource === "custom")
      values = ["any", "set", "missing", "in", "not_in"];
    else if (normalizedToken === "tobt")
      values = ["any", "set", "missing", "inactive", "unconfirmed", "confirmed", "unconfirmed_delay", "confirmed_delay", "expired"];
    else if (normalizedToken === "tsat")
      values = ["any", "set", "missing", "inactive", "future", "valid", "expired", "future_ctot", "valid_ctot", "expired_ctot"];
    else
      values = ["any", "set", "missing", "future", "past"];

    if (String(selected || "").trim()) values.push(String(selected).trim());
    return uniqueValues(values);
  }
  function parseRuleCondition(source, condition) {
    const normalizedSource = normalizeRuleSourceUi(source);
    const raw = String(condition || "").trim();
    if (normalizedSource === "vacdm") return { operator: raw || "any", values: "" };
    const simple = raw.toLowerCase();
    if (["any", "set", "missing"].includes(simple)) return { operator: simple, values: "" };
    const list = raw.match(/^(not_in|notin|not|in|list|sid)\s*:\s*(.*)$/i);
    if (list) {
      const operator = ["not_in", "notin", "not"].includes(list[1].toLowerCase()) ? "not_in" : "in";
      return { operator, values: list[2].trim() };
    }
    return { operator: "in", values: raw };
  }
  function composeRuleCondition(source, operator, values) {
    if (normalizeRuleSourceUi(source) === "vacdm") return String(operator || "any").trim();
    const normalizedOperator = String(operator || "any").trim().toLowerCase();
    if (!["in", "not_in"].includes(normalizedOperator)) return normalizedOperator || "any";
    const list = String(values || "").trim();
    return list ? `${normalizedOperator}: ${list}` : normalizedOperator;
  }
  function updateRuleConditionValueControl(row) {
    const source = $("[data-field='source']", row)?.value || "vacdm";
    const operator = $("[data-field='condition']", row)?.value || "any";
    const input = $("[data-field='condition-values']", row);
    if (!input) return;
    const acceptsValues = normalizeRuleSourceUi(source) !== "vacdm" && ["in", "not_in"].includes(operator);
    input.disabled = !acceptsValues;
    input.placeholder = acceptsValues
      ? (normalizeRuleSourceUi(source) === "runway" ? "09L, 27R" : "SID1X, SID2A")
      : "";
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
    const rows = items.map((rule, index) => {
      const selected = index === state.ui.selectedRuleIndex;
      const criteriaCount = Array.isArray(rule.criteria) && rule.criteria.length ? rule.criteria.length : 1;
      return `<button type="button" role="option" aria-selected="${selected}" class="${uiListRowClass("status", selected)}" data-rule-index="${index}" title="${escapeHtml(ruleLabel(rule, index))}"><span class="ui-list__label">${escapeHtml(ruleLabel(rule, index))}</span><span class="ui-list__trailing rule-criteria-count" aria-label="${criteriaCount} ${criteriaCount === 1 ? "condition" : "conditions"}">${criteriaCount}</span></button>`;
    }).join("");
    $("#ruleList").innerHTML = rows ? `<div class="ui-list__items" role="presentation">${rows}</div>` : `<div class="ui-list__empty">No rules</div>`;
    const hasSelection = items.length > 0;
    $('[data-action="duplicate-rule"]').disabled = !hasSelection;
    $('[data-action="delete-rule"]').disabled = !hasSelection;
	$('[data-action="copy-rule"]').disabled = !hasSelection;
    syncUiListFocus($("#ruleList"));
    renderRuleEditor();
  }
  function renderRuleEditor() {
    const item = rules()[state.ui.selectedRuleIndex];
    const disabled = !item;
    $("#ruleFormCaption").textContent = item ? ruleLabel(item, state.ui.selectedRuleIndex) : "Rule";
    $("#ruleEditorEmpty").hidden = !disabled;
    $("#ruleEditorForm").hidden = disabled;
    if (!item) {
      drafts.rule = null;
      $("#ruleName").value = "";
      $("#criteriaList").innerHTML = "";
      renderRuleStatusSelector({ status: "any" }, true);
      $$("#ruleEditorForm input, #ruleEditorForm select, #ruleEditorForm button").forEach(control => { control.disabled = true; });
      clearUnappliedEditorSection($("#ruleName"));
      return;
    }
    $$("#ruleEditorForm input, #ruleEditorForm select, #ruleEditorForm button").forEach(control => { control.disabled = false; });
    if (!drafts.rule || drafts.rule.index !== state.ui.selectedRuleIndex) drafts.rule = { index: state.ui.selectedRuleIndex, data: clone(item) };
    const rule = drafts.rule.data;
    $("#ruleName").value = rule.name || "";
    const criteria = Array.isArray(rule.criteria) && rule.criteria.length ? rule.criteria : [{ source: rule.source || "vacdm", token: rule.token || "", condition: rule.condition || "" }];
    $("#criteriaList").innerHTML = criteria.map((criterion, index) => {
      const parsedCondition = parseRuleCondition(criterion.source, criterion.condition);
      return `
      <div class="criterion-row" data-criterion-index="${index}">
        <select aria-label="Rule source" data-field="source">${ruleSelectOptions(RULE_SOURCES, normalizeRuleSourceUi(criterion.source), RULE_SOURCE_LABELS)}</select>
        <select aria-label="Rule token" data-field="token">${ruleSelectOptions(ruleTokensForSource(criterion.source), criterion.token)}</select>
        <select aria-label="Rule condition" data-field="condition">${ruleSelectOptions(ruleConditionsFor(criterion.source, criterion.token, parsedCondition.operator), parsedCondition.operator, { not_in: "not in" })}</select>
        <input aria-label="Rule match values" data-field="condition-values" spellcheck="false" type="text" value="${escapeHtml(parsedCondition.values)}"/>
        <button type="button" aria-label="Delete condition" class="ui-button ui-button--compact ui-button--icon ui-button--destructive criterion-delete" data-action="delete-condition" data-index="${index}" title="Delete condition"><svg aria-hidden="true" viewBox="0 0 24 24"><path d="M5 5l14 14M19 5L5 19"/></svg></button>
      </div>`;
    }).join("");
    $$("#criteriaList .criterion-row").forEach(updateRuleConditionValueControl);
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
    const criteria = $$("#criteriaList .criterion-row").map(row => {
      const source = $("[data-field='source']", row).value;
      return {
        source,
        token: $("[data-field='token']", row).value.trim(),
        condition: composeRuleCondition(
          source,
          $("[data-field='condition']", row).value,
          $("[data-field='condition-values']", row).value
        )
      };
    }).filter(criterion => criterion.source || criterion.token || criterion.condition);
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
    if (!item || !drafts.rule) {
      clearUnappliedEditorSection($("#ruleName"));
      return true;
    }
    const rule = captureRuleDraft();
    rules()[state.ui.selectedRuleIndex] = clone(rule);
    clearUnappliedEditorSection($("#ruleName"));
    markDirty("Rule updated", ["profiles"]);
    if (render) renderRules();
    return true;
  }

  function normalizeClipboardRule(value) {
    const sourceRule = value?.rule ?? value;
    if (!sourceRule || typeof sourceRule !== "object" || Array.isArray(sourceRule)) return null;
    const rawCriteria = Array.isArray(sourceRule.criteria) && sourceRule.criteria.length
      ? sourceRule.criteria
      : [{ source: sourceRule.source, token: sourceRule.token, condition: sourceRule.condition }];
    const criteria = rawCriteria.filter(item => item && typeof item === "object").map(item => {
      const source = normalizeRuleSourceUi(item.source);
      const tokens = ruleTokensForSource(source);
      const requestedToken = String(item.token || "").trim().toLowerCase();
      const token = tokens.includes(requestedToken) ? requestedToken : tokens[0];
      const parsed = parseRuleCondition(source, item.condition);
      const operators = ruleConditionsFor(source, token);
      const operator = operators.includes(parsed.operator) ? parsed.operator : "any";
      return { source, token, condition: composeRuleCondition(source, operator, parsed.values) };
    });
    if (!criteria.length) return null;

    const tagTypes = ["any", "departure", "arrival", "airborne", "uncorrelated"];
    const details = ["any", "normal", "detailed"];
    const normalized = {
      criteria,
      source: criteria[0].source,
      token: criteria[0].token,
      condition: criteria[0].condition,
      tag_type: tagTypes.includes(sourceRule.tag_type) ? sourceRule.tag_type : "any",
      detail: details.includes(sourceRule.detail) ? sourceRule.detail : "any"
    };
    const name = String(sourceRule.name || "").trim();
    if (name) normalized.name = name;
    normalized.statuses = selectedRuleStatuses(sourceRule);
    normalized.status = normalized.statuses.length === 1 ? normalized.statuses[0] : "any";
    ["target_color", "tag_color", "text_color"].forEach(key => {
      if (!isColorObject(sourceRule[key])) return;
      normalized[key] = {
        r: Math.round(clamp(sourceRule[key].r, 0, 255)),
        g: Math.round(clamp(sourceRule[key].g, 0, 255)),
        b: Math.round(clamp(sourceRule[key].b, 0, 255)),
        a: Math.round(clamp(sourceRule[key].a ?? 255, 0, 255))
      };
    });
    return normalized;
  }

  async function copyRule() {
    const item = rules()[state.ui.selectedRuleIndex];
    if (!item) return;
    captureRuleDraft();
    const rule = drafts.rule?.data || item;
    await writeEditorClipboard(JSON.stringify({ vsmr: "rule", version: 1, rule }, null, 2), "rule");
    showToast("Rule copied", "success");
  }

  async function pasteRule() {
    const raw = String(await readEditorClipboard("rule", "Paste a vSMR rule") || "").trim();
    if (!raw) return;
    let parsed;
    try { parsed = JSON.parse(raw); }
    catch (error) {
      showToast("Clipboard does not contain a vSMR rule", "error");
      return;
    }
    const rule = normalizeClipboardRule(parsed);
    if (!rule) {
      showToast("Clipboard does not contain a valid vSMR rule", "error");
      return;
    }
    const items = rules();
    if (items.length) items[state.ui.selectedRuleIndex] = rule;
    else {
      items.push(rule);
      state.ui.selectedRuleIndex = 0;
    }
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
