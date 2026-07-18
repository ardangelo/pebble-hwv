# Cyberdeck EVT3 nRF hardware-test coverage

## Scope

This report compares the interactive HWV tests in this repository with the nRF52840-controlled Cyberdeck EVT3 hardware and its confirmed firmware board definitions.

The scope is intentionally narrow:

- **nRF52840-side tests only.** K230, LPDDR4, microSD, AIC8800D Wi-Fi/Bluetooth, K230 USB host/debug, and other K230-side diagnostics are excluded.
- **Q20 keyboard with 83_OFN optical navigation only.** Q20 OEM and BB9900 keyboard variants are excluded.
- **The external W25Q256JW flash is disposable while HWV is running.** Destructive erase/write/stress and microphone scratch use do not need partition guards in this test firmware.
- Tests are interactive `hwv ...` shell commands unless stated otherwise. Most require a human or fixture to confirm the physical result.

Sources reviewed:

- `/store/git/cyberdeck-firmware/cyberdeck-evt3.pdf` (all seven sheets)
- `app/src/*.c`, `app/prj.conf`, and `README.md` in this repository
- HWV-local `/store/git/pebble-hwv/pebble-hwv/boards/coredevices/asterix/` and `/store/git/pebble-hwv/pebble-hwv/boards/coredevices/cyberdeck_evt3/`
- `/store/git/cyberdeck-firmware/src/fw/board/boards/board_cyberdeck_evt3.{c,h}`
- Cyberdeck firmware drivers named in the tables below

Unless an absolute path is shown, `src/fw/...`, `boards/...`, and `pblboot/...` references are relative to `/store/git/cyberdeck-firmware/`.

## Current HWV implementation status

The `cyberdeck_evt3` board now has standalone DTS and pinctrl definitions based on `board_cyberdeck_evt3.{c,h}`. Asterix-only sources are excluded, the shell runs over USB CDC, and the implemented commands cover the applicable automated and guided tests described below. No physical flashing has been performed.

## Confirmed EVT3 nRF pin assignments

The `cyberdeck-firmware` EVT3 board definitions are the pinout authority for this report. The earlier discrepancy list was incorrect because the schematic pin labels were read out of alignment. HWV DTS and pinctrl work should reproduce these firmware assignments:

| Function | Confirmed `cyberdeck-firmware` assignment | Source |
| --- | --- | --- |
| Shared I2C | SCL P0.11, SDA P0.12 | `src/fw/board/boards/board_cyberdeck_evt3.c` |
| DA7212 I2S | BCLK P1.09, WCLK P0.08, MCLK P0.06, nRF SDOUT/codec DATIN P1.08, nRF SDIN/codec DATOUT P0.07 | `src/fw/board/boards/board_cyberdeck_evt3.c` |
| nRF display | SCLK P0.05, MOSI P0.04, CS P0.27, on-control P1.10, EXTCOMIN P1.04 | `src/fw/board/boards/board_cyberdeck_evt3.h` |
| Display selection/backlight | `LCD_SEL` P1.05, `LCD_BKL` P1.06 | `src/fw/board/boards/board_cyberdeck_evt3.h` |
| LSM6DSO interrupt | INT1 P0.10 | `src/fw/board/boards/board_cyberdeck_evt3.c` |
| QSPI flash | CS P0.20, CLK P0.19, IO0 P0.17, IO1 P0.22, IO2 P0.23, IO3 P0.21 | `src/fw/board/boards/board_cyberdeck_evt3.c` |
| Other controls | Keyboard backlight P1.15, EC key P0.03, `LRA_EN` P1.07 | `src/fw/board/boards/board_cyberdeck_evt3.h` |

# 1. Present and directly applicable

These existing HWV tests exercise hardware present on Cyberdeck and should be retained after porting.

