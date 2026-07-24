# Pico8Pocket

PICO-8-compatible player for the Analogue Pocket, running on the
[openfpgaOS RISC-V environment](https://github.com/openfpgaOS/openfgpaSDK).

The compatibility target is PICO-8 **v0.2.7**. Both `.p8` and `.p8.png`
cartridges are in scope. The current alpha executes P8 Lua through z8lua,
decodes text and PNG carts, renders a 128×128 indexed framebuffer and packages
a native RV32 openFPGA core for Pocket.

Read [a personal note from the creator](ABOUT_ME.md) about how Pico8Pocket
started and how you can help improve it.

## Current state

- Host tests cover `.p8`, current and legacy compressed `.p8.png`, ZIP
  detection, integer scaling, P8 Lua execution and graphics/input calls. A
  locally supplied Celeste Classic cart enables an additional 120-frame
  acceptance test without distributing the game.
- APF launches the core through a core-specific instance descriptor in slot 0;
  the Pocket file picker binds the selected cartridge to slot 4.
- `_init`, `_update`/`_update60` and `_draw` run at cartridge-selected 30 or
  60 FPS. Top-level and `_init` code also run cooperatively, so cartridges
  built around an infinite loop and `flip()` yield real frames instead of
  locking during load.
- Overloaded 60 FPS cartridges use adaptive render decimation: gameplay and
  input continue updating at 60 Hz while `_draw` and video presentation fall
  back independently to 30, 20 or 15 FPS. This prevents effect-heavy scenes
  from slowing the entire game clock. The profiler's white number reports the
  resulting visible frame rate.
- The compatibility layer includes scaled `sspr`, texture-mapped `tline`,
  `oval`/`ovalfill`, patterned fills, 16/32-bit memory access, draw-state RAM
  aliases, remappable sprite/map memory and PICO-8's 32 KiB big-map mode.
- The first audio backend implements `sfx()`/`music()`, four 48 kHz stereo
  channels, the eight built-in waveforms, SFX loops, music patterns, fades and
  the standard slide/vibrato/drop/fade/arpeggio effects. It is deliberately
  integer/fixed-point to leave the RISC-V CPU available for the Lua VM.
- The current hardware-safe path scales 128×128 to 256×256 inside a proven
  320×288 openfpgaOS scanout. Pocket then scales the source 5×, producing a
  correct 1280×1280 image. Native 1408×1408 (11×) remains the target, but the
  stock bitstream's direct 128×128 mode crops the image on real hardware and
  is disabled until its scanout timing is fixed.
- The optional `FULL SOFT` display mode keeps that proven 320×288 timing but
  sharp-bilinear scales the game to 288×288 in RGB565. Pocket presents it as a
  1440×1440 square; the default remains the faster, crisp `2X` pixel-perfect
  path.
- Drawing uses an unpacked one-byte-per-pixel working surface and lazily
  synchronizes PICO-8's packed screen RAM for memory APIs. This removes the
  packed read-modify-write from the common sprite/pixel path and maps the
  display palette during the existing 2× presentation pass.
- Default controls are `B = O`, `A = X`, `Start = Pause`; `Y` and `X` are
  alternate `O` and `X` buttons. The in-core menu has a global profile plus a
  content-hash-keyed override for each cartridge; D-pad, A/B/X/Y, L/R and Start
  can all be reassigned while Select remains reserved for the system UI.
- Pressing Select opens the PICO-8-styled system menu and fully pauses the
  cartridge with a short audio fade. Holding Select and pressing the physical X
  button, or pressing X from the main system menu, toggles the opt-in
  profiler: white is FPS, green is Lua/update/draw milliseconds, orange is audio
  milliseconds, and blue is video presentation milliseconds. The profiler is
  hidden on every fresh launch.
- The paused cartridge behind the system menu is rendered through a dedicated
  15%-brightness palette before the checkerboard shade is applied, while menu
  text and panels retain their full palette brightness.
- Controller state is sampled from a lightweight Lua instruction hook while a
  cartridge frame is running. Short gameplay presses and system-button edges
  are retained until the main loop consumes them, including when emulation is
  below its target frame rate. Raw P8SCII and UTF-8 button-glyph constants are
  registered as the six standard buttons, matching compact PNG carts that use
  `btn(⬅️)`, `btn(🅾️)` or `btn(❎)` instead of numeric button indices.
- Nested PICO-8 shorthand statements now share their terminating source
  newline correctly. This fixes minified cartridges such as Moss Moss where a
  nested one-line `if` previously swallowed the following movement code into
  the wrong branch.
- The Pocket build optimizes z8lua's hot dispatch loop with defined signed
  wrapping. The input service hook is deliberately sparse, and scaled sprites
  and ellipses avoid per-pixel division/64-bit implicit-equation work.
- The menu provides Quick Save, nine numbered state slots, 64×64 previews,
  guarded deletion, restart, cart switching, controls, pixel-perfect and
  full-screen soft scale,
  volume/mute, cartridge info and English or Russian UI. States serialize Lua
  closures/globals through Eris together with RAM, framebuffer, palettes, RNG,
  input and audio channel position.
- State files are compressed and keyed by cartridge content rather than path.
  Each of Pocket's ten 256 KiB nonvolatile slots stores the corresponding
  logical state for several cartridges and evicts the least-recently-saved
  record only when that particular physical slot fills.
- `cartdata`, `dget` and `dset` use the same nonvolatile packed store and are
  keyed by the cartridge-supplied data ID. Dirty data is flushed periodically,
  on state saves and whenever the system menu opens.
- `Select Cart` directs the user to AnalogueOS Core Settings → Cartridge. The
  cartridge data slot is user-reloadable and restart-on-load. Its filename is
  deliberately not persisted, so every fresh core launch opens Pocket's native
  cartridge picker instead of reopening the previously used cart.
- Public releases contain no cartridges or third-party sound banks. The
  `Assets/pico8pocket/common/cards/` directory contains only installation
  instructions; users provide cartridges they are entitled to use.
- `make package` produces the public, cartridge-free Pocket ZIP containing the
  pinned `os20` bitstream and statically linked `rv32imafc` application.
  `make package-local` is an explicit private-build target which additionally
  bundles carts found under `assets/cards/`, `Доп игры` and `Топ игры`.
- Runtime failures show their actual Lua error on screen instead of the old
  rainbow bring-up square. Select remains available on that screen, allowing
  the cartridge to be restarted or exited without restarting the core.

This is still an alpha, not a full-compatibility release. Exact custom
instrument/filter/reverb audio behavior, local multicart switching, the PICO-8
pause menu, ZIP extraction and the indexed in-core browser are not implemented
yet. Hardware timing, state I/O and the expanded runtime need physical-Pocket
validation; scaler behavior beyond the initial boot/scaling check also needs
validation.

## Build

Run the portable tests:

```sh
make test
```

Fetch the pinned upstream dependencies:

```sh
make deps
```

Build and assemble the Pocket SD tree:

```sh
make pocket
```

The result is written to `build/pocket/pico8pocket/`. `make package` creates a
public, cartridge-free release ZIP under `releases/`. With a local Homebrew
RISC-V toolchain, use:

```sh
make package USE_SDK_CONTAINER=0
```

For private hardware testing with locally supplied cartridges, use:

```sh
make package-local USE_SDK_CONTAINER=0
```

The local archive is named `pico8pocket-v<VERSION>-local.zip` so it cannot be
mistaken for the public artifact. Local cartridge directories are ignored by
Git.

The RISC-V build uses the same container toolchain as openfpgaOS. Docker,
OrbStack, or Apple's `container` runtime is required unless a compatible
`riscv64-elf`/`riscv64-unknown-elf` toolchain is installed locally.

Install by extracting the release ZIP at the root of the Pocket SD card. On
launch, Pocket's native asset picker asks for a `.p8` or `.p8.png` cartridge.

## Cartridge location

Install cartridges under:

```text
/Assets/pico8pocket/common/cards/
```

Analogue's APF inserts `common` for platform assets, so this is the canonical
Pocket path corresponding to the requested `/assets/pico8pocket/cards` area.
At launch, Pocket's native asset picker selects the primary cart. The
planned in-core browser will consume an index stored beside the cards; see
[architecture.md](docs/architecture.md) for the APF filesystem constraint.

## Upstream bases

- openfpgaOS SDK commit `628a12b551ac8137373c477e97466b84d153d2af`
- Fake-08 commit `814991a2571ad3970e386cef48f3b148aa1c27b9`
- PICO-8 behavior target: [official v0.2.7 manual](https://www.lexaloffle.com/dl/docs/pico-8_manual.html)

These revisions are pinned by `scripts/fetch-deps.sh` so builds do not silently
change when upstream moves.

## License and disclaimer

Pico8Pocket source code authored for this repository is available under the
[MIT License](LICENSE). Dependencies and generated release components retain
their own licenses; see [third-party notices](THIRD_PARTY_NOTICES.md).

Public source and release archives contain no PICO-8 cartridges, ROMs,
commercial game data or third-party sound banks. Users must provide cartridges
they are legally entitled to use.

PICO-8 is a product of Lexaloffle Games LLP. Analogue Pocket and openFPGA are
products and technologies of Analogue. Pico8Pocket is an independent,
unofficial compatibility project and is not affiliated with, endorsed by, or
sponsored by Lexaloffle Games LLP or Analogue.
