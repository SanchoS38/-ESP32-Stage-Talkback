# ESP32 Stage-Talkback & Switched Mic Controller

ESP32 Stage-Talkback is an open-source wireless foot controller / pedal designed for vocalists, sound engineers, and stage performers. It controls microphone routing on a digital mixer using the OSC (Open Sound Control) protocol over UDP.

The primary purpose of the device is to temporarily remove a vocal mic channel from the Main FOH mix and from selected mix buses while the performer talks to the stage, and to restore everything as soon as the button is released.

## 🌟 Key Features

- **Multi-Mixer Support:** Behringer X32 / Midas M32, Behringer X Air / Midas MR, and Behringer WING / WING Rack.
- **Flexible Mute Logic:** While talkback is active, the selected channel is removed from Main (FOH) and from every selected mix bus (the buses you do not select stay untouched, e.g. your monitor lines). Supported bus count: 16 (X32, WING) or 6 (X Air).
- **Dual Operation Modes:**
  - **Push-To-Talk (PTT):** the channel is muted from FOH/buses only while the button is held.
  - **Latch (Toggle):** a single press switches talkback on, the next press switches it off.
- **Wi-Fi Manager & Captive Portal:** If no Wi-Fi network is saved, or the connection fails for 20 seconds, the device starts a fallback access point (`ESP32-Talkback-Setup`, address `5.5.5.5`). It reconnects automatically if the connection is lost.
- **Multilingual Web Interface:** Built-in web server with 4 languages (English, German, Ukrainian, Russian) to configure Wi-Fi, static IP, mDNS hostname, mixer model and IP, channel, bus mute matrix, button mode, LED colors and brightness.
- **Live Status Page:** Wi-Fi signal strength, mixer link status (online/offline), current mode (LIVE / TALKBACK) and battery level, refreshed every 2 seconds over AJAX.
- **Battery Monitoring:** ADC voltage measurement for a single Li-Ion/Li-Po cell (3.3 V – 4.2 V) with a percentage estimate in the web UI.
- **Low Network Traffic:** Commands are sent only when the button state changes (each command is sent twice for reliability, since UDP has no delivery guarantee), plus a small keep-alive request every 3 seconds to detect whether the mixer is reachable.
- **Debounced Button:** 40 ms software debounce.
- **Persistent Settings & Factory Reset:** All settings are stored in non-volatile storage (NVS). Holding the reset button for 3 seconds erases them and reboots into setup mode.

## 🎛 Supported Mixers

| Mixer | OSC port | Channels | Buses | Main (FOH) | Bus send |
|---|---|---|---|---|---|
| Behringer X32 / Midas M32 | 10023 | 1–32 | 16 | `/ch/NN/mix/st` | `/ch/NN/mix/MM/on` |
| Behringer X Air / Midas MR | 10024 | 1–16 | 6 | `/ch/NN/mix/lr` | `/ch/NN/mix/MM/level` |
| Behringer WING / WING Rack | 2223 | 1–40 | 16 | `/ch/N/main/1/on` | `/ch/N/send/B/on` |

Notes:

- On the **X Air**, a send is muted by setting its level to −∞. The current level of every selected send is read from the mixer right before muting and restored when the button is released.
- On the **WING**, OSC must be enabled in the console, and only Main 1 is switched.
- X Air and WING addresses are based on the public OSC documentation; verify them on your own hardware.

## 🛠 Hardware Requirements & Bill of Materials (BOM)

### Core Components

- **MCU Board:** ESP32 Development Board (e.g. ESP32 DevKit V1 / ESP32-WROOM-32). Other variants (ESP32-C3, ESP32-S3) need pin changes in the sketch, since GPIO 34 and some other pins are not available there.
- **Talkback Switch:** Momentary push-button / guitar footswitch connected to GPIO 4 (uses internal `INPUT_PULLUP`).
- **Reset Buttons:** Any of the following, wired to GND (internal `INPUT_PULLUP`):
  - GPIO 0 (the BOOT button on the dev board)
  - GPIO 16 (external micro tactile button; avoid this pin on WROVER modules with PSRAM)
