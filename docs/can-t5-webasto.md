# VW T5.2 Webasto WWZH on CAN: research and findings

Status: first capture session done 2026-11-22 (T5.2, built 2013, factory WWZH,
roof control unit 7E7906453C). Command candidate 0x5D3 found, not yet tested by sending.

## Bus
- Heater on **Komfort-CAN** (wire colour orange/green = CAN-H, orange/brown = CAN-L).
  Heater connector T6au: pin 1 W-Bus, pin 2 CAN-H, pin 3 CAN-L.
  Source: https://tx-board.de/threads/can-bus-zuheizer.55297/
- Bitrate presumably 100 kbit/s, likely fault-tolerant low-speed CAN (ISO 11898-3).
  An SN65HVD230 may or may not receive it reliably; fallback transceiver TJA1055.
  Recessive levels on FT-CAN: CAN-H ≈ 0 V, CAN-L ≈ 5 V (measure with a multimeter first).
- Radio Quadlock: A9 = CAN-H, A10 = CAN-L (probably Infotainment-CAN behind gateway J533;
  whether J533 forwards heater frames is unknown).
  http://www.passatplus.de/umbauteile/stecker/stecker4.htm
- Alternative tap: Komfort-CAN at the A-pillar connector station, blue 10-pin, pins 4/5,
  or the KI↔BCM line.
- T5.1 heaters use "CAN 1.6", T5.2 "CAN 2.0"; frames may differ from T5.1 reports.

## Known frames (T5.1 reports, unverified on T5.2)
### 0x3E5, heater status, every 200 ms + on change
Source: https://tx-board.de/threads/frame-fuer-standheizung.36723/

| Byte | Bits |
|---|---|
| 1 | b0 on via radio, b1 off via radio, b2 heating on, b3 ventilation on, b4 A/C off, b5 heater mode activating, b6 programming, b7 wake Komfort-CAN |
| 2 | b0 vehicle blower, b1 control LED, b2 combustion on, b3 water pump request, b4-6 error (0 none, 1 battery low, 2 low fuel, 3 defective, 4 crash, 5 other, 6 overheat), b7 error stored |
| 3-4 | fuel used, 1 ml steps; byte 4 b6 parking heater (runs without Kl.15), b7 Kl.30 lost |
| 5 | heating power |
| 6 | coolant temp = −48 + 0.75·x °C, 0xFF error |
| 7 | b0 fuel pump on, b1 water warmed up |
| 8 | b0 ventilation active |

Observed: `60 80 EE 86 27 00 00 00`, `E0 00 4E C1 27`.

### 0x3E1, unknown
About five frames `80 FE xx` around on/off (xx e.g. 00, 0E, 07, 14, 16). Possibly the roof unit.

### opendbc vw_pq.dbc
`Klima_1` 0x5E0 bit 1 `Fahrerwunsch_Zuheizer`; `Verbauliste_1` 0x5D8 bit 39 `Standheizung`.

## Control paths
- No public CAN start/stop command found. All known retrofits use W-Bus on pin 1.
- Diagnostics: address 0x18 (Standheizung), TP2.0 channel setup on 0x200, reply 0x218,
  output tests via KWP2000. Starting via diagnostics may wake the whole bus.

## Risks
- Keeping Komfort-CAN awake drains the starter battery.
- Missing/odd CAN signals → heater faults 49/53, heater lock (needs unlocking via VCDS, adaptation channel 42).
- Spoofing 0x3E5/0x3E1 collides with the real heater and roof unit.
- Never add a terminator; sniff listen-only.

## Capture log

### 2026-11-22, radio Quadlock A9/A10, 100 kbit/s
Hardware: LOLIN ESP32-S2 mini + SN65HVD230 (no Rs), listen-only.
- Bus: about 330 frames/s, 56 IDs, 0 missed, 1 bus error in total. SN65HVD230 reads it fine.
- Captures (local, gitignored): `mcp-can-capture/captures/baseline_radio_awake.*`,
  `roof_panel_actions.*` (504 s, markers heat_on, heat_off, heat_on_2, heat_off_2, vent_on, vent_off).

**0x3E5 layout confirmed** for the T5.2 (byte 1 = control LED/combustion/water pump,
byte 4 = power, byte 5 = coolant −48 + 0.75·x °C, byte 6 b0 fuel pump, byte 7 b0 ventilation).
Idle example `00 00 00 40 00 4C 00 00` (9 °C).

**0x5D3, command candidate** (DLC 2, cyclic 500 ms, sender unknown):

| Event (roof panel) | 0x5D3 | 0x3E5 reaction | 0x3E3 byte 4 |
|---|---|---|---|
| heat off (104.3 s, 411.6 s) | `02 00` → `00 00` | +87 / +180 ms: LED off, power 0 | → `00` |
| heat on (294.7 s) | `00 00` → `02 00` | +178 ms: LED on, fuel pump on | → `01` |
| vent on (450.3 s) | `00 00` → `04 00` | +13 ms: byte 1 `09`, byte 7 `01` (ventilation) | → `01` |
| vent off (484.1 s) | `04 00` → `00 00` | +82 ms: ventilation off | → `00` |

**Repeat run** (`roof_panel_actions_2`, 156 s, clean sequence with markers before each press):

| Event | 0x5D3 | 0x3E5 reaction |
|---|---|---|
| heat on (25.9 s) | `00 00` → `02 00` | +67 ms |
| heat off (82.9 s) | `02 00` → `00 00` | +178 ms |
| vent on (114.6 s) | `00 00` → `04 00` | +59 ms |
| vent off (142.8 s) | `04 00` → `00 00` | +65 ms |

Both runs agree: 8 of 8 transitions. No other ID changes with these events.

Hypothesis: 0x5D3 byte 0 = requested mode (0x00 off, 0x02 heat, 0x04 vent), sent by the
roof control unit (or forwarded by J533). 0x3E3 byte 4 looks like a "heater active" echo.
Open: who sends 0x5D3, run time/timer encoding, whether a foreign sender is accepted while
the original keeps sending `00 00`.

Other IDs seen: 0x3E1 (`20 00 10 00 xx …`) changes when the vehicle blower is operated,
not heater related. 0x5E1 byte 0 bit 7 changes together with 0x3E1.
