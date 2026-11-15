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
 *  - Buzzer aktif berbunyi tiap aksi (pompa/lampu/mode), secara non-blocking.
 *  - Dashboard web diakses dari HP melalui browser (IP ditampilkan di Serial).
 *  - Log riwayat aksi (jam NTP, metode AUTO/MANUAL, durasi hidup) disimpan di
 *    ring buffer RAM (60 event, tertua otomatis terbuang) -> card "Riwayat"
 *    di dashboard + endpoint /api/logs. Sengaja TIDAK disimpan ke flash
 *    supaya tidak memboroskan siklus tulis EEPROM.
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
#include <time.h>

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
#define PIN_BUZZER         25   // Buzzer aktif (kaki +)

/* ==========================================================================
 *  3. LOGIKA RELAY
 *  Sebagian besar modul relay = Active-LOW.
 *  Jika relay Anda Active-HIGH, cukup tukar kedua nilai di bawah ini saja.
 * ========================================================================== */
#define RELAY_ON    LOW
#define RELAY_OFF   HIGH

/* ==========================================================================
 *  3b. BUZZER (active buzzer)
 *  Tukar BUZZER_ON/OFF jika modul buzzer Anda polaritasnya terbalik.
 *  Pola beep: pompa ON 2x, pompa OFF 1x, lampu 1x, ganti mode 2x.
 * ========================================================================== */
#define BUZZER_ON       HIGH   // active buzzer: HIGH = bunyi
#define BUZZER_OFF      LOW
#define BEEP_ON_MS      80     // durasi tiap beep (ms)
#define BEEP_GAP_MS     120    // jeda antar beep (ms)
#define BEEP_PUMP_ON    2      // jumlah beep saat pompa ON
#define BEEP_PUMP_OFF   1      // jumlah beep saat pompa OFF
#define BEEP_LAMP       1      // jumlah beep saat lampu berubah
#define BEEP_MODE       2      // jumlah beep saat ganti mode

/* ==========================================================================
 *  4. PARAMETER SISTEM
 * ========================================================================== */
#define SENSOR_SAMPLE_MS   50    // interval pembacaan sensor (ms)
#define PROBE_DEBOUNCE_MS  400   // redam riak air / noise (ms)
#define WIFI_RETRY_MS      10000 // interval coba sambung ulang WiFi (ms)

/* --- Waktu (NTP) untuk jam log & dashboard --- */
#define TZ_OFFSET_SEC      (7 * 3600)     // WIB = UTC+7 (Asia/Jakarta, tanpa DST)
#define NTP_SERVER_1       "pool.ntp.org"
#define NTP_SERVER_2       "time.google.com"

/* --- Log riwayat (ring buffer di RAM, tanpa flash) --- */
#define LOG_CAPACITY       60   // maksimum event di RAM (~1 KB); tertua terbuang otomatis
#define LOG_API_DEFAULT    20   // jumlah default event yang dikirim oleh /api/logs

/* ==========================================================================
 *  5. OBJEK & STATE GLOBAL
 * ========================================================================== */
AsyncWebServer server(80);

enum PumpMode { MODE_AUTO, MODE_MANUAL, MODE_CUSTOM };

PumpMode pumpMode   = MODE_AUTO;  // mode awal pompa
bool     autoLatch  = false;      // latch histeresis AUTO
bool     manualPump = false;      // state tombol manual pompa
bool     lampOn     = false;      // state lampu
bool     pumpOn     = false;      // state akhir relay pompa

/* --- Custom Repeat Timer --- */
unsigned long customIntervalMs = 20UL * 60UL * 1000UL; // jeda antar penyalaan (default 20 menit)
unsigned long customDurationMs =  1UL * 60UL * 1000UL; // lama nyala tiap siklus (default 1 menit)
bool          customActive     = false;                // sedang dalam fase ON
unsigned long customPhaseStart = 0;                    // millis() mulai fase aktif/non-aktif

/* --- State buzzer (non-blocking, berbasis millis) --- */
uint8_t       beepQueue  = 0;     // sisa jumlah beep
bool          beepActive = false; // sedang bunyi
unsigned long beepTimer  = 0;

/* --- State probe (dengan debounce) --- */
struct ProbeState {
  uint8_t       pin;
  bool          wet;        // true = terendam air (LOW)
  bool          rawPrev;    // pembacaan mentah sebelumnya
  unsigned long lastFlip;   // waktu perubahan terakhir
};

ProbeState probeTop    = { PIN_PROBE_TOP,    false, false, 0 };
ProbeState probeBottom = { PIN_PROBE_BOTTOM, false, false, 0 };

