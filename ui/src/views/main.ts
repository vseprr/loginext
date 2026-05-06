import { card } from "../components/card";
import { segmented } from "../components/segmented";
import { toggle } from "../components/toggle";
import { ipc, type SettingsResponse, type DevicesResponse,
         type ControlsResponse, type PresetsResponse,
         type Preset, type AppRule } from "../ipc/client";
import { rulesCard, getRules, getPresets, onRulesChange,
         addRule as rulesAddRule, updateRule as rulesUpdateRule,
         deleteRule as rulesDeleteRule, getActiveApp } from "./rules";

type Mode = "low" | "medium" | "high";

// Current state
let currentMode: Mode | null = null;
let currentInvert: boolean | null = null;
let currentActivePreset: string | null = null;
let applying = false;
let lastAppliedMode: Mode | null = null;
let lastAppliedInvert: boolean | null = null;
let lastAppliedPreset: string | null = null;

// Context: "global" or a specific app name
type Context = { type: "global" } | { type: "app"; app: string };
let currentContext: Context = { type: "global" };

// ── SVG icons (inline, no external deps) ──────────────────────────

const mouseSvg = `<svg viewBox="0 0 24 24"><path d="M12 2C8.13 2 5 5.13 5 9v6c0 3.87 3.13 7 7 7s7-3.13 7-7V9c0-3.87-3.13-7-7-7z"/><line x1="12" y1="2" x2="12" y2="10"/></svg>`;

const wheelSvg = `<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="3"/><path d="M12 5v2M12 17v2M5 12h2M17 12h2"/><circle cx="12" cy="12" r="9" stroke-dasharray="4 3"/></svg>`;

const tabSvg = `<svg viewBox="0 0 24 24"><rect x="3" y="3" width="18" height="18" rx="3"/><line x1="3" y1="9" x2="21" y2="9"/><line x1="9" y1="3" x2="9" y2="9"/></svg>`;

const zoomSvg = `<svg viewBox="0 0 24 24"><circle cx="11" cy="11" r="7"/><line x1="16.5" y1="16.5" x2="21" y2="21"/><line x1="8" y1="11" x2="14" y2="11"/><line x1="11" y1="8" x2="11" y2="14"/></svg>`;

const globalSvg = `<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="10"/><path d="M2 12h20M12 2a15.3 15.3 0 0 1 4 10 15.3 15.3 0 0 1-4 10 15.3 15.3 0 0 1-4-10 15.3 15.3 0 0 1 4-10z"/></svg>`;

const appSvg = `<svg viewBox="0 0 24 24"><rect x="3" y="3" width="7" height="7" rx="1.5"/><rect x="14" y="3" width="7" height="7" rx="1.5"/><rect x="3" y="14" width="7" height="7" rx="1.5"/><rect x="14" y="14" width="7" height="7" rx="1.5"/></svg>`;

function presetIcon(presetId: string): string {
  if (presetId === "zoom") return zoomSvg;
  return tabSvg;
}

// ── Fatal-state overlay ───────────────────────────────────────────

let overlayEl: HTMLElement | null = null;

function showFatalOverlay(title: string, detail: string) {
  if (!overlayEl) {
    overlayEl = document.createElement("div");
    overlayEl.className = "daemon-overlay";
    overlayEl.setAttribute("role", "alert");
    document.body.appendChild(overlayEl);
  }
  overlayEl.innerHTML = "";

  const card = document.createElement("div");
  card.className = "daemon-overlay__card";

  const h = document.createElement("div");
  h.className = "daemon-overlay__title";
  h.textContent = title;

  const p = document.createElement("div");
  p.className = "daemon-overlay__detail";
  p.textContent = detail;

  const hint = document.createElement("div");
  hint.className = "daemon-overlay__hint";
  hint.textContent =
    "Try: ./deploy/install.sh, then relaunch. The UI will retry automatically.";

  card.appendChild(h);
  card.appendChild(p);
  card.appendChild(hint);
  overlayEl.appendChild(card);
  overlayEl.classList.add("daemon-overlay--visible");
}

function hideFatalOverlay() {
  if (overlayEl) overlayEl.classList.remove("daemon-overlay--visible");
}

