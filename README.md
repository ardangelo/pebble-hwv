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

Asterix uses its configured UART console at 115200 8N1. Cyberdeck EVT3 exposes the shell as USB CDC through the on-board hub; the host should enumerate `Cyberdeck EVT3 HWV` and show a prompt on its CDC ACM port.

### Cyberdeck EVT3

Cyberdeck commands are nRF-only and target the Q20 + 83_OFN assembly.

| Command | Description |
| --- | --- |
| `hwv cyberdeck probe` | Probe DRV2604, OPT3001, DA7212, LSM6DSO, TCA8418, and OFN83 with fixture-readable PASS/FAIL lines |
| `hwv cyberdeck status` | Summarize nRF control GPIOs and core device readiness; battery ADC is explicitly skipped |
| `hwv cyberdeck power on\|off\|cycle` | Control `NRF_PERIPH_PW_EN`; `cycle` reinitializes TCA8418 and OFN83 |
| `hwv cyberdeck rgb red\|green\|blue\|white\|off` | Verify WS2812 color order on P0.16 |
| `hwv cyberdeck backlight $VAL` | Set Q20 keyboard backlight to `$VAL: 0-100` (capped at the firmware-defined 67% duty cycle) |
| `hwv cyberdeck ec [$SECS]` | Require an EC/end-key press and release |
| `hwv cyberdeck keyboard status` | Read TCA8418 interrupt/configuration/FIFO status |
| `hwv cyberdeck keyboard check [$SECS]` | Require press and release events for every key in the current Q20 matrix map |
| `hwv cyberdeck ofn id` | Require 83_OFN product ID `0x30`; A320 is rejected |
| `hwv cyberdeck ofn motion [$SECS]` | Require OFN movement in positive and negative X/Y directions |
| `hwv cyberdeck fixture` | Print guided TP4054, rail, switchover, SWD, and USB checks |
| `hwv cyberdeck usb $TOKEN` | Echo a fixture token over USB CDC to prove shell RX/TX |
| `hwv cyberdeck reset` | Cold-reset the nRF; require a fresh HWV boot banner |

The W25Q256JW contents are disposable in Cyberdeck HWV. `erase_all`, flash stress, and microphone diagnostics may destroy the entire external flash. Battery ADC remains unimplemented because the authoritative EVT3 firmware board definition does not yet provide an SAADC assignment.

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

### Charger (Asterix only)

| Command | Description |
| --- | --- |
| `hwv charger status` | Check charger status |

To get meaningful status reports, you will need to plug the battery to `VBAT`,
`GND` and `NTC`. To test charging, connect `VBUS` to +5V.

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
| `hwv speaker play` | Play sound on speaker |
| `hwv speaker headphone` | Play a tone on both DA7212 headphone channels (Cyberdeck) |
| `hwv speaker codec` | Read DA7212 identification/status registers (Cyberdeck) |

### Microphone

| Command | Description |
| --- | --- |
| `hwv mic capture [$ARG]` | Asterix: capture optional seconds to flash; Cyberdeck: capture optional 16-ms I2S blocks and report min/max/DC/span |

For Asterix PDM captures, use a tone generator and `scripts/wavgen.py` to verify the emitted sample stream:

```shell
python scripts/wavgen.py -p /dev/$PORT -o test.wav [-s $SECS]
```

Then listen the generated WAV file in loop mode using any audio player. You
should hear the same tone you generated.

### LFXO accuracy

| Command | Description |
| --- | --- |
| `hwv lfxo test` | Obtain LFXO accuracy |

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
