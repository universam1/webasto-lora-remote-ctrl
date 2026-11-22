# VW T5.2 Webasto WWZH on CAN: research and findings

Status: research only, **nothing verified on our vehicle yet** (T5.2, built 2013,
factory WWZH, roof control unit 7E7906453C). Update this file after each capture session.

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
(fill in: date, tap point, bitrate, scan result, actions, findings)
