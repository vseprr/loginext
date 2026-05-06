import { card } from "../components/card";
import {
  ipc,
  type AppRule,
  type Preset,
  type ActiveAppResponse,
  type AppRulesResponse,
  type PresetsResponse,
} from "../ipc/client";

/*
 * Per-app rules — active-app detection + rule management.
 *
 * Phase 2.7 refactor: the "Defined rules" list is removed. Rule creation
 * happens via "+ Add rule" on the focused-app row; deletion happens via
 * click-to-deselect in the Available Presets list (owned by main.ts).
 *
 * This module exposes:
 *   - rulesCard()     — the "Per-App Rules / Currently Focused" card
 *   - getRules()      — current in-memory rule list
 *   - getPresets()    — cached preset list
 *   - addRule()       — add a per-app rule (called from main.ts context selector)
 *   - updateRule()    — change a rule's preset
 *   - deleteRule()    — remove a rule (called from main.ts deselect)
 *   - onRulesChange() — register a callback for rule mutations
 *   - getActiveApp()  — current active-app state
 */

type ToastFn = (msg: string, kind?: "success" | "error") => void;
type RulesChangeCb = (rules: AppRule[]) => void;

let toast: ToastFn = () => {};
let rulesChangeCb: RulesChangeCb | null = null;

let rules: AppRule[] = [];
let presets: Preset[] = [];
let activeApp: ActiveAppResponse | null = null;

let activeRowEl: HTMLElement | null = null;
let pollTimer: ReturnType<typeof setInterval> | null = null;
let saveTimer: ReturnType<typeof setTimeout> | null = null;

const SAVE_DEBOUNCE_MS = 250;
// 3 s was chosen to keep IPC traffic minimal back when the daemon-side
// active-window detection was the bottleneck. Now that the listener
// publishes synchronously into the atomic `active_app_hash`, the only
// gap is the UI's poll interval — drop it to 250 ms so the focused-app
// row tracks at near-real-time. UDS round-trip on a local socket is
// sub-millisecond, so the cost of 4× / second is negligible (and we
// already skip when the document is hidden).
const ACTIVE_POLL_MS   = 250;

// ── Public API ──────────────────────────────────────────────────────

export function rulesCard(toastFn: ToastFn): HTMLElement {
  toast = toastFn;
  const c = card({ title: "Per-App Rules" });

  activeRowEl = document.createElement("div");
  activeRowEl.className = "rules-active";
  activeRowEl.setAttribute("aria-live", "polite");
  c.appendChild(activeRowEl);

  renderActive();
  void initialFetch();
  startPolling();

  return c;
}

export function getRules(): AppRule[] { return rules; }
export function getPresets(): Preset[] { return presets; }
export function getActiveApp(): ActiveAppResponse | null { return activeApp; }

export function onRulesChange(cb: RulesChangeCb): void {
  rulesChangeCb = cb;
}

export function addRule(app: string, preset: string): void {
  if (findRule(app)) {
    toast(`Rule for "${app}" already exists`, "error");
    return;
  }
  // New chips inherit globals (mode="", invert=null). The user can
  // override either knob from the right-panel context-aware controls;
  // the rule then tracks that override across reloads.
  rules.push({ app, preset, mode: "", invert: null });
  rules.sort((a, b) => a.app.toLowerCase().localeCompare(b.app.toLowerCase()));
  renderActive();
  scheduleSave();
  rulesChangeCb?.(rules);
}

export function updateRule(app: string, preset: string): void {
  const existing = findRule(app);
  if (!existing) return;
  if (existing.preset === preset) return;
  existing.preset = preset;
  renderActive();
  scheduleSave();
  rulesChangeCb?.(rules);
}

// Set the per-app sensitivity override. Empty string clears the
// override (the daemon then falls back to settings.mode at lookup time).
// Preserves mode + invert state across bind/unbind cycles by living on
// the rule itself rather than a sidecar.
export function setRuleMode(app: string, mode: "" | "low" | "medium" | "high"): void {
  const existing = findRule(app);
  if (!existing) return;
  if ((existing.mode ?? "") === mode) return;
  existing.mode = mode;
  renderActive();
  scheduleSave();
  rulesChangeCb?.(rules);
}

// Set the per-app invert override. `null` clears the override.
export function setRuleInvert(app: string, invert: boolean | null): void {
  const existing = findRule(app);
  if (!existing) return;
  if ((existing.invert ?? null) === invert) return;
  existing.invert = invert;
  renderActive();
  scheduleSave();
  rulesChangeCb?.(rules);
}

// Clear the preset binding but keep the chip — used when the user
// deselects the active preset while staying on the per-app context.
// The chip remains in the context bar (so they can rebind without
// re-focusing the app), and any per-app mode/invert overrides survive.
export function unbindRule(app: string): void {
  const existing = findRule(app);
  if (!existing) return;
  if (existing.preset === "") return;
  existing.preset = "";
  renderActive();
  scheduleSave();
  rulesChangeCb?.(rules);
}

