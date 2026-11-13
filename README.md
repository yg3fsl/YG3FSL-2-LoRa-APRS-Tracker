# YG3FSL-2 LoRa APRS Tracker

Project tracker posisi berbasis GPS dan LoRa. ESP32-C3 membaca koordinat dari GPS NEO-6M, menampilkan informasi di OLED, lalu mengirim beacon APRS melalui modul RA-02 (SX1278).

Firmware yang disertakan adalah versi **v1.2** dengan callsign **YG3FSL-2**.

## Tampilan wiring

![Wiring diagram YG3FSL-2](hardware/wiring-diagram.png)

File versi vektor: [wiring-diagram.svg](hardware/wiring-diagram.svg).

## Komponen

- ESP32-C3 SuperMini
- Modul LoRa Ai-Thinker RA-02 (SX1278)
- GPS u-blox NEO-6M V2
- OLED 1,30 inci V2.2, SH1106 128×64, I2C
- Antena yang sesuai dengan frekuensi kerja radio
- Catu daya 3,3 V yang stabil dan mampu menyuplai kebutuhan RA-02
- Kabel jumper dan konektor
- Kabel USB untuk pemrograman ESP32-C3

**Tambahan yang disarankan:** kapasitor bypass 100 nF dan kapasitor bulk yang sesuai, dipasang dekat modul RA-02. Pastikan kemampuan regulator 3,3 V mencukupi saat radio melakukan transmisi.

## Wiring

### RA-02 SX1278 ke ESP32-C3

| Pin RA-02 | ESP32-C3 | Keterangan |
|---|---|---|
| VCC | 3V3 | Catu 3,3 V |
| GND | GND | Ground |
| SCK | GPIO4 | SPI clock |
| MISO | GPIO5 | Radio ke ESP32 |
| MOSI | GPIO6 | ESP32 ke radio |
| NSS / CS | GPIO7 | SPI chip select |
| RESET | GPIO10 | Reset radio |
| DIO0 | GPIO3 | Interrupt / packet event |

### GPS NEO-6M V2 ke ESP32-C3

| Pin GPS | ESP32-C3 | Keterangan |
|---|---|---|
| VCC | Sesuai spesifikasi breakout | Periksa modul GPS yang digunakan |
| GND | GND | Ground |
| TX | GPIO20 | Masuk ke RX ESP32 |
| RX | GPIO21 | Keluar dari TX ESP32 |

UART GPS menggunakan baud rate **9600**.

### OLED SH1106 ke ESP32-C3

| Pin OLED | ESP32-C3 | Keterangan |
|---|---|---|
| VCC | 3V3 | Catu daya |
| GND | GND | Ground |
| SCK | GPIO1 | I2C SCL |
| SDA | GPIO0 | I2C SDA |

Alamat I2C OLED adalah **0x3C**. Pada modul OLED ini, pin yang diberi label `SCK` digunakan sebagai clock I2C (SCL), bukan SPI.

**Catatan wiring:**
- Semua modul harus menggunakan ground bersama.
- RA-02 hanya menggunakan catu 3,3 V. Jangan hubungkan VCC RA-02 ke 5 V.
- Tegangan GPS mengikuti spesifikasi breakout NEO-6M yang digunakan. Jangan menganggap semua breakout memiliki regulator atau level shifting yang sama.
- Periksa tegangan di pin VCC RA-02 ketika transmisi berlangsung.

## Konfigurasi firmware

| Parameter | Nilai |
|---|---|
| Callsign | `YG3FSL-2` |
| Frekuensi | `432.150 MHz` |
| Bandwidth | `125 kHz` |
| Spreading Factor | `SF12` |
| Coding Rate | `4/5` |
| Sync Word | `0x12` |
| Preamble | `8` |
| CRC | Aktif |
| TX power setting | `18 dBm` |
| Interval beacon | `30 detik` |
| Destination | `APLT00` |
| Path | `WIDE1-1` |
| Simbol APRS | `/[` |
| Prefix LoRa APRS | `3C FF 01` |

