/**
 * QIDI Box NFC Tool
 * ESP32-S3 + PN532 (I2C, Generic V3 Module)
 *
 * Continuously reads NFC tags and displays their data via a web UI.
 * Also supports writing QIDI filament config (material + color) to tags.
 * Onboard RGB LED (GPIO 48) provides visual status feedback.
 *
 * Libraries required:
 *   - Adafruit PN532
 *   - Adafruit BusIO (dependency)
 *   - ESPAsyncWebServer
 *   - AsyncTCP
 *
 * Wiring (I2C mode - set DIP SW1=ON, SW2=OFF on PN532):
 *   PN532 VCC  → 3.3V
 *   PN532 GND  → GND
 *   PN532 SDA  → GPIO 8
 *   PN532 SCL  → GPIO 9
 */

#include <Wire.h>
#include <Adafruit_PN532.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// ─── CONFIG ──────────────────────────────────────────────────────────────────

#define SDA_PIN 8
#define SCL_PIN 9
#define RGB_LED 48
#define BOOT_PIN 0        // BOOT button on ESP32-S3-DevKitC-1

#define AP_SSID "QidiBox-Setup"
#define NVS_NAMESPACE "qidibox"

// ─── NFC ─────────────────────────────────────────────────────────────────────

Adafruit_PN532 nfc(SDA_PIN, SCL_PIN);

