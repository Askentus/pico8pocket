# Architecture

## Hardware and runtime

Pico8Pocket is an openFPGA core wrapping an openfpgaOS application. The current
ThinkElastic stack provides:

- VexiiRiscv `rv32imafc`, 100 MHz;
- 64 MB SDRAM;
- indexed and direct-color framebuffers;
- 48 kHz stereo audio and a hardware PCM mixer;
- Pocket controls, Dock controllers, keyboard and mouse;
- APF data slots and ten persistent 256 KB save slots;
- a small SDL2 compatibility layer.

The `os20` bitstream is the default because PICO-8 is a 2D workload and benefits
more from its dual-issue CPU than from the 2.5D/3D accelerators in the other
variants.

## Runtime boundaries

```text
P8 cartridge (.p8 / .p8.png)
        |
        v
cart decoder -> P8 Lua VM -> PICO-8 API / RAM model
                                  |
                   +--------------+--------------+
                   |              |              |
               128x128         48 kHz PCM      cartdata
               palette FB      synthesizer     save slots
                   |              |              |
                   +------- openfpgaOS HAL ------+
                                  |
                          Analogue Pocket APF
```

Fake-08 is the starting point for the cart decoder, z8lua integration, PICO-8
RAM model, graphics, and audio semantics. Its platform-specific frontend is
replaced by `src/platform/openfpgaos/`; compatibility gaps are then measured
against PICO-8 v0.2.7 rather than accepted as permanent Fake-08 limitations.

## Video

The logical surface is always 128×128 with square pixels. On the Pocket LCD,
the desired integer presentation is:

```text
scale = min(floor(1600 / 128), floor(1440 / 128)) = 11
image = 1408 x 1408
offset = (96, 16)
```

Physical-Pocket testing showed that the stock openfpgaOS bitstream does not
produce a matching APF scanout when its software framebuffer is changed
directly to 128×128: Pocket enlarges only part of the picture. The compatible
path therefore uses the known-good 320×288 timing and performs a 2× nearest-
neighbor expansion to a centered 256×256 image. APF then scales the full source
5×, resulting in a sharp 1280×1280 game image.

The desired 1408×1408 presentation is an overall prime 11× scale, so it cannot
be composed from the safe 2× software source scale. Achieving it requires a
true 128×128 scanout timing in the openfpgaOS gateware/runtime. That remains a
separate hardware task; the emulator now prefers a complete 10× image over a
cropped nominal 11× image.

An experimental sharp-bilinear full-screen mode was tested on real hardware and
removed because its 288×288 intermediate image was visibly blurry. `2X` is now
the only enlarged mode. A future full-screen option should use a true 128×128
scanout and Pocket's native 11× presentation instead of software filtering.

## Input

PICO-8 buttons are mapped by position and common emulator convention:

| Pocket | PICO-8 | Reason |
|---|---|---|
| D-pad | directions | direct |
| B (bottom) | O / button 4 | primary action; matches Fake-08 libretro `B` |
| A (right) | X / button 5 | secondary action; matches Fake-08 libretro `A` |
| Y (left) | O alternate | ergonomic duplicate |
| X (top) | X alternate | ergonomic duplicate |
| Start | Pause | reserved for the PICO-8 pause menu |
| Select | system | opens the system menu |
| Select + physical X | diagnostics | toggles the frame profiler |

Input edges reported by openfpgaOS are retained separately from held state so
short presses are not lost when a cartridge frame exceeds its budget.

The application keeps a global mapping and an optional mapping keyed by each
cartridge content hash. Select is deliberately excluded from remapping so the
system menu and diagnostics chord are always recoverable.

## System menu and state storage

The system menu runs on a separate 128×128 surface and stops VM frames while
open. The audio output ramps down over 1024 samples and then emits silence
without advancing the synthesizer; closing the menu ramps it back up.

Eris serializes the cartridge-created Lua globals and closures while treating
the runtime libraries/API functions as permanent objects. A state also stores
PICO RAM, the unpacked screen, draw/screen palettes, transparency, input repeat
state, RNG, camera/clip/cursor state, scheduler position and audio channels.
The combined blob is deflated and protected by a CRC32.

Pocket exposes ten 256 KiB persistent files. Each file is one logical lane
(Quick, Auto, or Slot 1–8) containing content-hash-keyed records for several
cartridges. Replacement is per cartridge; when a lane fills it compacts itself
and evicts its oldest record. Slot 0 also reserves 8 KiB for global settings
and up to 192 per-cartridge controller profiles.
PICO-8 `cartdata` records share slot 0, are keyed by their string IDs and keep
their 256-byte `dget`/`dset` payloads independent from save states and settings.

## Filesystem constraint

On Pocket, APF does not expose the SD card as a general block filesystem to a
core. The application can enumerate the virtual root of files already bound to
data slots and can ask APF to open a *known full path*, but APF has no command
for enumerating arbitrary SD directories. The official data-slot API also caps
a core at 32 slots.

Therefore the complete browser uses a two-part design:

1. Cards live under `/Assets/pico8pocket/common/cards/` and can initially be
   selected with Pocket's native asset picker.
2. A compact `cards.idx` catalog supplies paths, metadata, favorites, recent
   entries and ZIP members to the in-core browser. A host-side indexer updates
   it when cards are copied. Known paths can then be opened through APF target
   command `0x0192` without assigning every cart its own permanent slot.

This provides a real graphical browser for the card collection. Traversing
every unrelated directory on the whole SD card without an index is not
implementable through the published Pocket APF interface; supporting that
would require a new Pocket firmware API from Analogue.

## Milestones

1. **Implemented in alpha:** Pocket slot loading, video/input boundary, 30/60
   FPS scheduling and RV32/APF packaging.
2. **Implemented in alpha:** `.p8` execution with z8lua and the initial PICO-8
   graphics, input, map, palette, memory and utility API slice.
3. **Implemented in alpha:** `.p8.png` decode, legacy/current compressed code,
   plus a 120-frame headless Celeste test.
4. **Implemented in alpha:** integer/fixed-point 48 kHz SFX/music synthesis,
   built-in waveforms, music sequencing and standard note effects.
5. **Implemented in alpha:** paused system menu, content-keyed compressed save
   states with previews, English/Russian UI, per-cart controls, audio/display
   settings and native AnalogueOS cartridge reload.
6. **Implemented in alpha:** cooperative `flip`, extended drawing/memory APIs,
   draw-state aliases, remappable big maps and persistent `cartdata`.
7. **Next:** custom instruments/audio filters, PICO pause menu, multicart
   `load`, and physical-Pocket timing/scaler validation.
8. Browser/indexer, ZIP, cart previews, favorites and recent entries.
9. PICO-8 v0.2.7 conformance matrix.
