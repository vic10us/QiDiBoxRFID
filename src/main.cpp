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

// ─── WIFI SETUP PAGE ────────────────────────────────────────────────────────

const char SETUP_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>QIDI Box - WiFi Setup</title>
<style>
  :root {
    --bg: #0d0f12; --surface: #151820; --border: #1f2530;
    --accent: #e8ff47; --accent2: #47c8ff; --text: #e8eaf0;
    --muted: #5a6070; --success: #47ffaa; --error: #ff5c6a;
    --radius: 12px;
  }
  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background: var(--bg); color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
    min-height: 100vh; display: flex; align-items: center;
    justify-content: center; padding: 24px;
  }
  .card {
    background: var(--surface); border: 1px solid var(--border);
    border-radius: var(--radius); padding: 32px;
    width: 100%; max-width: 420px;
  }
  h1 { font-size: 24px; font-weight: 800; margin-bottom: 4px; }
  h1 span { color: var(--accent); }
  .sub { color: var(--muted); font-size: 13px; margin-bottom: 24px; }
  label {
    display: block; font-size: 13px; font-weight: 600;
    margin-bottom: 6px; margin-top: 16px;
  }
  input {
    width: 100%; background: var(--bg); color: var(--text);
    border: 1px solid var(--border); border-radius: 8px;
    padding: 12px 14px; font-size: 14px; outline: none;
    transition: border-color 0.15s;
  }
  input:focus { border-color: var(--accent); }
  .btn {
    width: 100%; padding: 14px; margin-top: 24px;
    background: var(--accent); color: #0d0f12; border: none;
    border-radius: var(--radius); font-size: 15px; font-weight: 800;
    text-transform: uppercase; letter-spacing: 0.05em;
    cursor: pointer; transition: all 0.15s;
  }
  .btn:hover { transform: translateY(-1px); box-shadow: 0 8px 24px rgba(232,255,71,0.25); }
  .btn:disabled { opacity: 0.4; cursor: not-allowed; transform: none; }
  .msg {
    margin-top: 16px; padding: 12px 16px; border-radius: 8px;
    font-size: 13px; display: none; border: 1px solid;
  }
  .msg.show { display: block; }
  .msg.ok { background: rgba(71,255,170,0.06); border-color: var(--success); color: var(--success); }
  .msg.err { background: rgba(255,92,106,0.06); border-color: var(--error); color: var(--error); }
  .hint { color: var(--muted); font-size: 11px; margin-top: 8px; }
</style>
</head>
<body>
<div class="card">
  <h1>QIDI <span>Box</span> Setup</h1>
  <p class="sub">Connect your device to WiFi</p>

  <form id="wifiForm" onsubmit="return saveWifi(event)">
    <label for="ssid">WiFi Network (SSID)</label>
    <input type="text" id="ssid" name="ssid" required maxlength="32" autocomplete="off" placeholder="Your WiFi name">

    <label for="pass">Password</label>
    <input type="password" id="pass" name="pass" maxlength="64" placeholder="WiFi password">

    <button type="submit" class="btn" id="saveBtn">Save &amp; Connect</button>
  </form>

  <div class="msg" id="msg"></div>
  <p class="hint">After saving, the device will restart and connect to your WiFi network. The setup hotspot will disappear.</p>
</div>

<script>
  async function saveWifi(e) {
    e.preventDefault();
    const btn = document.getElementById('saveBtn');
    const msg = document.getElementById('msg');
    btn.disabled = true;

    const ssid = document.getElementById('ssid').value;
    const pass = document.getElementById('pass').value;

    try {
      const res = await fetch('/wifi-save', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'ssid=' + encodeURIComponent(ssid) + '&pass=' + encodeURIComponent(pass)
      });
      const txt = await res.text();
      if (res.ok) {
        msg.className = 'msg show ok';
        msg.textContent = txt;
      } else {
        msg.className = 'msg show err';
        msg.textContent = txt;
        btn.disabled = false;
      }
    } catch(err) {
      msg.className = 'msg show err';
      msg.textContent = 'Connection error. Make sure you are connected to the QidiBox-Setup network.';
      btn.disabled = false;
    }
    return false;
  }
</script>
</body>
</html>
)rawhtml";

// ─── HTML PAGE ───────────────────────────────────────────────────────────────

