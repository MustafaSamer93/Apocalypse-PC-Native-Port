# Repocalypse

**Apocalypse** (PlayStation, 1998), rebuilt as a native PC game for Windows, Linux and Steam Deck.

Not an emulator. The game's original code was translated into native code ahead of time, so it
runs at modern resolutions, frame rates and aspect ratios while playing exactly like the
original.

[![Discord](https://img.shields.io/badge/Discord-join-5865F2?logo=discord&logoColor=white)](https://discord.gg/fnRND5rZq)
[![Download](https://img.shields.io/github/v/release/MustafaSamer93/Project_Repocalypse?label=download)](https://github.com/MustafaSamer93/Project_Repocalypse/releases/latest)

<!-- screenshots / gif here -->

> **You need your own copy of the game.** No game data is included. Repocalypse reads everything
> from a disc image you dump yourself: *Apocalypse (USA)*, `SLUS-00373`.

## Features

- **Up to 4K** resolution
- **Anti-aliasing**: supersampling, MSAA and FXAA
- **High fps**: native 60 fps support at the engine level, and interpolated 75, 90 and 120 fps
  on top
- **Aspect ratios**: 4:3, 16:9, 16:10, 21:9 and 32:9
- **Native GTE**: a native renderer with full floating-point precision for every polygon, effect
  and shadow, exact and perspective-correct textures, and a depth buffer, on OpenGL and Vulkan
  backends
- **Controls**: keyboard and mouse added, plus full XInput controller support, with full rebinding
  for both, with low input lag
- **Draw distance**: reworked to support a much larger draw distance than the original
- **Asset replacements**: support for custom textures and FMV replacements
- **Saving**: autosave at every checkpoint, and memory card saves compatible with emulators
- **One-liners**: subtitles for the voice lines, printed on screen with effects
- **Cheats**: added a dedicated new menu for in-game cheats
- **Cosmetics**: CRT filter, the game's own font at any size, FPS counter, sound filter
- **Extras**: chapter select, save states

## Install

1. Download the latest release for your system from
   [Releases](https://github.com/MustafaSamer93/Project_Repocalypse/releases/latest) and unzip it anywhere.
2. Put your disc image in `assets/image/`: its `.cue` and `.bin` files (any names), or only the
   `.bin` files.
3. Run `repocalypse.exe` (Windows) or `repocalypse` (Linux).

Saves and `settings.ini` are kept next to the program.

**Steam Deck**: use the Linux build, add `repocalypse` to Steam as a non-Steam game and play in
Game Mode.

### Optional extras

| Folder | What goes in it |
|---|---|
| `assets/replacements/textures` | HD textures and full-screen pictures |
| `assets/replacements/fmvs` | HD movies (`INTRO.mp4`, `SEWER.mkv`, ...) that play instead of the disc's |

## Controls

| Action | Keyboard and mouse | Controller |
|---|---|---|
| Move | WASD / arrow keys | D-pad / left stick |
| Aim and fire | left mouse button (toward the pointer) | right stick |
| Fire up / down / left / right | I / K / J / L | Y / A / X / B |
| Jump | Space / right mouse button | RB / R1 |
| Crouch | Left Shift | LB / L1 |
| Weapon select | mouse wheel / Q | LT / L2 |
| Smart bomb | F / middle mouse button | RT / R2 |
| Pause / Select | Enter / Backspace | Start / Back |

Every action can be rebound in the settings screen.

- **Settings screen**: hold **Select + Start** (Backspace + Enter). Graphics, controls, save
  states and chapters.
- **Recomp Enhancements and Cheats**: the game's main menu, under Options.
- **Tab** fast-forwards, **Esc** quits.

## Problems?

- Ask on [Discord](https://discord.gg/fnRND5rZq) or open an
  [issue](https://github.com/MustafaSamer93/Project_Repocalypse/issues).
- Attach `debug/repocalypse.log` (and any `debug/crash_*.log`) from the game folder.
- An error on first start almost always means the disc image is missing or wrong: check that the
  `.bin` files are in `assets/image/` and are a dump of the USA version.

## Source

Repocalypse is built with a static recompiler and a reimplementation of the PlayStation hardware.
The source, build instructions and technical details are in
[Repocalypse-Recomp](https://github.com/MustafaSamer93/Repocalypse-Recomp).

## Credits

- **Mustafa Samer** ([@MustafaSamer93](https://github.com/MustafaSamer93)): project lead
- **Claude** (Anthropic's AI, through [Claude Code](https://claude.com/claude-code)): co-author of
  the recompiler, runtime and enhancements

*Apocalypse* is © 1998 Activision and Neversoft. This is a fan project, not affiliated with or
endorsed by Activision or Neversoft.