/* --- State log riwayat (ring buffer di RAM) --- */
enum LogEv  : uint8_t { EV_PUMP_ON, EV_PUMP_OFF, EV_MODE, EV_LAMP };
enum LogMeth: uint8_t { METH_AUTO, METH_MANUAL, METH_CUSTOM, METH_NONE };
enum LogRsn : uint8_t { R_NONE, R_ATAS, R_KERING, R_WEB, R_MODECHG, R_REPEAT, R_ON, R_OFF };

struct LogEvent {
  uint32_t epoch;      // detik Unix; 0 = belum sinkron NTP
  uint32_t uptimeMs;   // millis() saat kejadian (cadangan / back-fill)
  uint16_t durSec;     // durasi sesi pompa (khusus EV_PUMP_OFF)
  uint8_t  ev;
  uint8_t  method;
  uint8_t  reason;
};

LogEvent        logBuf[LOG_CAPACITY];
uint16_t        logCount = 0;   // jumlah entri valid (maks LOG_CAPACITY)
uint16_t        logHead  = 0;   // index tujuan tulis berikutnya
portMUX_TYPE    logMux   = portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t pumpOffReason = R_NONE;  // alasan pompa OFF non-sensor

/* --- Statistik (durasi & jumlah nyala pompa) --- */
uint32_t todayMs = 0, totalMs = 0;        // akumulasi durasi (ms)
uint16_t todayCount = 0, totalCount = 0;  // jumlah sesi nyala

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
.stats{display:flex;gap:8px;margin-bottom:8px;}
.stat{flex:1;background:rgba(255,255,255,.03);border:1px solid var(--border);border-radius:10px;
padding:8px 4px;text-align:center;}
.stat b{display:block;font-size:.92rem;font-weight:700;font-variant-numeric:tabular-nums;}
.stat span{font-size:.62rem;text-transform:uppercase;letter-spacing:.6px;color:var(--muted);}
.logrow{display:flex;align-items:center;gap:7px;padding:7px 2px;font-size:.78rem;
border-bottom:1px dashed rgba(255,255,255,.07);flex-wrap:wrap;}
.logrow:last-child{border-bottom:none;}
.logrow .ts{color:var(--muted);font-variant-numeric:tabular-nums;white-space:nowrap;}
.logrow .ev{flex:1;min-width:80px;}
.rzn{color:var(--muted);font-size:.7rem;}
.badge{font-size:.62rem;padding:2px 7px;border-radius:99px;font-weight:700;letter-spacing:.4px;}
.badge.AUTO{background:rgba(56,189,248,.16);color:var(--accent);}
.badge.MANUAL{background:rgba(245,158,11,.16);color:var(--amber);}
.logrow .dur{color:var(--green);font-weight:600;white-space:nowrap;margin-left:auto;}
.empty{font-size:.78rem;color:var(--muted);text-align:center;padding:6px 0;}
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
      <button id="modeCustom" onclick="setMode('custom')">CUSTOM</button>
    </div>
    <button class="btn off" id="pumpBtn" onclick="togglePump()">POMPA: OFF</button>
    <div class="hint" id="pumpHint">Mode AUTO aktif &mdash; pompa dikendalikan otomatis oleh sensor.</div>
    <div class="hint" id="customHint">Mode CUSTOM aktif &mdash; pompa nyala berulang sesuai interval & durasi.</div>
  </div>

  <div class="card" id="customCard">
    <h2>Pengaturan Custom Repeat</h2>
    <div style="display:flex;flex-direction:column;gap:10px">
      <div style="display:flex;gap:8px;align-items:center">
        <div style="flex:1">
          <div style="font-size:.72rem;color:var(--muted);margin-bottom:4px">Interval (jeda antar nyala)</div>
          <div style="display:flex;gap:6px;align-items:center">
            <input id="intMin" type="number" min="0" max="720" step="1" placeholder="20" style="width:70px;padding:8px;border-radius:10px;border:1px solid var(--border);background:rgba(255,255,255,.05);color:var(--text);text-align:center">
            <span style="font-size:.78rem;color:var(--muted)">menit</span>
            <input id="intSec" type="number" min="0" max="59" step="1" placeholder="0" style="width:60px;padding:8px;border-radius:10px;border:1px solid var(--border);background:rgba(255,255,255,.05);color:var(--text);text-align:center">
            <span style="font-size:.78rem;color:var(--muted)">detik</span>
          </div>
        </div>
        <div style="flex:1">
          <div style="font-size:.72rem;color:var(--muted);margin-bottom:4px">Durasi Nyala</div>
          <div style="display:flex;gap:6px;align-items:center">
            <input id="durMin" type="number" min="0" max="720" step="1" placeholder="1" style="width:70px;padding:8px;border-radius:10px;border:1px solid var(--border);background:rgba(255,255,255,.05);color:var(--text);text-align:center">
            <span style="font-size:.78rem;color:var(--muted)">menit</span>
            <input id="durSec" type="number" min="0" max="59" step="1" placeholder="0" style="width:60px;padding:8px;border-radius:10px;border:1px solid var(--border);background:rgba(255,255,255,.05);color:var(--text);text-align:center">
            <span style="font-size:.78rem;color:var(--muted)">detik</span>
          </div>
        </div>
      </div>
      <div style="display:flex;justify-content:space-between;align-items:center;font-size:.78rem;color:var(--muted);padding:4px 2px">
        <div>
          <div id="customInfo">Interval 20m 0s · Durasi 1m 0s · Siklus: OFF</div>
          <div id="customDirty" style="display:none;color:#fbbf24;margin-top:4px">&#9998; Belum disimpan</div>
        </div>
        <button id="customSave" onclick="applyCustom()" style="padding:8px 12px;border-radius:10px;border:none;background:var(--accent);color:#04121f;font-weight:700;cursor:pointer">Simpan</button>
      </div>
    </div>
  </div>

  <div class="card">
    <h2>Kontrol Lampu (Relay 2)</h2>
    <button class="btn off" id="lampBtn" onclick="toggleLamp()">LAMPU: OFF</button>
  </div>

  <div class="card">
    <h2>Riwayat</h2>
    <div class="stats">
      <div class="stat"><b id="stClock">--:--:--</b><span>Jam</span></div>
      <div class="stat"><b id="stToday">0s</b><span>Hari ini</span></div>
      <div class="stat"><b id="stCount">0x</b><span>Kali nyala</span></div>
    </div>
    <div id="logList"><div class="empty">Memuat riwayat...</div></div>
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

