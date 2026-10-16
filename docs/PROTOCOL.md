# Levoit Dual 200S (LUH-D301S-WEU) — ESP32 ⇄ MCU UART protocol

Recovered by static analysis of the stock VeSync ESP32 firmware 1.0.09 (built 2023-06-26)
with Ghidra 12.1.4, then confirmed on a live unit (MCU firmware 2.0.02).

Confidence tags: **[code]** read directly from decompiled code / constants,
**[infer]** reasoned from code, needs confirmation with a capture.

## Physical link

| Item | Value | Source |
|---|---|---|
| ESP32 UART | UART2 | [infer] `vesync_device_info_get_int` flag 0x11/0x12 → 2 |
| ESP32 TX → MCU | GPIO17 | confirmed on hardware |
| ESP32 RX ← MCU | GPIO16 | confirmed on hardware |
| Baud | 9600 8N1 | confirmed on hardware |
| Console / ISP header | UART0 (GPIO1/3) 115200 | [code] boot log |

Same pins/baud as the Levoit LV600S humidifier ESPHome port.

## Transport frame  [code]

```
+------+------+-----+--------+--------+----------+------------------+
| 0xA5 | ctrl | seq | len_lo | len_hi | checksum | payload[len]     |
+------+------+-----+--------+--------+----------+------------------+
checksum = 0xFF - (sum of every other byte in the frame, incl. 0xA5) & 0xFF
           (i.e. all bytes including checksum sum to 0xFF mod 256)
len      < 0x5DD
```

`ctrl` bits:

| bits | meaning |
|---|---|
| 0x0F | protocol version, must be `2` |
| 0x10 | frame is an ACK |
| 0x20 | sender wants an ACK |
| 0x40 | error flag (in ACKs) |

- ESP → MCU command: `ctrl = 0x22`, `seq` increments per new frame. Only one
  outstanding frame; resent by a 1000 ms timer until the ACK with the same `seq` arrives.
- ACK: `ctrl = 0x12` (or `0x52` on error), `seq` copied from the frame being acked.
- MCU → ESP frames: low nibble 2, bit 0x10 clear. ESP stores their `seq` for its ACK.

## Payload  [code]

```
[0] 0x01          payload version (receiver rejects >= 2)
[1] cmd_lo
[2] cmd_hi        16-bit opcode, little endian
[3] status/err    0 in commands; error code in ACKs (0x34 = payload < 4, 0x1F = bad version)
[4..] data
```

## ESP → MCU commands  [code]

| Opcode | Function | Data |
|---|---|---|
| 0xA000 | set_power | `[on]` 0/1 |
| 0xA105 | set_display | `[brightness]` 0 = off, 100 = on |
| 0xA129 | set_wifi_led | `[mode 0/1/2, on_ms u16, off_ms u16, x]` [infer names] |
| 0xA260 | set_manual_mode_lvl | `[0, 1, level]`, level 1–2 |
| 0xA26A | set_timer_logo | `[on]` timer icon |
| 0xA5E5 | set_auto_stop | `[on]` stop when target reached |
| 0x4080 | set_auto_mode | `[target, target-5, target+5, 2, 1, 1]` |
| 0x40C0 | query_mcu_status | none → ACK carries status block |
| 0x40C3 | set_container_clean | `[on]` |
| 0x4081 | found by scan: sets the mode? `[1,1,…]` gave manual / mist 1 | [infer] |
| 0x4082 | found by scan: exists, rejected `[1,1,D2,50,00]` (err 0x14) | [infer: Classic 300S uses it for a humidity mode] |
| 0x40C2 | **set_night_light** (found by scan) | `[0]` off · `[1, 0]` colour cycle · `[1, 1, r, g, b]` fixed colour - **any RGB**, not only the button colours |
| 0x40C5 | found by scan: **overrides the reported humidity** with `data[0]` until power cycle - do not send | |
| 0xD004 | production_progress | 3 bytes (factory) |
| 0xD005 | production_result | 2 bytes (factory) |
| 0xD007 | uart_test | (factory) |
| 0xD101 | reboot_mcu | `[param]` |

## Display / UI behaviour of the stock firmware  [code]

