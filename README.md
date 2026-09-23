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
| 3D gameplay (GTE + software rasterizer) | working in attract demos and new games |
| Controller (keyboard / XInput gamepads via SDL3; F1 = settings) | working |
| Sound effects, XA speech/music, CD audio | working; SPU reverb not implemented yet |
| Memory card (`saves/card1.mcr`, emulator-compatible) | working |
| Linux | deferred (the code is portable; build scripts are Windows-only for now) |

## Building (Windows)

Requirements: Visual Studio 2022+ with the C++ workload and the **C++ Clang tools**,
Python 3.10+, Git (CMake fetches SDL3).

1. Put `Apocalypse (USA).cue` and its two `.bin` tracks in `asset\`.
2. Run `build.bat`.
3. Run `build-clang\apocalypse.exe` from the project folder.

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
| D-pad | arrow keys | D-pad / left stick |
| Cross / Circle / Square / Triangle | Z / X / A / S | South / East / West / North |
| L1 / R1 / L2 / R2 | Q / W / 1 / 2 | shoulders / triggers |
| Start / Select | Enter / Backspace | Start / Back |

Tab = fast-forward, F11 = fullscreen, Esc = quit.

## Command line

```
apocalypse [--cue PATH] [--memcard-dir DIR] [--scale N]
```

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
- **The game reads controllers directly over SIO.** Byte and `/ACK` timing are modelled,
  because the driver clears IRQ flags between the reply and the `/ACK`.
