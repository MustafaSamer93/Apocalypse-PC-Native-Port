# Project_Repocalypse

A static recompilation of **Apocalypse** (PlayStation, Neversoft/Activision 1998, NTSC-U
`SLUS-00373`) to native Windows code. The game's MIPS R3000A machine code is translated
ahead of time into C, compiled with a normal compiler, and linked against a runtime that
reimplements the PlayStation hardware and BIOS. There is no CPU emulator in the loop.

**No game data is included.** You need your own copy of the disc. The build reads the
executable from your disc image, and the program reads everything else from it at runtime.

## Status

| Area | State |
|---|---|
| Boot, BIOS (high-level reimplementation) | working |
| FMV (MDEC + CD streaming) | working |
| 2D (title, HUD, menus) | working |
| 3D gameplay (GTE + hardware renderer, 1x-8x internal resolution) | working |
| Renderers: OpenGL 3.3 and Vulkan 1.3 (setting "Renderer"; falls back to OpenGL if Vulkan cannot start) | working |
| 4:3 / 16:9 / 16:10 (widescreen widens the 3D view; the in-game HUD and pause menu keep their proportions; movies are shown at 4:3), CRT filter, texture smoothing | working; front-end menu text is still stretched in widescreen |
| Anti-aliasing: supersampling (renders up to 8x, averaged down), FXAA, and MSAA 1x/2x/4x/8x ("AA rate") | working |
| Keyboard + mouse, XInput gamepads (twin-stick aiming, rumble), in-game button remapping | working |
| Sound effects, XA speech/music, CD audio, SPU reverb | working |
| Memory card (`saves/card1.mcr`, emulator-compatible) | working |
| Save states (5 slots) and auto-save (at each checkpoint and when a level is finished) | working |
| Frame rate 30 / 60 / 90 / 120 / 150 fps, or Auto (match the display); setting "Frame rate" | working; the game still simulates at 30 Hz |
| 60 Hz simulation in level 1: player and camera update 60 times a second, everything moves every frame (setting "Simulation") | working in level 1; other levels: optional, untested |
| HD texture packs | not implemented |
| Linux | deferred (the code is portable; build scripts are Windows-only for now) |

## Building (Windows)

Requirements: Visual Studio 2022+ with the C++ workload and the **C++ Clang tools**,
Python 3.10+, Git (CMake fetches SDL3, the Vulkan headers, volk and glslang; no Vulkan SDK is
needed, the Vulkan shaders are compiled during the build).

1. Put `Apocalypse (USA).cue` and its two `.bin` tracks in `assets\`.
2. Run `build.bat`.
3. Run `apocalypse.exe`, which the build puts in the project folder. It needs nothing else
   installed: SDL and the C runtime are built in. It finds `assets\` next to itself, and
   keeps `saves\` and `settings.ini` there too.

What `build.bat` does:

```
python tools\discfs.py extract        # pull SLUS_003.73 (and CD.HED/WAD entries) off the disc
python tools\recomp\main.py           # MIPS -> C: generated\main_*.c (~2,660 functions)
cmake -B build-clang -G Ninja ...     # clang-cl; MSVC's optimizer crashed on the generated code
cmake --build build-clang
```

## Controls

| PlayStation | Keyboard | Gamepad |
|---|---|---|
| D-pad | WASD / arrow keys | D-pad / left stick |
| Cross / Circle / Square / Triangle | K / L / J / I | South / East / West / North |
| L1 / R1 / L2 / R2 | Q / E / Z / C | shoulders / triggers |
| Start / Select | Enter / Backspace | Start / Back |
| fire toward a direction | left mouse button (toward the cursor) | right stick |

F1 (or Select+Start held) = settings: save/load states, renderer, resolution, anti-aliasing, aspect ratio, CRT
filter, texture smoothing, frame rate (original 30 fps or smooth), simulation (60 Hz in
level 1 by default), controller choice and button remapping.
F5 = quick save, F9 = quick load, Tab = fast-forward, F11 = fullscreen, Esc = quit.

## Command line

```
apocalypse [--cue PATH] [--memcard-dir DIR] [--scale N] [--renderer opengl|vulkan]
```

Defaults: 16:9, rendered at 5x (1200 lines; supersampling raises it to about twice the window's
lines, at most 8x), with SSAA + FXAA anti-aliasing, in a 1080-line window, or fullscreen on a 1080p
screen. `--scale N` asks for a window 240*N lines high instead. `--renderer` overrides the
saved renderer for this run.

`--sim60` turns the 60 Hz simulation on in every level (default: level 1 only).
`--smooth K` forces K images per game step (1 = the original 30 fps) and `--refresh HZ`
overrides the detected display refresh rate. A frame rate above the display's refresh is
presented on a timer with vsync off (the counter in the window title shows it).

Debugging options: `--log core,bios,cd,gpu,spu,dma,irq,pad,mdec,host`, `--turbo`,
`--press START@600+10,...` (scripted input), `--watch ADDR` (log writes to a RAM word, including DMA),
`--dump-ram CYCLES FILE`, `--dump-vram FRAME FILE.bmp`, `--gte-dump FRAME`, `--wav FILE`,
`--interp` (run everything in the interpreter; for differential testing),
`--trace-call ADDR` (with `--interp`).

## How it works

```
tools/discfs.py        ISO9660 + CD.HED/CD.WAD reader for the raw MODE2/2352 image
tools/recomp/
  mips.py              R3000A decoder
  analysis.py          function discovery (entry point, call targets, function-pointer scan,
                       gap filling), jump-table recovery, load-delay hazard check
  emit.py              one C function per MIPS function; branches become gotos, delay slots
                       are folded into the branch, jal becomes a direct C call
  main.py              driver; also reads extra_seeds.txt (entry points found at runtime)
