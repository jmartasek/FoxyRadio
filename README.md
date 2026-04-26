# FoxyRadio

Custom RF controller for **Foxydry** clothes hangers / driers.

Replaces (or supplements) the stock RF remote control using an **ESP8266** or **ESP32** microcontroller paired with a **CC1101** 433 MHz transceiver module.

---

## Overview

Foxydry motorised clothes hangers ship with a simple 433 MHz OOK/ASK remote
control.  This project captures the raw RF pulse sequences that the remote
emits, stores them in flash, and re-transmits them on demand — from a web
browser, a REST call, or any home-automation platform (Home Assistant,
Node-RED, openHAB …).

```
┌─────────────┐   433 MHz OOK    ┌──────────────────────────────┐
│ Stock remote │ ───────────────► │  ESP8266 / ESP32 + CC1101    │
└─────────────┘                  │                              │
                                 │  1. Captures pulse timings   │
         Browser / HA / etc.     │  2. Stores in LittleFS       │
              │                  │  3. Re-transmits on demand   │
              └──── HTTP ───────►│                              │
                                 └──────────────────────────────┘
```

---

## Repository structure

```
FoxyRadio/
├── RFCapture/
│   └── RFCapture.ino          # Step 1 – capture RF codes from the remote
├── RFReplay/
│   └── RFReplay.ino           # Step 2 – replay stored codes via Serial commands
└── FoxyRadioController/
    └── FoxyRadioController.ino # Step 3 – full WiFi web controller (production)
```

---

## Hardware

| Part | Notes |
|------|-------|
| ESP8266 (Wemos D1 mini / NodeMCU) **or** ESP32 DevKit | 3.3 V logic |
| CC1101 433 MHz transceiver module | SPI interface |
| Jumper wires | — |

### Wiring — ESP8266

| CC1101 pin | ESP8266 pin | Wemos D1 label |
|-----------|-------------|----------------|
| VCC | 3.3 V | 3V3 |
| GND | GND | GND |
| MOSI | GPIO 13 | D7 |
| MISO | GPIO 12 | D6 |
| SCK | GPIO 14 | D5 |
| CSN | GPIO 15 | D8 |
| GDO0 | GPIO 5 | D1 |
| GDO2 | GPIO 4 | D2 |

### Wiring — ESP32

| CC1101 pin | ESP32 pin |
|-----------|-----------|
| VCC | 3.3 V |
| GND | GND |
| MOSI | GPIO 23 |
| MISO | GPIO 19 |
| SCK | GPIO 18 |
| CSN | GPIO 5 |
| GDO0 | GPIO 27 |
| GDO2 | GPIO 26 |

> **Note:** CC1101 is a 3.3 V device.  Do **not** connect it to 5 V.

---

## Software dependencies

Install all of the following via the Arduino Library Manager:

