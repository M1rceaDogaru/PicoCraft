cmake_minimum_required(VERSION 3.12)

# PICOCRAFT - a 3D voxel sandbox for the Pimoroni PicoSystem (RP2040).
# Needs Raspberry Pi's Pico SDK: set PICO_SDK_PATH to point at it.
include(vendor/picosystem/pico_sdk_import.cmake)

project(picocraft C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# The vendored copy of Pimoroni's PicoSystem v1 library
include(${CMAKE_CURRENT_LIST_DIR}/vendor/picosystem/libraries/picosystem.cmake REQUIRED)

picosystem_executable(
  picocraft
  src/main.cpp
)

pixel_double(picocraft)          # render at 120x120, doubled onto the 240x240 screen
disable_startup_logo(picocraft)  # we draw our own title card
no_spritesheet(picocraft)        # primitives only - saves 32kB of RAM