| Existing HWV test | Required Cyberdeck changes | `cyberdeck-firmware` implementation details to source | Expected result |
| --- | --- | --- | --- |
| `hwv display on`, `off`, `vpattern`, `hpattern`, `brightness N` (`app/src/display.c`) | Replace the Asterix LS013B7DH05 144x168 description with LS013B7DH01 400x240; use the confirmed firmware assignments SCLK P0.05, MOSI P0.04, CS P0.27, on-control P1.10, EXTCOMIN P1.04, `LCD_SEL` P1.05, and `LCD_BKL` P1.06. Fix the existing `<= width/height` pattern loops and return command failure when display API calls fail. | `src/fw/drivers/display/sharp_ls013b7dh01/sharp_ls013b7dh01_nrf5.c` for LSB-first SPI framing, row addressing, CS delays, and RTC/GPIOTE EXTCOMIN; `src/fw/board/displays/display_cyberdeck_evt3.h` for 400x240 geometry; `src/fw/drivers/backlight/pwm.c` for PWM behavior; `src/fw/board/boards/board_cyberdeck_evt3.h` for pins. | Full-panel patterns render without corruption or out-of-bounds writes; on/off and brightness are visible; driver errors produce nonzero status. |
| `hwv flash id`, `read`, `write`, `erase`, `stress` (`app/src/flash.c`) | Replace GD25LB255E data with W25Q256JW; use the verified QSPI mapping CS P0.20, CLK P0.19, IO0 P0.17, IO1 P0.22, IO2 P0.23, IO3 P0.21; allow destructive access to the whole chip under the HWV assumption. | `src/fw/drivers/flash/w25q256jw.c` for commands, JEDEC ID `ef 60 19`, 32 MiB size, EN4B `0xb7`, QER S2B1v6, erase timings, and low-power behavior; `src/fw/drivers/nrf5/qspi.c` and `board_cyberdeck_evt3.c` for nRF QSPI setup. | Correct ID; erase/write/read matches; random full-chip stress iterations pass. |
| `hwv haptic configure N` (`app/src/haptic.c`) | Use shared I2C address 0x5a and firmware-defined `LRA_EN` P1.07; tune voltage/clamp values for the fitted actuator. | `src/fw/drivers/vibe/vibe_drv2604.c` for STATUS probing, initialization table, RTP strength control, standby, register dump, and autocalibration. | LRA starts/stops reliably and strength changes with the requested value. |
| `hwv light get` (`app/src/light.c`) | Use shared I2C address 0x44 and polling; add manufacturer/device-ID checks. | `src/fw/drivers/ambient/ambient_light_opt3001.c` for IDs 0x5449/0x3001, continuous/single-shot setup, and result conversion. | Reading changes plausibly when covered and illuminated. |
| `hwv imu get` (`app/src/imu.c`) | Use shared I2C address 0x6a, Cyberdeck orientation, and correct power setup. Keep polling separate from the interrupt test below. | `src/fw/drivers/imu/lsm6dso/lsm6dso.c` for WHO_AM_I 0x6c, reset, range/ODR setup, axis mapping, FIFO, and interrupt behavior. | Accelerometer axes respond correctly to board rotation and gyro values respond to motion. |
| `hwv speaker play` (`app/src/speaker.c`) | Use DA7212 at 0x1a and the confirmed firmware I2S mapping: BCLK P1.09, WCLK P0.08, MCLK P0.06, nRF SDOUT P1.08, and nRF SDIN P0.07. Review route, gain, PLL, and sample rate. | `src/fw/drivers/speaker/nrf5/da7212.c` for checked register I/O, PLL/DAI setup, line/headphone routing, volume mapping, diagnostics, and nRF I2S streaming; `src/fw/board/boards/board_cyberdeck_evt3.c` for pins. | Known sample plays clearly through the speaker without clipping or I2S errors. |
| `hwv lfxo test` (`app/src/lfxo.c`) | Keep timer comparison and define a manufacturing ppm limit for the nRF 32.768-kHz crystal. | `board_cyberdeck_evt3.c::board_early_init()` for LFCLK crystal startup. That implementation currently has no timeout, so HWV should retain bounded failure reporting. | Measurement completes within a timeout and error is within the chosen ppm limit. |
| `hwv ble on` / `off` plus GATT write (`app/src/ble.c`) | Retain for the nRF52840 radio only; add an RSSI/range limit if antenna assembly faults must be detected. | `boards/cyberdeck_evt3/defconfig` confirms the nRF platform configuration; use the existing HWV Zephyr BLE implementation rather than K230/AIC8800D code. | Device advertises, connects, accepts a GATT write, disconnects, and disables cleanly. |

