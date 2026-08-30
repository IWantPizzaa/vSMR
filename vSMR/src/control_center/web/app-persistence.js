"use strict";

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
    return state.dirty || hasUnappliedEditorInputs();
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
      splitAvisoContext
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
    // represented in the current AVISO snapshot and will still be included.
    if (savedSnapshot && snapshotChunk(state.aviso) === savedSnapshot.aviso)
      delete payload.aviso;
    const submittedSnapshot = captureEditorSnapshot();
    const requestId = postBridge("state.save", payload);
    if (!requestId) return false;
    pending.save = requestId;
    saveInFlight = { requestId, snapshot: submittedSnapshot };
    armPendingTimeout("save", pending.save);
    setStatus("Saving configuration…", "info");
    updateCommandState();
    if (!HOST_MODE) {
      setTimeout(() => receiveHostMessage({
        version: PROTOCOL_VERSION,
        id: requestId,
        type: "state.saved",
        payload: {
          requestId,
          configRevision: state.configRevision || "preview-config",
          avisoRevision: state.avisoRevision || "preview-aviso",
          message: "Preview state saved"
        }
      }), 0);
    }
    return true;
  }

  function finishConfigurationSave(message, payload = {}) {
    if (!pending.save || !messageMatchesRequest(message, pending.save)) return false;
    const operation = saveInFlight;
    pending.save = "";
    saveInFlight = null;

    // The acknowledgement only advances durable revision tokens and host-owned
    // file metadata. The browser model is already newer-or-equal to the data
    // written by native code and must never be replaced by an echo response.
    if (typeof payload.configRevision === "string" && payload.configRevision)
      state.configRevision = payload.configRevision;
    if (typeof payload.avisoRevision === "string" && payload.avisoRevision)
      state.avisoRevision = payload.avisoRevision;
    if (payload.settings && typeof payload.settings === "object" && !Array.isArray(payload.settings)) {
      ["profileFile", "avisoFile", "aliasFile", "dataHealth"].forEach(key => {
        if (Object.hasOwn(payload.settings, key)) state.settings[key] = clone(payload.settings[key]);
      });
    }

    if (operation?.snapshot) savedSnapshot = operation.snapshot;
    const currentSnapshot = captureEditorSnapshot();
    const hasNewerEdits = !operation?.snapshot ||
      !editorSnapshotsEqual(currentSnapshot, operation.snapshot) || hasUnappliedEditorInputs();
    if (hasNewerEdits) {
      updateDirtyState("Saved previous changes; newer edits are queued…");
      scheduleAutosave(0);
    } else {
      markSaved(payload.message || "Configuration saved");
    }
    updateCommandState();
    return true;
  }

  function requestReload() {
	if (pending.reload || pending.save || pending.resource || runtimeCommandPending.size || splitAvisoContext) return;
    const couldNotStageDraft = !stageFocusedEditorValue();
    const hasUnsaved = state.dirty || hasUnappliedEditorInputs();
    if (couldNotStageDraft) {
      if (!window.confirm("Some current editor fields are invalid or unfinished. Discard them and all unsaved changes, then reload from disk?")) return;
    } else if (hasUnsaved && !window.confirm("Discard unsaved changes and reload configuration from disk?")) {
      return;
    }
    cancelAutosave();
    pending.reload = postBridge("state.reload", {});
    if (!pending.reload) return;
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

  function saveAll({ automatic = false } = {}) {
    if (automatic) {
      const active = document.activeElement;
      if (!stageEditorControl(active) || (active?.checkValidity && !active.checkValidity())) return false;
      if (hasUnappliedEditorInputs()) return false;
    } else if (!stageFocusedEditorValue()) return false;
    if (!state.dirty) return true;
    return startConfigurationSave();
  }

  function restoreProfilesBackup() {
	if (pending.reload || pending.save || pending.resource || runtimeCommandPending.size || splitAvisoContext || !state.settings?.dataHealth?.profilesBackupAvailable) return;
    const backupDescription = describeLegacyProfilesBackup(state.settings.dataHealth);
    if (!window.confirm(`${backupDescription}. Current vSMR saves do not update this legacy copy, so it may be older than expected. Restore it and discard all unsaved edits?`)) return;
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

  function confirmDelete(message) {
    return window.confirm(message);
  }

  function renderAll() {
    syncProfileTabSelection();
    setPage(state.ui.page);
    renderRuntimeMenu();
    syncSurfaceVisibility();
  }