const char HTML_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>QIDI NFC Tool</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Syne:wght@400;700;800&family=DM+Mono:wght@400;500&display=swap" rel="stylesheet">
<style>
  :root {
    --bg:       #0d0f12;
    --surface:  #151820;
    --border:   #1f2530;
    --accent:   #e8ff47;
    --accent2:  #47c8ff;
    --text:     #e8eaf0;
    --muted:    #5a6070;
    --success:  #47ffaa;
    --error:    #ff5c6a;
    --radius:   12px;
  }

  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

  body {
    background: var(--bg);
    color: var(--text);
    font-family: 'Syne', sans-serif;
    min-height: 100vh;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 24px;
    overflow-x: hidden;
  }

  body::before {
    content: '';
    position: fixed;
    inset: 0;
    background-image:
      linear-gradient(var(--border) 1px, transparent 1px),
      linear-gradient(90deg, var(--border) 1px, transparent 1px);
    background-size: 40px 40px;
    opacity: 0.4;
    pointer-events: none;
    z-index: 0;
  }

  .container {
    position: relative;
    z-index: 1;
    width: 100%;
    max-width: 520px;
  }

  header { margin-bottom: 32px; }

  .badge {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    font-family: 'DM Mono', monospace;
    font-size: 11px;
    letter-spacing: 0.12em;
    text-transform: uppercase;
    color: var(--accent);
    border: 1px solid var(--accent);
    padding: 4px 10px;
    border-radius: 4px;
    margin-bottom: 16px;
  }

  .badge::before {
    content: '';
    width: 6px;
    height: 6px;
    border-radius: 50%;
    background: var(--accent);
    animation: pulse 2s infinite;
  }

  @keyframes pulse {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.3; }
  }

  h1 {
    font-size: clamp(28px, 6vw, 40px);
    font-weight: 800;
    line-height: 1.1;
    letter-spacing: -0.02em;
  }

  h1 span { color: var(--accent); }

  .subtitle {
    margin-top: 8px;
    color: var(--muted);
    font-family: 'DM Mono', monospace;
    font-size: 13px;
  }

  .card {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 28px;
    margin-bottom: 16px;
  }

  .card-label {
    font-family: 'DM Mono', monospace;
    font-size: 10px;
    letter-spacing: 0.15em;
    text-transform: uppercase;
    color: var(--muted);
    margin-bottom: 16px;
  }

  .field { margin-bottom: 16px; }

  label {
    display: block;
    font-size: 13px;
    font-weight: 700;
    letter-spacing: 0.04em;
    color: var(--text);
    margin-bottom: 8px;
  }

  select {
    width: 100%;
    background: var(--bg);
    color: var(--text);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 12px 14px;
    font-family: 'DM Mono', monospace;
    font-size: 13px;
    appearance: none;
    background-image: url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='8' viewBox='0 0 12 8'%3E%3Cpath d='M1 1l5 5 5-5' stroke='%235a6070' stroke-width='1.5' fill='none' stroke-linecap='round'/%3E%3C/svg%3E");
    background-repeat: no-repeat;
    background-position: right 14px center;
    cursor: pointer;
    transition: border-color 0.15s;
  }

  select:focus { outline: none; border-color: var(--accent); }

  .color-row {
    display: flex;
    align-items: center;
    gap: 12px;
    margin-top: 10px;
  }

  .color-swatch {
    width: 28px;
    height: 28px;
    border-radius: 6px;
    border: 1px solid var(--border);
    transition: background 0.2s;
    flex-shrink: 0;
  }

  .color-name {
    font-family: 'DM Mono', monospace;
    font-size: 12px;
    color: var(--muted);
  }

  .payload-preview {
    background: var(--bg);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 14px;
    font-family: 'DM Mono', monospace;
    font-size: 12px;
    color: var(--accent2);
    display: flex;
    gap: 8px;
    align-items: center;
    flex-wrap: wrap;
  }

  .byte-box {
    background: var(--surface);
    border: 1px solid var(--border);
    padding: 4px 10px;
    border-radius: 4px;
    font-size: 13px;
    font-weight: 500;
  }

  .byte-label {
    color: var(--muted);
    font-size: 10px;
    display: block;
    text-align: center;
    margin-top: 2px;
  }

  .byte-group {
    display: flex;
    flex-direction: column;
    align-items: center;
  }

  .btn {
    width: 100%;
    padding: 16px;
    border: none;
    border-radius: var(--radius);
    font-family: 'Syne', sans-serif;
    font-size: 15px;
    font-weight: 800;
    letter-spacing: 0.05em;
    text-transform: uppercase;
    cursor: pointer;
    transition: all 0.15s;
    position: relative;
    overflow: hidden;
  }

  .btn:hover {
    transform: translateY(-1px);
  }

  .btn:active { transform: translateY(0); }

  .btn:disabled {
    opacity: 0.4;
    cursor: not-allowed;
    transform: none;
    box-shadow: none;
  }

  .btn-write {
    background: var(--accent);
    color: #0d0f12;
  }

  .btn-write:hover {
    box-shadow: 0 8px 24px rgba(232, 255, 71, 0.25);
  }

  .status {
    border-radius: var(--radius);
    padding: 16px 20px;
    font-family: 'DM Mono', monospace;
    font-size: 13px;
    display: none;
    align-items: flex-start;
    gap: 12px;
    border: 1px solid;
    margin-top: 16px;
    animation: fadeIn 0.2s ease;
  }

  .status.show { display: flex; }

  .status.success {
    background: rgba(71, 255, 170, 0.06);
    border-color: var(--success);
    color: var(--success);
  }

  .status.error {
    background: rgba(255, 92, 106, 0.06);
    border-color: var(--error);
    color: var(--error);
  }

  .status.waiting {
    background: rgba(71, 200, 255, 0.06);
    border-color: var(--accent2);
    color: var(--accent2);
  }

  .status-icon { font-size: 18px; flex-shrink: 0; margin-top: 1px; }

  @keyframes fadeIn {
    from { opacity: 0; transform: translateY(4px); }
    to   { opacity: 1; transform: translateY(0); }
  }

  /* Modal overlay */
  .modal-overlay {
    display: none;
    position: fixed;
    inset: 0;
    background: rgba(0, 0, 0, 0.7);
    backdrop-filter: blur(8px);
    z-index: 100;
    align-items: center;
    justify-content: center;
    padding: 24px;
    animation: fadeIn 0.2s ease;
  }

  .modal-overlay.show { display: flex; }

  .modal {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 28px;
    width: 100%;
    max-width: 480px;
    max-height: 90vh;
    overflow-y: auto;
    animation: modalSlide 0.25s ease;
  }

  @keyframes modalSlide {
    from { opacity: 0; transform: translateY(16px) scale(0.97); }
    to   { opacity: 1; transform: translateY(0) scale(1); }
  }

  .modal-title {
    font-size: 20px;
    font-weight: 800;
    margin-bottom: 4px;
  }

  .modal-subtitle {
    font-family: 'DM Mono', monospace;
    font-size: 11px;
    color: var(--muted);
    margin-bottom: 20px;
  }

  .tag-header {
    display: flex;
    align-items: center;
    gap: 12px;
    margin-bottom: 16px;
  }

  .tag-swatch {
    width: 44px;
    height: 44px;
    border-radius: 8px;
    border: 1px solid var(--border);
    flex-shrink: 0;
  }

  .tag-material {
    font-weight: 800;
    font-size: 18px;
  }

  .tag-color {
    font-family: 'DM Mono', monospace;
    font-size: 12px;
    color: var(--muted);
    margin-top: 2px;
  }

  .tag-warning {
    color: var(--error);
    font-family: 'DM Mono', monospace;
    font-size: 12px;
    margin-top: 12px;
    margin-bottom: 4px;
  }

  .tag-section-label {
    font-family: 'DM Mono', monospace;
    font-size: 10px;
    letter-spacing: 0.15em;
    text-transform: uppercase;
    color: var(--muted);
    margin-top: 16px;
    margin-bottom: 8px;
  }

  .tag-raw {
    font-family: 'DM Mono', monospace;
    font-size: 11px;
    color: var(--accent2);
    background: var(--bg);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 12px 14px;
    word-break: break-all;
    line-height: 1.6;
  }

  .tag-uid {
    font-family: 'DM Mono', monospace;
    font-size: 11px;
    color: var(--muted);
    margin-top: 8px;
  }

  .modal-actions {
    display: flex;
    gap: 10px;
    margin-top: 20px;
  }

  .modal-actions .btn {
    flex: 1;
    padding: 12px;
    font-size: 13px;
  }

  .btn-close {
    background: var(--border);
    color: var(--text);
  }

  .btn-close:hover {
    background: var(--muted);
    box-shadow: none;
  }

  .btn-load {
    background: var(--accent2);
    color: #0d0f12;
  }

  .btn-load:hover {
    box-shadow: 0 8px 24px rgba(71, 200, 255, 0.25);
  }

  /* Scanning indicator in header area */
  .scanning-hint {
    font-family: 'DM Mono', monospace;
    font-size: 12px;
    color: var(--accent2);
    display: flex;
    align-items: center;
    gap: 8px;
    margin-bottom: 24px;
  }

  .scanning-dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    background: var(--accent2);
    animation: pulse 1.5s infinite;
  }

  footer {
    text-align: center;
    font-family: 'DM Mono', monospace;
    font-size: 11px;
    color: var(--muted);
    margin-top: 24px;
  }
