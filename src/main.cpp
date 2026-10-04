/*
 * ============================================================================
 *  PROYEK  : PEMANTAU AIR
 *  FUNGSI  : Kontrol Pompa Air Sumur Bor + Lampu Rumah via Web Server Lokal
 *  BOARD   : ESP32 DevKit (Arduino framework)
 * ============================================================================
 *
 *  CARA KERJA SINGKAT
 *  ------------------
 *  - 3 kawat elektroda: 1 kawat GROUND (paling dasar) + Probe ATAS + Probe BAWAH.
 *    Jika air menyentuh probe -> pin terbaca LOW (air menghantarkan ground).
 *  - Relay 1 = Pompa (mode AUTO / MANUAL), Relay 2 = Lampu (manual penuh).
 *  - Dashboard web diakses dari HP melalui browser (IP ditampilkan di Serial).
 *
 *  LIBRARY YANG DIBUTUHKAN (install via Library Manager):
 *    1) "AsyncTCP"            (author: ESP32Async)
 *    2) "ESPAsyncWebServer"   (author: ESP32Async)
 *  Board package: "esp32" by Espressif Systems.
 * ============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

/* ==========================================================================
 *  1. KONFIGURASI WIFI & IP  ->  PINDAH KE FILE "secrets.h"
 *  SSID, password, dan setting IP sengaja TIDAK ditulis di sini supaya
 *  tidak ikut ter-commit ke Git. Cara pakai:
 *    1) Salin "secrets.example.h" menjadi "secrets.h"
 *    2) Isi nilai asli Anda di "secrets.h"
 *  Jika "secrets.h" belum ada, otomatis dipakai versi contoh (placeholder).
 * ========================================================================== */
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #include "secrets.example.h"
#endif

/* ==========================================================================
 *  2. KONFIGURASI PIN  (pin aman, bebas strapping/flash)
 * ========================================================================== */
#define PIN_PROBE_TOP      32   // Kawat Probe BATAS ATAS    -> INPUT_PULLUP
#define PIN_PROBE_BOTTOM   33   // Kawat Probe BATAS BAWAH   -> INPUT_PULLUP
#define PIN_RELAY_PUMP     26   // Relay 1 (IN1) -> Pompa Air
#define PIN_RELAY_LAMP     27   // Relay 2 (IN2) -> Lampu Rumah

/* ==========================================================================
 *  3. LOGIKA RELAY
 *  Sebagian besar modul relay = Active-LOW.
 *  Jika relay Anda Active-HIGH, cukup tukar kedua nilai di bawah ini saja.
 * ========================================================================== */
#define RELAY_ON    LOW
#define RELAY_OFF   HIGH

/* ==========================================================================
 *  4. PARAMETER SISTEM
 * ========================================================================== */
#define SENSOR_SAMPLE_MS   50    // interval pembacaan sensor (ms)
#define PROBE_DEBOUNCE_MS  400   // redam riak air / noise (ms)
#define WIFI_RETRY_MS      10000 // interval coba sambung ulang WiFi (ms)

/* ==========================================================================
 *  5. OBJEK & STATE GLOBAL
 * ========================================================================== */
AsyncWebServer server(80);

enum PumpMode { MODE_AUTO, MODE_MANUAL };

PumpMode pumpMode   = MODE_AUTO;  // mode awal pompa
bool     autoLatch  = false;      // latch histeresis AUTO
bool     manualPump = false;      // state tombol manual pompa
bool     lampOn     = false;      // state lampu
bool     pumpOn     = false;      // state akhir relay pompa

/* --- State probe (dengan debounce) --- */
struct ProbeState {
  uint8_t       pin;
  bool          wet;        // true = terendam air (LOW)
  bool          rawPrev;    // pembacaan mentah sebelumnya
  unsigned long lastFlip;   // waktu perubahan terakhir
};

ProbeState probeTop    = { PIN_PROBE_TOP,    false, false, 0 };
ProbeState probeBottom = { PIN_PROBE_BOTTOM, false, false, 0 };

/* ==========================================================================
 *  6. HALAMAN WEB (HTML + CSS + JS) DISIMPAN DI PROGMEM
 * ========================================================================== */