function applyDaemonStatus(status: { ok: boolean; state: string; err?: string }) {
  if (status.ok) {
    hideFatalOverlay();
    return;
  }
  switch (status.state) {
    case "binary_not_found":
      showFatalOverlay(
        "Daemon binary not found",
        status.err ?? "loginext is not installed where the UI can find it.",
      );
      break;
    case "spawn_failed":
      showFatalOverlay(
        "Daemon failed to start",
        status.err ?? "spawn() returned an error — check permissions on /dev/uinput and /dev/input.",
      );
      break;
    case "timeout":
      showFatalOverlay(
        "Daemon did not come up",
        status.err ?? "The daemon was launched but never opened its socket.",
      );
      break;
    default:
      hideFatalOverlay();
  }
}

// ── Toast system ──────────────────────────────────────────────────

let toastEl: HTMLElement | null = null;
let toastTimer: ReturnType<typeof setTimeout> | null = null;

function showToast(message: string, type: "success" | "error" = "success") {
  if (!toastEl) {
    toastEl = document.createElement("div");
    toastEl.className = "toast";
    document.body.appendChild(toastEl);
  }
  toastEl.textContent = message;
  toastEl.className = `toast toast--${type} toast--visible`;
  if (toastTimer) clearTimeout(toastTimer);
  toastTimer = setTimeout(() => {
    toastEl!.classList.remove("toast--visible");
  }, 2200);
}

// ── Apply settings (write config → reload daemon) ─────────────────

async function applyCurrentSettings() {
  if (applying) return;
  const mode   = currentMode   ?? "medium";
  const invert = currentInvert ?? true;
  const preset = currentActivePreset ?? "tab_nav";
  if (mode === lastAppliedMode && invert === lastAppliedInvert && preset === lastAppliedPreset) return;
  applying = true;
  try {
    const result = await ipc.applySettings(mode, invert, preset);
    if (result.ok) {
      currentMode       = mode;
      currentInvert     = invert;
      currentActivePreset = preset;
      lastAppliedMode   = mode;
      lastAppliedInvert = invert;
      lastAppliedPreset = preset;
      showToast("Settings applied ✓", "success");
    } else {
      showToast(`Reload failed: ${result.err}`, "error");
    }
  } catch (e) {
    showToast(`Error: ${String(e)}`, "error");
  } finally {
    applying = false;
  }
}

// ── Main render ───────────────────────────────────────────────────

export function renderMain(root: HTMLElement): void {
  root.className = "app";
  root.appendChild(deviceColumn());
  root.appendChild(controlColumn());
  root.appendChild(presetColumn());

  void fetchInitialState();
}

async function fetchInitialState() {
  try {
    const res = await ipc.getSettings();
    if (res.ok) {
      const s = res as SettingsResponse;
      currentMode = s.mode;
      currentInvert = s.invert_hwheel;
      currentActivePreset = s.active_preset;
      lastAppliedMode = currentMode;
      lastAppliedInvert = currentInvert;
      lastAppliedPreset = currentActivePreset;
      syncSegmented(currentMode);
      syncToggle(currentInvert);
      updatePresetHeader();
      refreshPresetListHighlight();
    }
  } catch {
    // Daemon may not be running
  }
}

// ── Device column (left) ──────────────────────────────────────────

function deviceColumn(): HTMLElement {
  const col = document.createElement("div");
  col.className = "app__col";
  col.id = "col-devices";

  const c = card({ title: "Devices" });
  const list = document.createElement("div");
  list.id = "device-list";

  const skel = document.createElement("div");
  skel.className = "skeleton";
  skel.style.width = "80%";
  skel.style.height = "40px";
  skel.style.margin = "8px 0";
  list.appendChild(skel);
  c.appendChild(list);
  col.appendChild(c);

  void (async () => {
    try {
      const res = await ipc.listDevices();
      list.innerHTML = "";
      if (res.ok) {
        const data = res as DevicesResponse;
        for (const dev of data.devices) {
          list.appendChild(deviceItem(dev.name, dev.id, true));
        }
      }
    } catch {
      list.innerHTML = "";
      list.appendChild(deviceItem("MX Master 3S", "mx_master_3s", true));
    }
  })();

  return col;
}

