import { card } from "../components/card";
import { segmented } from "../components/segmented";
import { toggle } from "../components/toggle";
import { ipc, type SettingsResponse, type DevicesResponse,
         type ControlsResponse, type PresetsResponse } from "../ipc/client";

type Mode = "low" | "medium" | "high";

// Current state — mutated by IPC fetch and user actions.
// Sentinel `null` until the first daemon round-trip completes — keeps the
// segmented control unselected during the initial paint so the user never
// sees a wrong highlight flash to the right one.
let currentMode: Mode | null = null;
let currentInvert: boolean | null = null;
let applying = false;
// Cached last-applied tuple — avoids a redundant write+reload when the user
// clicks the already-active mode after state sync.
let lastAppliedMode: Mode | null = null;
let lastAppliedInvert: boolean | null = null;

// ── SVG icons (inline, no external deps) ──────────────────────────

const mouseSvg = `<svg viewBox="0 0 24 24"><path d="M12 2C8.13 2 5 5.13 5 9v6c0 3.87 3.13 7 7 7s7-3.13 7-7V9c0-3.87-3.13-7-7-7z"/><line x1="12" y1="2" x2="12" y2="10"/></svg>`;

const wheelSvg = `<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="3"/><path d="M12 5v2M12 17v2M5 12h2M17 12h2"/><circle cx="12" cy="12" r="9" stroke-dasharray="4 3"/></svg>`;

const tabSvg = `<svg viewBox="0 0 24 24"><rect x="3" y="3" width="18" height="18" rx="3"/><line x1="3" y1="9" x2="21" y2="9"/><line x1="9" y1="3" x2="9" y2="9"/></svg>`;