Wi-Fi icon, `set_wifi_led` (0xA129) data `[mode, on_ms u16, off_ms u16, repeat]`,
mode 0 = off, 1 = on, 2 = blink. Stock LED behaviour table (network state → entry):

| State | When | Entry |
|---|---|---|
| 1 | boot | off |
| 2 | pairing (BLE/AP config) | blink 500/500 ms |
| 3 | pairing ended / timed out | off |
| 4 | receiving credentials | blink 125/125 ms |
| 6–10 | connecting, IP, server | off |
| 0xB | cloud connected | on |
| 0xC | factory reset | blink 125 ms, repeat 10 |
| 0xD | disconnected / test mode | off |

- Non-connected states are not sent while the humidifier is off; the current state is
  re-sent when it is switched on.
- Turning the display on while connected re-sends `on`.
- `set_display` is refused while the humidifier is off.
- Timer icon, `set_timer_logo` (0xA26A) `[on]`: 1 while a timer runs, 0 when it expires,
  is removed, and once at boot.
- Tank-cleaning reminder: on MCU 1.0.0–1.0.13 and 2.0.0 the ESP counts runtime and sends
  0x40C3; newer MCUs (e.g. 2.0.02) keep the reminder themselves (status byte 20) and the ESP
  only sends 0x40C4 `[0]` to reset it.

## MCU → ESP frames  [code]

| Opcode | Meaning | ESP must ACK? |
|---|---|---|
| 0x40C1 | status report (block below) | no |
| 0x40C4 | tank-cleaning reminder reset on the device | no |
| 0xD102–0xD103 | button event, `data[0]`: 2/3 = start Wi-Fi pairing, 4 = factory reset | yes if ctrl&0x20 |
| 0xD104 | enter factory test mode | yes if ctrl&0x20 |

ACK reply format: `A5 12 <their seq> 04 00 <cs> 01 <cmd_lo> <cmd_hi> 00`.

## Status block (data of 0x40C1, and of the ACK to 0x40C0)  [code]

| Offset | Field | Notes |
|---|---|---|
| 0 | MCU fw patch | version = `[2].[1].[0]` |
| 1 | MCU fw minor | |
| 2 | MCU fw major | |
| 3 | enabled | power on |
| 4 | water_tank_lifted | 1 = tank removed (reported to the app as `water_tank_lifted`) |
| 5 | water_lacks | tank empty |
| 6 | auto_stop_status | auto-stop reached |
| 7 | display_config | display on |
| 8 | fog_status | not reported to the app, 1 even while off; meaning unknown |
| 9 | tgt_humidity | % |
| 10 | cur_humidity | % |
| 11 | cur_temp | unit unknown [infer °C] |
| 12 | mode | 0 = auto, 1 = manual (stock internal mode 4) |
| 13 | mist_lvl | 1–2 |
| 14 | other_exception | error flags |
| 15 | night light on | 1 = on, 0 = off (US model with the 7-colour light) |
| 16 | night light mode | 0 = colour cycle, 1 = fixed colour |
| 17–19 | night light RGB | fixed colour; kept while off, `FF FF FF` in cycle mode |
| 20 | container_clean_status | |
| 21–24 | unknown | always `FF` so far (no brightness byte found) |

Total copied: 25 bytes.

## Example frames (computed)

```
power on      A5 22 01 05 00 90 01 00 A0 00 01
power off     A5 22 02 05 00 90 01 00 A0 00 00
query status  A5 22 03 04 00 30 01 C0 40 00
mist level 2  A5 22 04 07 00 27 01 60 A2 00 00 01 02
auto, 50 %    A5 22 05 0A 00 CE 01 80 40 00 32 2D 37 02 01 01
display off   A5 22 06 05 00 86 01 05 A1 00 00
```

## Still to confirm on hardware

- `cur_temp` unit, `other_exception` bits.
- ~~Whether the MCU pushes 0x40C1 on its own~~: it does (MCU 2.0.02), e.g. right after the
  night light button is pressed. The component polls 0x40C0 every 5 s either way.
