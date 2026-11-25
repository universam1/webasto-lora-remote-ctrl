# VW T5.2 Komfort-CAN: vehicle signals (climate control, trip log, overrun charging)

Status 2026-11-25: research plus analysis of the existing captures. **Nothing was verified
while driving yet.** The heater frames (0x3E5, 0x5D3, 0x3E3) are documented in
`can-t5-webasto.md`.

## Setup facts

- Our sniffer sits at the radio Quadlock (A9 CAN-H, A10 CAN-L), Komfort-CAN, 100 kbit/s.
- The Android head unit decodes speed, RPM, odometer and fuel through a SimpleSoft CAN box on
  the **same line**; the user taps the box connector. So these values travel on this bus,
  at least in the right vehicle state.
- Our captures so far:

  | State | Captures |
  |---|---|
  | ignition on (`0x575` d0 = `87`), engine off | `baseline_radio_awake`, `roof_panel_actions*`, `tx_test_heat` |
  | ignition off, bus awake | `ign_off_sleep`, `heat_2min`, `vent_manual`, `bit7_tests`, `off_retry`, `panel_vent` |

  The engine never ran and the car never moved. Speed and RPM are constant 0 there, so they
  cannot be found in these captures. IDs sent only with a running engine or while driving
  are missing for the same reason. **The absence of 0x280, 0x1A0, 0x320 and 0x353 is
  therefore not evidence that the data is not on the bus.**
- The PQ DBC (opendbc `vw_pq.dbc`) is for the Antrieb-CAN and Kombi messages. Almost none of
  our 56 IDs are in it. For the Komfort-CAN we rely on reverse-engineering notes from other
  VW models (Touran, Polo 9N, Passat B6/B7, Audi, e-Up). **Nothing exists for the T5.2.**

## Signals

| Signal | ID | Layout (byte 0 = d0) | Status | Evidence |
|---|---|---|---|---|
| Ignition (terminal 15) | 0x575 | d0 `87` on, `00` off | **confirmed** | 92 frames `87 20 00 00` vs 270 frames `00 20 00 00`. Also `0x470` d0 `C0` → `40`. |
| Engine coolant temperature | 0x35B | d3 − 64 °C | **confirmed** | d3 = 73 → 9.0 °C equals the heater coolant (9.00 °C) at the cold start and follows it with a lag (15 °C vs 34 °C). Formula from the Macchina Touran notes. |
| Board voltage | 0x571 | d0 × 0.05 + 5 V | **confirmed (user)** | 12.25–13.25 V in our captures, matches the user's readings. Dip of 0.2 V right after a heater start. The rise to 13.25 V in the evening came from the user connecting the board battery to support the starter battery. |
| Outside temperature | 0x527 | d5 / 2 − 50 °C | plausible (user) | d5 = `75` gives 8.5 °C, falling to 8.0 °C in the evening. Source: OVMS e-Up. Not yet compared with a reference reading. Alternative from Audi: `0x351` d5/d6, value / 2 − 40. |
| Odometer | 0x65D | km = ((d3 & 0x0F) << 16) \| (d2 << 8) \| d1 | **confirmed (user)** | Our d1..d3 = `7A 85 F4` gives **296 314 km**, the user confirmed this reading. The 16-bit variant from OVMS (50 554 km) is wrong for this car. |
| Vehicle speed | 0x351 / 0x320 | (d2·256 + d1 − 1) / 190 km/h (0x351, Touran, Audi); e-Up uses 0x320 | not seen yet | Needs a drive. |
| Engine speed | 0x353 | d1/d2 swapped, value / 4 = rpm (Audi) | not seen yet | Needs a running engine. |
| Doors, bonnet, tailgate | 0x470 | d1 bits 1, 2, 4, 8, 0x10, 0x20 | not tested | d1 was always 0, because no door was opened. |
| Lights, turn signals | 0x470 d0, d2, d3 | see notes of the analysis | not tested | `0x470` is `BSG_Kombi` in the DBC. |
| Interior temperature | none found | – | **not on the bus (likely)** | `0x35B` was first suspected, but is the coolant. The Climatronic evaluates its sensor locally (diagnostic measuring block only). The e-Up formula for `0x3E3` does not apply (`0x3E3` d2 is 0 here). |
| Fuel level | none found | – | not confirmed | Only found in Antrieb-CAN logs. |
| Overrun (coasting) | none | – | not on this bus | See "Overrun". |
| Second battery (voltage, current) | none | – | not on the bus | DBC `BatMan_1` (0x578) has relay states only. |

