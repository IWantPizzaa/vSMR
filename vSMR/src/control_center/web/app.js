"use strict";

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
	  hasUnappliedEditorInputs() && !trustedRuntimeResponse;
    // A hidden, preloaded Control Center can retain a focused input even though
    // nobody is editing it. It must still consume saves made by another radar
    // screen so its optimistic-concurrency token does not become stale. Actual
    // edits are already protected below by dirty/unapplied editor state.
    const preservesFocusedEditor = Boolean(editorControlScope(document.activeElement)) &&
      ["update", "preset"].includes(reason);
    const externallyChangedDirtyEditors =
	  (state.dirty || hasUnappliedEditorWork) &&
	  ["resource-source", "external-save", "backup-restored", "profile", "mode"].includes(reason);
    const incomingAirport = normalizeAirportCode(
      typeof incoming.airport === "string" ? incoming.airport : state.hostAirport
    );
	const preservesStagedEditors = preservesFocusedEditor || ((state.dirty || hasUnappliedEditorWork) &&
      !["initial", "reload"].includes(reason));
    const hostAirportChanged = !preservesStagedEditors && typeof incoming.airport === "string" &&
      incomingAirport !== previousHostAirport;
    const profileModeReplacement = !preservesStagedEditors &&
      ["profile", "mode"].includes(reason) && Array.isArray(incoming.profiles);
    const resetsSavedBaseline = !preservesStagedEditors && (
      ["external-save", "backup-restored"].includes(reason) ||
      profileModeReplacement || hostAirportChanged
    );
    let avisoChanged = false;

    // A background sync must not bless an older staged editor snapshot with a
    // newer disk revision.  Keeping its old token makes the next save fail
    // closed and directs the user to reload instead of overwriting another
    // Control Center's changes.
    if ((!preservesStagedEditors || reason === "preset") && typeof incoming.configRevision === "string")
      state.configRevision = incoming.configRevision;
    if (!preservesStagedEditors && typeof incoming.avisoRevision === "string")
      state.avisoRevision = incoming.avisoRevision;
    if (["initial", "reload", "backup-restored", "resource-source"].includes(reason))
      state.recoveryConfirmed = false;
    if (["initial", "reload", "backup-restored", "resource-source"].includes(reason))
      state.avisoRecoveryConfirmed = false;
    if (!preservesStagedEditors)
      state.externalEditConflict = false;
    else if (externallyChangedDirtyEditors)
      state.externalEditConflict = true;
    if (["initial", "reload", "backup-restored"].includes(reason) &&
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
        ["profileFile", "avisoFile", "aliasFile"].forEach(key => {
          if (typeof incoming.settings[key] === "string") state.settings[key] = incoming.settings[key];
        });
      }
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
	}
    if (avisoChanged) resetAvisoSelections();
	// A benign runtime/preset sync must not repaint the focused editor while
	// local staged changes are waiting for automatic persistence.
	if (preservesStagedEditors) {
	  renderRuntimeMenu();
	  updateCommandState();
	} else {
	  renderAll();
	}
    renderDataHealthStatus();
    if (preservesStagedEditors && state.airport && state.hostAirport &&
      state.airport !== state.hostAirport && previousHostAirport !== state.hostAirport) {
      const message = "The active airport changed. Reload before saving.";
      setStatus(message, "error");
      showToast(message, "error");
    }

    if (reason === "initial" || reason === "reload" || (resourceSourceChanged && !preservesStagedEditors) || resetsSavedBaseline) {
      resetSavedSnapshot();
    } else {
      const wasDirty = state.dirty;
      const currentSnapshot = captureEditorSnapshot();
      if (!wasDirty) savedSnapshot = currentSnapshot;
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
	  // A loaded source remains dirty until automatic persistence commits it
	  if (effectivePath) updateDirtyState();
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
		const completesReload = Boolean(context?.completesReload);
		const runtimeRequestId = String(context?.runtimeRequestId || "");
		const authoritativeState = context
		  ? { ...context.payload, aviso }
		  : { aviso };
		applyAuthoritativeState(authoritativeState, reason, trustedRuntimeResponse);
		if (completesReload) {
		  pending.reload = "";
		  showToast("Configuration reloaded", "success");
		}
		if (runtimeRequestId) finishRuntimeCommand(runtimeRequestId, false);
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
    if (message.type === "state.ack") {
      if (finishUpdateAck(message)) return;
      return;
    }
    if (message.type === "state.saved" || message.type === "saved") {
      const savedState = authoritativePayload(payload);
      finishConfigurationSave(message, savedState);
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
	  finishRuntimeCommand(responseId, true);
	  clearSplitAvisoContext(responseId);
      if (pending.save && messageMatchesRequest(message, pending.save)) {
        pending.save = "";
        saveInFlight = null;
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
  resetSavedSnapshot();
  setHostAuthoritativeReady(!HOST_MODE);
  window.setInterval(() => requestUpdateState(), 1500);
  document.addEventListener("visibilitychange", () => {
    if (!document.hidden && updateViewActive()) requestUpdateState(true);
  });
  setStatus(HOST_MODE ? "Waiting for configuration…" : "Bundled LFPG preview loaded");
  postBridge("ui.ready", {
    hostMode: HOST_MODE,
    protocolVersion: PROTOCOL_VERSION,
    capabilities: ["state", "save", "reload", "github-import", "computer-import", "window-actions", "split-aviso", "startup-updates"]
  });
