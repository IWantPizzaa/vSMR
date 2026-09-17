"use strict";

  const INTERACTION_SELECTOR = [
    "button", "[role='button']", "input:not([type='hidden'])", "select", "textarea", "[role='slider']"
  ].join(",");

  function interactionLabel(element) {
    const label = element.closest("label")?.querySelector(":scope > span:first-child")?.textContent?.trim();
    return label || element.getAttribute("aria-label") || element.dataset.tooltipText ||
      element.getAttribute("title") || element.textContent?.trim().replace(/\s+/g, " ") || "control";
  }

  function interactionHelpText(element) {
    const label = interactionLabel(element);
    if (element.dataset.page) return `Open the ${humanize(element.dataset.page)} page`;
    if (element.dataset.profileTab) return `Open the ${humanize(element.dataset.profileTab)} editor`;
    if (element.dataset.avisoView) return `Show AVISO ${humanize(element.dataset.avisoView)} styles`;
    if (element.dataset.treeToggle) return `Expand or collapse ${label}`;
    if (element.matches("[data-color-path], [data-tag-id], [data-aviso-geometry-style], [data-aviso-text-style]"))
      return `Select ${label}. Hold Ctrl to toggle items or Shift to select a range`;

    const action = String(element.dataset.action || "");
    if (action) {
      const subject = humanize(action.replace(/^(copy|paste|new|delete|duplicate|rename|move-up|move-down|open|close|add|remove|restore|reset|save|select|update|dismiss|activate)-?/, "")) || "item";
      if (action.startsWith("copy-")) return `Copy ${subject} to the clipboard`;
      if (action.startsWith("paste-")) return `Paste ${subject} from the clipboard`;
      if (action.startsWith("new-") || action.startsWith("add-")) return `Add ${subject}`;
      if (action.startsWith("delete-") || action.startsWith("remove-")) return `Delete ${subject}`;
      if (action.startsWith("duplicate-")) return `Duplicate ${subject}`;
      if (action.startsWith("move-up-")) return `Move ${subject} up`;
      if (action.startsWith("move-down-")) return `Move ${subject} down`;
      if (action.startsWith("open-")) return `Open ${subject}`;
      if (action.startsWith("close-")) return `Close ${subject}`;
      if (action.startsWith("restore-") || action.startsWith("reset-")) return `Restore ${subject}`;
      if (action.startsWith("save-")) return `Save ${subject}`;
      if (action.startsWith("select-") || action.startsWith("set-")) return `Select ${subject}`;
      if (element.dataset.tooltipText || element.title) return label;
      return humanize(action);
    }

    if (element instanceof HTMLInputElement) {
      if (element.type === "checkbox") return `Toggle ${label}`;
      if (element.type === "range") return `Adjust ${label}`;
      if (element.type === "color") return `Choose ${label}`;
      if (element.type === "file") return `Choose a file for ${label}`;
      return `Edit ${label}`;
    }
    if (element instanceof HTMLSelectElement) return `Choose ${label}`;
    if (element instanceof HTMLTextAreaElement) return `Edit ${label}`;
    return label;
  }

  function initializeInteractionHelp() {
    const tooltip = document.createElement("div");
    tooltip.id = "interactionTooltip";
    tooltip.className = "ui-interaction-tooltip";
    tooltip.setAttribute("role", "tooltip");
    tooltip.hidden = true;
    document.body.appendChild(tooltip);
    let timer = 0;
    let current = null;

    const hide = () => {
      window.clearTimeout(timer);
      timer = 0;
      current?.removeAttribute("aria-describedby");
      current = null;
      tooltip.classList.remove("visible");
      tooltip.hidden = true;
    };
    const show = element => {
      if (!element || !document.body.contains(element)) return;
      const text = interactionHelpText(element);
      if (!text) return;
      current = element;
      if (element.title) {
        element.dataset.tooltipText = element.title;
        element.removeAttribute("title");
      }
      tooltip.textContent = text;
      tooltip.hidden = false;
      tooltip.classList.add("visible");
      element.setAttribute("aria-describedby", tooltip.id);
      const target = element.getBoundingClientRect();
      const box = tooltip.getBoundingClientRect();
      const left = Math.max(6, Math.min(window.innerWidth - box.width - 6, target.left + (target.width - box.width) / 2));
      const below = target.bottom + 7;
      const top = below + box.height <= window.innerHeight - 6 ? below : Math.max(6, target.top - box.height - 7);
      tooltip.style.left = `${Math.round(left)}px`;
      tooltip.style.top = `${Math.round(top)}px`;
    };
    const schedule = (element, delay) => {
      hide();
      timer = window.setTimeout(() => show(element), delay);
    };

    document.addEventListener("pointerover", event => {
      if (event.pointerType === "touch") return;
      const element = event.target.closest?.(INTERACTION_SELECTOR);
      if (!element || element.contains(event.relatedTarget)) return;
      schedule(element, 550);
    });
    document.addEventListener("pointerout", event => {
      if (!current && !timer) return;
      const element = event.target.closest?.(INTERACTION_SELECTOR);
      if (element && !element.contains(event.relatedTarget)) hide();
    });
    document.addEventListener("focusin", event => {
      const element = event.target.closest?.(INTERACTION_SELECTOR);
      if (element) schedule(element, 350);
    });
    document.addEventListener("focusout", hide);
    document.addEventListener("keydown", event => { if (event.key === "Escape") hide(); });
    window.addEventListener("resize", hide);
  }
