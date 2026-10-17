#!/usr/bin/env bash
# Build PICOCRAFT into picocraft.uf2 for the Pimoroni PicoSystem.
#
# You need:
#   * Raspberry Pi Pico SDK  https://github.com/raspberrypi/pico-sdk
#     (set PICO_SDK_PATH to where it lives, or export PICO_SDK_FETCH_FROM_GIT=1)
#   * an arm-none-eabi cross toolchain in PATH (gcc-arm-none-eabi / arm-gnu-toolchain)
#
# Usage:  ./build.sh            -> leaves picocraft.uf2 next to this script
set -euo pipefail

if [[ ! "${PICO_SDK_PATH:-}" ]]; then
  echo "Set PICO_SDK_PATH to your copy of the Raspberry Pi Pico SDK, e.g."
  echo "  PICO_SDK_PATH=~/pico-sdk ./build.sh"
  exit 1
fi

cd "$(dirname "$0")"
root="$(pwd)"

rm -rf build
mkdir -p build
(
  cd build
  cmake .. -DPICO_PLATFORM=rp2040 -DPICO_COMPILER=pico_arm_cortex_m0plus_gcc
  make -j"$(nproc)"
)

cp build/picocraft.uf2 picocraft.uf2
echo "built: $root/picocraft.uf2"
echo
echo "To flash: hold X, then press power to boot the PicoSystem into DFU mode."
echo "The console shows up as a drive called RPI-RP2 - copy picocraft.uf2 onto it."