// ── Fatal-state overlay ───────────────────────────────────────────
//
// Renders a centered, non-blocking banner when the daemon-side spawn check
// reports a hard failure (binary_not_found / spawn_failed / timeout). These
// are states the user must act on — installing the daemon, fixing perms.
// Transient `unreachable` from a momentary daemon hiccup keeps using the
// status bar so we don't yell at the user for a 200 ms blip.

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
      // Unknown non-ok states fall through to the status-bar message.
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
  // If the user interacted before the initial fetch completed, fall back to
  // the daemon's documented defaults for the side they didn't touch. Avoids
  // dropping the click without risking a wrong-value write.
  const mode   = currentMode   ?? "medium";
  const invert = currentInvert ?? true;
  if (mode === lastAppliedMode && invert === lastAppliedInvert) return;
  applying = true;
  try {
    const result = await ipc.applySettings(mode, invert);
    if (result.ok) {
      currentMode       = mode;
      currentInvert     = invert;
      lastAppliedMode   = mode;
      lastAppliedInvert = invert;
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

  // Fetch initial state from daemon and sync controls
  void fetchInitialState();
}

async function fetchInitialState() {
  try {
    const res = await ipc.getSettings();
    if (res.ok) {
      const s = res as SettingsResponse;
      currentMode = s.mode;
      currentInvert = s.invert_hwheel;
      // Treat the fetched state as the last-applied baseline so a click on
      // the already-active segment is a no-op rather than a redundant IPC.
      lastAppliedMode = currentMode;
      lastAppliedInvert = currentInvert;
      // Sync the UI controls (DOM is the source of truth for both widgets).
      syncSegmented(currentMode);
      syncToggle(currentInvert);
    }
  } catch {
    // Daemon may not be running — controls keep defaults
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

  // Skeleton while loading
  const skel = document.createElement("div");
  skel.className = "skeleton";
  skel.style.width = "80%";
  skel.style.height = "40px";
  skel.style.margin = "8px 0";
  list.appendChild(skel);
  c.appendChild(list);
  col.appendChild(c);

  // Fetch real data
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

  // Fetch controls
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
  })();

  // Preset list card
  const presetCard = card({ title: "Available Presets" });
  const presetList = document.createElement("div");
  presetList.id = "preset-list";

  presetCard.appendChild(presetList);
  col.appendChild(presetCard);

  void (async () => {
    try {
      const res = await ipc.listPresets();
      presetList.innerHTML = "";
      if (res.ok) {
        const data = res as PresetsResponse;
        for (const p of data.presets) {
          const item = document.createElement("div");
          item.className = "list-item";
          item.setAttribute("aria-selected", "true");
          item.id = `preset-${p.id}`;

          const icon = document.createElement("div");
          icon.className = "icon-circle icon-circle--sm icon-circle--active";
          icon.innerHTML = tabSvg;

          const label = document.createElement("span");
          label.textContent = p.name;

          item.appendChild(icon);
          item.appendChild(label);
          presetList.appendChild(item);
        }
      }
    } catch {
      presetList.innerHTML = "";
      const item = document.createElement("div");
      item.className = "list-item";
      item.setAttribute("aria-selected", "true");

      const icon = document.createElement("div");
      icon.className = "icon-circle icon-circle--sm icon-circle--active";
      icon.innerHTML = tabSvg;

      const label = document.createElement("span");
      label.textContent = "Navigate between tabs";

      item.appendChild(icon);
      item.appendChild(label);
      presetList.appendChild(item);
    }
  })();

  return col;
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

// ── Preset column (right) — Navigate Between Tabs settings ────────

let segmentedEl: HTMLElement | null = null;
let toggleEl: HTMLElement | null = null;

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

  const headerName = document.createElement("div");
  headerName.className = "preset-header__name";
  headerName.textContent = "Navigate between tabs";

  header.appendChild(headerIcon);
  header.appendChild(headerName);
  c.appendChild(header);

  // Sensitivity
  const sensLabel = document.createElement("div");
  sensLabel.className = "section-label";
  sensLabel.textContent = "Sensitivity";

  // No initial highlight — `value` deliberately omitted so all options paint
  // as inactive on first frame. `fetchInitialState()` selects the right one
  // after the first daemon round-trip; eliminates the "default flashes before
  // correct value" flicker users used to see.
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
    // Same rationale as segmented above — render unchecked, sync after fetch.
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
//
// The status bar hosts a single SYSTEM ONLINE/OFFLINE button — the user
// clicks it to take the daemon up or down. The click flips a sticky flag
// in localStorage so the intent survives UI restarts: when the user has
// explicitly stopped the daemon we suppress auto-respawn, otherwise the
// heartbeat would fight the user's last action.
//
// Cadence (intent = ON, auto-respawn enabled):
//   - success → 5 s
//   - failure → 2 s, 4 s, 8 s, 16 s, 30 s (capped)
// Cadence (intent = OFF):
//   - 10 s gentle probe — flips the button to ONLINE if some external
//     agent (systemd, CLI) brings the daemon up, but never respawns.

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
  } catch { /* private mode / disabled storage — fall through */ }
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
    if (s === "online")  { label.textContent = "SYSTEM ONLINE";  btn.setAttribute("aria-pressed", "true");  }
    if (s === "offline") { label.textContent = "SYSTEM OFFLINE"; btn.setAttribute("aria-pressed", "false"); }
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
  let busy = false; // user click in flight — beat() backs off until it clears

  const schedule = (ms: number) => {
    if (timer) clearTimeout(timer);
    timer = setTimeout(beat, ms);
  };

  // Honour persisted intent immediately. The Tauri shell auto-spawns the
  // daemon before the window opens (it can't see localStorage), so if the
  // user previously chose OFF we must take it back down on first paint.
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
          // Kill failed — daemon almost certainly still up; don't lie.
          status.set("online");
          showToast(`Stop failed: ${r.err ?? r.state}`, "error");
          schedule(HEARTBEAT_OK_MS);
        }
      } else {
        // Going online — clear intent first so the heartbeat resumes
        // auto-respawn behaviour, then ask Tauri to spawn.
        setForcedOff(false);
        const r = await ipc.daemonRespawn();
        applyDaemonStatus(r);
        if (r.ok) {
          status.set("online");
          showToast("Daemon started", "success");
          failures = 0;
          schedule(HEARTBEAT_OK_MS);
        } else {
          // Spawn refused (binary missing, perms). Re-arm intent so we
          // don't keep hammering respawn on every heartbeat.
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

  // First-paint spawn-outcome surfacing — same role the old text status had,
  // but the button colour now carries the state, so we only need the overlay
  // for hard-fails.
  if (!isForcedOff()) {
    try {
      const s = await ipc.daemonStatus();
      if (!s.ok) applyDaemonStatus(s);
    } catch { /* heartbeat will own it */ }
  }

  async function beat(): Promise<void> {
    if (busy) { schedule(HEARTBEAT_MIN_FAIL); return; }

    if (isForcedOff()) {
      // Intent = OFF: probe but don't respawn. If the socket has come back
      // (systemd, manual CLI start), reflect that in the button.
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

    // Auto-respawn exhausted — settle on OFFLINE. Avoids a perpetual
    // pulsing "connecting" state when the binary is genuinely missing.
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
