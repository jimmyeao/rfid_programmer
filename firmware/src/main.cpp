//  PN532 wiring (I2C, adjust pins to your board):
//    PN532 SDA  →  GPIO8
//    PN532 SCL  →  GPIO9
//    PN532 IRQ  →  GPIO4   (required — library polls this pin)
//    PN532 RST  →  3.3 V   (tie high; not controlled by firmware)
//    PN532 VCC  →  3.3 V
//    PN532 GND  →  GND
//
//  On the PN532 breakout set jumper/switch to I2C mode (SEL0=1, SEL1=0).
//
//  USB: the device enumerates as composite HID keyboard + CDC serial.
//  HID types the UID + Tab each scan.  CDC serial is used by the Tauri app.
//
//  Serial protocol (newline-terminated commands from PC):
//    STATUS           → {"event":"status","hid":true/false}
//    MODE:HID         → enable HID keyboard output
//    MODE:APP         → disable HID keyboard output
//    READ_ALL         → dump all NTAG/Ultralight pages
//    READ_PAGE:n      → read single page n
//    WRITE_PAGE:n:HH  → write 4 bytes (8 hex chars) to NTAG page n
//    AUTH:s:A/B:key   → auth MIFARE Classic sector s with 6-byte key (12 hex)
//    READ_BLOCK:n     → read MIFARE Classic block n (must auth first)
//    WRITE_BLOCK:n:HH → write 16 bytes (32 hex) to MIFARE Classic block n

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include "USB.h"
#include "USBHIDKeyboard.h"

#define SDA_PIN    8
#define SCL_PIN    9
#define PN532_IRQ  4

Adafruit_PN532 nfc(PN532_IRQ, -1);  // RST tied to 3.3V on breakout
USBHIDKeyboard Keyboard;
// Serial = USB CDC (ARDUINO_USB_CDC_ON_BOOT=1 wires it automatically)

// ───── helpers ──────────────────────────────────────────────────────────────

static String bytesToHex(const uint8_t *buf, uint8_t len) {
    String s;
    for (uint8_t i = 0; i < len; i++) {
        if (buf[i] < 0x10) s += '0';
        s += String(buf[i], HEX);
    }
    s.toUpperCase();
    return s;
}

static bool hexToBytes(const String &hex, uint8_t *buf, uint8_t len) {
    if ((int)hex.length() != len * 2) return false;
    for (uint8_t i = 0; i < len; i++)
        buf[i] = (uint8_t)strtol(hex.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
    return true;
}

static bool sameUID(const uint8_t *uid, uint8_t uidLen,
                    const uint8_t *last, uint8_t lastLen) {
    if (uidLen != lastLen) return false;
    return memcmp(uid, last, uidLen) == 0;
}

// ───── tag type ─────────────────────────────────────────────────────────────

enum TagType { TAG_UNKNOWN, TAG_MIFARE_CLASSIC, TAG_ULTRALIGHT,
               TAG_NTAG213, TAG_NTAG215, TAG_NTAG216 };

static TagType detectType(uint8_t *uid, uint8_t uidLen) {
    if (uidLen == 4) return TAG_MIFARE_CLASSIC;
    // Read capability container (page 3) to distinguish NTAG vs Ultralight
    uint8_t cc[4];
    if (!nfc.mifareultralight_ReadPage(3, cc)) return TAG_UNKNOWN;
    switch (cc[2]) {
        case 0x12: return TAG_NTAG213;
        case 0x3E: return TAG_NTAG215;
        case 0x6D: return TAG_NTAG216;
        default:   return TAG_ULTRALIGHT;
    }
}

static const char *typeName(TagType t) {
    switch (t) {
        case TAG_MIFARE_CLASSIC: return "MIFARE_CLASSIC";
        case TAG_ULTRALIGHT:     return "MIFARE_ULTRALIGHT";
        case TAG_NTAG213:        return "NTAG213";
        case TAG_NTAG215:        return "NTAG215";
        case TAG_NTAG216:        return "NTAG216";
        default:                 return "UNKNOWN";
    }
}

static uint8_t pageCount(TagType t) {
    switch (t) {
        case TAG_NTAG213: return 45;
        case TAG_NTAG215: return 135;
        case TAG_NTAG216: return 231;
        case TAG_ULTRALIGHT: return 16;
        default: return 0;
    }
}

// ───── state ────────────────────────────────────────────────────────────────

static bool     hidEnabled = true;
static bool     tagPresent = false;
static uint8_t  lastUID[7];
static uint8_t  lastUIDLen = 0;
static TagType  currentType = TAG_UNKNOWN;

// ───── operations ────────────────────────────────────────────────────────────

static void opReadAll() {
    if (!tagPresent) { Serial.println(F("{\"event\":\"error\",\"msg\":\"No tag\"}")); return; }
    if (currentType == TAG_MIFARE_CLASSIC) {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Use AUTH+READ_BLOCK for MIFARE Classic\"}"));
        return;
    }
    uint8_t pages = pageCount(currentType);
    if (!pages) { Serial.println(F("{\"event\":\"error\",\"msg\":\"Unknown tag type\"}")); return; }

    Serial.println("{\"event\":\"read_start\",\"pages\":" + String(pages) + "}");
    uint8_t buf[4];
    for (uint8_t p = 0; p < pages; p++) {
        if (nfc.mifareultralight_ReadPage(p, buf))
            Serial.println("{\"event\":\"page\",\"page\":" + String(p) +
                              ",\"data\":\"" + bytesToHex(buf, 4) + "\"}");
        else
            Serial.println("{\"event\":\"page\",\"page\":" + String(p) + ",\"error\":true}");
    }
    Serial.println(F("{\"event\":\"read_done\"}"));
}