- **Visual Status Indicator:** 3× SK6812 RGBW (or compatible addressable) LEDs connected to GPIO 15.

### Power & Battery Management

- **Battery:** 3.7 V Li-Ion / Li-Po cell (e.g. 18650 or flat pouch cell).
- **Charging Module:** TP4056 Li-Ion charger module (USB-C).
- **Boost Converter:** MT3608 or DD0812SA step-up converter (3.7 V → 5 V for the ESP32 and LEDs).
- **Power Switch:** SPST toggle switch placed on the 5 V line after the boost converter.
- **Voltage Divider (Battery Monitor):** 2× 10 kΩ resistors, wired as `BAT+ → 10k → GPIO 34 → 10k → GND`. A 100 nF capacitor between GPIO 34 and GND is recommended for stable readings.

## 📐 Pinout

| Component | ESP32 Pin | Logic / Connection |
|---|---|---|
| Talkback Switch | GPIO 4 | Active LOW (`INPUT_PULLUP` to GND) |
| Reset Switch (BOOT) | GPIO 0 | Active LOW (`INPUT_PULLUP` to GND) |
| Reset Switch (external) | GPIO 16 | Active LOW (`INPUT_PULLUP` to GND) |
| LED Data (DIN) | GPIO 15 | Digital output, RGBW addressable LEDs |
| Battery Voltage Sensor | GPIO 34 | Analog input (ADC1 via 10k + 10k divider) |

> Holding GPIO 0 low while powering up or pressing EN puts the ESP32 into firmware download mode. Use it as a reset button only after the device has started.

## 🚦 LED Status Indicators

| LED | Meaning |
|---|---|
| Breathing green | Setup mode: access point `ESP32-Talkback-Setup` (`5.5.5.5`) |
| Breathing yellow | Connecting to the saved Wi-Fi network |
| 3 short blinks | Wi-Fi connected |
| Blue (solid, default) | Standby: mic is live to FOH |
| Green (solid, default) | Talkback active: mic removed from FOH and selected buses |
| Blinking magenta | Wi-Fi connected, but the mixer IP is not set or the mixer is unreachable |
| 10 white blinks | Settings were reset to factory defaults |

The standby and active colors, and the brightness, can be changed in the web interface.

## 🚀 Getting Started

1. **Install the tools.** In Arduino IDE install the **esp32** board package (Espressif Systems) from the Boards Manager and the **Adafruit NeoPixel** library from the Library Manager.
2. **Flash the firmware.** Open `ESP32_Talkback/ESP32_Talkback.ino` (the folder name must match the file name), select your ESP32 board (e.g. *ESP32 Dev Module*) and upload.
3. **Initial setup.** Power up the device and connect your phone or PC to the `ESP32-Talkback-Setup` Wi-Fi network.
4. **Configure.** Open `http://5.5.5.5` in your browser. Choose your Wi-Fi network and password, select the mixer model, enter the mixer IP address and your vocal channel, select the buses to mute during talkback, and click **Save & Reboot**.
5. **Later access.** Once the device is on your network, open `http://talkback.local` (or the hostname you set), or use the IP address from your router's device list. You can also enable a static IP in the settings.

## ⚠️ Known Limitations

- OSC runs over UDP, which has no delivery guarantee. Every command is sent twice, but if the device loses power or Wi-Fi at the exact moment the button is released, the channel can stay muted until you press and release the button again.
- The battery percentage is a linear estimate between 3.3 V and 4.2 V. The ESP32 ADC is not perfectly linear, so calibrate `BATTERY_DIVIDER` in the sketch against a multimeter for accurate readings.
- The setup access point is open (no password) and the web interface has no authentication; use it on a trusted network.
