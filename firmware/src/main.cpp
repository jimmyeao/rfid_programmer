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
//  RGB LED (WS2812) on GPIO48 shows operational state:
//    Booting          white dim, solid
//    Idle / APP mode  blue, slow breathe
//    Idle / HID mode  green, slow breathe
//    Tag present      white, solid bright
//    Reading          cyan, fast blink
//    Writing          amber, fast blink
//    Success          green flash (500 ms) → idle
//    Error            red flash (500 ms) → idle
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
#include <Adafruit_NeoPixel.h>
#include "USB.h"
#include "USBHIDKeyboard.h"

#define SDA_PIN    8
#define SCL_PIN    9
#define PN532_IRQ  4
#define LED_PIN    48

Adafruit_PN532     nfc(PN532_IRQ, -1);
Adafruit_NeoPixel  led(1, LED_PIN, NEO_GRB + NEO_KHZ800);
USBHIDKeyboard     Keyboard;

// ───── LED state machine ─────────────────────────────────────────────────────

enum LedMode {
    LED_BOOT,
    LED_IDLE,       // breathe; color depends on hidEnabled
    LED_TAG,        // solid white — tag on reader
    LED_READING,    // cyan fast blink
    LED_WRITING,    // amber fast blink
    LED_FLASH_OK,   // green 500 ms flash then back to LED_IDLE / LED_TAG
    LED_FLASH_ERR,  // red 500 ms flash then back to LED_IDLE / LED_TAG
};

static LedMode  ledMode      = LED_BOOT;
static LedMode  ledAfterFlash = LED_IDLE;
static uint32_t ledTimer     = 0;
static bool     ledBlinkOn   = false;

// Approximate breathing using a triangle wave on millis()
static uint8_t breathBrightness() {
    uint32_t t   = millis() % 3000;        // 3-second cycle
    uint32_t half = 1500;
    uint8_t  b   = (t < half) ? (uint8_t)(t * 60 / half)
                               : (uint8_t)((3000 - t) * 60 / half);
    return b + 10;                         // floor of 10 so it never fully off
}

static void ledUpdate() {
    uint32_t now = millis();

    switch (ledMode) {
        case LED_BOOT:
            led.setPixelColor(0, led.Color(20, 20, 20));
            break;

        case LED_IDLE: {
            uint8_t b = breathBrightness();
            // green in HID mode, blue in APP mode
            extern bool hidEnabled;
            if (hidEnabled)
                led.setPixelColor(0, led.Color(0, b, 0));
            else
                led.setPixelColor(0, led.Color(0, 0, b));
            break;
        }

        case LED_TAG:
            led.setPixelColor(0, led.Color(60, 60, 60));
            break;

        case LED_READING:
            if (now - ledTimer >= 120) {
                ledTimer   = now;
                ledBlinkOn = !ledBlinkOn;
            }
            led.setPixelColor(0, ledBlinkOn ? led.Color(0, 60, 60) : led.Color(0, 0, 0));
            break;

        case LED_WRITING:
            if (now - ledTimer >= 120) {
                ledTimer   = now;
                ledBlinkOn = !ledBlinkOn;
            }
            led.setPixelColor(0, ledBlinkOn ? led.Color(60, 40, 0) : led.Color(0, 0, 0));
            break;

        case LED_FLASH_OK:
            if (now - ledTimer >= 500) {
                ledMode = ledAfterFlash;
                return ledUpdate();
            }
            led.setPixelColor(0, led.Color(0, 80, 0));
            break;

        case LED_FLASH_ERR:
            if (now - ledTimer >= 500) {
                ledMode = ledAfterFlash;
                return ledUpdate();
            }
            led.setPixelColor(0, led.Color(80, 0, 0));
            break;
    }

    led.show();
}

static void ledSet(LedMode mode) {
    ledMode    = mode;
    ledTimer   = millis();
    ledBlinkOn = true;
    ledUpdate();
}

static void ledFlash(bool ok) {
    ledAfterFlash = (ledMode == LED_TAG) ? LED_TAG : LED_IDLE;
    ledSet(ok ? LED_FLASH_OK : LED_FLASH_ERR);
}

// ───── helpers ───────────────────────────────────────────────────────────────

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

// ───── tag type ──────────────────────────────────────────────────────────────

enum TagType { TAG_UNKNOWN, TAG_MIFARE_CLASSIC, TAG_ULTRALIGHT,
               TAG_NTAG213, TAG_NTAG215, TAG_NTAG216 };