Other IDs identified in our data:

| ID | Meaning |
|---|---|
| 0x65F | VIN, ASCII `WV1ZZZ7HZD…`, three frames in rotation |
| 0x400, 0x40C, 0x427, 0x438, 0x43A | OSEK network management (ID = 0x400 + node number); this is the sleep/wake coordination |
| 0x62C, 0x63B, 0x66F, 0x6D2, 0x6DA | cyclic index/identification tables, unchanged for hours |
| 0x62B, 0x62D | byte 4 counts +1 per minute while the ignition is on |

## Design decisions that follow

- **Interior temperature:** use our own sensor on the ESP32 (DS18B20, SHT3x or BME280),
  mounted away from the sun and the heater outlet.
- **Charging decision:** measure the board battery voltage with the ESP32 ADC at the pole
  (not at a distribution point). The CAN voltage has only 0.05 V resolution, from the same
  source as the car.
- **Overrun:** the engine messages (Motor_1 `0x280`: pedal, idle flag, driver torque) live on
  the Antrieb-CAN (500 kbit/s). There is no fuel-cut flag in the DBC; coasting has to be
  derived (pedal 0 %, idle flag set, RPM above idle, speed above 0). If speed and RPM show up
  on the Komfort-CAN during the drive test, "speed > 0, RPM above idle and no accelerator
  input" may be derived without the Antrieb-CAN. A T5.2 probably has no recuperation
  signalling; this is not proven.
- **Trip log:** speed and odometer from the Komfort-CAN if the test finds them. Otherwise
  GPS.
- Tapping the Antrieb-CAN is the last resort: 500 kbit/s, airbag-relevant, and a bad tap can
  disturb the bus. Forum hint (unverified): cluster connector T32a pins 19/20, Komfort 8/9,
  Infotainment 15/16.

## Protocol for the next session (drive test)

Write down, for every step: time, outside temperature (cluster), odometer, fuel level,
voltage at the 12 V socket (multimeter), coolant gauge, interior temperature (thermometer at
head height). Set a `mark_event` before every action.

| # | Action | Look at |
|---|---|---|
| 1 | Ignition off → on → off, 30 s each | `0x575`, `0x470` d0; new or vanishing IDs |
| 2 | Voltage: multimeter before ignition on, with ignition on, at engine start | `0x571` d0 against 0.05 · d0 + 5 |
| 3 | Start the engine, idle for 2 min, then hold 2000 rpm | a 16-bit value of about 800–850 · 4 at idle (`0x353`); `0x571` up to 13.8–14.4 V (d0 176–188); `0x35B` d3 |
| 4 | Doors one by one, 5 s each, then bonnet and tailgate | `0x470` d1 |
| 5 | Lights, turn signals, hazard lights, reverse gear | `0x470` d0, d2, d3 |
| 6 | Climatic: turn the temperature knob in 5 steps, then blower 0 to 4 | `0x3E1`, `0x5E1`, `0x58C` |
| 7 | Drive at a steady 30, 50 and 80 km/h, with GPS speed as reference | a 16-bit value with (hi·256 + lo − 1) / 190, in `0x351`, `0x320` or any new ID |
| 8 | Coast: lift off the accelerator in gear, then declutch, then brake | which bits flip exactly when the pedal is released |
| 9 | Compare the odometer | `0x65D` d1..d3, 50554 or 296314 against the cluster |

To decide afterwards:

| Result | Next step |
|---|---|
| Speed and RPM are on the Komfort-CAN | no second tap is needed |
| Only speed is there | RPM from the Antrieb-CAN, or an alternative |
| Neither | GPS for the trip log; Antrieb-CAN tap only if overrun charging is still wanted |

## Sources

- https://forum.macchina.cc/t/some-vw-can-stuff-to-help-out-people-with-mk5-and-some-mk6-type-platform-vehicles/660
- https://docs.openvehicles.com/en/latest/components/vehicle_vweup/docs/index_t26.html
- https://www.hackster.io/databus100/digital-speedometer-to-car-s-instrument-cluster-via-can-bus-66e273
- https://raw.githubusercontent.com/commaai/opendbc/master/opendbc/dbc/vw_pq.dbc
- https://tx-board.de/threads/can-bus-im-volkswagen.20100
- https://wiki.pq35.de/General_Info/Can_Bus
