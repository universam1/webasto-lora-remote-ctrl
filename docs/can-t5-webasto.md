# VW T5.2 Webasto WWZH on CAN

Vehicle: VW T5.2, built 2013, factory water heater (WWZH, Webasto Thermo Top),
roof control unit 7E7906453C (heat, ventilate, timer).

**Status 2026-11-22: the heater can be switched on, ventilated and switched off over CAN.**
The ESP32 sends 0x5D3 at the radio connector. This works with the ignition on or off, and
it works when the bus is asleep. Verified in the car; the open points are listed below.

## Quick reference

| Purpose | ID | Payload | Notes |
|---|---|---|---|
| Heat on | 0x5D3 | `02 00` | send 3×, 500 ms apart |
| Ventilate on | 0x5D3 | `04 00` | same |
| Off | 0x5D3 | `00 00` | same |
| Heater status | 0x3E5 | 8 bytes | every 200 ms while the bus is awake, see layout |
| Heater active echo | 0x3E3 | byte 4 `01`/`00` | about 40–120 ms after 0x3E5 |

The bus runs at 100 kbit/s. Tap point: radio Quadlock **A9 = CAN-H, A10 = CAN-L**, plus GND.
**No terminating resistor**: the vehicle bus is already terminated.

## Hardware that works

- LOLIN ESP32-S2 mini, native USB CDC (`/dev/ttyACM*`, udev symlink `/dev/ttyCANSNIFF`).
- SN65HVD230 module (3.3 V):
  - CTX = GPIO11, CRX = GPIO9.
  - Rs is not wired, because the module ties it to GND.
- The SN65HVD230 reads this bus reliably: 0 missed frames, 1–2 bus errors over hours.
  We expected fault-tolerant low-speed CAN that would need a TJA1055, but on this bus the
  SN65HVD230 is enough.
- Lessons from the first board (ESP32 DevKit with CP2102):
  - The CP2102 garbles output at 1 Mbaud. 921600 works.
  - The S2 has no such limit, because it uses native USB.
- The S2 native USB only sends while the host asserts DTR. `mcp-can-capture` does this
  automatically for the Espressif VID 0x303A.
- A laptop suspend or resume (S3) re-enumerates the S2, so the port changes, for example
  ACM0 to ACM1. The udev symlink follows the change. Captures running at that moment are
  lost.

## Bus behaviour

| State | Frames/s | IDs |
|---|---|---|
| Ignition on, at the radio | about 330 | 56 |
| Ignition off, just after locking | about 147 | 45 |
| Asleep | 0 | 0 |

- **The bus falls asleep** about 20 s after the last activity.
  - The heater does **not** keep the bus awake. It keeps running while the bus sleeps.
    In the 2 min test, fuel went from 0 to 10 ml while the bus was silent.
  - During its run-on, the heater wakes the bus by itself for about 10 s at a time.
- **A single 0x5D3 frame wakes the bus.**
  - Gateway and control units start sending within about 3 ms.
  - No separate network-management or wake-up frame is needed.
- **J533 forwards traffic in both directions** between the radio bus and the heater:
  - The heater's 0x3E5 status reaches the radio.
  - Our 0x5D3 from the radio side reaches the heater.
- **Opening the car** (door or remote) wakes the bus.

## 0x3E5, heater status

