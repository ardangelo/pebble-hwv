# Cyberdeck EVT3 nRF hardware-test coverage

## Scope and evidence

This is a command/fixture inventory, **not** a production acceptance record. It
covers the Q20 + 83_OFN EVT3 nRF assembly only. K230, its DDR/storage/USB host,
and the separate AIC8800D radio are outside nRF HWV. Asterix-only magnetometer,
pressure, GPIO buttons and nPM1300 tests remain in their own board build.

The earlier EVT3 bench report recorded partial physical exercise of I²C probes,
flash, sensors, display, selected keyboard/OFN inputs, haptic, lighting, BLE,
LFXO, IMU INT1, speaker, peripheral power cycling and repeated DA7212 MIC1
captures. That report does **not** verify the changes in this branch. Full
keyboard coverage, headphone channels, OFN physical click, battery ADC vs DMM,
TP4054 charge, rail measurements and source switchover need a new documented
fixture/device run. No electrical or acoustic production limit is certified
here. Record unit identity, build/image version, fixture setup, raw readings,
operator observations and failures separately.

Hardware reference: `/store/git/cyberdeck-firmware/cyberdeck-evt3.pdf`, sheets
5–7 (EVT3 schematic). The local `cyberdeck-kicad` PCB is an older revision,
not fitted-EVT3 pin/divider authority. Implementation in this repository is
`boards/coredevices/cyberdeck_evt3/`, `app/src/`, and `scripts/wavgen.py`.
Other firmware references below use paths under `/store/git/cyberdeck-firmware/`,
not this repository's `src/fw/` (which does not exist).

## Device and operator checks

| Subsystem | HWV path | External pass evidence still needed |
| --- | --- | --- |
| Boot | HWV version, initialization diagnostics, USB CDC shell prompt | Confirm correct image/version and no failed module initialization on the actual unit. |
| Q20 keys / EC | `hwv cyberdeck keyboard status`, `keyboard check [seconds]`, `ec [seconds]` | Every fitted key press **and release** plus EC, no stuck or missing keys; verify physical mapping on Q20. |
| Optical navigation | `hwv cyberdeck ofn id`, `motion [seconds]`, `click [seconds]` | Product ID 0x30; each X/Y direction and physical click; report I²C errors. |
| Display | `hwv display on/off`, `white`, `black`, `checker`, `border`, `vpattern`, `hpattern`, `brightness N` | Inspect 400×240 full-panel address coverage, missing lines, mux ownership and backlight changes. |
| Lighting and haptic | `hwv cyberdeck rgb red/green/blue/white/off`, `backlight N`; `hwv haptic configure N` | Verify each LED color/off, Q20 light/off and actual vibration/stop. |
| Audio | `hwv speaker play`, `headphone`, `codec`; `hwv mic capture 1` and `scripts/wavgen.py` | Listen to speaker and separately fitted headphone L/R; verify 16-kHz one-second MIC1 WAV against an applied acoustic signal and review RMS/DC/clipping. |
| Sensors | `hwv light get`, `imu get`, `imu interrupt [seconds]`; `hwv cyberdeck probe` | ALS changes under cover/light; IMU axes respond to rotation, INT1 arrives; devices report expected IDs. |
| nRF BLE | `hwv ble on/off` and the GATT write characteristic | Specified BLE peer connects, writes 0/1, disconnects; check antenna/range using an agreed setup (no RSSI threshold established). |
| USB | `hwv cyberdeck usb TOKEN`, `reset` | Observe hub plus `Cyberdeck EVT3 HWV` CDC, echo varied tokens bidirectionally, disconnect/reconnect and confirm fresh boot banner. |
| Flash | `hwv flash id`, `erase`, `write`, `read`, `stress N`, `erase_all` | Identify `ef 60 19`; verify written data and stress. Erase command waits for driver completion and `erase_all` spot-checks three locations, **not** a complete byte-for-byte chip scan. Destructive commands are HWV-only before production images. |
| LFXO | `hwv lfxo test` | Compare LF crystal kernel ticks to HFXO timer over ten seconds; record signed ppb, evaluate absolute ppm against an agreed spec. Defconfig selects 20-ppm LF source. |

A shell command's success proves its software checks, not the visible/audible
physical acceptance in the third column. Timeout/transport errors must be
recorded as failures, not interpreted as an operator pass. `README.md` lists
syntax and both-board microphone units (whole seconds; default/max one).

## Battery and guided electrical checks

EVT3 schematic sheet 5 places `VBAT_VOL` on nRF P0.31/AIN7. Sheet 7 gives
R76=300 kΩ from VBAT and R77=100 kΩ to ground: `VBAT_VOL = VBAT / 4`.
`hwv charger status` now samples this SAADC input and reports battery millivolts;
USB VBUS detection comes from the nRF USB regulator. Compare to a DMM across
battery/GND on the **fitted** board and log discrepancies. Neither sample
measures battery current, temperature, charge state, capacity or actual TP4054
charge completion. No charge current, maximum voltage, percentage, or thermal
pass criterion is defined by this software.

`hwv cyberdeck fixture` prints the guided checks; the operator must have a DMM,
a current measurement method appropriate to the fixture, a controlled USB
source and suitable battery. The schematic identifies:

- Battery: connector J8 `BAT+`/`GND`, VBAT (TP10), charger TP4054 U22,
  yellow CHRG# LED3, USB VBUS. Check charge indication and termination with
  external current observation, not an nRF telemetry field.
- Source path: `VSYS` TP13 and ground TP14. Observe USB-only, battery-only,
  then controlled switchover; record continuity and the actual measured voltage
  under each supply. Do not remove a battery while charging unless the fixture
  is designed for that sequence.
- nRF rails: TP5 (`NRF_5V`), TP8 (`NRF_SYS`), TP7 (`NRF_1V8`), TP12
  (`NRF_3V3`) against ground. Check unloaded and under peripheral activity;
  schematic rail names are nominal, not acceptance tolerances. Set allowable
  bands with the electrical owner and fitted assemblies.
- SWD: sheet 5 J2, GND/NRF_1V8/SWDIO/SWCLK/NRF_RST; program, halt, read and
  reset via the configured SWD runner, then recheck the USB CDC console.

Factory-specific limits, connector pin numbering, acceptable charge time and
fixture wiring are still needed before declaring jig/electrical parity.
Do not substitute a fabricated threshold or a software PASS for a DMM reading.
