# Changelog

## 0.0.31 — 2026-07-25

Second public alpha release.

### Highlights

- Reworks adaptive rendering around separately measured update and draw costs.
  Overloaded 30/60 FPS carts now skip drawing only when it materially protects
  game speed, recover visual cadence faster after heavy effects, and retain a
  bounded amount of frame-time debt instead of introducing extra waits.
- Expands the opt-in Select+X profiler with labelled logical/visible FPS,
  update/draw/audio/presentation timings, render divisor, approximate Lua work
  and PICO-8 API call categories. This makes CPU, drawing and presentation
  bottlenecks distinguishable on real Pocket hardware.
- Adds faster ordinary `map()` tile rendering, cheaper `mget()`/`fget()` and
  Lua table iteration, no-op palette/transparency fast paths, and a unity-gain
  audio path. Effect- and tile-heavy cartridges spend less time in common hot
  loops, although CPU-bound carts can still miss their target frame rate.
- Removes the experimental `FULL SOFT` scaler after hardware testing showed a
  visibly blurry result. Crisp pixel-perfect `2X` remains the default and the
  only enlarged display mode in this release.
- Confirms on physical Pocket hardware that all ten save-state slots persist
  after leaving and reopening the core, cartridge selection works on every
  fresh launch, Select is the sole menu shortcut, and the profiler starts off.
- Keeps the public package cartridge-free. No PICO-8 games, ROMs or third-party
  sound banks are included.

### Known limitations

- This is still an alpha and does not provide complete PICO-8 compatibility.
- Multicart `load()` switching, ZIP extraction, native 11× display output and
  the PICO-8 pause menu are not implemented.
- CPU-heavy cartridges such as BAS Escape, UFO and later Celeste 2 scenes can
  still run substantially below their intended frame rate.

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
