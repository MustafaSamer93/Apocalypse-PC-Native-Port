#!/usr/bin/env python3
"""Automated playtest: load a chapter, play it with seeded random input, and flag problems.

    python tools/playtest.py [--chapter N] [--seed S] [--frames F] [--every K] [--sim 0|1]
                             [--renderer opengl|vulkan] [--scale X] [--turbo]

One run = one apocalypse.exe process, window in the background, no audio. Afterwards:
    debug/playtest/<stamp>_chN_sS/  game.log, shot_<frame>.png, sheet.png (all shots tiled), report.json
Flags: crash (process died early / FATAL / NATIVE CRASH), log errors, black screen, frozen picture.
The user's saves/ and settings.ini are snapshotted right before the run and restored right after it.
Replay a run with the same --chapter/--seed/--frames (inputs are keyed to host frames; --turbo breaks that).
"""
import argparse, json, os, random, re, shutil, subprocess, sys, tempfile, time
from datetime import datetime
from pathlib import Path
from PIL import Image, ImageChops, ImageDraw, ImageStat

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "apocalypse.exe"
CHAPTERS = len(list((ROOT / "saves" / "chapters").glob("*.state")))
START = 90  # chapter loads at frame 30; give it a moment
BAD_LOG = re.compile(r"FATAL|NATIVE CRASH|invalid instruction|broke an object list|slot held a freed|invalid \(gp", re.I)

# What a player does, roughly: (weight, buttons held together)
ACTIONS = [
    (20, ["UP"]), (10, ["UP", "SQUARE"]), (8, ["SQUARE"]), (8, ["LEFT"]), (8, ["RIGHT"]),
    (6, ["UP", "R1"]), (5, ["R1"]), (5, ["DOWN"]), (4, ["UP", "LEFT"]), (4, ["UP", "RIGHT"]),
    (3, ["CROSS"]), (3, ["CIRCLE"]), (3, ["TRIANGLE"]), (3, ["L1"]), (3, ["R2"]), (3, ["L2"]),
    (5, []),  # stand still
]


def press_script(rng, frames):
    items, f = [], START
    while f < frames:
        n = rng.randint(15, 90)
        if rng.random() < 0.01:  # pause, wait, unpause
            items += [f"START@{f}+4", f"START@{f + 60}+4"]
            n = 80
        else:
            buttons = rng.choices([a[1] for a in ACTIONS], [a[0] for a in ACTIONS])[0]
            items += [f"{b}@{f}+{n}" for b in buttons]
        f += n
    return ",".join(items)


def robocopy(src, dst):
    r = subprocess.run(["robocopy", str(src), str(dst), "/MIR", "/NFL", "/NDL", "/NJH", "/NJS", "/NP"],
                       capture_output=True)
    if r.returncode >= 8:
        sys.exit(f"robocopy {src} -> {dst} failed ({r.returncode})")


