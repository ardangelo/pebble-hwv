# CoreDevices Hardware Verification Firmware (HWV)

This repository contains a basic application to verify CoreDevices hardware.

## Getting started

Before getting started, make sure you have a proper Zephyr development
environment. Follow the official
[Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/getting_started/index.html).

### Initialization

The first step is to initialize the workspace folder (`my-workspace`) where the
`pebble-hwv` and all Zephyr modules will be cloned. Run the following command:

```shell
# initialize my-workspace for the pebble-hwv (main branch)
west init -m https://github.com/coredevices/pebble-hwv --mr main my-workspace
# update Zephyr modules
cd my-workspace
west update
```

### Building and running

To build the application, run:

```shell
cd pebble-hwv
west build -b $BOARD_TARGET app
```

`$BOARD_TARGET` can be `asterix` or `cyberdeck_evt3`. Cyberdeck developers may also use `just configure && just build`; the `Justfile` supplies the repository's GNU Arm toolchain settings.

Once you have built the application, run the following command to flash it:

```shell
west flash
```

By default boards use the `jlink` runner. Alternative runners can be used with
`west flash -r $RUNNER_NAME`.

### Boards

Supported boards:

- `asterix`
- `cyberdeck_evt3`

## Usage

Asterix uses UART at 115200 8N1. Cyberdeck EVT3 exposes USB CDC through the
CH334R hub (`Cyberdeck EVT3 HWV`); open its CDC ACM port and check the version,
startup diagnostics, and shell prompt before running commands. The release
workflow runs for pushed `v*` tags and attaches separate
`pebble-hwv_<board>_<version>.hex` images for Asterix and Cyberdeck. Pushes to
`main` or `mfg-parity` run the two-board build workflow; a local branch build
is not a published release.

### Cyberdeck EVT3

Cyberdeck commands are nRF-only and target the Q20 + 83_OFN assembly.

| Command | Description |
| --- | --- |
| `hwv cyberdeck probe` | Probe DRV2604, OPT3001, DA7212, LSM6DSO, TCA8418, and OFN83 with fixture-readable PASS/FAIL lines |
| `hwv cyberdeck status` | Summarize nRF control GPIOs and core device readiness; use `hwv charger status` for measured battery voltage |
| `hwv cyberdeck power on\|off\|cycle` | Control `NRF_PERIPH_PW_EN`; `cycle` reinitializes TCA8418 and OFN83 |
| `hwv cyberdeck rgb red\|green\|blue\|white\|off` | Verify WS2812 color order on P0.16 |
| `hwv cyberdeck backlight $VAL` | Set Q20 keyboard backlight to `$VAL: 0-100` (capped at the firmware-defined 67% duty cycle) |
| `hwv cyberdeck ec [$SECS]` | Require an EC/end-key press and release |
| `hwv cyberdeck keyboard status` | Read TCA8418 interrupt/configuration/FIFO status |
| `hwv cyberdeck keyboard check [$SECS]` | Require press and release events for every key in the current Q20 matrix map |
| `hwv cyberdeck ofn id` | Require 83_OFN product ID `0x30`; A320 is rejected |
| `hwv cyberdeck ofn motion [$SECS]` / `click [$SECS]` | Require OFN movement in positive and negative X/Y directions, and a physical click separately |
| `hwv cyberdeck fixture` | Print guided TP4054, rail, switchover, SWD, and USB checks |
| `hwv cyberdeck usb $TOKEN` | Echo a fixture token over USB CDC to prove shell RX/TX |
| `hwv cyberdeck reset` | Cold-reset the nRF; require a fresh HWV boot banner |

The W25Q256JW contents are disposable only while Cyberdeck HWV owns the chip.
Run `hwv flash id` (expect `ef 60 19`) before destructive erase/write/stress;
`erase_all` checks completion and spot-checks the beginning, middle and end.
Never run these commands after provisioning identity or recovery images.

### BLE

| Command | Description |
| --- | --- |
| `hwv ble on` | Turn ON BLE and advertising as `Pebble HWV` |
| `hwv ble off` | Turn OFF BLE |

