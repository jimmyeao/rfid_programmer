use std::io::{Read, Write};
use std::sync::{mpsc, Arc, Mutex};
use tauri::State;

// ───── state ─────────────────────────────────────────────────────────────────

pub struct SerialState {
    cmd_tx: Mutex<Option<mpsc::Sender<String>>>,
    lines:  Arc<Mutex<Vec<String>>>,
}

impl Default for SerialState {
    fn default() -> Self {
        SerialState {
            cmd_tx: Mutex::new(None),
            lines:  Arc::new(Mutex::new(Vec::new())),
        }
    }
}

// ───── commands ──────────────────────────────────────────────────────────────

#[tauri::command]
fn list_ports() -> Vec<String> {
    serialport::available_ports()
        .unwrap_or_default()
        .into_iter()
        .map(|p| p.port_name)
        .collect()
}

#[tauri::command]
fn connect(
    state: State<SerialState>,
    port_name: String,
) -> Result<(), String> {
    *state.cmd_tx.lock().unwrap() = None;

    // Retry a few times — after a hot-reload the OS may not have released
    // the handle from the previous process yet.
    let mut port = None;
    let mut last_err = String::new();
    for attempt in 0..5u8 {
        match serialport::new(&port_name, 115_200)
            .data_bits(serialport::DataBits::Eight)
            .stop_bits(serialport::StopBits::One)
            .parity(serialport::Parity::None)
            .flow_control(serialport::FlowControl::None)
            .timeout(std::time::Duration::from_millis(100))
            .open()
        {
            Ok(p)  => { port = Some(p); break; }
            Err(e) => {
                last_err = e.to_string();
                if attempt < 4 {
                    std::thread::sleep(std::time::Duration::from_millis(400));
                }
            }
        }
    }
    let mut port = port.ok_or_else(|| format!("Cannot open {port_name}: {last_err}"))?;

    // Assert DTR so the USB CDC device knows a terminal is connected
    let _ = port.write_data_terminal_ready(true);

    let (tx, rx) = mpsc::channel::<String>();
    *state.cmd_tx.lock().unwrap() = Some(tx);

    let lines = Arc::clone(&state.lines);

    std::thread::spawn(move || {
        let mut buf = String::new();
        let mut byte = [0u8; 1];

        loop {
            // Drain command queue
            while let Ok(cmd) = rx.try_recv() {
                let _ = port.write_all(format!("{}\n", cmd.trim()).as_bytes());
            }

            match port.read(&mut byte) {
                Ok(1) => {
                    if byte[0] == b'\n' {
                        let line = buf.trim().to_string();
                        if !line.is_empty() {
                            lines.lock().unwrap().push(line);
                        }
                        buf.clear();
                    } else if byte[0] != b'\r' {
                        buf.push(byte[0] as char);
                    }
                }
                Ok(_) => {}
                Err(e) if e.kind() == std::io::ErrorKind::TimedOut => {}
                Err(_) => {
                    lines.lock().unwrap()
                        .push(r#"{"event":"disconnected"}"#.to_string());
                    break;
                }
            }
        }
    });

    Ok(())
}

#[tauri::command]
fn disconnect(state: State<SerialState>) {
    *state.cmd_tx.lock().unwrap() = None;
}

#[tauri::command]
fn send_command(state: State<SerialState>, cmd: String) -> Result<(), String> {
    let guard = state.cmd_tx.lock().unwrap();
    let tx = guard.as_ref().ok_or("Not connected")?;
    tx.send(cmd).map_err(|e| e.to_string())
}

// Frontend polls this every 100 ms to pull buffered lines
#[tauri::command]
fn drain_lines(state: State<SerialState>) -> Vec<String> {
    std::mem::take(&mut *state.lines.lock().unwrap())
}

// ───── entry point ───────────────────────────────────────────────────────────

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .manage(SerialState::default())
        .invoke_handler(tauri::generate_handler![
            list_ports,
            connect,
            disconnect,
            send_command,
            drain_lines,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
