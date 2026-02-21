# marax-shot-timer

Fork of [alexrus/marax_timer](https://github.com/alexrus/marax_timer) with WiFi connectivity, InfluxDB telemetry, and several bug fixes.

The original project provides an OLED shot timer for the Lelit MaraX espresso machine using an ESP8266 (NodeMCU) and a reed sensor on the vibration pump. This fork adds wireless data logging so you can track temperatures, heating cycles, and shot durations over time.

## What's different from the original

- **WiFi via WiFiManager** -- on first boot, the ESP creates a "MaraX-Timer" access point. Connect to it, enter your WiFi credentials and InfluxDB connection details. Credentials are persisted to flash (LittleFS).
- **InfluxDB v2 telemetry** -- sends `steam_temp`, `target_steam_temp`, `hx_temp`, `heating`, and `boost_countdown` every 5 seconds. Shot events (`marax_shot` measurement with `duration` field) are recorded when the pump stops after 15+ seconds.
- **PlatformIO** -- moved from Arduino IDE to PlatformIO for dependency management and reproducible builds. The Timer library (not available in PlatformIO registry) is replaced with a simple `millis()` approach.
- **WiFi status icon** -- small icon on the idle screen showing connection state.
- **Serial data validation** -- rejects corrupted frames (common on ESP8266 SoftwareSerial due to WiFi interrupt contention) by checking frame length and value ranges.
- **Bug fixes:**
  - Buffer overflow in `getTimer()` (`char outMin[2]` -> `char outMin[4]` for `sprintf("%02u", ...)`)
  - `long` -> `unsigned long` for all `millis()` variables (prevents overflow issues)
  - Duplicate shot detection -- added `currentShotValid` flag so stale `prevTimerCount` values from previous shots can't trigger false events
  - Sleep timeout comparison fix (`>= 0` was always true for unsigned)

## Hardware

Same as the original -- see the [upstream README](https://github.com/alexrus/marax_timer#hardware) for wiring details:

- NodeMCU V2 (ESP8266)
- 0.96" SSD1306 OLED display (I2C)
- Reed sensor on the vibration pump (connected to D7)
- Mara X serial: pin 3 (RX) -> D6, pin 4 (TX) -> D5

## Building and flashing

Requires [PlatformIO](https://platformio.org/).

```bash
# Build
pio run

# Flash (adjust upload_port in platformio.ini if needed)
pio run --target upload
```

## Configuration

On first boot (or after a WiFi reset), the ESP creates a captive portal:

1. Connect to the **MaraX-Timer** WiFi network
2. Enter your WiFi credentials
3. Fill in the InfluxDB fields:
   - **URL** -- e.g. `http://192.168.1.100:8086`
   - **Org** -- your InfluxDB organization
   - **Bucket** -- the bucket to write to
   - **Token** -- an InfluxDB API token with write access

Settings are saved to flash and persist across reboots.

## InfluxDB data

Two measurements are written:

**`marax`** (every 5s while the machine is sending data):

| Field | Type | Description |
|-------|------|-------------|
| `steam_temp` | int | Steam boiler temperature (°C) |
| `target_steam_temp` | int | Target steam temperature (°C) |
| `hx_temp` | int | Heat exchanger temperature (°C) |
| `heating` | int | Heating element state (0/1) |
| `boost_countdown` | int | Boost mode countdown |

**`marax_shot`** (on pump stop, if duration > 15s):

| Field | Type | Description |
|-------|------|-------------|
| `duration` | int | Shot duration in seconds |

Both measurements include a `mode` tag (`C` for coffee priority, `S` for steam priority).

## License

MIT -- same as the [original project](https://github.com/alexrus/marax_timer).
