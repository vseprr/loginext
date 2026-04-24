mod ipc_bridge;

#[tauri::command]
fn ipc_request(line: String) -> String {
    ipc_bridge::request(line)
}

#[tauri::command]
fn write_config(sensitivity: String, invert_hwheel: bool) -> Result<(), String> {
    ipc_bridge::write_config(&sensitivity, invert_hwheel)
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![ipc_request, write_config])
        .run(tauri::generate_context!())
        .expect("loginext-ui: tauri runtime failed");
}