function deviceItem(name: string, id: string, selected: boolean): HTMLElement {
  const el = document.createElement("div");
  el.className = "device-item";
  el.setAttribute("aria-selected", String(selected));
  el.id = `device-${id}`;

  const icon = document.createElement("div");
  icon.className = `icon-circle${selected ? " icon-circle--active" : ""}`;
  icon.innerHTML = mouseSvg;

  const info = document.createElement("div");
  info.className = "device-item__info";

  const nameEl = document.createElement("div");
  nameEl.className = "device-item__name";
  nameEl.textContent = name;

  const idEl = document.createElement("div");
  idEl.className = "device-item__id";
  idEl.textContent = id;

  info.appendChild(nameEl);
  info.appendChild(idEl);
  el.appendChild(icon);
  el.appendChild(info);
  return el;
}

// ── Control column (middle) ───────────────────────────────────────

let contextBarEl: HTMLElement | null = null;
let presetListEl: HTMLElement | null = null;

function controlColumn(): HTMLElement {
  const col = document.createElement("div");
  col.className = "app__col";
  col.id = "col-controls";

  const c = card({ title: "Controls" });
  const list = document.createElement("div");
  list.id = "control-list";
  list.style.display = "flex";
  list.style.flexDirection = "column";
  list.style.gap = "12px";

  c.appendChild(list);
  col.appendChild(c);

  void (async () => {
    try {
      const res = await ipc.listControls();
      list.innerHTML = "";
      if (res.ok) {
        const data = res as ControlsResponse;
        for (const ctrl of data.controls) {
          list.appendChild(controlItem(ctrl.name, ctrl.kind, true));
        }
      }
    } catch {
      list.innerHTML = "";
      list.appendChild(controlItem("Thumb Wheel", "Horizontal wheel", true));
    }
    // After controls render, create the context bar
    contextBarEl = document.createElement("div");
    contextBarEl.className = "context-selector";
    contextBarEl.id = "context-selector";
    list.appendChild(contextBarEl);
    renderContextBar();
  })();

  // Preset list card
  const presetCard = card({ title: "Available Presets" });
  presetListEl = document.createElement("div");
  presetListEl.id = "preset-list";

  presetCard.appendChild(presetListEl);
  col.appendChild(presetCard);

  void renderPresetList();

  // Per-app rules card (keeps "Currently focused" + "+ Add rule")
  col.appendChild(rulesCard(showToast));

  // Listen for rule mutations from the rules module
  onRulesChange((_rules: AppRule[]) => {
    renderContextBar();
    refreshPresetListHighlight();
  });

  return col;
}

// ── Context selector bar ─────────────────────────────────────────

function renderContextBar(): void {
  if (!contextBarEl) return;
  contextBarEl.innerHTML = "";

  const rules = getRules();

  // Global button
  const globalBtn = document.createElement("button");
  globalBtn.type = "button";
  globalBtn.className = "context-btn" +
    (currentContext.type === "global" ? " context-btn--active" : "");
  globalBtn.id = "ctx-global";
  globalBtn.title = "Global (all apps)";
  const globalIcon = document.createElement("span");
  globalIcon.className = "context-btn__icon";
  globalIcon.innerHTML = globalSvg;
  const globalLabel = document.createElement("span");
  globalLabel.className = "context-btn__label";
  globalLabel.textContent = "Global";
  globalBtn.appendChild(globalIcon);
  globalBtn.appendChild(globalLabel);
  globalBtn.addEventListener("click", () => {
    currentContext = { type: "global" };
    renderContextBar();
    refreshPresetListHighlight();
    updatePresetHeader();
  });
  contextBarEl.appendChild(globalBtn);

  // Per-app buttons for each rule
  for (const r of rules) {
    const appBtn = document.createElement("button");
    appBtn.type = "button";
    appBtn.className = "context-btn" +
      (currentContext.type === "app" && currentContext.app === r.app ? " context-btn--active" : "");
    appBtn.title = r.app;
    const appIcon = document.createElement("span");
    appIcon.className = "context-btn__icon";
    appIcon.innerHTML = appSvg;
    const appLabel = document.createElement("span");
    appLabel.className = "context-btn__label";
    appLabel.textContent = r.app;
    appBtn.appendChild(appIcon);
    appBtn.appendChild(appLabel);
    appBtn.addEventListener("click", () => {
      currentContext = { type: "app", app: r.app };
      renderContextBar();
      refreshPresetListHighlight();
      updatePresetHeader();
    });
    contextBarEl.appendChild(appBtn);
  }
}