The layout comes from T5.1 reports (https://tx-board.de/threads/frame-fuer-standheizung.36723/).
On our T5.2, bytes 1, 2/3, 4, 5, 6 and 7 are confirmed by observation.

| Byte (0-based) | Content | Confirmed on T5.2 |
|---|---|---|
| 0 | b0 on via radio, b1 off via radio, b2 heating on, b3 ventilation on, b4 A/C off, b5 heater mode activating, b6 programming, b7 wake Komfort-CAN | always `00` so far |
| 1 | b0 vehicle blower, b1 control LED (heater requested), b2 combustion, b3 water pump request, b4-6 error (0 none, 1 battery low, 2 low fuel, 3 defective, 4 crash, 5 other, 6 overheat), b7 error stored | b0 (ventilation), b1, b3 |
| 2–3 | fuel used, u16 LE, low 14 bits = ml; b14 parking heater (always 1), b15 Kl.30 lost | yes, counts ml |
| 4 | heating power, raw (0x44 at ignition, 0xF1 = 241 full load, 0x51/0x5A partial) | yes |
| 5 | coolant temperature = −48 + 0.75·x °C, 0xFF error | yes |
| 6 | b0 fuel pump (dosing pump during priming), b1 water warmed up | yes |
| 7 | b0 ventilation active | yes |

Typical payloads:

| State | Payload |
|---|---|
| Off, idle (9 °C) | `00 00 00 40 00 4C 00 00` |
| Start, priming | `00 02 00 40 00 4C 01 00` |
| Heating, full load | `00 0A 0B 40 F1 65 02 00` |
| Run-on after off (water pump) | `00 08 0A 40 00 7C 02 00` |
| Ventilating | `00 09 08 40 00 70 02 01` |

The fuel counter starts at 0 for each heater run. It is not reset by switching off; the
run-on above still shows `0A` = 10 ml.

## 0x5D3, requested mode

DLC 2, cyclic every 500 ms. The sender is probably the roof unit, possibly relayed by J533.
Byte 0 is the mode: `00` off, `02` heat, `04` ventilate. Byte 1 was always `00`.

**Reading the roof unit.** Two capture runs at the radio covered all operations: heat on/off
and ventilate on/off, 8 of 8 transitions.
- Each time, 0x5D3 changed 13–180 ms before 0x3E5 reacted.
- No other ID changed with these events.
- Captures: `roof_panel_actions`, `roof_panel_actions_2`.

**Sending.** Send 3× `5D3 <mode> 00`, 500 ms apart. The device stays in normal mode only for
the burst.
- After our frame, the original sender adopts the new mode and keeps sending it. A short
  burst is therefore enough, and there is no need to keep sending.
- The original sender keeps transmitting its old value for 0.5–1 s. During that time the
  LED can flicker once. This is harmless; the heater ends up in the commanded state.
- Results:

| Test | Condition | Result |
|---|---|---|
| `tx_test_heat_direct` | Ignition on, heat on | Heater starts. 0x5D3 `02 00` sticks from the 3rd frame on. |
| `tx_test_heat_off_direct` | Ignition on, off | LED off after the 1st frame, normal run-on. |
| `ign_off_sleep` | Ignition off, **bus asleep** (45 s silence), heat on | Bus wakes in 3 ms, the 1st frame sticks, heater starts after 0.57 s. |
| `ign_off_sleep` | Bus asleep again (43 s), off | Wakes, heater off after about 1 s of back and forth with the roof unit. |
| `heat_2min` | Bus asleep, heat on, about 4:20 min run, off | See next section. |

Bus errors stayed at 1–2 in all tests. No frames were missed.

## Heater run (`heat_2min`)

| t | Coolant | Power | Fuel | State |
|---|---|---|---|---|
| 0.6 s | 9.75 °C | 0 | 0 ml | LED on, dosing pump priming |
| 21 s | 9.75 °C | 0 | 0 ml | priming done |
| 42 s | 10.5 °C | 68 | 0 ml | water pump on, ignition |
| 163 s | 27.75 °C | 241 | 10 ml | full load |
| 213 s | 40.5 °C | 241 | 19 ml | |
| 260 s | 48.0 °C | 241 → 0 | 26 ml | **off** via `5D3 00 00` |
| 269 s | 49.5 °C | 0 | 26 ml | water pump run-on |

- Coolant rose about 0.25 K/s at full load.
- Fuel use at full load was 16 ml in 97 s, about **0.6 l/h**. This fits a Thermo Top of
  about 5 kW.
- About 40 s pass from the command to ignition: priming, then glow.

## Other IDs

- **0x3E1** (`20 00 10 00 xx …`, 100 ms): changes when the **vehicle blower** is operated.
  It is not heater related. 0x5E1 byte 0 bit 7 toggles together with it.
- **0x3E3** byte 4: `01` while the heater is active. It follows 0x3E5 and looks like an
  echo or ack.
- **0x62C, 0x63B, 0x66F, 0x6D2, 0x6DA**: variable DLC and content. Probably radio or cluster
  text and transport, not relevant here.
- From opendbc `vw_pq.dbc`: `Klima_1` 0x5E0 bit 1 `Fahrerwunsch_Zuheizer`, and
  `Verbauliste_1` 0x5D8 bit 39 `Standheizung`. Neither was observed at the radio.

## Tooling

- `src/cansniffer/main.cpp` (env `cansniffer`, or `cansniffer_tx` for TX): firmware that
  speaks GVRET, so it works with SavvyCAN.
  - It starts listen-only. TX exists only in `cansniffer_tx` and must also be enabled at
    runtime.
  - Text mode before GVRET: `help`, `stats`, `mon on|off`, `rate <bps>`, `scan`.
- `mcp-can-capture/`: an MCP server for Claude Code.
  - `connect`, `start_capture`/`stop_capture`, `mark_event`, `summarize`, `diff`,
    `decode_3e5`.
  - `enable_tx` + `send_frame`: the device is in normal mode only for the burst, and the
    server always switches it back to listen-only.
  - Captures are written as SavvyCAN CSV, candump log and markers JSONL into
    `mcp-can-capture/captures/` (gitignored).
- Working method that proved itself when reading out the roof unit:
  1. Set a marker.
  2. Then press the button.
  3. Then diff ±15 s around the marker.
  
  Pressing first and marking afterwards misses the transition.
- Only one program can use the serial port at a time: the MCP server, SavvyCAN or the
  dashboard.

## Implications for the receiver (TTGO, replacing W-Bus)

- The receiver needs an SN65HVD230 on free GPIOs (21/22 on the TTGO LoRa32). It taps the
  radio connector or the Komfort-CAN.
- Start, ventilate and stop are each 3 frames of 0x5D3. Keep-alive and renewal as with W-Bus
  are not needed. The heater uses its own default run time.
- To read the status, the receiver wakes the bus with one frame and then reads 0x3E5 for
  about 1 s. The open question is which frame to send for that without side effects.
  Candidate: repeat the current mode in 0x5D3; this worked in `heat_2min` at 163 s.
- Do not keep the bus awake permanently, because of the starter battery. At about 20 s per
  status poll, poll rarely.
- The transceiver TX pin must be recessive (HIGH) from power-up. A pull-up of about 10 kΩ
  from CTX to 3.3 V is recommended, so that a reset cannot wake or disturb the bus.

## Open

1. **Long sleep:** does one frame still wake the bus after hours or overnight? The gateway may
   sleep deeper. The test is planned; keep the laptop from suspending
   (`systemd-inhibit --what=sleep:idle sleep infinity`).
2. **Fault memory**, address 18 (Standheizung): read it with VCDS after the tests.
3. **Run time and timer:** how the roof unit sets them. 0x5D3 byte 1 was always `00`. Which
   default run time applies to a CAN start?
4. **Status poll without side effects**, see the receiver section.
5. Who sends 0x5D3: roof unit or J533? This matters only if the sender behaves differently
   with the ignition on or off.

## Background research (before the tests)

- Heater on Komfort-CAN: orange/green = CAN-H, orange/brown = CAN-L.
  - Heater connector T6au: pin 1 W-Bus, pin 2 CAN-H, pin 3 CAN-L.
  - Source: https://tx-board.de/threads/can-bus-zuheizer.55297/
- Alternative tap points: A-pillar connector station, blue 10-pin, pins 4/5, or the KI↔BCM
  line.
- Radio Quadlock pinout: http://www.passatplus.de/umbauteile/stecker/stecker4.htm
- T5.1 heaters use "CAN 1.6", T5.2 "CAN 2.0".
- Public sources described no CAN start command. All known retrofits use W-Bus on pin 1.
- Diagnostics:
  - Address 0x18 (Standheizung).
  - TP2.0 channel setup on 0x200, reply 0x218, output tests via KWP2000.
  - VCDS adaptation channel 42 unlocks the heater after 3 failed starts.
- Risks:
  - Keeping the bus awake drains the starter battery.
  - Odd CAN signals can lead to heater faults 49/53 and a locked heater.
  - Spoofing 0x3E5 collides with the real heater.
  - Never add a terminator.
