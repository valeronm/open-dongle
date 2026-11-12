cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(open_dongle)

set(manifest ${CMAKE_CURRENT_SOURCE_DIR}/tool/Cargo.toml)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${manifest})
file(STRINGS ${manifest} version_line REGEX "^version = \"[0-9]+\\.[0-9]+\\.[0-9]+\"$" LIMIT_COUNT 1)
if(NOT version_line MATCHES "([0-9]+)\\.([0-9]+)\\.([0-9]+)")
  message(FATAL_ERROR "no version line in ${manifest}")
endif()
set_property(SOURCE src/main.c APPEND PROPERTY COMPILE_DEFINITIONS
  FIRMWARE_VERSION_MAJOR=${CMAKE_MATCH_1} FIRMWARE_VERSION_MINOR=${CMAKE_MATCH_2}
  FIRMWARE_VERSION_PATCH=${CMAKE_MATCH_3})

target_sources(app PRIVATE src/radio.c src/noise.c src/usb.c src/indicator.c)
if(CONFIG_UNIFYING_POC)
  target_sources(app PRIVATE src/unifying_main.c src/unifying.c src/reports.c src/store.c
    src/chanpick.c)
else()
  target_sources(app PRIVATE
    src/main.c src/nuphy.c src/reports.c src/chanpick.c src/link.c src/store.c src/diag.c
    src/diagproto.c src/history.c)
endif()
