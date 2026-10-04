#pragma once
/* ============================================================================
 *  CONTOH KONFIGURASI  (file ini AMAN untuk di-commit)
 * ============================================================================
 *  Cara pakai:
 *    1) Salin file ini menjadi "secrets.h" (di folder yang sama).
 *    2) Isi WIFI_SSID, WIFI_PASSWORD, dan setting IP dengan nilai asli.
 *  File "secrets.h" sudah di-gitignore sehingga tidak akan ter-commit.
 * ============================================================================ */

/* --- Kredensial WiFi rumah --- */
#define WIFI_SSID       "GANTI_NAMA_WIFI_ANDA"
#define WIFI_PASSWORD   "GANTI_PASSWORD_WIFI_ANDA"

/* --- IP statis (opsional): set 1 untuk IP tetap, 0 untuk DHCP --- */
#define USE_STATIC_IP   0
#define STATIC_IP       IPAddress(192, 168, 1, 118) // IP tetap ESP32
#define STATIC_GATEWAY  IPAddress(192, 168, 1, 1)   // IP router / gateway
#define STATIC_SUBNET   IPAddress(255, 255, 255, 0) // Subnet mask
#define STATIC_DNS      IPAddress(192, 168, 1, 1)   // DNS (biasanya = gateway)
