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
 * Per-app rules editor.
 *
 * The daemon hashes the app id at config-load time, so the rules file is the
 * only way to round-trip strings back to the user. The Tauri side reads /
 * writes that file on our behalf — see ui/src-tauri/src/ipc_bridge.rs.
 *
 * Lifecycle:
 *   - On mount: fetch rules + preset list once, render.
 *   - On user edit: mutate the in-memory list, schedule a debounced
 *     `saveAppRules()` (250 ms) so rapid changes coalesce into one
 *     write+reload — the daemon's reload is the expensive bit, not the
 *     write.
 *   - Active-app banner polls `get_active_app` every 3 s when the window
 *     is visible. The poll is paused while the document is hidden so a
 *     minimised UI is not waking the daemon every few seconds.
 *
 * Failure shape: a save error keeps the local state but shows a toast and
 * paints the offending row red. The user can retry from the same form
 * — we never silently drop their edits.
 */

// ── External hooks ───────────────────────────────────────────────────
//
// Decouple the rules view from the host page's toast helper so this file
// stays import-free of views/main.ts (avoids a circular dep). The host
// wires it up via attachRules().

type ToastFn = (msg: string, kind?: "success" | "error") => void;
let toast: ToastFn = () => {};

// ── Local state ──────────────────────────────────────────────────────

let rules: AppRule[] = [];
let presets: Preset[] = [];
let activeApp: ActiveAppResponse | null = null;

let listEl: HTMLElement | null = null;
let activeRowEl: HTMLElement | null = null;
let pollTimer: ReturnType<typeof setInterval> | null = null;
let saveTimer: ReturnType<typeof setTimeout> | null = null;

const SAVE_DEBOUNCE_MS = 250;
const ACTIVE_POLL_MS   = 3_000;

// ── Public mount ─────────────────────────────────────────────────────

export function rulesCard(toastFn: ToastFn): HTMLElement {
  toast = toastFn;
  const c = card({ title: "Per-App Rules" });

  // Active-app row at the top — kept above the list so the user always sees
  // what the daemon currently considers "this window" without scrolling.
  activeRowEl = document.createElement("div");
  activeRowEl.className = "rules-active";
  activeRowEl.setAttribute("aria-live", "polite");
  c.appendChild(activeRowEl);

  // Rules list (one row per entry; empty placeholder when none).
  listEl = document.createElement("div");
  listEl.className = "rules-list";
  c.appendChild(listEl);

  // Initial paint with skeleton so the card has visible content before the
  // first round-trip resolves.
  renderActive();
  renderList();

  void initialFetch();
  startPolling();

  return c;
}

export function detachRules(): void {
  // Currently no caller — the rules card lives for the lifetime of the
  // window — but exposing the symmetry keeps the contract honest if the
  // view ever gets unmounted (settings page swap, etc.).
  if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
  if (saveTimer) { clearTimeout(saveTimer); saveTimer = null; }
}

// ── Initial data load ────────────────────────────────────────────────

async function initialFetch(): Promise<void> {
  // Presets first — the rule rows render preset dropdowns and we'd rather
  // paint them only once the options are known.
  try {
    const pres = await ipc.listPresets();
    if (pres.ok) presets = (pres as PresetsResponse).presets;
  } catch { /* offline — keep empty, dropdowns will be inactive */ }

  try {
    const rs = await ipc.listAppRules();
    if (rs.ok) rules = [...(rs as AppRulesResponse).rules];
  } catch { /* file unreadable — start empty */ }

  await refreshActiveApp();
  renderList();
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

  // Prefer the daemon's `name` (matches what app_rules.txt would key on);
  // fall back to "(unknown)" so the row always has visible text.
  const appKey = activeApp.name.trim();
  name.textContent = appKey.length > 0 ? appKey : "(unknown)";
  row.appendChild(name);

  // Subtle source pill so the user can tell whether they're looking at the
  // KWin bridge, X11, etc. — useful for diagnosing "why isn't my rule
  // matching?" without grepping the log.
  const source = document.createElement("span");
  source.className = "rules-active__source";
  source.textContent = activeApp.source;
  row.appendChild(source);

  // Add-rule affordance: shown when the focused app has no rule. If it
  // already has one, point at it so the user knows it's live.
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
      // Default new rule to the daemon's current global preset if known,
      // otherwise the first preset on the list. The user can change it
      // immediately via the row's dropdown.
      const def = activeApp?.global_preset ?? presets[0]?.id ?? "tab_nav";
      addRule(appKey, def);
    });
    row.appendChild(addBtn);
  }

  activeRowEl.appendChild(row);
}