export function deleteRule(app: string): void {
  const idx = rules.findIndex((r) => r.app.toLowerCase() === app.toLowerCase());
  if (idx < 0) return;
  rules.splice(idx, 1);
  renderActive();
  scheduleSave();
  rulesChangeCb?.(rules);
}

export function detachRules(): void {
  if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
  if (saveTimer) { clearTimeout(saveTimer); saveTimer = null; }
}

// ── Initial data load ────────────────────────────────────────────────

async function initialFetch(): Promise<void> {
  try {
    const pres = await ipc.listPresets();
    if (pres.ok) presets = (pres as PresetsResponse).presets;
  } catch { /* offline */ }

  try {
    const rs = await ipc.listAppRules();
    if (rs.ok) rules = [...(rs as AppRulesResponse).rules];
  } catch { /* file unreadable */ }

  await refreshActiveApp();
  rulesChangeCb?.(rules);
}

// ── Active-app polling ───────────────────────────────────────────────

function startPolling(): void {
  if (pollTimer) return;
  pollTimer = setInterval(() => {
    if (document.hidden) return;
    void refreshActiveApp();
  }, ACTIVE_POLL_MS);
}

async function refreshActiveApp(): Promise<void> {
  try {
    const r = await ipc.getActiveApp();
    if (r.ok) {
      activeApp = r as ActiveAppResponse;
    } else {
      activeApp = null;
    }
  } catch {
    activeApp = null;
  }
  renderActive();
}

// ── Rendering ────────────────────────────────────────────────────────

function renderActive(): void {
  if (!activeRowEl) return;
  activeRowEl.innerHTML = "";

  const label = document.createElement("div");
  label.className = "section-label";
  label.textContent = "Currently focused";
  activeRowEl.appendChild(label);

  const row = document.createElement("div");
  row.className = "rules-active__row";

  const name = document.createElement("div");
  name.className = "rules-active__name";

  if (!activeApp) {
    name.textContent = "(no detector — daemon offline?)";
    name.classList.add("rules-active__name--dim");
    row.appendChild(name);
    activeRowEl.appendChild(row);
    return;
  }

  const appKey = activeApp.name.trim();
  if (appKey.length > 0) {
    name.textContent = appKey;
  } else if (activeApp.source === "kwin-dbus") {
    // KWin DBus listener bound but receiving no events → almost always
    // the KWin script not being enabled in the user's kwinrc. Tell the
    // user exactly what to do; without this they see "(unknown)" and
    // have no obvious next step.
    name.textContent = "(unknown — enable LogiNext Focus Bridge in System Settings → KWin Scripts)";
    name.classList.add("rules-active__name--dim");
  } else if (activeApp.source === "none" || activeApp.source === "") {
    name.textContent = "(no compositor backend bound — daemon will log details)";
    name.classList.add("rules-active__name--dim");
  } else {
    name.textContent = "(unknown)";
    name.classList.add("rules-active__name--dim");
  }
  row.appendChild(name);

  const source = document.createElement("span");
  source.className = "rules-active__source";
  source.textContent = activeApp.source;
  row.appendChild(source);

  const existing = appKey ? findRule(appKey) : undefined;

  if (!appKey) {
    activeRowEl.appendChild(row);
    return;
  }

  if (existing) {
    const live = document.createElement("span");
    live.className = "rules-active__live";
    live.textContent = `→ ${presetName(existing.preset)}`;
    row.appendChild(live);
  } else {
    const addBtn = document.createElement("button");
    addBtn.type = "button";
    addBtn.className = "rules-add";
    addBtn.textContent = "+ Add rule";
    addBtn.addEventListener("click", () => {
      const def = activeApp?.global_preset ?? presets[0]?.id ?? "tab_nav";
      addRule(appKey, def);
    });
    row.appendChild(addBtn);
  }

  activeRowEl.appendChild(row);
}

// ── Save (debounced) ─────────────────────────────────────────────────

function scheduleSave(): void {
  if (saveTimer) clearTimeout(saveTimer);
  saveTimer = setTimeout(() => {
    saveTimer = null;
    void saveNow();
  }, SAVE_DEBOUNCE_MS);
}

async function saveNow(): Promise<void> {
  try {
    const r = await ipc.saveAppRules(rules);
    if (r.ok) {
      toast("Rules saved ✓", "success");
    } else {
      toast(`Save failed: ${r.err}`, "error");
    }
  } catch (e) {
    toast(`Save error: ${String(e)}`, "error");
  }
}

// ── Helpers ──────────────────────────────────────────────────────────

function findRule(app: string): AppRule | undefined {
  const k = app.toLowerCase();
  return rules.find((r) => r.app.toLowerCase() === k);
}

function presetName(id: string): string {
  const p = presets.find((x) => x.id === id);
  return p ? p.name : id;
}
