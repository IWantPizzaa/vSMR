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
    if (authoritative.profiles[0]) authoritative.profiles[0].name = hostileProfileName;
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

    const outbound = [];
    window.addEventListener("vsmr-control-center", event => outbound.push(event.detail));
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
    const colorKeyboardList = Array.from(document.querySelectorAll('#colorTree [role="listbox"]'))
      .find(list => list.querySelectorAll('.ui-list__row[role="option"]').length > 1);
    const colorKeyboardRows = Array.from(colorKeyboardList?.querySelectorAll(
      '.ui-list__row[role="option"]') || []).filter(row => row.getBoundingClientRect().height > 0);
    if (colorKeyboardRows.length > 1) {
      colorKeyboardRows[0].focus();
      pressKey(colorKeyboardRows[0], "ArrowDown");
      expect(document.activeElement === colorKeyboardRows[1],
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

    document.querySelector('[data-profile-tab="tags"]')?.click();
    sampleSharedList("#tagDefinitionList", "Tags");
    sampleVisiblePrimitives();

    document.querySelector('.rail-button[data-page="profiles"]')?.click();
    sampleSharedList("#profileList", "Profiles");
    sampleVisiblePrimitives();

    document.querySelector('.rail-button[data-page="aviso"]')?.click();
    document.querySelector('[data-aviso-view="geometry"]')?.click();
    sampleSharedList("#avisoGeometryStyleList", "Geometry");
    sampleVisiblePrimitives();
    document.querySelector('[data-aviso-view="text"]')?.click();
    sampleSharedList("#avisoTextStyleList", "Text");
    sampleVisiblePrimitives();

    const palette = document.querySelector(".aviso-palette-control.ui-toggle-group");
    const paletteButtons = Array.from(
      palette?.querySelectorAll(".ui-button.ui-button--toggle[data-aviso-color-palette]") || []
    );
    expect(paletteButtons.length === 2, "Day/Night uses one two-option toggle component");
    const paletteRects = paletteButtons.map(button => button.getBoundingClientRect());
    expect(paletteRects.length === 2 &&
      Math.abs(paletteRects[0].width - paletteRects[1].width) < .5 &&
      Math.abs(paletteRects[0].height - paletteRects[1].height) < .5,
      "Day/Night options have matching dimensions");
    const paletteRect = palette?.getBoundingClientRect();
    expect(Boolean(paletteRect) && paletteRects.every(rect =>
      Math.abs(rect.top - (paletteRect.top + 1)) < .75 &&
      Math.abs(rect.bottom - (paletteRect.bottom - 1)) < .75),
      "Day/Night options fill and align within the shared toggle boundary");
    expect(paletteButtons.every(button => {
      const style = getComputedStyle(button);
      return ["flex", "inline-flex"].includes(style.display) && style.alignItems === "center" &&
        style.justifyContent === "center";
    }), "Day/Night labels use the shared centered button layout");
    const originalPaletteButton = paletteButtons.find(
      button => button.getAttribute("aria-pressed") === "true");
    const alternatePaletteButton = paletteButtons.find(button => button !== originalPaletteButton);
    const originalUiTheme = document.documentElement.dataset.uiTheme;
    const originalPageBackground = getComputedStyle(document.documentElement)
      .getPropertyValue("--ui-page-bg").trim();
    expect(Boolean(originalPaletteButton) && paletteButtons.filter(
      button => button.getAttribute("aria-pressed") === "true").length === 1,
      "Day/Night exposes exactly one active option");
    alternatePaletteButton?.click();
    expect(alternatePaletteButton?.getAttribute("aria-pressed") === "true" &&
      originalPaletteButton?.getAttribute("aria-pressed") === "false" &&
      api.getState().settings?.avisoColorPalette === alternatePaletteButton?.dataset.avisoColorPalette,
      "Day/Night updates visual, accessible, and application state together");
    expect(document.documentElement.dataset.uiTheme === originalUiTheme &&
      getComputedStyle(document.documentElement).getPropertyValue("--ui-page-bg").trim() ===
        originalPageBackground,
      "AVISO palette changes do not alter the application UI theme");
    originalPaletteButton?.click();

    document.querySelector('.rail-button[data-page="settings"]')?.click();
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
    originalUiThemeButton?.click();
    expect(document.documentElement.dataset.uiTheme === originalUiTheme,
      "switching back restores the original UI theme");

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
