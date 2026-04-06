# Moon Phase Light

A moon phase lamp that drives an SK6812 WWA (Cool White / Warm White / Amber) LED strip to reflect the current lunar illumination. Supports ESP8266 and ESP32 boards.

## Hardware

### Supported Boards

| Board                  | PlatformIO Environment  | LED Pin |
|------------------------|-------------------------|---------|
| Wemos D1 Mini (ESP8266)| `d1_mini`               | D1 (GPIO5) |
| ESP32 D1 Mini          | `esp32_d1_mini`         | D1 (GPIO5) |
| ESP32-S3 Super Mini    | `esp32s3_super_mini`    | GPIO1   |

### LEDs

- **Type:** SK6812 WWA strip, 5 LEDs
- **Power:** 5V via USB or external supply

### SK6812 WWA Channel Mapping

The SK6812 WWA uses the GRB protocol with the following physical mapping:

| Wire Position | Color       |
|---------------|-------------|
| R             | Cool White  |
| G             | Amber       |
| B             | Warm White  |

If your strip has a different channel order, adjust the `setPixelWWA()` function in `src/main.cpp`.

## Features

- **Moon phase calculation** -- Computes lunar phase from NTP time using the synodic month cycle. Updates every 60 seconds.
- **LED color progression:**
  - New moon -- dim amber glow
  - Waxing -- warm white increases, amber fades
  - Full moon -- bright cool white with warm white
  - Waning -- reverses back toward amber
- **Startup animations:**
  - Pulsing cool white while connecting to WiFi
  - Pulsing amber while waiting for NTP time sync
- **WiFiManager** -- Non-blocking captive portal that stays active after WiFi connects, accessible at the device's IP address.
- **Configurable brightness** -- Max brightness setting (0-255) in the portal, applied to all LED output including startup pulses.
- **MQTT with Home Assistant discovery** -- Optional MQTT integration that auto-registers a dimmable light entity in Home Assistant.

## Getting Started

### Build & Flash

This is a [PlatformIO](https://platformio.org/) project. Build and upload for a specific board:

```bash
pio run -e d1_mini -t upload
pio run -e esp32_d1_mini -t upload
pio run -e esp32s3_super_mini -t upload
```

### WiFi Setup

1. On first boot the device creates a WiFi access point called **MoonPhase-Setup**.
2. Connect to it and a captive portal will open.
3. Select your WiFi network and enter the password.
4. Optionally configure MQTT and brightness (see below).
5. The device saves the configuration and connects.

The config portal remains accessible at the device's IP address after WiFi connects.

### Settings

The WiFiManager portal exposes the following settings:

#### MQTT Settings

| Field              | Description                    | Default |
|--------------------|--------------------------------|---------|
| Enable MQTT        | Checkbox to enable MQTT        | Off     |
| MQTT Host          | Broker hostname or IP          |         |
| MQTT Port          | Broker port                    | 1883    |
| MQTT Username      | Leave blank if not required    |         |
| MQTT Password      | Leave blank if not required    |         |
| Use HA Discovery   | Publish Home Assistant discovery| Off     |

#### LED Settings

| Field              | Description                    | Default |
|--------------------|--------------------------------|---------|
| Max Brightness     | Maximum brightness (0-255)     | 255     |

All settings are persisted in LittleFS as `/config.json`.

### MQTT / Home Assistant

When MQTT is enabled, the device:

- Connects to the MQTT broker.
- Accepts **on/off** and **brightness** commands (brightness is capped to the configured max).
- Reports **moon phase** name and **illumination** percentage as part of the state payload.

When **Use HA Discovery** is also enabled, the device publishes a [MQTT discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery) config for a **light** entity in Home Assistant.

### MQTT Topics

Topics include the ESP chip ID (shown as `<id>`) for uniqueness:

| Topic                                          | Direction | Description            |
|------------------------------------------------|-----------|------------------------|
| `moonphase/<id>/state`                         | Publish   | JSON state             |
| `moonphase/<id>/set`                           | Subscribe | JSON commands          |
| `moonphase/<id>/available`                     | Publish   | `online` / `offline`   |
| `homeassistant/light/moonphase_<id>/config`    | Publish   | HA discovery payload   |

## License

See [LICENSE](LICENSE) for details.