function renderList(): void {
  if (!listEl) return;
  listEl.innerHTML = "";

  const label = document.createElement("div");
  label.className = "section-label";
  label.textContent = `Defined rules (${rules.length})`;
  listEl.appendChild(label);

  if (rules.length === 0) {
    const empty = document.createElement("div");
    empty.className = "rules-empty";
    empty.textContent = "No per-app rules yet — every app uses the global preset.";
    listEl.appendChild(empty);
    return;
  }

  for (const r of rules) {
    listEl.appendChild(ruleRow(r));
  }
}

function ruleRow(r: AppRule): HTMLElement {
  const row = document.createElement("div");
  row.className = "rule-row";

  const appName = document.createElement("div");
  appName.className = "rule-row__app";
  appName.textContent = r.app;
  appName.title = r.app;
  row.appendChild(appName);

  const select = document.createElement("select");
  select.className = "rule-row__preset";
  for (const p of presets) {
    const opt = document.createElement("option");
    opt.value = p.id;
    opt.textContent = p.name;
    if (p.id === r.preset) opt.selected = true;
    select.appendChild(opt);
  }
  // Fallback: if the preset isn't in our cached list (e.g. we failed to
  // fetch presets but rules loaded), render the raw id so the user still
  // sees what's in the file.
  if (!presets.find((p) => p.id === r.preset)) {
    const opt = document.createElement("option");
    opt.value = r.preset;
    opt.textContent = `${r.preset} (?)`;
    opt.selected = true;
    select.appendChild(opt);
  }
  select.addEventListener("change", () => {
    updateRule(r.app, select.value);
  });
  row.appendChild(select);

  const del = document.createElement("button");
  del.type = "button";
  del.className = "rule-row__delete";
  del.title = "Remove rule";
  del.setAttribute("aria-label", `Remove rule for ${r.app}`);
  del.textContent = "×";
  del.addEventListener("click", () => deleteRule(r.app));
  row.appendChild(del);

  return row;
}

// ── Mutations ────────────────────────────────────────────────────────

function findRule(app: string): AppRule | undefined {
  // Case-insensitive — matches the daemon's FNV-1a lower-casing so the UI
  // never shows a "duplicate" rule that resolves to the same hash.
  const k = app.toLowerCase();
  return rules.find((r) => r.app.toLowerCase() === k);
}

function addRule(app: string, preset: string): void {
  if (findRule(app)) {
    toast(`Rule for "${app}" already exists`, "error");
    return;
  }
  rules.push({ app, preset });
  // Keep the persisted file (and the on-screen list) sorted alphabetically
  // by app name — manual edits stay readable, and the diff between two
  // saves is small.
  rules.sort((a, b) => a.app.toLowerCase().localeCompare(b.app.toLowerCase()));
  renderList();
  renderActive();
  scheduleSave();
}

function updateRule(app: string, preset: string): void {
  const existing = findRule(app);
  if (!existing) return;
  if (existing.preset === preset) return;
  existing.preset = preset;
  renderActive();
  scheduleSave();
}

function deleteRule(app: string): void {
  const idx = rules.findIndex((r) => r.app.toLowerCase() === app.toLowerCase());
  if (idx < 0) return;
  rules.splice(idx, 1);
  renderList();
  renderActive();
  scheduleSave();
}

// ── Save (debounced) ─────────────────────────────────────────────────
//
// Reload is the expensive side of save (the daemon re-reads the file +
// re-hashes the table). Coalescing rapid edits — e.g. flipping a preset
// dropdown and immediately deleting another row — into one write+reload
// keeps the perceived snappiness up and avoids a brief window where the
// daemon's table is half-stale.

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

function presetName(id: string): string {
  const p = presets.find((x) => x.id === id);
  return p ? p.name : id;
}
