/*
  ============================================================
  YG3FSL-2 LoRa APRS Tracker - TX Firmware v1.2
  Board  : ESP32-C3 SuperMini
  Radio  : RA-02 / SX1278
  GPS    : u-blox NEO-6M V2
  OLED   : 1.30 inch, SH1106 128x64, I2C address 0x3C

  IMPORTANT:
  - OLED is initialized AFTER GPS and LoRa initialization.
  - OLED uses the previously tested SH1106 U8g2 driver.
  - LoRa SPI pin mapping is the known-working mapping.
  - TX is attempted only when GPS position is valid and fresh.
  - The 3-byte LoRa APRS prefix (3C FF 01) is implementation-
    specific. Confirm that the target Bandung iGate supports
    this exact framing before expecting APRS-IS forwarding.
  - This is an experimental tracker firmware, not a guarantee
    of iGate compatibility or APRS.fi visibility.
  ============================================================
*/

#include <Wire.h>
#include <SPI.h>
#include <LoRa.h>
#include <U8g2lib.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <math.h>
#include <string.h>

// ============================================================
// STATION IDENTITY
// ============================================================

#define CALLSIGN  "YG3FSL-2"
#define APRS_DEST "APLT00"
#define APRS_PATH "WIDE1-1"

// APRS symbol table '/' and symbol code '[' (walker/person)

// ============================================================
// OLED CONFIGURATION
// ============================================================

#define OLED_SDA  0
#define OLED_SCL  1
#define OLED_ADDR 0x3C

U8G2_SH1106_128X64_NONAME_F_HW_I2C display(
  U8G2_R0,
  U8X8_PIN_NONE
);

// ============================================================
// GPS CONFIGURATION
// ============================================================

#define GPS_RX   20
#define GPS_TX   21
#define GPS_BAUD 9600

HardwareSerial GPS(1);
TinyGPSPlus gps;

// ============================================================
// LORA SPI PIN CONFIGURATION
// Previously tested mapping: SCK4, MISO5, MOSI6, NSS7
// ============================================================

#define LORA_SCK   4
#define LORA_MISO  5
#define LORA_MOSI  6
#define LORA_NSS   7
#define LORA_RESET 10
#define LORA_DIO0  3

// ============================================================
// LORA RADIO PARAMETERS
// ============================================================

#define LORA_FREQ       432150000L
#define LORA_BW         125E3
#define LORA_SF         12
#define LORA_CR         5
#define LORA_SYNC_WORD  0x12
// RA-02 datasheet specification: 18 +/- 1 dBm.
// Use 18 dBm as the module-rated upper nominal setting.
#define LORA_TX_POWER   18

// ============================================================
// BEACON SETTINGS
// ============================================================

// Requested 30-second interval. At SF12/BW125, airtime is long;
// use this for controlled testing and avoid congesting a shared channel.
#define BEACON_INTERVAL 30000UL
#define GPS_MAX_AGE     10000UL

// Implementation-specific LoRa APRS framing prefix.
// Verify compatibility with the target iGate firmware.
const uint8_t LORA_APRS_HEADER[] = {
  0x3C, 0xFF, 0x01
};

// ============================================================
// GLOBAL STATE
// ============================================================

bool loraReady = false;
bool oledReady = false;

unsigned long lastBeacon  = 0;
unsigned long lastDisplay = 0;
unsigned long lastDebug   = 0;

uint32_t txCount = 0;

char txStatus[20] = "STARTING";

// ============================================================
// GPS VALIDITY
// ============================================================

bool isGPSValid() {
  return gps.location.isValid() &&
         gps.location.age() < GPS_MAX_AGE;
}

// ============================================================
// OLED INITIALIZATION
// Called AFTER LoRa initialization as requested.
// ============================================================

