"use strict";

  async function copyAvisoGeometry() {
    stageEditorControl(document.activeElement);
    const entries = selectedAvisoGeometryEntries();
    const entry = entries.find(item => item.id === state.ui.selectedAvisoGeometryStyleId) || entries[0];
    if (!entry) return;
    const colorKey = entry.objectType === "Line" ? "stroke" : "fill";
    const opacityKey = entry.objectType === "Line" ? "stroke-opacity" : "fill-opacity";
    const style = {
      color: normalizeHex(entry.paint[colorKey], "#000000").toUpperCase(),
      opacity: clamp(Number(entry.paint[opacityKey] ?? 1), 0, 1)
    };
    await writeEditorClipboard(JSON.stringify({ vsmr: "aviso-geometry-style", version: 1, style }, null, 2), "AVISO geometry style");
    showToast("AVISO geometry style copied", "success");
  }

  async function pasteAvisoGeometry() {
    const raw = String(await readEditorClipboard("AVISO geometry style", "Paste a vSMR AVISO geometry style") || "").trim();
    if (!raw) return;
    let parsed;
    try { parsed = JSON.parse(raw); }
    catch (error) {
      showToast("Clipboard does not contain an AVISO geometry style", "error");
      return;
    }
    const style = parsed?.style ?? parsed;
    const color = String(style?.color || "").trim();
    const opacity = Number(style?.opacity);
    if (!/^#[0-9a-f]{6}$/i.test(color) || !Number.isFinite(opacity)) {
      showToast("Clipboard does not contain a valid AVISO geometry style", "error");
      return;
    }
    const entries = selectedAvisoGeometryEntries();
    if (!entries.length) return;
    entries.forEach(entry => {
      if (entry.isBackground) {
        state.aviso.metadata.background_colors[activeAvisoColorPalette()] = color.toUpperCase();
        return;
      }
      const colorKey = entry.objectType === "Line" ? "stroke" : "fill";
      const opacityKey = entry.objectType === "Line" ? "stroke-opacity" : "fill-opacity";
      const changes = { [colorKey]: color.toUpperCase(), [opacityKey]: clamp(opacity, 0, 1) };
      applyAvisoPaintChanges(ensureAvisoCatalogStyle(entry).paint, changes);
      entry.indices.forEach(index => {
        const properties = avisoFeatures()[index]?.properties;
        if (!properties) return;
        properties.style_id ||= entry.id;
        applyAvisoPaintChanges(properties, changes);
      });
    });
    drafts.avisoGeometry = null;
    clearUnappliedEditorSection($("#avisoGeometryColorHex"));
    markDirty(`${entries.length} geometry style${entries.length === 1 ? "" : "s"} pasted`, ["aviso"]);
    renderAvisoGeometry();
    showToast(`Geometry style pasted to ${entries.length} selection${entries.length === 1 ? "" : "s"}`, "success");
  }

  async function copyAvisoText() {
    stageEditorControl(document.activeElement);
    const entries = selectedAvisoTextEntries();
    const entry = entries.find(item => item.id === state.ui.selectedAvisoTextStyleId) || entries[0];
    if (!entry) return;
    const index = entry.indices[0];
    const read = key => index == null ? (entry.paint[key] ?? AVISO_TEXT_DEFAULTS[key]) : effectiveAvisoTextValue(index, entry, key);
    const style = Object.fromEntries(AVISO_TEXT_PAINT_KEYS.map(key => [key, clone(read(key))]));
    await writeEditorClipboard(JSON.stringify({ vsmr: "aviso-text-style", version: 1, style }, null, 2), "AVISO text style");
    showToast("AVISO text style copied", "success");
  }

  async function pasteAvisoText() {
    const raw = String(await readEditorClipboard("AVISO text style", "Paste a vSMR AVISO text style") || "").trim();
    if (!raw) return;
    let parsed;
    try { parsed = JSON.parse(raw); }
    catch (error) {
      showToast("Clipboard does not contain an AVISO text style", "error");
      return;
    }
    const source = parsed?.style ?? parsed;
    const textSize = Number(source?.["text-size"]);
    const haloWidth = Number(source?.["text-halo-width"]);
    const zoomLevel = Number(source?.zoomLevel);
    if (!source || typeof source !== "object" ||
        !/^#[0-9a-f]{6}$/i.test(String(source["text-color"] || "")) ||
        !/^#[0-9a-f]{6}$/i.test(String(source["text-halo-color"] || "")) ||
        ![textSize, haloWidth, zoomLevel].every(Number.isFinite)) {
      showToast("Clipboard does not contain a valid AVISO text style", "error");
      return;
    }
    const style = {
      "text-font": String(source["text-font"] || "Arial").trim().slice(0, 100) || "Arial",
      "text-size": clamp(textSize, 6, 32),
      "text-color": normalizeHex(source["text-color"], "#808080").toUpperCase(),
      "text-anchor": ["start", "center", "end"].includes(source["text-anchor"]) ? source["text-anchor"] : "center",
      "text-halo-color": normalizeHex(source["text-halo-color"], "#000000").toUpperCase(),
      "text-halo-width": clamp(haloWidth, 0, 6),
      zoomLevel: Math.round(clamp(zoomLevel, 0, 14))
    };
    const entries = selectedAvisoTextEntries();
    if (!entries.length) return;
    entries.forEach(entry => {
      applyAvisoPaintChanges(ensureAvisoCatalogStyle(entry).paint, style);
      entry.indices.forEach(index => {
        const properties = avisoFeatures()[index]?.properties;
        if (!properties) return;
        properties.style_id ||= entry.id;
        applyAvisoPaintChanges(properties, style);
      });
    });
    drafts.avisoTextStyle = null;
    clearUnappliedEditorSection($("#avisoTextFont"));
    markDirty(`${entries.length} text style${entries.length === 1 ? "" : "s"} pasted`, ["aviso"]);
    renderAvisoText();
    showToast(`Text style pasted to ${entries.length} selection${entries.length === 1 ? "" : "s"}`, "success");
  }
