import { card } from "../components/card";
import { segmented } from "../components/segmented";
import { toggle } from "../components/toggle";
import { ipc, type SettingsResponse, type DevicesResponse,
         type ControlsResponse, type PresetsResponse,
         type Preset, type AppRule } from "../ipc/client";
import { rulesCard, getRules, getPresets, onRulesChange,
         addRule as rulesAddRule, updateRule as rulesUpdateRule,
         deleteRule as rulesDeleteRule, unbindRule as rulesUnbindRule,
         setRuleMode as rulesSetMode, setRuleInvert as rulesSetInvert,
         getActiveApp } from "./rules";

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
  // Hard guard: this function writes settings.json and triggers a
  // daemon reload of the *global* config. It must never run while an
  // app context is active — otherwise sensitivity/invert toggled for
  // "code" would also clobber the global value and the per-app
  // decoupling would silently regress. Every existing call site
  // already gates on `currentContext.type === "global"`; this assert
  // is the canary that catches a future refactor that drops the
  // gate by mistake.
  if (currentContext.type !== "global") {
    // eslint-disable-next-line no-console
    console.warn(
      "[loginext-ui] applyCurrentSettings called from app context",
      currentContext.app,
      "— this is a bug; per-app overrides must go through rulesSetMode/rulesSetInvert.",
    );
    return;
  }
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

  // Listen for rule mutations from the rules module. Anything that
  // changes the rules (add/remove/unbind, mode/invert override) needs to
  // refresh the chip bar, the preset highlight, AND the right panel —
  // otherwise the segmented/toggle could go stale when the user edits a
  // chip's overrides via the same chip context they're already on.
  onRulesChange((_rules: AppRule[]) => {
    renderContextBar();
    refreshPresetListHighlight();
    updatePresetHeader();
    syncRightPanelToContext();
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
    syncRightPanelToContext();
  });
  contextBarEl.appendChild(globalBtn);

  // Per-app buttons for each rule. The chip stays in the bar even if its
  // preset binding has been cleared — the user explicitly removes it via
  // the X button in the corner. Chips with empty `preset` render dimmer
  // ("inactive") so the visual hierarchy still reflects which apps have
  // a daemon-side rule and which are tracked-only context entries.
  for (const r of rules) {
    const isActive = currentContext.type === "app" && currentContext.app === r.app;
    const isInactive = !r.preset;
    const appBtn = document.createElement("button");
    appBtn.type = "button";
    appBtn.className =
      "context-btn" +
      (isActive ? " context-btn--active" : "") +
      (isInactive ? " context-btn--inactive" : "");
    appBtn.title = r.app + (isInactive ? " (no preset bound — tracked only)" : "");
    const appIcon = document.createElement("span");
    appIcon.className = "context-btn__icon";
    appIcon.innerHTML = appSvg;
    const appLabel = document.createElement("span");
    appLabel.className = "context-btn__label";
    appLabel.textContent = r.app;

    // X button — appears on hover via CSS. Clicking it removes the chip
    // entirely (the rule is wiped from app_rules.txt + the daemon's
    // hash table on the next save+reload). Stops propagation so the
    // click doesn't also switch context to a chip we're about to delete.
    const closeBtn = document.createElement("span");
    closeBtn.className = "context-btn__close";
    closeBtn.setAttribute("role", "button");
    closeBtn.setAttribute("aria-label", `Remove ${r.app}`);
    closeBtn.title = `Remove "${r.app}"`;
    closeBtn.innerHTML =
      `<svg viewBox="0 0 12 12" aria-hidden="true">` +
      `<path d="M3 3 L9 9 M9 3 L3 9" stroke="currentColor" stroke-width="1.6" ` +
      `stroke-linecap="round"/></svg>`;
    closeBtn.addEventListener("click", (ev) => {
      ev.stopPropagation();
      const wasActive =
        currentContext.type === "app" && currentContext.app === r.app;
      rulesDeleteRule(r.app);
      if (wasActive) {
        currentContext = { type: "global" };
      }
      // No need to call renderContextBar() / refresh* here — the
      // onRulesChange subscription below already does both.
    });

    appBtn.appendChild(appIcon);
    appBtn.appendChild(appLabel);
    appBtn.appendChild(closeBtn);
    appBtn.addEventListener("click", () => {
      currentContext = { type: "app", app: r.app };
      renderContextBar();
      refreshPresetListHighlight();
      updatePresetHeader();
      syncRightPanelToContext();
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
    // App context. Deselecting clears the *preset binding* but keeps
    // the chip in the bar (preserving any per-app sensitivity / invert
    // overrides), so the user can re-bind without re-focusing the app.
    // Explicit deletion lives on the X button on each chip.
    const appName = currentContext.app;
    if (activeId === p.id) {
      rulesUnbindRule(appName);
    } else {
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

// Reflect the current context's sensitivity/invert into the right-panel
// controls. On app context with no per-app override, the global value is
// shown — but writes from these controls always target the active
// context (so a user inspecting "code" can't accidentally edit the
// global by reaching for the Sensitivity pills).
function syncRightPanelToContext(): void {
  if (currentContext.type === "global") {
    if (currentMode != null)   syncSegmented(currentMode);
    if (currentInvert != null) syncToggle(currentInvert);
    return;
  }
  // App context: read from the rule, fall back to global on inherit.
  const ctx = currentContext;
  const rule = getRules().find(
    (r) => r.app.toLowerCase() === ctx.app.toLowerCase()
  );
  const ruleMode   = rule?.mode ?? "";
  const ruleInvert = rule?.invert ?? null;
  const effectiveMode: Mode =
    ruleMode === "low" || ruleMode === "medium" || ruleMode === "high"
      ? ruleMode
      : (currentMode ?? "medium");
  const effectiveInvert =
    ruleInvert === null || ruleInvert === undefined
      ? (currentInvert ?? true)
      : ruleInvert;
  syncSegmented(effectiveMode);
  syncToggle(effectiveInvert);
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
      // Dispatch to the active context. Global writes settings.json (the
      // existing path); app context writes the rule's mode override so
      // changing sensitivity for "code" no longer also changes Global.
      if (currentContext.type === "app") {
        rulesSetMode(currentContext.app, m);
      } else {
        currentMode = m;
        void applyCurrentSettings();
      }
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
      // Same context-aware split as Sensitivity above.
      if (currentContext.type === "app") {
        rulesSetInvert(currentContext.app, v);
      } else {
        currentInvert = v;
        void applyCurrentSettings();
      }
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

// The toggle's truth source. When the unit file is present, we drive
// the daemon lifecycle entirely through `systemctl --user enable/disable
// --now` so the user's intent ("ON should mean: start now AND start at
// next login") is captured in systemd's autostart graph. When the unit
// is absent (older install / manual build), we fall back to the legacy
// spawn-detached + kill_daemon path so the toggle remains functional.
type LifecycleMode = "systemd" | "spawn";

export async function attachHeartbeat(bar: HTMLElement): Promise<void> {
  bar.className = "status-bar";

  const status = createStatusButton();
  bar.appendChild(status.el);

  let timer: ReturnType<typeof setTimeout> | null = null;
  let busy = false;
  let mode: LifecycleMode = "spawn";   // resolved on first state probe
  let failures = 0;                     // only used in spawn mode

  const schedule = (ms: number) => {
    if (timer) clearTimeout(timer);
    timer = setTimeout(beat, ms);
  };

  // ── Initial state ──────────────────────────────────────────────────
  // Source of truth on launch: systemd. Falls back to socket-probe for
  // hosts without the unit file installed.
  const initialSrv = await ipc.serviceState().catch(() => null);
  if (initialSrv?.ok && initialSrv.available) {
    mode = "systemd";
    const on = !!(initialSrv.active && initialSrv.enabled);
    status.set(on ? "online" : "offline");
    // Clear any stale localStorage flag — the systemd path is now
    // authoritative for intent, so the legacy `daemon_forced_off`
    // sentinel must not silently override it on the next launch.
    setForcedOff(false);
  } else {
    mode = "spawn";
    if (isForcedOff()) {
      status.set("offline");
      try { await ipc.daemonKill(); } catch { /* best-effort */ }
    }
  }

  // ── Click handler ──────────────────────────────────────────────────
  status.el.addEventListener("click", async () => {
    if (busy) return;
    busy = true;
    if (timer) { clearTimeout(timer); timer = null; }

    const goingOffline = status.el.getAttribute("aria-pressed") === "true";
    status.set("pending");

    try {
      if (mode === "systemd") {
        const r = goingOffline
          ? await ipc.serviceDisable()
          : await ipc.serviceEnable();
        if (r.ok) {
          const on = !!(r.active && r.enabled);
          status.set(on ? "online" : (goingOffline ? "offline" : "pending"));
          showToast(
            goingOffline ? "Daemon disabled (won't autostart)" : "Daemon enabled (autostarts at login)",
            "success",
          );
          schedule(on ? HEARTBEAT_OK_MS : HEARTBEAT_OFF_MS);
        } else {
          status.set(goingOffline ? "online" : "offline");
          showToast(`systemctl: ${r.err ?? r.state ?? "failed"}`, "error");
          schedule(HEARTBEAT_OK_MS);
        }
      } else {
        // Legacy spawn/kill path. Used when systemd is absent.
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
      }
    } catch (e) {
      showToast(`Toggle error: ${String(e)}`, "error");
      schedule(HEARTBEAT_MIN_FAIL);
    } finally {
      busy = false;
    }
  });

  if (mode === "spawn" && !isForcedOff()) {
    try {
      const s = await ipc.daemonStatus();
      if (!s.ok) applyDaemonStatus(s);
    } catch { /* heartbeat will own it */ }
  }

  // ── Heartbeat ───────────────────────────────────────────────────────
  // systemd mode: re-probe service state at HEARTBEAT_OK_MS / HEARTBEAT_OFF_MS
  // so an externally-issued `systemctl --user start/stop` is reflected
  // in the toggle within a few seconds.
  // spawn mode: original UDS-ping + auto-respawn behaviour, unchanged.
  async function beat(): Promise<void> {
    if (busy) { schedule(HEARTBEAT_MIN_FAIL); return; }

    if (mode === "systemd") {
      const r = await ipc.serviceState().catch(() => null);
      if (!r?.ok || !r.available) {
        // Unit disappeared mid-session (rare — package downgrade /
        // manual unit file removal). Fall back to spawn mode silently.
        mode = "spawn";
        schedule(HEARTBEAT_MIN_FAIL);
        return;
      }
      const on = !!(r.active && r.enabled);
      status.set(on ? "online" : "offline");
      hideFatalOverlay();
      schedule(on ? HEARTBEAT_OK_MS : HEARTBEAT_OFF_MS);
      return;
    }

    // ── spawn mode (legacy) ──────────────────────────────────────────
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

  if (mode === "systemd" || !isForcedOff()) {
    await beat();
  } else {
    schedule(HEARTBEAT_OFF_MS);
  }
}
