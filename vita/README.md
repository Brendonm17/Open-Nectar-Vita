# Open Nectar for PS Vita

A native PS Vita port of Pikmin, built on the Open Nectar decompilation.
It needs your own copy of the game. No game data is included.

## Requirements

- A PS Vita or PS TV running HENkaku/Ensō
- A Pikmin (USA) GameCube disc image, game ID `GPIE01`, revision 1
- About 1.5 GB free on `ux0:`

## Install

1. Install `nectar_vita.vpk` with VitaShell.
2. Copy your disc image to `ux0:data/Pikmin/`.
   `.iso`, `.gcm`, `.rvz`, `.wia` and Dolphin `.ciso` all work. RVZ keeps it small.
3. Launch Pikmin from the home screen.

Updating: install the new VPK over the old one. To refresh the LiveArea
artwork after an update, delete the bubble first, then install.
Saves live in `ux0:data/Pikmin`, so deleting the bubble keeps them.

## Saves

Saves are standard GameCube `.gci` files in `ux0:data/Pikmin/`.
They work in Dolphin and on a real GameCube, and Dolphin saves can be copied in.

## Options

Controls and settings are in the in-game options menu on the title screen.

## Building

Linux or WSL with:

- [VitaSDK](https://vitasdk.org) with `VITASDK` set
- CMake 3.16+, Ninja, Python 3
- VitaSDK packages: `sdl2`, `zstd`, `xz`, `bzip2`, `libmathneon`, `vitaShaRK`, `SceShaccCgExt`
- The vitaGL fork used by this port:

```
bash vita/tools/build_vitagl.sh
```

Then build:

```
bash vita/build.sh
```

The VPK lands in `vita/build/nectar_vita.vpk`.
Plain CMake works too:

```
cmake -S vita -B vita/build -G Ninja
cmake --build vita/build
```

Build options (`-D<name>=ON/OFF`, or `CMAKE_ARGS` with `build.sh`):

| Option | Default | |
|---|---|---|
| `NECTAR_LOGGING` | ON | Runtime log at `ux0:data/Pikmin/nectar.log` |
| `NECTAR_LTO` | ON | Link-time optimisation |
| `NECTAR_PROF` | OFF | Sampling profiler |
| `NECTAR_DEBUG_UNLOCK` | OFF | Unlock all stages (debug only) |

LiveArea art is not included. Put `icon0.png` and
`livearea/contents/{bg0.png,startup.png,template.xml}` in `vita/sce_sys/`
to have them packed into the VPK.

## Layout

- `vita/src` platform layer, render thread, shader tables
- `vita/shaders` baked GXP shaders and their manifest
- `pc_port/gl` GX translation shared with the desktop port

## Credits

Open Nectar and the Pikmin decompilation projects.
Pikmin is a trademark of Nintendo. This project is not affiliated with Nintendo or Sony.