function fmtSec(sec){
  sec = Math.max(0, Math.floor(sec || 0));
  const m = Math.floor(sec / 60);
  const s = sec % 60;
  return {m,s};
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
  const manual = (d.mode === 'MANUAL');
  const custom = (d.mode === 'CUSTOM');
  $('modeAuto').classList.toggle('active', auto);
  $('modeManual').classList.toggle('active', manual);
  $('modeCustom').classList.toggle('active', custom);

  const pb = $('pumpBtn');
  pb.textContent = 'POMPA: ' + (d.pump ? 'ON' : 'OFF');
  pb.className = 'btn ' + (d.pump ? 'on' : 'off');
  pb.disabled = auto || custom;                       // tombol manual nonaktif saat AUTO/CUSTOM
  $('pumpHint').style.display = auto ? 'block' : 'none';
  $('customHint').style.display = custom ? 'block' : 'none';

  if ($('customCard')) $('customCard').style.display = custom ? 'block' : 'block'; // selalu tampil, biar bisa setting kapan aja

  const lb = $('lampBtn');
  lb.textContent = 'LAMPU: ' + (d.lamp ? 'ON' : 'OFF');
  lb.className = 'btn ' + (d.lamp ? 'on' : 'off');

  $('ip').textContent = d.ip || '-';
  $('rssi').textContent = (d.rssi !== undefined ? d.rssi + ' dBm' : '-');

  // Custom form
  if (d.customIntervalMs !== undefined && d.customDurationMs !== undefined) {
    const iv = Math.floor(d.customIntervalMs / 1000);
    const dv = Math.floor(d.customDurationMs / 1000);
    const fiv = fmtSec(iv);
    const fdv = fmtSec(dv);
    // Jangan timpa isian yang sedang diedit / belum disimpan oleh pengguna.
    if (!customDirty) {
      $('intMin').value = fiv.m;
      $('intSec').value = fiv.s;
      $('durMin').value = fdv.m;
      $('durSec').value = fdv.s;
    }
    let info = 'Interval ' + fiv.m + 'm ' + fiv.s + 's · Durasi ' + fdv.m + 'm ' + fdv.s + 's';
    if (custom) {
      const elapsed = (d.uptime * 1000 - (d.customPhaseStart || 0));
      if (d.customActive) {
        const left = Math.max(0, dv - Math.floor(elapsed/1000));
        info += ' · Siklus: ON (sisa ' + left + 'd)';
      } else {
        const left = Math.max(0, iv - Math.floor(elapsed/1000));
        info += ' · Siklus: OFF (sisa ' + left + 'd)';
      }
    } else {
      info += ' · Siklus: OFF';
    }
    if ($('customInfo')) $('customInfo').textContent = info;
  }
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
// customDirty = true selama pengguna mengubah form Custom Repeat dan belum menyimpan.
// Selama itu, auto-refresh tidak menimpa nilai di input.
let customDirty = false;
function setCustomDirty(v){
  customDirty = v;
  $('customDirty').style.display = v ? 'block' : 'none';
}
['intMin','intSec','durMin','durSec'].forEach(function(id){
  $(id).addEventListener('input', function(){ setCustomDirty(true); });
});

async function applyCustom(){
  const num = id => Math.max(0, parseInt($(id).value, 10) || 0);
  const ivSec = num('intMin')*60 + num('intSec');
  const dvSec = num('durMin')*60 + num('durSec');
  if (ivSec < 1 || dvSec < 1){
    alert('Interval dan durasi harus minimal 1 detik.');
    return;
  }
  const btn = $('customSave');
  btn.disabled = true; btn.textContent = 'Menyimpan...';
  try{
    const r = await fetch('/api/custom?intervalSec=' + ivSec + '&durationSec=' + dvSec, { cache:'no-store' });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    setCustomDirty(false);           // tersimpan -> boleh disinkron lagi dari server
    render(await r.json());
    btn.textContent = 'Tersimpan ✓';
  }catch(e){
    btn.textContent = 'Gagal, coba lagi';   // isian tetap dipertahankan
  }
  setTimeout(function(){ btn.disabled = false; btn.textContent = 'Simpan'; }, 1500);
}

const EVLABEL = { pump_on:'Pompa ON', pump_off:'Pompa OFF', mode:'Mode', lamp:'Lampu' };

function fmtDur(sec){
  sec = Math.max(0, Math.round(sec));
  if (sec < 60) return sec + ' dtk';
  const h = Math.floor(sec/3600), m = Math.floor((sec%3600)/60), s = sec%60;
  if (h) return h + 'j ' + String(m).padStart(2,'0') + 'm';
  return m + 'm ' + String(s).padStart(2,'0') + 's';
}

async function refreshLogs(){
  try{
    const r = await fetch('/api/logs?n=20', { cache:'no-store' });
    const d = await r.json();
    $('stClock').textContent = d.timeOk ? (d.clock || '--:--:--') : '...';
    $('stToday').textContent = fmtDur(d.todayMs/1000);
    $('stCount').textContent = d.todayCount + 'x';
    const items = (d.items || []).slice(-8).reverse();   // 8 terbaru di atas
    if (!items.length){
      $('logList').innerHTML = '<div class="empty">Belum ada riwayat</div>';
      return;
    }
    $('logList').innerHTML = items.map(function(it){
      let h = '<div class="logrow"><span class="ts">' + it.ts + '</span>';
      h += '<span class="ev">' + (EVLABEL[it.ev] || it.ev);
      if (it.r) h += ' <span class="rzn">&middot; ' + it.r + '</span>';
      h += '</span>';
      if (it.m) h += '<span class="badge ' + it.m + '">' + it.m + '</span>';
      if (it.d > 0) h += '<span class="dur">' + fmtDur(it.d) + '</span>';
      return h + '</div>';
    }).join('');
  }catch(e){}
}

setInterval(refresh, 1000);   // auto-refresh tiap 1 detik tanpa reload halaman
setInterval(refreshLogs, 5000);
refresh();
refreshLogs();
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
      if (autoLatch) pumpOffReason = R_KERING;   // alasan OFF: proteksi dry-run
      autoLatch = false;                         // Batas Bawah kering -> POMPA OFF
    }
    pumpOn = autoLatch;                        // tetap ON walau air di antara atas & bawah
  } else if (pumpMode == MODE_MANUAL) {
    pumpOn = manualPump;                       // mode MANUAL: ikuti tombol web
  } else if (pumpMode == MODE_CUSTOM) {
    unsigned long now = millis();
    if (customIntervalMs == 0) customIntervalMs = 20UL * 60UL * 1000UL;
    if (customDurationMs == 0) customDurationMs = 1UL * 60UL * 1000UL;
    if (!customActive) {
      if (now - customPhaseStart >= customIntervalMs) {
        customActive     = true;
        customPhaseStart = now;
        pumpOffReason    = R_REPEAT;
        pumpOn           = true;
      } else {
        pumpOn = false;
      }
    } else {
      if (now - customPhaseStart >= customDurationMs) {
        customActive     = false;
        customPhaseStart = now;
        pumpOffReason    = R_REPEAT;
        pumpOn           = false;
      } else {
        pumpOn = true;
      }
    }
  } else {
    pumpOn = false;
  }
}

