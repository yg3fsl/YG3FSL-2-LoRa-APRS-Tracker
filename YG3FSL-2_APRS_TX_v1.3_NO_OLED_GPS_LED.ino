/*
  ============================================================
  YG3FSL-2 LoRa APRS Tracker - TX Firmware v1.3 NO OLED
  Board  : ESP32-C3 SuperMini
  Radio  : RA-02 / SX1278
  GPS    : u-blox NEO-6M V2
  Status : Onboard LED indicates GPS lock

  IMPORTANT:
  - OLED/LCD and all I2C display code have been removed.
  - Onboard LED GPIO8 is active-low on typical ESP32-C3 SuperMini.
  - LED blinks while GPS has no valid/fresh position fix.
  - LED stays solid ON while GPS position fix is valid and fresh.
  - LoRa SPI pin mapping and APRS settings are preserved.
  - TX is attempted only when GPS position is valid and fresh.
  - The 3-byte LoRa APRS prefix (3C FF 01) is implementation-
    specific. Confirm that the target Bandung iGate supports
    this exact framing before expecting APRS-IS forwarding.
  - This is an experimental tracker firmware, not a guarantee
    of iGate compatibility or APRS.fi visibility.
  ============================================================
*/

#include <SPI.h>
#include <LoRa.h>
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
// GPS STATUS LED - ESP32-C3 SUPERMINI
// Typical SuperMini onboard blue LED: GPIO8, active LOW.
// LOW = LED ON, HIGH = LED OFF.
// ============================================================

#define GPS_LED_PIN             8
#define GPS_LED_ON              LOW
#define GPS_LED_OFF             HIGH
#define GPS_LED_BLINK_INTERVAL  500UL

bool gpsLedBlinkState = true;
bool gpsWasLocked = false;
unsigned long lastLedToggle = 0;

void initializeGPSLED() {
  // Set the output latch HIGH before switching to OUTPUT.
  // This keeps the active-low LED OFF during initialization.
  digitalWrite(GPS_LED_PIN, GPS_LED_OFF);
  pinMode(GPS_LED_PIN, OUTPUT);

  // Start with LED ON; it will blink until GPS lock is valid.
  gpsLedBlinkState = true;
  digitalWrite(GPS_LED_PIN, GPS_LED_ON);
  lastLedToggle = millis();

  Serial.println("[LED] GPS status LED initialized on GPIO8 (active LOW).");
}

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

unsigned long lastBeacon = 0;
unsigned long lastDebug  = 0;

uint32_t txCount = 0;

// ============================================================
// GPS VALIDITY
// ============================================================

bool isGPSValid() {
  return gps.location.isValid() &&
         gps.location.age() < GPS_MAX_AGE;
}

// ============================================================
// GPS STATUS LED UPDATE
// Blink when GPS has no valid/fresh fix.
// Stay continuously ON when GPS fix is valid and fresh.
// Non-blocking: uses millis(), so GPS parsing and LoRa timing
// are not interrupted by delay() calls.
// ============================================================

void updateGPSLED() {
  const unsigned long now = millis();
  const bool gpsLocked = isGPSValid();

  if (gpsLocked) {
    digitalWrite(GPS_LED_PIN, GPS_LED_ON);
    gpsLedBlinkState = true;
    lastLedToggle = now;
    gpsWasLocked = true;
    return;
  }

  // Start the blink cycle immediately when a previously valid
  // GPS fix becomes invalid or too old.
  if (gpsWasLocked) {
    gpsWasLocked = false;
    gpsLedBlinkState = true;
    digitalWrite(GPS_LED_PIN, GPS_LED_ON);
    lastLedToggle = now;
    return;
  }

  if (now - lastLedToggle >= GPS_LED_BLINK_INTERVAL) {
    lastLedToggle = now;
    gpsLedBlinkState = !gpsLedBlinkState;

    digitalWrite(
      GPS_LED_PIN,
      gpsLedBlinkState ? GPS_LED_ON : GPS_LED_OFF
    );
  }
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
    Serial.println("[TX] GPS fix invalid/stale; beacon skipped.");
    return;
  }

  Serial.println();
  Serial.println("================================");
  Serial.println("       YG3FSL-2 APRS TX");
  Serial.println("================================");

  Serial.print("Frame  : ");
  Serial.println(aprsFrame);

  Serial.println("Prefix : 3C FF 01");

  if (LoRa.beginPacket() != 1) {
    Serial.println("[TX] beginPacket() failed.");
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

    Serial.println("[TX] LoRa packet sent.");
    Serial.print("[TX] Count: ");
    Serial.println(txCount);
  } else {
    Serial.println("[TX] Transmission failed.");
  }

  Serial.println("================================");
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

  // Initialize the onboard LED immediately after Serial starts.
  initializeGPSLED();

  delay(2000);

  Serial.println();
  Serial.println("================================");
  Serial.println(" YG3FSL-2 APRS TRACKER TX v1.3");
  Serial.println("          NO OLED");
  Serial.println("================================");

  // 1. GPS
  Serial.println("[GPS] Initializing UART...");

  GPS.begin(
    GPS_BAUD,
    SERIAL_8N1,
    GPS_RX,
    GPS_TX
  );

  Serial.println("[GPS] UART ready.");

  // 2. LoRa
  initializeLoRa();

  Serial.println();
  Serial.println("================================");
  Serial.println("SYSTEM STATUS");
  Serial.println("================================");
  Serial.println("GPS  : UART READY; waiting for valid fix");
  Serial.print("LORA : ");
  Serial.println(loraReady ? "READY" : "ERROR");
  Serial.println("LED  : GPIO8, active LOW");
  Serial.println("LED  : BLINK = NO GPS FIX; SOLID ON = GPS LOCK");
  Serial.println("APRS : TNC2-style position frame");
  Serial.println("BEACON INTERVAL: 30 seconds");
  Serial.println("================================");

  // First beacon interval starts after boot.
  lastBeacon = millis();
  lastDebug = millis();
  lastLedToggle = millis();
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop() {
  // Continuously parse GPS UART data.
  readGPS();

  // Update onboard LED without blocking GPS parsing.
  updateGPSLED();

  // GPS diagnostic output every 5 seconds.
  if (millis() - lastDebug >= 5000UL) {
    lastDebug = millis();

    Serial.print("[GPS] Fix: ");
    Serial.print(isGPSValid() ? "VALID / LOCKED" : "NO FIX");

    Serial.print(" | Satellites: ");
    Serial.print(
      gps.satellites.isValid() ?
      gps.satellites.value() : 0
    );

    Serial.print(" | Age (ms): ");
    Serial.print(
      gps.location.isValid() ?
      gps.location.age() : 0
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
