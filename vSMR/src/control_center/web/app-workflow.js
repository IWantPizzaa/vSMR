"use strict";

  async function writeEditorClipboard(text, kind) {
    editorClipboard[kind] = text;
    try {
      if (navigator.clipboard?.writeText) await navigator.clipboard.writeText(text);
    } catch (error) {
      console.warn("System clipboard write was unavailable; using the vSMR clipboard", error);
    }
  }

  async function readEditorClipboard(kind, promptLabel) {
    try {
      if (navigator.clipboard?.readText) {
        const text = await navigator.clipboard.readText();
        if (String(text || "").trim()) return text;
      }
    } catch (error) {
      console.warn("System clipboard read was unavailable; using the vSMR clipboard", error);
    }
    if (editorClipboard[kind]) return editorClipboard[kind];
    return window.prompt(promptLabel, "") || "";
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
    // never blocks automatic saving or navigation.
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
    [savedSnapshot].forEach(snapshot => {
      if (!snapshot?.metadata) return;
      try {
        const metadata = JSON.parse(snapshot.metadata);
        if (assignAvisoPresetRoot(metadata, root)) {
          snapshot.metadata = JSON.stringify(metadata);
        }
      } catch (error) {
        console.warn("Could not rebase inset presets in the saved editor snapshot", error);
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
    [savedSnapshot].forEach(snapshot => {
      if (!snapshot?.profiles) return;
      try {
        const records = JSON.parse(snapshot.profiles);
        if (applySelections(records)) snapshot.profiles = JSON.stringify(records);
      } catch (error) {
        console.warn("Could not rebase display mode selections in the saved editor snapshot", error);
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
        if (slot === "save") saveInFlight = null;
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

  function captureEditorSnapshot() {
    const persistedSettings = clone(state.settings);
    delete persistedSettings.profileFile;
    delete persistedSettings.avisoFile;
    delete persistedSettings.dataHealth;
    const values = {
      profiles: state.profiles,
      metadata: state.metadata,
      profileExtras: state.profileExtras,
      aviso: state.aviso,
      settings: persistedSettings
    };
    const snapshot = {};
    Object.entries(values).forEach(([key, value]) => {
      snapshot[key] = snapshotChunk(value);
    });
    return snapshot;
  }

  function editorSnapshotsEqual(left, right) {
    if (!left || !right) return false;
    return ["profiles", "metadata", "profileExtras", "aviso", "settings"]
      .every(key => left[key] === right[key]);
  }

  function resourceHasUnsavedChanges(resource) {
    if (!savedSnapshot) return Boolean(state.dirty);
    if (resource === "aviso")
      return snapshotChunk(state.aviso) !== savedSnapshot.aviso;
    if (resource === "profiles") {
      const current = captureEditorSnapshot();
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
	  runtimeCommandPending.size || splitAvisoContext
	);
    const reloadButton = $("#reloadButton");
    if (reloadButton) {
	  reloadButton.disabled = !hostAuthoritativeReady || busy;
      reloadButton.classList.toggle("pending", Boolean(pending.reload));
      reloadButton.title = pending.reload ? "Reverting…" : "Revert to saved configuration";
    }
    updateWorkspaceInterlock();
  }

  function setHostAuthoritativeReady(ready) {
    hostAuthoritativeReady = !HOST_MODE || Boolean(ready);
    const overlay = $("#hostLoadingOverlay");
    if (overlay) overlay.hidden = hostAuthoritativeReady;
    updateCommandState();
  }

  function updateDirtyState(message = "") {
    state.dirty = !editorSnapshotsEqual(captureEditorSnapshot(), savedSnapshot);
    const visuallyDirty = state.dirty || hasUnappliedEditorInputs();
    updateCommandState();
    if (message) setStatus(message, visuallyDirty ? "info" : "");
  }

  function resetSavedSnapshot() {
    savedSnapshot = captureEditorSnapshot();
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
      button.className = "ui-button ui-button--compact";
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
      if (health.profilesBackupAvailable) actions.push({ label: "Restore legacy .bak", action: "restore-profiles-backup" });
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
    document.documentElement.dataset.status = type || "ready";
    if (type === "error") setPersistentStatus(message, "error", [], "native");
  }
  function markDirty(message = "Saving automatically…") {
    state.dirty = true;
    updateCommandState();
    if (message) setStatus(message, "info");
    scheduleAutosave();
  }

  function markSaved(message = "Saved") {
    state.profiles.forEach(record => {
      record.original = clone(record.data);
      record.persistedName = String(record.data?.name || "");
    });
    savedSnapshot = captureEditorSnapshot();
    state.dirty = false;
    state.recoveryConfirmed = false;
    state.avisoRecoveryConfirmed = false;
    state.externalEditConflict = false;
    updateDirtyState();
    setStatus(message);
    if (!hasAutosaveWork()) cancelAutosave();
    else scheduleAutosave();
  }

  function setPage(page) {
    if (!PAGE_TITLES[page]) return;
    state.ui.page = page;
    $$(".rail-button[data-page]").forEach(button => button.classList.toggle("active", button.dataset.page === page));
    $$('[data-page-panel]').forEach(panel => panel.classList.toggle("active", panel.dataset.pagePanel === page));
    if (page === "display") renderCurrentProfileTab();
    if (page === "aviso") renderAviso();
    if (page === "alerts") renderAlerts();
    if (page === "groups") renderAvisoGroups();
    if (page === "modes") renderModes();
    if (page === "profiles") renderProfilesManager();
    if (page === "settings") {
      renderSettings();
      renderUpdateCenter();
      requestUpdateState(true);
    }
  }

  function setProfileTab(tab) {
    if (!PROFILE_TITLES[tab]) return;
    state.ui.profileTab = tab;
    syncProfileTabSelection();
    renderCurrentProfileTab();
  }

  function syncProfileTabSelection() {
    const tab = state.ui.profileTab;
    syncTabButtons('[data-profile-tab]', tab, "profileTab");
    $$('[data-profile-panel]').forEach(panel => panel.classList.toggle("active", panel.dataset.profilePanel === tab));
  }

  function switchActiveProfile(profileId, syncFilters = true) {
    if (!state.profiles.some(record => record.id === profileId)) return false;
    if (profileId === state.activeProfileId) return true;
	if (state.dirty || hasUnappliedEditorInputs()) {
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
      drafts: clone(drafts),
      savedSnapshot,
      dirty: state.dirty,
      unappliedEditorSections: Array.from(unappliedEditorSections),
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
    Object.keys(drafts).forEach(key => { drafts[key] = rollback.drafts?.[key] ?? null; });
    savedSnapshot = rollback.savedSnapshot;
    state.dirty = rollback.dirty;
    unappliedEditorSections.clear();
    (rollback.unappliedEditorSections || []).forEach(key => unappliedEditorSections.add(String(key)));
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
      unappliedAtSend: hasUnappliedEditorInputs()
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
        !pendingCommand.unappliedAtSend && !hasUnappliedEditorInputs()
    };
  }

  function pendingRuntimeCommandInfo(requestId) {
    const pendingCommand = runtimeCommandPending.get(requestId);
    if (!pendingCommand) return null;
    return {
      type: pendingCommand.type,
      trustedCleanResponse: !pendingCommand.rollback?.dirty &&
        !pendingCommand.unappliedAtSend && !hasUnappliedEditorInputs()
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
