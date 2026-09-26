"""Extracts the game's voice lines and other XA clips from APOC.XA to WAV files.

  python tools/extract_voice.py [OUT_DIR]      (default: assets/extracted/voice)

APOC.XA interleaves 16 channels (file 1, channels 0-15). The EXE lists where each clip is:
  0x800A5F80  22 x s32  block start, in sectors of one channel (the clip starts at sector
                        start * 16 + channel of the file)
  0x800A5FD8  22 x 16 x s32  clip length in sectors of its channel, -1 = no clip
Clips are named bBB_cCC.wav (block, channel); the runtime's subtitles use the same names.
"""
import struct
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from discfs import Disc, RAW_SECTOR, bin_from_cue  # noqa: E402

EXE_BASE = 0x80010000 - 0x800
POS_TABLE, LEN_TABLE, BLOCKS, CHANNELS = 0x800A5F80, 0x800A5FD8, 22, 16
K0, K1 = (0, 60, 115, 98, 122), (0, 0, -52, -55, -60)


def clip_table(exe: bytes) -> list[tuple[int, int, int, int]]:
    pos = struct.unpack_from(f"<{BLOCKS}i", exe, POS_TABLE - EXE_BASE)
    lens = struct.unpack_from(f"<{BLOCKS * CHANNELS}i", exe, LEN_TABLE - EXE_BASE)
    return [(b, ch, pos[b], lens[b * CHANNELS + ch]) for b in range(BLOCKS) for ch in range(CHANNELS)
            if lens[b * CHANNELS + ch] != -1]


def decode_sector(data: bytes, stereo: bool, hist: list[list[int]]) -> list[list[int]]:
    """One XA audio sector (2304 bytes after the subheader) -> samples per channel (4-bit ADPCM)."""
    out = [[], []] if stereo else [[]]
    for g in range(18):
        grp = data[g * 128:(g + 1) * 128]
        for unit in range(8):
            ch = unit & 1 if stereo else 0
            hdr = grp[4 + unit]
            shift, filt = hdr & 15, (hdr >> 4) & 3
            old, older = hist[ch]
            for i in range(28):
                byte = grp[16 + i * 4 + unit // 2]
                nib = (byte >> 4) if unit & 1 else (byte & 15)
                t = nib - 16 if nib & 8 else nib
                s = ((t << 12) >> shift) + ((old * K0[filt] + older * K1[filt] + 32) >> 6)
                s = max(-32768, min(32767, s))
                older, old = old, s
                out[ch].append(s)
            hist[ch] = [old, older]
    return out


def main():
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "assets" / "extracted" / "voice"
    out_dir.mkdir(parents=True, exist_ok=True)
    exe = (ROOT / "extracted" / "SLUS_003.73").read_bytes()
    disc = Disc(bin_from_cue(ROOT / "assets" / "Apocalypse (USA).cue"))
    xa = disc.find("APOC.XA")
    f = disc.f
    clips = clip_table(exe)
    for b, ch, start, length in clips:
        pcm: list[list[int]] = []
        stereo = rate = None
        hist = [[0, 0], [0, 0]]
        for k in range(length):
            lba = xa.lba + (start + k) * CHANNELS + ch
            f.seek(lba * RAW_SECTOR + 16)
            sub = f.read(8)
            coding = sub[3]
            stereo, rate = bool(coding & 1), 18900 if coding & 4 else 37800
            data = f.read(2304)
            dec = decode_sector(data, stereo, hist)
            if not pcm:
                pcm = [[] for _ in dec]
            if len(dec) == len(pcm):
                for c in range(len(dec)):
                    pcm[c] += dec[c]
        path = out_dir / f"b{b:02d}_c{ch:02d}.wav"
        with wave.open(str(path), "wb") as w:
            w.setnchannels(len(pcm))
            w.setsampwidth(2)
            w.setframerate(rate)
            frames = bytearray()
            for i in range(len(pcm[0])):
                for c in range(len(pcm)):
                    frames += struct.pack("<h", pcm[c][i])
            w.writeframes(bytes(frames))
        print(f"{path.name}  {len(pcm[0]) / rate:5.1f}s {'stereo' if len(pcm) == 2 else 'mono'}")
    print(f"{len(clips)} clips in {out_dir}")


if __name__ == "__main__":
    main()