# 2. Not applicable and should be removed or replaced

| Existing HWV test/module | Reason | Action for Cyberdeck HWV |
| --- | --- | --- |
| `hwv press get` / `app/src/press.c` | No BMP390 exists on Cyberdeck EVT3. | Exclude source, init, alias, and configuration from the Cyberdeck build. |
| `hwv mag get` / `app/src/mag.c` | No MMC5603 exists on Cyberdeck EVT3. | Exclude from the Cyberdeck build. |
| `hwv charger status` / `app/src/charger.c` | It is nPM1300-specific. Cyberdeck uses TP4054 plus discrete power-path circuitry and `VBAT_VOL`. | Remove and replace with battery ADC and guided charge/power tests. |
| `hwv mic capture` / `app/src/mic.c` | It uses the Asterix nRF PDM microphone. Cyberdeck microphone input is through DA7212 and I2S. | Replace with the I2S microphone test below. Flash may be freely erased for capture scratch data in HWV. |
| `hwv buttons check` / `app/src/buttons.c` | It assumes four independent Asterix GPIO buttons. The scoped input is the Q20 TCA8418 keyboard with 83_OFN, plus the dedicated EC/end key. | Replace with Q20 matrix, OFN, and EC-key tests. Do not add Q20 OEM or BB9900 mappings. |

`app/src/main.c` and `app/CMakeLists.txt` should select modules by board so absent Asterix hardware is neither compiled nor initialized on Cyberdeck.

# 3. Tests yet to be implemented

## 3.1 Core nRF peripheral tests

| New/modified test | Implementation and pass criteria | `cyberdeck-firmware` source details to reuse | Priority |
| --- | --- | --- | --- |
| Expected-device probe | Probe DRV2604 0x5a, OPT3001 0x44, DA7212 0x1a, LSM6DSO 0x6a, TCA8418 0x34, and OFN83 0x33. Read identity/status registers where stable and report each device separately. | Address and confirmed SCL P0.11/SDA P0.12 composition in `src/fw/board/boards/board_cyberdeck_evt3.c`; identity/probe sequences in `src/fw/drivers/vibe/vibe_drv2604.c`, `src/fw/drivers/ambient/ambient_light_opt3001.c`, `src/fw/drivers/imu/lsm6dso/lsm6dso.c`, `src/fw/drivers/tca8418.c`, and `src/fw/drivers/a320.c`. | P0 |
| RGB LED | Add red, green, blue, white, and off commands; human confirms color order and no stuck channel. | `src/fw/drivers/neopixel.c` implements WS2812B GRB packing and PWM waveform output on firmware-assigned P0.16, plus `command_led_on/off`. Port the logic to Zephyr PWM. | P0 |
| IMU INT1 | Configure data-ready or motion interrupt on firmware-assigned P0.10 and require events after guided motion. | Interrupt/FIFO behavior in `src/fw/drivers/imu/lsm6dso/lsm6dso.c`; pin and GPIOTE channel in `src/fw/board/boards/board_cyberdeck_evt3.c`. | P1 |
| nRF peripheral power enable | Safely cycle `NRF_PERIPH_PW_EN` P1.10 and prove expected I2C devices disappear/reappear. A fixture may also verify 5 V, 3.3 V, and 1.8 V test points. | `board_cyberdeck_evt3.c::board_early_init()` and `BOARD_CONFIG_CYBERDECK_PERIPH.nrfperiph_pw_en` define the confirmed assignment and sequencing. | P1 |
| Explicit nRF HFXO test | Start/measure the HF crystal with a timeout, or document BLE timing/range as sufficient coverage. | Use nRF clock-control patterns near `board_cyberdeck_evt3.c::board_early_init()` and the existing HWV `app/src/lfxo.c` bounded measurement style. | P2 |
| nRF reset test | Guided reset must produce a fresh boot banner; a fixture can also observe reset timing. | `pblboot/boards/coredevices/cyberdeck/cyberdeck.dts` for nRF reset/debug context; implement the actual guided result in HWV. | P2 |

