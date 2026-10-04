# Pemantau Air — ESP32 Water Pump & Light Controller

Sistem kontrol pompa air sumur bor (pipa sempit) + lampu rumah berbasis **Web Server Lokal**
di ESP32. Membaca ketinggian air dengan **3 kawat elektroda** dan mengendalikan **2 channel relay**.

---

## 1. Daftar Library

### Arduino IDE
1. **Board package ESP32** — `Boards Manager` → cari **esp32** by *Espressif Systems* → install.
2. **AsyncTCP** — `Library Manager` → cari **AsyncTCP** by *ESP32Async* → install.
3. **ESPAsyncWebServer** — `Library Manager` → cari **ESPAsyncWebServer** by *ESP32Async* → install.
   - Jika muncul dialog "install dependencies", pilih **Install all**.

> **Penting:** gunakan **AsyncTCP** dan **ESPAsyncWebServer** dari author **ESP32Async**
> (fork yang masih dirawat). Versi keduanya harus sepadan — install edisi terbaru keduanya sekaligus.
> Jangan campur dengan versi lama `me-no-dev` karena sering gagal compile.

### PlatformIO
Cukup build — `platformio.ini` sudah berisi `lib_deps` untuk kedua library di atas:

```ini
lib_deps =
    https://github.com/ESP32Async/AsyncTCP.git
    https://github.com/ESP32Async/ESPAsyncWebServer.git
```

Pengaturan board (Arduino IDE): **ESP32 Dev Module**, Flash Size **4MB**, Partition Scheme
**Default 4MB with spiffs**, Upload Speed **921600**, Monitor Speed **115200**.

---

## 2. Tabel Wiring / Pinout

### Sensor elektroda (water level probe)

| Kawat | Terhubung ke | Keterangan |
|-------|--------------|------------|
| Probe **GROUND** | **GND** ESP32 | Kawat telanjang, dicelup **paling dasar** (selalu di bawah air) |
| Probe **BATAS BAWAH** | **GPIO 33** | Mode `INPUT_PULLUP` |
| Probe **BATAS ATAS** | **GPIO 32** | Mode `INPUT_PULLUP` |

Sifat air menghantarkan listrik: saat air menyentuh probe → pin terbaca **LOW**;
saat air turun/kering → pin terbaca **HIGH**.

### Modul Relay 2 Channel

| Pin Modul Relay | Terhubung ke | Keterangan |
|-----------------|--------------|------------|
| **VCC** | **5V / VIN** ESP32 | Kumparan relay butuh 5V |
| **GND** | **GND** ESP32 | Ground harus **sama** dengan ESP32 |
| **IN1** | **GPIO 26** | Kontrol **Pompa Air** (Relay 1) |
| **IN2** | **GPIO 27** | Kontrol **Lampu Rumah** (Relay 2) |

### Beban AC (Pompa & Lampu)

| Beban | Sambungan |
|-------|-----------|
| Pompa Air | Fasa (L) sumber → **COM** Relay 1, **NO** Relay 1 → Pompa → Netral (N) |
| Lampu | Fasa (L) sumber → **COM** Relay 2, **NO** Relay 2 → Lampu → Netral (N) |

Gunakan kontak **COM–NO** supaya beban OFF saat tidak diaktifkan.

### Ringkasan Pin ESP32

```
GPIO 32  -> Probe Batas Atas   (INPUT_PULLUP)
GPIO 33  -> Probe Batas Bawah  (INPUT_PULLUP)
GND      -> Probe Ground (kawat ke-3)
GPIO 26  -> Relay 1 IN1 (Pompa)
GPIO 27  -> Relay 2 IN2 (Lampu)
5V/VIN   -> Relay VCC
GND      -> Relay GND
```

> Pin 32 & 33 dipilih karena aman, mendukung `INPUT_PULLUP`, dan bukan pin strapping/boot.
> Pin 26 & 27 aman sebagai output dan tidak mengganggu proses boot.

---

## 3. Logika Sistem

