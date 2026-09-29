#!/usr/bin/env python3
"""Step-by-step control of a running game for an AI tester (runtime side: APOC_BOT in host_sdl.cpp).

    python tools/bot.py start --chapter N [--sim 1|0] [--renderer opengl|vulkan]
    python tools/bot.py step BUTTONS FRAMES     e.g. "UP+SQUARE 30", "- 20" (nothing held)
    python tools/bot.py stop

The game stops after every step until the next one. `step` prints the player's state and the
path of the step's last picture (debug/bot/live/shot_FRAME.png). Buttons: UP DOWN LEFT RIGHT
CROSS CIRCLE SQUARE TRIANGLE L1 R1 L2 R2 START SELECT. `start` snapshots the user's saves/ and
settings.ini, `stop` puts them back: always call stop.
"""
import os, shutil, subprocess, sys, time
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
BOT = ROOT / "debug" / "bot"
LIVE, SNAP = BOT / "live", BOT / "snap"


def running():
    return b"apocalypse.exe" in subprocess.run(["tasklist", "/FI", "IMAGENAME eq apocalypse.exe"],
                                               capture_output=True).stdout.lower()


def robocopy(src, dst):
    if subprocess.run(["robocopy", str(src), str(dst), "/MIR", "/NFL", "/NDL", "/NJH", "/NJS", "/NP"]).returncode >= 8:
        sys.exit(f"robocopy {src} -> {dst} failed")


def log_tail(n=15):
    lines = (BOT / "game.log").read_text(errors="replace").splitlines()
    return "\n".join(lines[-n:])


def start(args):
    import argparse
    ap = argparse.ArgumentParser(prog="bot.py start")
    ap.add_argument("--chapter", type=int, required=True)
    ap.add_argument("--sim", type=int, default=1)
    ap.add_argument("--renderer", default="opengl")
    a = ap.parse_args(args)
    if running():
        sys.exit("apocalypse.exe is already running (the user may be playing, or run bot.py stop)")
    if SNAP.exists():
        sys.exit(f"{SNAP} exists: a previous session was not stopped. Run bot.py stop first.")
    LIVE.mkdir(parents=True, exist_ok=True)
    for f in LIVE.iterdir():
        f.unlink()
    robocopy(ROOT / "saves", SNAP / "saves")
    shutil.copy2(ROOT / "settings.ini", SNAP / "settings.ini")
    ini = BOT / "test.ini"
    ini.write_text(f"gfx scale=1 aspect=1 crt=0 filter=0 fps=0\nsim {a.sim}\ndrawdist 400\nprecise 1\nfont 1\n"
                   f"fullscreen 0\naa 0\nmsaa 0\nrenderer {a.renderer}\nautosave 0\n")
    env = dict(os.environ, APOC_BOT=str(LIVE), APOC_BACKGROUND="1", APOC_SETTINGS=str(ini),
               SDL_AUDIO_DRIVER="dummy", APOC_SIM60_CHECK="1")
    with open(BOT / "game.log", "wb") as log:
        subprocess.Popen([str(ROOT / "apocalypse.exe"), "--turbo", "--log", "host", "--chapter", str(a.chapter)],
                         cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
    print(f"started chapter {a.chapter}; send the first step")


def step(args):
    if len(args) != 2:
        sys.exit("usage: bot.py step BUTTONS FRAMES")
    state = LIVE / "state.txt"
    state.unlink(missing_ok=True)
    (LIVE / "cmd.tmp").write_text(f"{args[0]} {int(args[1])}\n")
    os.replace(LIVE / "cmd.tmp", LIVE / "cmd.txt")
    t0 = time.time()
    while not state.exists():
        if not running():
            sys.exit(f"GAME EXITED (crash?). Log tail:\n{log_tail()}")
        if time.time() - t0 > 60:
            sys.exit(f"NO RESPONSE in 60 s (hang?). Log tail:\n{log_tail()}")
        time.sleep(0.05)
    text = state.read_text()
    shot = Path(dict(l.split(" ", 1) for l in text.splitlines())["shot"])
    for _ in range(100):  # the picture is written a moment after the state file
        if shot.exists():
            break
        time.sleep(0.05)
    png = shot.with_suffix(".png")
    Image.open(shot).convert("RGB").save(png)
    shot.unlink()
    print("
".join(f"shot {png}" if l.startswith("shot ") else l for l in text.splitlines()))
    errors = [l for l in log_tail(200).splitlines() if "FATAL" in l or "NATIVE CRASH" in l or "broke" in l]
    if errors:
        print("LOG ERRORS:\n" + "\n".join(errors))


def stop(_):
    if running():
        (LIVE / "cmd.txt").write_text("quit 0\n")
        for _ in range(50):
            if not running():
                break
            time.sleep(0.1)
        else:
            subprocess.run(["taskkill", "/IM", "apocalypse.exe", "/F"], capture_output=True)
            time.sleep(1)
    if SNAP.exists():
        robocopy(SNAP / "saves", ROOT / "saves")
        shutil.copy2(SNAP / "settings.ini", ROOT / "settings.ini")
        shutil.rmtree(SNAP)
    print("stopped; user files restored")


if __name__ == "__main__":
    cmds = {"start": start, "step": step, "stop": stop}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        sys.exit(__doc__)
    cmds[sys.argv[1]](sys.argv[2:])
