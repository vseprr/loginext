import { invoke } from "@tauri-apps/api/core";

/*
 * Thin typed wrapper around the Rust-side `ipc_request` tauri::command.
 *
 * The Rust side owns the UnixStream, so the frontend never touches a raw
 * socket. Every call is a single line-delimited JSON round-trip; the daemon
 * writes exactly one response line per request.
 */

export interface IpcOk {
  ok: true;
  [k: string]: unknown;
}
export interface IpcErr {
  ok: false;
  err: string;
}
export type IpcResult = IpcOk | IpcErr;

// Typed response shapes for specific commands
export interface SettingsResponse extends IpcOk {
  mode: "low" | "medium" | "high";
  invert_hwheel: boolean;
  // Daemon-side identifier for the currently-bound preset on the thumb wheel.
  // Stable across daemon restarts; matches one of the ids returned by
  // `list_presets`. Phase 2.4 ships with a single value, "tab_nav".
  active_preset: string;
}

export interface Device {
  id: string;
  name: string;
}
export interface DevicesResponse extends IpcOk {
  devices: Device[];
}

export interface Control {
  id: string;
  name: string;
  kind: string;
}
export interface ControlsResponse extends IpcOk {
  controls: Control[];
}

export interface Preset {
  id: string;
  name: string;
}
export interface PresetsResponse extends IpcOk {
  presets: Preset[];
  // Id of the currently-active preset — same field surfaced by get_settings.
  // Optional for backward compatibility with older daemon builds.
  active?: string;
}

// Per-app rule entry. `app` is the lookup key (X11 instance, Hyprland class,
// KWin resourceName/Class) — case-insensitive on the daemon side because the
// FNV-1a hash lower-cases ASCII before mixing.
//
// `preset` is one of the ids returned by `list_presets`, or `""` for a
// "tracked-only" chip (UI-visible context entry with no daemon-side rule).
//
// `mode` and `invert` carry per-app overrides for the global sensitivity /
// invert axis settings. Empty `mode` and `null` `invert` mean "inherit the
// global value at the moment of resolution" — the daemon does the lookup
// every event, so changing the global propagates to inheriting rules
// without a re-save.
export interface AppRule {
  app: string;
  preset: string;
  mode?: "" | "low" | "medium" | "high";   // "" = inherit
  invert?: boolean | null;                  // null = inherit
}

export interface AppRulesResponse extends IpcOk {
  rules: AppRule[];
}

// Active-window probe — what `get_active_app` reports. `name` is empty
// before the listener has seen its first focus event, or when no listener
// backend bound. `source` is the backend that reported it
// ("kwin-dbus" / "x11" / "hyprland" / "kde-wayland" / "none").
// `rule_matched` distinguishes "the per-app rule fired" from "fell back to
// the global preset", which the UI surfaces in the focused-app row.
// `mode` and `invert` are the *effective* values after per-app override,
// useful for the UI to show what the daemon will apply right now.
export interface ActiveAppResponse extends IpcOk {
  hash: string;            // "0x9de02622" — purely informational
  name: string;
  source: string;
  preset: string;          // effective preset (after per-app override)
  global_preset: string;   // current Settings::active_preset
  rule_matched: boolean;
  mode: "low" | "medium" | "high";
  invert: boolean;
}

export async function request(cmd: string, extra: Record<string, unknown> = {}): Promise<IpcResult> {
  const payload = JSON.stringify({ cmd, ...extra });
  const raw = await invoke<string>("ipc_request", { line: payload });
  try {
    return JSON.parse(raw) as IpcResult;
  } catch {
    return { ok: false, err: "bad_response" };
  }
}

// Write config file then tell daemon to reload. Returns the reload ack.
export async function applySettings(
  sensitivity: "low" | "medium" | "high",
  invertHwheel: boolean,
  activePreset: string,
): Promise<IpcResult> {
  await invoke("write_config", {
    sensitivity,
    invertHwheel,
    activePreset,
  });
  return request("reload");
}