void initializeOLED() {
  Serial.println();
  Serial.println("[OLED] Initializing I2C display...");

  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(100000);

  // Scan I2C bus for diagnostic information.
  uint8_t foundDevices = 0;

  Serial.println("[OLED] I2C scan:");

  for (uint8_t address = 1; address < 127; address++) {
    Wire.beginTransmission(address);

    if (Wire.endTransmission() == 0) {
      Serial.print("  Device found at 0x");
      if (address < 16) Serial.print('0');
      Serial.println(address, HEX);
      foundDevices++;
    }
  }

  Serial.print("[OLED] Devices found: ");
  Serial.println(foundDevices);

  display.setI2CAddress(OLED_ADDR * 2);
  display.begin();

  // Force display out of power-save mode.
  display.setPowerSave(0);
  display.setContrast(255);

  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(5, 15, "YG3FSL-2 APRS");
  display.drawHLine(5, 20, 118);
  display.drawStr(5, 36, "OLED INITIALIZED");
  display.drawStr(5, 52, "SYSTEM STARTING...");
  display.sendBuffer();

  oledReady = true;

  Serial.println("[OLED] READY");
}

// ============================================================
// APRS COORDINATE FORMATTERS
// Latitude: DDMM.mmN/S
// Longitude: DDDMM.mmE/W
// ============================================================

bool formatLatitude(double latitude,
                    char *output,
                    size_t outputSize) {
  if (latitude < -90.0 || latitude > 90.0) {
    return false;
  }

  char hemisphere = (latitude >= 0.0) ? 'N' : 'S';
  double absoluteValue = fabs(latitude);

  int degrees = (int)absoluteValue;
  double minutes = (absoluteValue - degrees) * 60.0;

  if (minutes >= 59.995) {
    degrees++;
    minutes = 0.0;
  }

  int written = snprintf(
    output,
    outputSize,
    "%02d%05.2f%c",
    degrees,
    minutes,
    hemisphere
  );

  return written > 0 && (size_t)written < outputSize;
}

bool formatLongitude(double longitude,
                     char *output,
                     size_t outputSize) {
  if (longitude < -180.0 || longitude > 180.0) {
    return false;
  }

  char hemisphere = (longitude >= 0.0) ? 'E' : 'W';
  double absoluteValue = fabs(longitude);

  int degrees = (int)absoluteValue;
  double minutes = (absoluteValue - degrees) * 60.0;

  if (minutes >= 59.995) {
    degrees++;
    minutes = 0.0;
  }

  int written = snprintf(
    output,
    outputSize,
    "%03d%05.2f%c",
    degrees,
    minutes,
    hemisphere
  );

  return written > 0 && (size_t)written < outputSize;
}

// ============================================================
// BUILD TNC2-STYLE APRS POSITION FRAME
// ============================================================

bool buildAPRSFrame(char *frame, size_t frameSize) {
  if (!isGPSValid()) {
    return false;
  }

  char latitude[12];
  char longitude[12];

  if (!formatLatitude(
        gps.location.lat(),
        latitude,
        sizeof(latitude))) {
    return false;
  }

  if (!formatLongitude(
        gps.location.lng(),
        longitude,
        sizeof(longitude))) {
    return false;
  }

  int course = 0;
  if (gps.course.isValid()) {
    course = (int)lround(gps.course.deg());
  }
  if (course >= 360) course = 0;
  if (course < 0) course = 0;

  int speedKnots = 0;
  if (gps.speed.isValid()) {
    speedKnots = (int)lround(gps.speed.knots());
  }
  if (speedKnots < 0) speedKnots = 0;
  if (speedKnots > 999) speedKnots = 999;

  bool altitudeValid = gps.altitude.isValid();
  long altitudeFeet = 0;

  if (altitudeValid) {
    altitudeFeet = lround(gps.altitude.meters() * 3.28084);

    if (altitudeFeet < 0 || altitudeFeet > 999999) {
      altitudeValid = false;
    }
  }

  int written;

  if (altitudeValid) {
    written = snprintf(
      frame,
      frameSize,
      "%s>%s,%s:!%s/%s[%03d/%03d/A=%06ld YG3FSL-2 LoRa APRS",
      CALLSIGN,
      APRS_DEST,
      APRS_PATH,
      latitude,
      longitude,
      course,
      speedKnots,
      altitudeFeet
    );
  } else {
    written = snprintf(
      frame,
      frameSize,
      "%s>%s,%s:!%s/%s[%03d/%03d YG3FSL-2 LoRa APRS",
      CALLSIGN,
      APRS_DEST,
      APRS_PATH,
      latitude,
      longitude,
      course,
      speedKnots
    );
  }

  return written > 0 && (size_t)written < frameSize;
}

// ============================================================
// OLED UPDATE
// ============================================================