</style>
</head>
<body>
<div class="container">

  <header>
    <div class="badge">NFC Tool</div>
    <h1>QIDI <span>Box</span><br>Tag Tool</h1>
    <p class="subtitle">// ESP32-S3 + PN532 &middot; read &amp; write</p>
  </header>

  <!-- ── TAG DETECTED MODAL ── -->
  <div class="modal-overlay" id="tagModal">
    <div class="modal">
      <div class="modal-title" id="modalTitle">Tag Detected</div>
      <div class="modal-subtitle" id="modalSubtitle"></div>

      <div class="tag-header" id="tagHeader">
        <div class="tag-swatch" id="tagSwatch"></div>
        <div>
          <div class="tag-material" id="tagMaterial"></div>
          <div class="tag-color" id="tagColor"></div>
        </div>
      </div>

      <div class="payload-preview" id="tagBytes">
        <div class="byte-group">
          <div class="byte-box" id="tagByte0"></div>
          <span class="byte-label">Material</span>
        </div>
        <div class="byte-group">
          <div class="byte-box" id="tagByte1"></div>
          <span class="byte-label">Color</span>
        </div>
        <div class="byte-group">
          <div class="byte-box" id="tagByte2"></div>
          <span class="byte-label">Mfr</span>
        </div>
      </div>

      <div class="tag-warning" id="tagWarning"></div>

      <div class="tag-section-label" id="rawLabel">Raw Data</div>
      <div class="tag-raw" id="tagRaw"></div>
      <div class="tag-uid" id="tagUid"></div>

      <div class="modal-actions">
        <button class="btn btn-close" onclick="closeModal()">Close</button>
        <button class="btn btn-load" id="clearBtn" onclick="clearTag()" style="background: var(--error);">Clear Tag</button>
        <button class="btn btn-load" id="loadBtn" onclick="loadToWriter()">Load to Writer</button>
      </div>
    </div>
  </div>

  <div id="scanHint" class="scanning-hint">
    <div class="scanning-dot"></div>
    Scanning for tags&hellip; place a tag on the reader
  </div>

  <!-- ── TAG WRITER ── -->
  <div class="card">
    <p class="card-label">01 &middot; Write Tag</p>

    <div class="field">
      <label for="material">Material</label>
      <select id="material" onchange="updatePreview()">
        <option value="1">PLA</option>
        <option value="2">PLA Matte</option>
        <option value="3">PLA Metal</option>
        <option value="4">PLA Silk</option>
        <option value="5">PLA-CF</option>
        <option value="6">PLA-Wood</option>
        <option value="7">PLA Basic</option>
        <option value="8">PLA Matte Basic</option>
        <option value="11">ABS</option>
        <option value="12">ABS-GF</option>
        <option value="13">ABS-Metal</option>
        <option value="14">ABS-Odorless</option>
        <option value="18">ASA</option>
        <option value="19">ASA-AERO</option>
        <option value="24">UltraPA</option>
        <option value="25">PA-CF</option>
        <option value="26">UltraPA-CF25</option>
        <option value="27">PA12-CF</option>
        <option value="30">PAHT-CF</option>
        <option value="31">PAHT-GF</option>
        <option value="32">Support For PAHT</option>
        <option value="33">Support For PET/PA</option>
        <option value="34">PC/ABS-FR</option>
        <option value="37">PET-CF</option>
        <option value="38">PET-GF</option>
        <option value="39" selected>PETG Basic</option>
        <option value="40">PETG Tough</option>
        <option value="41">PETG Rapido</option>
        <option value="42">PETG-CF</option>
        <option value="43">PETG-GF</option>
        <option value="44">PPS-CF</option>
        <option value="45">PETG Translucent</option>
        <option value="47">PVA</option>
        <option value="49">TPU-Aero</option>
        <option value="50">TPU</option>
      </select>
    </div>

    <div class="field">
      <label for="color">Color</label>
      <select id="color" onchange="updatePreview()">
        <option value="1" data-hex="#FAFAFA">White</option>
        <option value="2" data-hex="#060606">Black</option>
        <option value="3" data-hex="#D9E3ED">Light Grey</option>
        <option value="4" data-hex="#5CF30F">Lime Green</option>
        <option value="5" data-hex="#63E492">Mint Green</option>
        <option value="6" data-hex="#2850FF">Blue</option>
        <option value="7" data-hex="#FE98FE">Pink</option>
        <option value="8" data-hex="#DFD628">Yellow</option>
        <option value="9" data-hex="#228332">Dark Green</option>
        <option value="10" data-hex="#99DEFF">Sky Blue</option>
        <option value="11" data-hex="#1714B0">Dark Blue</option>
        <option value="12" data-hex="#CEC0FE">Lavender</option>
        <option value="13" data-hex="#CADE4B">Yellow-Green</option>
        <option value="14" data-hex="#1353AB">Navy</option>
        <option value="15" data-hex="#5EA9FD">Cornflower Blue</option>
        <option value="16" data-hex="#A878FF">Purple</option>
        <option value="17" data-hex="#FE717A">Salmon</option>
        <option value="18" data-hex="#FF362D">Red</option>
        <option value="19" data-hex="#E2DFCD">Beige</option>
        <option value="20" data-hex="#898F9B">Grey</option>
        <option value="21" data-hex="#6E3812">Brown</option>
        <option value="22" data-hex="#CAC59F">Sand</option>
        <option value="23" data-hex="#F28636">Orange</option>
        <option value="24" data-hex="#B87F2B">Gold</option>
      </select>
      <div class="color-row">
        <div class="color-swatch" id="swatch"></div>
        <span class="color-name" id="colorHex"></span>
      </div>
    </div>

    <div class="card" style="padding: 14px; margin-bottom: 16px;">
      <p class="card-label" style="margin-bottom: 10px;">Payload Preview</p>
      <div class="payload-preview">
        <div class="byte-group">
          <div class="byte-box" id="byte0">0x27</div>
          <span class="byte-label">Material</span>
        </div>
        <div class="byte-group">
          <div class="byte-box" id="byte1">0x17</div>
          <span class="byte-label">Color</span>
        </div>
        <div class="byte-group">
          <div class="byte-box">0x01</div>
          <span class="byte-label">Mfr</span>
        </div>
        <div class="byte-group">
          <div class="byte-box" style="color: var(--muted)">0x00 &times;13</div>
          <span class="byte-label">Padding</span>
        </div>
      </div>
    </div>

    <button class="btn btn-write" id="writeBtn" onclick="writeTag()">
      Place Tag &amp; Write
    </button>

    <div class="status" id="writeStatus">
      <span class="status-icon" id="writeStatusIcon"></span>
      <span id="writeStatusMsg"></span>
    </div>
  </div>

  <footer>
    Sector 1 &middot; Block 0 &middot; ISO/IEC 14443-A &middot; 13.56 MHz
    <br><a href="/wifi-setup" style="color: var(--muted); text-decoration: none; border-bottom: 1px dashed var(--muted);">WiFi Settings</a>
  </footer>