static void opReadPage(uint8_t page) {
    uint8_t buf[4];
    if (nfc.mifareultralight_ReadPage(page, buf))
        Serial.println("{\"event\":\"page\",\"page\":" + String(page) +
                          ",\"data\":\"" + bytesToHex(buf, 4) + "\"}");
    else
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Read page failed\"}"));
}

static void opWritePage(uint8_t page, const String &hex) {
    if (page < 4) {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Pages 0-3 are system pages — write blocked\"}"));
        return;
    }
    uint8_t total = pageCount(currentType);
    if (total > 0 && page >= total - 5) {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Config pages are protected — write blocked\"}"));
        return;
    }
    uint8_t data[4];
    if (!hexToBytes(hex, data, 4)) {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Need exactly 8 hex chars\"}"));
        return;
    }
    if (nfc.ntag2xx_WritePage(page, data))
        Serial.println("{\"event\":\"ok\",\"msg\":\"Page written\",\"page\":" + String(page) + "}");
    else
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Write failed\"}"));
}

static void opAuth(uint8_t sector, uint8_t keyType, const String &keyHex) {
    uint8_t key[6];
    if (!hexToBytes(keyHex, key, 6)) {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Need exactly 12 hex chars for key\"}"));
        return;
    }
    uint8_t block = sector * 4;
    if (nfc.mifareclassic_AuthenticateBlock(lastUID, lastUIDLen, block, keyType, key))
        Serial.println("{\"event\":\"ok\",\"msg\":\"Auth ok\",\"sector\":" + String(sector) + "}");
    else
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Auth failed — wrong key?\"}"));
}

static void opReadBlock(uint8_t block) {
    uint8_t buf[16];
    if (nfc.mifareclassic_ReadDataBlock(block, buf))
        Serial.println("{\"event\":\"block\",\"block\":" + String(block) +
                          ",\"data\":\"" + bytesToHex(buf, 16) + "\"}");
    else
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Read block failed (auth first?)\"}"));
}

static void opWriteBlock(uint8_t block, const String &hex) {
    if (block == 0) {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Block 0 is manufacturer data — write blocked\"}"));
        return;
    }
    if ((block % 4) == 3) {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Sector trailer blocks are protected — write blocked\"}"));
        return;
    }
    uint8_t data[16];
    if (!hexToBytes(hex, data, 16)) {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Need exactly 32 hex chars\"}"));
        return;
    }
    if (nfc.mifareclassic_WriteDataBlock(block, data))
        Serial.println("{\"event\":\"ok\",\"msg\":\"Block written\",\"block\":" + String(block) + "}");
    else
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Write failed\"}"));
}

// ───── command parser ────────────────────────────────────────────────────────

