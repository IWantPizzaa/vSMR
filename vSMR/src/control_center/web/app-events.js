"use strict";

  function applyQueryState() {
    const params = new URLSearchParams(window.VSMR_PREVIEW_QUERY || location.search);
    const requestedPage = params.get("page");
    const page = ["performance", "diagnostics", "updates"].includes(requestedPage)
      ? "settings"
      : requestedPage;
    const tab = params.get("tab");
    if (PAGE_TITLES[page]) state.ui.page = page;
    if (PROFILE_TITLES[tab]) state.ui.profileTab = tab;
    const avisoView = params.get("aviso") || params.get("view");
    if (["geometry", "text"].includes(avisoView)) state.ui.avisoView = avisoView;
    if (params.has("palette")) state.settings.avisoColorPalette = normalizeAvisoColorPalette(params.get("palette"));
    if (["day", "night"].includes(params.get("theme"))) state.settings.uiColorTheme = params.get("theme");
    const ui = params.get("ui");
    if (ui === "control" || params.get("control") === "1" || PAGE_TITLES[page]) state.ui.controlCenterOpen = true;
    if (ui === "runtime") state.ui.controlCenterOpen = false;
    if (["mode", "groups", "inset", "profile"].includes(params.get("popup"))) state.ui.runtimePopover = params.get("popup");
    if (params.get("tag")) {
      state.ui.selectedTagId = params.get("tag");
      state.ui.selectedTagIds = [state.ui.selectedTagId];
      state.ui.tagSelectionAnchorId = state.ui.selectedTagId;
    }
    if (params.get("profile")) {
      const match = state.profiles.find(record => record.data.name.toLowerCase() === params.get("profile").toLowerCase());
      if (match) { state.activeProfileId = match.id; state.ui.managedProfileId = match.id; }
    }
  }

  function editorControlScope(control) {
    if (!(control instanceof HTMLInputElement || control instanceof HTMLSelectElement ||
        control instanceof HTMLTextAreaElement)) return "";
    if (control.matches(
      '[type="search"], [type="file"], ' +
      '#tagTokenSelect'
    ) || control.closest(".updater-general-group, #runtimeMenu, .page-rail, dialog")) return "";

    const profilePanel = control.closest("[data-profile-panel]")?.dataset.profilePanel;
    if (["colors", "icons", "tags", "rules"].includes(profilePanel)) return profilePanel;
    if (control.closest("[data-page-panel='modes']")) return "modes";
    if (control.closest("[data-page-panel='profiles']")) return "profiles";
    if (control.closest("[data-aviso-view-panel='geometry']")) return "aviso-geometry";
    if (control.closest("[data-aviso-view-panel='text']")) return "aviso-text";
    if (control.closest("[data-page-panel='groups']")) return "groups";
    if (control.closest("[data-page-panel='alerts']")) return "alerts";
    if (control.closest(".settings-layout")) return "settings";
    return "";
  }

  function stageEditorControl(control) {
    const scope = editorControlScope(control);
    if (!scope) return true;
    const sectionKey = editorSectionKey(control);
    if (sectionKey && !unappliedEditorSections.has(sectionKey)) return true;
    if (control.matches("#colorHex, #ruleTargetColor, #ruleTagColor, #ruleTextColor") &&
        !/^#?[0-9a-f]{6}$/i.test(control.value.trim())) return false;
    if (control.matches(".color-channel-value") && !/^\d{1,3}$/.test(control.value.trim())) return false;
    if (control.checkValidity && !control.checkValidity()) return false;
    const result = (() => {
      if (scope === "colors") return applyColorDraft({ render: false });
      if (scope === "icons") return applyIcons({ render: false });
      if (scope === "tags") return applyTag({
        render: false,
        applyContent: Boolean(control.closest("#tagDefinitionEditor"))
      });
      if (scope === "rules") return applyRule({ render: false });
      if (scope === "modes") return applyMode({ render: false });
      if (scope === "profiles") return applyProfile({ render: false });
      if (scope === "aviso-geometry") return applyAvisoGeometry({ render: false, feedback: false });
      if (scope === "aviso-text") return applyAvisoTextStyles({ render: false, feedback: false });
      if (scope === "groups") return applyAvisoGroup({ render: false, feedback: false });
      if (scope === "alerts") return applyAlerts({ render: false, feedback: false });
      if (scope === "settings") return applySettings({ render: false });
    })();
    if (result === false) return false;
    refreshEditorDerivedVisuals(scope, control);
    return true;
  }

  const deferredDerivedRefreshes = new WeakMap();

  function refreshSelectedColorRows() {
    selectedColorEntries().forEach(entry => {
      const row = $$("#colorTree [data-color-path]").find(item => item.dataset.colorPath === entry.id);
      if (!row) return;
      row.style.setProperty("--node-color", colorToHex(entry.color).toUpperCase());
      row.title = entry.name;
    });
  }

  function performDeferredDerivedRefresh(scope) {
    if (scope === "aviso-geometry") renderAvisoGeometry();
    else if (scope === "aviso-text") renderAvisoText();
    else if (scope === "alerts") renderAlerts();
  }

  function deferDerivedRefreshUntilFocusout(scope, control) {
    if (!control || document.activeElement !== control) {
      performDeferredDerivedRefresh(scope);
      return;
    }
    let scopes = deferredDerivedRefreshes.get(control);
    if (!scopes) {
      scopes = new Set();
      deferredDerivedRefreshes.set(control, scopes);
      control.addEventListener("focusout", () => {
        const queued = deferredDerivedRefreshes.get(control) || new Set();
        deferredDerivedRefreshes.delete(control);
        window.requestAnimationFrame(() => queued.forEach(performDeferredDerivedRefresh));
      }, { once: true });
    }
    scopes.add(scope);
  }

  function refreshEditorDerivedVisuals(scope, control = null) {
    if (scope === "colors") refreshSelectedColorRows();
    else if (scope === "rules") {
      const item = rules()[state.ui.selectedRuleIndex];
      if (item) {
        $("#ruleFormCaption").textContent = ruleLabel(item, state.ui.selectedRuleIndex);
        const row = $(`[data-rule-index="${state.ui.selectedRuleIndex}"] span`);
        if (row) row.textContent = ruleLabel(item, state.ui.selectedRuleIndex);
      }
    } else if (scope === "groups") {
      const group = selectedAvisoGroup();
      if (group) {
        $("#avisoGroupCaption").textContent = group.name;
        const row = $$("#avisoGroupList [data-aviso-group-id]").find(item => item.dataset.avisoGroupId === group.id);
        const label = row && $(".ui-list__label", row);
        if (label) label.textContent = group.name;
      }
    } else if (scope === "icons" && control?.id !== "targetSymbolScale") renderIconSymbolPreview();
    else if (scope === "settings") renderIconSymbolPreview();
    else if (["aviso-geometry", "aviso-text", "alerts"].includes(scope))
      deferDerivedRefreshUntilFocusout(scope, control);
  }

  function bindAvisoColorEditor(prefix, draftName, scope, includeOpacity, apply) {
    const element = suffix => $("#" + prefix + suffix);
    const draft = () => drafts[draftName];
    const syncRgbOutputs = () => {
      const current = hexToColor(draft()?.hex || "#000000");
      const values = ["Red", "Green", "Blue"].map((channel, index) => {
        const value = element(channel + "Output").value.trim();
        return value === "" ? [current.r, current.g, current.b][index] : Number(value);
      });
      if (values.every(Number.isFinite)) updateAvisoColorDraftFromRgb(draft(), prefix, ...values, includeOpacity);
    };

    element("Hex").addEventListener("input", event => {
      if (/^#?[0-9a-f]{6}$/i.test(event.target.value.trim()))
        updateAvisoColorDraftFromHex(draft(), prefix, event.target.value, includeOpacity);
    });
    element("Hex").addEventListener("change", event => {
      if (/^#?[0-9a-f]{6}$/i.test(event.target.value.trim()))
        updateAvisoColorDraftFromHex(draft(), prefix, event.target.value, includeOpacity);
    });
    element("Hue").addEventListener("input", event => updateAvisoColorDraftFromHsv(draft(), prefix, Number(event.target.value), draft()?.s ?? 0, draft()?.v ?? 1, includeOpacity));
    ["Red", "Green", "Blue"].forEach(suffix => {
      element(suffix).addEventListener("input", () => updateAvisoColorDraftFromRgb(draft(), prefix, element("Red").value, element("Green").value, element("Blue").value, includeOpacity));
    });
    element("HueOutput").addEventListener("input", event => {
      if (event.target.value.trim() === "") return;
      const value = Number(event.target.value);
      if (Number.isFinite(value)) updateAvisoColorDraftFromHsv(draft(), prefix, value, draft()?.s ?? 0, draft()?.v ?? 1, includeOpacity);
    });
    ["RedOutput", "GreenOutput", "BlueOutput"].forEach(suffix => {
      element(suffix).addEventListener("input", event => {
        if (event.target.value.trim() !== "") syncRgbOutputs();
      });
    });
    if (includeOpacity) {
      const updateOpacity = value => {
        if (!draft() || !Number.isFinite(Number(value))) return;
        draft().opacity = Math.round(clamp(Number(value), 0, 100));
        draft().opacityMixed = false;
        markControlTouched(element("Opacity"));
        markControlTouched(element("OpacityOutput"));
        syncAvisoColorEditor(prefix, draft(), true);
      };
      element("Opacity").addEventListener("input", event => updateOpacity(event.target.value));
      element("OpacityOutput").addEventListener("input", event => {
        if (event.target.value.trim() !== "") updateOpacity(event.target.value);
      });
    }

    const palette = element("SvPalette");
    const updateFromPointer = event => {
      if (!draft()) return;
      const rect = palette.getBoundingClientRect();
      const saturation = clamp((event.clientX - rect.left) / Math.max(1, rect.width), 0, 1);
      const value = 1 - clamp((event.clientY - rect.top) / Math.max(1, rect.height), 0, 1);
      updateAvisoColorDraftFromHsv(draft(), prefix, draft().h, saturation, value, includeOpacity);
    };
    palette.addEventListener("pointerdown", event => {
      palette.setPointerCapture(event.pointerId);
      palette.dataset.dragging = "true";
      updateFromPointer(event);
    });
    palette.addEventListener("pointermove", event => {
      if (palette.dataset.dragging === "true") updateFromPointer(event);
    });
    const stopDrag = event => {
      const changed = palette.dataset.dragging === "true";
      palette.dataset.dragging = "false";
      if (event.pointerId != null && palette.hasPointerCapture?.(event.pointerId)) palette.releasePointerCapture(event.pointerId);
      if (!changed) return;
      apply({ render: false, feedback: false });
      performDeferredDerivedRefresh(scope);
    };
    palette.addEventListener("pointerup", stopDrag);
    palette.addEventListener("pointercancel", stopDrag);
    palette.addEventListener("keydown", event => {
      if (!draft() || !["ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown"].includes(event.key)) return;
      event.preventDefault();
      const step = event.shiftKey ? .05 : .01;
      const nextS = draft().s + (event.key === "ArrowRight" ? step : event.key === "ArrowLeft" ? -step : 0);
      const nextV = draft().v + (event.key === "ArrowUp" ? step : event.key === "ArrowDown" ? -step : 0);
      updateAvisoColorDraftFromHsv(draft(), prefix, draft().h, nextS, nextV, includeOpacity);
      apply({ render: false, feedback: false });
      performDeferredDerivedRefresh(scope);
    });
  }

  const scrollCueObservers = [];

  function updateScrollCues(list) {
    const shell = list?.closest(".scroll-cue-shell");
    if (!shell) return;
    if (list.clientHeight <= 0) {
      shell.classList.remove("can-scroll-up", "can-scroll-down");
      return;
    }
    const tolerance = 2;
    shell.classList.toggle("can-scroll-up", list.scrollTop > tolerance);
    shell.classList.toggle(
      "can-scroll-down",
      list.scrollTop + list.clientHeight < list.scrollHeight - tolerance
    );
  }

  function initializeScrollCues() {
    SCROLL_CUE_LIST_IDS.forEach(id => {
      const list = $("#" + id);
      if (!list || list.closest(".scroll-cue-shell")) return;
      const shell = document.createElement("div");
      shell.className = "scroll-cue-shell";
      list.parentNode.insertBefore(shell, list);
      shell.appendChild(list);
      list.classList.add("scroll-cued-list");

      const top = document.createElement("div");
      top.className = "scroll-edge-cue scroll-edge-cue-top";
      top.setAttribute("aria-hidden", "true");
      top.innerHTML = "<span>▲</span>";
      const bottom = document.createElement("div");
      bottom.className = "scroll-edge-cue scroll-edge-cue-bottom";
      bottom.setAttribute("aria-hidden", "true");
      bottom.innerHTML = "<span>▼</span>";
      shell.append(top, bottom);

      let updateFrame = 0;
      const scheduleUpdate = () => {
        if (updateFrame) return;
        updateFrame = window.requestAnimationFrame(() => {
          updateFrame = 0;
          updateScrollCues(list);
        });
      };
      list.addEventListener("scroll", scheduleUpdate, { passive: true });
      const mutationObserver = new MutationObserver(scheduleUpdate);
      mutationObserver.observe(list, { childList: true, subtree: true });
      const resizeObserver = typeof ResizeObserver === "function" ? new ResizeObserver(scheduleUpdate) : null;
      resizeObserver?.observe(list);
      resizeObserver?.observe(shell);
      scrollCueObservers.push({ mutationObserver, resizeObserver });
      scheduleUpdate();
    });
  }

  function bindEvents() {
    document.addEventListener("input", event => {
      if (!editorControlScope(event.target)) return;
      markEditorSectionUnapplied(event.target);
      // Every valid editor input commits to the single live model immediately.
      // Temporary partial values (for example an unfinished hex code) stay in
      // the control without repainting and are committed as soon as valid.
      stageEditorControl(event.target);
    });
    document.addEventListener("change", event => {
      // Do not depend on a preceding `input` event. WebView/keyboard/control
      // combinations may emit only `change`; marking here guarantees ARR/DEP
      // checkboxes and every other editor control are staged before Save.
      if (editorControlScope(event.target)) markEditorSectionUnapplied(event.target);
      if (!stageEditorControl(event.target)) {
        revealInvalidEditorControl(event.target);
      }
    });

    document.addEventListener("click", event => {
      const guardedEditorAction = event.target.closest(
        "#controlWindow button[data-action], #controlWindow button[data-page], " +
        "#controlWindow button[data-profile-tab], #controlWindow [data-tree-toggle], " +
        "#controlWindow [data-color-path], #controlWindow [data-tag-id], " +
        "#controlWindow [data-rule-index], #controlWindow [data-mode-index], " +
        "#controlWindow [data-managed-profile-id], #controlWindow [data-aviso-group-id], " +
        "#controlWindow [data-aviso-geometry-style], #controlWindow [data-aviso-text-style]"
      );
      if (guardedEditorAction && hasUnappliedEditorInputs() && !stageFocusedEditorValue()) {
        event.preventDefault();
        return;
      }
      if (!event.target.closest("#ruleStatusDropdown")) setRuleStatusMenuOpen(false);
      if (!event.target.closest("#runtimeMenu") && state.ui.runtimePopover) {
        state.ui.runtimePopover = "";
        renderRuntimeMenu();
      }

      const runtimePopoverButton = event.target.closest("[data-runtime-popover]");
      if (runtimePopoverButton) { setRuntimePopover(runtimePopoverButton.dataset.runtimePopover); return; }
      const runtimeMode = event.target.closest("[data-runtime-mode]");
      if (runtimeMode) { setRuntimeMode(runtimeMode.dataset.runtimeMode); return; }
      const runtimeProfile = event.target.closest("[data-runtime-profile]");
      if (runtimeProfile) {
		const rollback = captureRuntimeCommandRollback();
		if (!switchActiveProfile(runtimeProfile.dataset.runtimeProfile)) return;
		postActiveProfileChange(rollback);
        state.ui.runtimePopover = "";
        renderRuntimeMenu();
        showToast(`Profile: ${activeProfile().name}`, "success");
        return;
      }
      const runtimeGroup = event.target.closest("[data-runtime-group]");
      if (runtimeGroup) { toggleRuntimeGroup(runtimeGroup.dataset.runtimeGroup); return; }
      const runtimeInset = event.target.closest("[data-runtime-inset]");
      if (runtimeInset) { toggleInsetWindow(runtimeInset.dataset.runtimeInset); return; }
      const insetPreset = event.target.closest("[data-inset-preset]");
      if (insetPreset) { loadAvisoPreset(insetPreset.dataset.insetPreset); return; }
      const groupContentType = event.target.closest("[data-aviso-group-content-type]");
      if (groupContentType) {
        state.ui.avisoGroupContentType = groupContentType.dataset.avisoGroupContentType;
        state.ui.avisoGroupContentSearch = "";
        renderAvisoGroupContentDialog();
        return;
      }
      const groupContentRow = event.target.closest("[data-group-content-key]");
      if (groupContentRow) { toggleAvisoGroupContentCandidate(groupContentRow.dataset.groupContentKey); return; }
      const groupVisibilityButton = event.target.closest('[data-action="toggle-aviso-group-visibility"]');
      if (groupVisibilityButton) { toggleRuntimeGroup(groupVisibilityButton.dataset.groupId); return; }
      const groupMemberRemove = event.target.closest('[data-action="remove-aviso-group-member"]');
      if (groupMemberRemove) { removeAvisoGroupMember(groupMemberRemove); return; }
      const avisoGroupRow = event.target.closest("[data-aviso-group-id]");
      if (avisoGroupRow) {
        if (!stageFocusedEditorValue()) return;
        state.ui.selectedAvisoGroupId = avisoGroupRow.dataset.avisoGroupId;
        drafts.avisoGroup = null;
        clearUnappliedEditorSection($("#avisoGroupName"));
        renderAvisoGroups();
        return;
      }

      const pageButton = event.target.closest("[data-page]");
      if (pageButton) { if (stageFocusedEditorValue()) setPage(pageButton.dataset.page); return; }
      const tabButton = event.target.closest("[data-profile-tab]");
      if (tabButton) { if (stageFocusedEditorValue()) setProfileTab(tabButton.dataset.profileTab); return; }
      const avisoViewButton = event.target.closest("[data-aviso-view]");
      if (avisoViewButton) { if (stageFocusedEditorValue()) { state.ui.avisoView = avisoViewButton.dataset.avisoView; renderAviso(); } return; }
      const treeToggle = event.target.closest("[data-tree-toggle]");
      if (treeToggle) {
        if (!stageFocusedEditorValue()) return;
        const store = treeState[treeToggle.dataset.treeToggle];
        const key = treeToggle.dataset.treeKey;
        if (store && key) {
          if (store.has(key)) store.delete(key); else store.add(key);
          if (treeToggle.dataset.treeToggle === "colors") renderColors();
          if (treeToggle.dataset.treeToggle === "tags") renderTags();
        }
        return;
      }
      const colorRow = event.target.closest("[data-color-path]");
      if (colorRow) { if (!stageFocusedEditorValue()) return; selectProfileColor(colorRow.dataset.colorPath, event); return; }
      const tagRow = event.target.closest("[data-tag-id]");
      if (tagRow) { if (!stageFocusedEditorValue()) return; selectTagDefinition(tagRow.dataset.tagId, event); return; }
      const ruleRow = event.target.closest("[data-rule-index]");
      if (ruleRow) { if (!stageFocusedEditorValue()) return; state.ui.selectedRuleIndex = Number(ruleRow.dataset.ruleIndex); drafts.rule = null; clearUnappliedEditorSection($("#ruleName")); renderRules(); return; }
      const modeRow = event.target.closest("[data-mode-index]");
      if (modeRow) { if (!stageFocusedEditorValue()) return; state.ui.selectedModeIndex = Number(modeRow.dataset.modeIndex); drafts.mode = null; clearUnappliedEditorSection($("#modeName")); renderModes(); return; }
      const profileRow = event.target.closest("[data-managed-profile-id]");
      if (profileRow) { if (!stageFocusedEditorValue()) return; state.ui.managedProfileId = profileRow.dataset.managedProfileId; drafts.profile = null; clearUnappliedEditorSection($("#profileName")); renderProfilesManager(); return; }
      const styleVisibilityToggle = event.target.closest("[data-aviso-style-visibility]");
      if (styleVisibilityToggle) {
        if (!stageFocusedEditorValue()) return;
        toggleAvisoStyleVisibility(
          styleVisibilityToggle.dataset.avisoStyleVisibility,
          styleVisibilityToggle.dataset.avisoStyleId
        );
        return;
      }
      const avisoGeometryStyleRow = event.target.closest("[data-aviso-geometry-style]");
      if (avisoGeometryStyleRow) {
        if (!stageFocusedEditorValue()) return;
        selectAvisoGeometryStyle(avisoGeometryStyleRow.dataset.avisoGeometryStyle, event);
        return;
      }
      const avisoTextStyleRow = event.target.closest("[data-aviso-text-style]");
      if (avisoTextStyleRow) {
        if (!stageFocusedEditorValue()) return;
        selectAvisoTextStyle(avisoTextStyleRow.dataset.avisoTextStyle, event);
        return;
      }
      const actionButton = event.target.closest("[data-action]");
      if (actionButton) handleAction(actionButton.dataset.action, actionButton);
    });

    $("#tagLabelFontSize").addEventListener("change", event => {
      event.target.value = String(Math.round(clamp(event.target.value, 1, 5)));
    });

    $("#colorHex").addEventListener("input", event => {
      if (/^#?[0-9a-f]{6}$/i.test(event.target.value.trim())) setColorDraftFromHex(event.target.value);
    });
    $("#colorHex").addEventListener("change", event => setColorDraftFromHex(event.target.value));
    $("#colorHue").addEventListener("input", event => setColorDraftFromHsv(Number(event.target.value), drafts.color?.s ?? 0, drafts.color?.v ?? 1));
    [["colorRed", "r"], ["colorGreen", "g"], ["colorBlue", "b"]].forEach(([id]) => {
      $("#" + id).addEventListener("input", () => setColorDraftFromRgb($("#colorRed").value, $("#colorGreen").value, $("#colorBlue").value));
    });
    $("#colorOpacity").addEventListener("input", event => {
      if (!drafts.color) return;
      drafts.color.opacity = Number(event.target.value);
      syncColorEditorControls();
    });
    $("#colorHueOutput").addEventListener("input", event => {
      if (event.target.value.trim() === "") return;
      const value = Number(event.target.value);
      if (Number.isFinite(value)) setColorDraftFromHsv(value, drafts.color?.s ?? 0, drafts.color?.v ?? 1);
    });
    [["colorRedOutput", "r"], ["colorGreenOutput", "g"], ["colorBlueOutput", "b"]].forEach(([id]) => {
      $("#" + id).addEventListener("input", event => {
        if (event.target.value.trim() === "") return;
        const values = [$("#colorRedOutput").value, $("#colorGreenOutput").value, $("#colorBlueOutput").value].map(Number);
        if (values.every(Number.isFinite)) setColorDraftFromRgb(...values);
      });
    });
    $("#colorOpacityOutput").addEventListener("input", event => {
      if (!drafts.color || event.target.value.trim() === "") return;
      const value = Number(event.target.value);
      if (!Number.isFinite(value)) return;
      markEditorSectionUnapplied(event.target);
      drafts.color.opacity = clamp(value, 0, 100);
      syncColorEditorControls();
    });
    const colorPalette = $("#colorSvPalette");
    colorPalette.addEventListener("pointerdown", event => {
      colorPalette.setPointerCapture(event.pointerId);
      colorPalette.dataset.dragging = "true";
      updateColorFromPalettePointer(event);
    });
    colorPalette.addEventListener("pointermove", event => {
      if (colorPalette.dataset.dragging === "true") updateColorFromPalettePointer(event);
    });
    const stopColorDrag = event => {
      const changed = colorPalette.dataset.dragging === "true";
      colorPalette.dataset.dragging = "false";
      if (event.pointerId != null && colorPalette.hasPointerCapture?.(event.pointerId)) colorPalette.releasePointerCapture(event.pointerId);
      if (changed) {
        applyColorDraft({ render: false });
        refreshEditorDerivedVisuals("colors");
      }
    };
    colorPalette.addEventListener("pointerup", stopColorDrag);
    colorPalette.addEventListener("pointercancel", stopColorDrag);
    colorPalette.addEventListener("keydown", event => {
      if (!drafts.color || !["ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown"].includes(event.key)) return;
      event.preventDefault();
      const step = event.shiftKey ? .05 : .01;
      const nextS = drafts.color.s + (event.key === "ArrowRight" ? step : event.key === "ArrowLeft" ? -step : 0);
      const nextV = drafts.color.v + (event.key === "ArrowUp" ? step : event.key === "ArrowDown" ? -step : 0);
      setColorDraftFromHsv(drafts.color.h, nextS, nextV);
      applyColorDraft({ render: false });
      refreshEditorDerivedVisuals("colors");
    });

    $("#targetSymbolScale").addEventListener("input", event => {
      const scale = clamp(event.target.value, 0.25, 5);
      $("#targetSymbolScaleOutput").value = `${scale.toFixed(2)}×`;
    });
    ["targetTrailGroundPoints", "targetTrailAirbornePoints"].forEach(id => $("#" + id).addEventListener("input", event => {
      $("#" + id + "Output").value = String(Math.round(Number(event.target.value)));
    }));
    $("#targetTrailEnabled").addEventListener("change", updateIconDependencies);
    ["targetIconStyle"].forEach(id => $("#" + id).addEventListener("change", renderIconSymbolPreview));

    $("#tagLineGrid").addEventListener("focusin", event => { if (event.target.matches(".tag-line-input")) activeTagInput = event.target; });
    $("#tagDetailedInherits").addEventListener("change", event => {
      captureTagDraft();
      drafts.tag.data.definition_detailed_inherits_normal = $("#tagDetailedInherits").checked;
      applyTag({ render: false });
      renderTagEditor();
    });

    $("#criteriaList").addEventListener("change", event => {
      const field = event.target.dataset.field;
      if (!field || !["source", "token", "condition"].includes(field)) return;
      const row = event.target.closest(".criterion-row");
      const source = $("[data-field='source']", row);
      const token = $("[data-field='token']", row);
      const condition = $("[data-field='condition']", row);
      if (field === "source") {
        const tokens = ruleTokensForSource(source.value);
        token.innerHTML = ruleSelectOptions(tokens, tokens[0]);
      }
      if (field === "source" || field === "token") {
        condition.innerHTML = ruleSelectOptions(ruleConditionsFor(source.value, token.value), "any", { not_in: "not in" });
        $("[data-field='condition-values']", row).value = "";
      }
      updateRuleConditionValueControl(row);
    });

    ["Target", "Tag", "Text"].forEach(kind => {
      $(`#ruleUse${kind}Color`).addEventListener("change", () => {
        const enabled = $(`#ruleUse${kind}Color`).checked;
        $(`#rule${kind}Color`).disabled = !enabled;
        $(`#rule${kind}Picker`).disabled = !enabled;
      });
      $(`#rule${kind}Color`).addEventListener("input", event => {
        if (!/^#?[0-9a-f]{6}$/i.test(event.target.value.trim())) return;
        const hex = normalizeHex(event.target.value, "#ffffff");
        $(`#rule${kind}Picker`).value = hex;
        $(`#rule${kind}Picker`).closest("label").style.setProperty("--swatch-color", hex);
      });
      $(`#rule${kind}Picker`).addEventListener("input", event => {
        $(`#rule${kind}Color`).value = event.target.value.toUpperCase();
        event.target.closest("label").style.setProperty("--swatch-color", event.target.value);
      });
    });

    $("#ruleStatusButton").addEventListener("click", event => {
      event.stopPropagation();
      setRuleStatusMenuOpen($("#ruleStatusMenu").hidden);
    });
    $("#ruleStatusAll").addEventListener("change", event => {
      $$("#ruleStatusOptions input[data-rule-status]").forEach(input => { input.checked = event.target.checked; });
      updateRuleStatusDropdownLabel();
    });
    $("#ruleStatusOptions").addEventListener("change", event => {
      if (!event.target.matches("input[data-rule-status]")) return;
      updateRuleStatusDropdownLabel();
    });
    $("#ruleStatusDropdown").addEventListener("keydown", event => {
      if (event.key === "Escape") { setRuleStatusMenuOpen(false); $("#ruleStatusButton").focus(); }
    });
    $("#avisoGroupMemberSearch").addEventListener("input", event => {
      captureAvisoGroupDraft();
      state.ui.avisoGroupMemberSearch = event.target.value;
      renderAvisoGroupEditor();
    });
    $("#avisoGroupName").addEventListener("input", event => {
      event.target.setCustomValidity(event.target.value.trim() ? "" : "Enter a group name");
      const draft = captureAvisoGroupDraft();
      if (draft) {
        draft.data.name = event.target.value;
        $("#avisoGroupCaption").textContent = event.target.value || "Group";
      }
    });
    $("#avisoGroupContentSearch").addEventListener("input", event => {
      state.ui.avisoGroupContentSearch = event.target.value;
      renderAvisoGroupContentDialog();
    });
    $("#avisoGroupContentDialog").addEventListener("close", () => {
      avisoGroupContentDraft = null;
      renderAvisoGroups();
      renderRuntimeMenu();
      updateCommandState();
    });
    const avisoGroupList = $("#avisoGroupList");
    avisoGroupList.addEventListener("dragstart", event => {
      const row = event.target.closest("[data-aviso-group-id]");
      if (!row) {
        event.preventDefault();
        return;
      }
      draggedAvisoGroupId = row.dataset.avisoGroupId;
      row.classList.add("is-dragging");
      event.dataTransfer.effectAllowed = "move";
      event.dataTransfer.setData("text/plain", draggedAvisoGroupId);
    });
    avisoGroupList.addEventListener("dragover", event => {
      const row = event.target.closest("[data-aviso-group-id]");
      if (!row || !draggedAvisoGroupId || row.dataset.avisoGroupId === draggedAvisoGroupId) return;
      event.preventDefault();
      event.dataTransfer.dropEffect = "move";
      $$(".aviso-group-row.is-drop-target", avisoGroupList).forEach(item => item.classList.remove("is-drop-target", "is-drop-after"));
      row.classList.add("is-drop-target");
      if (event.clientY > row.getBoundingClientRect().top + row.getBoundingClientRect().height / 2) row.classList.add("is-drop-after");
    });
    avisoGroupList.addEventListener("drop", event => {
      const target = event.target.closest("[data-aviso-group-id]");
      if (!target || !draggedAvisoGroupId || target.dataset.avisoGroupId === draggedAvisoGroupId) return;
      event.preventDefault();
      const groups = avisoGroups();
      const sourceIndex = groups.findIndex(group => group.id === draggedAvisoGroupId);
      const targetId = target.dataset.avisoGroupId;
      const after = target.classList.contains("is-drop-after");
      if (sourceIndex < 0) return;
      const [moved] = groups.splice(sourceIndex, 1);
      let insertionIndex = groups.findIndex(group => group.id === targetId);
      if (insertionIndex < 0) insertionIndex = groups.length;
      else if (after) insertionIndex += 1;
      groups.splice(insertionIndex, 0, moved);
      state.ui.selectedAvisoGroupId = moved.id;
      draggedAvisoGroupId = "";
      markDirty("AVISO groups reordered", ["aviso"]);
      renderAvisoGroups();
      renderRuntimeMenu();
    });
    avisoGroupList.addEventListener("dragend", () => {
      draggedAvisoGroupId = "";
      $$(".aviso-group-row", avisoGroupList).forEach(item => item.classList.remove("is-dragging", "is-drop-target", "is-drop-after"));
    });

    bindAvisoColorEditor("avisoGeometryColor", "avisoGeometry", "aviso-geometry", true, applyAvisoGeometry);
    bindAvisoColorEditor("avisoTextColor", "avisoTextStyle", "aviso-text", false, applyAvisoTextStyles);
    ["avisoTextFont", "avisoTextSize", "avisoTextHaloWidth"].forEach(id => {
      $("#" + id).addEventListener("input", event => markControlTouched(event.target));
      $("#" + id).addEventListener("change", event => markControlTouched(event.target));
    });
    const syncAvisoTextZoom = (sourceId) => {
      const number = $("#avisoTextZoomLevel");
      const slider = $("#avisoTextZoomSlider");
      const value = Math.round(clamp(sourceId === "avisoTextZoomSlider" ? slider.value : number.value, 0, 14));
      number.value = String(value);
      slider.value = String(value);
      $("#avisoTextZoomMeaning").textContent = MAP_ZOOM_LABELS[value] || `Zoom level ${value}`;
      markControlTouched(number);
      markControlTouched(slider);
    };
    $("#avisoTextZoomLevel").addEventListener("input", () => syncAvisoTextZoom("avisoTextZoomLevel"));
    $("#avisoTextZoomSlider").addEventListener("input", () => syncAvisoTextZoom("avisoTextZoomSlider"));
    document.addEventListener("change", event => {
      if (event.target.matches("#runtimePresetLinked")) toggleRuntimePresetLinked(event.target.checked);
      if (event.target.matches("[data-alert-type]")) event.target.closest(".alert-toggle-card")?.classList.toggle("inactive", !event.target.checked);
    });
    $("#resourceGithubLoadConfirm").addEventListener("click", loadResourceFromGithub);
    $("#resourceGithubUrl").addEventListener("keydown", event => {
      if (event.key === "Enter") { event.preventDefault(); loadResourceFromGithub(); }
    });

    document.addEventListener("keydown", event => {
      const list = event.target.closest?.(".ui-list");
      if (!list || !["ArrowDown", "ArrowUp", "Home", "End"].includes(event.key)) return;
      const listbox = event.target.closest?.('[role="listbox"]');
      const rows = $$(
        listbox ? '.ui-list__row[role="option"]:not(:disabled)' : ".ui-list__selection:not(:disabled)",
        listbox || list
      ).filter(row => !row.closest('[hidden]'));
      if (!rows.length) return;
      const focused = event.target.closest('.ui-list__row[role="option"], .ui-list__selection');
      const selected = rows.find(row => row.classList.contains("is-current")
        || row.closest(".ui-list__row")?.classList.contains("is-current"))
        || rows.find(row => row.getAttribute("aria-selected") === "true")
        || rows.find(row => row.getAttribute("aria-pressed") === "true");
      const currentIndex = Math.max(0, rows.indexOf(focused || selected || rows[0]));
      const nextIndex = event.key === "Home" ? 0
        : event.key === "End" ? rows.length - 1
          : event.key === "ArrowDown" ? Math.min(rows.length - 1, currentIndex + 1)
            : Math.max(0, currentIndex - 1);
      event.preventDefault();
      rows.forEach(row => { row.tabIndex = -1; });
      rows[nextIndex].tabIndex = 0;
      rows[nextIndex].focus();
    });

    document.addEventListener("keydown", event => {
      const tab = event.target.closest?.('[role="tab"]');
      if (!tab || !["ArrowLeft", "ArrowRight", "Home", "End"].includes(event.key)) return;
      const tablist = tab.closest('[role="tablist"]');
      if (!tablist) return;
      const tabs = $$('[role="tab"]:not(:disabled)', tablist);
      if (!tabs.length) return;
      const currentIndex = Math.max(0, tabs.indexOf(tab));
      const nextIndex = event.key === "Home" ? 0
        : event.key === "End" ? tabs.length - 1
          : event.key === "ArrowRight" ? (currentIndex + 1) % tabs.length
            : (currentIndex - 1 + tabs.length) % tabs.length;
      event.preventDefault();
      tabs[nextIndex].focus();
      tabs[nextIndex].click();
    });

    $("#avisoGeometryStyleList").addEventListener("keydown", event => {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "a") {
        event.preventDefault();
        const ids = avisoStyleEntries("geometry").map(entry => entry.id);
        if (ids.length) {
          state.ui.selectedAvisoGeometryStyleIds = ids;
          state.ui.selectedAvisoGeometryStyleId = ids[ids.length - 1];
          state.ui.avisoGeometrySelectionAnchorId = ids[0];
          clearUnappliedEditorSection($("#avisoGeometryColorHex"));
          renderAvisoGeometry();
        }
      } else if (event.key === "Escape") {
        const id = state.ui.selectedAvisoGeometryStyleId;
        state.ui.selectedAvisoGeometryStyleIds = id ? [id] : [];
        clearUnappliedEditorSection($("#avisoGeometryColorHex"));
        renderAvisoGeometry();
      }
    });
    $("#avisoTextStyleList").addEventListener("keydown", event => {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "a") {
        event.preventDefault();
        const ids = avisoStyleEntries("text").map(entry => entry.id);
        if (ids.length) {
          state.ui.selectedAvisoTextStyleIds = ids;
          state.ui.selectedAvisoTextStyleId = ids[ids.length - 1];
          state.ui.avisoTextSelectionAnchorId = ids[0];
          clearUnappliedEditorSection($("#avisoTextFont"));
          renderAvisoText();
        }
      } else if (event.key === "Escape") {
        const id = state.ui.selectedAvisoTextStyleId;
        state.ui.selectedAvisoTextStyleIds = id ? [id] : [];
        clearUnappliedEditorSection($("#avisoTextFont"));
        renderAvisoText();
      }
    });
    $("#tagDefinitionList").addEventListener("keydown", event => {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "a") {
        event.preventDefault();
        const ids = tagDefinitions().map(entry => entry.id);
        if (ids.length) {
          state.ui.selectedTagIds = ids;
          state.ui.selectedTagId = ids[ids.length - 1];
          state.ui.tagSelectionAnchorId = ids[0];
          drafts.tag = null;
          clearUnappliedEditorSection($("#tagDefinitionEditor"));
          renderTags();
        }
      } else if (event.key === "Escape") {
        const id = state.ui.selectedTagId;
        state.ui.selectedTagIds = id ? [id] : [];
        drafts.tag = null;
        clearUnappliedEditorSection($("#tagDefinitionEditor"));
        renderTags();
      }
    });
    $("#colorTree").addEventListener("keydown", event => {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "a") {
        event.preventDefault();
        const ids = collectProfileColors(activeProfile()).map(entry => entry.id);
        if (ids.length) {
          state.ui.selectedColorPaths = ids;
          state.ui.selectedColorPath = ids[ids.length - 1];
          state.ui.colorSelectionAnchorPath = ids[0];
          drafts.color = null;
          clearUnappliedEditorSection($("#colorHex"));
          renderColors();
        }
      } else if (event.key === "Escape") {
        const id = state.ui.selectedColorPath;
        state.ui.selectedColorPaths = id ? [id] : [];
        drafts.color = null;
        clearUnappliedEditorSection($("#colorHex"));
        renderColors();
      }
    });

    $("#updateChannel").addEventListener("change", event => {
      submitUpdateSettings({ channel: event.target.value === "stable" ? "stable" : "beta" }, "Update channel saved");
    });
    $("#updateAutoCheck").addEventListener("change", event => {
      submitUpdateSettings({ auto_check: event.target.checked }, "Automatic update checks saved");
    });
    $("#updateAutoDownload").addEventListener("change", event => {
      submitUpdateSettings({ auto_download: event.target.checked }, "Automatic download preference saved");
    });
    $("#updateAutoInstall").addEventListener("change", event => {
      submitUpdateSettings({ auto_install: event.target.checked }, "Automatic activation preference saved");
    });
    $("#updateProtectModifiedAviso").addEventListener("change", event => {
      submitUpdateSettings({ protect_modified_aviso: event.target.checked }, "AVISO edit protection saved");
    });
    $("#reloadButton").addEventListener("click", requestReload);
    $("#closeButton").addEventListener("click", closeControlCenter);
    $("#profilesFileInput").addEventListener("change", importProfilesFile);
    $("#avisoFileInput").addEventListener("change", importAvisoFile);
    bindWindowDrag();
    bindRuntimeMenuDrag();
  }

  function handleAction(action, button) {
    if (action === "open-control-center") openControlCenter();
    else if (action === "open-settings") { openControlCenter(); setPage("settings"); }
    else if (action === "set-ui-theme") {
      const theme = button.dataset.uiColorTheme === "day" ? "day" : "night";
      if (state.settings.uiColorTheme !== theme) {
        state.settings.uiColorTheme = theme;
        applyUiTheme();
        renderSettings();
        markDirty(`${theme === "day" ? "Day" : "Night"} UI theme selected`, ["settings"]);
      }
    }
    else if (action === "set-aviso-palette") {
      const palette = normalizeAvisoColorPalette(button.dataset.avisoColorPalette);
      if (state.settings.avisoColorPalette !== palette) {
        state.settings.avisoColorPalette = palette;
        renderSettings();
        renderAviso();
        markDirty(`AVISO ${palette} palette selected`, ["settings"]);
      }
    }
    else if (action === "dismiss-persistent-status") {
      if (persistentStatusState)
        dismissedPersistentStatusKey = `${persistentStatusState.type}|${persistentStatusState.message}`;
      renderPersistentStatus();
    }
    else if (action === "restore-bundled-defaults") restoreBundledDefaults();
    else if (action === "update-retry") requestUpdateAction("retry_update");
    else if (action === "update-reload-aviso") {
      if (updateCenter.config.protect_modified_aviso === false &&
        !window.confirm("AVISO edit protection is disabled. Reloading will replace locally modified bundled AVISOs with the installed GitHub release copies on the next startup. Continue?")) return;
      requestUpdateAction("reload_aviso");
    }
    else if (action === "update-release-open") openUpdateRelease();
    else if (action === "update-skip") {
      const version = String(updateCenter.state?.available_version || "");
      if (version) submitUpdateSettings({ skipped_version: version }, `${version} skipped`);
    }
    else if (action === "update-clear-skip") submitUpdateSettings({ skipped_version: "" }, "Skipped update cleared");
    else if (action === "assign-legacy-inset-presets") assignLegacyInsetPresetsToActiveAirport();
    else if (action === "close-runtime-popover") { state.ui.runtimePopover = ""; renderRuntimeMenu(); }
    else if (action === "save-inset-preset") openInsetPresetDialog("capture");
    else if (action === "rename-inset-preset") openInsetPresetDialog("rename");
    else if (action === "capture-inset-preset") confirmInsetPresetDialog();
    else if (action === "update-inset-preset") updateAvisoPreset();
    else if (action === "reset-inset-preset") resetAvisoPreset();
    else if (action === "duplicate-inset-preset") duplicateAvisoPreset();
    else if (action === "default-inset-preset") setDefaultAvisoPreset();
    else if (action === "delete-inset-preset") deleteAvisoPreset();
    else if (action === "select-aviso-text-color") {
      const target = button.dataset.colorTarget === "halo" ? "halo" : "text";
      if (target !== state.ui.avisoTextColorTarget) {
        state.ui.avisoTextColorTarget = target;
        drafts.avisoTextStyle = null;
        renderAvisoTextEditor();
      }
    }
    else if (action === "copy-profile-color") copyProfileColor();
    else if (action === "paste-profile-color") pasteProfileColor();
    else if (action === "copy-tag-definition") copyTagDefinition();
    else if (action === "paste-tag-definition") pasteTagDefinition();
    else if (action === "copy-rule") copyRule();
    else if (action === "paste-rule") pasteRule();
    else if (action === "copy-aviso-geometry") copyAvisoGeometry();
    else if (action === "paste-aviso-geometry") pasteAvisoGeometry();
    else if (action === "copy-aviso-text") copyAvisoText();
    else if (action === "paste-aviso-text") pasteAvisoText();
    else if (action === "insert-tag-token") insertTagToken();
    else if (action === "new-rule") createRule();
    else if (action === "duplicate-rule") duplicateRule();
    else if (action === "delete-rule") deleteRule();
    else if (action === "add-condition") {
      const draft = captureRuleDraft();
      if (!drafts.rule || !draft) return;
      drafts.rule.data.criteria.push({ source: "vacdm", token: "", condition: "" });
      renderRuleEditor();
      applyRule({ render: false });
    }
    else if (action === "delete-condition") deleteRuleCondition(Number(button.dataset.index));
    else if (action === "new-mode") createMode();
    else if (action === "duplicate-mode") duplicateMode();
    else if (action === "delete-mode") deleteMode();
    else if (action === "activate-mode") activateMode();
    else if (action === "mode-statuses-all") setModeStatusVisibility(true);
    else if (action === "mode-statuses-none") setModeStatusVisibility(false);
    else if (action === "new-profile") createProfile();
    else if (action === "duplicate-profile") duplicateProfile();
    else if (action === "delete-profile") deleteProfile();
    else if (action === "activate-profile") {
      const record = managedProfileRecord();
      if (record) {
		const rollback = captureRuntimeCommandRollback();
		if (!switchActiveProfile(record.id)) return;
		postActiveProfileChange(rollback);
      }
    }
    else if (action === "load-profiles-computer") {
	  if (!confirmResourceReplacement("profiles")) return;
      if (HOST_MODE) {
        postBridge("resource.computer.load", { resource: "profiles" });
        setStatus("Choose a profiles file…", "info");
      } else $("#profilesFileInput").click();
    }
    else if (action === "load-aviso-computer") {
	  if (!confirmResourceReplacement("aviso")) return;
      if (HOST_MODE) {
        postBridge("resource.computer.load", { resource: "aviso" });
        setStatus("Choose an AVISO GeoJSON file…", "info");
      } else $("#avisoFileInput").click();
    }
    else if (action === "load-profiles-github") {
	  openResourceGithubDialog("profiles");
	}
    else if (action === "load-aviso-github") {
	  openResourceGithubDialog("aviso");
	}
    else if (action === "new-aviso-group") createAvisoGroup();
    else if (action === "duplicate-aviso-group") duplicateAvisoGroup();
    else if (action === "delete-aviso-group") deleteAvisoGroup();
    else if (action === "clear-aviso-group") clearSelectedAvisoGroup();
    else if (action === "open-aviso-group-content") openAvisoGroupContentDialog();
    else if (action === "select-filtered-group-content") setFilteredAvisoGroupContent(true);
    else if (action === "clear-filtered-group-content") setFilteredAvisoGroupContent(false);
    else if (action === "toggle-aviso-group-visibility") toggleRuntimeGroup(button.dataset.groupId);
    else if (action === "remove-aviso-group-member") removeAvisoGroupMember(button);
    else if (action === "alert-runways-all-arr") setAllAlertRunwayField("arrival", true);
    else if (action === "alert-runways-all-dep") setAllAlertRunwayField("departure", true);
    else if (action === "alert-runways-open-all") setAllAlertRunwayField("closed", false);
    else if (action === "new-alert-runway") addAlertRunway();
    else if (action === "remove-alert-runway") removeAlertRunway(Number(button.dataset.index));
    else if (action.startsWith("browse-")) { postBridge(action.replaceAll("-", ".")); showToast("Native file picker requested"); }

  }
