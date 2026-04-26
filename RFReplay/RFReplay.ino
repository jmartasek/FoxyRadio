/**
 * FoxyRadio – RF Replay Sketch
 * =============================
 * Replays a previously captured OOK/ASK RF pulse sequence through the CC1101
 * transceiver to control a Foxydry clothes-hanger / drier.
 *
 * Usage
 * -----
 *  1. Run RFCapture.ino and copy the "Raw timings (µs):" line for each
 *     command you want to store (e.g. UP, DOWN, STOP).
 *  2. Paste the timing arrays into the COMMANDS section below.
 *  3. Flash this sketch to your ESP8266 / ESP32.
 *  4. The sketch waits for a single character over Serial to trigger each
 *     command:
 *       'u' – send UP
 *       'd' – send DOWN
 *       's' – send STOP
 *       'l' – list stored commands
 *  5. Each command is transmitted REPEAT_COUNT times to improve reliability.
 *
 * Wiring is identical to RFCapture.ino – see that file for details.
 *
 * Dependencies
 *  Install "SmartRC-CC1101-Driver-Lib" (ELECHOUSE) via the Arduino Library
 *  Manager.
 *
 * Serial baud rate: 115200
 */

#include <ELECHOUSE_CC1101_SRC_DRV.h>

// ---------------------------------------------------------------------------
// Pin configuration – edit to match your board
// ---------------------------------------------------------------------------
#if defined(ESP8266)
  #define GDO0_PIN   5   // GPIO 5 = D1 on Wemos / NodeMCU (TX/data output)
  #define CC1101_CS  15  // GPIO 15 = D8
#elif defined(ESP32)
  #define GDO0_PIN   27
  #define CC1101_CS   5
#else
  #error "Unsupported board – please add pin definitions for your MCU."
#endif

// ---------------------------------------------------------------------------
// Replay settings
// ---------------------------------------------------------------------------
#define REPEAT_COUNT   3     // transmit each command this many times
#define INTER_REPEAT_GAP_US  10000  // µs gap between repeated transmissions

// ---------------------------------------------------------------------------
// Captured command pulse arrays
// ---------------------------------------------------------------------------
// Each array contains alternating HIGH / LOW pulse durations in microseconds,
// starting with the first HIGH pulse.  Paste the values from RFCapture here.
//
// EXAMPLE – replace with your own captured timings:
// ---------------------------------------------------------------------------

// UP command – paste your captured timings between the braces
static const uint32_t CMD_UP[] = {
  // Example placeholder – replace with real captured timings
  // 450,850,450,850,450,1700,450,850, ...
  450, 850, 450, 850, 450, 1700, 450, 850,
  450, 850, 450, 1700, 450, 850, 450, 1700,
  450, 1700, 450, 850, 450, 850, 450, 1700,
  450, 850, 450, 850, 450, 850, 450, 1700,
  450, 1700, 450, 1700, 450, 850, 450, 850,
  450, 1700, 450, 850, 450, 850, 450, 850,
  450, 850, 450, 1700, 450, 850, 450, 850,
  450, 850, 450, 1700, 450, 850, 450, 1700,
  10000
};
static const uint16_t CMD_UP_LEN = sizeof(CMD_UP) / sizeof(CMD_UP[0]);

// DOWN command – paste your captured timings between the braces
static const uint32_t CMD_DOWN[] = {
  // Example placeholder – replace with real captured timings
  450, 850, 450, 850, 450, 1700, 450, 850,
  450, 850, 450, 850, 450, 850, 450, 1700,
  450, 1700, 450, 1700, 450, 1700, 450, 850,
  450, 850, 450, 850, 450, 850, 450, 1700,
  450, 1700, 450, 1700, 450, 850, 450, 850,
  450, 1700, 450, 850, 450, 850, 450, 850,
  450, 850, 450, 1700, 450, 850, 450, 850,
  450, 850, 450, 1700, 450, 850, 450, 1700,
  10000
};
static const uint16_t CMD_DOWN_LEN = sizeof(CMD_DOWN) / sizeof(CMD_DOWN[0]);

