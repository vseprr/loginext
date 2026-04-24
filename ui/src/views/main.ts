import { card } from "../components/card";
import { segmented } from "../components/segmented";
import { toggle } from "../components/toggle";
import { ipc, type SettingsResponse, type DevicesResponse,
         type ControlsResponse, type PresetsResponse } from "../ipc/client";

type Mode = "low" | "medium" | "high";

// Current state — mutated by IPC fetch and user actions.
// Default matches the daemon's default profile so pre-fetch clicks on the
// highlighted segment don't fire a redundant apply.
let currentMode: Mode = "medium";
let currentInvert = true;
let applying = false;
// Cached last-applied tuple — avoids a redundant write+reload when the user
// clicks the already-active mode after state sync.
let lastAppliedMode: Mode | null = null;
let lastAppliedInvert: boolean | null = null;

// ── SVG icons (inline, no external deps) ──────────────────────────

const mouseSvg = `<svg viewBox="0 0 24 24"><path d="M12 2C8.13 2 5 5.13 5 9v6c0 3.87 3.13 7 7 7s7-3.13 7-7V9c0-3.87-3.13-7-7-7z"/><line x1="12" y1="2" x2="12" y2="10"/></svg>`;

const wheelSvg = `<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="3"/><path d="M12 5v2M12 17v2M5 12h2M17 12h2"/><circle cx="12" cy="12" r="9" stroke-dasharray="4 3"/></svg>`;

const tabSvg = `<svg viewBox="0 0 24 24"><rect x="3" y="3" width="18" height="18" rx="3"/><line x1="3" y1="9" x2="21" y2="9"/><line x1="9" y1="3" x2="9" y2="9"/></svg>`;

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
  // Skip redundant apply when the new tuple matches the last one we wrote.
  // Prevents a needless write+reload cycle after initial state sync.
  if (currentMode === lastAppliedMode && currentInvert === lastAppliedInvert) return;
  applying = true;
  try {
    const result = await ipc.applySettings(currentMode, currentInvert);
    if (result.ok) {
      lastAppliedMode = currentMode;
      lastAppliedInvert = currentInvert;
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

  segmentedEl = segmented<Mode>({
    options: [
      { value: "low",    label: "Low" },
      { value: "medium", label: "Medium" },
      { value: "high",   label: "High" },
    ],
    value: currentMode,
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
    checked: currentInvert,
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

// ── Heartbeat ─────────────────────────────────────────────────────

export async function attachHeartbeat(bar: HTMLElement): Promise<void> {
  // Add status dot
  const dot = document.createElement("span");
  dot.className = "status-bar__dot";
  bar.prepend(dot);

  const textEl = document.createElement("span");
  bar.appendChild(textEl);

  const beat = async () => {
    try {
      const r = await ipc.ping();
      bar.className = "status-bar " + (r.ok ? "status-bar--ok" : "status-bar--err");
      textEl.textContent = r.ok ? "daemon: connected" : `daemon: ${r.err}`;
    } catch (e) {
      bar.className = "status-bar status-bar--warn";
      textEl.textContent = `daemon: unreachable (${String(e)})`;
    }
  };
  await beat();
  setInterval(beat, 5000);
}
