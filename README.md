# RFID Programmer

A desktop app for reading and writing RFID tags, built with Tauri + React. Communicates with an ESP32-S3 + PN532 reader over USB serial.

## Features

- Read and display full tag memory (pages/blocks) with hex and ASCII view
- Write pages (NTAG/Ultralight) and blocks (MIFARE Classic with sector auth)
- NDEF encode/decode: NFC Text records, URI records, Spotify playlist URLs
- Dual-mode firmware: **HID keyboard** (types UID + Tab on scan) or **APP mode** (serial control)
- Supports NTAG213/215/216, MIFARE Ultralight, MIFARE Classic

## Hardware

| Component | Detail |
|---|---|
| Microcontroller | ESP32-S3-DevKitC-1 |
| NFC module | PN532 (I2C mode) |
| Interface | USB composite — CDC serial + HID keyboard on one port |

**PN532 wiring (I2C):**

```
PN532 SDA  →  GPIO8
PN532 SCL  →  GPIO9
PN532 IRQ  →  GPIO4
PN532 RST  →  3.3 V (tie high)
PN532 VCC  →  3.3 V
PN532 GND  →  GND
```

Set the PN532 breakout jumper to I2C mode (SEL0=1, SEL1=0).

## Getting Started

### Firmware

Requires [PlatformIO](https://platformio.org/).

```bash
cd firmware
pio run --target upload
```

### Desktop App

Requires [Node.js](https://nodejs.org/) and the [Tauri prerequisites](https://tauri.app/start/prerequisites/) for your platform (Rust + system deps).

```bash
cd app
npm install
npm run tauri dev
```

To build a distributable installer:

```bash
npm run tauri build
```

## Serial Protocol

The desktop app communicates with the firmware over USB CDC serial. Commands are newline-terminated plain text; responses are newline-terminated JSON.

**Commands (host → device):**

| Command | Description |
|---|---|
| `STATUS` | Query HID mode state |
| `MODE:HID` | Enable HID keyboard output |
| `MODE:APP` | Disable HID keyboard output |
| `READ_ALL` | Dump all pages (NTAG/Ultralight) |
| `READ_PAGE:n` | Read single page n |
| `WRITE_PAGE:n:HHHHHHHH` | Write 4 bytes (8 hex chars) to page n |
| `AUTH:sector:A/B:HHHHHHHHHHHH` | Authenticate MIFARE Classic sector |
| `READ_BLOCK:n` | Read MIFARE Classic block n |
| `WRITE_BLOCK:n:HH...` | Write 16 bytes (32 hex chars) to block n |

**Events (device → host):**

```json
{"event":"ready","fw":"1.7"}
{"event":"tag","uid":"04AABBCCDD","type":"NTAG215","pages":135}
{"event":"page","page":4,"data":"D1010E55"}
{"event":"tag_removed"}
{"event":"ok","msg":"Page written","page":5}
{"event":"error","msg":"Auth failed — wrong key?"}
```

## Project Structure

```
rfid-tool/
├── app/          # Tauri + React desktop application
│   ├── src/      # React frontend (TypeScript)
│   └── src-tauri/# Rust backend (serial port management)
└── firmware/     # PlatformIO firmware for ESP32-S3 + PN532
```
