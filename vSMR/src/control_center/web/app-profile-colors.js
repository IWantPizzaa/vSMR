"use strict";

  function collectProfileColors(profile) {
    const entries = [];
    const roots = ["labels", "rimcas", "targets", "approach_insets"];
    function visit(value, path) {
      if (isColorObject(value)) {
        const groupInfo = colorGroupInfo(path);
        entries.push({
          id: path.join("."),
          path,
          group: groupInfo.group,
          family: groupInfo.family,
          section: groupInfo.section,
          name: humanize(path[path.length - 1]),
          key: String(path[path.length - 1]),
          color: value
        });
        return;
      }
      if (!value || typeof value !== "object") return;
      if (Array.isArray(value)) value.forEach((item, index) => visit(item, [...path, index]));
      else Object.entries(value).forEach(([key, child]) => visit(child, [...path, key]));
    }
    roots.forEach(root => {
      if (root !== "targets") {
        visit(profile?.[root], [root]);
        return;
      }
      const targets = profile?.targets;
      const iconStyle = String(targets?.icon_style || "realistic").toLowerCase();
      if (iconStyle === "nova") {
        visit(targets?.target_color, ["targets", "target_color"]);
      } else {
        visit(targets?.departure, ["targets", "departure"]);
        visit(targets?.arrival, ["targets", "arrival"]);
      }
    });
    return entries.sort((a, b) => {
      const familyOrder = value => {
        const index = COLOR_FAMILY_ORDER.indexOf(value);
        return index < 0 ? 999 : index;
      };
      const sectionOrder = value => {
        const index = COLOR_SECTION_ORDER.indexOf(value || "");
        return index < 0 ? 999 : index;
      };
      const tagColorOrder = entry => {
        if (entry.family !== "Tags" || !TAG_SCOPES.includes(String(entry.path[1] || ""))) return 999;
        const scope = String(entry.path[1]);
        const statusKeys = ["default", ...TAG_STATUS_ORDER]
          .map(status => TAG_STATUS_COLOR_KEYS[scope]?.[status])
          .filter(Boolean);
        const trailingKeys = ["text_on_ground_color", "text_airborne_color", "text_color", "background_no_sid_color"];
        const orderedKeys = [...new Set([...statusKeys, ...trailingKeys])];
        const index = orderedKeys.indexOf(entry.key);
        return index < 0 ? 999 : index;
      };
      return familyOrder(a.family) - familyOrder(b.family)
        || sectionOrder(a.section) - sectionOrder(b.section)
        || a.family.localeCompare(b.family)
        || a.section.localeCompare(b.section)
        || tagColorOrder(a) - tagColorOrder(b)
        || a.name.localeCompare(b.name);
    });
  }

  function colorGroupInfo(path) {
    const [root, second] = path;
    if (root === "labels") {
      const section = second && TAG_SCOPES.includes(second) ? humanize(second) : "General";
      return { family: "Tags", section, group: `Tags · ${section}` };
    }
    if (root === "targets") {
      const section = second && ["departure", "arrival"].includes(second) ? humanize(second) : "General";
      return { family: "Targets", section, group: `Targets · ${section}` };
    }
    if (root === "rimcas") return { family: "RIMCAS", section: "", group: "RIMCAS" };
    if (root === "approach_insets") return { family: "SRW 1", section: "", group: "SRW 1" };
    const family = humanize(root);
    return { family, section: "", group: family };
  }

  function selectedColorEntry() {
    const entries = collectProfileColors(activeProfile());
    const selected = entries.find(entry => entry.id === state.ui.selectedColorPath) || entries[0];
    if (selected && state.ui.selectedColorPath !== selected.id) state.ui.selectedColorPath = selected.id;
    return selected;
  }

  function renderColors() {
    const entries = collectProfileColors(activeProfile());

    const groups = new Map();
    entries.forEach(entry => {
      const caption = entry.family === "Targets" && entry.section === "General"
        ? "Target"
        : entry.section ? `${entry.family} · ${entry.section}` : entry.family;
      const key = `${entry.family}:${entry.section || "general"}`;
      if (!groups.has(key)) groups.set(key, { key, caption, family: entry.family, section: entry.section, items: [] });
      groups.get(key).items.push(entry);
    });

    $("#colorTree").innerHTML = [...groups.values()].map(group => {
      const groupKey = `colors:${group.key}`;
      const collapsed = treeState.colors.has(groupKey);
      const accent = colorToHex(group.items[0]?.color, "#5096b4");
      const rows = group.items.map(entry => {
        const hex = colorToHex(entry.color).toUpperCase();
        const selected = entry.id === state.ui.selectedColorPath;
        return `<button type="button" role="option" aria-selected="${selected}" class="${uiListRowClass("color", selected, false, "color-menu-row")}" data-color-path="${escapeHtml(entry.id)}" style="--node-color:${hex}" title="${escapeHtml(entry.name)}">
          <span class="ui-list__leading menu-row-swatch tree-color-swatch" aria-hidden="true"></span>
          <span class="ui-list__label menu-row-title">${escapeHtml(entry.name)}</span>
        </button>`;
      }).join("");
      return `<section class="ui-list__section color-menu-section" style="--menu-accent:${accent}">
        <button type="button" class="ui-list__heading" data-tree-toggle="colors" data-tree-key="${escapeHtml(groupKey)}" aria-expanded="${!collapsed}">
          <span class="ui-list__caret" aria-hidden="true">${collapsed ? "▸" : "▾"}</span>
          <span class="ui-list__heading-label">${escapeHtml(group.caption)}</span>
        </button>
        <div aria-label="${escapeHtml(group.caption)} colors" class="ui-list__items" role="listbox" ${collapsed ? "hidden" : ""}>${rows}</div>
      </section>`;
    }).join("") || `<div class="ui-list__empty">No colors found</div>`;
    syncUiListFocus($("#colorTree"));
    requestAnimationFrame(() => $("#colorTree .color-menu-row.is-selected")?.scrollIntoView({ block: "nearest" }));
    renderColorEditor();
  }

  function rgbToHsv(r, g, b) {
    r = clamp(r, 0, 255) / 255;
    g = clamp(g, 0, 255) / 255;
    b = clamp(b, 0, 255) / 255;
    const max = Math.max(r, g, b);
    const min = Math.min(r, g, b);
    const delta = max - min;
    let h = 0;
    if (delta) {
      if (max === r) h = 60 * (((g - b) / delta) % 6);
      else if (max === g) h = 60 * ((b - r) / delta + 2);
      else h = 60 * ((r - g) / delta + 4);
    }
    if (h < 0) h += 360;
    return { h, s: max === 0 ? 0 : delta / max, v: max };
  }

  function hsvToRgb(h, s, v) {
    h = ((Number(h) % 360) + 360) % 360;
    s = clamp(s, 0, 1);
    v = clamp(v, 0, 1);
    const c = v * s;
    const x = c * (1 - Math.abs((h / 60) % 2 - 1));
    const m = v - c;
    let rp = 0, gp = 0, bp = 0;
    if (h < 60) [rp, gp, bp] = [c, x, 0];
    else if (h < 120) [rp, gp, bp] = [x, c, 0];
    else if (h < 180) [rp, gp, bp] = [0, c, x];
    else if (h < 240) [rp, gp, bp] = [0, x, c];
    else if (h < 300) [rp, gp, bp] = [x, 0, c];
    else [rp, gp, bp] = [c, 0, x];
    return {
      r: Math.round((rp + m) * 255),
      g: Math.round((gp + m) * 255),
      b: Math.round((bp + m) * 255)
    };
  }

  function createAvisoColorDraft(signature, colorResult, fallback, opacityResult = null) {
    const hex = normalizeHex(colorResult?.value, fallback);
    const rgb = hexToColor(hex);
    const hsv = rgbToHsv(rgb.r, rgb.g, rgb.b);
    return {
      signature,
      hex,
      h: hsv.h,
      s: hsv.s,
      v: hsv.v,
      colorMixed: Boolean(colorResult?.mixed),
      opacity: Math.round(clamp(opacityResult?.value ?? 100, 0, 100)),
      opacityMixed: Boolean(opacityResult?.mixed)
    };
  }

  function syncAvisoColorEditor(prefix, draft, includeOpacity = false) {
    if (!draft) return;
    const rgb = hexToColor(draft.hex);
    const hex = normalizeHex(draft.hex);
    const opacity = Math.round(clamp(draft.opacity, 0, 100));
    const selectedRgb = `rgb(${rgb.r}, ${rgb.g}, ${rgb.b})`;
    const element = suffix => $("#" + prefix + suffix);

    const hexInput = element("Hex");
    hexInput.value = draft.colorMixed ? "" : hex.toUpperCase();
    hexInput.placeholder = draft.colorMixed ? "Mixed" : "#RRGGBB";
    element("Hue").value = Math.round(draft.h);
    element("Hue").style.setProperty("--hue-slider-value", String(Math.round(draft.h)));
    element("HueOutput").value = draft.colorMixed ? "" : String(Math.round(draft.h));
    element("Red").value = rgb.r;
    element("Green").value = rgb.g;
    element("Blue").value = rgb.b;
    element("RedOutput").value = draft.colorMixed ? "" : String(rgb.r);
    element("GreenOutput").value = draft.colorMixed ? "" : String(rgb.g);
    element("BlueOutput").value = draft.colorMixed ? "" : String(rgb.b);

    const configureChannel = (suffix, low, high, thumb = selectedRgb) => {
      const slider = element(suffix);
      slider.style.setProperty("--channel-low", low);
      slider.style.setProperty("--channel-high", high);
      slider.style.setProperty("--channel-thumb", thumb);
    };
    configureChannel("Red", `rgb(0, ${rgb.g}, ${rgb.b})`, `rgb(255, ${rgb.g}, ${rgb.b})`);
    configureChannel("Green", `rgb(${rgb.r}, 0, ${rgb.b})`, `rgb(${rgb.r}, 255, ${rgb.b})`);
    configureChannel("Blue", `rgb(${rgb.r}, ${rgb.g}, 0)`, `rgb(${rgb.r}, ${rgb.g}, 255)`);

    if (includeOpacity) {
      element("Opacity").value = opacity;
      element("OpacityOutput").value = draft.opacityMixed ? "" : String(opacity);
      configureChannel(
        "Opacity",
        `rgba(${rgb.r}, ${rgb.g}, ${rgb.b}, 0)`,
        selectedRgb,
        `rgba(${rgb.r}, ${rgb.g}, ${rgb.b}, ${opacity / 100})`
      );
    }

    const palette = element("SvPalette");
    palette.style.setProperty("--palette-hue", String(Math.round(draft.h)));
    palette.setAttribute("aria-valuenow", String(Math.round(draft.v * 100)));
    palette.setAttribute("aria-valuetext", `Saturation ${Math.round(draft.s * 100)}%, brightness ${Math.round(draft.v * 100)}%`);
    const cursor = element("PaletteCursor");
    cursor.style.left = `${draft.s * 100}%`;
    cursor.style.top = `${(1 - draft.v) * 100}%`;
    const editor = element("Editor");
    editor?.classList.toggle("mixed", draft.colorMixed || (includeOpacity && draft.opacityMixed));
    const swatch = element("Swatch");
    swatch.style.setProperty("--swatch-color", `rgba(${rgb.r}, ${rgb.g}, ${rgb.b}, ${includeOpacity ? opacity / 100 : 1})`);
    swatch.style.setProperty("--swatch-solid", hex);
  }

  function initializeAvisoColorControls(prefix, draft, includeOpacity = false) {
    ["Hex", "Hue", "HueOutput", "Red", "RedOutput", "Green", "GreenOutput", "Blue", "BlueOutput"]
      .forEach(suffix => resetControlFlags($("#" + prefix + suffix), draft.colorMixed));
    if (includeOpacity) {
      resetControlFlags($("#" + prefix + "Opacity"), draft.opacityMixed);
      resetControlFlags($("#" + prefix + "OpacityOutput"), draft.opacityMixed);
    }
    syncAvisoColorEditor(prefix, draft, includeOpacity);
  }

  function updateAvisoColorDraftFromRgb(draft, prefix, r, g, b, includeOpacity = false) {
    if (!draft) return;
    const rgb = { r: Math.round(clamp(r, 0, 255)), g: Math.round(clamp(g, 0, 255)), b: Math.round(clamp(b, 0, 255)) };
    const hsv = rgbToHsv(rgb.r, rgb.g, rgb.b);
    draft.hex = colorToHex(rgb);
    draft.h = hsv.h;
    draft.s = hsv.s;
    draft.v = hsv.v;
    draft.colorMixed = false;
    markControlTouched($("#" + prefix + "Hex"));
    syncAvisoColorEditor(prefix, draft, includeOpacity);
  }

  function updateAvisoColorDraftFromHex(draft, prefix, value, includeOpacity = false) {
    if (!draft) return;
    const normalized = normalizeHex(value, draft.hex || "#ffffff");
    const rgb = hexToColor(normalized);
    const hsv = rgbToHsv(rgb.r, rgb.g, rgb.b);
    draft.hex = normalized;
    draft.h = hsv.h;
    draft.s = hsv.s;
    draft.v = hsv.v;
    draft.colorMixed = false;
    markControlTouched($("#" + prefix + "Hex"));
    syncAvisoColorEditor(prefix, draft, includeOpacity);
  }

  function updateAvisoColorDraftFromHsv(draft, prefix, h, saturation, value, includeOpacity = false) {
    if (!draft) return;
    draft.h = ((Number(h) % 360) + 360) % 360;
    draft.s = clamp(saturation, 0, 1);
    draft.v = clamp(value, 0, 1);
    const rgb = hsvToRgb(draft.h, draft.s, draft.v);
    draft.hex = colorToHex(rgb);
    draft.colorMixed = false;
    markControlTouched($("#" + prefix + "Hex"));
    syncAvisoColorEditor(prefix, draft, includeOpacity);
  }

  function setColorDraftFromRgb(r, g, b) {
    if (!drafts.color) return;
    markEditorSectionUnapplied($("#colorSvPalette"));
    const rgb = { r: Math.round(clamp(r, 0, 255)), g: Math.round(clamp(g, 0, 255)), b: Math.round(clamp(b, 0, 255)) };
    const hsv = rgbToHsv(rgb.r, rgb.g, rgb.b);
    drafts.color.hex = colorToHex(rgb);
    drafts.color.h = hsv.h;
    drafts.color.s = hsv.s;
    drafts.color.v = hsv.v;
    syncColorEditorControls();
  }

  function setColorDraftFromHex(value) {
    if (!drafts.color) return;
    markEditorSectionUnapplied($("#colorHex"));
    const normalized = normalizeHex(value, drafts.color.hex || "#ffffff");
    const rgb = hexToColor(normalized);
    const hsv = rgbToHsv(rgb.r, rgb.g, rgb.b);
    drafts.color.hex = normalized;
    drafts.color.h = hsv.h;
    drafts.color.s = hsv.s;
    drafts.color.v = hsv.v;
    syncColorEditorControls();
  }

  function setColorDraftFromHsv(h, saturation, value) {
    if (!drafts.color) return;
    markEditorSectionUnapplied($("#colorSvPalette"));
    drafts.color.h = ((Number(h) % 360) + 360) % 360;
    drafts.color.s = clamp(saturation, 0, 1);
    drafts.color.v = clamp(value, 0, 1);
    const rgb = hsvToRgb(drafts.color.h, drafts.color.s, drafts.color.v);
    drafts.color.hex = colorToHex(rgb);
    syncColorEditorControls();
  }

  function syncColorEditorControls() {
    if (!drafts.color) return;
    const rgb = hexToColor(drafts.color.hex);
    const hsv = Number.isFinite(drafts.color.h) ? drafts.color : rgbToHsv(rgb.r, rgb.g, rgb.b);
    drafts.color.h = hsv.h;
    drafts.color.s = hsv.s;
    drafts.color.v = hsv.v;
    const hex = normalizeHex(drafts.color.hex);
    const opacity = Math.round(clamp(drafts.color.opacity, 0, 100));

    $("#colorHex").value = hex.toUpperCase();
    $("#colorHue").value = Math.round(drafts.color.h);
    $("#colorHue").style.setProperty("--hue-slider-value", String(Math.round(drafts.color.h)));
    $("#colorHueOutput").value = String(Math.round(drafts.color.h));
    $("#colorRed").value = rgb.r;
    $("#colorGreen").value = rgb.g;
    $("#colorBlue").value = rgb.b;
    const selectedRgb = `rgb(${rgb.r}, ${rgb.g}, ${rgb.b})`;
    const configureChannelSlider = (selector, low, high, thumb = selectedRgb) => {
      const slider = $(selector);
      slider.style.setProperty("--channel-low", low);
      slider.style.setProperty("--channel-high", high);
      slider.style.setProperty("--channel-thumb", thumb);
    };
    configureChannelSlider("#colorRed", `rgb(0, ${rgb.g}, ${rgb.b})`, `rgb(255, ${rgb.g}, ${rgb.b})`);
    configureChannelSlider("#colorGreen", `rgb(${rgb.r}, 0, ${rgb.b})`, `rgb(${rgb.r}, 255, ${rgb.b})`);
    configureChannelSlider("#colorBlue", `rgb(${rgb.r}, ${rgb.g}, 0)`, `rgb(${rgb.r}, ${rgb.g}, 255)`);
    configureChannelSlider(
      "#colorOpacity",
      `rgba(${rgb.r}, ${rgb.g}, ${rgb.b}, 0)`,
      selectedRgb,
      `rgba(${rgb.r}, ${rgb.g}, ${rgb.b}, ${opacity / 100})`
    );
    $("#colorRedOutput").value = rgb.r;
    $("#colorGreenOutput").value = rgb.g;
    $("#colorBlueOutput").value = rgb.b;
    $("#colorOpacity").value = opacity;
    $("#colorOpacityOutput").value = String(opacity);

    const palette = $("#colorSvPalette");
    palette.style.setProperty("--palette-hue", String(Math.round(drafts.color.h)));
    palette.setAttribute("aria-valuenow", String(Math.round(drafts.color.v * 100)));
    palette.setAttribute(
      "aria-valuetext",
      `Saturation ${Math.round(drafts.color.s * 100)}%, brightness ${Math.round(drafts.color.v * 100)}%`
    );
    const cursor = $("#colorPaletteCursor");
    cursor.style.left = `${drafts.color.s * 100}%`;
    cursor.style.top = `${(1 - drafts.color.v) * 100}%`;
    const swatch = $("#colorSwatch");
    swatch.style.setProperty("--swatch-color", `rgba(${rgb.r}, ${rgb.g}, ${rgb.b}, ${opacity / 100})`);
    swatch.style.setProperty("--swatch-solid", hex);
  }

  function updateColorFromPalettePointer(event) {
    if (!drafts.color) return;
    const palette = $("#colorSvPalette");
    const rect = palette.getBoundingClientRect();
    const saturation = clamp((event.clientX - rect.left) / Math.max(1, rect.width), 0, 1);
    const value = 1 - clamp((event.clientY - rect.top) / Math.max(1, rect.height), 0, 1);
    setColorDraftFromHsv(drafts.color.h, saturation, value);
  }

  function renderColorEditor() {
    const entry = selectedColorEntry();
    if (!entry) return;
    if (!drafts.color || drafts.color.path !== entry.id) {
      const hex = colorToHex(entry.color);
      const rgb = hexToColor(hex);
      const hsv = rgbToHsv(rgb.r, rgb.g, rgb.b);
      drafts.color = {
        path: entry.id,
        hex,
        opacity: Math.round((entry.color.a ?? 255) / 255 * 100),
        h: hsv.h,
        s: hsv.s,
        v: hsv.v
      };
    }
    $("#selectedColorPath").textContent = entry.name;
    syncColorEditorControls();
  }
  function applyColorDraft({ render = true } = {}) {
    const entry = selectedColorEntry();
    if (!entry || !drafts.color) return;
    const hadAlpha = Object.prototype.hasOwnProperty.call(entry.color, "a");
    const next = hexToColor(drafts.color.hex, drafts.color.opacity / 100 * 255);
    if (!hadAlpha && Number(drafts.color.opacity) === 100) delete next.a;
    setAtPath(activeProfile(), entry.path, next);
    clearUnappliedEditorSection($("#colorHex"));
    markDirty(`${entry.name} updated`, ["profiles"]);
    if (render) renderColors();
  }

  async function copyProfileColor() {
    stageEditorControl(document.activeElement);
    const entry = selectedColorEntry();
    if (!entry) return;
    const alpha = Math.round(clamp(entry.color.a ?? 255, 0, 255));
    const value = `${colorToHex(entry.color).toUpperCase()}${alpha.toString(16).padStart(2, "0").toUpperCase()}`;
    await writeEditorClipboard(value, "color");
    showToast("Color copied", "success");
  }

  async function pasteProfileColor() {
    const raw = String(await readEditorClipboard("color", "Paste a color such as #4A90E2 or #4A90E280") || "").trim();
    const match = raw.match(/^#?([0-9a-f]{6})([0-9a-f]{2})?$/i);
    if (!match) {
      showToast("Paste a 6- or 8-digit hex color", "error");
      return;
    }
    const entry = selectedColorEntry();
    if (!entry) return;
    if (!drafts.color || drafts.color.path !== entry.id) renderColorEditor();
    setColorDraftFromHex(`#${match[1]}`);
    drafts.color.opacity = match[2]
      ? Math.round(parseInt(match[2], 16) / 255 * 100)
      : 100;
    syncColorEditorControls();
    applyColorDraft({ render: false });
    refreshEditorDerivedVisuals("colors");
    showToast("Color pasted", "success");
  }