</div>

<script>
  const COLORS = {
    1:'#FAFAFA', 2:'#060606', 3:'#D9E3ED', 4:'#5CF30F', 5:'#63E492',
    6:'#2850FF', 7:'#FE98FE', 8:'#DFD628', 9:'#228332', 10:'#99DEFF',
    11:'#1714B0', 12:'#CEC0FE', 13:'#CADE4B', 14:'#1353AB', 15:'#5EA9FD',
    16:'#A878FF', 17:'#FE717A', 18:'#FF362D', 19:'#E2DFCD', 20:'#898F9B',
    21:'#6E3812', 22:'#CAC59F', 23:'#F28636', 24:'#B87F2B'
  };

  const MATERIALS = {
    1:'PLA', 2:'PLA Matte', 3:'PLA Metal', 4:'PLA Silk', 5:'PLA-CF',
    6:'PLA-Wood', 7:'PLA Basic', 8:'PLA Matte Basic',
    11:'ABS', 12:'ABS-GF', 13:'ABS-Metal', 14:'ABS-Odorless',
    18:'ASA', 19:'ASA-AERO',
    24:'UltraPA', 25:'PA-CF', 26:'UltraPA-CF25', 27:'PA12-CF',
    30:'PAHT-CF', 31:'PAHT-GF', 32:'Support For PAHT', 33:'Support For PET/PA',
    34:'PC/ABS-FR',
    37:'PET-CF', 38:'PET-GF', 39:'PETG Basic', 40:'PETG Tough',
    41:'PETG Rapido', 42:'PETG-CF', 43:'PETG-GF', 44:'PPS-CF',
    45:'PETG Translucent',
    47:'PVA',
    49:'TPU-Aero', 50:'TPU'
  };

  const COLOR_NAMES = {
    1:'White', 2:'Black', 3:'Light Grey', 4:'Lime Green', 5:'Mint Green',
    6:'Blue', 7:'Pink', 8:'Yellow', 9:'Dark Green', 10:'Sky Blue',
    11:'Dark Blue', 12:'Lavender', 13:'Yellow-Green', 14:'Navy',
    15:'Cornflower Blue', 16:'Purple', 17:'Salmon', 18:'Red',
    19:'Beige', 20:'Grey', 21:'Brown', 22:'Sand', 23:'Orange', 24:'Gold'
  };

  let lastTimestamp = 0;
  let currentTagData = null;
  let modalDismissedTimestamp = 0;
  let suppressModal = false;

  function toHex(n) {
    return '0x' + n.toString(16).toUpperCase().padStart(2, '0');
  }

  function updatePreview() {
    const mat = parseInt(document.getElementById('material').value);
    const col = parseInt(document.getElementById('color').value);
    const hex = COLORS[col];

    document.getElementById('byte0').textContent = toHex(mat);
    document.getElementById('byte1').textContent = toHex(col);
    document.getElementById('swatch').style.background = hex;
    document.getElementById('colorHex').textContent = hex;
  }

  function showWriteStatus(type, icon, msg) {
    const el = document.getElementById('writeStatus');
    el.className = 'status show ' + type;
    document.getElementById('writeStatusIcon').textContent = icon;
    document.getElementById('writeStatusMsg').textContent = msg;
  }

  async function writeTag() {
    const mat = document.getElementById('material').value;
    const col = document.getElementById('color').value;
    const btn = document.getElementById('writeBtn');

    btn.disabled = true;
    suppressModal = true;
    showWriteStatus('waiting', '\u23F3', 'Waiting for tag \u2014 place it on the reader now...');

    try {
      const res = await fetch('/write?material=' + mat + '&color=' + col);
      const txt = await res.text();

      if (res.ok) {
        showWriteStatus('success', '\u2705', txt);
      } else {
        showWriteStatus('error', '\u274C', txt);
      }
    } catch (e) {
      showWriteStatus('error', '\u274C', 'Network error \u2014 is the ESP32 reachable?');
    }

    // Sync timestamp then release suppress
    try {
      const s = await fetch('/status');
      if (s.ok) { const d = await s.json(); lastTimestamp = d.timestamp || 0; }
    } catch(e2) {}
    suppressModal = false;
    btn.disabled = false;
  }

  // ── Modal ──

  function formatHexDump(hexStr, bytesPerRow, cardTypeId) {
    if (!hexStr) return '(no data)';
    const bytes = hexStr.split(' ');
    let lines = '';
    const isClassic = (cardTypeId === 1);

    for (let i = 0; i < bytes.length; i += bytesPerRow) {
      const chunk = bytes.slice(i, i + bytesPerRow).join(' ');
      let label;
      if (isClassic) {
        // Show block number for Classic (16 bytes per block)
        const block = Math.floor(i / 16);
        label = 'B' + block.toString().padStart(2, '0');
      } else {
        // Show page number for UL/NTAG (4 bytes per page)
        const page = Math.floor(i / 4);
        label = 'P' + page.toString().padStart(2, '0');
      }
      lines += (lines ? '\n' : '') + label + ':  ' + chunk;
    }
    return lines;
  }

  function showModal(data) {
    currentTagData = data;
    const modal = document.getElementById('tagModal');
    const warning = document.getElementById('tagWarning');
    const loadBtn = document.getElementById('loadBtn');
    const clearBtn = document.getElementById('clearBtn');

    const cardType = data.cardType || 'Unknown';
    const isClassic = (data.cardTypeId === 1);
    const isUltralight = (data.cardTypeId === 2);

    // UID line with card type and data size
    let uidLine = 'UID: ' + data.uid + '  \u00B7  ' + cardType;
    if (data.dataLen > 0) {
      uidLine += '  \u00B7  ' + data.dataLen + ' bytes';
      if (isClassic && data.blocksRead) uidLine += ' (' + data.blocksRead + ' blocks)';
      if (isUltralight && data.pagesRead) uidLine += ' (' + data.pagesRead + ' pages)';
    }
    document.getElementById('tagUid').textContent = uidLine;

    // Raw data label
    if (isClassic) {
      document.getElementById('rawLabel').textContent = 'Raw Data \u00B7 Sectors 0\u20133';
    } else if (isUltralight) {
      document.getElementById('rawLabel').textContent = 'Raw Data \u00B7 Pages 0\u2013' + (data.pagesRead - 1);
    } else {
      document.getElementById('rawLabel').textContent = 'Raw Data';
    }

    // Format hex dump: 16 bytes/row for Classic, 4 bytes/row for UL/NTAG
    const rowSize = isClassic ? 16 : 4;
    document.getElementById('tagRaw').textContent = formatHexDump(data.raw, rowSize, data.cardTypeId);

    // Default: show clear button for any readable card
    clearBtn.style.display = (data.dataLen > 0 && !data.authFailed) ? 'block' : 'none';

    if (data.authFailed) {
      document.getElementById('modalTitle').textContent = 'Locked Tag';
      document.getElementById('modalSubtitle').textContent = cardType + ' \u00B7 Authentication failed';
      document.getElementById('tagHeader').style.display = 'none';
      document.getElementById('tagBytes').style.display = 'none';
      warning.textContent = 'This tag uses non-default authentication keys. Block data cannot be read.';
      warning.style.display = 'block';
      loadBtn.style.display = 'none';
      clearBtn.style.display = 'none';
      modal.classList.add('show');
      return;
    }

    if (data.readFailed) {
      document.getElementById('modalTitle').textContent = 'Read Error';
      document.getElementById('modalSubtitle').textContent = cardType + ' \u00B7 Read failed';
      document.getElementById('tagHeader').style.display = 'none';
      document.getElementById('tagBytes').style.display = 'none';
      warning.textContent = 'Could not read data from this tag.';
      warning.style.display = 'block';
      loadBtn.style.display = 'none';
      clearBtn.style.display = 'none';
      modal.classList.add('show');
      return;
    }

    if (data.isQidiFormat && isClassic) {
      // ── QIDI format on MIFARE Classic ──
      const matName = MATERIALS[data.material] || ('Unknown Material (' + toHex(data.material) + ')');
      const colName = COLOR_NAMES[data.color] || ('Unknown Color (' + toHex(data.color) + ')');
      const colHex = COLORS[data.color] || '#444444';

      document.getElementById('modalTitle').textContent = 'QIDI Tag Detected';
      document.getElementById('modalSubtitle').textContent = cardType + ' \u00B7 Valid QIDI filament configuration';
      document.getElementById('tagHeader').style.display = 'flex';
      document.getElementById('tagBytes').style.display = 'flex';
      document.getElementById('tagByte0').textContent = toHex(data.material);
      document.getElementById('tagByte1').textContent = toHex(data.color);
      document.getElementById('tagByte2').textContent = toHex(data.manufacturer);
      document.getElementById('tagMaterial').textContent = matName;
      document.getElementById('tagColor').textContent = colName;
      document.getElementById('tagSwatch').style.background = colHex;
      warning.style.display = 'none';

      const canLoad = MATERIALS[data.material] && COLOR_NAMES[data.color];
      loadBtn.style.display = canLoad ? 'block' : 'none';

    } else {
      // ── Non-QIDI or non-Classic: show raw data ──
      let title, subtitle, warnText;

      if (isUltralight) {
        title = 'Ultralight / NTAG Tag';
        subtitle = cardType + ' \u00B7 ' + data.pagesRead + ' pages read';
        warnText = 'This is an Ultralight/NTAG card. QIDI filament data requires MIFARE Classic 1K.';
      } else if (isClassic) {
        title = 'MIFARE Classic Tag';
        subtitle = cardType + ' \u00B7 Non-QIDI data';
        warnText = 'This MIFARE Classic tag does not contain recognized QIDI filament data (Mfr byte \u2260 0x01).';
      } else {
        title = 'NFC Tag Detected';
        subtitle = cardType;
        warnText = 'Unknown card type. Raw data may not be available.';
      }

      document.getElementById('modalTitle').textContent = title;
      document.getElementById('modalSubtitle').textContent = subtitle;
      document.getElementById('tagHeader').style.display = 'none';
      document.getElementById('tagBytes').style.display = 'none';
      warning.textContent = warnText;
      warning.style.display = 'block';
      loadBtn.style.display = 'none';
    }

    modal.classList.add('show');
  }

  function closeModal() {
    document.getElementById('tagModal').classList.remove('show');
    modalDismissedTimestamp = lastTimestamp;
  }

  function loadToWriter() {
    if (!currentTagData || !currentTagData.isQidiFormat) return;

    // Set dropdowns to match tag values
    const matSelect = document.getElementById('material');
    const colSelect = document.getElementById('color');

    // Only set if the value exists as an option
    const matOpt = matSelect.querySelector('option[value="' + currentTagData.material + '"]');
    const colOpt = colSelect.querySelector('option[value="' + currentTagData.color + '"]');

    if (matOpt) matSelect.value = currentTagData.material;
    if (colOpt) colSelect.value = currentTagData.color;

    updatePreview();
    closeModal();

    // Flash the write card to draw attention
    showWriteStatus('success', '\u2705', 'Loaded from tag: ' +
      (MATERIALS[currentTagData.material] || toHex(currentTagData.material)) + ' / ' +
      (COLOR_NAMES[currentTagData.color] || toHex(currentTagData.color)));
  }

  async function clearTag() {
    const clearBtn = document.getElementById('clearBtn');
    clearBtn.disabled = true;
    clearBtn.textContent = 'Clearing\u2026';
    suppressModal = true;

    closeModal();

    try {
      const res = await fetch('/clear');
      const txt = await res.text();

      if (res.ok) {
        showWriteStatus('success', '\u2705', txt);
      } else {
        showWriteStatus('error', '\u274C', txt);
      }
    } catch (e) {
      showWriteStatus('error', '\u274C', 'Network error \u2014 is the ESP32 reachable?');
    }

    // Sync timestamp then release suppress
    try {
      const s = await fetch('/status');
      if (s.ok) { const d = await s.json(); lastTimestamp = d.timestamp || 0; }
    } catch(e2) {}
    suppressModal = false;
    clearBtn.disabled = false;
    clearBtn.textContent = 'Clear Tag';
  }

  // ── Polling ──

  async function pollStatus() {
    try {
      const res = await fetch('/status');
      if (!res.ok) return;
      const data = await res.json();

      if (data.timestamp !== lastTimestamp) {
        lastTimestamp = data.timestamp;

        if (data.tagPresent && !suppressModal && data.timestamp !== modalDismissedTimestamp) {
          document.getElementById('scanHint').style.display = 'none';
          showModal(data);
        }
      }
    } catch (e) {
      // Silently ignore poll errors
    }
  }

  // Init: sync timestamp with server so we don't popup stale data on load
  async function init() {
    updatePreview();
    try {
      const res = await fetch('/status');
      if (res.ok) {
        const data = await res.json();
        lastTimestamp = data.timestamp || 0;
      }
    } catch (e) {}
    setInterval(pollStatus, 1500);
  }
  init();
</script>
</body>
</html>
)rawhtml";


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
      req->send(200, "text/html", SETUP_PAGE);
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
      req->send(200, "text/html", SETUP_PAGE);
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
    req->send(200, "text/html", HTML_PAGE);
  });

  // WiFi reconfigure endpoint (accessible from main UI)
  server.on("/wifi-setup", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/html", SETUP_PAGE);
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
