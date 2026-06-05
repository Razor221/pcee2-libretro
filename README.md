# PCEE2 — PCSX2 libretro core

A [libretro](https://www.libretro.com/) core frontend for the current PCSX2
codebase, letting RetroArch (and other libretro frontends) run PS2 games with
an up-to-date emulation core.

Unlike [LRPS2](https://github.com/libretro/ps2) — a hard fork of an older
PCSX2 snapshot — this port keeps the libretro layer *additive*: the emulation
core is tracked from [upstream PCSX2](https://github.com/PCSX2/pcsx2) with a
minimal set of hooks, so rebasing onto new upstream releases stays cheap.

This project is not affiliated with or endorsed by the PCSX2 team.

## Status

| Area | Status |
|---|---|
| Boot + video (Vulkan, surfaceless) | ✅ working |
| Software renderer (presented via Vulkan) | ✅ working |
| Audio (48 kHz / 44.1 kHz PSX mode) | ✅ working |
| Pad input (DualShock 2, 2 ports, analogs) | ✅ working |
| Savestates (`retro_serialize`) | ✅ working, deterministic |
| Memory cards | ✅ working (`<system>/pcsx2/memcards`) |
| Core options (renderer, resolution, BIOS, fast boot) | ✅ working |
| PAL (50 Hz) / NTSC av_info | ✅ working |
| Fast-forward | ✅ working |
| OpenGL renderer | ✅ working (surfaceless EGL) |
| D3D / Metal renderers | ❌ Windows/macOS untested |
| RetroAchievements | ✅ via RetroArch (EE RAM exposed; log in to RetroAchievements in RetroArch settings) |
| Multitap, USB devices | ❌ not wired up |
| Windows / macOS builds | ❌ untested |

Output is a per-frame GPU readback (double-buffered on the GS thread, one
frame of latency). A zero-copy path via libretro Vulkan context negotiation
may come later.

## Setup

1. Put a PS2 BIOS dump into `<retroarch system dir>/pcsx2/bios/`.
2. Copy the `resources` directory from a PCSX2 installation (or from `bin/resources` of this repo) to `<retroarch system dir>/pcsx2/resources/`.
3. For built-in game patches (including the widescreen / no-interlacing options), download [`patches.zip`](https://github.com/PCSX2/pcsx2_patches/releases/latest/download/patches.zip) into `<retroarch system dir>/pcsx2/resources/patches.zip`.
4. Load a disc image (`.iso`, `.chd`, `.cso`, `.gz`, `.bin`, `.mdf`, `.nrg`, `.elf`) with the core.
5. Optional: copy your standalone `PCSX2.ini` to `<retroarch system dir>/pcsx2/inis/PCSX2.ini` — the core adopts its emulation settings (EmuCore, speed hacks, CPU, GS, game fixes) as the baseline. Core options still override their respective settings.

Memory cards, savestates metadata, cache, etc. live under
`<retroarch system dir>/pcsx2/`.

## Building (Linux)

```sh
# distro packages (Ubuntu/KDE neon)
sudo apt install -y cmake ninja-build clang liblz4-dev libwebp-dev libsdl3-dev \
  libshaderc-dev libcurl4-openssl-dev libpcap-dev libfontconfig-dev libudev-dev \
  libx11-dev libxrandr-dev extra-cmake-modules libwayland-dev libegl-dev libdbus-1-dev

# small deps not packaged by distros: plutovg, plutosvg, rapidyaml, libbacktrace
# build them into ./deps (static, PIC) — see deps-src/ recipes or upstream's
# .github/workflows/scripts/linux/build-dependencies-qt.sh for versions

cmake -B build-libretro -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DENABLE_QT_UI=OFF -DENABLE_TESTS=OFF -DENABLE_LIBRETRO=ON \
  -DCMAKE_PREFIX_PATH=$PWD/deps \
  -DSHADERC_LIBRARY=/usr/lib/x86_64-linux-gnu/libshaderc.so.1
ninja -C build-libretro pcsx2-libretro
# -> build-libretro/bin/pcee2_libretro.so
```

## Core options

| Option | Values | Notes |
|---|---|---|
| BIOS | auto / discovered images | restart required |
| Fast Boot | enabled / disabled | restart required |
| Renderer | Vulkan / OpenGL / Software | applies on the fly |
| Internal Resolution | 1x–4x | applies on the fly, scales output too |
| Blending Accuracy | Minimum–Maximum | default Basic |
| Texture Filtering | Nearest / Bilinear (PS2/Forced/Forced-no-sprites) | default PS2 |
| Trilinear Filtering | Auto / Off / PS2 / Forced | |
| Anisotropic Filtering | Off–16x | |
| Dithering | Off / Scaled / Unscaled | default Unscaled |
| Hardware Mipmapping | enabled / disabled | |
| Deinterlacing | Automatic + 8 manual modes | default GameDB-driven |
| FXAA | enabled / disabled | |
| CAS (sharpening) | Disabled / Sharpen Only + sharpness 10–100 | |
| Widescreen Patches | enabled / disabled | built-in 16:9 patches |
| No-Interlacing Patches | enabled / disabled | built-in progressive patches |
| EE Cycle Rate | 50%–300% | speed hack, may break games |
| EE Cycle Skip | Disabled–Maximum | speed hack, may break games |

All graphics options map directly onto the corresponding standalone PCSX2
settings; anything not exposed yet runs at the standalone default (including
automatic per-game fixes from the GameDB).

## Architecture notes

- The frontend (`pcsx2-libretro/Libretro.cpp`) is modeled on `pcsx2-gsrunner`:
  a dedicated CPU thread runs the `VMManager::Execute()` loop, and
  `Host::PumpMessagesOnCPUThread()` paces it 1:1 against `retro_run()`.
- Frames arrive through `GSSetFramebufferReadback()` — a double-buffered
  readback on the GS thread added for this port (`GSRenderer.cpp`).
- Audio is pulled from a custom `AudioStream` registered through
  `SPU2::CustomOutputStreamFactory`.
- Savestates use `SaveState_ZipToBuffer`/`SaveState_UnzipFromBuffer`
  (in-memory variants of the existing zip paths).
- Core modifications beyond these hooks are intentionally minimal; see
  `git log --oneline upstream/master..libretro -- pcsx2/ common/` for the
  full delta.

## License

GPL-3.0+, same as PCSX2. All emulation code is the work of the
[PCSX2 team and contributors](https://github.com/PCSX2/pcsx2/graphs/contributors).