void updateDisplay() {
  if (!oledReady) return;

  display.clearBuffer();
  display.setFont(u8g2_font_4x6_tf);

  display.drawStr(0, 7, "YG3FSL-2 APRS TRACKER");
  display.drawHLine(0, 10, 128);

  char line[32];

  if (isGPSValid()) {
    snprintf(
      line,
      sizeof(line),
      "GPS: FIX SAT:%02lu",
      (unsigned long)(
        gps.satellites.isValid() ?
        gps.satellites.value() : 0
      )
    );
  } else {
    snprintf(
      line,
      sizeof(line),
      "GPS: NO FIX SAT:%02lu",
      (unsigned long)(
        gps.satellites.isValid() ?
        gps.satellites.value() : 0
      )
    );
  }
  display.drawStr(0, 18, line);

  if (isGPSValid()) {
    char lat[12];

    if (formatLatitude(
          gps.location.lat(),
          lat,
          sizeof(lat))) {
      snprintf(line, sizeof(line), "LAT:%s", lat);
    } else {
      snprintf(line, sizeof(line), "LAT: INVALID");
    }
  } else {
    snprintf(line, sizeof(line), "LAT: WAITING...");
  }
  display.drawStr(0, 26, line);

  if (isGPSValid()) {
    char lon[12];

    if (formatLongitude(
          gps.location.lng(),
          lon,
          sizeof(lon))) {
      snprintf(line, sizeof(line), "LON:%s", lon);
    } else {
      snprintf(line, sizeof(line), "LON: INVALID");
    }
  } else {
    snprintf(line, sizeof(line), "LON: WAITING...");
  }
  display.drawStr(0, 34, line);

  if (isGPSValid() && gps.speed.isValid()) {
    snprintf(
      line,
      sizeof(line),
      "SPD: %.1f km/h",
      gps.speed.kmph()
    );
  } else {
    snprintf(line, sizeof(line), "SPD: -- km/h");
  }
  display.drawStr(0, 42, line);

  if (isGPSValid() && gps.altitude.isValid()) {
    snprintf(
      line,
      sizeof(line),
      "ALT: %.1f m",
      gps.altitude.meters()
    );
  } else {
    snprintf(line, sizeof(line), "ALT: -- m");
  }
  display.drawStr(0, 50, line);

  snprintf(
    line,
    sizeof(line),
    "LORA:%s %s",
    loraReady ? "OK" : "ERR",
    txStatus
  );
  display.drawStr(0, 61, line);

  display.sendBuffer();
}

// ============================================================
// GPS UART READER
// ============================================================

void readGPS() {
  while (GPS.available()) {
    gps.encode(GPS.read());
  }
}

// ============================================================
// TRANSMIT APRS PACKET
// ============================================================

void transmitAPRS() {
  char aprsFrame[180];

  if (!buildAPRSFrame(aprsFrame, sizeof(aprsFrame))) {
    strncpy(txStatus, "WAIT GPS", sizeof(txStatus) - 1);
    txStatus[sizeof(txStatus) - 1] = '\0';

    Serial.println("[TX] GPS fix invalid/stale; beacon skipped.");
    updateDisplay();
    return;
  }

  Serial.println();
  Serial.println("================================");
  Serial.println("       YG3FSL-2 APRS TX");
  Serial.println("================================");

  Serial.print("Frame  : ");
  Serial.println(aprsFrame);

  Serial.println("Prefix : 3C FF 01");

  strncpy(txStatus, "SENDING", sizeof(txStatus) - 1);
  txStatus[sizeof(txStatus) - 1] = '\0';
  updateDisplay();

  if (LoRa.beginPacket() != 1) {
    strncpy(txStatus, "TX ERROR", sizeof(txStatus) - 1);
    txStatus[sizeof(txStatus) - 1] = '\0';

    Serial.println("[TX] beginPacket() failed.");
    updateDisplay();
    return;
  }

  LoRa.write(
    LORA_APRS_HEADER,
    sizeof(LORA_APRS_HEADER)
  );

  LoRa.print(aprsFrame);

  int result = LoRa.endPacket();
  LoRa.idle();

  if (result == 1) {
    txCount++;

    strncpy(txStatus, "TX OK", sizeof(txStatus) - 1);
    txStatus[sizeof(txStatus) - 1] = '\0';

    Serial.println("[TX] LoRa packet sent.");
    Serial.print("[TX] Count: ");
    Serial.println(txCount);
  } else {
    strncpy(txStatus, "TX FAILED", sizeof(txStatus) - 1);
    txStatus[sizeof(txStatus) - 1] = '\0';

    Serial.println("[TX] Transmission failed.");
  }

  Serial.println("================================");

  updateDisplay();
}