runtime/
  include/recomp.h     ABI used by the generated code (CPU state, inline memory access)
  src/core.cpp         memory map, scheduler, interrupt controller, COP0, dispatch
  src/bios.cpp         kernel A0/B0/C0 functions, events, interrupt chains/hooks, memory card files
  src/interp.cpp       fallback interpreter for code not found by static analysis
  src/gte.cpp          Geometry Transformation Engine (tests: runtime/tests/gte_test.cpp)
  src/gpu.cpp          GP0/GP1 + software rasterizer into 1024x512 VRAM
  src/cdrom.cpp        CD controller, XA-ADPCM decoding, CD-DA
  src/spu.cpp          24-voice ADPCM SPU
  src/mdec.cpp         movie decoder
  src/hw.cpp           DMA, root counters, controller port (SIO0)
  src/smooth.cpp       extra frames between the game's 30 Hz steps (smooth frame rate)
  src/sim60.cpp        60 Hz simulation (player and camera) for level 1
  src/host_sdl.cpp     window, audio, input, frame pacing (SDL3)
```

Things the game does that the tools have to handle:

- **Interrupts.** They are delivered at backward branches and scheduler checkpoints, with the
  full register file saved. PsyQ's handler is entered through a `setjmp` continuation.
- **libgte helpers that return with `jr t6`.** An indirect jump to the function's own
  return address is treated as a return.
- **Switch tables whose base sits in a callee-saved register.** It is resolved by a
  long-range constant search. Anything left unresolved runs in the interpreter until the
  function returns.
- **libcd toggling DMA interrupt enables with byte stores to `DICR+2`.** All 32-bit I/O
  registers support sub-word access.
- **A fixed 30 Hz simulation.** The level loop advances the world one fixed step per rendered
  frame, so it cannot simply run faster. Its render, though, can already draw a moment between
  two steps (it did so when the PS1 fell behind). `runtime/src/smooth.cpp` runs that render again
  for fractions of a step, outside emulated time and with the machine rolled back afterwards, and
  shows those images between the game's own.
- **A 60 Hz simulation for level 1.** `runtime/src/sim60.cpp` makes the level loop tick and
  render every VBlank, updates the player and cameras on the ticks that did nothing, and retimes
  their per-update amounts (half of each update's change for positions and velocities; whole-
  number counters such as animation frames keep their 30 Hz rate). Enemies and shots keep their
  30 Hz logic; the frame after each world update draws the world half way between before and
  after it (positions, rotations, the screen tint, and animation poses, which are blended per
  vertex by rendering twice), so everything in level 1 moves every frame.
- **The game reads controllers directly over SIO.** Byte and `/ACK` timing are modelled,
  because the driver clears IRQ flags between the reply and the `/ACK`.
