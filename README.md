# Project_Repocalypse

A static recompilation of **Apocalypse** (PlayStation, Neversoft/Activision 1998, NTSC-U
`SLUS-00373`) to native Windows and Linux code. The game's MIPS R3000A machine code is
translated ahead of time into C, compiled with a normal compiler, and linked against a runtime
that reimplements the PlayStation hardware and BIOS. There is no CPU emulator in the loop.

**No game data is included.** You need your own copy of the disc. The build reads the
executable from your disc image, and the program reads everything else from it at runtime.

## Status

| Area | State |
|---|---|
| Boot, BIOS (high-level reimplementation), FMV (MDEC + CD streaming) | working |
| 2D (title, HUD, menus) and 3D gameplay (GTE + hardware renderer) | working |
| Renderers: OpenGL 3.3 and Vulkan 1.3 (falls back to the other if one cannot start); software renderer | working |
| Resolution 1x-9x (up to 2160 lines), supersampling, FXAA, MSAA 1x-8x | working |
| Aspect ratios 4:3, 16:9, 16:10, 21:9, 32:9 (the 3D view widens; HUD, menu text and icons keep their shape) | working |
| Precise 3D: polygons at their exact positions, perspective-correct textures, Z-buffer (no wobble) | working |
| 60 Hz simulation (levels 1-4 by default, every level optional) | working |
| CRT filter, texture smoothing, FPS counter, draw distance up to 4x | working |
| Replacement textures, full-screen pictures and movies; the game's font from a TrueType file | working |
| Subtitles for the voice lines, cheats, auto-save at checkpoints, chapter list | working |
| Keyboard + mouse, gamepads (twin-stick aiming, free aim, rumble), button remapping | working |
| Sound effects, XA speech/music, CD audio, SPU reverb | working |
| Memory card (`saves/card1.mcr`, emulator-compatible), save states (5 slots) | working |

## Building

Put your disc image (its `.cue` and `.bin` files, any names, or only the `.bin` files) in
`assets/image/`; see `assets/README.txt` for the optional extras. Then:

- **Windows**: Visual Studio 2022+ with the C++ workload and the **C++ Clang tools**, Python
  3.10+ and Git. Run `build.bat`.
- **Linux**: cmake, ninja, clang or gcc, python3, git and SDL3's build dependencies (listed at
  the top of `build.sh`). Run `./build.sh`; `tools/package_linux.sh` makes a portable build.

CMake fetches SDL3, the Vulkan headers, volk, glslang and stb; no Vulkan SDK is needed. The
result, `repocalypse.exe` (Windows) or `repocalypse` (Linux), is made in the project folder and
needs nothing else installed. It finds `assets/` next to itself and keeps `saves/` and
`settings.ini` there.

What the build does:

```
python tools/discfs.py extract        # SLUS_003.73 and the CD.HED/CD.WAD entries off the disc
python tools/recomp/main.py           # MIPS -> C: generated/main_*.c (~2,660 functions)
cmake -B build-clang -G Ninja ...     # clang (MSVC's optimizer crashed on the generated code)
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

Hold **Select + Start** (Backspace + Enter) for the settings screen: save/load states, chapters
(restart points reached so far), renderer, resolution, anti-aliasing, aspect ratio, CRT filter,
texture smoothing, replacement textures, controller choice and button set-up, and the Recomp
Enhancements page. The game's own main menu has Options -> Recomp Enhancements and Cheats.
Tab = fast-forward, Esc = quit.

## Command line

```
repocalypse [--cue PATH] [--scale N] [--renderer opengl|vulkan] [--sim60] [--chapter N]
```

`--cue` picks a disc image other than the one in `assets/image`. `--scale N` asks for a window
240*N lines high (default: 1080 lines, or what fits). `--renderer` overrides the saved renderer
for this run. `--sim60` turns the 60 Hz simulation on in every level. `--chapter N` starts at
chapter N, a state kept in `saves/chapters` the first time each restart point is reached.

For scripted test runs: `--turbo` (unthrottled), `--press START@600+10,...` (buttons held from
a frame for some frames), `--screenshot 200,400 PREFIX` (saves those frames as BMPs),
`--log core,bios,cd,gpu,spu,dma,irq,pad,mdec,host`. Environment: `APOC_SETTINGS=FILE` (another
settings file), `APOC_BACKGROUND=1` (the window opens behind the others), `APOC_NO_DIALOGS=1`
(errors only in the log), `APOC_TEXREPLACE_DIR=DIR`, `APOC_TEX_DUMP=1`.

A double-clicked game logs to `debug/repocalypse.log` (earlier runs in `debug/logs`, crashed
ones as `debug/crash_*.log`): the file to send with a bug report.

## Source layout

```
tools/
  discfs.py              ISO9660 + CD.HED/CD.WAD reader for the raw MODE2/2352 image
  recomp/                the static recompiler
    mips.py              R3000A decoder
    analysis.py          function discovery (entry point, call targets, function-pointer scan,
                         gap filling), jump-table recovery, load-delay hazard check
    emit.py              one C function per MIPS function; branches become gotos, delay slots are
                         folded into the branch, jal becomes a direct C call; the runtime's hooks
    main.py              driver; also reads extra_seeds.txt (entry points found at runtime)
  psx_textures.py        exports the game's textures with the names replacements use
  extract_images.py, extract_voice.py, extract_movies.py, make_icon.py, package_linux.sh
runtime/
  include/recomp.h       ABI of the generated code (CPU state, inline memory access, hooks)
  shaders/               Vulkan shaders (SPIR-V at build time); the CRT filter's GLSL, also used by OpenGL
  data/subtitles.txt     the voice lines' text (built into the exe)
  src/main.cpp           start-up: the disc, the executable, the hardware, the entry point
  src/core/              the machine: memory map, scheduler, interrupts, COP0, dispatch (core.cpp),
                         kernel A0/B0/C0 functions (bios.cpp), the fallback interpreter, memory
                         cards, save states, chapters and auto-save (savestate.cpp); psx.h
  src/hw/                PlayStation hardware: GPU commands and software rasterizer (gpu.cpp), GTE,
                         CD-ROM with XA/CD-DA, SPU, MDEC, DMA / timers / controller port (hw.cpp)
  src/render/            hardware renderers: OpenGL (gpu_gl.cpp) and Vulkan (gpu_vk.cpp) with their
                         shared half (hw_common.cpp); presenters (present_gl/vk/common.cpp); menu text
  src/host/              SDL3: window, frame pacing and audio (host.cpp), controllers (input.cpp),
                         settings.ini and the enhancement options (settings.cpp), the settings screen
  src/game/              Apocalypse enhancements: 60 Hz simulation (sim60.cpp), draw distance, the
                         game's own menus, subtitles, cheats, replacement textures / pictures / movies
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
- **A fixed 30 Hz simulation.** `runtime/src/game/sim60.cpp` makes the level loop tick and render
  every VBlank, updates the player and cameras on the ticks that did nothing, and retimes their
  per-update amounts (half of each update's change for positions and velocities; whole-number
  counters such as animation frames keep their 30 Hz rate). Enemies and shots keep their 30 Hz
  logic; the frame after each world update draws the world half way between before and after it
  (positions, rotations, the screen tint, and animation poses, which are blended per vertex by
  rendering twice), so everything moves every frame.
- **The game reads controllers directly over SIO.** Byte and `/ACK` timing are modelled,
  because the driver clears IRQ flags between the reply and the `/ACK`.
