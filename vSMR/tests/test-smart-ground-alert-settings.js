"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const controls = [
  { checked: true, dataset: { alertType: "NO PUSH" } },
  { checked: false, dataset: { alertType: "NO TAXI" } },
  { checked: false, dataset: { alertType: "SG COLLISION" } },
  { checked: true, dataset: { alertType: "OFF TRACK" } }
];
const context = vm.createContext({
  state: { activeProfileId: "profile-1" },
  drafts: { alerts: { profileId: "profile-1", data: {
    visibility: "normal", runways: [], rimcas: { inactive_alerts: ["NO TAXI", "FUTURE ALERT"], timer: [60, 45, 30, 15, 0], timer_lvp: [120, 90, 60, 30, 0] }
  } } },
  clone: value => structuredClone(value),
  $: () => null,
  $$: selector => selector === "[data-alert-type]" ? controls : []
});
vm.runInContext(fs.readFileSync(path.join(__dirname, "../src/control_center/web/app-settings.js"), "utf8"), context);
let result = context.captureAlertsDraft();
assert.deepEqual(new Set(Array.from(result.rimcas.inactive_alerts)), new Set(["FUTURE ALERT", "NO TAXI", "SG COLLISION"]));
controls[2].checked = true; controls[3].checked = false;
result = context.captureAlertsDraft();
assert.deepEqual(new Set(Array.from(result.rimcas.inactive_alerts)), new Set(["FUTURE ALERT", "NO TAXI", "OFF TRACK"]));
assert.equal(result.visibility, "normal");
assert.deepEqual(Array.from(result.rimcas.timer), [60, 45, 30, 15, 0]);
console.log("Smart Ground alert settings: independent switches, existing RIMCAS and unlisted IDs preserved.");