const char index_html[] PROGMEM = R"HTMLDOC(
<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Pemantau Air</title>
<style>
:root{--bg:#0b1220;--card:#151f32;--card2:#1d2a42;--text:#e6edf7;--muted:#8fa3bf;
--accent:#38bdf8;--green:#22c55e;--red:#ef4444;--amber:#f59e0b;--border:#24344f;}
*{box-sizing:border-box;margin:0;padding:0;}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
background:radial-gradient(1200px 600px at 50% -10%,#16233a,var(--bg));color:var(--text);
min-height:100vh;padding:16px;display:flex;justify-content:center;}
.wrap{width:100%;max-width:460px;}
header{display:flex;align-items:center;justify-content:space-between;margin-bottom:14px;}
h1{font-size:1.15rem;font-weight:700;letter-spacing:.3px;}
.brand{display:flex;align-items:center;gap:8px;}
.status-dot{width:9px;height:9px;border-radius:50%;background:var(--red);
box-shadow:0 0 0 4px rgba(239,68,68,.15);transition:.3s;}
.status-dot.online{background:var(--green);box-shadow:0 0 0 4px rgba(34,197,94,.15);}
.card{background:linear-gradient(180deg,var(--card),var(--card2));border:1px solid var(--border);
border-radius:16px;padding:16px;margin-bottom:14px;box-shadow:0 6px 24px rgba(0,0,0,.25);}
.card h2{font-size:.72rem;text-transform:uppercase;letter-spacing:1.2px;color:var(--muted);
margin-bottom:10px;font-weight:600;}
.water{text-align:center;padding:22px 16px;}
.water .icon{font-size:2.6rem;line-height:1;margin-bottom:8px;}
.water .label{font-size:1.35rem;font-weight:700;}
.water.ok .label{color:var(--green);}
.water.warn .label{color:var(--amber);}
.water.bad .label{color:var(--red);}
.water.ok{border-color:rgba(34,197,94,.45);}
.water.warn{border-color:rgba(245,158,11,.45);}
.water.bad{border-color:rgba(239,68,68,.45);}
.probes{display:flex;gap:10px;margin-top:14px;}
.probe{flex:1;background:rgba(255,255,255,.03);border:1px solid var(--border);border-radius:12px;
padding:10px;text-align:center;font-size:.78rem;color:var(--muted);}
.probe .dot{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:6px;
background:#475569;vertical-align:middle;}
.probe .dot.on{background:var(--accent);box-shadow:0 0 8px var(--accent);}
.seg{display:flex;background:rgba(255,255,255,.04);border:1px solid var(--border);
border-radius:12px;padding:4px;margin-bottom:12px;}
.seg button{flex:1;background:none;border:none;color:var(--muted);padding:9px;border-radius:9px;
font-size:.85rem;font-weight:600;cursor:pointer;transition:.2s;}
.seg button.active{background:var(--accent);color:#04121f;}
.btn{width:100%;border:none;border-radius:12px;padding:15px;font-size:1rem;font-weight:700;
letter-spacing:.5px;cursor:pointer;color:#fff;transition:.2s;}
.btn.on{background:linear-gradient(180deg,#22c55e,#16a34a);}
.btn.off{background:linear-gradient(180deg,#334155,#1e293b);}
.btn:disabled{opacity:.45;cursor:not-allowed;}
.hint{font-size:.75rem;color:var(--amber);margin-top:8px;text-align:center;display:none;}
.meta{display:flex;justify-content:space-between;font-size:.78rem;color:var(--muted);padding:0 4px;}
footer{text-align:center;font-size:.7rem;color:#5b6b85;margin:6px 0 20px;}
</style>
</head>
<body>
<div class="wrap">
  <header>
    <div class="brand"><span style="font-size:1.4rem">&#128167;</span><h1>Pemantau Air</h1></div>
    <div style="display:flex;align-items:center;gap:8px;font-size:.75rem;color:var(--muted)">
      <span id="linkText">Offline</span><span class="status-dot" id="linkDot"></span>
    </div>
  </header>

  <div class="card water" id="waterCard">
    <div class="icon" id="waterIcon">&#127754;</div>
    <div class="label" id="waterText">Memuat...</div>
    <div class="probes">
      <div class="probe"><span class="dot" id="topDot"></span>Batas Atas</div>
      <div class="probe"><span class="dot" id="botDot"></span>Batas Bawah</div>
    </div>
  </div>

  <div class="card">
    <h2>Kontrol Pompa (Relay 1)</h2>
    <div class="seg">
      <button id="modeAuto" onclick="setMode('auto')">AUTO</button>
      <button id="modeManual" onclick="setMode('manual')">MANUAL</button>
    </div>
    <button class="btn off" id="pumpBtn" onclick="togglePump()">POMPA: OFF</button>
    <div class="hint" id="pumpHint">Mode AUTO aktif &mdash; pompa dikendalikan otomatis oleh sensor.</div>
  </div>

  <div class="card">
    <h2>Kontrol Lampu (Relay 2)</h2>
    <button class="btn off" id="lampBtn" onclick="toggleLamp()">LAMPU: OFF</button>
  </div>

  <div class="card" style="padding:12px 16px">
    <div class="meta"><span>IP: <b id="ip">-</b></span><span>WiFi: <b id="rssi">-</b></span></div>
  </div>

  <footer>ESP32 Local Web Dashboard</footer>
</div>

<script>
const $ = id => document.getElementById(id);
let S = { mode:'AUTO', pump:false, lamp:false };

function setOnline(ok){
  $('linkDot').classList.toggle('online', ok);
  $('linkText').textContent = ok ? 'Online' : 'Offline';
}

function render(d){
  S = d;
  $('waterText').textContent = d.water;
  const card = $('waterCard');
  card.classList.remove('ok','warn','bad');
  let icon = '\uD83C\uDF0A';
  if(d.topWet){ card.classList.add('ok'); icon = '\u2705'; }
  else if(d.bottomWet){ card.classList.add('warn'); icon = '\u26A0\uFE0F'; }
  else { card.classList.add('bad'); icon = '\uD83D\uDEB1'; }
  $('waterIcon').textContent = icon;

  $('topDot').className = 'dot ' + (d.topWet ? 'on' : '');
  $('botDot').className = 'dot ' + (d.bottomWet ? 'on' : '');

  const auto = (d.mode === 'AUTO');
  $('modeAuto').classList.toggle('active', auto);
  $('modeManual').classList.toggle('active', !auto);

  const pb = $('pumpBtn');
  pb.textContent = 'POMPA: ' + (d.pump ? 'ON' : 'OFF');
  pb.className = 'btn ' + (d.pump ? 'on' : 'off');
  pb.disabled = auto;                       // tombol manual nonaktif saat AUTO
  $('pumpHint').style.display = auto ? 'block' : 'none';

  const lb = $('lampBtn');
  lb.textContent = 'LAMPU: ' + (d.lamp ? 'ON' : 'OFF');
  lb.className = 'btn ' + (d.lamp ? 'on' : 'off');

  $('ip').textContent = d.ip || '-';
  $('rssi').textContent = (d.rssi !== undefined ? d.rssi + ' dBm' : '-');
}

async function refresh(){
  try{
    const r = await fetch('/api/status', { cache:'no-store' });
    render(await r.json());
    setOnline(true);
  }catch(e){ setOnline(false); }
}

async function cmd(url){
  try{ await fetch(url, { cache:'no-store' }); }catch(e){}
  refresh();
}

function setMode(m){ cmd('/api/mode?value=' + m); }
function togglePump(){ cmd('/api/pump?state=' + (S.pump ? 'off' : 'on')); }
function toggleLamp(){ cmd('/api/lamp?state=' + (S.lamp ? 'off' : 'on')); }

setInterval(refresh, 1000);   // auto-refresh tiap 1 detik tanpa reload halaman
refresh();
</script>
</body>
</html>
)HTMLDOC";

/* ==========================================================================
 *  7. FUNGSI SENSOR & LOGIKA
 * ========================================================================== */

/* Baca satu probe dengan debounce. LOW = terendam air. */
void pollProbe(ProbeState &p) {
  bool raw = (digitalRead(p.pin) == LOW);      // LOW -> air menyentuh probe

  if (raw != p.rawPrev) {
    p.rawPrev  = raw;
    p.lastFlip = millis();
  } else if (p.wet != raw && (millis() - p.lastFlip) >= PROBE_DEBOUNCE_MS) {
    p.wet = raw;                               // commit setelah stabil
  }
}

/* Hitung state relay pompa sesuai mode. */
void computePumpState() {
  bool topWet    = probeTop.wet;
  bool bottomWet = probeBottom.wet;

  if (pumpMode == MODE_AUTO) {
    // --- Histeresis AUTO ---
    if (topWet) {
      autoLatch = true;                        // air menyentuh Batas Atas -> POMPA ON
    }
    if (!topWet && !bottomWet) {
      autoLatch = false;                       // Batas Bawah kering -> POMPA OFF (dry-run)
    }
    pumpOn = autoLatch;                        // tetap ON walau air di antara atas & bawah
  } else {
    pumpOn = manualPump;                       // mode MANUAL: ikuti tombol web
  }
}

/* Tulis state ke pin relay. */
void applyRelays() {
  digitalWrite(PIN_RELAY_PUMP, pumpOn ? RELAY_ON : RELAY_OFF);
  digitalWrite(PIN_RELAY_LAMP, lampOn ? RELAY_ON : RELAY_OFF);
}

/* Susun JSON status untuk endpoint /api/status. */
String buildStatusJson() {
  bool topWet    = probeTop.wet;
  bool bottomWet = probeBottom.wet;

  String water;
  if (topWet)         water = "Penuh / Siap Sedot";   // Batas Atas terendam
  else if (bottomWet) water = "Sedang Berkurang";     // air di antara atas & bawah
  else                water = "Kritis / Kering";      // Batas Bawah kering

  String j = "{";
  j += "\"topWet\":";    j += (topWet ? "true" : "false");     j += ",";
  j += "\"bottomWet\":"; j += (bottomWet ? "true" : "false");  j += ",";
  j += "\"water\":\"";   j += water;                           j += "\",";
  j += "\"mode\":\"";    j += (pumpMode == MODE_AUTO ? "AUTO" : "MANUAL"); j += "\",";
  j += "\"pump\":";      j += (pumpOn ? "true" : "false");     j += ",";
  j += "\"lamp\":";      j += (lampOn ? "true" : "false");     j += ",";
  j += "\"rssi\":";      j += String(WiFi.RSSI());             j += ",";
  j += "\"ip\":\"";      j += WiFi.localIP().toString();       j += "\"";
  j += "}";
  return j;
}

/* ==========================================================================
 *  8. SETUP
 * ========================================================================== */
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== PEMANTAU AIR - ESP32 ===");

  // Relay: set ke OFF lebih dulu supaya tidak ada "glitch" saat boot.
  pinMode(PIN_RELAY_PUMP, OUTPUT);
  pinMode(PIN_RELAY_LAMP, OUTPUT);
  digitalWrite(PIN_RELAY_PUMP, RELAY_OFF);
  digitalWrite(PIN_RELAY_LAMP, RELAY_OFF);

  // Probe sebagai input pull-up.
  pinMode(PIN_PROBE_TOP, INPUT_PULLUP);
  pinMode(PIN_PROBE_BOTTOM, INPUT_PULLUP);

  // Ambil kondisi awal probe (tanpa debounce saat boot).
  probeTop.rawPrev    = probeTop.wet    = (digitalRead(PIN_PROBE_TOP)    == LOW);
  probeBottom.rawPrev = probeBottom.wet = (digitalRead(PIN_PROBE_BOTTOM) == LOW);

  // --- Koneksi WiFi ---
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

#if USE_STATIC_IP
  // Terapkan IP statis SEBELUM WiFi.begin()
  if (WiFi.config(STATIC_IP, STATIC_GATEWAY, STATIC_SUBNET, STATIC_DNS)) {
    Serial.print("IP statis: ");
    Serial.println(STATIC_IP);
  } else {
    Serial.println("Gagal set IP statis, memakai DHCP.");
  }
#endif

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Menghubungkan ke WiFi: ");
  Serial.print(WIFI_SSID);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi TERHUBUNG! Buka browser ke: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi belum tersambung. Sistem akan mencoba ulang otomatis.");
  }

  // --- Rute Web Server ---
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", index_html);
  });

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", buildStatusJson());
  });

  // Atur mode pompa: /api/mode?value=auto | manual
  server.on("/api/mode", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("value")) {
      String v = request->getParam("value")->value();
      if (v == "auto") {
        pumpMode = MODE_AUTO;
        Serial.println("[WEB] Mode -> AUTO");
      } else if (v == "manual") {
        pumpMode   = MODE_MANUAL;
        manualPump = false;      // aman: pompa OFF saat pindah ke manual
        Serial.println("[WEB] Mode -> MANUAL");
      }
    }
    request->send(200, "application/json", buildStatusJson());
  });

  // Toggle pompa manual: /api/pump?state=on | off  (hanya berlaku saat MANUAL)
  server.on("/api/pump", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (pumpMode == MODE_MANUAL && request->hasParam("state")) {
      manualPump = (request->getParam("state")->value() == "on");
      Serial.print("[WEB] Pompa manual -> ");
      Serial.println(manualPump ? "ON" : "OFF");
    }
    request->send(200, "application/json", buildStatusJson());
  });

  // Toggle lampu: /api/lamp?state=on | off
  server.on("/api/lamp", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("state")) {
      lampOn = (request->getParam("state")->value() == "on");
      Serial.print("[WEB] Lampu -> ");
      Serial.println(lampOn ? "ON" : "OFF");
    }
    request->send(200, "application/json", buildStatusJson());
  });

  server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "404 - Tidak ditemukan");
  });

  server.begin();
  Serial.println("Web server aktif pada port 80.");
}

/* ==========================================================================
 *  9. LOOP
 * ========================================================================== */
void loop() {
  unsigned long now = millis();

  // --- Sampling sensor + update relay ---
  static unsigned long lastSample = 0;
  if (now - lastSample >= SENSOR_SAMPLE_MS) {
    lastSample = now;
    pollProbe(probeTop);
    pollProbe(probeBottom);
    computePumpState();
    applyRelays();
  }

  // --- Jaga koneksi WiFi ---
  static unsigned long lastWifiCheck = 0;
  if (now - lastWifiCheck >= WIFI_RETRY_MS) {
    lastWifiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi terputus, mencoba menyambung ulang...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }
}