## 3.2 Q20 + 83_OFN input tests

Only the Q20 keyboard assembly using the TCA8418 matrix and 83_OFN optical navigation device is in scope.

| New test | Implementation and pass criteria | `cyberdeck-firmware` source details to reuse | Priority |
| --- | --- | --- | --- |
| Q20 matrix all-keys test | Probe TCA8418, show each raw matrix key-down/up, verify its interrupt, detect stuck keys, and maintain a guided checklist until every expected Q20 key has passed. | `src/fw/drivers/tca8418.c` for probe, matrix configuration, FIFO draining, interrupt handling, polling fallback, counters, and `command_tca8418_status()`; `src/fw/services/cyberdeck/keyboard_manager.c` for the current `s_bb_keymap` matrix map and formatted-event path. Validate that table against the scoped Q20+83_OFN assembly rather than assuming its name proves a keyboard variant. | P0 |
| 83_OFN identity/init test | Require I2C address 0x33 product ID 0x30, apply only the OFN83 initialization table, and fail if an A320 or unknown product is detected. | `src/fw/drivers/a320.c`: `TouchpadKind_OFN83`, OFN83 register map, product-ID selection, `prv_configure_ofn83()`, reset/shutdown sequencing, and bad-ID recovery. The public file name remains `a320.c`, but the scoped backend is OFN83 only. | P0 |
| 83_OFN motion/interrupt test | Report X/Y deltas, motion interrupt activity, click flags if useful, recovery count, and guided directional movement. Require activity in all requested directions and no persistent I2C errors. | `src/fw/drivers/a320.c` for OFN83 motion registers, 8-ms asserted-INT polling, recovery, and event generation; `src/fw/services/cyberdeck/keyboard_manager.c` for converting OFN deltas into scroll detents and haptic feedback. | P0 |
| EC/end key | Report press/release transitions on P0.03 and timeout if no edge is observed. | `src/fw/services/cyberdeck/keyboard_manager.c::prv_ec_key_exti_handler()` and `BOARD_CONFIG_CYBERDECK_PERIPH.ec_key` for event semantics and the confirmed P0.03 assignment. | P0 |
| Q20 keyboard backlight | Drive `KBD_BKL` on P1.15 with off/on and brightness levels; verify visible, even illumination and acceptable current. | `BACKLIGHT_PWM` in `src/fw/board/boards/board_cyberdeck_evt3.h` configures the confirmed PWM output P1.15; `src/fw/drivers/backlight/pwm.c` implements initialization and duty-cycle control; `boards/cyberdeck_evt3/defconfig` enables `CONFIG_BACKLIGHT_PWM`. | P1 |

## 3.3 Display and audio coverage gaps

| New/modified test | Implementation and pass criteria | `cyberdeck-firmware` source details to reuse | Priority |
| --- | --- | --- | --- |
| nRF display ownership | Drive firmware-assigned `LCD_SEL` P1.05 to nRF ownership before all display traffic and confirm an nRF test pattern. This validates only the mux's nRF route. | `BOARD_CONFIG_CYBERDECK_PERIPH.lcd_sel` in `src/fw/board/boards/board_cyberdeck_evt3.h` defines the GPIO control. No K230 protocol or power behavior is part of this test. | P0 |
| Full-panel LCD diagnostics | Add black, white, checkerboard, border, row/column-address, and walking-line patterns for 400x240. | Buffer and row framing from `src/fw/drivers/display/sharp_ls013b7dh01/sharp_ls013b7dh01_nrf5.c`; geometry from `src/fw/board/displays/display_cyberdeck_evt3.h`. | P1 |
| DA7212/I2S microphone | Capture MIC1 through DA7212/I2S; report sample count, min/max, DC, AC range, and optionally waveform/echo. Flash can be freely used as scratch storage. | `src/fw/drivers/mic/nrf5/i2s_mic.c`, especially `command_mic_start()`, `command_mic_read()`, and `command_mic_echo()`; DA7212 diagnostic input routing in `src/fw/drivers/speaker/nrf5/da7212.c`; confirmed I2S pin composition in `src/fw/board/boards/board_cyberdeck_evt3.c`. | P0 |
| Headphone output | Route a known signal to both DA7212 headphone channels and verify left/right output at the jack. | Headphone gain/control and diagnostic routing in `src/fw/drivers/speaker/nrf5/da7212.c`. | P1 |
| Codec loopback/register diagnostic | Verify checked register reads/writes and perform digital or analog loopback covering both I2S directions. | DA7212 checked I2C helpers, diagnostic routes, PLL, DAI, ADC, DAC, line, and headphone setup in `src/fw/drivers/speaker/nrf5/da7212.c`; capture/playback support in `src/fw/drivers/mic/nrf5/i2s_mic.c`. | P1 |

