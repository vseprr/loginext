import { ipc, type SystemInfoResponse, type DaemonLogResponse } from "../ipc/client";

// Bug-report panel state. Singleton — we never open two at once.
let panelEl: HTMLElement | null = null;
let isOpen = false;

export function createBugReportButton(): HTMLButtonElement {
  const btn = document.createElement("button");
  btn.type = "button";
  btn.className = "bug-report-btn";
  btn.setAttribute("aria-label", "Open bug report");
  btn.title = "Report a bug — collects system info + recent log";
  btn.innerHTML =
    `<svg viewBox="0 0 24 24" aria-hidden="true">` +
    `<path d="M12 2a4 4 0 0 1 4 4v1H8V6a4 4 0 0 1 4-4z" ` +
    `fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round"/>` +
    `<rect x="6" y="8" width="12" height="11" rx="6" ` +
    `fill="none" stroke="currentColor" stroke-width="1.6"/>` +
    `<path d="M3 12h3M18 12h3M3 18h3M18 18h3M3 8h3M18 8h3" ` +
    `stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/></svg>`;
  btn.addEventListener("click", () => {
    if (isOpen) closePanel();
    else void openPanel();
  });
  return btn;
}

async function openPanel(): Promise<void> {
  if (!panelEl) panelEl = buildPanelShell();
  document.body.appendChild(panelEl);
  // Force reflow so the slide-in transition runs.
  void panelEl.offsetWidth;
  panelEl.classList.add("bug-report--open");
  isOpen = true;

  // Populate async — show skeletons in the meantime.
  const [info, log] = await Promise.all([
    ipc.systemInfo(),
    ipc.readDaemonLog(100),
  ]);
  fillPanel(info, log);
}

function closePanel(): void {
  if (!panelEl) return;
  panelEl.classList.remove("bug-report--open");
  isOpen = false;
  // Detach after the transition completes.
  setTimeout(() => {
    if (panelEl && !isOpen) {
      panelEl.remove();
    }
  }, 300);
}

function buildPanelShell(): HTMLElement {
  const panel = document.createElement("div");
  panel.className = "bug-report";
  panel.setAttribute("role", "dialog");
  panel.setAttribute("aria-labelledby", "bug-report-title");

  panel.innerHTML = `
    <div class="bug-report__header">
      <h2 id="bug-report-title" class="bug-report__title">Report a bug</h2>
      <button class="bug-report__close" aria-label="Close bug report">
        <svg viewBox="0 0 16 16"><path d="M4 4 L12 12 M12 4 L4 12" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"/></svg>
      </button>
    </div>
    <div class="bug-report__body">
      <div class="bug-report__intro">
        Describe what you observed — we'll bundle it with the system
        info and recent daemon log so we can reproduce the issue.
      </div>

      <label class="bug-report__label" for="bug-report-desc">
        What happened?
      </label>
      <textarea id="bug-report-desc" class="bug-report__desc"
        placeholder="The thumb wheel scroll suddenly stopped switching tabs in Firefox after I…"
        rows="4"></textarea>

      <div class="bug-report__section-title">System info</div>
      <div class="bug-report__info" id="bug-report-info">
        <div class="bug-report__skeleton"></div>
      </div>

      <div class="bug-report__section-title">
        Recent daemon log
        <span class="bug-report__log-path" id="bug-report-log-path"></span>
      </div>
      <pre class="bug-report__log" id="bug-report-log">Loading…</pre>

      <div class="bug-report__actions">
        <button class="bug-report__btn bug-report__btn--primary"
          id="bug-report-copy">
          Copy as GitHub Issue markdown
        </button>
        <span class="bug-report__copied" id="bug-report-copied"></span>
      </div>

      <div class="bug-report__hint">
        Once copied, paste into a new issue at
        <span class="bug-report__url">github.com/vseprr/loginext/issues/new</span>
        — bug-tracking integration coming in a future release.
      </div>
    </div>
  `;

  panel.querySelector(".bug-report__close")?.addEventListener("click", closePanel);
  panel.querySelector("#bug-report-copy")?.addEventListener("click", () => void onCopy());

  return panel;
}

