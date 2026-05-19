# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Commands

```bash
# Start full Tauri dev environment (frontend + backend)
npm run tauri dev

# Frontend dev server only (Vite on port 1420)
npm run dev

# Build frontend only
npm run build

# Build full desktop app
npm run tauri build

# Rust backend only (from src-tauri/)
cargo build
cargo build --release
```

There are no configured lint or test commands. TypeScript strict mode (`strict: true`, `noUnusedLocals`, `noUnusedParameters`) acts as the type-checking gate during build.

## Architecture

This is a Tauri v2 + React 18 desktop app for reading and writing RFID tags via serial port.

**Three layers:**

1. **React frontend** (`src/`) — single-page UI in `App.tsx`. Communicates with the backend exclusively via `@tauri-apps/api/core` `invoke()` calls. Polls every 100ms for serial data using `drain_lines`.

2. **Tauri/Rust backend** (`src-tauri/src/lib.rs`) — manages a `SerialState` (held in `tauri::State<Mutex<...>>`). On `connect`, spawns a dedicated read thread that buffers incoming newline-terminated serial bytes. Commands from the frontend are queued via an `mpsc` channel and written to the serial port.

3. **Firmware** (`../firmware/`) — PlatformIO project (planned, not yet implemented). When complete, it will run on the embedded RFID reader hardware that the desktop app talks to over serial.

**Tauri commands (frontend → backend):**

| Command | Purpose |
|---|---|
| `list_ports` | Enumerate available serial ports |
| `connect(portName)` | Open serial port, start read thread |
| `disconnect` | Close connection |
| `send_command(cmd)` | Queue a text command to the device |
| `drain_lines` | Flush and return buffered serial responses |

**Serial protocol:**

The device sends JSON event objects, one per line:
- `{"event":"tag","uid":"04AA...","type":"NTAG215","pages":64}`
- `{"event":"page","page":0,"data":"04AABB..."}`
- `{"event":"ready","fw":"v1.0"}`
- `{"event":"error","msg":"..."}`

The host sends plain-text commands:
- `READ_ALL`, `STATUS`
- `WRITE_PAGE:<n>:<hexdata>`
- `MODE:HID` / `MODE:APP`
- `AUTH:<sector>:<key_type>:<hex_key>` (MIFARE Classic)
- `READ_BLOCK:<n>`, `WRITE_BLOCK:<n>:<hexdata>`

**Supported tag types:** NTAG215/213/216, MIFARE Ultralight, MIFARE Classic (with sector auth). NDEF encoding for NFC Text and URI records (including Spotify URL → NDEF) is implemented entirely in the frontend (`App.tsx`).

## Key configuration

- Tauri window: 1050×720, resizable, centered; CSP disabled.
- Vite dev port: 1420 (strict). Build targets ES2021, Chrome 100+.
- Rust release profile: LTO enabled, `opt-level = "s"` (size-optimized), debug symbols stripped.
