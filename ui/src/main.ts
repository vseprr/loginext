import { attachHeartbeat, attachAlwaysOnTopPin, renderMain } from "./views/main";

const app = document.getElementById("app");
if (!app) throw new Error("#app mount point missing");

// App shell: header → three-column main → status bar
const shell = document.createElement("div");
shell.className = "app-shell";

// ── Header ────────────────────────────────────────────────────────

const header = document.createElement("header");
header.className = "app-header";

const logo = document.createElement("div");
logo.className = "app-header__logo";
logo.innerHTML = `<svg viewBox="0 0 16 16"><path d="M8 1L14 5v6l-6 4-6-4V5l6-4z"/></svg>`;

const title = document.createElement("span");
title.className = "app-header__title";
title.textContent = "LogiNext";

const version = document.createElement("span");
version.className = "app-header__version";
version.textContent = "v0.1";

// Pin (always-on-top) button. Placed flush-right via the spacer below
// so it doesn't crowd the title cluster on the left. The actual click
// handler + persistence lives in `attachAlwaysOnTopPin` so it can
// share the localStorage / Tauri-invoke plumbing with the rest of
// the lifecycle code in views/main.ts.
const headerSpacer = document.createElement("div");
headerSpacer.className = "app-header__spacer";

const pin = document.createElement("button");
pin.type = "button";
pin.className = "app-header__pin";
pin.setAttribute("aria-pressed", "false");
pin.setAttribute("aria-label", "Keep LogiNext above other windows");
pin.title = "Keep above (so the per-app rule UI stays visible while you click around)";
// Inline SVG pin icon — outlined when off, filled-tilted when on.
// Rotated 30° in the "on" state via CSS so the affordance reads as a
// thumbtack pressed in. Stroke colour switches to the accent.
pin.innerHTML =
  `<svg viewBox="0 0 16 16" aria-hidden="true">` +
  `<path d="M8 1.5l3 3v3l1.5 1.5H10v3.5L8 14.5l-2-2.5V9H3.5L5 7.5v-3l3-3z"` +
  ` fill="none" stroke="currentColor" stroke-width="1.4" stroke-linejoin="round"/></svg>`;

header.appendChild(logo);
header.appendChild(title);
header.appendChild(version);
header.appendChild(headerSpacer);
header.appendChild(pin);

// ── Main area ─────────────────────────────────────────────────────

const main = document.createElement("div");

// ── Status bar ────────────────────────────────────────────────────

const status = document.createElement("div");
status.className = "status-bar";

shell.appendChild(header);
shell.appendChild(main);
shell.appendChild(status);
app.appendChild(shell);

renderMain(main);
void attachHeartbeat(status);
void attachAlwaysOnTopPin(pin);