def analyse(out, shots):
    """Convert shots to PNG, find black and frozen stretches, tile a contact sheet."""
    black, frozen, prev, run = [], [], None, []
    thumbs = []
    for f in shots:
        bmp = out / f"shot_{f}.bmp"
        if not bmp.exists():
            break
        im = Image.open(bmp).convert("RGB")
        im.save(out / f"shot_{f}.png")
        bmp.unlink()
        if max(ImageStat.Stat(im).mean) < 3:
            black.append(f)
        if prev is not None and not ImageChops.difference(im, prev).getbbox():
            run.append(f)
        else:
            if len(run) >= 4:
                frozen.append([run[0], run[-1]])
            run = []
        prev = im
        t = im.resize((213, 160))
        ImageDraw.Draw(t).text((3, 2), str(f), fill=(255, 255, 0))
        thumbs.append(t)
    if len(run) >= 4:
        frozen.append([run[0], run[-1]])
    if thumbs:
        cols = 8
        sheet = Image.new("RGB", (213 * cols, 160 * ((len(thumbs) + cols - 1) // cols)))
        for i, t in enumerate(thumbs):
            sheet.paste(t, (213 * (i % cols), 160 * (i // cols)))
        sheet.save(out / "sheet.png")
    return len(thumbs), black, frozen


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--chapter", type=int, help=f"1-{CHAPTERS} (default: random)")
    ap.add_argument("--seed", type=int, help="input seed (default: random)")
    ap.add_argument("--frames", type=int, default=1800, help="host frames to play (60 = 1 s)")
    ap.add_argument("--every", type=int, default=30, help="screenshot every K frames")
    ap.add_argument("--sim", type=int, default=1, help="1 = 60 Hz simulation, 0 = original 30 Hz")
    ap.add_argument("--renderer", default="opengl", choices=["opengl", "vulkan"])
    ap.add_argument("--scale", type=int, default=1)
    ap.add_argument("--turbo", action="store_true", help="run unthrottled (not replayable)")
    ap.add_argument("--press", help="explicit --press script instead of random input")
    a = ap.parse_args()

    if b"apocalypse.exe" in subprocess.run(["tasklist", "/FI", "IMAGENAME eq apocalypse.exe"],
                                           capture_output=True).stdout.lower():
        sys.exit("apocalypse.exe is already running (the user may be playing); not touching saves/")

    seed = a.seed if a.seed is not None else random.randrange(1 << 31)
    rng = random.Random(seed)
    chapter = a.chapter or rng.randint(1, CHAPTERS)
    press = a.press or press_script(rng, a.frames)
    shots = list(range(START, a.frames + 1, a.every))
    out = ROOT / "debug" / "playtest" / f"{datetime.now():%Y%m%d_%H%M%S}_ch{chapter}_s{seed}"
    out.mkdir(parents=True)

    snap = Path(tempfile.mkdtemp(prefix="playtest_snap_"))
    ini = out / "test.ini"
    ini.write_text(f"gfx scale={a.scale} aspect=1 crt=0 filter=0 fps=0\nsim {a.sim}\ndrawdist 400\nprecise 1\n"
                   f"font 1\nfullscreen 0\naa 0\nmsaa 0\nrenderer {a.renderer}\nautosave 0\n")
    env = dict(os.environ, APOC_BACKGROUND="1", APOC_SETTINGS=str(ini), SDL_AUDIO_DRIVER="dummy",
               APOC_SIM60_CHECK="1")
    cmd = [str(EXE), "--scale", str(a.scale), "--log", "host", "--chapter", str(chapter), "--press", press,
           "--screenshot", ",".join(map(str, shots)), str(out / "shot")] + (["--turbo"] if a.turbo else [])
    (out / "press.txt").write_text(press)

    before = set((ROOT / "debug").glob("crash_*"))
    robocopy(ROOT / "saves", snap / "saves")
    shutil.copy2(ROOT / "settings.ini", snap / "settings.ini")
    t0, exit_code = time.time(), None
    try:
        with open(out / "game.log", "wb") as log:
            p = subprocess.Popen(cmd, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
            limit = 30 + a.frames / 60 * 2  # generous: loading + slow frames
            while time.time() - t0 < limit:
                if p.poll() is not None:
                    exit_code = p.returncode
                    break
                if (out / f"shot_{shots[-1]}.bmp").exists():
                    time.sleep(0.5)
                    break
                time.sleep(0.5)
            timed_out = p.poll() is None and not (out / f"shot_{shots[-1]}.bmp").exists()
            if p.poll() is None:
                p.kill()
                p.wait()
    finally:
        robocopy(snap / "saves", ROOT / "saves")
        shutil.copy2(snap / "settings.ini", ROOT / "settings.ini")
        shutil.rmtree(snap, ignore_errors=True)

    taken, black, frozen = analyse(out, shots)
    text = (out / "game.log").read_text(errors="replace").splitlines()
    log_errors = [l for l in text if BAD_LOG.search(l)][:40]
    crash_files = sorted(str(f) for f in set((ROOT / "debug").glob("crash_*")) - before)
    report = {
        "chapter": chapter, "chapter_file": sorted(p.name for p in (ROOT / "saves" / "chapters").glob("*.state"))[chapter - 1],
        "seed": seed, "frames": a.frames, "sim": a.sim, "renderer": a.renderer, "turbo": a.turbo,
        "seconds": round(time.time() - t0, 1), "exit_code": exit_code, "timed_out": timed_out,
        "shots_taken": taken, "shots_planned": len(shots), "black_frames": black, "frozen_ranges": frozen,
        "log_errors": log_errors, "crash_files": crash_files, "dir": str(out),
        "replay": f"python tools/playtest.py --chapter {chapter} --seed {seed} --frames {a.frames} --sim {a.sim} --renderer {a.renderer}",
    }
    report["problems"] = [k for k, bad in [
        ("crash", exit_code is not None and exit_code != 0 or bool(crash_files)),
        ("stopped_early", taken < len(shots)), ("timed_out", timed_out), ("log_errors", bool(log_errors)),
        ("black_screen", len(black) >= 3), ("frozen_picture", bool(frozen))] if bad]
    (out / "report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