// ── Preset list ──────────────────────────────────────────────────

async function renderPresetList(): Promise<void> {
  if (!presetListEl) return;
  let presets: Preset[] = [];
  try {
    const res = await ipc.listPresets();
    if (res.ok) {
      const data = res as PresetsResponse;
      presets = data.presets;
    }
  } catch {
    presets = [{ id: "tab_nav", name: "Navigate between tabs" }];
  }

  presetListEl.innerHTML = "";
  for (const p of presets) {
    presetListEl.appendChild(presetListItem(p));
  }
  refreshPresetListHighlight();
}

function getActivePresetForContext(): string | null {
  if (currentContext.type === "global") {
    return currentActivePreset;
  }
  const ctx = currentContext;
  const rules = getRules();
  const rule = rules.find(
    (r) => r.app.toLowerCase() === ctx.app.toLowerCase()
  );
  return rule ? rule.preset : null;
}

function refreshPresetListHighlight(): void {
  if (!presetListEl) return;
  const activeId = getActivePresetForContext();
  for (const child of Array.from(presetListEl.children)) {
    const el = child as HTMLElement;
    const id = el.dataset.presetId;
    const isActive = id != null && activeId != null && id === activeId;
    el.setAttribute("aria-selected", String(isActive));
    const icon = el.querySelector(".icon-circle");
    if (icon) {
      if (isActive) icon.classList.add("icon-circle--active");
      else icon.classList.remove("icon-circle--active");
    }
  }
}

function presetListItem(p: Preset): HTMLElement {
  const item = document.createElement("div");
  item.className = "list-item";
  item.setAttribute("aria-selected", "false");
  item.id = `preset-${p.id}`;
  item.dataset.presetId = p.id;

  const icon = document.createElement("div");
  icon.className = "icon-circle icon-circle--sm";
  icon.innerHTML = presetIcon(p.id);

  const label = document.createElement("span");
  label.textContent = p.name;

  item.appendChild(icon);
  item.appendChild(label);

  // Click handler: bind/unbind
  item.addEventListener("click", () => {
    void handlePresetClick(p);
  });

  return item;
}

async function handlePresetClick(p: Preset): Promise<void> {
  const activeId = getActivePresetForContext();

  if (currentContext.type === "global") {
    if (activeId === p.id) {
      // Deselect: unbind global → passthrough (none)
      currentActivePreset = "none";
      void applyCurrentSettings();
    } else {
      // Select: bind global to this preset
      currentActivePreset = p.id;
      void applyCurrentSettings();
    }
    refreshPresetListHighlight();
    updatePresetHeader();
  } else {
    // App context
    const appName = currentContext.app;
    if (activeId === p.id) {
      // Deselect: remove the per-app rule entirely
      rulesDeleteRule(appName);
      // If no rules left for this app, switch back to global context
      const remaining = getRules().find(
        (r) => r.app.toLowerCase() === appName.toLowerCase()
      );
      if (!remaining) {
        currentContext = { type: "global" };
        renderContextBar();
      }
    } else {
      // Select: create or update per-app rule
      const existing = getRules().find(
        (r) => r.app.toLowerCase() === appName.toLowerCase()
      );
      if (existing) {
        rulesUpdateRule(appName, p.id);
      } else {
        rulesAddRule(appName, p.id);
      }
    }
    refreshPresetListHighlight();
    updatePresetHeader();
  }
}

function controlItem(name: string, kind: string, selected: boolean): HTMLElement {
  const el = document.createElement("div");
  el.className = "control-item";
  el.setAttribute("aria-selected", String(selected));

  const icon = document.createElement("div");
  icon.className = `icon-circle${selected ? " icon-circle--active" : ""}`;
  icon.innerHTML = wheelSvg;

  const info = document.createElement("div");
  info.className = "control-item__info";

  const nameEl = document.createElement("div");
  nameEl.className = "control-item__name";
  nameEl.textContent = name;

  const kindEl = document.createElement("div");
  kindEl.className = "control-item__kind";
  kindEl.textContent = kind;

  info.appendChild(nameEl);
  info.appendChild(kindEl);
  el.appendChild(icon);
  el.appendChild(info);
  return el;
}

// ── Preset column (right) — settings ──────────────────────────────