/* Tulis state ke pin relay. */
void applyRelays() {
  digitalWrite(PIN_RELAY_PUMP, pumpOn ? RELAY_ON : RELAY_OFF);
  digitalWrite(PIN_RELAY_LAMP, lampOn ? RELAY_ON : RELAY_OFF);
}

/* ==========================================================================
 *  7b. WAKTU (NTP) & LOG RIWAYAT
 * ========================================================================== */

/* True jika jam sudah tersinkron NTP (epoch > 2020-09-13). */
bool timeOk() {
  return time(nullptr) > 1600000000UL;
}

/* "HH:MM:SS" sesuai timezone; "" bila belum sinkron. */
String fmtClock() {
  if (!timeOk()) return "";
  time_t t = time(nullptr);
  struct tm ti;
  localtime_r(&t, &ti);
  char b[12];
  snprintf(b, sizeof(b), "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
  return String(b);
}

/* Stempel log: "DD/MM HH:MM:SS" dari epoch; fallback "+HH:MM:SS" dari uptime. */
String fmtStamp(uint32_t epoch, uint32_t uptimeMs) {
  char b[24];
  if (epoch) {
    time_t t = epoch;
    struct tm ti;
    localtime_r(&t, &ti);
    snprintf(b, sizeof(b), "%02d/%02d %02d:%02d:%02d",
             ti.tm_mday, ti.tm_mon + 1, ti.tm_hour, ti.tm_min, ti.tm_sec);
  } else {
    snprintf(b, sizeof(b), "+%02lu:%02lu:%02lu",
             (uptimeMs / 3600000UL) % 100, (uptimeMs / 60000UL) % 60,
             (uptimeMs / 1000UL) % 60);
  }
  return String(b);
}

/* Serial log dengan prefix jam (fallback uptime sebelum NTP sinkron). */
void logLine(const String &msg) {
  char b[24];
  if (timeOk()) {
    time_t t = time(nullptr);
    struct tm ti;
    localtime_r(&t, &ti);
    snprintf(b, sizeof(b), "[%02d:%02d:%02d] ", ti.tm_hour, ti.tm_min, ti.tm_sec);
  } else {
    unsigned long ms = millis();
    snprintf(b, sizeof(b), "[%02lu:%02lu:%02lu] ",
             (ms / 3600000UL) % 100, (ms / 60000UL) % 60, (ms / 1000UL) % 60);
  }
  Serial.print(b);
  Serial.println(msg);
}

const char *evName(uint8_t ev) {
  switch (ev) {
    case EV_PUMP_ON:  return "pump_on";
    case EV_PUMP_OFF: return "pump_off";
    case EV_MODE:     return "mode";
    default:          return "lamp";
  }
}

const char *methName(uint8_t m) {
  switch (m) {
    case METH_AUTO:   return "AUTO";
    case METH_MANUAL: return "MANUAL";
    case METH_CUSTOM: return "CUSTOM";
    default:          return "";
  }
}

const char *rsnName(uint8_t r) {
  switch (r) {
    case R_ATAS:    return "batas atas";
    case R_KERING:  return "bawah kering";
    case R_WEB:     return "tombol web";
    case R_MODECHG: return "ganti mode";
    case R_REPEAT:  return "berulang";
    case R_ON:      return "nyala";
    case R_OFF:     return "mati";
    default:        return "";
  }
}

/* Catat satu event ke ring buffer (panggil dari loop(), aman thd. async). */
void pushLog(uint8_t ev, uint8_t method, uint8_t reason, uint16_t durSec) {
  LogEvent e;
  e.epoch    = timeOk() ? (uint32_t)time(nullptr) : 0;
  e.uptimeMs = millis();
  e.durSec   = durSec;
  e.ev       = ev;
  e.method   = method;
  e.reason   = reason;

  portENTER_CRITICAL(&logMux);
  logBuf[logHead] = e;
  logHead = (uint16_t)((logHead + 1) % LOG_CAPACITY);
  if (logCount < LOG_CAPACITY) logCount++;
  portEXIT_CRITICAL(&logMux);
}

/* Kosongkan riwayat (dipanggil dari /api/logs?clear=1). */
void clearLog() {
  portENTER_CRITICAL(&logMux);
  logCount = 0;
  logHead  = 0;
  portEXIT_CRITICAL(&logMux);
}

/* JSON riwayat: statistik + "want" event terakhir (terbaru di akhir array). */
String buildLogsJson(uint16_t want) {
  uint16_t total, start;
  portENTER_CRITICAL(&logMux);
  total = logCount;
  start = (uint16_t)((logHead + LOG_CAPACITY - (want > total ? total : want)) % LOG_CAPACITY);
  portEXIT_CRITICAL(&logMux);
  if (want > total) want = total;

  bool     tOk   = timeOk();
  uint32_t nowMs = millis();
  uint32_t nowEp = tOk ? (uint32_t)time(nullptr) : 0;

  String j = "{";
  j += "\"clock\":\"";      j += tOk ? fmtClock() : ""; j += "\",";
  j += "\"timeOk\":";       j += tOk ? "true" : "false"; j += ",";
  j += "\"cap\":";          j += String(LOG_CAPACITY);  j += ",";
  j += "\"total\":";        j += String(total);         j += ",";
  j += "\"todayMs\":";      j += String(todayMs);       j += ",";
  j += "\"todayCount\":";   j += String(todayCount);    j += ",";
  j += "\"totalMs\":";      j += String(totalMs);       j += ",";
  j += "\"totalCount\":";   j += String(totalCount);    j += ",";
  j += "\"items\":[";

  for (uint16_t i = 0; i < want; i++) {
    LogEvent e;
    portENTER_CRITICAL(&logMux);
    e = logBuf[(uint16_t)((start + i) % LOG_CAPACITY)];
    portEXIT_CRITICAL(&logMux);

    // Back-fill epoch event lama begitu NTP tersinkron.
    if (e.epoch == 0 && tOk && e.uptimeMs <= nowMs)
      e.epoch = nowEp - (nowMs - e.uptimeMs) / 1000;

    if (i) j += ",";
    j += "{\"ts\":\""; j += fmtStamp(e.epoch, e.uptimeMs); j += "\",";
    j += "\"ev\":\"";  j += evName(e.ev);                  j += "\",";
    j += "\"m\":\"";   j += methName(e.method);            j += "\",";
    j += "\"r\":\"";   j += rsnName(e.reason);             j += "\",";
    j += "\"d\":";     j += String(e.durSec);              j += "}";
  }

  j += "]}";
  return j;
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
  String modeStr;
  switch (pumpMode) {
    case MODE_MANUAL: modeStr = "MANUAL"; break;
    case MODE_CUSTOM: modeStr = "CUSTOM"; break;
    default:          modeStr = "AUTO";   break;
  }
  j += "\"mode\":\"";    j += modeStr;                        j += "\",";
  j += "\"pump\":";      j += (pumpOn ? "true" : "false");     j += ",";
  j += "\"lamp\":";      j += (lampOn ? "true" : "false");     j += ",";
  j += "\"clock\":\"";   j += fmtClock();                      j += "\",";
  j += "\"timeOk\":";    j += (timeOk() ? "true" : "false");   j += ",";
  j += "\"uptime\":";    j += String(millis() / 1000UL);       j += ",";
  j += "\"todayMs\":";   j += String(todayMs);                 j += ",";
  j += "\"todayCount\":";j += String(todayCount);              j += ",";
  j += "\"rssi\":";      j += String(WiFi.RSSI());             j += ",";
  j += "\"ip\":\"";      j += WiFi.localIP().toString();       j += "\",";
  j += "\"customIntervalMs\":"; j += String(customIntervalMs); j += ",";
  j += "\"customDurationMs\":"; j += String(customDurationMs); j += ",";
  j += "\"customActive\":";     j += (customActive ? "true" : "false"); j += ",";
  j += "\"customPhaseStart\":"; j += String(customPhaseStart);
  j += "}";
  return j;
}

/* ==========================================================================
 *  7c. BUZZER: pola beep NON-BLOCKING (tanpa delay)
 * ========================================================================== */

/* Minta bunyi sebanyak "times"; dieksekusi di updateBuzzer(). */
void beep(uint8_t times) {
  beepQueue = times;
}

/* Panggil tiap loop untuk menggerakkan pola beep. */
void updateBuzzer() {
  unsigned long now = millis();

  if (beepActive) {
    if (now - beepTimer >= BEEP_ON_MS) {      // selesai 1 beep
      digitalWrite(PIN_BUZZER, BUZZER_OFF);
      beepActive = false;
      beepTimer  = now;
      if (beepQueue > 0) beepQueue--;
    }
  } else if (beepQueue > 0 && (now - beepTimer >= BEEP_GAP_MS)) {
    digitalWrite(PIN_BUZZER, BUZZER_ON);      // mulai 1 beep
    beepActive = true;
    beepTimer  = now;
  }
}

/* ==========================================================================
 *  8. SETUP
 * ========================================================================== */
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  logLine("=== PEMANTAU AIR - ESP32 ===");

  // Relay: set ke OFF lebih dulu supaya tidak ada "glitch" saat boot.
  pinMode(PIN_RELAY_PUMP, OUTPUT);
  pinMode(PIN_RELAY_LAMP, OUTPUT);
  digitalWrite(PIN_RELAY_PUMP, RELAY_OFF);
  digitalWrite(PIN_RELAY_LAMP, RELAY_OFF);

  // Buzzer: pastikan mati saat boot.
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, BUZZER_OFF);

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
    logLine("WiFi TERHUBUNG! Buka browser ke: http://" + WiFi.localIP().toString());
  } else {
    logLine("WiFi belum tersambung. Sistem akan mencoba ulang otomatis.");
  }

  // --- Waktu (NTP): jam nyata WIB untuk log & statistik harian ---
  // SNTP jalan terus di background; bila WiFi putus lalu tersambung lagi,
  // sinkronisasi ulang otomatis. Sebelum tersinkron, log memakai uptime.
  configTime(TZ_OFFSET_SEC, 0, NTP_SERVER_1, NTP_SERVER_2);
  logLine(String("NTP dijalankan -> ") + NTP_SERVER_1 + " (WIB/UTC+7)");

  // --- Rute Web Server ---
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", index_html);
  });

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", buildStatusJson());
  });

  // Atur mode pompa: /api/mode?value=auto | manual | custom
  server.on("/api/mode", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("value")) {
      String v = request->getParam("value")->value();
      v.toLowerCase();
      if (v == "auto" || v == "manual" || v == "custom") {
        if (v == "manual") {
          if (pumpOn) pumpOffReason = R_MODECHG;
          pumpMode   = MODE_MANUAL;
          manualPump = false;
          logLine("[WEB] Mode -> MANUAL");
        } else if (v == "auto") {
          if (pumpOn && pumpMode != MODE_AUTO) {
            if (!autoLatch) pumpOffReason = R_MODECHG;
          }
          pumpMode = MODE_AUTO;
          logLine("[WEB] Mode -> AUTO");
        } else if (v == "custom") {
          if (pumpOn) pumpOffReason = R_MODECHG;
          pumpMode         = MODE_CUSTOM;
          customActive     = false;
          customPhaseStart = millis();
          logLine("[WEB] Mode -> CUSTOM");
        }
      }
    }
    request->send(200, "application/json", buildStatusJson());
  });

  // Toggle pompa manual: /api/pump?state=on | off  (hanya berlaku saat MANUAL)
  server.on("/api/pump", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (pumpMode == MODE_MANUAL && request->hasParam("state")) {
      bool on = (request->getParam("state")->value() == "on");
      if (!on) pumpOffReason = R_WEB;           // alasan OFF: tombol web
      manualPump = on;
      logLine(String("[WEB] Pompa manual -> ") + (on ? "ON" : "OFF"));
    }
    request->send(200, "application/json", buildStatusJson());
  });

  // Atur custom repeat: /api/custom?intervalSec=1200&durationSec=60 atau intervalMin/durationMin
  server.on("/api/custom", HTTP_GET, [](AsyncWebServerRequest *request) {
    bool changed = false;
    if (request->hasParam("intervalSec")) {
      long v = request->getParam("intervalSec")->value().toInt();
      if (v < 0) v = 0;
      customIntervalMs = (unsigned long)v * 1000UL;
      changed = true;
    }
    if (request->hasParam("durationSec")) {
      long v = request->getParam("durationSec")->value().toInt();
      if (v < 0) v = 0;
      customDurationMs = (unsigned long)v * 1000UL;
      changed = true;
    }
    if (request->hasParam("intervalMin")) {
      long v = request->getParam("intervalMin")->value().toInt();
      if (v < 0) v = 0;
      customIntervalMs = (unsigned long)v * 60UL * 1000UL;
      changed = true;
    }
    if (request->hasParam("durationMin")) {
      long v = request->getParam("durationMin")->value().toInt();
      if (v < 0) v = 0;
      customDurationMs = (unsigned long)v * 60UL * 1000UL;
      changed = true;
    }
    if (changed) {
      if (pumpMode == MODE_CUSTOM) {
        customActive     = false;
        customPhaseStart = millis();
      }
      logLine(String("[WEB] Custom repeat: interval=") + (customIntervalMs/1000UL) + "s dur=" + (customDurationMs/1000UL) + "s");
    }
    request->send(200, "application/json", buildStatusJson());
  });

  // Toggle lampu: /api/lamp?state=on | off
  server.on("/api/lamp", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("state")) {
      lampOn = (request->getParam("state")->value() == "on");
      logLine(String("[WEB] Lampu -> ") + (lampOn ? "ON" : "OFF"));
    }
    request->send(200, "application/json", buildStatusJson());
  });

  // Riwayat aksi: /api/logs?n=JUMLAH   |   /api/logs?clear=1
  server.on("/api/logs", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("clear")) {
      clearLog();
      logLine("[WEB] Riwayat log dikosongkan");
      request->send(200, "application/json", "{\"ok\":true,\"total\":0}");
      return;
    }
    int n = LOG_API_DEFAULT;
    if (request->hasParam("n")) n = request->getParam("n")->value().toInt();
    if (n < 1) n = 1;
    if (n > LOG_CAPACITY) n = LOG_CAPACITY;
    request->send(200, "application/json", buildLogsJson((uint16_t)n));
  });

  server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "404 - Tidak ditemukan");
  });

  server.begin();
  logLine("Web server aktif pada port 80.");
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

  // --- Beep + catat log tiap aksi (otomatis maupun manual) ---
  static PumpMode prevMode = pumpMode;
  static bool     prevPump = pumpOn;
  static bool     prevLamp = lampOn;
  static unsigned long pumpOnSince = 0;

  if (pumpMode != prevMode) {
    beep(BEEP_MODE);
    String modeStr;
    uint8_t modeMeth;
    switch (pumpMode) {
      case MODE_MANUAL: modeStr = "MANUAL"; modeMeth = METH_MANUAL; break;
      case MODE_CUSTOM: modeStr = "CUSTOM"; modeMeth = METH_CUSTOM; break;
      default:          modeStr = "AUTO";   modeMeth = METH_AUTO;   break;
    }
    logLine(String("[LOG] Mode -> ") + modeStr);
    pushLog(EV_MODE, modeMeth, R_NONE, 0);
    if (pumpMode == MODE_CUSTOM) {
      customActive     = false;
      customPhaseStart = millis();
    }
    prevMode = pumpMode;
  }

  if (pumpOn != prevPump) {
    beep(pumpOn ? BEEP_PUMP_ON : BEEP_PUMP_OFF);
      uint8_t meth;
      if (pumpMode == MODE_AUTO) meth = METH_AUTO;
      else if (pumpMode == MODE_MANUAL) meth = METH_MANUAL;
      else meth = METH_CUSTOM;
    if (pumpOn) {
      pumpOnSince = now;
      todayCount++;
      totalCount++;
      logLine(String("[LOG] POMPA ON (") + methName(meth) + ")");
      pushLog(EV_PUMP_ON, meth, pumpMode == MODE_AUTO ? R_ATAS : R_WEB, 0);
    } else {
      unsigned long durMs = now - pumpOnSince;
      unsigned long durSec = durMs / 1000UL;
      if (durSec > 65535UL) durSec = 65535UL;
      todayMs += durMs;
      totalMs += durMs;
      uint8_t rsn = pumpOffReason;             // alasan dari pemicu (web/mode/sensor)
      pumpOffReason = R_NONE;
      if (rsn == R_NONE) {
        if (pumpMode == MODE_AUTO) rsn = R_KERING;
        else if (pumpMode == MODE_MANUAL) rsn = R_WEB;
        else rsn = R_REPEAT;
      }
      logLine(String("[LOG] POMPA OFF (") + methName(meth) + ") durasi " + durSec + "s");
      pushLog(EV_PUMP_OFF, meth, rsn, (uint16_t)durSec);
    }
    prevPump = pumpOn;
  }

  if (lampOn != prevLamp) {
    beep(BEEP_LAMP);
    logLine(String("[LOG] Lampu -> ") + (lampOn ? "ON" : "OFF"));
    pushLog(EV_LAMP, METH_NONE, lampOn ? R_ON : R_OFF, 0);
    prevLamp = lampOn;
  }
  updateBuzzer();

  // --- Statistik harian: reset saat tanggal berganti (butuh jam NTP) ---
  if (timeOk()) {
    struct tm ti;
    time_t t = time(nullptr);
    localtime_r(&t, &ti);
    static int markYear = -1, markYday = -1;
    if (markYear < 0) {
      markYear = ti.tm_year; markYday = ti.tm_yday;
    } else if (ti.tm_year != markYear || ti.tm_yday != markYday) {
      markYear = ti.tm_year; markYday = ti.tm_yday;
      todayMs = 0;
      todayCount = 0;
      logLine("[LOG] Hari berganti -> statistik harian direset");
    }
  }

  // --- Jaga koneksi WiFi ---
  static unsigned long lastWifiCheck = 0;
  if (now - lastWifiCheck >= WIFI_RETRY_MS) {
    lastWifiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      logLine("WiFi terputus, mencoba menyambung ulang...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }
}
