"""Exports the game's FMVs (every .STR on the disc) to MKV at 4:3 (640x480), with their XA sound.

  python tools/extract_movies.py [OUT_DIR]      (default: assets/extracted/movies)

The .STR files are raw MODE2 sectors (MDEC video + interleaved XA audio); ffmpeg's psxstr demuxer
reads them as-is, so each movie's sectors are piped straight from the disc image into ffmpeg.
Needs ffmpeg on PATH, or set FFMPEG=path\\to\\ffmpeg.exe.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from discfs import Disc, RAW_SECTOR, find_data_bin  # noqa: E402

KDENLIVE_FFMPEG = r"C:\Program Files\Kdenlive\bin\ffmpeg.exe"


def main():
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "assets" / "extracted" / "movies"
    out_dir.mkdir(parents=True, exist_ok=True)
    ffmpeg = os.environ.get("FFMPEG") or shutil.which("ffmpeg") or KDENLIVE_FFMPEG
    if not Path(ffmpeg).exists() and not shutil.which(ffmpeg):
        sys.exit("ffmpeg not found (put it on PATH or set FFMPEG)")
    disc = Disc(find_data_bin())
    movies = [e for e in disc.list_files() if e.path.upper().endswith(".STR")]
    for e in movies:
        disc.f.seek(e.lba * RAW_SECTOR)
        raw = disc.f.read((e.size + 2047) // 2048 * RAW_SECTOR)
        dest = out_dir / (Path(e.path).stem + ".mkv")
        subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "psxstr", "-i", "-",
                        "-vf", "scale=640:480:flags=lanczos,setsar=1",
                        "-c:v", "libx264", "-crf", "16", "-preset", "slow", "-pix_fmt", "yuv420p",
                        "-c:a", "aac", "-b:a", "192k", str(dest)],
                       input=raw, check=True)
        print(f"{e.path:24s} -> {dest.name}")
    print(f"{len(movies)} movies in {out_dir}")


if __name__ == "__main__":
    main()