| Library | Purpose |
|---------|---------|
| [SmartRC-CC1101-Driver-Lib](https://github.com/LSatan/SmartRC-CC1101-Driver-Lib) | CC1101 SPI driver (by LSatan / ELECHOUSE) |
| [ArduinoJson](https://arduinojson.org/) (v6.x) | JSON for REST responses (`FoxyRadioController` only) |

Board packages needed in Arduino IDE:

* **ESP8266**: `esp8266` community board package  
* **ESP32**: `esp32` by Espressif

---

## Quick-start guide

### Step 1 — Capture the remote codes (`RFCapture`)

1. Wire the CC1101 to your ESP8266/ESP32.
2. Open `RFCapture/RFCapture.ino` in Arduino IDE.
3. Verify the pin definitions at the top of the file match your wiring.
4. Flash to your board, open the Serial Monitor at **115200 baud**.
5. Point the Foxydry remote at the CC1101 antenna and press a button.
6. The sketch prints three representations of the captured burst:

```
=== RF Burst captured ===
Pulse count : 65
Raw timings (µs):
450,850,450,850,450,1700,450,850,...
Binary : 001010110...
Hex    : 0x2AB...
=========================
```

7. Repeat for every command you want to control (UP, DOWN, STOP).
8. Copy the **Raw timings** line for each command.

### Step 2 — Replay via Serial (`RFReplay`)

1. Open `RFReplay/RFReplay.ino`.
2. Paste the captured timing arrays into the `CMD_UP[]`, `CMD_DOWN[]`, and
   `CMD_STOP[]` arrays (replace the placeholder values).
3. Flash to your board, open the Serial Monitor at **115200 baud**.
4. Type a command character and press Enter:

| Key | Action |
|-----|--------|
| `u` | Send UP |
| `d` | Send DOWN |
| `s` | Send STOP |
| `l` | List stored commands |

### Step 3 — WiFi web controller (`FoxyRadioController`)

The production sketch adds WiFi and a web UI with over-the-air command
learning.  No pre-captured arrays needed in code.

1. Open `FoxyRadioController/FoxyRadioController.ino`.
2. Edit the WiFi credentials near the top:
   ```cpp
   #define WIFI_SSID     "YOUR_WIFI_SSID"
   #define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
   ```
3. Flash to your board.
4. Open a browser to the IP shown on the Serial Monitor (or connect to the
   `FoxyRadio` access point if your WiFi is unreachable).
5. The web UI looks like this:

```
┌───────────────────────────────┐
│  FoxyRadio                    │
│  Foxydry clothes-hanger RF    │
│  controller                   │
│                               │
│  [ ▲  UP   ]                  │
│  [ ▼  DOWN ]                  │
│  [ ■  STOP ]                  │
│                               │
│  Learn a new command          │
│  [ 📻 Start capture ]         │
│  [UP v] [ 💾 Save capture ]  │
│                               │
│  Status: Ready                │
└───────────────────────────────┘
```

6. To learn a command:
   a. Select the command slot (UP / DOWN / STOP) from the drop-down.
   b. Click **Start capture** — the device listens for up to 30 seconds.
   c. Press the corresponding button on the Foxydry remote.
   d. Click **Save capture** to store it permanently in flash.
7. Learned commands survive power cycles (stored in LittleFS).

#### REST API

| Endpoint | Description |
|----------|-------------|
| `GET /cmd?name=up` | Send UP command |
| `GET /cmd?name=down` | Send DOWN command |
| `GET /cmd?name=stop` | Send STOP command |
| `GET /capture/start` | Arm receiver and wait for burst (≤30 s) |
| `GET /capture/save?name=up` | Save last capture as UP |
| `GET /status` | JSON status object |

Example Home Assistant `rest_command`:

```yaml
rest_command:
  foxydry_up:
    url: "http://192.168.1.123/cmd?name=up"
  foxydry_down:
    url: "http://192.168.1.123/cmd?name=down"
  foxydry_stop:
    url: "http://192.168.1.123/cmd?name=stop"
```

---

## How it works

The Foxydry remote uses **OOK (On-Off Keying)** at **433.92 MHz** — the same
technique used by most inexpensive RF remotes.

* The CC1101 is configured as an OOK receiver/transmitter.
* In **receive** mode the demodulated digital stream appears on the **GDO2**
  pin.  A GPIO interrupt fires on every rising and falling edge; the time
  between edges is the pulse width.
* A burst is considered complete when no edges are seen for more than 10 ms
  (configurable via `SILENCE_US`).
* In **transmit** mode the ESP drives the **GDO0** pin high/low to reproduce
  the exact pulse widths through the CC1101 power amplifier.
* Each command is sent `REPEAT_COUNT` times (default 3) to improve
  reliability in noisy environments.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| "CC1101 NOT detected" | SPI wiring error | Double-check MOSI/MISO/SCK/CSN; ensure 3.3 V supply |
| No pulses captured | Wrong GDO2 pin | Verify `GDO2_PIN` definition; try pressing the remote closer |
| Captured but replay fails | Power mismatch | Increase `REPEAT_COUNT`; bring device closer to hanger |
| WiFi not connecting | Wrong credentials | Check `WIFI_SSID` / `WIFI_PASSWORD`; connect to `FoxyRadio` AP instead |
| LittleFS format error | First boot | Normal — sketch auto-formats on first run |

---

## License

[MIT](LICENSE)
