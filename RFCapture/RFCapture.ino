/**
 * FoxyRadio – RF Capture Sketch
 * ==============================
 * Captures raw OOK/ASK RF signals from the stock Foxydry clothes-hanger
 * remote control using a CC1101 transceiver wired to an ESP8266 or ESP32.
 *
 * What it does
 * ------------
 *  1. Initialises the CC1101 in OOK receive mode at 433.92 MHz.
 *  2. Monitors the demodulated serial stream on the GDO2 pin via a GPIO
 *     interrupt and records every rising / falling edge timestamp.
 *  3. When a burst of activity followed by silence is detected the
 *     complete pulse sequence is printed to the Serial monitor as:
 *       a) raw microsecond timings (HIGH us, LOW us, ...)
 *       b) a human-readable binary string (short = 0, long = 1)
 *       c) a hex digest that can be pasted into RFReplay.ino
 *  4. Repeats continuously so multiple button presses can be compared.
 *
 * Wiring – ESP8266 (Wemos D1 mini / NodeMCU)
 * -------------------------------------------
 *  CC1101  →  ESP8266
 *  VCC        3.3 V
 *  GND        GND
 *  MOSI       D7  (GPIO 13)
 *  MISO       D6  (GPIO 12)
 *  SCK        D5  (GPIO 14)
 *  CSN        D8  (GPIO 15)
 *  GDO0       D1  (GPIO  5)   ← optional carrier-sense indicator
 *  GDO2       D2  (GPIO  4)   ← demodulated data, connected to interrupt
 *
 * Wiring – ESP32
 * --------------
 *  CC1101  →  ESP32
 *  VCC        3.3 V
 *  GND        GND
 *  MOSI       GPIO 23
 *  MISO       GPIO 19
 *  SCK        GPIO 18
 *  CSN        GPIO  5
 *  GDO0       GPIO 27   ← optional
 *  GDO2       GPIO 26   ← demodulated data, connected to interrupt
 *
 * Dependencies
 *  Install "SmartRC-CC1101-Driver-Lib" (ELECHOUSE) via the Arduino Library
 *  Manager or from https://github.com/LSatan/SmartRC-CC1101-Driver-Lib
 *
 * Serial baud rate: 115200
 */

#include <ELECHOUSE_CC1101_SRC_DRV.h>

// ---------------------------------------------------------------------------
// Pin configuration – edit to match your board
// ---------------------------------------------------------------------------
#if defined(ESP8266)
  #define GDO2_PIN   4   // GPIO 4  = D2 on Wemos / NodeMCU
  #define GDO0_PIN   5   // GPIO 5  = D1 (optional, carrier sense)
  #define CC1101_CS  15  // GPIO 15 = D8
#elif defined(ESP32)
  #define GDO2_PIN   26
  #define GDO0_PIN   27
  #define CC1101_CS   5
#else
  #error "Unsupported board – please add pin definitions for your MCU."
#endif

// ---------------------------------------------------------------------------
// Capture settings
// ---------------------------------------------------------------------------
#define MAX_PULSES        512   // maximum edges to record per burst
#define MIN_PULSE_US       80   // ignore glitches shorter than this (µs)
#define SILENCE_US      10000   // gap that marks end of transmission (µs)
#define SHORT_THRESHOLD  1000   // pulse lengths shorter than this → bit '0'

// ---------------------------------------------------------------------------
// Globals (shared between ISR and main loop)
// ---------------------------------------------------------------------------
volatile uint32_t pulseBuffer[MAX_PULSES];
volatile uint16_t pulseCount   = 0;
volatile bool     captureReady = false;
volatile uint32_t lastEdgeTime = 0;

// ---------------------------------------------------------------------------
// Interrupt service routine – records time between edges
// ---------------------------------------------------------------------------
IRAM_ATTR void edgeISR() {
  uint32_t now = micros();
  uint32_t delta = now - lastEdgeTime;
  lastEdgeTime = now;

  if (captureReady) return;          // buffer not yet consumed, skip

  if (delta < MIN_PULSE_US) return;  // noise filter

  if (pulseCount < MAX_PULSES) {
    pulseBuffer[pulseCount++] = delta;
  }
}

