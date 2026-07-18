cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(app LANGUAGES C)

target_sources(
  app
  PRIVATE
    src/main.c
    src/ble.c
    src/display.c
    src/flash.c
    src/haptic.c
    src/imu.c
    src/light.c
    src/lfxo.c
)

if (CONFIG_BOARD_ASTERIX)
  target_sources(
    app
    PRIVATE
      src/audio_data.c
      src/speaker.c
      src/buttons.c
      src/charger.c
      src/mag.c
      src/mic.c
      src/press.c
  )
endif()

if (CONFIG_BOARD_CYBERDECK_EVT3)
  target_sources(
    app
    PRIVATE
      src/cyberdeck.c
      src/mic_i2s.c
      src/speaker_cyberdeck.c
  )
endif()
