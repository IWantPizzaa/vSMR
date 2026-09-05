"use strict";

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
      <button aria-label="Remove runway pair ${escapeHtml(runway.id)}" class="ui-button ui-button--compact ui-button--icon ui-button--destructive alert-runway-remove" data-action="remove-alert-runway" data-index="${index}" title="Remove" type="button">×</button>
    </div>`).join("");
    $("#alertRunwayTable").innerHTML = `<div class="alert-runway-header"><span>Runway pair</span><span>ARR</span><span>DEP</span><span>Closed</span><span></span></div>${runwayRowsHtml || `<div class="ui-list__empty">No monitored runway pairs.</div>`}`;

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

  function describeLegacyProfilesBackup(health) {
    const unixSeconds = Number(health?.profilesBackupModifiedUnixSeconds);
    if (!Number.isFinite(unixSeconds) || unixSeconds <= 0)
      return "Validated legacy profiles .bak (modification date unavailable)";

    const modified = new Date(unixSeconds * 1000);
    if (Number.isNaN(modified.getTime()))
      return "Validated legacy profiles .bak (modification date unavailable)";

    const ageMinutes = Math.floor(Math.max(0, Date.now() - modified.getTime()) / 60000);
    const ageValue = ageMinutes >= 1440
      ? Math.floor(ageMinutes / 1440)
      : ageMinutes >= 60
        ? Math.floor(ageMinutes / 60)
        : ageMinutes;
    const ageUnit = ageMinutes >= 1440 ? "day" : ageMinutes >= 60 ? "hour" : "minute";
    return `Validated legacy profiles .bak from ${modified.toLocaleString()} (${ageValue} ${ageUnit}${ageValue === 1 ? "" : "s"} old)`;
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
      protect_modified_aviso: updateCenter.config.protect_modified_aviso !== false,
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
      updateCenter.state.message = action === "reload_aviso"
        ? "AVISO reload queued for the next startup."
        : "Update retry queued for the next startup.";
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
    const versionSummary = $("#updateVersionSummary");
    if (!versionSummary) return;
    expireUpdateRequest("settings");
    expireUpdateRequest("action");

    const updater = updateCenter.state || {};
    const config = updateCenter.config || {};
    const status = String(updater.status || "idle").toLowerCase();
    const writable = !HOST_MODE || (updateCenter.available && updateCenter.configWritable);
    const busy = Boolean(updateCenter.pending.settings);

    const installed = String(updater.installed_version || "--");
    const available = String(updater.available_version || "").trim();
    versionSummary.textContent = available
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
    $("#updateProtectModifiedAviso").checked = config.protect_modified_aviso !== false;
    $("#updateChannel").disabled = !writable || busy;
    $("#updateAutoCheck").disabled = !writable || busy;
    $("#updateAutoDownload").disabled = !writable || busy || config.auto_check === false;
    $("#updateAutoInstall").disabled = !writable || busy || config.auto_download === false;
    $("#updateProtectModifiedAviso").disabled = !writable || busy;

    const pendingAction = Boolean(updateCenter.pending.action);
    const pendingActionName = String(updateCenter.pending.action?.action || "");
    const reloadAvisoButton = $("#updateReloadAvisoButton");
    reloadAvisoButton.disabled = !updateCenter.available || pendingAction || busy;
    reloadAvisoButton.textContent = pendingActionName === "reload_aviso" ? "Queuing..." : "Reload AVISOs";

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
    const aliasFile = String(settings.aliasFile || "").trim();
    $("#settingsAliasFile").value = aliasFile || "No alias file found";
    $("#settingsProfileFile").title = settings.profileFile;
    $("#settingsAvisoFile").title = settings.avisoFile;
    $("#settingsAliasFile").title = aliasFile || "No alias file found";
    ensureSelectValue($("#settingsResolutionPreset"), settings.resolutionPreset || "1080p");
    $("#settingsShowFps").checked = settings.showFps !== false;
    const uiColorTheme = settings.uiColorTheme === "day" ? "day" : "night";
    syncToggleButtons('[data-ui-color-theme]', uiColorTheme, "uiColorTheme");
    const avisoColorPalette = settings.avisoColorPalette === "day" ? "day" : "night";
    syncToggleButtons('[data-aviso-color-palette]', avisoColorPalette, "avisoColorPalette");
    const restoreBackup = $("#restoreProfilesBackupButton");
    if (restoreBackup) {
      restoreBackup.disabled = !settings.dataHealth?.profilesBackupAvailable || Boolean(pending.reload || pending.save || pending.resource);
      restoreBackup.title = restoreBackup.disabled
        ? "No validated profiles backup is available"
        : describeLegacyProfilesBackup(settings.dataHealth);
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
      showFps: $("#settingsShowFps").checked
    });
    state.profiles.forEach(record => {
      record.data.targets ||= {};
      record.data.targets.small_icon_boost_resolution_preset = state.settings.resolutionPreset;
    });
    clearUnappliedEditorSection($("#settingsResolutionPreset"));
    markDirty("Settings updated", ["profiles", "settings"]);
    if (render) renderIcons();
  }