uint8_t keyA[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const uint8_t QIDI_BLOCK = 4;

// ─── TAG STATE ───────────────────────────────────────────────────────────────

// Max data we'll read: Classic=16 blocks*16B=256, UL/NTAG=45 pages*4B=180
#define MAX_TAG_DATA 256
#define CLASSIC_BLOCKS_TO_READ 16   // blocks 0-15 (sectors 0-3)
#define UL_PAGES_TO_READ 44         // pages 0-43 (covers UL + NTAG213)

enum CardType : uint8_t {
  CARD_UNKNOWN = 0,
  CARD_MIFARE_CLASSIC,
  CARD_MIFARE_ULTRALIGHT
};

struct TagData {
  bool valid;
  uint8_t uid[7];
  uint8_t uidLength;
  CardType cardType;
  uint8_t data[MAX_TAG_DATA];
  uint16_t dataLen;           // actual bytes read
  uint8_t pagesRead;          // for UL/NTAG: how many pages succeeded
  uint8_t blocksRead;         // for Classic: how many blocks succeeded
  bool isQidiFormat;
  bool authFailed;
  bool readFailed;
  unsigned long timestamp;
};

const char* cardTypeName(CardType ct) {
  switch (ct) {
    case CARD_MIFARE_CLASSIC:    return "MIFARE Classic";
    case CARD_MIFARE_ULTRALIGHT: return "MIFARE Ultralight / NTAG";
    default:                     return "Unknown";
  }
}

CardType detectCardType(uint8_t uidLen) {
  switch (uidLen) {
    case 4:  return CARD_MIFARE_CLASSIC;
    case 7:  return CARD_MIFARE_ULTRALIGHT;
    default: return CARD_UNKNOWN;
  }
}

TagData lastTag = {};
bool nfcBusy = false;

// Track previous UID to detect remove-and-re-present
uint8_t prevUid[7] = {0};
uint8_t prevUidLen = 0;
bool tagPresent = false;  // was a tag seen on the previous poll?

// LED state management
unsigned long ledGreenUntil = 0;
unsigned long ledRedUntil = 0;

// ─── WEB SERVER & WIFI ───────────────────────────────────────────────────────

AsyncWebServer server(80);
DNSServer dnsServer;
Preferences prefs;
bool apMode = false;       // true when running in AP/setup mode
String storedSSID;
String storedPass;

// ─── LED HELPERS ─────────────────────────────────────────────────────────────

void ledOff()    { neopixelWrite(RGB_LED, 0, 0, 0); }
void ledBlue()   { neopixelWrite(RGB_LED, 0, 0, 40); }
void ledGreen()  { neopixelWrite(RGB_LED, 0, 40, 0); }
void ledYellow() { neopixelWrite(RGB_LED, 40, 30, 0); }
void ledRed()    { neopixelWrite(RGB_LED, 40, 0, 0); }
void ledPurple() { neopixelWrite(RGB_LED, 30, 0, 40); }

// ─── EMBEDDED WEB FILES ─────────────────────────────────────────────────────
// These are embedded at compile time via board_build.embed_txtfiles

extern const char index_html_start[] asm("_binary_data_index_html_start");
extern const char style_css_start[]  asm("_binary_data_style_css_start");
extern const char app_js_start[]     asm("_binary_data_app_js_start");
extern const char setup_html_start[] asm("_binary_data_setup_html_start");

// ─── SETUP ───────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== QIDI NFC Tool ===");

  ledOff();

  // --- PN532 init ---
  Wire.begin(SDA_PIN, SCL_PIN);
  nfc.begin();

  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("ERROR: PN532 not found. Check wiring and DIP switches.");
    while (1) {
      ledRed();
      delay(500);
      ledOff();
      delay(500);
    }
  }

  Serial.printf("PN532 found. Chip: PN5%02X, Firmware: %d.%d\n",
    (versiondata >> 24) & 0xFF,
    (versiondata >> 16) & 0xFF,
    (versiondata >>  8) & 0xFF);

  nfc.SAMConfig();

  // --- BOOT button check: hold to reset WiFi ---
  pinMode(BOOT_PIN, INPUT_PULLUP);
  delay(100);
  if (digitalRead(BOOT_PIN) == LOW) {
    Serial.println("BOOT button held — clearing WiFi credentials...");
    prefs.begin(NVS_NAMESPACE, false);
    prefs.remove("ssid");
    prefs.remove("pass");
    prefs.end();
    // Flash red/purple to confirm reset
    for (int i = 0; i < 6; i++) {
      ledPurple(); delay(200); ledOff(); delay(200);
    }
  }

  // --- Load WiFi credentials from NVS ---
  prefs.begin(NVS_NAMESPACE, true); // read-only
  storedSSID = prefs.getString("ssid", "");
  storedPass = prefs.getString("pass", "");
  prefs.end();

  if (storedSSID.length() == 0) {
    // No credentials — start AP mode for setup
    apMode = true;
    Serial.println("No WiFi credentials found. Starting setup AP...");
    ledPurple();

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID);
    delay(500);
    Serial.printf("AP started: %s\nSetup URL: http://%s\n",
      AP_SSID, WiFi.softAPIP().toString().c_str());

    // Captive portal DNS — redirect all domains to our IP
    dnsServer.start(53, "*", WiFi.softAPIP());

    // Setup-mode routes
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
      req->send(200, "text/html", setup_html_start);
    });

    server.on("/wifi-save", HTTP_POST, [](AsyncWebServerRequest* req) {
      if (!req->hasParam("ssid", true)) {
        req->send(400, "text/plain", "SSID is required.");
        return;
      }
      String newSSID = req->getParam("ssid", true)->value();
      String newPass = req->hasParam("pass", true) ? req->getParam("pass", true)->value() : "";

      if (newSSID.length() == 0 || newSSID.length() > 32) {
        req->send(400, "text/plain", "SSID must be 1-32 characters.");
        return;
      }

      prefs.begin(NVS_NAMESPACE, false);
      prefs.putString("ssid", newSSID);
      prefs.putString("pass", newPass);
      prefs.end();

      Serial.printf("WiFi credentials saved: %s\n", newSSID.c_str());
      req->send(200, "text/plain",
        "WiFi credentials saved! The device will restart in 3 seconds...");

      // Restart after a brief delay to let the response send
      delay(3000);
      ESP.restart();
    });

    // Captive portal: redirect all other requests to setup page
    server.onNotFound([](AsyncWebServerRequest* req) {
      req->redirect("http://" + WiFi.softAPIP().toString());
    });

    server.begin();
    Serial.println("Setup server started. Connect to 'QidiBox-Setup' WiFi.");
    return; // Don't proceed to normal operation
  }

  // --- Normal WiFi connection ---
  ledYellow();
  WiFi.mode(WIFI_STA);
  Serial.printf("Connecting to %s", storedSSID.c_str());
  WiFi.begin(storedSSID.c_str(), storedPass.c_str());

  // Wait up to 15 seconds for connection
  unsigned long wifiDeadline = millis() + 15000;
  while (WiFi.status() != WL_CONNECTED && millis() < wifiDeadline) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi connection failed! Starting setup AP...");
    // Fall back to AP mode so user can reconfigure
    WiFi.disconnect();
    apMode = true;
    ledPurple();

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID);
    delay(500);
    Serial.printf("AP started: %s\nSetup URL: http://%s\n",
      AP_SSID, WiFi.softAPIP().toString().c_str());

    dnsServer.start(53, "*", WiFi.softAPIP());

    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
      req->send(200, "text/html", setup_html_start);
    });
    server.on("/wifi-save", HTTP_POST, [](AsyncWebServerRequest* req) {
      if (!req->hasParam("ssid", true)) {
        req->send(400, "text/plain", "SSID is required.");
        return;
      }
      String newSSID = req->getParam("ssid", true)->value();
      String newPass = req->hasParam("pass", true) ? req->getParam("pass", true)->value() : "";

      prefs.begin(NVS_NAMESPACE, false);
      prefs.putString("ssid", newSSID);
      prefs.putString("pass", newPass);
      prefs.end();

      req->send(200, "text/plain",
        "WiFi credentials saved! The device will restart in 3 seconds...");
      delay(3000);
      ESP.restart();
    });
    server.onNotFound([](AsyncWebServerRequest* req) {
      req->redirect("http://" + WiFi.softAPIP().toString());
    });
    server.begin();
    return;
  }

  Serial.printf("\nConnected! IP: http://%s\n", WiFi.localIP().toString().c_str());
  ledBlue();

  // --- Routes ---

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/html", index_html_start);
  });

  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/css", style_css_start);
  });

  server.on("/app.js", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "application/javascript", app_js_start);
  });

  // WiFi reconfigure endpoint (accessible from main UI)
  server.on("/wifi-setup", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/html", setup_html_start);
  });

  server.on("/wifi-save", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("ssid", true)) {
      req->send(400, "text/plain", "SSID is required.");
      return;
    }
    String newSSID = req->getParam("ssid", true)->value();
    String newPass = req->hasParam("pass", true) ? req->getParam("pass", true)->value() : "";

    if (newSSID.length() == 0 || newSSID.length() > 32) {
      req->send(400, "text/plain", "SSID must be 1-32 characters.");
      return;
    }

    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString("ssid", newSSID);
    prefs.putString("pass", newPass);
    prefs.end();

    Serial.printf("WiFi credentials updated: %s\n", newSSID.c_str());
    req->send(200, "text/plain",
      "WiFi credentials saved! The device will restart in 3 seconds...");
    delay(3000);
    ESP.restart();
  });

  // Status endpoint: returns last-read tag data as JSON
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!lastTag.valid) {
      req->send(200, "application/json", "{\"tagPresent\":false,\"timestamp\":0}");
      return;
    }

    char uidStr[22];
    int uidPos = 0;
    for (int i = 0; i < lastTag.uidLength; i++) {
      uidPos += snprintf(uidStr + uidPos, sizeof(uidStr) - uidPos,
                         "%s%02X", i ? ":" : "", lastTag.uid[i]);
    }

    // Build hex dump of all data read
    // Each byte = "XX " (3 chars), max 256 bytes = 768 + null
    char hexStr[800];
    int hexPos = 0;
    for (int i = 0; i < lastTag.dataLen && hexPos < (int)sizeof(hexStr) - 4; i++) {
      hexPos += snprintf(hexStr + hexPos, sizeof(hexStr) - hexPos,
                         "%s%02X", i ? " " : "", lastTag.data[i]);
    }
    if (lastTag.dataLen == 0) hexStr[0] = '\0';

    // QIDI data lives at block 4 offset in Classic
    uint8_t material = 0, color = 0, manufacturer = 0;
    if (lastTag.cardType == CARD_MIFARE_CLASSIC && lastTag.dataLen >= (QIDI_BLOCK + 1) * 16) {
      material     = lastTag.data[QIDI_BLOCK * 16];
      color        = lastTag.data[QIDI_BLOCK * 16 + 1];
      manufacturer = lastTag.data[QIDI_BLOCK * 16 + 2];
    }

    // JSON buffer: fixed fields ~200 + hex ~768 = ~1000
    char* json = (char*)malloc(1200);
    if (!json) {
      req->send(500, "text/plain", "Out of memory");
      return;
    }

    snprintf(json, 1200,
      "{\"tagPresent\":true,"
      "\"uid\":\"%s\","
      "\"cardType\":\"%s\","
      "\"cardTypeId\":%d,"
      "\"isQidiFormat\":%s,"
      "\"material\":%d,"
      "\"color\":%d,"
      "\"manufacturer\":%d,"
      "\"dataLen\":%d,"
      "\"blocksRead\":%d,"
      "\"pagesRead\":%d,"
      "\"raw\":\"%s\","
      "\"authFailed\":%s,"
      "\"readFailed\":%s,"
      "\"timestamp\":%lu}",
      uidStr,
      cardTypeName(lastTag.cardType),
      (int)lastTag.cardType,
      lastTag.isQidiFormat ? "true" : "false",
      material, color, manufacturer,
      lastTag.dataLen,
      lastTag.blocksRead,
      lastTag.pagesRead,
      hexStr,
      lastTag.authFailed ? "true" : "false",
      lastTag.readFailed ? "true" : "false",
      lastTag.timestamp);

    req->send(200, "application/json", json);
    free(json);
  });

  // Write endpoint: /write?material=X&color=Y
  server.on("/write", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("material") || !req->hasParam("color")) {
      req->send(400, "text/plain", "Missing material or color parameter.");
      return;
    }

    uint8_t material = req->getParam("material")->value().toInt();
    uint8_t color    = req->getParam("color")->value().toInt();

    if (material < 1 || material > 50) {
      req->send(400, "text/plain", "Material code out of range (1-50).");
      return;
    }
    if (color < 1 || color > 24) {
      req->send(400, "text/plain", "Color code out of range (1-24).");
      return;
    }

    Serial.printf("Write request — Material: %d, Color: %d\n", material, color);

    nfcBusy = true;
    ledYellow();

    uint8_t uid[7];
    uint8_t uidLength;
    bool found = false;

    unsigned long deadline = millis() + 10000;
    while (millis() < deadline) {
      if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 500)) {
        found = true;
        break;
      }
    }

    if (!found) {
      ledRed();
      ledRedUntil = millis() + 2000;
      nfcBusy = false;
      req->send(408, "text/plain", "No tag detected within 10 seconds. Try again.");
      return;
    }

    Serial.print("Tag UID: ");
    for (int i = 0; i < uidLength; i++) {
      Serial.printf("%02X ", uid[i]);
    }
    CardType ct = detectCardType(uidLength);
    Serial.printf("(%s)\n", cardTypeName(ct));

    // Only MIFARE Classic (4-byte UID) supports sector-based write
    if (ct != CARD_MIFARE_CLASSIC) {
      ledRed();
      ledRedUntil = millis() + 2000;
      nfcBusy = false;
      char errMsg[128];
      snprintf(errMsg, sizeof(errMsg),
        "Wrong card type: %s. Only MIFARE Classic 1K tags are supported for writing.",
        cardTypeName(ct));
      req->send(400, "text/plain", errMsg);
      return;
    }

    if (!nfc.mifareclassic_AuthenticateBlock(uid, uidLength, QIDI_BLOCK, 0, keyA)) {
      ledRed();
      ledRedUntil = millis() + 2000;
      nfcBusy = false;
      req->send(500, "text/plain",
        "Authentication failed. Tag may use non-default keys or is not MIFARE Classic 1K.");
      return;
    }

    uint8_t data[16] = {0};
    data[0] = material;
    data[1] = color;
    data[2] = 0x01;

    if (!nfc.mifareclassic_WriteDataBlock(QIDI_BLOCK, data)) {
      ledRed();
      ledRedUntil = millis() + 2000;
      nfcBusy = false;
      req->send(500, "text/plain", "Write failed. Tag may be locked or damaged.");
      return;
    }

    uint8_t readback[16];
    if (!nfc.mifareclassic_ReadDataBlock(QIDI_BLOCK, readback)) {
      ledRed();
      ledRedUntil = millis() + 2000;
      nfcBusy = false;
      req->send(500, "text/plain",
        "Write succeeded but verification read failed.");
      return;
    }

    if (readback[0] != material || readback[1] != color || readback[2] != 0x01) {
      ledRed();
      ledRedUntil = millis() + 2000;
      nfcBusy = false;
      req->send(500, "text/plain",
        "Verification failed — data mismatch after write.");
      return;
    }

    // Update lastTag so the UI reflects the write immediately
    memset(&lastTag, 0, sizeof(lastTag));
    lastTag.valid = true;
    memcpy(lastTag.uid, uid, uidLength);
    lastTag.uidLength = uidLength;
    lastTag.cardType = CARD_MIFARE_CLASSIC;
    memcpy(lastTag.data + (QIDI_BLOCK * 16), readback, 16);
    lastTag.dataLen = CLASSIC_BLOCKS_TO_READ * 16;
    lastTag.blocksRead = 1;
    lastTag.isQidiFormat = true;
    lastTag.timestamp = millis();

    // Mark tag as present so loop() won't re-read it
    tagPresent = true;
    prevUidLen = uidLength;
    memcpy(prevUid, uid, uidLength);

    ledGreen();
    ledGreenUntil = millis() + 3000;
    nfcBusy = false;

    char msg[96];
    snprintf(msg, sizeof(msg),
      "Tag written and verified! Material: 0x%02X  Color: 0x%02X  Mfr: 0x01",
      material, color);

    Serial.println(msg);
    req->send(200, "text/plain", msg);
  });

  // Clear endpoint: zeros out writable data on the tag
  server.on("/clear", HTTP_GET, [](AsyncWebServerRequest* req) {
    Serial.println("Clear request received");

    nfcBusy = true;
    ledYellow();

    uint8_t uid[7];
    uint8_t uidLength;
    bool found = false;

    unsigned long deadline = millis() + 10000;
    while (millis() < deadline) {
      if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 500)) {
        found = true;
        break;
      }
    }

    if (!found) {
      ledRed(); ledRedUntil = millis() + 2000;
      nfcBusy = false;
      req->send(408, "text/plain", "No tag detected within 10 seconds.");
      return;
    }

    CardType ct = detectCardType(uidLength);
    uint8_t zeros[16] = {0};

    if (ct == CARD_MIFARE_CLASSIC) {
      // Clear blocks 4-6 in sector 1 (skip block 7 = sector trailer!)
      if (!nfc.mifareclassic_AuthenticateBlock(uid, uidLength, QIDI_BLOCK, 0, keyA)) {
        ledRed(); ledRedUntil = millis() + 2000;
        nfcBusy = false;
        req->send(500, "text/plain", "Authentication failed.");
        return;
      }
      bool ok = true;
      for (uint8_t b = 4; b <= 6; b++) {
        if (!nfc.mifareclassic_WriteDataBlock(b, zeros)) {
          ok = false;
          break;
        }
      }
      if (!ok) {
        ledRed(); ledRedUntil = millis() + 2000;
        nfcBusy = false;
        req->send(500, "text/plain", "Clear failed — could not write to block.");
        return;
      }
      // Update lastTag
      memset(&lastTag, 0, sizeof(lastTag));
      lastTag.valid = true;
      memcpy(lastTag.uid, uid, uidLength);
      lastTag.uidLength = uidLength;
      lastTag.cardType = ct;
      lastTag.dataLen = CLASSIC_BLOCKS_TO_READ * 16;
      lastTag.blocksRead = 3;
      lastTag.timestamp = millis();

    } else if (ct == CARD_MIFARE_ULTRALIGHT) {
      // Clear user data pages (4+). Pages 0-3 are UID/lock/OTP — don't touch.
      uint8_t zeroPage[4] = {0};
      uint8_t cleared = 0;
      for (uint8_t page = 4; page < UL_PAGES_TO_READ; page++) {
        if (!nfc.mifareultralight_WritePage(page, zeroPage)) {
          break; // Reached end of writable area
        }
        cleared++;
      }
      if (cleared == 0) {
        ledRed(); ledRedUntil = millis() + 2000;
        nfcBusy = false;
        req->send(500, "text/plain", "Clear failed — could not write to any page.");
        return;
      }
      memset(&lastTag, 0, sizeof(lastTag));
      lastTag.valid = true;
      memcpy(lastTag.uid, uid, uidLength);
      lastTag.uidLength = uidLength;
      lastTag.cardType = ct;
      lastTag.pagesRead = cleared + 4; // include header pages conceptually
      lastTag.dataLen = lastTag.pagesRead * 4;
      lastTag.timestamp = millis();

      Serial.printf("Cleared %d UL/NTAG pages\n", cleared);
    } else {
      nfcBusy = false;
      req->send(400, "text/plain", "Unsupported card type for clearing.");
      return;
    }

    // Mark tag as present so loop() won't re-read it
    tagPresent = true;
    prevUidLen = uidLength;
    memcpy(prevUid, uid, uidLength);

    ledGreen(); ledGreenUntil = millis() + 3000;
    nfcBusy = false;
    req->send(200, "text/plain", "Tag cleared successfully.");
  });

  server.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "text/plain", "Not found.");
  });

  server.begin();
  Serial.println("Web server started. Open the IP above in your browser.");
}

