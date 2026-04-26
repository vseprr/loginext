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
): Promise<IpcResult> {
  await invoke("write_config", {
    sensitivity,
    invertHwheel,
  });
  return request("reload");
}

// Daemon lifecycle envelope returned by the Tauri-side spawn check.
// `state` is the discriminator; payload fields depend on it.
export interface DaemonStatus {
  ok: boolean;
  state: "already_running" | "spawned" | "spawn_failed" | "binary_not_found" | "timeout";
  pid?: number;
  err?: string;
}

async function invokeDaemon(cmd: "daemon_status" | "daemon_respawn"): Promise<DaemonStatus> {
  const raw = await invoke<string>(cmd);
  try {
    return JSON.parse(raw) as DaemonStatus;
  } catch {
    return { ok: false, state: "spawn_failed", err: "bad_response" };
  }
}

export const ipc = {
  ping:          () => request("ping"),
  getSettings:   () => request("get_settings") as Promise<SettingsResponse | IpcErr>,
  listDevices:   () => request("list_devices") as Promise<DevicesResponse | IpcErr>,
  listControls:  () => request("list_controls") as Promise<ControlsResponse | IpcErr>,
  listPresets:   () => request("list_presets") as Promise<PresetsResponse | IpcErr>,
  reload:        () => request("reload"),
  applySettings,
  // Lifecycle: report startup outcome + ask Tauri to re-probe / respawn.
  daemonStatus:  () => invokeDaemon("daemon_status"),
  daemonRespawn: () => invokeDaemon("daemon_respawn"),
};
