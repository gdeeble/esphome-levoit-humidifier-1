# ESPHome Levoit Humidifier

[![Compile](https://github.com/Ahmed-max/esphome-levoit-humidifier/actions/workflows/compile.yml/badge.svg)](https://github.com/Ahmed-max/esphome-levoit-humidifier/actions/workflows/compile.yml)

> [!WARNING]
> **Early development.** This works on the author's Dual 200S, but it is young, has only been
> tested on one unit (MCU firmware 2.0.02) and may change without notice. Back up the stock
> firmware before flashing and expect rough edges. Bug reports with logs are very welcome.

Local, cloud-free control of Levoit smart humidifiers with ESPHome. The ESP32 module that
ships inside the humidifier is reflashed with ESPHome and talks to the humidifier's own
controller (MCU) over UART, so no extra hardware is needed. Behaviour follows the stock
VeSync firmware, which was reverse engineered for this; see [docs/PROTOCOL.md](docs/PROTOCOL.md).

| Model | Code | Status |
|---|---|---|
| Dual 200S | LUH-D301S | 🧪 early development, tested on one unit (MCU firmware 2.0.02) |

**Contents:** [Quick start](#quick-start) · [Entities](#entities) ·
[Configuration](#configuration) · [Humidifier entity (MQTT)](#humidifier-entity-optional-mqtt) ·
[Behaviour](#behaviour) · [Flashing](#flashing) · [Troubleshooting](#troubleshooting) ·
[Limitations](#known-limitations)

## Quick start

Use the ready-made package. It brings in the board settings, the UART and every entity.
Pin a [release](https://github.com/Ahmed-max/esphome-levoit-humidifier/releases), and set the
same tag twice: once for the package, once (`levoit_ref`) for the component it builds.

```yaml
esphome:
  name: levoit-dual200s
  friendly_name: Levoit Dual 200S

packages:
  levoit:
    url: https://github.com/Ahmed-max/esphome-levoit-humidifier
    ref: v0.1.1
    files:
      - path: packages/dual200s.yaml
        vars:
          levoit_ref: v0.1.1

logger:
api:
  encryption:
    key: !secret levoit_api_key
ota:
  - platform: esphome
    password: !secret levoit_ota_password
wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
```

The first install has to be flashed over the serial header, see [Flashing](#flashing);
after that, updates go over Wi-Fi.

**Updating:** change both tags to the new release and install again. Changes are listed in
the [changelog](CHANGELOG.md). To follow development instead, use `ref: main` with
`refresh: 0s` and leave out `vars`; `main` can break at any time.

## Entities

The package adds these, grouped the way Home Assistant shows them on the device page.

| Section | Name | Platform / `type` | Notes |
|---|---|---|---|
| Controls | Power | `switch` / `power` | |
| | Mode | `select` / `mode` | `Auto` / `Low` / `High`, like the button on the device; switches the humidifier on |
| | Target humidity | `number` / `target_humidity` | 30–80 %, only in Auto mode while on |
| | Timer | `number` / `timer` | minutes until the power flips (off if running, on if off); counts down, 0 cancels. Ends early if the power is switched that way by other means |
| Sensors | Humidity | `sensor` / `humidity` | % |
| | Low water | `binary_sensor` / `low_water` | tank empty; only reported while running (the MCU says "lacks" whenever it is off) |
| | Water tank lifted | `binary_sensor` / `water_tank_lifted` | tank removed |
| | Mist active | `binary_sensor` / `mist_active` | on, tank in, water available and target not reached |
| | Target humidity reached | `binary_sensor` / `auto_stopped` | paused by Auto Off |
| | Clean tank reminder | `binary_sensor` / `clean_reminder` | kept by the controller, MCU 2.0.1 and later |
| Configuration | Display | `switch` / `display` | reads off while the humidifier is off, like the app |
| | Auto Off | `switch` / `auto_stop` | stop misting at the target; kept across reboots |
| | Reset clean tank reminder | `button` / `reset_clean_reminder` | |
| | Restart | `button` / `restart` (ESPHome) | |
| Diagnostic | MCU firmware | `text_sensor` / `mcu_version` | firmware of the humidifier controller |
| | Error | `binary_sensor` / `error` | controller reports a fault, code in the log |
| | Humidity high | `binary_sensor` / `humidity_high` | humidity above 80 % |
| | Wi-Fi signal | `sensor` / `wifi_signal` (ESPHome) | |

Also available but not in the package:

- `number` / `mist_level`: 1–2, same as Low / High.
- `sensor` / `temperature`: °C from the controller. The VeSync app never shows it; it is most
  likely only used internally to compensate the humidity sensor, so treat it as a rough value.

Every `levoit_humidifier` entity takes the usual ESPHome options (`name`, `icon`,
`internal`, `entity_category`, ...) plus its `type`. Icons, units and sections have sensible
defaults per type.

**Changing the entities.** To rename, drop or add entities, use the component directly (below)
and copy what you need from [packages/dual200s.yaml](packages/dual200s.yaml).

## Configuration

### Using the component directly

```yaml
external_components:
  - source: github://Ahmed-max/esphome-levoit-humidifier@main
    components: [levoit_humidifier]
    refresh: 0s

esp32:
  board: esp32dev
  flash_size: 4MB
  framework:
    type: esp-idf
    sdkconfig_options:
      CONFIG_FREERTOS_UNICORE: y   # the ESP32-SOLO-1C is single core

uart:
  tx_pin: GPIO17
  rx_pin: GPIO16
  baud_rate: 9600

levoit_humidifier:

switch:
  - platform: levoit_humidifier
    type: power
    name: Power
```

### Hub options

| Option | Default | Description |
|---|---|---|
| `id` | | ID of the hub, needed only with more than one humidifier |
| `uart_id` | | UART bus to use, if there is more than one |
| `model` | `DUAL200S` | Humidifier model; only the Dual 200S exists so far |
| `update_interval` | `5s` | How often the status is polled |
| `mqtt_humidifier` | `false` | Also announce a Home Assistant humidifier entity over MQTT, see below |
| `mqtt_discovery_prefix` | `homeassistant` | Discovery prefix for that entity |
| `wifi_icon` | `wifi` | What lights the Wi-Fi icon: `wifi` (connected to Wi-Fi), `home_assistant` (Home Assistant connected, the closest match to the stock "cloud connected"; needs `api:`) or `status` (on with Home Assistant, slow 1 s blink with Wi-Fi only, fast 0.5 s blink during an OTA update; needs `api:`). In every mode the fallback hotspot blinks fast, and like the stock firmware nothing but "on" is shown while the humidifier is off |

## Humidifier entity (optional, MQTT)

ESPHome has no humidifier entity type, so by default the main controls are separate Power,
Mode and Target humidity entities. With `mqtt_humidifier: true` the component also announces
a real Home Assistant **humidifier** entity over MQTT: the round dial with on/off,
Auto / Low / High, target humidity, current humidity and a humidifying / idle / off state.
It lands on the same device as the ESPHome entities, matched by MAC address.

It needs an MQTT broker (e.g. the Mosquitto add-on) and the MQTT integration in Home
Assistant. Add to your device YAML:

```yaml
levoit_humidifier:
  mqtt_humidifier: true

mqtt:
  broker: 192.168.178.10     # your broker
  username: !secret mqtt_username
  password: !secret mqtt_password
  discovery: false           # keep: otherwise every entity shows up a second time
```

- Turning the dial while in Low or High switches to Auto, like the app.
- The separate entities stay; hide them in Home Assistant if you only want the dial.
- If the broker is down only this one entity becomes unavailable.
- `mqtt_humidifier: false` is the default and needs no `mqtt:` block.
- **Turning it off again:** keep the `mqtt:` block for one install with
  `mqtt_humidifier: false` and the entity is removed. If you drop `mqtt:` straight away,
  delete the entity in Home Assistant (or clear the retained
  `homeassistant/humidifier/<device name>/humidifier/config` topic).

## Behaviour

### Control rules

Mode changes are always sent, also without water or with the tank lifted: the display shows
the chosen mode and the humidifier decides whether it can mist. A few changes are refused,
like in the stock firmware; the entity then snaps back and the log says why:

- the target can only be set in Auto while the humidifier is on
- the display can only be changed while the humidifier is on
- choosing a mode switches the humidifier on

### Display icons

- **Wi-Fi icon:** solid while connected, blinking while the fallback hotspot is up, off
  otherwise.
- **Timer icon:** lit while a timer runs.
- **Long presses** on the device's buttons, which started Wi-Fi pairing or a factory reset in
  the VeSync firmware, are ignored: pairing does not exist here and nothing on the ESP can be
  wiped by a stray press.

## Flashing

> [!CAUTION]
> Back up the stock firmware first. Levoit does not publish it, so the backup is the only
> way back to the VeSync app. The backup also contains your Wi-Fi password and the device's
> cloud keys: keep it private.

You need a 3.3 V USB-UART adapter (CP2102, CH340, FTDI) and [esptool](https://github.com/espressif/esptool)
(`pip install esptool`).

1. Unplug the humidifier, open the base and find the 6-pin `ISP` header next to the
   ESP32-SOLO-1C, marked `EN, GND, VDD, TXD, RXD, IO0` (pin 1 = `EN`, square pad).
2. Connect the adapter: adapter RX → `TXD`, adapter TX → `RXD`, GND → `GND`.
   Leave `VDD` unconnected: a USB-UART 3.3 V pin cannot supply the board. The humidifier
   powers it from its own DC adapter.
3. Bridge `IO0` to `GND` and plug the humidifier in to enter the bootloader.
4. Back up the full 4 MB flash, ideally twice, and compare:
   ```bash
   esptool --port COMx --baud 460800 read-flash 0 ALL dual200s_stock.bin
   ```
5. In ESPHome, *Install → Manual download → Factory format*, then flash it
   (replug the humidifier with `IO0` still bridged first):
   ```bash
   esptool --port COMx --baud 460800 write-flash 0 levoit-dual200s.factory.bin
   ```
6. Remove the `IO0` bridge and power-cycle.

**Back to stock:** write the backup to address 0 the same way.

## Troubleshooting

- **What the humidifier reports:** every status change is logged as
  `Status: power=... mode=... raw=...`. The `raw` bytes are what the MCU sent; please include
  that line, and what the device really shows, in bug reports.
- **`MCU did not ACK command 0x....`:** the controller is not answering. Check the UART pins
  (TX GPIO17, RX GPIO16, 9600 baud).
- **`Cannot set ...` / `Manual mode cannot set target humidity`** (Low / High are manual): a change the stock firmware
  would refuse too, see [Behaviour](#behaviour).
- **Changes on GitHub do not show up:** keep `refresh: 0s` on the package, or use
  *Clean Build Files* in the ESPHome dashboard before installing.
- **Old entities stay "Unavailable" after an update:** entities that were renamed or removed;
  delete them in Home Assistant under *Settings → Entities*.

## Known limitations

- Only the Dual 200S, tested on a single unit with MCU firmware 2.0.02.
- The timer lives on the ESP and is lost when it reboots (the stock firmware does the same).
- Settings kept across reboots (Auto Off) are stored per hub `id`; renaming the `id` resets them.
- Not ported: schedules (use Home Assistant automations), usage history, sleep mode
  (rejected on this model by the stock firmware too) and Wi-Fi pairing.
- The tank-cleaning reminder needs MCU firmware 2.0.1 or later, where the controller keeps it.

## Development

The UART protocol lives in [protocol.h](components/levoit_humidifier/protocol.h) without ESPHome
dependencies, so it is unit tested on the host with frames captured from a real device:

```bash
g++ -std=c++17 -Wall -Wextra -Werror -I components tests/host/test_protocol.cpp -o test_protocol
./test_protocol
```

CI runs these tests and compiles the configs in `components/levoit_humidifier/tests`. A compiler
warning in the component fails the build.

## Credits

Structure inspired by [tuct/levoit](https://github.com/tuct/levoit) (Levoit air purifiers).
Licensed under the [MIT License](LICENSE).