static TagType detectType(uint8_t *uid, uint8_t uidLen) {
    if (uidLen == 4) return TAG_MIFARE_CLASSIC;
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

// ───── state ─────────────────────────────────────────────────────────────────

bool            hidEnabled  = true;
static bool     tagPresent  = false;
static uint8_t  lastUID[7];
static uint8_t  lastUIDLen  = 0;
static TagType  currentType = TAG_UNKNOWN;

// ───── operations ─────────────────────────────────────────────────────────────

static void opReadAll() {
    if (!tagPresent) {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"No tag\"}"));
        ledFlash(false);
        return;
    }
    if (currentType == TAG_MIFARE_CLASSIC) {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Use AUTH+READ_BLOCK for MIFARE Classic\"}"));
        ledFlash(false);
        return;
    }
    uint8_t pages = pageCount(currentType);
    if (!pages) {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Unknown tag type\"}"));
        ledFlash(false);
        return;
    }

    ledSet(LED_READING);

    Serial.println("{\"event\":\"read_start\",\"pages\":" + String(pages) + "}");
    uint8_t buf[4];
    for (uint8_t p = 0; p < pages; p++) {
        if (nfc.mifareultralight_ReadPage(p, buf))
            Serial.println("{\"event\":\"page\",\"page\":" + String(p) +
                              ",\"data\":\"" + bytesToHex(buf, 4) + "\"}");
        else
            Serial.println("{\"event\":\"page\",\"page\":" + String(p) + ",\"error\":true}");
        ledUpdate();
    }
    Serial.println(F("{\"event\":\"read_done\"}"));
    ledSet(LED_TAG);
}

static void opReadPage(uint8_t page) {
    uint8_t buf[4];
    if (nfc.mifareultralight_ReadPage(page, buf)) {
        Serial.println("{\"event\":\"page\",\"page\":" + String(page) +
                          ",\"data\":\"" + bytesToHex(buf, 4) + "\"}");
        ledFlash(true);
    } else {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Read page failed\"}"));
        ledFlash(false);
    }
}

static void opWritePage(uint8_t page, const String &hex) {
    if (page < 4) {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Pages 0-3 are system pages — write blocked\"}"));
        ledFlash(false);
        return;
    }
    uint8_t total = pageCount(currentType);
    if (total > 0 && page >= total - 5) {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Config pages are protected — write blocked\"}"));
        ledFlash(false);
        return;
    }
    uint8_t data[4];
    if (!hexToBytes(hex, data, 4)) {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Need exactly 8 hex chars\"}"));
        ledFlash(false);
        return;
    }

    ledSet(LED_WRITING);

    if (nfc.ntag2xx_WritePage(page, data)) {
        Serial.println("{\"event\":\"ok\",\"msg\":\"Page written\",\"page\":" + String(page) + "}");
        ledFlash(true);
    } else {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Write failed\"}"));
        ledFlash(false);
    }
}

static void opAuth(uint8_t sector, uint8_t keyType, const String &keyHex) {
    uint8_t key[6];
    if (!hexToBytes(keyHex, key, 6)) {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Need exactly 12 hex chars for key\"}"));
        ledFlash(false);
        return;
    }
    uint8_t block = sector * 4;
    if (nfc.mifareclassic_AuthenticateBlock(lastUID, lastUIDLen, block, keyType, key)) {
        Serial.println("{\"event\":\"ok\",\"msg\":\"Auth ok\",\"sector\":" + String(sector) + "}");
        ledFlash(true);
    } else {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Auth failed — wrong key?\"}"));
        ledFlash(false);
    }
}

static void opReadBlock(uint8_t block) {
    uint8_t buf[16];
    if (nfc.mifareclassic_ReadDataBlock(block, buf)) {
        Serial.println("{\"event\":\"block\",\"block\":" + String(block) +
                          ",\"data\":\"" + bytesToHex(buf, 16) + "\"}");
        ledFlash(true);
    } else {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Read block failed (auth first?)\"}"));
        ledFlash(false);
    }
}

static void opWriteBlock(uint8_t block, const String &hex) {
    if (block == 0) {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Block 0 is manufacturer data — write blocked\"}"));
        ledFlash(false);
        return;
    }
    if ((block % 4) == 3) {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Sector trailer blocks are protected — write blocked\"}"));
        ledFlash(false);
        return;
    }
    uint8_t data[16];
    if (!hexToBytes(hex, data, 16)) {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Need exactly 32 hex chars\"}"));
        ledFlash(false);
        return;
    }

    ledSet(LED_WRITING);

    if (nfc.mifareclassic_WriteDataBlock(block, data)) {
        Serial.println("{\"event\":\"ok\",\"msg\":\"Block written\",\"block\":" + String(block) + "}");
        ledFlash(true);
    } else {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Write failed\"}"));
        ledFlash(false);
    }
}

// ───── command parser ─────────────────────────────────────────────────────────