- ESP → MCU command that sets the night light (opcode and data unknown, see below).
- Position of the clean-reminder flag (byte 20); it has only been seen as 0.
- Which physical long-press sends which 0xD102/0xD103 value.

## Night light (US Dual 200S, MCU 2.0.02)

The US unit has a 7-colour night light, controllable from the VeSync app, so it goes through
the MCU. Its state is in status bytes 15–19 (decoded from captures, 2026-10-15). The button
on the device steps through:

| Step | byte 15 | byte 16 | RGB (17–19) |
|---|---|---|---|
| Off | 00 | (last) | (last) |
| Colour cycle | 01 | 00 | FF FF FF |
| White | 01 | 01 | FF FF FF |
| Red | 01 | 01 | D7 1E 78 |
| Purple | 01 | 01 | 28 00 FF |
| Light purple | 01 | 01 | 14 14 BE |
| Blue | 01 | 01 | 14 28 8C |
| Aqua | 01 | 01 | 14 E6 B4 |
| Orange | 01 | 01 | D2 50 00 |

Confirmed on hardware 2026-10-15 with the night-light branch (two full button cycles,
humidifier off, tank in): every step decodes as above, and Off only clears byte 15 (the
tank flag in an earlier capture was unrelated). The night light works while the
humidifier itself is off.

~~The set command is not known yet.~~ Solved: 0xA003 was a dead end; 0x40C2 (found by scanning) is the night light, layout in the command table. Original guess: the Classic 300S sets its (single-colour)
night light with 0xA003 `[1, brightness%]`, and it shares the A0xx/A1xx/A2xx opcode family
with this model, so 0xA003 with `[on, mode, r, g, b]` (the status layout) is the first
thing to try.

### Night light set command: search log

| Date | Probe | Result |
|---|---|---|
| 2026-10-15 | 0xA003 with `[1,1,D2,50,00]`, `[0,1,D2,50,00]`, `[1,0,FF,FF,FF]`, `[1,64]` | no answer at all (3 resends each) |
| 2026-10-15 | control: 0xA26A `[0]` and `[0,0]` | both ACKed: the MCU ignores data length, and stays silent for opcodes it has no handler for |
| 2026-10-15 | scan 0xA100–0xA1FF, data `[1,1,D2,50,00]` | no ACKs besides the known 0xA105 / 0xA129 (skipped). Scan had no positive control yet |
| 2026-10-15 | scan 0xA000–0xA5FF, same data | night light came on roughly 120 opcodes in (around 0xA060–0xA090); log not kept, narrowing down |
| 2026-10-15 | scan 0x4000–0x40FF, same data, control OK | ACKed: **0x4081** (mode went to manual / Low), **0x4082** (error flag, err 0x14), **0x40C2** (the night light - see below), **0x40C5** (reported humidity became 1 %, i.e. `data[0]`; held in MCU RAM, a power cycle restored the real 55 %). Confirmed 0x40C5 alone by sending it separately. All four are now skipped by scans |
| 2026-10-15 | 0x40C2 `[1]` on its own, light off before | **night light turned on - 0x40C2 is the night light.** The "around 120 opcodes in" sighting was this 0x40xx scan (0x40C2 is ~190 in, close enough by eye), not the A0xx sweep. The scan's `[1,1,D2,50,00]` left it in colour cycle, not fixed orange, so the layout is not simply the status bytes |
| 2026-10-15 | 0x40C2 `[0]` | off |
| 2026-10-15 | 0x40C2 `[1,0,D2,50,00]` | on, colour cycle |
| 2026-10-15 | 0x40C2 `[1,1]` / `[1,0]` | fixed colour / colour cycle - **byte 1 = mode, same values as status byte 16** |
| 2026-10-15 | 0x40C2 `[1,64,1,D2,50,00]` (6 bytes) | ACKed, no change - wrong length ignored? |
| 2026-10-15 | 0x40C2 `[1,2,D2,50,00]` | ACKed, no change - mode 2 invalid |
| 2026-10-15 | 0x40C2 `[1,1,D2,50,00]`, `[1,1,14,E6,B4]`, `[1,1,FF,00,00]`, `[1,1,00,FF,00]` | orange, aqua, pure red, pure green - **full RGB works**. Layout solved |
