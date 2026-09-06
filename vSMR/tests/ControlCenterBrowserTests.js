"use strict";

(async function runControlCenterBrowserTests() {
  const result = document.createElement("pre");
  result.id = "vsmr-browser-test-result";
  result.dataset.status = "running";
  document.body.append(result);

  const failures = [];
  const expect = (condition, message) => {
    if (!condition) failures.push(message);
  };
  const waitFor = async (predicate, message, timeoutMs = 2500) => {
    const deadline = performance.now() + timeoutMs;
    while (performance.now() < deadline) {
      if (predicate()) return true;
      await new Promise(resolve => setTimeout(resolve, 20));
    }
    failures.push(message);
    return false;
  };
  const pressKey = (element, key) => element?.dispatchEvent(new KeyboardEvent("keydown", {
    key,
    bubbles: true,
    cancelable: true
  }));
  const sharedListSamples = [];
  const formControlSamples = [];
  const panelSamples = [];
  const sampledFormControls = new Set();
  const sampledPanels = new Set();
  const isVisible = element => {
    const rect = element.getBoundingClientRect();
    return rect.width > 0 && rect.height > 0;
  };
  const sampleVisiblePrimitives = () => {
    document.querySelectorAll(
      'input:is([type="text"], [type="search"], [type="number"]):not(:disabled), select:not(:disabled)'
    ).forEach(control => {
      if (sampledFormControls.has(control) || !isVisible(control)) return;
      sampledFormControls.add(control);
      const baseStyle = getComputedStyle(control);
      const baseBorderColor = baseStyle.borderTopColor;
      control.focus({ preventScroll: true });
      const focusStyle = getComputedStyle(control);
      formControlSamples.push({
        id: control.id,
        kind: control.matches("select") ? "select" : control.type,
        height: control.getBoundingClientRect().height,
        radius: Number.parseFloat(focusStyle.borderTopLeftRadius),
        baseBorderColor,
        focusBorderColor: focusStyle.borderTopColor,
        focusBoxShadow: focusStyle.boxShadow
      });
    });

    document.querySelectorAll(".panel").forEach(panel => {
      if (sampledPanels.has(panel) || !isVisible(panel)) return;
      sampledPanels.add(panel);
      const style = getComputedStyle(panel);
      const title = panel.querySelector(":scope > .panel-title");
      panelSamples.push({
        signature: [
          style.borderTopWidth,
          style.borderTopStyle,
          style.borderTopColor,
          style.borderTopLeftRadius,
          style.backgroundColor
        ].join("|"),
        titleHeight: title && isVisible(title) ? title.getBoundingClientRect().height : null
      });
    });
  };
  const sampleSharedList = (selector, label) => {
    const list = document.querySelector(selector);
    expect(Boolean(list), `${label} list is rendered`);
    expect(list?.classList.contains("ui-list"),
      `${label} uses the shared list component`);
    const rows = Array.from(list?.querySelectorAll(".ui-list__row") || []);
    expect(rows.length > 0, `${label} renders shared list rows`);
    expect(rows.every(row =>
      (row.getAttribute("role") === "option" && (row.matches("button") || row.tabIndex >= 0)) ||
      (row.getAttribute("role") === "listitem" &&
        Boolean(row.querySelector(":scope > .ui-list__selection")))),
      `${label} rows use a shared selectable-row structure`);
    const visibleRow = rows.find(row => row.getBoundingClientRect().height > 0);
    expect(Boolean(visibleRow), `${label} has a measurable visible row`);
    if (!list || !visibleRow) return rows;

    const listStyle = getComputedStyle(list);
    const rowStyle = getComputedStyle(visibleRow);
    expect(listStyle.overflowY === "auto" || listStyle.overflowY === "scroll",
      `${label} uses the shared scrolling behavior`);
    sharedListSamples.push({
      label,
      listWidth: list.getBoundingClientRect().width,
      rowHeight: visibleRow.getBoundingClientRect().height,
      rowSignature: [
        rowStyle.paddingTop,
        rowStyle.paddingRight,
        rowStyle.paddingBottom,
        rowStyle.paddingLeft,
        rowStyle.fontFamily,
        rowStyle.fontSize,
        rowStyle.lineHeight,
        rowStyle.borderRadius
      ].join("|")
    });
    return rows;
  };

  try {
    const api = window.vsmrControlCenter;
    expect(Boolean(api), "supported Control Center API is published");
    expect(typeof api?.receive === "function", "host receive API is available");
    expect(typeof api?.getState === "function", "state snapshot API is available");

    const initial = api.getState();
    expect(Array.isArray(initial.profiles) && initial.profiles.length > 0,
      "bundled profiles initialize");
    expect(initial.aviso?.type === "FeatureCollection", "bundled AVISO initializes");

    const authoritative = structuredClone(initial);
    authoritative.airport = "TEST";
    authoritative.activeProfile = authoritative.profiles[0]?.name || "Default";
    authoritative.recoveryConfirmed = true;
    authoritative.settings ||= {};
    authoritative.settings.dataHealth = {
      ...(authoritative.settings.dataHealth || {}),
      profilesHealthy: true,
      avisoHealthy: true
    };
    const hostileProfileName = 'Profile <img id="vsmr-profile-injection" src=x>';
    const hostileGroupName = 'Group <img id="vsmr-group-injection" src=x>';
    if (authoritative.profiles[0]) {
      authoritative.profiles[0].name = hostileProfileName;
      authoritative.profiles[0].rules = { version: 1, items: [] };
    }
    authoritative.activeProfile = hostileProfileName;
    authoritative.aviso.vsmr_groups = [{
      id: "browser-hostile-group",
      name: hostileGroupName,
      visible: true,
      accent: "#84b7d5"
    }];
    api.receive({
      version: 1,
      id: "browser-state-1",
      type: "state.authoritative",
      payload: authoritative
    });
    expect(api.getState().airport === "TEST", "native authoritative state is applied");
    const contentSecurityPolicy = document.querySelector(
      'meta[http-equiv="Content-Security-Policy"]')?.content || "";
    expect(contentSecurityPolicy.includes("default-src 'none'") &&
      contentSecurityPolicy.includes("script-src 'self'") &&
      contentSecurityPolicy.includes("style-src 'self' 'unsafe-inline'") &&
      contentSecurityPolicy.includes("img-src 'self' data: https://icons.vsmr") &&
      contentSecurityPolicy.includes("connect-src 'none'"),
      "Control Center policy permits only its self-hosted UI, inline style variables and mapped icon host");

    const displayButton = document.querySelector('.rail-button[data-page="display"]');
    displayButton?.click();
    expect(displayButton?.classList.contains("active"), "page navigation event is bound");
    expect(document.querySelector("#closeButton")?.textContent.trim() === "X",
      "Control Center and native inset close buttons use the same X glyph");
    displayButton?.blur();
    displayButton?.dispatchEvent(new FocusEvent("focusin", { bubbles: true }));
    await waitFor(() => {
      const tooltip = document.querySelector("#interactionTooltip.visible");
      return tooltip && /display page/i.test(tooltip.textContent);
    }, "interactive controls expose a delayed explanatory tooltip", 1200);
    expect(Boolean(displayButton?.querySelector("svg rect")),
      "Display navigation uses a recognizable monitor icon");

    const outbound = [];
    window.addEventListener("vsmr-control-center", event => outbound.push(event.detail));

    const rulesTab = document.querySelector('[data-profile-tab="rules"]');
    rulesTab?.click();
    const emptyRuleEditor = document.querySelector("#ruleEditorEmpty");
    const ruleEditorForm = document.querySelector("#ruleEditorForm");
    const ruleNameWithoutSelection = document.querySelector("#ruleName");
    expect(Boolean(emptyRuleEditor) && !emptyRuleEditor.hidden && Boolean(ruleEditorForm?.hidden),
      "an empty rules list shows a dedicated non-editable state");
    expect(Array.from(ruleEditorForm?.querySelectorAll("input, select, button") || [])
      .every(control => control.disabled),
      "rule controls stay disabled until a rule exists");
    if (ruleNameWithoutSelection) {
      ruleNameWithoutSelection.value = "must not stage";
      ruleNameWithoutSelection.dispatchEvent(new Event("input", { bubbles: true }));
    }
    const colorsAfterEmptyEdit = document.querySelector('[data-profile-tab="colors"]');
    colorsAfterEmptyEdit?.click();
    expect(colorsAfterEmptyEdit?.getAttribute("aria-selected") === "true",
      "editing cannot trap navigation when no rule exists");
    rulesTab?.click();
    document.querySelector('#ruleEditorEmpty [data-action="new-rule"]')?.click();
    const createdRuleName = document.querySelector("#ruleName");
    expect(Boolean(createdRuleName) && !createdRuleName.disabled && !ruleEditorForm?.hidden &&
      document.querySelectorAll("#criteriaList .criterion-row").length === 1,
      "creating a rule enables a complete editor with one condition");
    expect(document.querySelectorAll(".rule-editor-summary-grid > fieldset").length === 2 &&
      document.querySelectorAll(".rule-color-grid > .rule-color-row").length === 3,
      "Rules use dedicated identity, scope, condition, and color-override sections");
    const firstCriterion = document.querySelector("#criteriaList .criterion-row");
    const vSidSource = firstCriterion?.querySelector('[data-field="source"]');
    if (vSidSource) {
      vSidSource.value = "cdm";
      vSidSource.dispatchEvent(new Event("change", { bubbles: true }));
    }
    expect(Array.from(firstCriterion?.querySelectorAll('[data-field="token"] option') || [])
      .map(option => option.value).join(",") === "tobt,tsat,ttot,ctot,tsac,asrt,asat",
      "Rules expose the selected CDM bridge fields through one dedicated source");
    if (vSidSource) {
      vSidSource.value = "vsid";
      vSidSource.dispatchEvent(new Event("change", { bubbles: true }));
    }
    expect(Array.from(firstCriterion?.querySelectorAll('[data-field="token"] option') || [])
      .map(option => option.value).join(",") === "vsid_sid,vsid_rwy,vsid_cfl",
      "Rules expose the vSID bridge fields through one dedicated source");
    expect(Boolean(document.querySelector('[data-action="copy-rule"]')) &&
      Boolean(document.querySelector('[data-action="paste-rule"]')),
      "Rules expose shared copy and paste actions");
    if (createdRuleName) {
      createdRuleName.value = "Browser rule";
      createdRuleName.dispatchEvent(new Event("input", { bubbles: true }));
    }
    await waitFor(
      () => outbound.some(message => message.type === "state.save" &&
        JSON.stringify(message.payload?.profiles || []).includes("Browser rule")),
      "a newly created rule can be edited and saved"
    );
    document.querySelector('[data-action="copy-rule"]')?.click();
    await new Promise(resolve => setTimeout(resolve, 30));
    const changedRuleName = document.querySelector("#ruleName");
    if (changedRuleName) {
	  changedRuleName.value = "Temporary rule name";
	  changedRuleName.dispatchEvent(new Event("input", { bubbles: true }));
	}
    document.querySelector('[data-action="paste-rule"]')?.click();
    await waitFor(() => document.querySelector("#ruleName")?.value === "Browser rule",
	  "Rule Copy/Paste round-trips validated rule data");
    await new Promise(resolve => setTimeout(resolve, 100));

    document.querySelector('.rail-button[data-page="profiles"]')?.click();
    expect(!document.querySelector("#vsmr-profile-injection"),
      "profile names are rendered as text rather than markup");
    expect(document.querySelector("#profileList")?.textContent.includes(hostileProfileName),
      "escaped profile names remain readable");
    const profileName = document.querySelector("#profileName");
    expect(Boolean(profileName), "profile editor is rendered");
    if (profileName) {
      profileName.value = `${profileName.value || "Profile"} browser-test`;
      profileName.dispatchEvent(new Event("input", { bubbles: true }));
    }
    await waitFor(
      () => outbound.some(message => message.type === "state.save" &&
        Array.isArray(message.payload?.profiles)),
      "profile edit emits a state.save request"
    );

	const modeSaveStart = outbound.length;
	document.querySelector('.rail-button[data-page="modes"]')?.click();
	const readyRequirement = document.querySelector("#reqReady");
	expect(Boolean(readyRequirement), "Modes expose the Ready requirement");
	if (readyRequirement) {
		readyRequirement.checked = true;
		readyRequirement.dispatchEvent(new Event("change", { bubbles: true }));
	}
	await waitFor(
		() => outbound.slice(modeSaveStart).some(message => message.type === "state.save" &&
			JSON.stringify(message.payload?.profiles || []).includes('"require_ready":true')),
		"Modes persist the Ready requirement"
	);

    await new Promise(resolve => setTimeout(resolve, 100));
    const groupsButton = document.querySelector('.rail-button[data-page="groups"]');
    groupsButton?.click();
    expect(groupsButton?.classList.contains("active"), "AVISO groups page navigation is bound");
    expect(!document.querySelector("#vsmr-group-injection"),
      "AVISO group names are rendered as text rather than markup");
    expect(document.querySelector("#avisoGroupList")?.textContent.includes(hostileGroupName),
      "escaped AVISO group names remain readable");
    const newGroupButton = document.querySelector('[data-action="new-aviso-group"]');
    expect(Boolean(newGroupButton), "AVISO group action is rendered");
    newGroupButton?.click();
    await waitFor(
      () => outbound.some(message => message.type === "state.save" &&
        message.payload?.aviso?.type === "FeatureCollection"),
      "AVISO edit emits a state.save request"
    );

    // Exercise every shared list while it is visible so computed dimensions
    // catch page-specific overrides as well as markup regressions.
    document.querySelector('[data-action="open-control-center"]')?.click();
    expect(!document.querySelector("#controlWindow")?.classList.contains("hidden"),
      "Control Center opens for component layout coverage");
    displayButton?.click();
    const colorsTab = document.querySelector('[data-profile-tab="colors"]');
    colorsTab?.click();
    sampleSharedList("#colorTree", "Profile colors");
    sampleVisiblePrimitives();
    const colorList = document.querySelector("#colorTree");
    const colorScrollShell = colorList?.closest(".scroll-cue-shell");
    expect(Boolean(colorScrollShell), "shared lists use the edge-fade scroll shell");
    expect(getComputedStyle(colorList).scrollbarWidth === "none",
      "shared lists hide the native scrollbar");
    if (colorScrollShell && colorList) {
      colorScrollShell.style.height = "56px";
      const lowerCue = colorScrollShell.querySelector(".scroll-edge-cue-bottom");
      if (lowerCue) lowerCue.style.transition = "none";
      await waitFor(() => colorScrollShell.classList.contains("can-scroll-down"),
        "overflowing lists expose the lower scroll cue");
      await waitFor(() => Boolean(lowerCue) && Number.parseFloat(getComputedStyle(lowerCue).opacity) > 0,
        "the lower list fade becomes visible");
      const lowerCueStyle = lowerCue ? getComputedStyle(lowerCue) : null;
      const lowerArrowStyle = lowerCue?.firstElementChild
        ? getComputedStyle(lowerCue.firstElementChild) : null;
      expect(Boolean(lowerCueStyle) && lowerCueStyle.backgroundImage.includes("linear-gradient"),
        "overflowing lists use a visible edge fade instead of a scroll rail");
      expect(Number.parseFloat(lowerArrowStyle?.borderTopWidth || "0") > 0,
        "the lower list fade includes a small direction arrow");
      colorScrollShell.style.height = "";
    }
    const liveColorRows = () => {
	  const list = Array.from(document.querySelectorAll('#colorTree [role="listbox"]'))
		.find(candidate => candidate.querySelectorAll('.ui-list__row[role="option"]').length > 1);
	  return Array.from(list?.querySelectorAll('.ui-list__row[role="option"]') || [])
		.filter(row => row.getBoundingClientRect().height > 0);
	};
    const colorKeyboardRows = liveColorRows();
    if (colorKeyboardRows.length > 1) {
	  colorKeyboardRows[0].click();
	  const rowsAfterFirstSelection = liveColorRows();
	  rowsAfterFirstSelection[1]?.dispatchEvent(new MouseEvent("click", {
		bubbles: true,
		cancelable: true,
		ctrlKey: true
	  }));
	  expect(document.querySelectorAll('#colorTree .ui-list__row[aria-selected="true"]').length === 2 &&
		document.querySelector("#selectedColorPath")?.textContent === "2 colors",
		"Profile colors support additive multi-selection");
	  const multiColorHex = document.querySelector("#colorHex");
	  if (multiColorHex) {
		multiColorHex.value = "#123456";
		multiColorHex.dispatchEvent(new Event("input", { bubbles: true }));
	  }
	  const rowsAfterMultiSelection = liveColorRows();
	  expect(rowsAfterMultiSelection.slice(0, 2).every(row =>
		row.style.getPropertyValue("--node-color") === "#123456"),
		"a profile-color edit applies to every selected color");
      rowsAfterMultiSelection[0].focus();
      pressKey(rowsAfterMultiSelection[0], "ArrowDown");
      expect(document.activeElement === rowsAfterMultiSelection[1],
        "shared list keyboard navigation moves focus to the next row");
    } else {
      expect(false, "Profile colors provide two rows for keyboard navigation coverage");
    }

    colorsTab?.focus();
    pressKey(colorsTab, "ArrowRight");
    const iconsTab = document.querySelector('[data-profile-tab="icons"]');
    expect(document.activeElement === iconsTab && iconsTab?.getAttribute("aria-selected") === "true",
      "shared tabs move focus and selection with ArrowRight");
    expect(colorsTab?.getAttribute("aria-selected") === "false",
      "shared tabs clear the previous ARIA selection");
    expect(Boolean(document.querySelector(".icon-preview-column")) &&
      Boolean(document.querySelector(".icon-settings-stack")),
      "Icons use a dedicated preview column and shared settings cards");
    const symbolScaleRange = document.querySelector("#targetSymbolScale");
    expect(symbolScaleRange?.min === "0.25" && symbolScaleRange?.max === "5",
      "target symbol scaling exposes the complete 0.25× to 5.00× range");
    const iconPreviewStage = document.querySelector(".icon-preview-stage");
    const initialIconPreviewBackground = iconPreviewStage
      ? getComputedStyle(iconPreviewStage).backgroundColor
      : "";
    const previewFlight = document.querySelector(".icon-preview-flight");
    const previewTrail = previewFlight?.querySelector(".icon-preview-trail");
    const previewSymbol = previewFlight?.querySelector(".icon-preview-symbol");
    const previewTrailRect = previewTrail?.getBoundingClientRect();
    const previewSymbolRect = previewSymbol?.getBoundingClientRect();
    expect(Boolean(previewFlight) && getComputedStyle(previewFlight).display === "flex" &&
      Boolean(previewTrailRect) && Boolean(previewSymbolRect) &&
      previewTrailRect.right <= previewSymbolRect.left + .5 &&
      Math.abs((previewTrailRect.top + previewTrailRect.bottom) / 2 -
        (previewSymbolRect.top + previewSymbolRect.bottom) / 2) < 1,
      "target preview places the horizontal trail directly behind the aircraft");
    const originalSymbolScale = symbolScaleRange?.value || "1";
    const stablePreviewWidth = previewSymbolRect?.width;
    if (symbolScaleRange) {
      symbolScaleRange.value = "5";
      symbolScaleRange.dispatchEvent(new Event("input", { bubbles: true }));
    }
    expect(document.querySelector("#targetSymbolScaleOutput")?.value === "5.00×" &&
      Math.abs((previewSymbol?.getBoundingClientRect().width || 0) - stablePreviewWidth) < .5,
      "maximum symbol scale updates its value without resizing the reference preview");
    if (symbolScaleRange) {
      symbolScaleRange.value = "0.25";
      symbolScaleRange.dispatchEvent(new Event("input", { bubbles: true }));
    }
    expect(document.querySelector("#targetSymbolScaleOutput")?.value === "0.25×" &&
      Math.abs((previewSymbol?.getBoundingClientRect().width || 0) - stablePreviewWidth) < .5,
      "minimum symbol scale updates its value without resizing the reference preview");
    if (symbolScaleRange) {
      symbolScaleRange.value = originalSymbolScale;
      symbolScaleRange.dispatchEvent(new Event("input", { bubbles: true }));
    }
    const visibleIconRanges = Array.from(document.querySelectorAll(
      '#profilePanelIcons input[type="range"]')).filter(isVisible);
    expect(visibleIconRanges.length === 3 && visibleIconRanges.every(range =>
      range.getBoundingClientRect().height === visibleIconRanges[0].getBoundingClientRect().height),
      "Icon sliders use one shared range-control geometry");

	document.querySelector('[data-profile-tab="tags"]')?.click();
    sampleSharedList("#tagDefinitionList", "Tags");
    sampleVisiblePrimitives();
    expect(["vsid_sid", "vsid_rwy", "vsid_cfl"].every(token =>
      Boolean(document.querySelector(`#tagTokenSelect option[value="${token}"]`))),
      "Tags expose every vSID bridge field");
		expect(["ready_startup", "tobt", "tsat", "ttot", "ctot", "tsac", "asrt", "asat"].every(token =>
			Boolean(document.querySelector(`#tagTokenSelect option[value="${token}"]`))),
			"Tags expose the selected CDM bridge fields without prefixes");
		expect(["cdm_deice", "cdm_tobt_set_by", "cdm_flow_restriction",
			"cdm_ecfmp_restriction", "cdm_manual_ctot"].every(token =>
			!document.querySelector(`#tagTokenSelect option[value="${token}"]`)),
			"Tags omit unused CDM metadata fields");
    const tagBehaviour = document.querySelector(".tag-behaviour-grid");
    const tagBehaviourControls = Array.from(tagBehaviour?.querySelectorAll(".check-field") || []);
    expect(Boolean(tagBehaviour) && tagBehaviour.scrollWidth <= tagBehaviour.clientWidth &&
      tagBehaviourControls.every(control => control.scrollWidth <= control.clientWidth),
      "Tag Options behaviour controls remain contained without text collisions");

    document.querySelector('.rail-button[data-page="profiles"]')?.click();
    sampleSharedList("#profileList", "Profiles");
    sampleVisiblePrimitives();

    document.querySelector('.rail-button[data-page="aviso"]')?.click();
    document.querySelector('[data-aviso-view="geometry"]')?.click();
    sampleSharedList("#avisoGeometryStyleList", "Geometry");
    expect(Boolean(document.querySelector('[data-action="copy-aviso-geometry"]')) &&
      Boolean(document.querySelector('[data-action="paste-aviso-geometry"]')),
      "AVISO geometry exposes shared copy and paste actions");
    const originalGeometryColor = document.querySelector("#avisoGeometryColorHex")?.value;
    document.querySelector('[data-action="copy-aviso-geometry"]')?.click();
    await new Promise(resolve => setTimeout(resolve, 30));
    const changedGeometryColor = document.querySelector("#avisoGeometryColorHex");
    if (changedGeometryColor) {
	  changedGeometryColor.value = "#123456";
	  changedGeometryColor.dispatchEvent(new Event("input", { bubbles: true }));
	}
    document.querySelector('[data-action="paste-aviso-geometry"]')?.click();
    await waitFor(() => document.querySelector("#avisoGeometryColorHex")?.value === originalGeometryColor,
	  "AVISO geometry Copy/Paste round-trips validated paint data");
    sampleVisiblePrimitives();
    document.querySelector('[data-aviso-view="text"]')?.click();
    sampleSharedList("#avisoTextStyleList", "Text");
    expect(Boolean(document.querySelector('[data-action="copy-aviso-text"]')) &&
      Boolean(document.querySelector('[data-action="paste-aviso-text"]')),
      "AVISO text exposes shared copy and paste actions");
    const originalTextFont = document.querySelector("#avisoTextFont")?.value;
    document.querySelector('[data-action="copy-aviso-text"]')?.click();
    await new Promise(resolve => setTimeout(resolve, 30));
    const changedTextFont = document.querySelector("#avisoTextFont");
    if (changedTextFont) {
	  changedTextFont.value = "Courier New";
	  changedTextFont.dispatchEvent(new Event("input", { bubbles: true }));
	}
    document.querySelector('[data-action="paste-aviso-text"]')?.click();
    await waitFor(() => document.querySelector("#avisoTextFont")?.value === originalTextFont,
	  "AVISO text Copy/Paste round-trips validated paint data");
    sampleVisiblePrimitives();

    const palette = document.querySelector(".aviso-palette-control.ui-toggle-group");
    const paletteButtons = Array.from(
      palette?.querySelectorAll(".ui-button.ui-button--toggle[data-aviso-color-palette]") || []
    );
    expect(paletteButtons.length === 3 &&
      paletteButtons.map(button => button.dataset.avisoColorPalette).join(",") === "dark,light,real",
      "AVISO uses one Dark/Light/Real toggle component");
    const paletteRects = paletteButtons.map(button => button.getBoundingClientRect());
    expect(paletteRects.length === 3 &&
      paletteRects.every(rect => Math.abs(rect.width - paletteRects[0].width) < .5 &&
        Math.abs(rect.height - paletteRects[0].height) < .5),
      "AVISO palette options have matching dimensions");
    const paletteRect = palette?.getBoundingClientRect();
    expect(Boolean(paletteRect) && paletteRects.every(rect =>
      Math.abs(rect.top - (paletteRect.top + 1)) < .75 &&
      Math.abs(rect.bottom - (paletteRect.bottom - 1)) < .75),
      "AVISO palette options fill and align within the shared toggle boundary");
    expect(paletteButtons.every(button => {
      const style = getComputedStyle(button);
      return ["flex", "inline-flex"].includes(style.display) && style.alignItems === "center" &&
        style.justifyContent === "center";
    }), "AVISO palette labels use the shared centered button layout");
    const originalPaletteButton = paletteButtons.find(
      button => button.getAttribute("aria-pressed") === "true");
    const alternatePaletteButton = paletteButtons.find(
      button => button.dataset.avisoColorPalette === "real");
    const originalUiTheme = document.documentElement.dataset.uiTheme;
    const originalPageBackground = getComputedStyle(document.documentElement)
      .getPropertyValue("--ui-page-bg").trim();
    expect(Boolean(originalPaletteButton) && paletteButtons.filter(
      button => button.getAttribute("aria-pressed") === "true").length === 1,
      "Dark/Light/Real exposes exactly one active option");
    alternatePaletteButton?.click();
    expect(alternatePaletteButton?.getAttribute("aria-pressed") === "true" &&
      originalPaletteButton?.getAttribute("aria-pressed") === "false" &&
      api.getState().settings?.avisoColorPalette === alternatePaletteButton?.dataset.avisoColorPalette,
      "Dark/Light/Real updates visual, accessible, and application state together");
    expect(document.documentElement.dataset.uiTheme === originalUiTheme &&
      getComputedStyle(document.documentElement).getPropertyValue("--ui-page-bg").trim() ===
        originalPageBackground,
      "AVISO palette changes do not alter the application UI theme");
    originalPaletteButton?.click();

    const completePaletteState = api.getState();
    api.receive({
      type: "state.authoritative",
      payload: {
        settings: { ...completePaletteState.settings, avisoColorPalette: "dark", avisoColorPalettes: ["dark", "light"] },
        aviso: completePaletteState.aviso,
        airport: completePaletteState.airport,
        avisoFollows: false,
        reason: "reload"
      }
    });
    const unavailableRealButton = document.querySelector('[data-aviso-color-palette="real"]');
    const paletteBeforeDisabledClick = api.getState().settings.avisoColorPalette;
    unavailableRealButton?.click();
    expect(unavailableRealButton?.disabled &&
      getComputedStyle(unavailableRealButton).opacity === "1" &&
      api.getState().settings.avisoColorPalette === paletteBeforeDisabledClick,
      "Unavailable AVISO palettes are gray, disabled, and cannot change state");
    api.receive({
      type: "state.authoritative",
      payload: {
        settings: { ...completePaletteState.settings, avisoColorPalette: "dark", avisoColorPalettes: ["real"] },
        aviso: completePaletteState.aviso,
        airport: "TEST",
        avisoFollows: false,
        reason: "reload"
      }
    });
    expect(api.getState().settings.avisoColorPalette === "real" &&
      document.querySelector('[data-aviso-color-palette="dark"]')?.disabled &&
      document.querySelector('[data-aviso-color-palette="light"]')?.disabled &&
      !document.querySelector('[data-aviso-color-palette="real"]')?.disabled,
      "An airport palette change selects its first valid state and disables missing alternatives");
    api.receive({
      type: "state.authoritative",
      payload: {
        settings: { ...completePaletteState.settings, avisoColorPalette: originalPaletteButton?.dataset.avisoColorPalette, avisoColorPalettes: ["dark", "light", "real"] },
        aviso: completePaletteState.aviso,
        airport: completePaletteState.airport,
        avisoFollows: false,
        reason: "reload"
      }
    });

    document.querySelector('.rail-button[data-page="settings"]')?.click();
    expect(!document.querySelector('[data-action="restore-profiles-backup"]'),
      "legacy profiles backup recovery is absent from the UI");
    const uiThemeButtons = Array.from(document.querySelectorAll(
      '.settings-theme-control .ui-button--toggle[data-ui-color-theme]'));
    const originalUiThemeButton = uiThemeButtons.find(
      button => button.getAttribute("aria-pressed") === "true");
    const alternateUiThemeButton = uiThemeButtons.find(button => button !== originalUiThemeButton);
    expect(uiThemeButtons.length === 2 && Boolean(originalUiThemeButton),
      "Settings provides one accessible Day/Night UI theme control");
    expect(document.querySelector("#runtimeThemeButton") === null,
      "Runtime Menu does not duplicate the UI theme control");
    alternateUiThemeButton?.click();
    expect(document.documentElement.dataset.uiTheme === alternateUiThemeButton?.dataset.uiColorTheme &&
      getComputedStyle(document.documentElement).colorScheme ===
        (alternateUiThemeButton?.dataset.uiColorTheme === "day" ? "light" : "dark") &&
      api.getState().settings?.uiColorTheme === alternateUiThemeButton?.dataset.uiColorTheme,
      "Settings Day/Night applies visual, accessible, and application UI theme state together");
    expect(getComputedStyle(document.documentElement).getPropertyValue("--ui-page-bg").trim() !==
      originalPageBackground &&
      api.getState().settings?.avisoColorPalette === originalPaletteButton?.dataset.avisoColorPalette,
      "UI theme changes shared design colors without changing the AVISO palette");
    expect(Boolean(iconPreviewStage) &&
      getComputedStyle(iconPreviewStage).backgroundColor !== initialIconPreviewBackground,
      "target icon preview follows the selected Day/Night UI background");
    originalUiThemeButton?.click();
    expect(document.documentElement.dataset.uiTheme === originalUiTheme,
      "switching back restores the original UI theme");

    const settingsLeft = document.querySelector(".settings-page .display-group")
      ?.getBoundingClientRect().left;
    groupsButton?.click();
    const groupsLeft = document.querySelector(".groups-page .master-panel")
      ?.getBoundingClientRect().left;
    document.querySelector('.rail-button[data-page="profiles"]')?.click();
    const profilesLeft = document.querySelector(".profiles-page .master-panel")
      ?.getBoundingClientRect().left;
    expect([settingsLeft, groupsLeft, profilesLeft].every(Number.isFinite) &&
      Math.max(settingsLeft, groupsLeft, profilesLeft) -
        Math.min(settingsLeft, groupsLeft, profilesLeft) < .5,
      "Groups and Settings share the standard page-left offset");
    groupsButton?.click();
    sampleSharedList("#avisoGroupList", "Groups");
    sampleVisiblePrimitives();

    const referenceListSample = sharedListSamples[0];
    const rowHeightToken = Number.parseFloat(getComputedStyle(document.documentElement)
      .getPropertyValue("--ui-list-row-height"));
    expect(sharedListSamples.length === 6, "all six requested list types are audited");
    expect(sharedListSamples.every(sample =>
      Math.abs(sample.rowHeight - referenceListSample.rowHeight) < .5 &&
      sample.rowSignature === referenceListSample.rowSignature),
      "all list types share row height, spacing, typography, and border geometry");
    expect(Number.isFinite(rowHeightToken) && sharedListSamples.every(sample =>
      Math.abs(sample.rowHeight - rowHeightToken) < .5),
      "all list rows are sized by the shared list-height token");
    expect(sharedListSamples.every(sample =>
      Math.abs(sample.listWidth - referenceListSample.listWidth) < 1),
      "all list types use the same master-list width");

    const rootStyle = getComputedStyle(document.documentElement);
    const controlHeightToken = Number.parseFloat(rootStyle.getPropertyValue("--ui-control-height"));
    const controlRadiusToken = Number.parseFloat(rootStyle.getPropertyValue("--ui-control-radius"));
    const sampledControlKinds = new Set(formControlSamples.map(sample => sample.kind));
    expect(["text", "search", "number", "select"].every(kind => sampledControlKinds.has(kind)),
      "text, search, number, and select controls are covered while visible");
    const mismatchedFormControls = formControlSamples.filter(sample =>
      Math.abs(sample.height - controlHeightToken) >= .5 ||
      Math.abs(sample.radius - controlRadiusToken) >= .5);
    expect(formControlSamples.length > 0 && mismatchedFormControls.length === 0,
      `single-line form controls use the shared height and radius tokens${
        mismatchedFormControls.length ? `: ${mismatchedFormControls.map(sample =>
          `${sample.id || sample.kind} ${sample.height}px/${sample.radius}px`).join(", ")}` : ""}`);
    expect(formControlSamples.every(sample =>
      sample.focusBorderColor !== sample.baseBorderColor && sample.focusBoxShadow !== "none"),
      "single-line form controls use the shared visible focus treatment");
    const focusSignatures = new Set(formControlSamples.map(sample =>
      `${sample.focusBorderColor}|${sample.focusBoxShadow}`));
    expect(focusSignatures.size === 1,
      "single-line form controls resolve one shared focus style");

    const panelTitleHeightToken = Number.parseFloat(
      rootStyle.getPropertyValue("--ui-panel-title-height"));
    expect(panelSamples.length > 0 && new Set(
      panelSamples.map(sample => sample.signature)).size === 1,
      "visible panels share one border, radius, and background contract");
    expect(panelSamples.every(sample => sample.titleHeight === null ||
      Math.abs(sample.titleHeight - panelTitleHeightToken) < .5),
      "visible panel titles use the shared title-height token");

    const unsharedActionButtons = Array.from(document.querySelectorAll("button[data-action]"))
      .filter(button => !button.classList.contains("ui-button"));
    expect(unsharedActionButtons.length === 0,
      `all action buttons use the shared button component${unsharedActionButtons.length
        ? `: ${unsharedActionButtons.map(button => button.dataset.action).join(", ")}` : ""}`);
    const unownedButtons = Array.from(document.querySelectorAll("button")).filter(button =>
      !button.matches(
        ".ui-button, .ui-list__row, .ui-list__heading, .ui-list__selection, .ui-field-control"));
    expect(unownedButtons.length === 0,
      `every button belongs to a shared component${unownedButtons.length
        ? `: ${unownedButtons.map(button => button.id || button.className).join(", ")}` : ""}`);
    expect(!document.querySelector(
      'button :is(button, input, select, textarea, a[href], [role="button"], [role="checkbox"], [role="switch"])'),
      "shared buttons never contain a second interactive control");
    expect(Array.from(document.querySelectorAll('[role="tab"]')).every(button =>
      button.matches(".ui-button.ui-button--tab")),
      "all tabs use the shared tab-button variant");
  } catch (error) {
    failures.push(`unexpected browser exception: ${error?.stack || error}`);
  }

  result.dataset.status = failures.length ? "failed" : "passed";
  result.textContent = failures.length ? failures.join("\n") : "Control Center browser tests passed";
  document.documentElement.dataset.vsmrBrowserTests = result.dataset.status;
})();