function fillPanel(info: SystemInfoResponse, log: DaemonLogResponse): void {
  const infoEl = document.getElementById("bug-report-info");
  if (infoEl) {
    infoEl.innerHTML = formatSystemInfoHtml(info);
  }
  const logEl = document.getElementById("bug-report-log");
  if (logEl) {
    logEl.textContent = log.ok
      ? (log.body ?? "(empty log)")
      : `(could not read daemon log: ${log.err ?? "unknown"})`;
  }
  const pathEl = document.getElementById("bug-report-log-path");
  if (pathEl && log.path) {
    pathEl.textContent = log.path;
  }
}

function formatSystemInfoHtml(i: SystemInfoResponse): string {
  if (!i.ok) {
    return `<div class="bug-report__info-err">Could not collect system info: ${i.err ?? "unknown"}</div>`;
  }
  const rows: Array<[string, string]> = [
    ["OS",            i.os ?? "unknown"],
    ["Kernel",        i.kernel ?? "unknown"],
    ["Desktop",       i.compositor ?? "unknown"],
    ["Session",       i.session ?? "unknown"],
    ["Wayland",       i.wayland ?? "unknown"],
    ["LogiNext",      i.loginext_version ?? "unknown"],
    ["Service",       serviceStateLabel(i)],
    ["KWin bridge",   i.kwin_focus_bridge ?? "unknown"],
  ];
  return rows.map(([k, v]) =>
    `<div class="bug-report__info-row"><span class="bug-report__info-k">${esc(k)}</span><span class="bug-report__info-v">${esc(v)}</span></div>`
  ).join("");
}

function serviceStateLabel(i: SystemInfoResponse): string {
  if (i.service_available === false) return "unit not installed";
  const parts: string[] = [];
  parts.push(i.service_active ? "active" : "inactive");
  parts.push(i.service_enabled ? "enabled" : "disabled");
  return parts.join(", ");
}

function esc(s: string): string {
  return s
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

async function onCopy(): Promise<void> {
  const descEl = document.getElementById("bug-report-desc") as HTMLTextAreaElement | null;
  const description = (descEl?.value ?? "").trim() || "(no description provided)";

  // Re-fetch fresh state for the copy. The panel may have been open a
  // while; we don't want to ship stale info.
  const [info, log] = await Promise.all([
    ipc.systemInfo(),
    ipc.readDaemonLog(100),
  ]);
  const markdown = buildMarkdown(description, info, log);

  const result = await ipc.copyToClipboard(markdown);
  const copiedEl = document.getElementById("bug-report-copied");
  if (!copiedEl) return;
  if (result.ok) {
    copiedEl.textContent = `Copied (via ${result.tool}). Paste into a new GitHub issue.`;
    copiedEl.className = "bug-report__copied bug-report__copied--ok";
  } else {
    copiedEl.textContent = `Copy failed: ${result.err ?? "no clipboard tool"}. Select the text below manually.`;
    copiedEl.className = "bug-report__copied bug-report__copied--err";
    // Render the markdown as plain text so the user can manually copy.
    const logEl = document.getElementById("bug-report-log");
    if (logEl) {
      logEl.textContent = markdown;
    }
  }
}

function buildMarkdown(
  description: string,
  info: SystemInfoResponse,
  log: DaemonLogResponse,
): string {
  const env = info.ok ? [
    `- **OS:** ${info.os ?? "unknown"}`,
    `- **Kernel:** ${info.kernel ?? "unknown"}`,
    `- **Desktop:** ${info.compositor ?? "unknown"} (${info.session ?? "unknown"}, wayland=${info.wayland ?? "unknown"})`,
    `- **LogiNext version:** ${info.loginext_version ?? "unknown"}`,
    `- **Service state:** ${serviceStateLabel(info)}`,
    `- **KWin focus bridge:** ${info.kwin_focus_bridge ?? "unknown"}`,
  ].join("\n") : "(system info unavailable)";

  const logBody = log.ok ? (log.body ?? "(empty)") : `(could not read log: ${log.err ?? "unknown"})`;
  const logPath = log.path ? ` (${log.path})` : "";

  return `### Bug report — LogiNext

**What happened:**
${description}

**Environment:**
${env}

**Recent daemon log${logPath}:**
\`\`\`
${logBody}
\`\`\`
`;
}
