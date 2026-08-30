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
    api.receive({
      version: 1,
      id: "browser-state-1",
      type: "state.authoritative",
      payload: authoritative
    });
    expect(api.getState().airport === "TEST", "native authoritative state is applied");

    const displayButton = document.querySelector('.rail-button[data-page="display"]');
    displayButton?.click();
    expect(displayButton?.classList.contains("active"), "page navigation event is bound");

    const outbound = [];
    window.addEventListener("vsmr-control-center", event => outbound.push(event.detail));
    document.querySelector('.rail-button[data-page="profiles"]')?.click();
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
    const newGroupButton = document.querySelector('[data-action="new-aviso-group"]');
    expect(Boolean(newGroupButton), "AVISO group action is rendered");
    newGroupButton?.click();
    await waitFor(
      () => outbound.some(message => message.type === "state.save" &&
        message.payload?.aviso?.type === "FeatureCollection"),
      "AVISO edit emits a state.save request"
    );
  } catch (error) {
    failures.push(`unexpected browser exception: ${error?.stack || error}`);
  }

  result.dataset.status = failures.length ? "failed" : "passed";
  result.textContent = failures.length ? failures.join("\n") : "Control Center browser tests passed";
  document.documentElement.dataset.vsmrBrowserTests = result.dataset.status;
})();
