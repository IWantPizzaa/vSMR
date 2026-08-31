"use strict";

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
        dirty: state.dirty || hasUnappliedEditorInputs()
      });
      return;
    }
    state.ui.controlCenterOpen = false;
    syncSurfaceVisibility();
    renderRuntimeMenu();
  }

  function closeControlCenter() {
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
        ? `<div class="ui-list__items runtime-choice-box">${modes.map(mode => {
            const selected = mode.name === active;
            return `<button type="button" class="${uiListRowClass("runtime", selected, false, "runtime-choice-row runtime-compact-row")}" data-runtime-mode="${escapeHtml(mode.name)}">${runtimeSelectionIcon(selected)}<strong class="ui-list__label runtime-row-label">${escapeHtml(mode.name)}</strong></button>`;
          }).join("")}</div>`
        : `<div class="runtime-popover-empty">No modes in this profile.</div>`;
      return;
    }

    if (kind === "profile") {
      title.textContent = "Profile";
      content.innerHTML = `<div class="ui-list__items runtime-choice-box">${state.profiles.map(record => {
        const active = record.id === state.activeProfileId;
        return `<button type="button" class="${uiListRowClass("runtime", active, false, "runtime-choice-row runtime-compact-row")}" data-runtime-profile="${escapeHtml(record.id)}">${runtimeSelectionIcon(active)}<strong class="ui-list__label runtime-row-label">${escapeHtml(record.data.name)}</strong></button>`;
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
        return `<button type="button" class="${uiListRowClass("runtime", false, false, "runtime-choice-row runtime-compact-row runtime-inset-row")}" data-runtime-inset="${id}">${runtimeVisibilityIcon(visible)}<strong class="ui-list__label runtime-row-label">${label}</strong></button>`;
      }).join("");
      const presetRows = presets.map(preset => {
        const active = preset.name === activePreset?.name;
        return `<button type="button" class="${uiListRowClass("runtime", active, false, "runtime-choice-row runtime-compact-row runtime-preset-row")}" data-inset-preset="${escapeHtml(preset.name)}">${runtimeSelectionIcon(active)}<strong class="ui-list__label runtime-row-label">${escapeHtml(preset.name)}</strong></button>`;
      }).join("");
      content.innerHTML = `<div class="ui-list__items runtime-choice-box">${insetRows}</div>
        <div class="runtime-section-heading"><span>Preset</span></div>
        ${presetRows ? `<div class="ui-list__items runtime-choice-box runtime-preset-list">${presetRows}</div>` : `<div class="runtime-popover-empty">No inset presets.</div>`}
        <label class="runtime-linked-toggle"><input type="checkbox" id="runtimePresetLinked" ${activePreset?.linked_movement ? "checked" : ""} ${activePreset ? "" : "disabled"}><span>Linked movement</span></label>
        <div class="runtime-preset-actions">
          <button class="ui-button ui-button--primary" data-action="save-inset-preset" type="button">Save…</button>
          <button class="ui-button" data-action="update-inset-preset" type="button" ${activePreset ? "" : "disabled"}>Update</button>
          <button class="ui-button" data-action="reset-inset-preset" type="button" ${activePreset ? "" : "disabled"}>Reset</button>
          <button class="ui-button" data-action="rename-inset-preset" type="button" ${activePreset ? "" : "disabled"}>Rename…</button>
          <button class="ui-button" data-action="duplicate-inset-preset" type="button" ${activePreset ? "" : "disabled"}>Duplicate</button>
          <button class="ui-button" data-action="default-inset-preset" type="button" ${activePreset ? "" : "disabled"}>${activePreset?.name === store.default ? "Default ✓" : "Set default"}</button>
          <button class="ui-button ui-button--destructive runtime-preset-delete" data-action="delete-inset-preset" type="button" ${activePreset ? "" : "disabled"}>Delete</button>
        </div>`;
      return;
    }

    title.textContent = "Groups";
    const groupRows = avisoGroups().map(group => {
      const visible = group.visible !== false;
      return `<button type="button" class="${uiListRowClass("runtime", false, false, `runtime-choice-row runtime-compact-row ${visible ? "" : "muted"}`)}" data-runtime-group="${escapeHtml(group.id)}">${runtimeVisibilityIcon(visible)}<strong class="ui-list__label runtime-row-label">${escapeHtml(group.name)}</strong></button>`;
    }).join("");
    content.innerHTML = groupRows
      ? `<div class="ui-list__items runtime-choice-box">${groupRows}</div>`
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
	if (state.dirty || hasUnappliedEditorInputs()) {
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
