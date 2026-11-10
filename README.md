# PICOCRAFT — Minecraft for the Pimoroni PicoSystem, in real 3D

A voxel sandbox for the [Pimoroni PicoSystem](https://shop.pimoroni.com/products/picosystem)
(Raspberry Pi's RP2040 handheld). It is **true 3D**, not a raycaster and not Mode 7:
cubes are projected through a real camera, shaded per face normal, blended into
distance fog and drawn back to front so you can walk around them, jump on top of
them, dig under them and build towers block by block.

`picocraft.uf2` in this folder is ready to play — see *Flashing* below.

## Controls

| Input        | Action                                                        |
| ------------ | ------------------------------------------------------------- |
| D-pad        | walk forward/back, turn left/right                            |
| `A`          | jump (swim up while in water)                                 |
| `B`          | break the block at the crosshair — hold to keep digging       |
| `X`          | place the selected block at the crosshair                     |
| `Y` (tap)    | change which block you are placing                            |
| `Y` (hold)   | aim mode: the D-pad looks around instead of walking           |

You start with a small backpack of blocks. Breaking blocks adds them to your
backpack, placing spends them. The hotbar in the bottom-left shows every block
type you can carry; the bracketed slot is the one you place, and its exact count
is printed next to it (mining 10 gold at a time pops a "GOLD MINE" toast). The
gold wireframe box marks the block `B` would break and `X` would build against.

The world is **infinite**: terrain is generated on demand in 16x16 chunks from a
pure heightmap function, so hills, beaches, water and trees keep going forever in
every direction — there is no island edge to fall off. A 5x5 ring of chunks (~128 KB)
stays resident around you and old chunks are regenerated identically when you walk
back. The only floor is bedrock at the bottom of the world, so digging down dead-ends
but walking never does.

There is a full day/night cycle (sun and moon arc across the sky with drifting
clouds by day and stars by night), water you can swim in with a breath meter, fall
damage and hearts.

## How the 3D works

* **Camera** — yaw/pitch basis vectors built from a 2048-entry sine table in Q14
  fixed point; no floating point anywhere in the renderer.
* **Projection** — every voxel face corner is rotated into view space and divided
  by its depth (Q24 dot products, one divide per vertex).
* **Hidden surface removal** — visible cells are bucketed by view depth with a
  counting sort and drawn far to near, so uniform size voxels always overlap
  correctly. Faces hidden by a solid neighbour and backfacing faces are skipped
  before they ever reach the rasteriser.
* **Shading** — one flat colour per face from the material's top/side colour, a
  fixed light level per axis (bright tops, dark bottoms) times the daylight
  factor, then blended toward the horizon colour for fog.
* **Rasterising** — scanline triangle fill writing straight into the framebuffer,
  no blending, so overdraw is cheap.

* **Chunked world** — terrain columns come from a pure noise function of world
  (x, z), so every chunk agrees with its neighbours at the seam; trees are written
  from a scan that reaches 3 blocks past the chunk border so canopies cross seams.
* **Frame pacing** — input and physics run every ~60 Hz frame with real dt from
  `time_us()`, while the scene draws on alternate frames: a steady ~30 fps render
  where each picture gets two engine frames' worth of time. The measured rate is
  shown bottom-right, averaged over a window of rendered frames so it stays put
  instead of being pinned by one bad sample.

Typical rendered frames gather ~2000–3000 cells and draw ~3000–4000 faces at
120x120 (pixel doubled to the 240x240 screen). Flash usage is ~110 KB; RAM sits
around 212 KB of the console's region, leaving room for stack and heap.

## Layout

```
picocraft.uf2              the built game, ready to flash
src/main.cpp               the whole game (engine + world + HUD)
CMakeLists.txt             build recipe for the Pico SDK + PicoSystem library
vendor/picosystem/         vendored copy of Pimoroni's PicoSystem v1 library
host/                      desktop stand-in so the logic can be tested on a PC
build.sh                   convenience build script
build/                     generated CMake tree from this machine (snapshot)
```

## Building

```bash
PICO_SDK_PATH=~/pico-sdk ./build.sh
```

Requirements: Raspberry Pi's [Pico SDK](https://github.com/raspberrypi/pico-sdk)
and an `arm-none-eabi` cross toolchain in your `PATH`. The vendored PicoSystem
library (MIT style license, see `vendor/picosystem/PICOSYSTEM_LICENSE`) means you
do not need the picosystem repo itself.

## Flashing

1. Plug the PicoSystem in with a USB-C cable.
2. Hold down `X` and press the power switch — it boots into DFU mode and mounts
   as a drive called `RPI-RP2`.
3. Copy `picocraft.uf2` onto that drive; it launches straight away.

```bash
cp picocraft.uf2 /media/$(whoami)/RPI-RP2      # Linux
```

## Testing the logic on a desktop

`host/picosystem_stub.hpp` implements just enough of the PicoSystem API to run
the game binary on a PC, and `host/driver.cpp` feeds scripted inputs and dumps
frames as PNGs:

```bash
g++ -std=c++17 -O1 -DHOST_TEST -iquote host src/main.cpp host/driver.cpp -o /tmp/game
/tmp/game 800          # prints player state and writes f####.ppm frames next to it
/tmp/game 300 profile  # prints a text horizon profile instead of images
/tmp/game 3000 walk    # walks forever in every direction, then checks that every
                       # sampled chunk column still agrees with the pure heightmap
```
# PicoCraft
# PicoCraft
# PicoCraft
# PicoCraft
# PicoCraft
# PicoCraft