static void processCommand(String cmd) {
    cmd.trim();
    if (!cmd.length()) return;

    if (cmd == "STATUS") {
        Serial.println("{\"event\":\"status\",\"hid\":" +
                          String(hidEnabled ? "true" : "false") + "}");
    } else if (cmd == "MODE:HID") {
        hidEnabled = true;
        ledSet(tagPresent ? LED_TAG : LED_IDLE);
        Serial.println(F("{\"event\":\"ok\",\"mode\":\"hid\"}"));
    } else if (cmd == "MODE:APP") {
        hidEnabled = false;
        ledSet(tagPresent ? LED_TAG : LED_IDLE);
        Serial.println(F("{\"event\":\"ok\",\"mode\":\"app\"}"));
    } else if (cmd == "READ_ALL") {
        opReadAll();
    } else if (cmd.startsWith("READ_PAGE:")) {
        opReadPage((uint8_t)cmd.substring(10).toInt());
    } else if (cmd.startsWith("WRITE_PAGE:")) {
        int sep = cmd.indexOf(':', 11);
        if (sep < 0) { Serial.println(F("{\"event\":\"error\",\"msg\":\"Syntax: WRITE_PAGE:page:hexdata\"}")); ledFlash(false); return; }
        opWritePage((uint8_t)cmd.substring(11, sep).toInt(), cmd.substring(sep + 1));
    } else if (cmd.startsWith("AUTH:")) {
        int s1 = cmd.indexOf(':', 5);
        int s2 = s1 > 0 ? cmd.indexOf(':', s1 + 1) : -1;
        if (s1 < 0 || s2 < 0) { Serial.println(F("{\"event\":\"error\",\"msg\":\"Syntax: AUTH:sector:A/B:hexkey\"}")); ledFlash(false); return; }
        uint8_t sector  = (uint8_t)cmd.substring(5, s1).toInt();
        uint8_t keyType = (cmd.substring(s1 + 1, s2) == "B") ? 1 : 0;
        opAuth(sector, keyType, cmd.substring(s2 + 1));
    } else if (cmd.startsWith("READ_BLOCK:")) {
        opReadBlock((uint8_t)cmd.substring(11).toInt());
    } else if (cmd.startsWith("WRITE_BLOCK:")) {
        int sep = cmd.indexOf(':', 12);
        if (sep < 0) { Serial.println(F("{\"event\":\"error\",\"msg\":\"Syntax: WRITE_BLOCK:block:hexdata\"}")); ledFlash(false); return; }
        opWriteBlock((uint8_t)cmd.substring(12, sep).toInt(), cmd.substring(sep + 1));
    } else {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"Unknown command\"}"));
        ledFlash(false);
    }
}

// ───── setup / loop ───────────────────────────────────────────────────────────

void setup() {
    led.begin();
    led.setBrightness(255);
    ledSet(LED_BOOT);

    Keyboard.begin();
    USB.begin();

    delay(500);

    Wire.begin(SDA_PIN, SCL_PIN);
    nfc.begin();

    uint32_t ver = nfc.getFirmwareVersion();
    if (!ver) {
        Serial.println(F("{\"event\":\"error\",\"msg\":\"PN532 not found — check wiring\"}"));
        // Rapid red blink to signal hardware fault
        while (true) {
            led.setPixelColor(0, led.Color(80, 0, 0)); led.show(); delay(200);
            led.setPixelColor(0, led.Color(0,  0, 0)); led.show(); delay(200);
        }
    }

    nfc.SAMConfig();

    char buf[64];
    snprintf(buf, sizeof(buf), "{\"event\":\"ready\",\"fw\":\"%d.%d\"}",
             (int)((ver >> 16) & 0xFF), (int)((ver >> 8) & 0xFF));
    Serial.println(buf);

    ledSet(LED_IDLE);
}

void loop() {
    while (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        processCommand(cmd);
    }

    uint8_t uid[7];
    uint8_t uidLen = 0;
    bool detected = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 50);

    if (detected) {
        if (!tagPresent || !sameUID(uid, uidLen, lastUID, lastUIDLen)) {
            tagPresent = true;
            memcpy(lastUID, uid, uidLen);
            lastUIDLen  = uidLen;
            currentType = detectType(uid, uidLen);

            String uidStr = bytesToHex(uid, uidLen);

            if (hidEnabled) {
                Keyboard.print(uidStr);
                Keyboard.print("\t");
            }

            String json = "{\"event\":\"tag\",\"uid\":\"" + uidStr +
                          "\",\"type\":\"" + typeName(currentType) + "\"";
            if (currentType == TAG_MIFARE_CLASSIC)
                json += ",\"sectors\":16";
            else
                json += ",\"pages\":" + String(pageCount(currentType));
            json += "}";
            Serial.println(json);

            ledSet(LED_TAG);
        }
    } else {
        if (tagPresent) {
            tagPresent  = false;
            lastUIDLen  = 0;
            currentType = TAG_UNKNOWN;
            Serial.println(F("{\"event\":\"tag_removed\"}"));
            ledSet(LED_IDLE);
        }
    }

    ledUpdate();
}