// Read app_rules.txt via the Tauri-side helper (the daemon has no
// `list_rules` IPC — string rules don't survive the FNV-1a hashing in
// RuleTable, so the file is the source of truth).
export async function listAppRules(): Promise<AppRulesResponse | IpcErr> {
  const raw = await invoke<string>("read_app_rules");
  try {
    const parsed = JSON.parse(raw) as IpcResult & { rules?: AppRule[] };
    if (parsed.ok) return parsed as AppRulesResponse;
    return parsed as IpcErr;
  } catch {
    return { ok: false, err: "bad_response" };
  }
}

// Atomic write + daemon reload. The Rust side rejects entries with
// reserved characters before the write hits disk.
export async function saveAppRules(rules: AppRule[]): Promise<IpcResult> {
  try {
    await invoke("write_app_rules", { rules });
  } catch (e) {
    return { ok: false, err: `save: ${String(e)}` };
  }
  return request("reload");
}

// Daemon lifecycle envelope returned by the Tauri-side spawn check.
// `state` is the discriminator; payload fields depend on it.
export interface DaemonStatus {
  ok: boolean;
  state:
    | "already_running"
    | "spawned"
    | "spawn_failed"
    | "binary_not_found"
    | "timeout"
    // kill_daemon outcomes:
    | "killed"
    | "not_running"
    | "signal_failed";
  pid?: number;
  err?: string;
}

async function invokeDaemon(
  cmd: "daemon_status" | "daemon_respawn" | "kill_daemon",
): Promise<DaemonStatus> {
  const raw = await invoke<string>(cmd);
  try {
    return JSON.parse(raw) as DaemonStatus;
  } catch {
    return { ok: false, state: "spawn_failed", err: "bad_response" };
  }
}

// systemd-user service envelope. `available=false` means the unit file
// isn't installed at all (the toast tells the user to run install.sh or
// reinstall the package). Otherwise the toggle reads `active && enabled`
// to decide its position — both bits matter so a manually-stopped unit
// (active=false, enabled=true) renders as OFF rather than mid-state.
export interface ServiceState {
  ok: boolean;
  available?: boolean;
  active?: boolean;
  enabled?: boolean;
  state?: "enabled" | "disabled" | "enable_failed" | "disable_failed";
  err?: string;
}

async function invokeService(
  cmd: "service_state" | "service_enable" | "service_disable",
): Promise<ServiceState> {
  const raw = await invoke<string>(cmd);
  try {
    return JSON.parse(raw) as ServiceState;
  } catch {
    return { ok: false, err: "bad_response" };
  }
}

export const ipc = {
  ping:          () => request("ping"),
  getSettings:   () => request("get_settings") as Promise<SettingsResponse | IpcErr>,
  listDevices:   () => request("list_devices") as Promise<DevicesResponse | IpcErr>,
  listControls:  () => request("list_controls") as Promise<ControlsResponse | IpcErr>,
  listPresets:   () => request("list_presets") as Promise<PresetsResponse | IpcErr>,
  getActiveApp:  () => request("get_active_app") as Promise<ActiveAppResponse | IpcErr>,
  reload:        () => request("reload"),
  applySettings,
  // Per-app rules: file-backed, UI is the editor.
  listAppRules,
  saveAppRules,
  // Lifecycle: report startup outcome + ask Tauri to re-probe / respawn.
  daemonStatus:  () => invokeDaemon("daemon_status"),
  daemonRespawn: () => invokeDaemon("daemon_respawn"),
  daemonKill:    () => invokeDaemon("kill_daemon"),
  // systemd --user service control (drives the DAEMON ONLINE/OFFLINE toggle).
  serviceState:   () => invokeService("service_state"),
  serviceEnable:  () => invokeService("service_enable"),
  serviceDisable: () => invokeService("service_disable"),
};