## 3.4 Battery, power, USB, and debug tests

| New test | Implementation and pass criteria | `cyberdeck-firmware` source details to reuse | Priority |
| --- | --- | --- | --- |
| Battery voltage ADC | Add a guided battery-voltage reading and compare it against a DMM/fixture tolerance after the EVT3 ADC net, pin, and divider are confirmed in a firmware board definition. | `src/fw/drivers/battery.h` provides voltage-monitor reading/conversion API concepts, but `boards/cyberdeck_evt3/defconfig` currently selects `CONFIG_BATTERY_STUB` and the current EVT3 board definitions do not provide an SAADC assignment. Do not derive the pin from the previously misaligned schematic reading. | P0 |
| TP4054 charge indication | With USB and battery connected, verify yellow `CHRG#` LED behavior and charge current/termination using a fixture. No nPM1300-style telemetry should be reported. | No nRF Cyberdeck driver exists; source the expected behavior from schematic sheet 7. This is guided/fixture-assisted. | P0 |
| USB/battery switchover | Verify operation from USB only, battery only, and through controlled source switchover while monitoring VSYS. | No complete Cyberdeck firmware implementation exists; source topology and test points from schematic sheet 7. | P1 |
| nRF rail voltages | Fixture checks nRF 5 V, 3.3 V, and 1.8 V rails under idle and peripheral load. | Rail enable sequencing in `board_cyberdeck_evt3.c`; regulator topology/test points in schematic sheet 7. | P1 |
| nRF USB through hub | Host must enumerate the CH334R hub and nRF USB CDC function, exchange sustained bidirectional data, and recover after disconnect/reset. K230 downstream functions are ignored. | `src/fw/drivers/usb_uart.c` for TinyUSB mount/unmount, CDC RX/TX, buffering, and nRF USBD setup; `board_cyberdeck_evt3.c` for USB console composition. | P0 |
| SWD and nRF debug/UART | Fixture records successful program, halt/reset, memory read, and console traffic. | `pblboot/boards/coredevices/cyberdeck/support/openocd.cfg`, `pblboot/boards/coredevices/cyberdeck/cyberdeck.dts`, and this repository's Tigard/OpenOCD configuration. | P1 |

## 3.5 Test-harness status

Implemented:

1. Board-selected source modules and a standalone Cyberdeck DTS/pinctrl.
2. Explicit device IDs, command timeouts, sensor plausibility checks, and nonzero shell status on failures.
3. Guided completion tracking for all expected Q20 raw key codes, OFN directions, EC key, RGB colors, display patterns, speaker/headphone output, microphone signal, and haptic output.
4. Fixture-readable `HWV probe ... PASS/FAIL`, keyboard/OFN summaries, and `hwv cyberdeck status` output.
5. Q20 + 83_OFN-only behavior; A320 product ID is rejected and no K230 diagnostics are present.

Guided or blocked coverage:

- TP4054 charging, rail voltages, USB/battery switchover, and SWD require operator or fixture measurements; `hwv cyberdeck fixture` prints the checklist.
- Battery ADC remains skipped because the authoritative EVT3 firmware board definitions do not provide an SAADC assignment.
- Physical pass/fail confirmation still requires Cyberdeck EVT3 hardware; this implementation has only been built, not flashed.