// ---------------------------------------------------------------------------
// CC1101 initialisation in OOK receive mode
// ---------------------------------------------------------------------------
void initCC1101() {
  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setMHZ(433.92);       // Foxydry remotes use 433.92 MHz
  ELECHOUSE_cc1101.setModulation(2);     // 2 = OOK / ASK
  ELECHOUSE_cc1101.setDRate(3.79);       // ~3.79 kBaud (typical for 433 MHz OOK)
  ELECHOUSE_cc1101.setRxBW(58);         // 58 kHz receive bandwidth
  ELECHOUSE_cc1101.setPA(10);           // receive PA – value ignored in RX
  ELECHOUSE_cc1101.setCCMode(0);        // disable packet handling (raw mode)
  ELECHOUSE_cc1101.setChannel(0);
  ELECHOUSE_cc1101.SetRx();             // start receiving
}

// ---------------------------------------------------------------------------
// Detect a completed burst: silence detected after pulseCount > 0
// ---------------------------------------------------------------------------
void checkSilence() {
  if (pulseCount == 0 || captureReady) return;

  uint32_t elapsed = micros() - lastEdgeTime;
  if (elapsed > SILENCE_US) {
    captureReady = true;
  }
}

// ---------------------------------------------------------------------------
// Print the captured pulse sequence to Serial
// ---------------------------------------------------------------------------
void printCapture() {
  // Take a local snapshot (ISR is still active but captureReady blocks it)
  uint16_t count = pulseCount;

  Serial.println();
  Serial.println(F("=== RF Burst captured ==="));
  Serial.print(F("Pulse count : "));
  Serial.println(count);

  // --- 1. Raw timings ---
  Serial.println(F("Raw timings (µs):"));
  for (uint16_t i = 0; i < count; i++) {
    Serial.print(pulseBuffer[i]);
    if (i < count - 1) Serial.print(',');
    if ((i & 0x0F) == 0x0F) Serial.println();
  }
  Serial.println();

  // --- 2. Binary string (short pulse = 0, long pulse = 1) ---
  Serial.print(F("Binary : "));
  for (uint16_t i = 0; i < count; i++) {
    Serial.print(pulseBuffer[i] < SHORT_THRESHOLD ? '0' : '1');
  }
  Serial.println();

  // --- 3. Hex digest (pack bits into bytes) ---
  Serial.print(F("Hex    : 0x"));
  uint8_t  byte_val = 0;
  uint8_t  bit_pos  = 7;
  for (uint16_t i = 0; i < count; i++) {
    if (pulseBuffer[i] >= SHORT_THRESHOLD) {
      byte_val |= (1 << bit_pos);
    }
    if (bit_pos == 0) {
      if (byte_val < 0x10) Serial.print('0');
      Serial.print(byte_val, HEX);
      byte_val = 0;
      bit_pos  = 7;
    } else {
      bit_pos--;
    }
  }
  // flush remaining bits
  if (bit_pos != 7) {
    if (byte_val < 0x10) Serial.print('0');
    Serial.print(byte_val, HEX);
  }
  Serial.println();
  Serial.println(F("========================="));
}

// ---------------------------------------------------------------------------
// Arduino setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println(F("\nFoxyRadio – RF Capture"));
  Serial.println(F("Press a button on the Foxydry remote…\n"));

  // Initialise CC1101
  if (ELECHOUSE_cc1101.getCC1101()) {
    Serial.println(F("CC1101 detected OK"));
  } else {
    Serial.println(F("CC1101 NOT detected – check wiring!"));
  }
  initCC1101();

  // Attach interrupt to GDO2
  pinMode(GDO2_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(GDO2_PIN), edgeISR, CHANGE);

  Serial.println(F("Listening for RF signal…"));
}

// ---------------------------------------------------------------------------
// Arduino loop
// ---------------------------------------------------------------------------
void loop() {
  checkSilence();

  if (captureReady) {
    printCapture();

    // Reset for next capture
    noInterrupts();
    pulseCount   = 0;
    captureReady = false;
    interrupts();

    // Brief pause so serial output settles before next burst
    delay(500);
  }
}
