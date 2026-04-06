# Moon Light

An ESP8266-based moon phase lamp that drives an SK6812 WWA (Cool White / Warm White / Amber) LED strip to reflect the current lunar illumination.

## Hardware

- **Board:** Wemos D1 Mini (ESP8266)
- **LEDs:** SK6812 WWA strip, 3 LEDs, connected to pin **D1**
- **Power:** 5V via USB or external supply

### SK6812 WWA Channel Mapping

The SK6812 WWA uses the standard 4-byte GRBW protocol with the following physical mapping:

| Wire Position | Color       |
|---------------|-------------|
| G             | Cool White  |
| R             | Warm White  |
| B             | Amber       |
| W             | Unused      |

If your strip has a different channel order, adjust the `setPixelWWA()` function in `src/main.cpp`.

## Features

- **Moon phase calculation** — Computes lunar phase from NTP time using the synodic month cycle. Updates every 60 seconds.
- **LED color progression:**
  - New moon — dim amber glow
  - Waxing — warm white increases, amber fades
  - Full moon — bright cool white with warm white
  - Waning — reverses back toward amber
- **WiFiManager** — Captive portal for WiFi configuration on first boot.
- **MQTT with Home Assistant discovery** — Optional MQTT integration that auto-registers a dimmable light entity in Home Assistant.

## Getting Started

### Build & Flash

This is a [PlatformIO](https://platformio.org/) project. Build and upload with:

```bash
pio run -t upload
```

### WiFi Setup

1. On first boot the device creates a WiFi access point called **MoonLight-Setup**.
2. Connect to it and a captive portal will open.
3. Select your WiFi network and enter the password.
4. Optionally configure MQTT (see below).
5. The device saves the configuration and connects.

To **re-enter the config portal**, hold the **FLASH** button on the D1 Mini during boot.

### MQTT / Home Assistant

In the WiFiManager portal, check **Enable MQTT** to reveal the MQTT fields:

| Field         | Description                    | Default |
|---------------|--------------------------------|---------|
| MQTT Host     | Broker hostname or IP          |         |
| MQTT Port     | Broker port                    | 1883    |
| MQTT Username | Leave blank if not required    |         |
| MQTT Password | Leave blank if not required    |         |

When enabled, the device:

- Connects to the MQTT broker.
- Publishes a Home Assistant [MQTT discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery) config for a **light** entity.
- Accepts **on/off** and **brightness** commands from Home Assistant.
- Reports **moon phase** and **illumination** as part of the state payload.

### MQTT Topics

Topics include the ESP chip ID (shown as `<id>`) for uniqueness:

| Topic                              | Direction | Description            |
|------------------------------------|-----------|------------------------|
| `moonlight/<id>/state`             | Publish   | JSON state             |
| `moonlight/<id>/set`               | Subscribe | JSON commands          |
| `moonlight/<id>/available`         | Publish   | `online` / `offline`   |
| `homeassistant/light/moonlight_<id>/config` | Publish | HA discovery payload |

## License

See [LICENSE](LICENSE) for details.
