# VW T5.2 Webasto WWZH on CAN

Vehicle: VW T5.2, built 2013, factory water heater (WWZH, Webasto Thermo Top),
roof control unit 7E7906453C (heat, ventilate, timer).

**Status 2026-11-22 (evening): heat on and off work over CAN; ventilate-on currently does
not.** The ESP32 sends 0x5D3 at the radio connector, with the ignition on or off and with
the bus asleep. Since about 22:30 the roof unit sets bit 7 in 0x5D3 byte 0 (`80/82/84`).
Commands must mirror that bit and be repeated until the roof unit echoes them. In this
`8x` state the roof unit never adopts ventilate (`84`) from CAN. Verified in the car; the
open points are listed below.

## Quick reference

| Purpose | ID | Payload | Notes |
|---|---|---|---|
| Heat on | 0x5D3 | `02 00` / `82 00` | bit 7 as the roof unit currently sends it; repeat every 500 ms until the roof unit echoes it, max about 10× |
| Ventilate on | 0x5D3 | `04 00` / `84 00` | **never adopted over CAN** in either state (`04` in the dashboard at 22:28, `84` later); the heater ventilates only while our frame is on the bus |
| Off | 0x5D3 | `00 00` / `80 00` | as heat on |
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

- **The bus falls asleep** about 20 s after a mode change. A wake-up without a mode change
  (a 0x5D3 equal to the current value, the 0x3E5 spoof, the heater's own wake-ups) keeps it
  awake for only about 5.3 s.
  - The heater does **not** keep the bus awake. It keeps running while the bus sleeps.
    In the 2 min test, fuel went from 0 to 10 ml while the bus was silent.
  - During its run-on, the heater wakes the bus by itself for about 5 s at a time; the
    first frame is then 0x427.
- **A single 0x5D3 frame wakes the bus.**
  - Gateway and control units start sending within about 3 ms.
  - No separate network-management or wake-up frame is needed.
- **J533 forwards traffic in both directions** between the radio bus and the heater:
  - The heater's 0x3E5 status reaches the radio.
  - Our 0x5D3 from the radio side reaches the heater.
- **Opening the car** (door or remote) wakes the bus. A panel press on a sleeping bus also
  starts with another ECU (first frame 0x438), not with 0x5D3.
- **Every wake-up**, independent of the mode, shows the same pattern in all evening captures:
  0x665 byte 0 goes `00` → `80`, and 0x3E1 goes from `C0 …` to `E0 … FA …` (byte 4 `FA`,
  later `00`). With the ignition on, 0x665 is `80` and 0x3E1 `20 …` throughout.
- Sending into a sleeping bus is fine: our frame is ACKed and wakes the bus.

## 0x3E5, heater status

The layout comes from T5.1 reports (https://tx-board.de/threads/frame-fuer-standheizung.36723/).
On our T5.2, bytes 1, 2/3, 4, 5, 6 and 7 are confirmed by observation.

| Byte (0-based) | Content | Confirmed on T5.2 |
|---|---|---|
| 0 | b0 on via radio, b1 off via radio, b2 heating on, b3 ventilation on, b4 A/C off, b5 heater mode activating, b6 programming, b7 wake Komfort-CAN | always `00` (6115 frames in all captures); see below |
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

**Byte 0 is not a command path.** The forum numbers bytes from 1, so its "Byte 1" is our
byte 0. There, bit 7 is "Komfort CAN aufwecken", and bits 0–3 (on, off, heating,
ventilation via radio) are sent 5× each. Test (`off_retry`, marker `tx_3e5_wake_vent`):
5× `3E5 89 00 00 40 00 92 02 00` at 100 ms (wake + on via radio + ventilation, the other
bytes copied from the real frame).
- The bus woke, the roof unit stayed at `80`, the real 0x3E5 showed no ventilation, and no
  extra bus errors occurred.
- These bits are most likely heater-side reports of externally triggered starts (radio,
  W-Bus). Spoofing 0x3E5 is not useful.

## 0x5D3, requested mode

DLC 2, cyclic every 500 ms. The sender is probably the roof unit, possibly relayed by J533.
Byte 0 is the mode: `00` off, `02` heat, `04` ventilate, plus bit 7 (see below). Byte 1 was
always `00`; the run time is not in this frame.

**Reading the roof unit.** Two capture runs at the radio covered all operations: heat on/off
and ventilate on/off, 8 of 8 transitions.
- Each time, 0x5D3 changed 13–180 ms before 0x3E5 reacted.
- No other ID changed with these events.
- Captures: `roof_panel_actions`, `roof_panel_actions_2`.

**Byte 0 bit 7 (`0x80`).** Meaning unknown.
- Until `heat_2min` (ends 22:16) the roof unit sent `00/02/04`. From `vent_manual`
  (22:30) on it sends `80/82/84`. In between came several rapid dashboard commands
  (vent, vent, heat, off within about 50 s), not captured.
- A heat start from the panel at 22:33 kept the bit: `80` → `82` (`vent_manual`, 128.6 s;
  the bus was woken by 0x438, 0x3E5 LED on 0.16 s later).
- Only commands whose bit 7 matches the roof unit's current value are adopted. In
  `vent_manual`, 3× `04 00` while the roof unit sent `80`: 0x3E5 showed ventilation from
  0.57 to 1.37 s, then the roof unit's `80` won. The dashboard showed this as "starts 3×
  briefly and aborts".

**Sending.** Repeat `5D3 <mode | bit 7> 00` every 500 ms until the roof unit sends the same
value, then stop. The device stays in normal mode only for the burst.
- After adoption the roof unit keeps sending the new mode, so no keep-alive is needed.
- A single frame is not enough: the heater reacts for about 0.4 s, then the roof unit's next
  frame overrides it (`bit7_tests`, `tx_heat_bit7_once`).
- When a command was adopted, the roof unit echoed it on the 2nd–4th frame:

| Capture, marker | Command | Adopted |
|---|---|---|
| `bit7_tests`, `tx_heat_bit7_10x` | 10× `82 00` | 3rd frame, heater on |
| `bit7_tests`, `tx_off_bit7_10x` | 10× `80 00` during full load | **never**, the roof unit stayed `82` |
| `off_retry` (start, 72 s later) | 10× `80 00` | 4th frame, off |
| `vent_manual`, `tx_off_bit7` | 3× `80 00` | 2nd frame, off |
| `panel_vent`, `tx_vent_off_80` | 10× `80 00` | 3rd frame, ventilation off |
| `off_retry`, `tx_vent_bit7_10x` | 10× `84 00`, 500 ms, run-on | never; ventilation on for about 0.2–0.4 s per frame |
| `off_retry`, `tx_vent_bit7_200ms` | 25× `84 00`, 200 ms, after run-on | never; ventilation toggles on/off every 200 ms |

  Correction: ventilate-on over CAN **never** worked, not even in the morning. The morning `04`
  transitions in `roof_panel_actions*` came from the panel itself, and the dashboard `04` at
  22:28 (state unknown, no capture) failed the same way. So bit 7 is probably **not** the
  reason why vent is rejected.

**Why heat is adopted and vent is not (hypothesis, 2026-11-23 analysis):**

| 0x3E5 while our frames are on the bus | byte 1 | byte 7 | roof unit |
|---|---|---|---|
| heat (`82`) | `0A`/`0B`: **b1 control LED** + b3 water pump (+ b0) | `00` | adopts after 1–4 frames |
| vent (`84`) | `09`: b0 vehicle blower + b3 water pump, **LED off** | `01` | never adopts |
| vent started on the panel | `09` | `01` | panel already sends `84` |

- The heater itself reacts correctly to our `84`. It sets the Climatic bits (blower and
  ventilation active), so the heater→Climatic path works.
- The roof unit follows a heater that reports itself active through the control LED bit, but
  ventilation never sets that bit.
- For starts not made on the panel (radio/Telestart), the forum layout gives 0x3E5 byte 0:
  - b0: on via radio
  - b3: explicit ventilation via radio
  - b7: wake Komfort-CAN
  
  Each is sent 5× at the moment of switching. This is probably how the panel learns about an
  external vent start.
- Our spoof test (`3E5 89 00 00 40 …`) was **not** a valid test of this. The heater was off,
  no `84` was on the bus, and byte 1 (`00`) and byte 7 (`00`) said "not ventilating", which
  contradicts byte 0.
- Next test:
  1. Send `5D3 84`.
  2. As soon as the heater reports ventilation, send 5× 0x3E5 with byte 0 = `09` (b0 + b3,
     optionally + b7), byte 1 = `09`, byte 7 = `01`. Copy bytes 2–6 from the real frame.
     Space the frames 100 ms apart, between the heater's own 200 ms frames.

**Ventilation from the panel** (`panel_vent`): another ECU woke the bus (first frame 0x438).
The roof unit sent `80`, then `84` 0.5 s later. 0x3E5 showed ventilation 0.17 s after that,
with byte 0 still `00`. The panel showed **120 min**.

**Earlier results** (all in the `00` state, 3 frames):

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

**Power value (0x3E5 byte 4) to watts.** I measured the time between two 1 ml steps of the
fuel counter, using only intervals without a bus gap (`roof_panel_actions`,
`bit7_tests`). The fuel rate is proportional to the raw value:

| Power raw | Interval per 1 ml | Fuel rate | ml/h per step |
|---|---|---|---|
| 90 | 16.6 s | 217 ml/h | 2.41 |
| 100 | 14.8 s | 243 ml/h | 2.43 |
| 241 (full load) | 6.2 s | 580 ml/h | 2.41 |

- Fuel rate ≈ **2.41 ml/h × raw**. The relation is linear through zero.
- Diesel holds about 9.95 Wh/ml, so one step is about **24 W of fuel energy**. Raw 241 is
  about 5.8 kW fuel energy.
- Assuming the heater delivers its nominal 5 kW at 0.58–0.61 l/h, the heat output is about
  **20.7 W per step**: raw 241 ≈ 5.0 kW, raw 90 ≈ 1.9 kW. That puts the efficiency at about
  86 %.
- The forum's "1 W steps" (T5.1) does not fit, because that would cap the heater at 255 W.
- The fuel counter is the more direct measurement. The watt figure is an estimate that
  depends on the nominal rating.
- About 40 s pass from the command to ignition: priming, then glow.

Run times shown on the panel:

| Start | Run time |
|---|---|
| Heat via 0x5D3 | 30 min |
| Heat from the panel | 20 min |
| Ventilate from the panel | 120 min |

The run time is not in 0x5D3 (byte 1 always `00`). The roof unit applies its own value.

## Other IDs

- **0x3E1** (`20 00 10 00 xx …`, 100 ms): changes when the **vehicle blower** is operated.
  It is not heater related. 0x5E1 byte 0 bit 7 toggles together with it. With the ignition
  off it starts as `C0 …` / `E0 … FA` on each wake-up, see Bus behaviour.
- **0x665** byte 0: `00` → `80` on each wake-up, not mode specific.
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
- Dashboard: `venv/bin/python -m webasto_can_capture.dashboard --port /dev/ttyCANSNIFF --allow-tx`.
- Only one program can use the serial port at a time: the MCP server, SavvyCAN or the
  dashboard. The dashboard only works while the MCP server is disconnected.
- Once the serial link stalled right after a TX (`vent_manual`, 246.9 s, the capture ends
  there). A reset (RST) of the S2 fixed it.

## Implications for the receiver (TTGO, replacing W-Bus)

- The receiver needs an SN65HVD230 on free GPIOs (21/22 on the TTGO LoRa32). It taps the
  radio connector or the Komfort-CAN.
- Command algorithm:
  1. Wake the bus and read the roof unit's current 0x5D3 to get bit 7.
  2. Send `5D3 <mode | bit 7> 00` every 500 ms until the roof unit echoes that value.
  3. Stop after the echo, or after about 10 frames, and report failure. Check 0x3E5 too.
  Keep-alive and renewal as with W-Bus are not needed. The roof unit applies its own run
  time.
- Ventilation may need W-Bus or the panel, as long as `84` is rejected in the `8x` state.
- Status poll: send one 0x5D3 equal to the roof unit's current value (`wake_neutral` in
  `off_retry`: `80 00`). It wakes the bus without changing the mode; then read 0x3E5. The
  bus sleeps again after about 5 s. The value must be known from the last poll; a wrong
  bit 7 or mode briefly switches the heater (see above).
- Do not keep the bus awake permanently, because of the starter battery. Poll rarely.
- The transceiver TX pin must be recessive (HIGH) from power-up. A pull-up of about 10 kΩ
  from CTX to 3.3 V is recommended, so that a reset cannot wake or disturb the bus.

## Radio remote (Telestart / VW Funkfernbedienung): what we are emulating

Research from 2026-11-23. Forum and Webasto sources are listed at the end of this section.

- **The receiver is W-Bus only.** It has 3 wires: Kl.30, W-Bus to heater pin 1, and GND.
  - There is no CAN radio receiver on the T5, and so no receiver CAN ID to imitate.
  - On a factory WWZH the pin-1 wire (green/white) already exists and ends open at the 8-pin
    underbody connector, chamber 5.
- **The heater reports radio commands on CAN** in 0x3E5 byte 0. The VW signal names come from
  the mStHeizung_1 signal list (Golf V):

  | Bit | Signal | Meaning |
  |---|---|---|
  | 0 | SH1_Funk_ein | radio command on (heat or vent) received |
  | 1 | SH1_Funk_aus | radio command off received |
  | 2 | SH1_FunkSH_Ein | radio command "heat on" |
  | 3 | SH1_FunkStLftg | radio command "ventilate on" |
  | 4 | SH1_Clima_aus | A/C off |
  | 5 | SH1_SH_Modus | parking-heater mode |
  | 6 | SH1_Prog_Modus | programming |
  | 7 | SH1_Wake_Up_Komfort | wake Komfort-CAN |

  The radio bits are an impulse: 5 frames, then 0 again. Their exact timing, and whether b0 is
  combined with b2/b3, is not documented.
- **With the VW remote, the hand transmitter only gives an "on" impulse.**
  - The roof unit decides mode and run time, from its menu **FERNBED.** (default 30 min,
    up to 120 min).
  - The roof unit shows the remote start with its remaining time.
  - This fits our CAN start running 30 min.
- **Coding:**
  - Roof unit, address 6E: `000202` → `000203` makes the FERNBED. menu appear. Note that
    `0003xx` only enables the compass display.
  - Heater, address 18: 4th digit "Funkfernbedienung verbaut" (0/1). If it is coded to 1
    without a receiver, the heater logs DTC 02245 "Verbindung Funkfernbedienung – kein Signal".
  - Teaching remotes: address 18, adaptation channel 05.
  - Not yet checked whether our 7E7 906 453 C sits at 6E with this coding.
- **W-Bus addresses** (libwbus): Telestart = 2 (header `0x24`), timer 1533 = 3, diagnostics =
  F (`0xF4`, used by our receiver), heater = 4.
- **Ventilation via a remote works on a T5.2 factory WWZH** (Webasto T91 on pin 1). The forum
  statements "Lüften geht beim T5 nicht" refer to the retrofit Zuheizer kit, not to the
  factory WWZH.

**CAN-only emulation plan (speculative, to be tested):**
1. Code 6E to `…03`. In FERNBED., set mode = Lüften and a run time.
2. Use `radio_impulse('on')`. It sends 5× spoofed 0x3E5 with byte 0 = `81` (b0 + wake), bytes
   1–7 copied from the real frame, and no 0x5D3.
3. Check whether the roof unit then sends `84` by itself and shows the FERNBED. time.
4. Variants:
   - explicit bits `09` (b0 + b3, used by `set_heater_mode(radio_emulation=True)`) or `05` (heat)
   - `radio_impulse('on', vent_bits=True)`: byte 0 `89`, plus byte 1 b0 ("vehicle blower on", the
     request to the Climatic) and byte 7 b0 ("ventilation active"). The frame then looks like a
     heater that is really ventilating. The real heater sends these bits as 0 in between.
   - `radio_impulse('off')` with b1
   - 200 ms spacing vs a fast block
5. Keep heater coding 18 unchanged at first, to avoid DTC 02245.

**Pure W-Bus alternative.** It is documented to work for heat, vent and status, with the run
time taken from FERNBED. It needs the sender address 2 instead of F, codings 18 and 6E, and
possibly answering the heater's polling of the receiver (unknown). Sniffing a real VW receiver
(7N0/1K0/1N0 963 513) would settle this.

Sources:
- https://tx-board.de/threads/funkmodul-webasto-standheizung.133759/page-2
- https://tx-board.de/threads/wwzh-fernbedienung-anlernen-klappt-nicht.143424/page-2
- https://tx-board.de/threads/t91-telestart-nachgeruestet-heizt-aber-nur-30min.124874
- https://tx-board.de/threads/codierung-dachuhr-wwzh-mit-fernbedienung-ploetzlich-kompassanzeige.99655
- https://www.canhack.de/viewtopic.php?t=199
- https://www.caliboard.de/topic/4079-wwzh-webasto-kit-kann-can-nicht-wecken
- https://raw.githubusercontent.com/futuranet/libwbus/master/wbus/wbus_const.h
- Webasto 9012106D (T5 TT-C kit) and 9030705C (Telestart T91/T100)

## Next steps in the car

Order of work; each block ends with a decision or a result for this document.

**0. Preparation**
- Keep the laptop awake: `systemd-inhibit --what=sleep:idle --why="CAN" sleep infinity`.
- Sniffer at the radio Quadlock (A9 CAN-H, A10 CAN-L, GND, no terminator), firmware
  `cansniffer_tx`. Restart Claude Code so the MCP server is loaded.
- Bring VCDS, a multimeter, a thermometer; note odometer, fuel level and outside temperature
  from the cluster for every block.

**1. Heater: CAN or W-Bus (decides the transport)**
1. VCDS: read and write down the coding of control unit 18 and of the roof unit (6E); read the
   fault memory of 18 as the starting point. Do **not** change 18 yet (DTC 02245).
2. Roof unit 6E: `…02` to `…03`, then set mode "Lüften" and a run time under FERNBED.
3. Test 1, `radio_impulse("on")`: does the roof unit send `84` by itself and show the run time?
4. Test 2, `radio_impulse("on", vent_bits=true)`.
5. Test 3, `set_heater_mode("vent", radio_emulation=true)`.
6. After each test switch off (`set_heater_mode("off")`), wait 30 s. Ask the user before every
   transmission; spoofing 0x3E5 needs `enable_status_spoof`.
7. Read the fault memory of 18 again and compare.
8. Decision: vent works over CAN, so pure CAN; otherwise choose between CAN without ventilation
   and pure W-Bus (sender address 2, coding 18 "Funk verbaut").

**2. Heater: open measurements**
- Meaning of 0x5D3 bit 7 and how the roof unit gets back to `00` (ignition on/off, panel menu).
- Long-sleep test: after at least 30 min (better overnight), one `set_heater_mode("heat")` frame
  must wake the bus. Then switch off.
- Status poll without side effects: repeat the roof unit's current 0x5D3 value as a wake frame
  and read 0x3E5 for about 1 s.

**3. Drive test on the Komfort-CAN** (protocol in `can-t5-vehicle-signals.md`)
- Ignition off, on, off; voltage against the multimeter (0x571); engine start and 2000 rpm.
- Doors, lights, turn signals, reverse gear (0x470).
- Steady 30, 50 and 80 km/h against GPS: find speed (0x351/0x320) and RPM (0x353).
- Coasting: lift off in gear, declutch, brake; mark every step with `mark_event`.
- Odometer 0x65D is confirmed (296 314 km), outside temperature 0x527 still needs a reference.
- Decide the overrun detection: rule from speed/RPM/deceleration, or a second sniffer on the
  Antrieb-CAN (listen-only).

**4. Board battery**
- Photo and type of the separation relay, its control wire (D+ or terminal 75X) and fuse.
- Record 0x571 together with the multimeter and a clamp meter on the board battery line, and
  check whether 0x570, 0x578 or 0x392 ever show up.

**5. Firmware, once the transport is decided**
- Flash the receiver (`env:receiver`, CAN) and the sender, check the stack watermarks and the
  free heap with BLE, WiFi, MQTT and LoRa running together, and check that LoRa timing does not
  shift.
- The receiver uses 99.6 % of the OTA slot (1 305 177 of 1 310 720 bytes). Before adding
  features, switch the partition table to `min_spiffs.csv` (1.9 MB per slot) or shrink the image.
- Test BLE pairing with a phone, the automatic reconnect of the PWA (Chrome on Android) and the
  PWA against both roles (`Webasto-RX-XXXX`, `Webasto-TX-XXXX`).
- Fit the interior sensor (DS18B20) and test the climate control with the heater.
- Fail-safe relay for charging in overrun (NC contact, minimum on/off time, engine qualifier).

## Open

1. **0x5D3 bit 7:** what it means, what set it, and how to get back to `00`. Candidates:
   ignition on/off, the panel menu, a long sleep.
2. **Ventilate in the `8x` state:** why the roof unit rejects `84` from CAN while it
   accepts `82` and `80`.
3. **Off not adopted once** (`bit7_tests`, `tx_off_bit7_10x`, 10× `80` during full load);
   72 s later the same command worked. Cause unknown.
4. **Long sleep:** does one frame still wake the bus after hours or overnight? The gateway may
   sleep deeper. The test is planned; keep the laptop from suspending
   (`systemd-inhibit --what=sleep:idle sleep infinity`).
5. **Run time per mode and timer:** where the roof unit takes 30/20/120 min from, and whether
   they can be changed in the panel menu. Not in 0x5D3.
6. **Fault memory**, address 18 (Standheizung): read it with VCDS after the tests.
7. Who sends 0x5D3: roof unit or J533? This matters only if the sender behaves differently
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
  - Spoofing 0x3E5 collides with the real heater (a 5× test had no visible effect, see 0x3E5).
  - Never add a terminator.