// ============================================================
// LORA INITIALIZATION
// ============================================================

void initializeLoRa() {
  Serial.println();
  Serial.println("[LORA] Initializing SPI...");

  SPI.begin(
    LORA_SCK,
    LORA_MISO,
    LORA_MOSI,
    LORA_NSS
  );

  LoRa.setSPI(SPI);

  LoRa.setPins(
    LORA_NSS,
    LORA_RESET,
    LORA_DIO0
  );

  LoRa.setSPIFrequency(1000000);

  Serial.println("[LORA] Initializing SX1278...");

  if (!LoRa.begin(LORA_FREQ)) {
    loraReady = false;

    Serial.println("[LORA] Initialization FAILED.");
    return;
  }

  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.setPreambleLength(8);
  LoRa.enableCrc();
  LoRa.disableInvertIQ();

  // Low initial test power. Confirm permitted power
  // and antenna/load before any RF transmission.
  LoRa.setTxPower(
    LORA_TX_POWER,
    PA_OUTPUT_PA_BOOST_PIN
  );

  LoRa.idle();

  loraReady = true;

  Serial.println("[LORA] Initialization SUCCESS.");
  Serial.println("[LORA] Frequency : 432.150 MHz");
  Serial.println("[LORA] Bandwidth : 125 kHz");
  Serial.println("[LORA] SF        : 12");
  Serial.println("[LORA] CR        : 4/5");
  Serial.println("[LORA] Sync word : 0x12");
  Serial.println("[LORA] Power     : 18 dBm setting");
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("================================");
  Serial.println(" YG3FSL-2 APRS TRACKER TX v1.2");
  Serial.println("================================");

  // 1. GPS first
  Serial.println("[GPS] Initializing UART...");

  GPS.begin(
    GPS_BAUD,
    SERIAL_8N1,
    GPS_RX,
    GPS_TX
  );

  Serial.println("[GPS] UART ready.");

  // 2. LoRa second
  initializeLoRa();

  // 3. OLED AFTER GPS AND LORA
  // This retains the known-good SH1106/I2C setup.
  initializeOLED();

  if (!loraReady) {
    strncpy(txStatus, "LORA ERROR", sizeof(txStatus) - 1);
    txStatus[sizeof(txStatus) - 1] = '\0';
  } else {
    strncpy(txStatus, "READY", sizeof(txStatus) - 1);
    txStatus[sizeof(txStatus) - 1] = '\0';
  }

  Serial.println();
  Serial.println("================================");
  Serial.println("SYSTEM STATUS");
  Serial.println("================================");
  Serial.println("GPS  : UART READY");
  Serial.print("LORA : ");
  Serial.println(loraReady ? "READY" : "ERROR");
  Serial.print("OLED : ");
  Serial.println(oledReady ? "READY" : "ERROR");
  Serial.println("APRS : TNC2-style position frame");
  Serial.println("BEACON INTERVAL: 30 seconds");
  Serial.println("================================");

  updateDisplay();

  // First beacon interval starts after boot.
  lastBeacon = millis();
  lastDisplay = millis();
  lastDebug = millis();
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop() {
  // Continuously parse GPS UART data.
  readGPS();

  // Refresh OLED twice per second.
  if (millis() - lastDisplay >= 500UL) {
    lastDisplay = millis();
    updateDisplay();
  }

  // GPS diagnostic output every 5 seconds.
  if (millis() - lastDebug >= 5000UL) {
    lastDebug = millis();

    Serial.print("[GPS] Fix: ");
    Serial.print(isGPSValid() ? "VALID" : "NO FIX");

    Serial.print(" | Satellites: ");
    Serial.print(
      gps.satellites.isValid() ?
      gps.satellites.value() : 0
    );

    Serial.print(" | NMEA chars: ");
    Serial.println(gps.charsProcessed());
  }

  // Send beacon every configured interval, only if radio ready.
  if (loraReady &&
      millis() - lastBeacon >= BEACON_INTERVAL) {
    lastBeacon = millis();
    transmitAPRS();
  }
}
