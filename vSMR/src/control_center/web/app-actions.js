"use strict";

  function insertTagToken() {
    if (!activeTagInput || !document.body.contains(activeTagInput)) activeTagInput = $("#tagLineGrid .tag-line-input:not(:disabled)");
    if (!activeTagInput) return;
    const token = $("#tagTokenSelect").value;
    const start = activeTagInput.selectionStart ?? activeTagInput.value.length;
    const end = activeTagInput.selectionEnd ?? start;
    const prefix = start > 0 && !/\s$/.test(activeTagInput.value.slice(0, start)) ? " " : "";
    activeTagInput.value = activeTagInput.value.slice(0, start) + prefix + token + " " + activeTagInput.value.slice(end);
    applyTag({ render: false });
    activeTagInput.focus();
  }

  function createRule() {
    rules().push({ source: "vacdm", token: "tsat", condition: "valid", criteria: [{ source: "vacdm", token: "tsat", condition: "valid" }], tag_type: "departure", status: "any", statuses: RULE_STATUSES.slice(), detail: "normal", text_color: hexToColor("#ffffff") });
    state.ui.selectedRuleIndex = rules().length - 1;
    drafts.rule = null;
    clearUnappliedEditorSection($("#ruleName"));
    markDirty("Rule created", ["profiles"]);
    renderRules();
  }
  function duplicateRule() {
    const item = rules()[state.ui.selectedRuleIndex];
    if (!item) return;
    const copy = clone(item);
    copy.name = `${ruleLabel(item, state.ui.selectedRuleIndex)} copy`;
    rules().splice(state.ui.selectedRuleIndex + 1, 0, copy);
    state.ui.selectedRuleIndex += 1;
    drafts.rule = null;
    clearUnappliedEditorSection($("#ruleName"));
    markDirty("Rule copied", ["profiles"]);
    renderRules();
  }
  function deleteRule() {
    if (!rules().length || !confirmDelete("Delete this rule?")) return;
    rules().splice(state.ui.selectedRuleIndex, 1);
    state.ui.selectedRuleIndex = Math.max(0, state.ui.selectedRuleIndex - 1);
    drafts.rule = null;
    clearUnappliedEditorSection($("#ruleName"));
    markDirty("Rule deleted", ["profiles"]);
    renderRules();
  }
  function deleteRuleCondition(index) {
    captureRuleDraft();
    if (!drafts.rule) return;
    drafts.rule.data.criteria.splice(index, 1);
    if (!drafts.rule.data.criteria.length) drafts.rule.data.criteria.push({ source: "vacdm", token: "", condition: "" });
    renderRuleEditor();
    applyRule({ render: false });
  }

  function createMode() {
    modes().push({
      name: "New mode",
      require_assigned_squawk: false,
      accept_pilot_squawk: true,
      statuses: Object.fromEntries(MODE_STATUSES.map(status => [status, true])),
      require_clearance: false,
      require_valid_tsat: false,
      require_active_tobt: false,
      tower_filter: false,
      structured_rules: true,
      max_airborne_altitude_ft: 5500,
      max_airborne_speed_kt: 250
    });
    state.ui.selectedModeIndex = modes().length - 1;
    drafts.mode = null;
    clearUnappliedEditorSection($("#modeName"));
    markDirty("Mode created", ["profiles"]);
    renderModes();
    renderRuntimeMenu();
  }
  function duplicateMode() {
    const item = modes()[state.ui.selectedModeIndex];
    if (!item) return;
    const copy = clone(item); copy.name = `${item.name} copy`;
    modes().splice(state.ui.selectedModeIndex + 1, 0, copy);
    state.ui.selectedModeIndex += 1;
    drafts.mode = null;
    clearUnappliedEditorSection($("#modeName"));
    markDirty("Mode copied", ["profiles"]); renderModes(); renderRuntimeMenu();
  }
  function deleteMode() {
    const items = modes();
    if (items.length <= 1 || !confirmDelete("Delete this display mode?")) return;
    const deleted = items[state.ui.selectedModeIndex];
    items.splice(state.ui.selectedModeIndex, 1);
    state.ui.selectedModeIndex = Math.max(0, state.ui.selectedModeIndex - 1);
    if (activeProfile().filters.display_modes.active === deleted.name) activeProfile().filters.display_modes.active = items[0].name;
    drafts.mode = null;
    clearUnappliedEditorSection($("#modeName"));
    markDirty("Mode deleted", ["profiles"]); renderModes(); renderRuntimeMenu();
  }
  function activateMode() {
    const mode = modes()[state.ui.selectedModeIndex];
    if (!mode) return;
	if (mode.name === activeProfile()?.filters?.display_modes?.active) return;
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
    activeProfile().filters.display_modes.active = mode.name;
    renderModes();
    renderRuntimeMenu();
	postRuntimeCommand(
	  "runtime.mode.change",
	  { profile: activeProfile().name, mode: mode.name },
	  rollback
	);
  }

  function createProfile() {
    const base = clone(activeProfile());
    base.name = "New profile";
    const record = { id: uid("profile"), persistedName: "", data: base, original: clone(base) };
    state.profiles.push(record);
    state.ui.managedProfileId = record.id;
    drafts.profile = null;
    clearUnappliedEditorSection($("#profileName"));
    markDirty("Profile created", ["profiles"]);
    renderProfilesManager(); renderRuntimeMenu();
  }
  function duplicateProfile() {
    const source = managedProfileRecord(); if (!source) return;
    const data = clone(source.data); data.name = `${data.name} copy`;
    const record = { id: uid("profile"), persistedName: "", data, original: clone(data) };
    const index = state.profiles.indexOf(source) + 1;
    state.profiles.splice(index, 0, record);
    state.ui.managedProfileId = record.id;
    drafts.profile = null;
    clearUnappliedEditorSection($("#profileName"));
    markDirty("Profile copied", ["profiles"]); renderProfilesManager(); renderRuntimeMenu();
  }
  function deleteProfile() {
    if (state.profiles.length <= 1 || !confirmDelete("Delete this profile?")) return;
    const record = managedProfileRecord(); if (!record) return;
    const index = state.profiles.indexOf(record); state.profiles.splice(index, 1);
    if (record.id === state.activeProfileId) state.activeProfileId = state.profiles[Math.max(0, index - 1)].id;
    state.ui.managedProfileId = state.activeProfileId;
    drafts.profile = null;
    clearUnappliedEditorSection($("#profileName"));
    markDirty("Profile deleted", ["profiles", "metadata"]); renderAllProfileSections(); renderRuntimeMenu();
  }


  function normalizeGithubRawUrl(value) {
    const raw = String(value || "").trim();
    if (!raw) throw new Error("Enter a GitHub file URL");
    const url = new URL(raw);
    if (url.protocol !== "https:" || url.username || url.password || url.port)
      throw new Error("Use an HTTPS GitHub URL without credentials or a custom port");
    if (url.hostname === "raw.githubusercontent.com") return url.href;
    if (url.hostname !== "github.com" && url.hostname !== "www.github.com") throw new Error("Use a github.com or raw.githubusercontent.com URL");
    const parts = url.pathname.split("/").filter(Boolean);
    if (parts.length < 5) throw new Error("The GitHub URL must point to a file");
    const [owner, repo, mode, branch, ...fileParts] = parts;
    if (mode === "blob" || mode === "raw") {
      if (!fileParts.length) throw new Error("The GitHub URL must point to a JSON or GeoJSON file");
      return `https://raw.githubusercontent.com/${owner}/${repo}/${branch}/${fileParts.join("/")}`;
    }
    throw new Error("Open the file on GitHub and copy its file URL");
  }

  function resourceSourceKey(type) {
    return type === "profiles" ? "profileFile" : "avisoFile";
  }

  function setResourceSource(type, source) {
    state.settings[resourceSourceKey(type)] = source;
    if (state.ui.page === "settings") renderSettings();
  }

  function applyProfilesPayload(parsed, source = "") {
    if (!Array.isArray(parsed)) throw new Error("Expected a vSMR profiles JSON array");
    const { records, metadata, extras } = getProfileRecords(parsed);
    if (!records.length) throw new Error("No profiles were found in this file");
    const preferred = records.find(record => record.data.name === metadata.last_active_profile) || records[0];
    migrateProfileAvisoPresetStores(metadata, records, preferred.id);
    state.profiles = records;
    state.metadata = metadata;
    state.profileExtras = clone(extras);
    state.activeProfileId = preferred.id;
    state.ui.managedProfileId = preferred.id;
    state.ui.selectedRuleIndex = 0;
    state.ui.selectedModeIndex = Math.max(0, (preferred.data.filters?.display_modes?.items || []).findIndex(mode => mode.name === preferred.data.filters?.display_modes?.active));
    state.ui.selectedTagId = "departure:taxi";
    state.ui.selectedTagIds = [state.ui.selectedTagId];
    state.ui.tagSelectionAnchorId = state.ui.selectedTagId;
    const colors = collectProfileColors(preferred.data);
    state.ui.selectedColorPath = colors[0]?.id || "";
    state.settings.resolutionPreset = preferred.data.targets?.small_icon_boost_resolution_preset || state.settings.resolutionPreset || "1080p";
    Object.keys(drafts).forEach(key => drafts[key] = null);
    clearAllUnappliedEditorSections();
    if (source) setResourceSource("profiles", source);
    renderAllProfileSections();
    renderSettings();
    renderRuntimeMenu();
    markDirty(`${source || "Profiles"} loaded`, ["profiles", "metadata", "profileExtras", "settings"]);
  }


  function applyAvisoPayload(parsed, source = "") {
    if (parsed?.type !== "FeatureCollection" || !Array.isArray(parsed.features)) throw new Error("Expected a GeoJSON FeatureCollection");
    state.aviso = normalizeAvisoData(parsed);
    clearAllUnappliedEditorSections();
    resetAvisoSelections();
    if (source) setResourceSource("aviso", source);
    renderAviso();
    renderRuntimeMenu();
    markDirty(`${source || "AVISO GeoJSON"} loaded`, ["aviso"]);
  }

  function openResourceGithubDialog(type) {
    githubResourceType = ["profiles", "aviso"].includes(type) ? type : "aviso";
    const labels = { profiles: "Profiles", aviso: "AVISO GeoJSON" };
    const examples = {
      profiles: "https://github.com/owner/repo/blob/branch/vSMR_Profiles.json",
      aviso: "https://github.com/owner/repo/blob/branch/AVISO.geojson"
    };
    const dialog = $("#resourceGithubDialog");
    $("#resourceGithubTitle").textContent = `Load ${labels[githubResourceType]} from GitHub`;
    $("#resourceGithubUrl").value = "";
    $("#resourceGithubUrl").placeholder = examples[githubResourceType];
    $("#resourceGithubHint").textContent = "Paste a GitHub file URL or a raw.githubusercontent.com URL.";
    if (typeof dialog.showModal === "function") dialog.showModal();
    else dialog.setAttribute("open", "");
    setTimeout(() => $("#resourceGithubUrl")?.focus(), 0);
  }

  function setGithubRequestPending(isPending) {
    const button = $("#resourceGithubLoadConfirm");
    const input = $("#resourceGithubUrl");
    if (button) {
      button.disabled = isPending;
      button.textContent = isPending ? "Loading..." : "Load";
    }
    if (input) input.disabled = isPending;
  }

  function loadResourceFromGithub() {
    const input = $("#resourceGithubUrl");
    try {
      const sourceUrl = String(input.value || "").trim();
      const url = normalizeGithubRawUrl(sourceUrl);
      if (pending.resource) return;
      const resource = githubResourceType;
	  if (!confirmResourceReplacement(resource)) return;
      const id = postBridge("resource.github.load", { resource, url });
      if (!id) return;
      pending.resource = { id, resource, source: sourceUrl, kind: "github" };
      armPendingTimeout("resource", id);
      setGithubRequestPending(true);
      setStatus(`Loading ${resource === "aviso" ? "AVISO GeoJSON" : "profiles"} from GitHub...`, "info");
      if (!HOST_MODE) {
        setTimeout(() => receiveHostMessage({
          version: PROTOCOL_VERSION,
          id,
          type: "resource.error",
          payload: { requestId: id, resource, message: "GitHub loading is available in the native Control Center." }
        }), 0);
      }
    } catch (error) {
      showToast(error.message || "Could not load the GitHub file", "error");
    }
  }

  async function importProfilesFile(event) {
    const file = event.target.files?.[0];
    if (!file) return;
    try {
      applyProfilesPayload(JSON.parse(await file.text()), file.name);
      showToast("Profiles loaded", "success");
    } catch (error) { showToast(error.message, "error"); }
    event.target.value = "";
  }

  async function importAvisoFile(event) {
    const file = event.target.files?.[0];
    if (!file) return;
    try {
      applyAvisoPayload(JSON.parse(await file.text()), file.name);
      showToast("GeoJSON loaded", "success");
    } catch (error) { showToast(error.message, "error"); }
    event.target.value = "";
  }