let segmentedEl: HTMLElement | null = null;
let toggleEl: HTMLElement | null = null;
let presetHeaderNameEl: HTMLElement | null = null;
let presetHeaderIconEl: HTMLElement | null = null;

function syncSegmented(mode: Mode) {
  if (!segmentedEl) return;
  for (const child of Array.from(segmentedEl.children)) {
    const btn = child as HTMLElement;
    btn.setAttribute("aria-selected", String(btn.dataset.value === mode));
  }
}

function syncToggle(checked: boolean) {
  if (!toggleEl) return;
  toggleEl.setAttribute("aria-checked", String(checked));
}

function updatePresetHeader(): void {
  if (!presetHeaderNameEl) return;
  const activeId = getActivePresetForContext();
  if (!activeId || activeId === "none") {
    presetHeaderNameEl.textContent = "No preset (passthrough)";
    if (presetHeaderIconEl) {
      presetHeaderIconEl.classList.remove("icon-circle--active");
    }
    return;
  }
  const presets = getPresets();
  const match = presets.find((p) => p.id === activeId);
  presetHeaderNameEl.textContent = match ? match.name : activeId;
  if (presetHeaderIconEl) {
    presetHeaderIconEl.classList.add("icon-circle--active");
    presetHeaderIconEl.innerHTML = presetIcon(activeId);
  }
}

function presetColumn(): HTMLElement {
  const col = document.createElement("div");
  col.className = "app__col";
  col.id = "col-presets";

  const c = card({});

  // Preset header
  const header = document.createElement("div");
  header.className = "preset-header";

  const headerIcon = document.createElement("div");
  headerIcon.className = "icon-circle icon-circle--sm icon-circle--active";
  headerIcon.innerHTML = tabSvg;
  presetHeaderIconEl = headerIcon;

  const headerName = document.createElement("div");
  headerName.className = "preset-header__name";
  headerName.textContent = "Navigate between tabs";
  presetHeaderNameEl = headerName;

  header.appendChild(headerIcon);
  header.appendChild(headerName);
  c.appendChild(header);

  // Sensitivity
  const sensLabel = document.createElement("div");
  sensLabel.className = "section-label";
  sensLabel.textContent = "Sensitivity";

  segmentedEl = segmented<Mode>({
    options: [
      { value: "low",    label: "Low" },
      { value: "medium", label: "Medium" },
      { value: "high",   label: "High" },
    ],
    onChange: (m) => {
      currentMode = m;
      void applyCurrentSettings();
    },
  });

  c.appendChild(sensLabel);
  c.appendChild(segmentedEl);

  // Invert axis
  const invLabel = document.createElement("div");
  invLabel.className = "section-label";
  invLabel.textContent = "Invert axis";

  const invRow = document.createElement("div");
  invRow.className = "setting-row";

  const invDesc = document.createElement("div");
  invDesc.className = "setting-row__label";
  invDesc.textContent = "Reverse scroll direction";

  toggleEl = toggle({
    checked: false,
    onChange: (v) => {
      currentInvert = v;
      void applyCurrentSettings();
    },
  });

  invRow.appendChild(invDesc);
  invRow.appendChild(toggleEl);

  c.appendChild(invLabel);
  c.appendChild(invRow);

  col.appendChild(c);
  return col;
}

// ── Heartbeat + status toggle ─────────────────────────────────────

const HEARTBEAT_OK_MS    = 5_000;
const HEARTBEAT_MIN_FAIL = 2_000;
const HEARTBEAT_MAX_FAIL = 30_000;
const HEARTBEAT_OFF_MS   = 10_000;
const FORCED_OFF_KEY     = "daemon_forced_off";

function isForcedOff(): boolean {
  try { return localStorage.getItem(FORCED_OFF_KEY) === "true"; }
  catch { return false; }
}

function setForcedOff(value: boolean): void {
  try {
    if (value) localStorage.setItem(FORCED_OFF_KEY, "true");
    else localStorage.removeItem(FORCED_OFF_KEY);
  } catch { /* private mode / disabled storage */ }
}

type ButtonState = "online" | "offline" | "pending";

interface StatusButton {
  el: HTMLButtonElement;
  set: (s: ButtonState) => void;
}

