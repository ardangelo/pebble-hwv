board := "cyberdeck_evt3"
build_dir := "build/cyberdeck_evt3"
west_env := "ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb GNUARMEMB_TOOLCHAIN_PATH=/usr"
openocd_bin := env_var_or_default("OPENOCD", "/store/git/openocd/install/bin/openocd")
gdb_bin := env_var_or_default("GDB", "/store/gcc-14-arm-none-eabi/bin/arm-none-eabi-gdb")
openocd_cfg := "boards/coredevices/cyberdeck_evt3/support/openocd.cfg"

# Build the Cyberdeck EVT3 HWV image.
default: build

# Configure a pristine Cyberdeck EVT3 build directory without compiling.
configure:
    {{west_env}} west build -b {{board}} app -d {{build_dir}} --pristine=always --cmake-only

# Compile, configuring first when necessary.
build:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -f "{{build_dir}}/build.ninja" ]]; then
        just configure
    fi
    {{west_env}} west build -d {{build_dir}}

# Flash the HWV image through the Tigard SWD adapter.
flash: build
    {{openocd_bin}} -f {{openocd_cfg}} -c "program {{build_dir}}/zephyr/zephyr.hex verify reset exit"

# Run an OpenOCD debug server for the Tigard (GDB on localhost:3333).
openocd:
    {{openocd_bin}} -f {{openocd_cfg}}

# Attach GDB to an OpenOCD server started with `just openocd`.
debug: build
    {{gdb_bin}} {{build_dir}}/zephyr/zephyr.elf -ex "target extended-remote :3333"