// STOP command – paste your captured timings between the braces
static const uint32_t CMD_STOP[] = {
  // Example placeholder – replace with real captured timings
  450, 850, 450, 850, 450, 1700, 450, 850,
  450, 1700, 450, 850, 450, 850, 450, 1700,
  450, 850, 450, 850, 450, 850, 450, 850,
  450, 1700, 450, 1700, 450, 850, 450, 1700,
  450, 1700, 450, 850, 450, 1700, 450, 1700,
  450, 850, 450, 850, 450, 850, 450, 850,
  450, 850, 450, 1700, 450, 850, 450, 850,
  450, 850, 450, 1700, 450, 850, 450, 1700,
  10000
};
static const uint16_t CMD_STOP_LEN = sizeof(CMD_STOP) / sizeof(CMD_STOP[0]);

// ---------------------------------------------------------------------------
// CC1101 initialisation in OOK transmit mode
// ---------------------------------------------------------------------------
void initCC1101TX() {
  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setMHZ(433.92);     // Foxydry remote frequency
  ELECHOUSE_cc1101.setModulation(2);   // 2 = OOK / ASK
  ELECHOUSE_cc1101.setDRate(3.79);     // baud rate
  ELECHOUSE_cc1101.setPA(10);          // TX power (dBm): -30,-20,-15,-10,0,5,7,10
  ELECHOUSE_cc1101.setCCMode(0);       // raw / direct mode
  ELECHOUSE_cc1101.SetTx();
}

// ---------------------------------------------------------------------------
// Transmit a raw pulse sequence via CC1101 GDO0 (direct mode)
// ---------------------------------------------------------------------------
void transmitPulses(const uint32_t* pulses, uint16_t len) {
  // Switch CC1101 to TX
  ELECHOUSE_cc1101.SetTx();
  delayMicroseconds(200);

  // Drive GDO0 manually to reproduce the OOK waveform
  for (uint16_t i = 0; i < len; i++) {
    digitalWrite(GDO0_PIN, (i % 2 == 0) ? HIGH : LOW);  // alternate H/L
    delayMicroseconds(pulses[i]);
  }
  digitalWrite(GDO0_PIN, LOW);

  // Return to idle
  ELECHOUSE_cc1101.goSleep();
}

// ---------------------------------------------------------------------------
// Send a command REPEAT_COUNT times
// ---------------------------------------------------------------------------
void sendCommand(const uint32_t* pulses, uint16_t len, const char* name) {
  Serial.print(F("Sending: "));
  Serial.println(name);
  for (uint8_t i = 0; i < REPEAT_COUNT; i++) {
    transmitPulses(pulses, len);
    delayMicroseconds(INTER_REPEAT_GAP_US);
  }
  Serial.println(F("Done."));
}

// ---------------------------------------------------------------------------
// Arduino setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println(F("\nFoxyRadio – RF Replay"));

  // GDO0 drives the OOK data into the CC1101 in direct TX mode
  pinMode(GDO0_PIN, OUTPUT);
  digitalWrite(GDO0_PIN, LOW);

  if (ELECHOUSE_cc1101.getCC1101()) {
    Serial.println(F("CC1101 detected OK"));
  } else {
    Serial.println(F("CC1101 NOT detected – check wiring!"));
  }
  initCC1101TX();

  Serial.println(F("Commands: u=UP  d=DOWN  s=STOP  l=list"));
}

// ---------------------------------------------------------------------------
// Arduino loop
// ---------------------------------------------------------------------------
void loop() {
  if (Serial.available() > 0) {
    char cmd = (char)Serial.read();
    // consume any trailing newline / CR
    while (Serial.available() > 0) Serial.read();

    switch (cmd) {
      case 'u':
      case 'U':
        sendCommand(CMD_UP, CMD_UP_LEN, "UP");
        break;
      case 'd':
      case 'D':
        sendCommand(CMD_DOWN, CMD_DOWN_LEN, "DOWN");
        break;
      case 's':
      case 'S':
        sendCommand(CMD_STOP, CMD_STOP_LEN, "STOP");
        break;
      case 'l':
      case 'L':
        Serial.println(F("Stored commands:"));
        Serial.print(F("  UP   – "));   Serial.print(CMD_UP_LEN);   Serial.println(F(" pulses"));
        Serial.print(F("  DOWN – "));   Serial.print(CMD_DOWN_LEN); Serial.println(F(" pulses"));
        Serial.print(F("  STOP – "));   Serial.print(CMD_STOP_LEN); Serial.println(F(" pulses"));
        break;
      default:
        Serial.println(F("Unknown command. Use: u=UP  d=DOWN  s=STOP  l=list"));
        break;
    }
  }
}