// ─── LOOP ────────────────────────────────────────────────────────────────────

void loop() {
  // In AP mode, just process DNS for captive portal
  if (apMode) {
    dnsServer.processNextRequest();
    return;
  }

  // Handle LED timeout (return to blue after green/red flash)
  if (ledGreenUntil && millis() > ledGreenUntil) {
    ledGreenUntil = 0;
    ledBlue();
  }
  if (ledRedUntil && millis() > ledRedUntil) {
    ledRedUntil = 0;
    ledBlue();
  }

  // Don't poll NFC while a write/clear is in progress
  if (nfcBusy) return;

  // Poll for a tag (500ms timeout)
  uint8_t uid[7];
  uint8_t uidLength;

  if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 500)) {
    // No tag found — mark as absent so next detection is treated as new
    tagPresent = false;
    return;
  }

  // Check if this is the same tag that's still sitting on the reader
  bool sameTag = tagPresent &&
                 uidLength == prevUidLen &&
                 memcmp(uid, prevUid, uidLength) == 0;

  if (sameTag) {
    return; // Same tag still present, don't re-read
  }

  // New tag (or tag removed and re-placed) — store as current
  tagPresent = true;
  prevUidLen = uidLength;
  memcpy(prevUid, uid, uidLength);

  // Tag detected
  ledYellow();

  Serial.print("Tag detected — UID: ");
  for (int i = 0; i < uidLength; i++) {
    Serial.printf("%02X ", uid[i]);
  }
  Serial.println();

  TagData tag = {};
  tag.valid = true;
  memcpy(tag.uid, uid, uidLength);
  tag.uidLength = uidLength;
  tag.cardType = detectCardType(uidLength);
  tag.timestamp = millis();

  Serial.printf("Card type: %s\n", cardTypeName(tag.cardType));

  if (tag.cardType == CARD_MIFARE_CLASSIC) {
    // ── Read MIFARE Classic: sectors 0-3 (blocks 0-15) ──
    // Each sector needs separate authentication
    bool anyRead = false;
    for (uint8_t sector = 0; sector < 4; sector++) {
      uint8_t firstBlock = sector * 4;
      // Authenticate each sector
      if (!nfc.mifareclassic_AuthenticateBlock(uid, uidLength, firstBlock, 0, keyA)) {
        Serial.printf("Sector %d auth failed\n", sector);
        if (sector == 0 && tag.blocksRead == 0) {
          tag.authFailed = true;
        }
        continue;
      }
      // Read 4 blocks per sector (including trailer — useful for display)
      for (uint8_t b = 0; b < 4; b++) {
        uint8_t block = firstBlock + b;
        if (nfc.mifareclassic_ReadDataBlock(block, tag.data + (block * 16))) {
          tag.blocksRead++;
          anyRead = true;
        }
      }
    }
    tag.dataLen = CLASSIC_BLOCKS_TO_READ * 16;

    if (!anyRead) {
      if (tag.authFailed) {
        Serial.println("Auth failed on all sectors.");
      } else {
        tag.readFailed = true;
        Serial.println("Read failed on all blocks.");
      }
      lastTag = tag;
      ledRed();
      ledRedUntil = millis() + 2000;
      delay(1000);
      return;
    }

    // Check QIDI format at block 4 (sector 1, block 0): byte[2] == 0x01
    tag.isQidiFormat = (tag.data[QIDI_BLOCK * 16 + 2] == 0x01);

    if (tag.isQidiFormat) {
      Serial.printf("QIDI tag — Material: %d, Color: %d\n",
        tag.data[QIDI_BLOCK * 16], tag.data[QIDI_BLOCK * 16 + 1]);
    }

    Serial.printf("Read %d/%d blocks\n", tag.blocksRead, CLASSIC_BLOCKS_TO_READ);

  } else if (tag.cardType == CARD_MIFARE_ULTRALIGHT) {
    // ── Read Ultralight / NTAG pages ──
    // Read pages until failure (card boundary) or max
    uint8_t buf[4];
    for (uint8_t page = 0; page < UL_PAGES_TO_READ; page++) {
      if (!nfc.mifareultralight_ReadPage(page, buf)) {
        break; // Reached end of card or error
      }
      memcpy(tag.data + (page * 4), buf, 4);
      tag.pagesRead++;
    }
    tag.dataLen = tag.pagesRead * 4;

    if (tag.pagesRead == 0) {
      tag.readFailed = true;
      Serial.println("UL/NTAG read failed — no pages read.");
      lastTag = tag;
      ledRed();
      ledRedUntil = millis() + 2000;
      delay(1000);
      return;
    }

    Serial.printf("Read %d pages (%d bytes)\n", tag.pagesRead, tag.dataLen);

  } else {
    // Unknown card type — just store UID
    Serial.println("Unknown card type — cannot read data.");
  }

  lastTag = tag;
  ledGreen();
  ledGreenUntil = millis() + 3000;
}
