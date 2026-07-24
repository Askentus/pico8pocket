# Compatibility target

The behavioral target is the official PICO-8 v0.2.7 specification:

- 128×128 display and base/extended palettes;
- six-button input for player 0;
- 64 KB base RAM and 32 KB cartridge ROM layout;
- P8 Lua syntax, 16.16 fixed-point numbers and VM timing semantics;
- `_init`, `_update`, `_update60`, `_draw`, `flip` and pause-menu behavior;
- graphics, map, memory, math, string, table, audio and persistence APIs;
- `.p8`, `.p8.png`, legacy and current compressed-code encodings;
- local multicart `load`/`reload` behavior;
- `cartdata`, `dget` and `dset` persistence.

Out of scope for the Pocket hardware target:

- online SPLORE/BBS downloads, because Pocket has no network interface;
- multiplayer input beyond player 0, by product decision;
- PICO-8 editor and development tools. This project is a player.

## Initial test set

- Celeste Classic: first real-game acceptance target.
- PICO-8 `API.P8`: broad API smoke test once legally supplied by the user.
- Small project-owned carts for fixed-point overflow, shorthand syntax,
  palette/transparency, map sharing, `btnp`, audio effects, multicart loading,
  persistence and 30/60 FPS scheduling.

Compatibility claims must be backed by a named cart/test and a Pocket run.

## Alpha implementation status

Implemented and covered by host tests:

- text `.p8` ROM-section parsing;
- RGBA/RGB `.p8.png` extraction with uncompressed, legacy `:c:` and current
  PXA compressed-code streams;
- P8 Lua parsing/execution with 16.16 fixed-point numbers;
- `_init`, `_update`, `_update60`, `_draw`, 30/60 FPS selection and player-0
  `btn`/`btnp` state, including raw P8SCII button-glyph constants used by
  compressed PNG carts;
- cooperative top-level and `_init` execution with real `flip()` yielding;
- `run()` cartridge restart plus nil-safe PICO-8 table helpers and relative
  mouse `stat(38)`/`stat(39)` defaults;
- extended graphics (`sspr`, `tline`, ovals and `fillp`), sprite/map remapping,
  big-map memory, draw-state aliases and 8/16/32-bit memory APIs;
- persistent `cartdata`, `dget` and `dset` records in Pocket save storage;
- four-channel 48 kHz `sfx`/`music` playback with built-in waveforms, loops,
  music patterns, fades and the standard note effects;
- Eris-based Lua state persistence together with RAM, renderer, input, RNG and
  audio state, compressed per cartridge with state previews;
- 120 error-free headless frames of the official Celeste Classic cart.

Not yet compatibility-complete:

- exact custom-instrument, filter, reverb and hardware-audio behavior;
- pause/menu callbacks, general P8SCII/custom font behavior beyond button
  constants, and the remaining obscure draw-state/hardware aliases;
- PICO-8 `load()` multicart behavior;
- ZIP extraction and the graphical indexed card browser;
- full API/conformance coverage and physical-Pocket performance validation.