function createStatusButton(): StatusButton {
  const btn = document.createElement("button");
  btn.type = "button";
  btn.className = "status-toggle status-toggle--pending";
  btn.setAttribute("aria-pressed", "false");

  const dot = document.createElement("span");
  dot.className = "status-toggle__dot";
  const label = document.createElement("span");
  label.className = "status-toggle__label";
  label.textContent = "CONNECTING…";

  btn.appendChild(dot);
  btn.appendChild(label);

  const set = (s: ButtonState) => {
    btn.classList.remove(
      "status-toggle--online",
      "status-toggle--offline",
      "status-toggle--pending",
    );
    btn.classList.add(`status-toggle--${s}`);
    if (s === "online")  { label.textContent = "DAEMON ONLINE";  btn.setAttribute("aria-pressed", "true");  }
    if (s === "offline") { label.textContent = "DAEMON OFFLINE"; btn.setAttribute("aria-pressed", "false"); }
    if (s === "pending") { label.textContent = "CONNECTING…";    btn.setAttribute("aria-pressed", "false"); }
  };

  return { el: btn, set };
}

export async function attachHeartbeat(bar: HTMLElement): Promise<void> {
  bar.className = "status-bar";

  const status = createStatusButton();
  bar.appendChild(status.el);

  let failures = 0;
  let timer: ReturnType<typeof setTimeout> | null = null;
  let busy = false;

  const schedule = (ms: number) => {
    if (timer) clearTimeout(timer);
    timer = setTimeout(beat, ms);
  };

  if (isForcedOff()) {
    status.set("offline");
    try { await ipc.daemonKill(); } catch { /* best-effort */ }
  }

  status.el.addEventListener("click", async () => {
    if (busy) return;
    busy = true;
    if (timer) { clearTimeout(timer); timer = null; }

    const goingOffline = !isForcedOff();
    status.set("pending");

    try {
      if (goingOffline) {
        const r = await ipc.daemonKill();
        if (r.ok) {
          setForcedOff(true);
          status.set("offline");
          showToast("Daemon stopped", "success");
          failures = 0;
          schedule(HEARTBEAT_OFF_MS);
        } else {
          status.set("online");
          showToast(`Stop failed: ${r.err ?? r.state}`, "error");
          schedule(HEARTBEAT_OK_MS);
        }
      } else {
        setForcedOff(false);
        const r = await ipc.daemonRespawn();
        applyDaemonStatus(r);
        if (r.ok) {
          status.set("online");
          showToast("Daemon started", "success");
          failures = 0;
          schedule(HEARTBEAT_OK_MS);
        } else {
          setForcedOff(true);
          status.set("offline");
          showToast(`Start failed: ${r.err ?? r.state}`, "error");
          schedule(HEARTBEAT_OFF_MS);
        }
      }
    } catch (e) {
      showToast(`Toggle error: ${String(e)}`, "error");
      schedule(HEARTBEAT_MIN_FAIL);
    } finally {
      busy = false;
    }
  });

  if (!isForcedOff()) {
    try {
      const s = await ipc.daemonStatus();
      if (!s.ok) applyDaemonStatus(s);
    } catch { /* heartbeat will own it */ }
  }

  async function beat(): Promise<void> {
    if (busy) { schedule(HEARTBEAT_MIN_FAIL); return; }

    if (isForcedOff()) {
      try {
        const r = await ipc.ping();
        status.set(r.ok ? "online" : "offline");
      } catch {
        status.set("offline");
      }
      schedule(HEARTBEAT_OFF_MS);
      return;
    }

    try {
      const r = await ipc.ping();
      if (r.ok) {
        failures = 0;
        status.set("online");
        hideFatalOverlay();
        schedule(HEARTBEAT_OK_MS);
        return;
      }
      failures += 1;
      status.set("pending");
    } catch {
      failures += 1;
      status.set("pending");
    }

    if (failures <= 3) {
      try {
        const respawn = await ipc.daemonRespawn();
        applyDaemonStatus(respawn);
        if (respawn.ok) {
          failures = 0;
          status.set("online");
          schedule(HEARTBEAT_OK_MS);
          return;
        }
      } catch { /* swallow */ }
    }

    if (failures > 3) status.set("offline");

    const backoff = Math.min(HEARTBEAT_MIN_FAIL << (failures - 1), HEARTBEAT_MAX_FAIL);
    schedule(backoff);
  }

  if (!isForcedOff()) {
    await beat();
  } else {
    schedule(HEARTBEAT_OFF_MS);
  }
}
