# ESP-NOW Candles

Retrofit battery (2×AA) flameless candles with an ESP32-C3 so they can be controlled
from Home Assistant (and HomeKit through HA), while the candle's own IR remote keeps working.

> **Read [docs/STATUS.md](docs/STATUS.md) first.** No warranty, personal use only, and the
> code is 100% AI-generated. It worked on my machine with my setup.

## How it works

```
Candles (ESP32-C3, battery) --ESP-NOW LR--> Hub B (ESP32) --UART--> Hub A (ESP32) --Wi-Fi/MQTT--> Home Assistant
```

- **Candles** sleep between check-ins and talk ESP-NOW (Long Range mode) to hub B. Each drives
  a wick LED (flicker in firmware) and a base LED, reads battery voltage, and listens to the
  stock IR remote directly, so the remote works even when HA or the hubs are down.
- **Hub B** is the ESP-NOW radio. It relays to **hub A** over a UART link.
- **Hub A** joins Wi-Fi, publishes to MQTT with Home Assistant discovery, and serves a setup page.
- **Wireless updates:** new candle and hub B firmware is uploaded to hub A, relayed down, and
  pulled by each candle at its next check-in. A failed update does not brick the candle.

## Hardware

- Circuit: [docs/schematic.svg](docs/schematic.svg). Board layout is up to you.
- Per candle: ESP32-C3 SuperMini, TPS61023 boost, HT7333 LDO, 2N7000, 38 kHz IR receiver,
  1000 µF cap, 120 Ω ×2, 100 kΩ.
- Hubs: 2× ESP32 DevKitC, wired TX/RX/GND plus shared 3V3.
- 3D models (`3d_models/`): candle deck (`candle_deck_v5.stl`), LED arm, hub house body/roof.
  Generators are Blender scripts in `candle_insert/` and `hub_house/`
  (`blender -b --python candle_insert/candle_deck_v5.py`). The deck fits *my* candles;
  measure yours and edit the parameters at the top of the script.

## Firmware

Arduino sketches in `hub_firmware/`: `candle`, `hub_a`, `hub_b`.

- ESP32 Arduino core 3.3.x · libraries: ArduinoJson 7.x, IRremote 4.x, PubSubClient 2.8.
- Hub A settings (Wi-Fi, MQTT, OTA password, hub name, topic prefix) live in the hub's flash
  and are entered on its **setup page**: on first boot it opens its own Wi-Fi network, then
  browse to `http://192.168.4.1`. Nothing secret is compiled in.
- `hub_a/secrets.example.h` exists only for seeding an existing hub; you do not need it.

## Scripts (Windows PowerShell 5.1 or 7)

Copy `candle.config.example.psd1` to `candle.config.psd1` (git-ignored) and fill it in.
Every setting can also come from a parameter or environment variable; see `hub_firmware/candle.common.ps1`.
Passwords never go in the config: use `$env:CANDLE_OTA_PASS` and `$env:CANDLE_MQTT_PASS`.

| Script | Does |
|---|---|
| `build_all.ps1` | Compile all three sketches into `build\` |
| `flash_v3.ps1 -Targets candle:COM3` | Compile and flash over USB (remove JP1 / batteries first) |
| `flash_checked.ps1` | Flash a hub only after it identifies itself on the port |
| `ota_candles.ps1` | Wireless update of every candle via hub A |
| `ota_hub_b.ps1` / `ota_hub_a_web.ps1` / `ota_hub_a.ps1` | Wireless update of the hubs |
| `mqtt_peek.ps1` / `mqtt_pub.ps1` | Read / publish candle topics |
| `candle_log.ps1` / `watch_attr.ps1` | Serial log capture / watch a candle's attributes |

## License

Personal, non-commercial use only. See [LICENSE](LICENSE).
