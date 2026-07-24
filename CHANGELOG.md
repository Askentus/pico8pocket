# Changelog

## 0.0.27 — 2026-07-23

First public alpha release.

### Highlights

- Runs single-cartridge `.p8` and `.p8.png` PICO-8 games on Analogue Pocket.
- Adds a Select-driven system menu with per-cartridge controls, ten compressed
  save-state slots with previews, restart and cartridge switching.
- Implements PICO-8 graphics, input, fixed-point Lua execution, four-channel
  audio, `cartdata`, `dget` and `dset` compatibility.
- Provides pixel-perfect `2X` and optional full-screen soft scaling.
- Adds an opt-in FPS/runtime/audio/presentation profiler with Select+X.
- Fixes persistent-state slot alignment for the `os20` Pocket runtime.
- Includes compatibility and performance work for minified carts, heavy
  effects, large off-screen fills and responsive input under load.

### Known limitations

- This is an alpha and does not provide complete PICO-8 compatibility.
- Multicart `load()` switching, ZIP extraction and the PICO-8 pause menu are
  not implemented.
- CPU-heavy cartridges may run below their intended frame rate.
- Save persistence changes require confirmation on physical Pocket hardware.
- No cartridges, ROMs or third-party sound banks are included. Users must
  provide cartridges they are legally entitled to use.