- **Mode AUTO (histeresis):**
  - Air menyentuh **Batas Atas** → pompa **ON**.
  - Pompa tetap ON walau air mulai turun meninggalkan Batas Atas.
  - Air turun sampai **Batas Bawah kering** (kedua probe HIGH) → pompa **OFF** (proteksi dry-run).
  - Pompa **tidak** menyala lagi sampai air kembali menyentuh Batas Atas.
- **Mode MANUAL:** pompa dinyalakan/dimatikan dari tombol web. Tombol pompa otomatis
  **nonaktif** saat mode AUTO aktif. Berpindah ke MANUAL selalu memaksa pompa OFF (aman).
- **Lampu:** murni manual dari dashboard, bebas ON/OFF kapan saja.
- **Status air di dashboard:**
  - `Penuh / Siap Sedot` — Batas Atas terendam.
  - `Sedang Berkurang` — air di antara Batas Atas & Batas Bawah.
  - `Kritis / Kering` — Batas Bawah kering.

Dashboard memakai **Fetch API setiap 1 detik** (auto-refresh tanpa reload halaman).

### API Endpoint

| Endpoint | Fungsi |
|----------|--------|
| `GET /` | Halaman dashboard |
| `GET /api/status` | JSON status lengkap |
| `GET /api/mode?value=auto\|manual` | Ubah mode pompa |
| `GET /api/pump?state=on\|off` | Toggle pompa (hanya aktif di mode MANUAL) |
| `GET /api/lamp?state=on\|off` | Toggle lampu |

---

## 4. Catatan Keselamatan & Perawatan

- **Bahaya tegangan AC 220V!** Lakukan wiring beban saat listrik dimatikan. Pastikan semua
  sambungan AC terisolasi rapat dan relay berada di dalam kotak tertutup. Jangan sentuh
  rangkaian saat menyala.
- Pompa adalah beban induktif dengan arus start besar. Gunakan modul relay yang sanggup
  menangani arus pompa, atau lebih aman pakai **SSR / kontaktor**. Pasang **snubber/MOV**
  bila perlu untuk meredam spike.
- Modul relay umumnya **Active-LOW**. Jika relay Anda Active-HIGH, cukup tukar di kode:
  `#define RELAY_ON HIGH` dan `#define RELAY_OFF LOW`.
- Beberapa modul relay ber-optocoupler punya jumper **JD-VCC**. Untuk isolasi penuh:
  lepas jumper, beri **JD-VCC = 5V** dari supply terpisah, dan satukan GND supply 5V dengan GND ESP32.
- Jika kabel probe panjang dan terbaca tidak stabil, tambahkan resistor pull-up eksternal
  **10kΩ** ke 3.3V dan kapasitor kecil 100nF dari pin ke GND.
- Elektroda DC dapat mengalami **korosi/elektrolisis**. Gunakan kawat **stainless steel**
  (atau batang karbon) dan bersihkan berkala.

---

## 5. Cara Pakai

1. Salin `src/secrets.example.h` menjadi `src/secrets.h`, lalu isi kredensial WiFi Anda:
   - `WIFI_SSID`, `WIFI_PASSWORD`
   - (Opsional) IP statis: set `USE_STATIC_IP 1` dan sesuaikan `STATIC_IP`
     (mis. `192.168.1.118`), `STATIC_GATEWAY` (IP router), `STATIC_SUBNET`, `STATIC_DNS`.
   **Catatan:** `src/secrets.h` sudah masuk `.gitignore` — jangan commit file ini.
   Jika `secrets.h` belum dibuat, kode otomatis memakai versi contoh (placeholder).
2. Upload ke ESP32.
3. Buka **Serial Monitor (115200)** — akan tampil `WiFi TERHUBUNG! Buka browser ke: http://<IP>`.
4. Dari HP, buka alamat IP tersebut. Dashboard akan ter-update otomatis tiap 1 detik.

> Jika upload gagal (`No serial data received` / `timed out`), `upload_speed` di
> `platformio.ini` sudah diturunkan ke **115200** agar stabil di chip CP2102. Bisa
> diturunkan lagi ke 74880 bila perlu.