static void processCommand(String cmd) {
    cmd.trim();
    if (!cmd.length()) return;

    if (cmd == "STATUS") {
        Serial.println("{\"event\":\"status\",\"hid\":" +
                          String(hidEnabled ? "true" : "false") + "}");
    } else if (cmd == "MODE:HID") {
        hidEnabled = true;
        Serial.println(F("{\"event\":\"ok\",\"mode\":\"hid\"}"));
    } else if (cmd == "MODE:APP") {
        hidEnabled = false;
        Serial.println(F("{\"event\":\"ok\",\"mode\":\"app\"}"));
    } else if (cmd == "READ_ALL") {
        opReadAll();
    } else if (cmd.startsWith("READ_PAGE:")) {
        opReadPage((uint8_t)cmd.substring(10).toInt());
    } else if (cmd.startsWith("WRITE_PAGE:")) {
        int sep = cmd.indexOf(':', 11);
        if (sep < 0) { Serial.println(F("{\"event\":\"error\",\"msg\":\"Syntax: WRITE_PAGE:page:hexdata\"}")); return; }
        opWritePage((uint8_t)cmd.substring(11, sep).toInt(), cmd.substring(sep + 1));
    } else if (cmd.startsWith("AUTH:")) {
        // AUTH:sector:A/B:hexkey
        int s1 = cmd.indexOf(':', 5);
        int s2 = s1 > 0 ? cmd.indexOf(':', s1 + 1) : -1;
        if (s1 < 0 || s2 < 0) { Serial.println(F("{\"event\":\"error\",\"msg\":\"Syntax: AUTH:sector:A/B:hexkey\"}")); return; }
        uint8_t sector  = (uint8_t)cmd.substring(5, s1).toInt();
        uint8_t keyType = (cmd.substring(s1 + 1, s2) == "B") ? 1 : 0;
        opAuth(sector, keyType, cmd.substring(s2 + 1));
    } else if (cmd.startsWith("READ_BLOCK:")) {
        opReadBlock((uint8_t)cmd.substring(11).toInt());
    } else if (cmd.startsWith("WRITE_BLOCK:")) {
        int sep = cmd.indexOf(':', 12);
        if (sep < 0) { Serial.println(F("{\"event\":\"error\",\"msg\":\"Syntax: WRITE_BLOCK:block:hexdata\"}")); return; }
        opWriteBlock((uint8_t)cmd.substring(12, sep).toInt(), cmd.substring(sep + 1));
    } else {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Unknown command\"}"));
    }
}

// ───── setup / loop ──────────────────────────────────────────────────────────

void setup() {
    Keyboard.begin();
    USB.begin();   // must be called after all USB class inits
    // Serial (USB CDC) auto-starts via ARDUINO_USB_CDC_ON_BOOT=1

    delay(500);

    Wire.begin(SDA_PIN, SCL_PIN);
    nfc.begin();

    uint32_t ver = nfc.getFirmwareVersion();
    if (!ver) {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"PN532 not found — check wiring\"}"));
        while (true) delay(1000);
    }

    nfc.SAMConfig();

    char buf[64];
    snprintf(buf, sizeof(buf), "{\"event\":\"ready\",\"fw\":\"%d.%d\"}",
             (int)((ver >> 16) & 0xFF), (int)((ver >> 8) & 0xFF));
    Serial.println(buf);
}

void loop() {
    // Drain serial commands
    while (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        processCommand(cmd);
    }

    // Poll for ISO14443A tag (50 ms timeout keeps loop responsive)
    uint8_t uid[7];
    uint8_t uidLen = 0;
    bool detected = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 50);

    if (detected) {
        if (!tagPresent || !sameUID(uid, uidLen, lastUID, lastUIDLen)) {
            tagPresent = true;
            memcpy(lastUID, uid, uidLen);
            lastUIDLen = uidLen;
            currentType = detectType(uid, uidLen);

            String uidStr = bytesToHex(uid, uidLen);

            if (hidEnabled) {
                Keyboard.print(uidStr);
                Keyboard.print("\t");  // Tab to next cell in Excel
            }

            String json = "{\"event\":\"tag\",\"uid\":\"" + uidStr +
                          "\",\"type\":\"" + typeName(currentType) + "\"";
            if (currentType == TAG_MIFARE_CLASSIC)
                json += ",\"sectors\":16";
            else
                json += ",\"pages\":" + String(pageCount(currentType));
            json += "}";
            Serial.println(json);
        }
    } else {
        if (tagPresent) {
            tagPresent  = false;
            lastUIDLen  = 0;
            currentType = TAG_UNKNOWN;
            Serial.println(F("{\"event\":\"tag_removed\"}"));
        }
    }
}
