import { attachHeartbeat, renderMain } from "./views/main";

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

header.appendChild(logo);
header.appendChild(title);
header.appendChild(version);

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