Nilai 18 dBm adalah pengaturan pada firmware, bukan hasil pengukuran daya RF di keluaran antena.

## Library

Install library berikut melalui Arduino IDE:

- **LoRa** by Sandeep Mistry
- **TinyGPSPlus** by Mikal Hart
- **U8g2** by oliver

`SPI`, `Wire`, dan `HardwareSerial` sudah disediakan oleh framework Arduino-ESP32.

## Upload firmware

1. Buka file `firmware/YG3FSL-2_APRS_TX_v1.2_POWER18_BEACON30S.ino` di Arduino IDE.
2. Install library yang disebutkan di atas.
3. Pilih board **ESP32C3 Dev Module**.
4. Jika menggunakan USB CDC native, atur **USB CDC On Boot** ke **Enabled**.
5. Upload firmware ke ESP32-C3.
6. Buka Serial Monitor pada **115200 baud**.
7. Uji GPS di area terbuka sampai mendapatkan GPS fix.

Firmware membaca data GPS secara terus-menerus. Beacon hanya dikirim jika posisi GPS valid dan data posisi berusia kurang dari 10 detik. Jika GPS belum mendapatkan fix, pengiriman beacon dilewati.

## Cara kerja singkat

1. ESP32-C3 membaca data NMEA dari GPS melalui UART.
2. Koordinat, kecepatan, arah, dan ketinggian diproses menggunakan TinyGPSPlus.
3. Informasi status GPS dan LoRa ditampilkan pada OLED.
4. Firmware membentuk frame posisi APRS, lalu menambahkan prefix LoRa APRS `3C FF 01`.
5. Paket dikirim melalui RA-02 setiap 30 detik selama radio siap dan data GPS masih valid.

## Status pengujian

Berikut status berdasarkan pengujian yang sudah dicatat:

- ESP32-C3 berhasil menjalankan firmware.
- OLED terdeteksi pada alamat I2C `0x3C` dan tampil normal.
- GPS NEO-6M berhasil membaca data NMEA dan mendapatkan GPS lock.
- Pengiriman dan penerimaan paket LoRa antara dua perangkat berhasil diuji.
- Firmware TX v1.2 dengan setting 18 dBm dan interval beacon 30 detik berhasil dijalankan.

**Yang belum dikonfirmasi:**
- Paket `YG3FSL-2` diterima oleh iGate Bandung.
- iGate meneruskan paket ke APRS-IS.
- Posisi `YG3FSL-2` muncul di APRS.fi.

`LoRa.endPacket()` yang berhasil hanya menunjukkan proses pengiriman pada radio selesai. Hal tersebut belum membuktikan paket diterima iGate atau diteruskan ke APRS-IS.

## Catatan sebelum digunakan

- Prefix `3C FF 01` merupakan framing khusus implementasi LoRa APRS tertentu, bukan header universal. Pastikan iGate tujuan mendukung format ini.
- Frekuensi, daya pancar, antena, dan interval beacon harus mengikuti izin serta ketentuan radio amatir yang berlaku di Indonesia.
- Pastikan antena yang sesuai sudah terpasang sebelum melakukan transmisi.
- Setting SF12 dan bandwidth 125 kHz menghasilkan airtime yang relatif panjang. Interval 30 detik digunakan untuk pengujian; evaluasi kembali interval yang sesuai sebelum penggunaan rutin.
- Gunakan catu 3,3 V yang stabil untuk RA-02 dan hindari penurunan tegangan saat TX.

## Struktur repository

```text
YG3FSL-2_GitHub/
├── README.md
├── firmware/
│   └── YG3FSL-2_APRS_TX_v1.2_POWER18_BEACON30S.ino
├── hardware/
│   ├── wiring-diagram.png
│   └── wiring-diagram.svg
└── docs/
    └── Dokumentasi_Teknis_LoRa_APRS_Tracker_YG3FSL-2_v2.0.md
```

---

Catatan pengembangan berikutnya adalah menguji kompatibilitas paket dengan iGate Bandung, memeriksa forwarding ke APRS-IS, lalu memastikan posisi tampil dengan benar di APRS.fi.