You can use any utility to test connection and writing to the exposed GATT characteristic (e.g. [LightBlue](https://punchthrough.com/lightblue/)). Note that after disconnecting the firmware will no longer advertise.

### Buttons (Asterix only)

| Command | Description |
| --- | --- |
| `hwv buttons check` | Check if buttons are pressed/release |

### Charger and battery

| Command | Description |
| --- | --- |
| `hwv charger status` | Asterix: nPM1300 telemetry; Cyberdeck: ADC-measured VBAT in mV and nRF USB VBUS detection |

Cyberdeck EVT3 schematic sheets 5 and 7 connect `VBAT_VOL` to nRF
P0.31/AIN7 through R76=300 kΩ and R77=100 kΩ (4:1 divider). Compare the
reported VBAT to a DMM across the battery contacts. TP4054 exposes its
`CHRG#` yellow LED, not digital charge telemetry: battery current, temperature,
charge state, capacity and termination are **not measured by HWV**. With a
suitable battery and USB supply, measure charging current externally in series
with the battery and observe LED behavior through termination. Use the
board-specific operating procedure for agreed electrical limits; no ADC
reading alone establishes a pass for the charging circuit. On Asterix connect
the battery to `VBAT`, `GND`, `NTC` and +5 V to `VBUS` for charger testing.

### Display

| Command | Description |
| --- | --- |
| `hwv display on` | Turn ON the display |
| `hwv display off` | Turn OFF the display |
| `hwv display vpattern` | Draw a vertical pattern |
| `hwv display hpattern` | Draw an horizontal pattern |
| `hwv display white` / `black` | Draw full-panel solid patterns |
| `hwv display checker` | Draw a checkerboard pattern |
| `hwv display border` | Draw a border/addressing pattern |
| `hwv display brightness $VAL` | Adjust display backlight brightness, `$VAL: 0-100` |

On Cyberdeck, `off` clears the LCD and disables its backlight without switching
off the shared peripheral rail. EXTCOMIN keeps running while the LCD is powered.
Drawing while blanked updates the saved framebuffer; `on` restores it. Use
`hwv cyberdeck power off` only when the shared peripherals should lose power.

### Flash

| Command | Description |
| --- | --- |
| `hwv flash id` | Read flash chip JEDEC ID |
| `hwv flash erase $ADDR` | Erase flash page for the given `$ADDR` |
| `hwv flash erase_all` | Destructively erase the entire external flash |
| `hwv flash read $ADDR $N` | Read `$N` bytes from address `$ADDR` |
| `hwv flash write $ADDR $VAL` | Write `$VAL` (hex encoded, e.g. `aabbccdd`) to `$ADDR` |
| `hwv flash stress $ITERS` | Perform flash stress test `$ITERS` times |

### Haptic

| Command | Description |
| --- | --- |
| `hwv haptic configure $VAL` | Configure haptic motor intensity `$VAL: 0-100` |

### Sensors

| Command | Description |
| --- | --- |
| `hwv imu get` | Obtain IMU readings (acc/gyro) |
| `hwv imu interrupt [$SECS]` | Wait for an LSM6DSO INT1 data-ready event |
| `hwv light get` | Obtain ALS readings |
| `hwv mag get` | Obtain magnetometer readings (Asterix only) |
| `hwv press get` | Obtain pressure sensor readings (Asterix only) |

### Speaker

| Command | Description |
| --- | --- |
| `hwv speaker play` | Play a tone through fixed 4 MHz-MCK direct nrfx I2S (Cyberdeck) |
| `hwv speaker headphone` | Play the same fixed-clock tone on both DA7212 headphone channels (Cyberdeck) |
| `hwv speaker codec` | Read DA7212 identification/status registers (Cyberdeck) |

### Microphone

| Command | Description |
| --- | --- |
| `hwv mic capture [$SECS]` | Both boards: capture one second (default/max), at 16 kHz, mono signed 16-bit; Asterix PDM uses disposable flash and Cyberdeck DA7212 MIC1_P uses bounded RAM. Both emit `S`, one decimal sample per line, `E`; Cyberdeck also reports RMS/DC/clipping diagnostics before the stream. |

Install `pyserial` on the host. Close miniterm or any other console reader
before running the capture script. For either board, apply a test tone at the
microphone and capture a WAV from the console port:

```shell
python scripts/wavgen.py -p /dev/$PORT -o test.wav -s 1
```

`wavgen.py` ignores the command echo and diagnostics such as
`DA7212 mic PLL status: 0x07` before `S`. It requires exactly 16,000 samples
between `S` and `E` and fails on timeout, truncated output, bad samples or a
reported HWV error; it does not create a partial WAV. After rejecting a sample,
it discards the remaining stream through `E` before releasing the port, within
the original 120-second capture deadline.

If an older script fails at `int(line)` on a diagnostic, update the host script;
that parser failure does not require reflashing the board. The board continues
transmitting after the script exits, so numbers printed when reattaching are
the remaining PCM samples. Let them finish through `E` and the shell prompt
before issuing another command.

Check the WAV is one second at 16 kHz and listen for the applied tone. The
Cyberdeck path preserves its MIC1/codec-master 16-kHz clocks and fixed 4-MHz
MCK. Its RAM bound is one second; Asterix checks flash capacity before erasure.
Cyberdeck rejects a capture if nrfx reports a reused DMA buffer, even when the
requested sample count was reached before shutdown. A failed capture emits no
`S`/`E` PCM stream, so the host must not treat it as a valid WAV.

### LFXO accuracy

| Command | Description |
| --- | --- |
| `hwv lfxo test` | Compare LF crystal-derived kernel ticks against an HFXO-driven timer over ten seconds; report signed error in parts per billion (ppb). |

The board defconfig selects the 20-ppm LF crystal source. Apply the agreed
absolute error limit to `abs(ppb) / 1000` (ppm); the number reported by HWV is
**not** an absolute-error pass/fail decision without that limit. Check clock
configuration if this test is used with a different build. An external
frequency counter is required only if the agreed accuracy specification
demands an independent reference.

## Low power measurement tips

To perform low-power measurements it is advised to compile with serial disabled
using the `no-serial.conf` overlay, i.e.

```shell
west build -b $BOARD_TARGET app -- -DOVERLAY_CONFIG="no-serial.conf"
```

Without serial, you may also want to hardcode certain commands. You can do it
like this at the end of `main`:

```c
shell_execute_cmd(NULL, "hwv ble on");
shell_execute_cmd(NULL, "hwv display on");
shell_execute_cmd(NULL, "hwv display vpattern");
```

RTT may also come handy as it will only consume power when RTT is attached. For
this purpose, you can append the `rtt.conf` to the overlay list.
